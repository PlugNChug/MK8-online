//! The setup program: the launcher binary itself, named `MK8-online-<version>-setup.exe` (or run with
//! `--install`). Setup saves the data folder in `mk8-online.json`, copies `Launcher.exe`,
//! unpacks this version's sources, creates shortcuts and registers the uninstaller for the
//! current user. It can then open the launcher in the same window.
//!
//! `/SILENT` (the in-launcher updater) installs over the previous installation with its folders and starts the
//! new launcher; `/DIR=` and `/DATADIR=` choose the folders, `/DESKTOP=0|1` the desktop shortcut and `/NOLAUNCH`
//! skips starting it (tests). Installed Uninstall.exe opens the removal wizard.
//! Silent uninstall keeps the data folder, including builds and saves.
use serde_json::{json, Value};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    process::Command,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use crate::{update, Error};

const UNINSTALL_KEY: &str = r"HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\MK8-online";
/// The key of the Inno Setup installer used up to 0.1.1 (replaced on the first install of this setup).
const INNO_KEY: &str = r"HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F2C9E1A-4B8D-4E7A-9C51-3A7D2E8F0B64}_is1";
pub const NEEDED_GIB: f64 = 30.0;
const NO_WINDOW: u32 = 0x0800_0000;

pub enum Mode {
    Install {
        silent: bool,
    },
    Uninstall {
        silent: bool,
    },
    /// The copy in %TEMP% that removes the installed files (a running program cannot delete itself).
    UninstallRun {
        app: PathBuf,
        silent: bool,
    },
}

fn flag(args: &[String], names: &[&str]) -> bool {
    args.iter()
        .any(|a| names.iter().any(|n| a.eq_ignore_ascii_case(n)))
}

fn value(args: &[String], name: &str) -> Option<String> {
    let prefix = format!("{name}=");
    args.iter().find_map(|a| {
        (a.len() > prefix.len() && a[..prefix.len()].eq_ignore_ascii_case(&prefix))
            .then(|| a[prefix.len()..].trim_matches('"').to_string())
    })
}

pub fn mode(args: &[String]) -> Option<Mode> {
    let silent = flag(args, &["/SILENT", "/VERYSILENT", "--silent"]);
    if let Some(n) = args.iter().position(|a| a == "--uninstall-run") {
        return Some(Mode::UninstallRun {
            app: PathBuf::from(args.get(n + 1)?),
            silent,
        });
    }
    if flag(args, &["--uninstall"])
        || args
            .first()
            .and_then(|p| Path::new(p).file_name())
            .is_some_and(|n| n.eq_ignore_ascii_case("Uninstall.exe"))
    {
        return Some(Mode::Uninstall { silent });
    }
    let named_setup = std::env::current_exe()
        .ok()
        .and_then(|p| {
            p.file_stem()
                .map(|s| s.to_string_lossy().to_ascii_lowercase())
        })
        .is_some_and(|stem| stem.contains("setup"));
    (named_setup || flag(args, &["--install"])).then_some(Mode::Install { silent })
}

pub fn run(mode: Mode, args: &[String]) -> Result<(), Error> {
    match mode {
        Mode::Install { silent: true } => {
            let previous = Previous::find();
            let options = Options {
                app: value(args, "/DIR")
                    .map(PathBuf::from)
                    .unwrap_or(previous.app),
                data: value(args, "/DATADIR")
                    .map(PathBuf::from)
                    .unwrap_or(previous.data),
                // (/DESKTOP=0|1 overrides keeping the previous choice)
                desktop: value(args, "/DESKTOP").map_or(previous.desktop, |v| v != "0"),
            };
            let progress = Arc::new(Mutex::new(Progress::default()));
            install(&options, &progress)?;
            if flag(args, &["/NOLAUNCH"]) {
                return Ok(());
            }
            launch(&options.app)
        }
        Mode::Install { silent: false } => crate::installer_ui::window(None),
        Mode::Uninstall { silent } => {
            // Run from a copy in %TEMP%, so the installed Launcher.exe can be deleted.
            let exe = std::env::current_exe()?;
            let app = exe.parent().ok_or("pasta do programa")?.to_path_buf();
            let temp = std::env::temp_dir().join(format!(
                "mk8-online-uninstall-{:016x}.exe",
                u64::from_le_bytes(mk8_transport::random())
            ));
            fs::copy(&exe, &temp)?;
            let mut command = Command::new(&temp);
            command.arg("--uninstall-run").arg(&app);
            if silent {
                command.arg("/SILENT");
            }
            let worker = command.spawn()?;
            // The temporary image is in use until the worker exits. A separate
            // process waits on its PID and removes only our generated literal path.
            use std::os::windows::process::CommandExt;
            let tidy = "Wait-Process -Id ([int]$env:MK8_WORKER_PID) -ErrorAction SilentlyContinue; Remove-Item -LiteralPath $env:MK8_WORKER_COPY";
            let shell = PathBuf::from(std::env::var_os("SystemRoot").ok_or("SystemRoot ausente")?)
                .join("System32/WindowsPowerShell/v1.0/powershell.exe");
            let _ = Command::new(shell)
                .args(["-NoProfile", "-NonInteractive", "-Command", tidy])
                .env("MK8_WORKER_PID", worker.id().to_string())
                .env("MK8_WORKER_COPY", &temp)
                .creation_flags(NO_WINDOW)
                .spawn();
            Ok(())
        }
        Mode::UninstallRun { app, silent: true } => uninstall(&app, false),
        Mode::UninstallRun { app, silent: false } => crate::installer_ui::window(Some(app)),
    }
}

/* ---------- folders of a previous installation ---------- */

fn reg_query(key: &str, name: &str) -> Option<String> {
    use windows_sys::Win32::System::Registry::{
        RegGetValueW, HKEY_CURRENT_USER, RRF_RT_REG_EXPAND_SZ, RRF_RT_REG_SZ,
    };
    let wide = |s: &str| s.encode_utf16().chain(Some(0)).collect::<Vec<_>>();
    let key = wide(key.strip_prefix(r"HKCU\")?);
    let name = wide(name);
    let mut buffer = vec![0u16; 32768];
    let mut size = (buffer.len() * 2) as u32;
    let code = unsafe {
        RegGetValueW(
            HKEY_CURRENT_USER,
            key.as_ptr(),
            name.as_ptr(),
            RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
            std::ptr::null_mut(),
            buffer.as_mut_ptr().cast(),
            &mut size,
        )
    };
    if code != 0 || size < 2 {
        return None;
    }
    String::from_utf16(&buffer[..size as usize / 2 - 1])
        .ok()
        .filter(|s| !s.is_empty())
}

fn local_appdata() -> PathBuf {
    std::env::var_os("LOCALAPPDATA")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
}

fn shortcut_paths() -> (PathBuf, Option<PathBuf>) {
    let start = std::env::var_os("APPDATA")
        .map(PathBuf::from)
        .unwrap_or_default()
        .join(r"Microsoft\Windows\Start Menu\Programs\MK8-online.lnk");
    let desktop = std::env::var_os("USERPROFILE")
        .map(PathBuf::from)
        .map(|home| {
            // (OneDrive may move the desktop; the shell knows where it is)
            known_desktop()
                .unwrap_or_else(|| home.join("Desktop"))
                .join("MK8-online.lnk")
        });
    (start, desktop)
}

fn known_desktop() -> Option<PathBuf> {
    use std::os::windows::process::CommandExt;
    let output = Command::new("powershell.exe")
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "[Environment]::GetFolderPath('Desktop')",
        ])
        .creation_flags(NO_WINDOW)
        .output()
        .ok()?;
    let path = String::from_utf8_lossy(&output.stdout).trim().to_string();
    (!path.is_empty()).then(|| PathBuf::from(path))
}

pub fn launcher_data(base: &Path) -> PathBuf {
    base.join("data")
}

pub struct Previous {
    pub app: PathBuf,
    pub data: PathBuf,
    pub desktop: bool,
    pub found: bool,
}

impl Previous {
    pub fn find() -> Self {
        let app = reg_query(UNINSTALL_KEY, "InstallLocation")
            .or_else(|| reg_query(INNO_KEY, "InstallLocation"))
            .map(|p| PathBuf::from(p.trim_end_matches('\\')))
            .filter(|p| p.join("Launcher.exe").is_file());
        let found = app.is_some();
        let app = app.unwrap_or_else(|| local_appdata().join(r"Programs\MK8-online"));
        let data = fs::read_to_string(app.join("mk8-online.json"))
            .ok()
            .and_then(|text| serde_json::from_str::<Value>(&text).ok())
            .and_then(|v| v["home"].as_str().map(PathBuf::from))
            .unwrap_or_else(|| local_appdata().join("MK8-online"));
        let desktop = !found || shortcut_paths().1.is_some_and(|p| p.is_file());
        Self {
            app,
            data,
            desktop,
            found,
        }
    }
}

/* ---------- installing ---------- */

pub struct Options {
    pub app: PathBuf,
    pub data: PathBuf,
    pub desktop: bool,
}

#[derive(Clone, Default)]
pub struct Progress {
    pub percent: f64,
    pub step: String,
    pub state: String,
    pub error: Option<String>,
}

fn report(progress: &Mutex<Progress>, percent: f64, step: &str) {
    let mut p = progress.lock().unwrap();
    p.percent = p.percent.max(percent);
    p.step = step.into();
    p.state = "running".into();
}

/// Free space (GiB) of the disk that holds `path`, or of its nearest existing parent.
pub fn free_gib(path: &Path) -> Option<f64> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetDiskFreeSpaceExW(
            dir: *const u16,
            avail: *mut u64,
            total: *mut u64,
            free: *mut u64,
        ) -> i32;
    }
    let existing = path.ancestors().find(|p| p.is_dir())?;
    let wide: Vec<u16> = existing.as_os_str().encode_wide().chain(Some(0)).collect();
    let (mut avail, mut total, mut free) = (0u64, 0u64, 0u64);
    let ok = unsafe { GetDiskFreeSpaceExW(wide.as_ptr(), &mut avail, &mut total, &mut free) };
    (ok != 0).then(|| avail as f64 / (1024.0 * 1024.0 * 1024.0))
}

/// Whether the disk that holds `path` is a hard disk (the storage driver reports a seek
/// penalty), `None` when Windows does not say (network drives, some RAID or USB adapters).
/// The game import and the build write ~12 GB of small files: on a hard disk they take
/// several times longer.
pub fn hard_disk(path: &Path) -> Option<bool> {
    use std::os::windows::ffi::OsStrExt;
    #[repr(C)]
    struct Query {
        property: u32,
        kind: u32,
        extra: [u8; 4],
    }
    #[repr(C)]
    #[derive(Default)]
    struct SeekPenalty {
        version: u32,
        size: u32,
        incurs_seek_penalty: u8,
    }
    #[link(name = "kernel32")]
    extern "system" {
        fn GetVolumePathNameW(file: *const u16, volume: *mut u16, length: u32) -> i32;
        fn CreateFileW(
            name: *const u16,
            access: u32,
            share: u32,
            security: *mut std::ffi::c_void,
            disposition: u32,
            flags: u32,
            template: *mut std::ffi::c_void,
        ) -> isize;
        fn DeviceIoControl(
            device: isize,
            code: u32,
            input: *const std::ffi::c_void,
            input_size: u32,
            output: *mut std::ffi::c_void,
            output_size: u32,
            returned: *mut u32,
            overlapped: *mut std::ffi::c_void,
        ) -> i32;
        fn CloseHandle(handle: isize) -> i32;
    }
    const STORAGE_DEVICE_SEEK_PENALTY_PROPERTY: u32 = 7;
    const IOCTL_STORAGE_QUERY_PROPERTY: u32 = 0x002D_1400;
    let existing = path.ancestors().find(|p| p.is_dir())?;
    let wide: Vec<u16> = existing.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut volume = [0u16; 261];
    if unsafe { GetVolumePathNameW(wide.as_ptr(), volume.as_mut_ptr(), volume.len() as u32) } == 0 {
        return None;
    }
    let root = String::from_utf16_lossy(&volume[..volume.iter().position(|&c| c == 0)?]);
    let letter = root.strip_suffix(":\\").filter(|l| l.len() == 1)?;
    let device: Vec<u16> = format!("\\\\.\\{letter}:")
        .encode_utf16()
        .chain(Some(0))
        .collect();
    // No access rights are needed to query storage properties, so this works without elevation.
    let handle = unsafe {
        CreateFileW(
            device.as_ptr(),
            0,
            3,
            std::ptr::null_mut(),
            3,
            0,
            std::ptr::null_mut(),
        )
    };
    if handle == -1 {
        return None;
    }
    let query = Query {
        property: STORAGE_DEVICE_SEEK_PENALTY_PROPERTY,
        kind: 0,
        extra: [0; 4],
    };
    let mut answer = SeekPenalty::default();
    let mut returned = 0u32;
    let ok = unsafe {
        DeviceIoControl(
            handle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query as *const Query as *const std::ffi::c_void,
            std::mem::size_of::<Query>() as u32,
            &mut answer as *mut SeekPenalty as *mut std::ffi::c_void,
            std::mem::size_of::<SeekPenalty>() as u32,
            &mut returned,
            std::ptr::null_mut(),
        )
    };
    unsafe { CloseHandle(handle) };
    (ok != 0 && returned as usize >= std::mem::size_of::<SeekPenalty>() - 3)
        .then_some(answer.incurs_seek_penalty != 0)
}

/// The file system of the volume that holds `path` (or its nearest existing parent) and whether it
/// supports hard links. The build exposes the ~11 GB game RomFS a second time through hard links
/// (toolchain/build.py `mirror`); FAT32 and exFAT have none, so the data folder needs NTFS or ReFS.
pub fn file_system(path: &Path) -> Option<(String, bool)> {
    use std::os::windows::ffi::OsStrExt;
    #[link(name = "kernel32")]
    extern "system" {
        fn GetVolumePathNameW(file: *const u16, volume: *mut u16, length: u32) -> i32;
        fn GetVolumeInformationW(
            root: *const u16,
            name: *mut u16,
            name_size: u32,
            serial: *mut u32,
            max_component: *mut u32,
            flags: *mut u32,
            file_system: *mut u16,
            file_system_size: u32,
        ) -> i32;
    }
    const FILE_SUPPORTS_HARD_LINKS: u32 = 0x0040_0000;
    let existing = path.ancestors().find(|p| p.is_dir())?;
    let wide: Vec<u16> = existing.as_os_str().encode_wide().chain(Some(0)).collect();
    let mut volume = vec![0u16; 32768];
    if unsafe { GetVolumePathNameW(wide.as_ptr(), volume.as_mut_ptr(), volume.len() as u32) } == 0 {
        return None;
    }
    let (mut flags, mut name) = (0u32, [0u16; 64]);
    let ok = unsafe {
        GetVolumeInformationW(
            volume.as_ptr(),
            std::ptr::null_mut(),
            0,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
            &mut flags,
            name.as_mut_ptr(),
            name.len() as u32,
        )
    };
    let length = name.iter().position(|&c| c == 0)?;
    (ok != 0).then(|| {
        (
            String::from_utf16_lossy(&name[..length]),
            flags & FILE_SUPPORTS_HARD_LINKS != 0,
        )
    })
}

/// Refuses a data folder whose file system cannot hold the build's hard links.
pub(crate) fn require_hard_links(data: &Path) -> Result<(), String> {
    hard_link_refusal(file_system(data))
}

fn hard_link_refusal(found: Option<(String, bool)>) -> Result<(), String> {
    match found {
        Some((name, false)) => Err(format!(
            "A pasta de dados está num disco {name}, que não aceita hard links. A preparação do jogo precisa de um disco NTFS: escolha outra pasta ou converta o disco para NTFS."
        )),
        _ => Ok(()),
    }
}

fn powershell(script: &str, env: &[(&str, &Path)]) -> Result<(), Error> {
    use std::os::windows::process::CommandExt;
    let mut command = Command::new("powershell.exe");
    command
        .args([
            "-NoProfile",
            "-NonInteractive",
            "-ExecutionPolicy",
            "Bypass",
            "-Command",
            script,
        ])
        .creation_flags(NO_WINDOW);
    for (key, path) in env {
        command.env(key, path);
    }
    let output = command.output()?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr)
            .trim()
            .to_string()
            .into());
    }
    Ok(())
}

/// Closes the launcher of this installation (matched by its path: other programs may be called Launcher.exe).
fn stop_launcher(exe: &Path) {
    if exe.is_file() {
        let _ = powershell(
            "Get-Process Launcher -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $env:MK8_EXE } | Stop-Process -Force",
            &[("MK8_EXE", exe)],
        );
    }
}

fn create_shortcut(link: &Path, target: &Path) -> Result<(), Error> {
    if let Some(dir) = link.parent() {
        fs::create_dir_all(dir)?;
    }
    powershell(
        "$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:MK8_LNK); $s.TargetPath = $env:MK8_TARGET; \
         $s.WorkingDirectory = $env:MK8_DIR; $s.IconLocation = $env:MK8_TARGET + ',0'; $s.Description = 'MK8-online'; $s.Save()",
        &[("MK8_LNK", link), ("MK8_TARGET", target), ("MK8_DIR", target.parent().unwrap_or(target))],
    )
}

fn reg(args: &[&str]) -> Result<(), Error> {
    use std::os::windows::process::CommandExt;
    let output = Command::new("reg")
        .args(args)
        .creation_flags(NO_WINDOW)
        .output()?;
    if !output.status.success() {
        return Err(format!(
            "reg {}: {}",
            args[0],
            String::from_utf8_lossy(&output.stderr).trim()
        )
        .into());
    }
    Ok(())
}

/// Copies this program to `target` in chunks (progress from `from` to `to`), through a temporary name, retrying
/// while the previous launcher still holds the file (an update closes it right before).
fn copy_self(target: &Path, progress: &Mutex<Progress>, from: f64, to: f64) -> Result<(), Error> {
    let exe = std::env::current_exe()?;
    if exe.canonicalize().ok() == target.canonicalize().ok() {
        return Ok(());
    }
    let partial = target.with_extension(format!(
        "exe.new-{:016x}",
        u64::from_le_bytes(mk8_transport::random())
    ));
    let mut source = fs::File::open(&exe)?;
    let total = source.metadata()?.len().max(1);
    let mut out = fs::File::create_new(&partial)?;
    let mut buffer = vec![0u8; 1 << 20];
    let mut copied = 0u64;
    loop {
        let n = source.read(&mut buffer)?;
        if n == 0 {
            break;
        }
        out.write_all(&buffer[..n])?;
        copied += n as u64;
        report(
            progress,
            from + (to - from) * copied as f64 / total as f64,
            "Copiando o MK8-online",
        );
    }
    out.sync_all()?;
    drop(out);
    let deadline = Instant::now() + Duration::from_secs(30);
    loop {
        match fs::rename(&partial, target) {
            Ok(()) => return Ok(()),
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                std::thread::sleep(Duration::from_millis(300));
            }
            Err(error) => {
                let _ = fs::remove_file(&partial);
                return Err(format!(
                    "O MK8-online anterior ainda está aberto ({error}). Feche-o e tente de novo."
                )
                .into());
            }
        }
    }
}

fn same_dir(a: &str, b: &Path) -> bool {
    Path::new(a.trim_end_matches('\\')) == b
}

pub fn install(options: &Options, progress: &Mutex<Progress>) -> Result<(), Error> {
    let result = install_steps(options, progress);
    let mut p = progress.lock().unwrap();
    match &result {
        Ok(()) => {
            p.percent = 100.0;
            p.step = "Tudo pronto".into();
            p.state = "done".into();
        }
        Err(error) => {
            p.state = "failed".into();
            p.error = Some(error.to_string());
        }
    }
    result
}

fn install_steps(options: &Options, progress: &Mutex<Progress>) -> Result<(), Error> {
    let (app, data) = (&options.app, &options.data);
    if !app.is_absolute() || !data.is_absolute() {
        return Err("Escolha pastas completas (por exemplo C:\\Jogos\\MK8-online).".into());
    }
    report(progress, 2.0, "Preparando as pastas");
    fs::create_dir_all(app)?;
    fs::create_dir_all(data)?;
    let exe = app.join("Launcher.exe");
    report(progress, 5.0, "Preparando a instalação");
    let uninstaller = app.join("Uninstall.exe");
    for part in uninstaller.ancestors() {
        crate::python_setup::no_link(part)?;
    }
    if uninstaller.exists()
        && !(reg_query(UNINSTALL_KEY, "InstallLocation").is_some_and(|p| same_dir(&p, app))
            && reg_query(UNINSTALL_KEY, "UninstallString")
                .is_some_and(|p| p.trim_matches('"') == uninstaller.to_string_lossy()))
        && mk8_manifest::hash_file(&uninstaller)?.sha256
            != mk8_manifest::hash_file(&std::env::current_exe()?)?.sha256
    {
        return Err("Já existe um Uninstall.exe de outro programa nessa pasta. Escolha uma pasta própria para o MK8-online.".into());
    }
    stop_launcher(&exe);
    copy_self(&exe, progress, 6.0, 32.0)?;
    copy_self(&uninstaller, progress, 32.0, 40.0)?;
    crate::managed::create_data(data)?;
    fs::write(
        app.join("mk8-online.json"),
        format!("{}\r\n", json!({ "home": data })),
    )?;

    report(progress, 42.0, "Preparando os arquivos do launcher");
    #[cfg(mk8_portable)]
    {
        // (the launcher would do this on its first start; here it shows in the progress)
        let base = data.clone();
        let worker =
            std::thread::spawn(move || crate::unpack_payload(&base).map_err(|e| e.to_string()));
        let mut percent = 42.0;
        while !worker.is_finished() {
            std::thread::sleep(Duration::from_millis(120));
            percent += (86.0 - percent) * 0.035;
            report(progress, percent, "Preparando os arquivos do launcher");
        }
        worker
            .join()
            .map_err(|_| "falha ao preparar os arquivos")??;
    }

    report(progress, 88.0, "Criando atalhos");
    let (start_menu, desktop) = shortcut_paths();
    create_shortcut(&start_menu, &exe)?;
    if let Some(desktop) = desktop {
        if options.desktop {
            create_shortcut(&desktop, &exe)?;
        } else if desktop.is_file() {
            let _ = fs::remove_file(desktop);
        }
    }

    report(progress, 94.0, "Registrando no Windows");
    let exe_text = exe.display().to_string();
    let size_kb = fs::metadata(&exe)
        .map(|m| m.len() / 1024)
        .unwrap_or(0)
        .to_string();
    for (name, kind, data) in [
        ("DisplayName", "REG_SZ", "MK8-online".to_string()),
        ("DisplayVersion", "REG_SZ", update::CURRENT.to_string()),
        ("Publisher", "REG_SZ", "MK8-online contributors".to_string()),
        ("InstallLocation", "REG_SZ", app.display().to_string()),
        ("DisplayIcon", "REG_SZ", format!("{exe_text},0")),
        (
            "UninstallString",
            "REG_SZ",
            format!("\"{}\"", uninstaller.display()),
        ),
        (
            "QuietUninstallString",
            "REG_SZ",
            format!("\"{}\" /SILENT", uninstaller.display()),
        ),
        ("DataFolder", "REG_SZ", data.display().to_string()),
        ("NoModify", "REG_DWORD", "1".to_string()),
        ("NoRepair", "REG_DWORD", "1".to_string()),
        ("EstimatedSize", "REG_DWORD", size_kb),
    ] {
        reg(&[
            "add",
            UNINSTALL_KEY,
            "/v",
            name,
            "/t",
            kind,
            "/d",
            &data,
            "/f",
        ])?;
    }
    // An installation by the earlier Inno Setup in the same folder: its uninstaller and menu entries go away.
    if reg_query(INNO_KEY, "InstallLocation").is_some_and(|location| same_dir(&location, app)) {
        let _ = reg(&["delete", INNO_KEY, "/f"]);
        for name in ["unins000.exe", "unins000.dat"] {
            let _ = fs::remove_file(app.join(name));
        }
        if let Some(group) = start_menu.parent().map(|p| p.join("MK8-online")) {
            for entry in fs::read_dir(&group).into_iter().flatten().flatten() {
                if entry.path().extension().is_some_and(|e| e == "lnk") {
                    let _ = fs::remove_file(entry.path());
                }
            }
            let _ = fs::remove_dir(group);
        }
    }
    Ok(())
}

fn launch(app: &Path) -> Result<(), Error> {
    Command::new(app.join("Launcher.exe"))
        .current_dir(app)
        .spawn()?;
    Ok(())
}

/* ---------- uninstalling ---------- */

pub(crate) fn uninstall(app: &Path, purge: bool) -> Result<(), Error> {
    let registered =
        reg_query(UNINSTALL_KEY, "InstallLocation").ok_or("Instalação não registrada.")?;
    let data = reg_query(UNINSTALL_KEY, "DataFolder").ok_or("Pasta de dados não registrada.")?;
    if crate::canonical(app)? != crate::canonical(Path::new(&registered))? {
        return Err("Esta pasta não corresponde à instalação registrada.".into());
    }
    for part in app.ancestors() {
        crate::python_setup::no_link(part)?;
    }
    let receipt: Value = serde_json::from_slice(&fs::read(app.join("mk8-online.json"))?)?;
    if receipt["home"].as_str().map(Path::new) != Some(Path::new(&data)) {
        return Err("A pasta de dados diverge do registro da instalação.".into());
    }
    let exe = app.join("Launcher.exe");
    stop_launcher(&exe);
    if purge {
        crate::managed::remove_data(Path::new(&data))?;
    }
    for name in ["Launcher.exe", "Uninstall.exe"] {
        remove_file_if_present(&app.join(name))?;
    }
    let (start, desktop) = shortcut_paths();
    remove_file_if_present(&start)?;
    if let Some(p) = desktop {
        remove_file_if_present(&p)?;
    }
    reg(&["delete", UNINSTALL_KEY, "/f"])?;
    remove_file_if_present(&app.join("mk8-online.json"))?;
    let _ = fs::remove_dir(app); // only if empty
    Ok(())
}

fn remove_file_if_present(path: &Path) -> Result<(), Error> {
    match fs::remove_file(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        result => Ok(result?),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn data_folder_needs_a_file_system_with_hard_links() {
        // The test drive (NTFS) supports them; a folder not created yet is judged by its parent.
        let missing = std::env::temp_dir().join("mk8-fs-check-missing/nested");
        let (name, links) = file_system(&missing).unwrap();
        assert!(!name.is_empty());
        assert_eq!(links, name != "FAT32" && name != "exFAT" && name != "FAT");
        assert!(require_hard_links(&std::env::temp_dir()).is_ok());
        let refused = hard_link_refusal(Some(("exFAT".into(), false))).unwrap_err();
        assert!(
            refused.contains("exFAT") && refused.contains("NTFS"),
            "{refused}"
        );
        assert!(hard_link_refusal(Some(("NTFS".into(), true))).is_ok());
        // Unknown volumes (network shares) are not refused here.
        assert!(hard_link_refusal(None).is_ok());
    }

    fn args(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    #[test]
    fn modes_and_inno_style_options() {
        assert!(matches!(
            mode(&args(&["x", "--install"])),
            Some(Mode::Install { silent: false })
        ));
        assert!(matches!(
            mode(&args(&["x", "--install", "/SILENT"])),
            Some(Mode::Install { silent: true })
        ));
        assert!(matches!(
            mode(&args(&["x", "--uninstall"])),
            Some(Mode::Uninstall { silent: false })
        ));
        assert!(matches!(
            mode(&args(&["x", "--uninstall-run", "C:\\MK8", "/SILENT"])),
            Some(Mode::UninstallRun { silent: true, .. })
        ));
        assert!(mode(&args(&["x", "--headless"])).is_none());
        let options = args(&[
            "x",
            "/SILENT",
            "/DATADIR=\"D:\\MK8 dados\"",
            "/dir=C:\\Programas\\MK8",
        ]);
        assert_eq!(
            value(&options, "/DATADIR").as_deref(),
            Some("D:\\MK8 dados")
        );
        assert_eq!(
            value(&options, "/DIR").as_deref(),
            Some("C:\\Programas\\MK8")
        );
        assert_eq!(value(&options, "/MISSING"), None);
    }

    #[test]
    fn free_space_of_a_folder_still_to_be_created() {
        let missing = std::env::temp_dir().join("mk8-online-nao-existe/ainda/nao");
        assert!(free_gib(&missing).is_some_and(|gib| gib > 0.0));
    }

    #[test]
    fn disk_kind_of_a_missing_folder_matches_its_existing_parent() {
        // Storage drivers may not report seek penalty (including on CI virtual disks).
        // Unknown is valid; looking up a missing folder must preserve the parent's result.
        let parent = std::env::temp_dir();
        let missing = parent.join(format!(
            "mk8-disk-query-{}-{}/ainda/nao",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        assert!(!missing.exists());
        assert_eq!(hard_disk(&missing), hard_disk(&parent));
    }

    #[test]
    fn install_copies_writes_the_data_folder_and_shortcuts_into_given_folders() {
        // Only the file steps: the registry and shortcut steps are exercised by the setup test script.
        let base = std::env::temp_dir().join(format!("mk8-setup-{}", std::process::id()));
        let _ = fs::remove_dir_all(&base);
        let progress = Mutex::new(Progress::default());
        let target = base.join("app/Launcher.exe");
        fs::create_dir_all(target.parent().unwrap()).unwrap();
        copy_self(&target, &progress, 0.0, 40.0).unwrap();
        assert_eq!(
            fs::metadata(&target).unwrap().len(),
            fs::metadata(std::env::current_exe().unwrap())
                .unwrap()
                .len()
        );
        assert!((progress.lock().unwrap().percent - 40.0).abs() < 1e-9);
        assert!(!base.join("app/Launcher.exe.new").exists());
        fs::remove_dir_all(&base).unwrap();
    }
}
