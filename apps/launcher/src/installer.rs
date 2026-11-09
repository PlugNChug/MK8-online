//! The setup program: the launcher binary itself, named `MK8-online-<version>-setup.exe` (or run with
//! `--install`). It shows its own window (welcome, folders, desktop shortcut, progress), copies itself as
//! `Launcher.exe`, writes `mk8-online.json` (the data folder), unpacks this version's sources ahead of the first
//! start, creates the shortcuts and registers the uninstaller under the current user (no administrator rights).
//!
//! `/SILENT` (the in-launcher updater) installs over the previous installation with its folders and starts the
//! new launcher; `/DIR=` and `/DATADIR=` choose the folders, `/DESKTOP=0|1` the desktop shortcut and `/NOLAUNCH`
//! skips starting it (tests). `--uninstall` (Windows Settings) removes the program
//! and its shortcuts and keeps the data folder: prepared game, builds and saves.
use serde_json::{json, Value};
use std::{
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    process::Command,
    sync::{Arc, Mutex, OnceLock},
    time::{Duration, Instant},
};
use tauri::{WebviewUrl, WebviewWindowBuilder};
use tiny_http::{Method, Server};

use crate::{server::response, update, Error};

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
    if flag(args, &["--uninstall"]) {
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
        Mode::Install { silent: false } => window(),
        Mode::Uninstall { silent } => {
            // Run from a copy in %TEMP%, so the installed Launcher.exe can be deleted.
            let exe = std::env::current_exe()?;
            let app = exe.parent().ok_or("pasta do programa")?.to_path_buf();
            let temp = std::env::temp_dir()
                .join(format!("mk8-online-uninstall-{}.exe", std::process::id()));
            fs::copy(&exe, &temp)?;
            let mut command = Command::new(&temp);
            command.arg("--uninstall-run").arg(&app);
            if silent {
                command.arg("/SILENT");
            }
            command.spawn()?;
            Ok(())
        }
        Mode::UninstallRun { app, silent } => uninstall(&app, silent),
    }
}

/* ---------- folders of a previous installation ---------- */

fn reg_query(key: &str, name: &str) -> Option<String> {
    use std::os::windows::process::CommandExt;
    let output = Command::new("reg")
        .args(["query", key, "/v", name])
        .creation_flags(NO_WINDOW)
        .output()
        .ok()?;
    let text = String::from_utf8_lossy(&output.stdout);
    text.lines().find_map(|line| {
        let line = line.trim();
        let rest = line.strip_prefix(name)?.trim_start();
        let rest = rest
            .strip_prefix("REG_SZ")
            .or_else(|| rest.strip_prefix("REG_EXPAND_SZ"))?;
        Some(rest.trim().to_string()).filter(|v| !v.is_empty())
    })
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
    let partial = target.with_extension("exe.new");
    let mut source = fs::File::open(&exe)?;
    let total = source.metadata()?.len().max(1);
    let mut out = fs::File::create(&partial)?;
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
    stop_launcher(&exe);
    copy_self(&exe, progress, 6.0, 40.0)?;
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
            format!("\"{exe_text}\" --uninstall"),
        ),
        (
            "QuietUninstallString",
            "REG_SZ",
            format!("\"{exe_text}\" --uninstall /SILENT"),
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

fn uninstall(app: &Path, silent: bool) -> Result<(), Error> {
    let exe = app.join("Launcher.exe");
    let data = fs::read_to_string(app.join("mk8-online.json"))
        .ok()
        .and_then(|text| serde_json::from_str::<Value>(&text).ok())
        .and_then(|v| v["home"].as_str().map(str::to_string))
        .unwrap_or_else(|| local_appdata().join("MK8-online").display().to_string());
    let english = crate::language::read(&PathBuf::from(&data).join("data")) != "pt-BR";
    if !silent {
        let answer = rfd::MessageDialog::new()
            .set_title(if english { "Uninstall MK8-online" } else { "Desinstalar MK8-online" })
            .set_level(rfd::MessageLevel::Info)
            .set_buttons(rfd::MessageButtons::YesNo)
            .set_description(if english { format!(
                "Remove MK8-online from this PC?\n\nYour data (prepared game, builds and saves) will remain in:\n{data}\nYou can remove that folder later to free up space."
            ) } else { format!(
                "Remover o MK8-online deste computador?\n\nSeus dados (jogo preparado, builds e saves) ficam em:\n{data}\nApague essa pasta depois, se quiser liberar o espaço."
            ) })
            .show();
        if answer != rfd::MessageDialogResult::Yes {
            return Ok(());
        }
    }
    stop_launcher(&exe);
    let deadline = Instant::now() + Duration::from_secs(15);
    while exe.exists() && fs::remove_file(&exe).is_err() && Instant::now() < deadline {
        std::thread::sleep(Duration::from_millis(300));
    }
    for name in [
        "mk8-online.json",
        "Launcher.exe.new",
        "unins000.exe",
        "unins000.dat",
    ] {
        let _ = fs::remove_file(app.join(name));
    }
    let _ = fs::remove_dir(app); // (only if empty)
    let (start_menu, desktop) = shortcut_paths();
    let _ = fs::remove_file(start_menu);
    if let Some(desktop) = desktop {
        let _ = fs::remove_file(desktop);
    }
    let _ = reg(&["delete", UNINSTALL_KEY, "/f"]);
    if !silent {
        rfd::MessageDialog::new()
            .set_title("MK8-online")
            .set_level(rfd::MessageLevel::Info)
            .set_description(if english {
                format!("MK8-online has been removed. Your data remains in {data}.")
            } else {
                format!("O MK8-online foi removido. Seus dados continuam em {data}.")
            })
            .show();
    }
    // This copy in %TEMP% removes itself once it has exited.
    if let Ok(me) = std::env::current_exe() {
        use std::os::windows::process::CommandExt;
        // (raw: cmd parses its own quoting; the escaped form Rust would build is not understood)
        let _ = Command::new("cmd")
            .raw_arg(format!(
                "/d /c ping -n 3 127.0.0.1 >nul & del /f /q \"{}\"",
                me.display()
            ))
            .creation_flags(NO_WINDOW)
            .spawn();
    }
    Ok(())
}

/* ---------- the setup window ---------- */

type Parent = Arc<OnceLock<tauri::WebviewWindow>>;

struct Setup {
    previous: Previous,
    progress: Arc<Mutex<Progress>>,
    app: Option<PathBuf>,
    started: bool,
    language: String,
}

fn window() -> Result<(), Error> {
    let server = Server::http(("127.0.0.1", 0))?;
    let authority = server.server_addr().to_string();
    let url = format!("http://{authority}");
    let token = mk8_transport::random::<32>()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let parent: Parent = Arc::new(OnceLock::new());
    let previous = Previous::find();
    let setup = Setup {
        language: crate::language::read(&previous.data.join("data")),
        previous,
        progress: Arc::new(Mutex::new(Progress::default())),
        app: None,
        started: false,
    };
    {
        let parent = parent.clone();
        let url = url.clone();
        std::thread::spawn(move || serve(setup, server, authority, url, token, parent));
    }
    let target: tauri::Url = url.parse()?;
    let origin = target.origin();
    tauri::Builder::default()
        .setup(move |tauri_app| {
            let window =
                WebviewWindowBuilder::new(tauri_app, "setup", WebviewUrl::External(target))
                    .title("MK8-online Setup")
                    .inner_size(880.0, 620.0)
                    .resizable(false)
                    .maximizable(false)
                    .theme(Some(tauri::Theme::Dark))
                    .center()
                    .disable_drag_drop_handler()
                    .on_navigation(move |url| url.origin() == origin)
                    .build()?;
            let _ = parent.set(window);
            Ok(())
        })
        .run(tauri::generate_context!())?;
    Ok(())
}

fn state(setup: &Setup) -> Value {
    let previous = &setup.previous;
    json!({
        "version": update::CURRENT,
        "app": previous.app,
        "data": previous.data,
        "desktop": previous.desktop,
        "update": previous.found,
        "free_gib": free_gib(&previous.data),
        "hard_disk": hard_disk(&previous.data),
        "needed_gib": NEEDED_GIB,
    })
}

fn handle(setup: &mut Setup, method: &Method, path: &str, body: &str) -> Result<Value, String> {
    let body: Value = serde_json::from_str(body).unwrap_or_default();
    match (method.as_str(), path) {
        ("GET", "/api/state") => Ok(state(setup)),
        ("POST", "/api/language") => {
            let language = body["language"]
                .as_str()
                .filter(|value| crate::language::valid(value))
                .ok_or("Unsupported language / Idioma não suportado")?;
            setup.language = language.into();
            Ok(json!(language))
        }
        ("POST", "/api/space") => {
            let path = PathBuf::from(body["path"].as_str().unwrap_or(""));
            let absolute = path.is_absolute();
            Ok(json!({
                "free_gib": absolute.then(|| free_gib(&path)).flatten(),
                "hard_disk": absolute.then(|| hard_disk(&path)).flatten(),
            }))
        }
        ("POST", "/api/install") => {
            if setup.started && setup.progress.lock().unwrap().state == "running" {
                return Err("A instalação já está em andamento.".into());
            }
            let folder = |key: &str| {
                body[key]
                    .as_str()
                    .map(|p| PathBuf::from(p.trim()))
                    .filter(|p| p.is_absolute())
                    .ok_or_else(|| {
                        "Escolha pastas completas (por exemplo C:\\Jogos\\MK8-online).".to_string()
                    })
            };
            let options = Options {
                app: folder("app")?,
                data: folder("data")?,
                desktop: body["desktop"].as_bool().unwrap_or(true),
            };
            let language = body["language"].as_str().unwrap_or(&setup.language);
            if !crate::language::valid(language) {
                return Err("Unsupported language / Idioma não suportado".into());
            }
            crate::language::write(&options.data.join("data"), &json!({"language": language}))?;
            setup.language = language.into();
            *setup.progress.lock().unwrap() = Progress {
                state: "running".into(),
                ..Progress::default()
            };
            setup.started = true;
            setup.app = Some(options.app.clone());
            let progress = setup.progress.clone();
            std::thread::spawn(move || {
                let _ = install(&options, &progress);
            });
            Ok(json!({}))
        }
        ("GET", "/api/progress") => {
            let p = setup.progress.lock().unwrap().clone();
            Ok(json!({"percent": p.percent, "step": p.step, "state": p.state, "error": p.error}))
        }
        ("POST", "/api/finish") => {
            if body["launch"].as_bool().unwrap_or(false) {
                if let Some(app) = setup.app.as_ref() {
                    launch(app).map_err(|e| e.to_string())?;
                }
            }
            Ok(json!({"exit": true}))
        }
        _ => Err("Rota não encontrada".into()),
    }
}

fn serve(
    mut setup: Setup,
    server: Server,
    authority: String,
    url: String,
    token: String,
    parent: Parent,
) {
    for mut req in server.incoming_requests() {
        let host = req
            .headers()
            .iter()
            .find(|h| h.field.equiv("Host"))
            .map(|h| h.value.as_str());
        if host != Some(authority.as_str()) {
            response(req, 403, "Host inválido".into(), "text/plain");
            continue;
        }
        let path = req.url().to_string();
        let method = req.method().clone();
        let page = match (method.as_str(), path.as_str()) {
            ("GET", "/") => Some((
                include_str!("../web/setup.html")
                    .replace("__TOKEN__", &token)
                    .replace("__LANGUAGE__", &setup.language),
                "text/html; charset=utf-8",
            )),
            ("GET", "/i18n.js") => Some((
                include_str!("../web/i18n.js").into(),
                "text/javascript; charset=utf-8",
            )),
            ("GET", "/en.json") => Some((
                include_str!("../web/en.json").into(),
                "application/json; charset=utf-8",
            )),
            ("GET", "/setup.js") => Some((
                include_str!("../web/setup.js").to_string(),
                "text/javascript; charset=utf-8",
            )),
            ("GET", "/style.css") => Some((
                include_str!("../web/style.css").to_string(),
                "text/css; charset=utf-8",
            )),
            ("GET", "/setup.css") => Some((
                include_str!("../web/setup.css").to_string(),
                "text/css; charset=utf-8",
            )),
            _ => None,
        };
        if let Some((text, mime)) = page {
            response(req, 200, text, mime);
            continue;
        }
        let authenticated = req
            .headers()
            .iter()
            .any(|h| h.field.equiv("X-MK8-Token") && h.value.as_str() == token);
        let origin = req
            .headers()
            .iter()
            .find(|h| h.field.equiv("Origin"))
            .map(|h| h.value.as_str());
        if !authenticated || origin.is_some_and(|value| value != url) {
            response(
                req,
                403,
                "{\"ok\":false,\"error\":\"Sessão local inválida\"}".into(),
                "application/json",
            );
            continue;
        }
        let mut body = String::new();
        if req
            .as_reader()
            .take(8193)
            .read_to_string(&mut body)
            .is_err()
            || body.len() > 8192
        {
            response(req, 413, "{}".into(), "application/json");
            continue;
        }
        if method == Method::Post && path == "/api/browse" {
            let value: Value = serde_json::from_str(&body).unwrap_or_default();
            let current = PathBuf::from(value["current"].as_str().unwrap_or(""));
            let title = value["title"]
                .as_str()
                .unwrap_or("Escolher pasta")
                .chars()
                .take(80)
                .collect::<String>();
            let parent = parent.clone();
            std::thread::spawn(move || {
                let mut dialog = rfd::FileDialog::new().set_title(title);
                if let Some(window) = parent.get() {
                    dialog = dialog.set_parent(window);
                }
                if let Some(dir) = current.ancestors().find(|p| p.is_dir()) {
                    dialog = dialog.set_directory(dir);
                }
                let picked = dialog.pick_folder().map(|p| p.display().to_string());
                response(
                    req,
                    200,
                    json!({"ok": true, "data": {"path": picked}}).to_string(),
                    "application/json; charset=utf-8",
                );
            });
            continue;
        }
        let (code, data, exit) = match handle(&mut setup, &method, &path, &body) {
            Ok(value) => {
                let exit = value["exit"] == true;
                (200, json!({"ok": true, "data": value}), exit)
            }
            Err(error) => (400, json!({"ok": false, "error": error}), false),
        };
        response(
            req,
            code,
            data.to_string(),
            "application/json; charset=utf-8",
        );
        if exit {
            std::thread::sleep(Duration::from_millis(300));
            std::process::exit(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn disk_kind_of_the_system_drive_is_reported() {
        // Every local fixed disk answers the seek-penalty query (true on a hard disk, false on an
        // SSD); a folder still to be created is looked up through its existing parent.
        let missing = std::env::temp_dir().join("mk8-online-nao-existe/ainda/nao");
        assert!(hard_disk(&missing).is_some());
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
