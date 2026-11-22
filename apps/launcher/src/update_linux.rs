//! A same-filesystem exchange leaves the package entry point present through every update phase.
use super::{download_verified, Release, CURRENT};
use flate2::read::GzDecoder;
use mk8_manifest::Result;
use serde::{Deserialize, Serialize};
use std::{
    collections::HashSet,
    ffi::{CString, OsStr, OsString},
    fs::{self, DirBuilder, File, OpenOptions, TryLockError},
    io::{self, Read, Write},
    os::{
        fd::{AsRawFd, FromRawFd, OwnedFd, RawFd},
        unix::{
            ffi::OsStrExt,
            fs::{DirBuilderExt, MetadataExt, OpenOptionsExt, PermissionsExt},
            process::CommandExt,
        },
    },
    path::{Component, Path, PathBuf},
    process::{Child, Command, Stdio},
    time::{Duration, Instant},
};

const HANDOFF: &str = "--linux-update-handoff";
const HEALTH: &str = "MK8_UPDATE_HEALTH";
const MARKER: &str = ".mk8-package.json";
const PACKAGE_DIR: &str = "MK8-online";
const MAX_PAYLOAD: u64 = 2 << 30;
const RESERVE: u64 = 4 << 30;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
enum Format {
    AppImage,
    Tar,
}

impl Format {
    fn running() -> Self {
        #[cfg(mk8_portable)]
        if let Some((_, appimage)) = crate::installer::installed_target_for_process() {
            return if appimage { Self::AppImage } else { Self::Tar };
        }
        if std::env::var_os("APPIMAGE").is_some() || std::env::var_os("MK8_PACKAGE_ROOT").is_none()
        {
            Self::AppImage
        } else {
            Self::Tar
        }
    }

    fn suffix(self) -> &'static str {
        match self {
            Self::AppImage => "-linux-x86_64.appimage",
            Self::Tar => "-linux-x86_64.tar.gz",
        }
    }

    fn validate(self, path: &Path, version: Option<&str>) -> io::Result<()> {
        match self {
            Self::AppImage => {
                let mut header = [0; 20];
                File::open(path)?.read_exact(&mut header)?;
                if header[..6] != *b"\x7fELF\x02\x01"
                    || header[8..11] != *b"AI\x02"
                    || header[18..20] != 62u16.to_le_bytes()
                {
                    return Err(invalid("AppImage x86-64 inválido."));
                }
            }
            Self::Tar => {
                let info: serde_json::Value =
                    serde_json::from_reader(File::open(path.join(MARKER))?)?;
                let executable = path.join("AppRun").symlink_metadata()?;
                if info["format"] != "tar.gz"
                    || info["arch"] != "x86_64"
                    || version.is_some_and(|v| info["version"] != v)
                    || !executable.is_file()
                    || executable.mode() & 0o111 == 0
                {
                    return Err(invalid("Pacote Linux x86-64 inválido."));
                }
            }
        }
        Ok(())
    }
}

pub fn release_suffix() -> &'static str {
    Format::running().suffix()
}

pub fn package_present() -> bool {
    Package::locate().is_ok()
}

fn invalid(detail: &str) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, detail)
}

fn validate_inventory(root: &Path) -> io::Result<()> {
    #[derive(Deserialize)]
    struct Inventory {
        entries: HashSet<PathBuf>,
    }
    let Inventory { mut entries } = serde_json::from_reader(File::open(root.join(MARKER))?)?;
    let mut dirs = vec![root.to_path_buf()];
    while let Some(dir) = dirs.pop() {
        for item in fs::read_dir(dir)? {
            let item = item?;
            let path = item.path();
            let relative = path.strip_prefix(root).map_err(io::Error::other)?;
            let file_type = item.file_type()?;
            if relative == Path::new(MARKER) && file_type.is_file() {
                continue;
            }
            if !entries.remove(relative) || (!file_type.is_file() && !file_type.is_dir()) {
                return Err(invalid(
                    "A pasta do aplicativo contém arquivos extras. Mova os seus arquivos para a pasta de dados antes de atualizar.",
                ));
            }
            if file_type.is_dir() {
                dirs.push(path);
            }
        }
    }
    if entries.is_empty() {
        Ok(())
    } else {
        Err(invalid("Faltam arquivos na pasta do aplicativo."))
    }
}

struct Package {
    target: PathBuf,
    stage: PathBuf,
    format: Format,
}

impl Package {
    fn new(target: PathBuf, format: Format) -> io::Result<Self> {
        let name = target
            .file_name()
            .filter(|_| target.is_absolute())
            .ok_or_else(|| invalid("Destino do pacote inválido."))?;
        let mut stage = OsString::from(".");
        stage.push(name);
        stage.push(".mk8-update");
        let stage = target.with_file_name(stage);
        Ok(Self {
            target,
            stage,
            format,
        })
    }

    fn locate() -> io::Result<Self> {
        #[cfg(mk8_portable)]
        if let Some((target, appimage)) = crate::installer::installed_target_for_process() {
            let format = if appimage {
                Format::AppImage
            } else {
                Format::Tar
            };
            format.validate(target, None)?;
            return Self::new(target.canonicalize()?, format);
        }
        let format = Format::running();
        let key = match format {
            Format::AppImage => "APPIMAGE",
            Format::Tar => "MK8_PACKAGE_ROOT",
        };
        let target = PathBuf::from(
            std::env::var_os(key)
                .ok_or_else(|| invalid("Abra o pacote distribuído para atualizar."))?,
        )
        .canonicalize()?;
        if format == Format::Tar && std::env::current_exe()? != target.join("usr/bin/Launcher") {
            return Err(invalid("Pasta do aplicativo incompatível."));
        }
        format.validate(&target, None)?;
        Self::new(target, format)
    }

    fn lock(&self) -> io::Result<Option<File>> {
        let lock = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .custom_flags(libc::O_NOFOLLOW)
            .mode(0o600)
            .open(self.stage.with_extension("mk8-update.lock"))?;
        match lock.try_lock() {
            Ok(()) => Ok(Some(lock)),
            Err(TryLockError::WouldBlock) => Ok(None),
            Err(TryLockError::Error(e)) => Err(e),
        }
    }

    fn payload(&self) -> PathBuf {
        self.stage.join("package")
    }

    fn read_journal(&self) -> io::Result<Journal> {
        let metadata = self.stage.symlink_metadata()?;
        if !metadata.is_dir()
            || metadata.mode() & 0o077 != 0
            || metadata.uid() != unsafe { libc::geteuid() }
        {
            return Err(invalid("Pasta de atualização inesperada."));
        }
        Ok(serde_json::from_reader(File::open(
            self.stage.join("journal.json"),
        )?)?)
    }

    fn launch(&self, journal: &Journal, health: Option<&str>) -> io::Result<Child> {
        let program = match self.format {
            Format::AppImage => self.target.clone(),
            Format::Tar => self.target.join("AppRun"),
        };
        let mut cmd = Command::new(program);
        cmd.current_dir(&journal.data)
            .arg("--data-dir")
            .arg(&journal.data)
            .arg("--port")
            .arg(journal.port.to_string())
            .stdin(Stdio::null());
        if journal.headless {
            cmd.arg("--headless");
        }
        for key in [
            "APPIMAGE",
            "APPDIR",
            "ARGV0",
            "OWD",
            "MK8_PACKAGE_ROOT",
            HEALTH,
        ] {
            cmd.env_remove(key);
        }
        if let Some(health) = health {
            cmd.env(HEALTH, health);
        }
        // A Type 2 runtime started with --appimage-extract-and-run may supply
        // the extraction directory without exporting APPIMAGE_EXTRACT_AND_RUN.
        if self.format == Format::AppImage
            && std::env::var_os("APPDIR").is_some_and(|dir| {
                Path::new(&dir)
                    .file_name()
                    .is_some_and(|name| name.as_bytes().starts_with(b"appimage_extracted_"))
            })
        {
            cmd.env("APPIMAGE_EXTRACT_AND_RUN", "1");
        }
        cmd.spawn()
    }

    fn sync(&self) -> io::Result<()> {
        File::open(&self.stage)?.sync_all()?;
        File::open(self.target.parent().expect("absolute package path"))?.sync_all()
    }

    fn clear_stage(&self) -> io::Result<()> {
        // Retire the journal atomically before deleting anything. An interruption
        // during cleanup must never put a partially removed backup back in service.
        let preserve = self.format == Format::Tar && validate_inventory(&self.payload()).is_err();
        let retired = self
            .stage
            .with_extension(format!("mk8-update-{}.old", nonce()));
        fs::rename(&self.stage, &retired)?;
        File::open(self.target.parent().expect("absolute package path"))?.sync_all()?;
        if preserve {
            Ok(())
        } else {
            fs::remove_dir_all(retired)
        }
    }

    fn exchange(&self) -> io::Result<()> {
        let target = CString::new(self.target.as_os_str().as_bytes())?;
        let candidate = CString::new(self.payload().as_os_str().as_bytes())?;
        let result = unsafe {
            libc::renameat2(
                libc::AT_FDCWD,
                target.as_ptr(),
                libc::AT_FDCWD,
                candidate.as_ptr(),
                libc::RENAME_EXCHANGE,
            )
        };
        if result != 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(())
    }

    // The inode identities make recovery independent of when the helper stopped.
    fn exchanged(&self, journal: &Journal) -> io::Result<bool> {
        let current = Identity::read(&self.target)?;
        if current == journal.before {
            return Ok(false);
        }
        if current == journal.after && Identity::read(&self.payload())? == journal.before {
            return Ok(true);
        }
        Err(invalid(
            "O pacote foi alterado durante a atualização. A cópia anterior foi preservada.",
        ))
    }

    fn healthy(&self, journal: &Journal) -> bool {
        fs::read_to_string(self.stage.join("ready")).is_ok_and(|value| value == journal.nonce)
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
struct Identity {
    dev: u64,
    inode: u64,
}

impl Identity {
    fn read(path: &Path) -> io::Result<Self> {
        let metadata = path.symlink_metadata()?;
        Ok(Self {
            dev: metadata.dev(),
            inode: metadata.ino(),
        })
    }
}

#[derive(Serialize, Deserialize)]
struct Journal {
    before: Identity,
    after: Identity,
    version: String,
    nonce: String,
    data: PathBuf,
    format: Format,
    #[serde(default)]
    headless: bool,
    #[serde(default)]
    port: u16,
}

fn available(path: &Path) -> io::Result<u64> {
    let free = crate::installer::free_gib(path).unwrap_or(0.0);
    let free_bytes = (free * (1u64 << 30) as f64) as u64;
    free_bytes
        .checked_sub(RESERVE)
        .filter(|free| *free > 0)
        .ok_or_else(|| invalid("A atualização precisa preservar a reserva de 4 GiB."))
}

fn extract_tar(archive: &Path, stage: &Path) -> io::Result<()> {
    let decoder = GzDecoder::new(File::open(archive)?).take(MAX_PAYLOAD);
    let mut archive = tar::Archive::new(decoder);
    fs::create_dir(stage)?;
    let mut bytes = available(stage)?.min(MAX_PAYLOAD);
    for entry in archive.entries()? {
        let mut entry = entry?;
        let path = entry.path()?;
        let mut parts = path.components();
        if parts.next() != Some(Component::Normal(OsStr::new(PACKAGE_DIR)))
            || !parts.all(|c| matches!(c, Component::Normal(_)))
        {
            return Err(invalid("Caminho inesperado no pacote Linux."));
        }
        let path = stage.join(path.strip_prefix(PACKAGE_DIR).map_err(io::Error::other)?);
        let kind = entry.header().entry_type();
        if kind.is_dir() {
            fs::create_dir_all(path)?;
        } else if kind.is_file() && path != stage {
            bytes = bytes
                .checked_sub(entry.size())
                .ok_or_else(|| invalid("Pacote Linux grande demais."))?;
            fs::create_dir_all(path.parent().expect("path inside stage"))?;
            let mode = if entry.header().mode()? & 0o111 == 0 {
                0o644
            } else {
                0o755
            };
            let mut out = OpenOptions::new()
                .create_new(true)
                .write(true)
                .mode(mode)
                .open(path)?;
            if io::copy(&mut entry, &mut out)? != entry.size() {
                return Err(invalid("Pacote Linux truncado."));
            }
            out.sync_all()?;
        } else {
            return Err(invalid(
                "O pacote Linux contém um tipo de arquivo inesperado.",
            ));
        }
    }
    Ok(())
}

fn nonce() -> String {
    mk8_transport::random::<16>()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

pub fn download_and_start(local: &Path, release: &Release, expected: &str) -> Result<PathBuf> {
    start(local, release, expected)
        .map_err(|e| format!("Falha ao preparar a atualização Linux: {e}"))
}

fn start(local: &Path, release: &Release, expected: &str) -> io::Result<PathBuf> {
    let package = Package::locate()?;
    let data = local.canonicalize()?;
    let shared_tools =
        std::env::var_os("MK8_NATIVE_TOOLS").and_then(|p| PathBuf::from(p).canonicalize().ok());
    if package.format == Format::Tar
        && (data.starts_with(&package.target)
            || shared_tools.is_some_and(|p| p.starts_with(&package.target)))
    {
        return Err(invalid(
            "A pasta de dados deve ficar fora da pasta do aplicativo.",
        ));
    }

    if package.format == Format::Tar {
        validate_inventory(&package.target)?;
    }
    let lock = package
        .lock()?
        .ok_or_else(|| invalid("Uma atualização já está em andamento."))?;
    DirBuilder::new().mode(0o700).create(&package.stage)?;
    let result = (|| {
        let archive = package.stage.join("download");
        let max_bytes = available(&package.stage)?.min(MAX_PAYLOAD);
        download_verified(&release.setup_url, expected, &archive, max_bytes)
            .map_err(io::Error::other)?;
        let payload = package.payload();
        match package.format {
            Format::AppImage => {
                fs::set_permissions(&archive, fs::Permissions::from_mode(0o755))?;
                File::open(&archive)?.sync_all()?;
                fs::rename(&archive, &payload)?;
            }
            Format::Tar => {
                extract_tar(&archive, &payload)?;
                validate_inventory(&payload)?;
                fs::remove_file(&archive)?;
            }
        }
        let version = release.tag.trim_start_matches(['v', 'V']);
        package.format.validate(&payload, Some(version))?;
        let args: Vec<_> = std::env::args().collect();
        let journal = Journal {
            headless: args
                .iter()
                .any(|arg| matches!(arg.as_str(), "--headless" | "--no-browser")),
            port: crate::argument(&args, "--port")
                .and_then(|v| v.parse().ok())
                .unwrap_or(0),
            before: Identity::read(&package.target)?,
            after: Identity::read(&payload)?,
            version: version.into(),
            nonce: nonce(),
            data,
            format: package.format,
        };
        let pending = package.stage.join("journal.pending");
        let file = File::create(&pending)?;
        serde_json::to_writer(&file, &journal)?;
        file.sync_all()?;
        fs::rename(pending, package.stage.join("journal.json"))?;
        package.sync()?;
        spawn_handoff(&package, &journal, lock)
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(&package.stage);
    }
    result?;
    Ok(package.target)
}

fn spawn_handoff(package: &Package, journal: &Journal, lock: File) -> io::Result<()> {
    let fd = unsafe { libc::syscall(libc::SYS_pidfd_open, std::process::id(), 0) };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let wait = unsafe { OwnedFd::from_raw_fd(fd as RawFd) };
    let fds = [wait.as_raw_fd(), lock.as_raw_fd()];
    let mut command = Command::new(std::env::current_exe()?);
    command
        .arg(HANDOFF)
        .arg(&package.target)
        .arg(fds[0].to_string())
        .arg(fds[1].to_string())
        .current_dir(&journal.data)
        .stdin(Stdio::null());
    if journal.headless {
        command.arg("--headless");
    }
    // The helper needs the original process handle and the already held lock.
    unsafe {
        command.pre_exec(move || {
            for fd in fds {
                if libc::fcntl(fd, libc::F_SETFD, 0) < 0 {
                    return Err(io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    command.spawn()?;
    Ok(())
}

pub fn is_handoff(args: &[String]) -> bool {
    args.get(1).is_some_and(|value| value == HANDOFF)
}

pub fn run_handoff(args: &[String]) -> Result<()> {
    handoff(args).map_err(|e| format!("Falha na atualização Linux: {e}"))
}

fn handoff(args: &[String]) -> io::Result<()> {
    let args = if args.last().is_some_and(|arg| arg == "--headless") {
        &args[..args.len() - 1]
    } else {
        args
    };
    let [_, _, target, wait, lock] = args else {
        return Err(invalid("Argumentos de atualização inválidos."));
    };
    let target = PathBuf::from(target);
    let format = if target.is_dir() {
        Format::Tar
    } else {
        Format::AppImage
    };
    let package = Package::new(target, format)?;
    let journal = package.read_journal()?;
    if journal.format != package.format {
        return Err(invalid("Formato de atualização incompatível."));
    }

    let inherit = |text: &str| -> io::Result<OwnedFd> {
        let fd = text
            .parse::<RawFd>()
            .ok()
            .filter(|fd| *fd > 2)
            .ok_or_else(|| invalid("Descritor de atualização inválido."))?;
        if unsafe { libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC) } < 0 {
            return Err(io::Error::last_os_error());
        }
        Ok(unsafe { OwnedFd::from_raw_fd(fd) })
    };
    if wait == lock {
        return Err(invalid("Descritores de atualização repetidos."));
    }
    let _lock = inherit(lock)?;
    let wait = inherit(wait)?;
    let mut poll = libc::pollfd {
        fd: wait.as_raw_fd(),
        events: libc::POLLIN,
        revents: 0,
    };
    if unsafe { libc::poll(&mut poll, 1, 30_000) } != 1 || poll.revents & libc::POLLIN == 0 {
        return Err(invalid(
            "O launcher precisa fechar antes da troca do pacote.",
        ));
    }
    activate(&package, &journal)
}

fn activate(package: &Package, journal: &Journal) -> io::Result<()> {
    if package.exchanged(journal)? || Identity::read(&package.payload())? != journal.after {
        return Err(invalid("O pacote mudou antes da instalação."));
    }
    let swap = (|| {
        if package.format == Format::Tar {
            validate_inventory(&package.target)?;
        }
        package.exchange()
    })();
    if let Err(error) = swap {
        fs::remove_dir_all(&package.stage)?;
        package.launch(journal, None)?;
        return Err(io::Error::other(format!(
            "Não foi possível trocar o pacote neste sistema de arquivos: {error}"
        )));
    }

    let result = package.sync().and_then(|()| {
        let mut child = package.launch(journal, Some(&journal.nonce))?;
        let deadline = Instant::now() + Duration::from_secs(45);
        let failure = loop {
            if package.healthy(journal) {
                return Ok(());
            }
            match child.try_wait() {
                Ok(Some(status)) => {
                    break format!("O novo launcher fechou durante a abertura ({status}).")
                }
                Ok(None) if Instant::now() < deadline => {
                    std::thread::sleep(Duration::from_millis(100))
                }
                Ok(None) => break "O novo launcher não confirmou a abertura.".into(),
                Err(error) => break error.to_string(),
            }
        };
        let _ = child.kill();
        child.wait()?;
        Err(io::Error::other(failure))
    });
    if result.is_err() {
        if !package.exchanged(journal)? {
            return Err(invalid("A cópia anterior do pacote não está disponível."));
        }
        package.exchange()?;
        package.sync()?;
        package.launch(journal, None)?;
    }
    package.clear_stage()?;
    result
}

pub fn mark_ready() {
    let Some(nonce) = std::env::var_os(HEALTH) else {
        return;
    };
    if let Ok(package) = Package::locate() {
        if let Ok(journal) = package.read_journal() {
            if nonce == OsStr::new(&journal.nonce)
                && journal.version == CURRENT
                && matches!(package.exchanged(&journal), Ok(true))
            {
                let ready = package.stage.join("ready");
                let _ = OpenOptions::new()
                    .write(true)
                    .create_new(true)
                    .mode(0o600)
                    .open(ready)
                    .and_then(|mut file| {
                        file.write_all(journal.nonce.as_bytes())?;
                        file.sync_all()?;
                        package.sync()
                    });
            }
        }
    }
}

pub fn recover_interrupted() -> Result<bool> {
    recover().map_err(|e| format!("Falha ao recuperar a atualização Linux: {e}"))
}

fn recover() -> io::Result<bool> {
    if !cfg!(mk8_portable) || std::env::var_os(HEALTH).is_some() {
        return Ok(false);
    }
    let Ok(package) = Package::locate() else {
        return Ok(false);
    };
    // Read-only installs work normally when there is no interrupted replacement.
    if !package.stage.exists() {
        return Ok(false);
    }
    let Some(_lock) = package.lock()? else {
        return Ok(false);
    };
    reconcile(&package)
}

fn reconcile(package: &Package) -> io::Result<bool> {
    let journal = match package.read_journal() {
        Ok(journal) => journal,
        Err(e) if e.kind() == io::ErrorKind::NotFound => {
            // The journal is written durably before any exchange.
            fs::remove_dir_all(&package.stage)?;
            return Ok(false);
        }
        Err(e) => return Err(e),
    };
    if journal.format != package.format {
        return Err(invalid("Formato de atualização incompatível."));
    }
    let rollback = package.exchanged(&journal)? && !package.healthy(&journal);
    if rollback {
        package.exchange()?;
        package.sync()?;
        package.launch(&journal, None)?;
    }
    package.clear_stage()?;
    Ok(rollback)
}

#[cfg(test)]
mod tests {
    use super::*;
    use flate2::{write::GzEncoder, Compression};

    struct Fixture {
        root: PathBuf,
        package: Package,
    }

    impl Fixture {
        fn new() -> Self {
            let id: String = mk8_transport::random::<8>()
                .iter()
                .map(|b| format!("{b:02x}"))
                .collect();
            let root = std::env::temp_dir().join(format!("mk8-update-{id}"));
            fs::create_dir(&root).unwrap();
            let package = Package::new(root.join("app folder"), Format::Tar).unwrap();
            DirBuilder::new()
                .mode(0o700)
                .create(&package.stage)
                .unwrap();
            Self { root, package }
        }

        fn journal(&self, script: &str) -> Journal {
            let old = &self.package.target;
            let new = self.package.payload();
            for (path, script) in [(old, r#"printf '%s' "$*" > restarted"#), (&new, script)] {
                fs::create_dir(path).unwrap();
                let app_run = path.join("AppRun");
                fs::write(&app_run, format!("#!/bin/sh\n{script}\n")).unwrap();
                fs::set_permissions(&app_run, fs::Permissions::from_mode(0o755)).unwrap();
                fs::write(path.join(MARKER), br#"{"entries":["AppRun"]}"#).unwrap();
            }
            let journal = Journal {
                before: Identity::read(old).unwrap(),
                after: Identity::read(&new).unwrap(),
                version: "0.4.0".into(),
                nonce: "test-marker".into(),
                data: self.root.clone(),
                format: Format::Tar,
                headless: true,
                port: 31742,
            };
            let file = File::create(self.package.stage.join("journal.json")).unwrap();
            serde_json::to_writer(file, &journal).unwrap();
            journal
        }

        fn verify_rollback(&self, journal: &Journal) {
            assert!(!self.package.exchanged(journal).unwrap());
            assert!(!self.package.stage.exists());
            let restarted = self.root.join("restarted");
            let deadline = Instant::now() + Duration::from_secs(3);
            let expected = format!("--data-dir {} --port 31742 --headless", self.root.display());
            while fs::read_to_string(&restarted).ok().as_deref() != Some(expected.as_str()) {
                assert!(Instant::now() < deadline, "old launcher did not restart");
                std::thread::sleep(Duration::from_millis(20));
            }
        }
    }

    impl Drop for Fixture {
        fn drop(&mut self) {
            fs::remove_dir_all(&self.root).unwrap();
        }
    }

    #[test]
    fn verified_update_keeps_new_package_after_ready_signal() {
        let fixture = Fixture::new();
        let journal =
            fixture.journal(r#"[ "$3 $4 $5" = "--port 31742 --headless" ] || exit 9; printf '%s' "$MK8_UPDATE_HEALTH" > '.app folder.mk8-update/ready'"#);
        fs::write(fixture.root.join("save"), "private").unwrap();
        activate(&fixture.package, &journal).unwrap();
        assert!(Identity::read(&fixture.package.target).unwrap() == journal.after);
        assert!(!fixture.package.stage.exists());
        assert_eq!(
            fs::read_to_string(fixture.root.join("save")).unwrap(),
            "private"
        );
    }

    #[test]
    fn legacy_journals_default_to_gui_with_an_automatic_port() {
        let fixture = Fixture::new();
        let mut journal = serde_json::to_value(fixture.journal("exit 0")).unwrap();
        let fields = journal.as_object_mut().unwrap();
        fields.remove("headless");
        fields.remove("port");
        let restored: Journal = serde_json::from_value(journal).unwrap();
        assert!(!restored.headless);
        assert_eq!(restored.port, 0);
    }

    #[test]
    fn failed_and_interrupted_replacements_restore_previous_package() {
        let failed = Fixture::new();
        let journal = failed.journal("exit 9");
        assert!(activate(&failed.package, &journal)
            .unwrap_err()
            .to_string()
            .contains("fechou"));
        failed.verify_rollback(&journal);

        let interrupted = Fixture::new();
        let journal = interrupted.journal("exit 9");
        interrupted.package.exchange().unwrap();
        assert!(interrupted.package.exchanged(&journal).unwrap());
        assert!(reconcile(&interrupted.package).unwrap());
        interrupted.verify_rollback(&journal);
    }

    fn build_tar(path: &Path, contents: &[(&str, &[u8], u32)], link: bool) {
        let out = GzEncoder::new(File::create(path).unwrap(), Compression::fast());
        let mut archive = tar::Builder::new(out);
        for (name, bytes, mode) in contents {
            let mut header = tar::Header::new_gnu();
            header.set_mode(*mode);
            header.set_size(bytes.len() as u64);
            archive.append_data(&mut header, name, *bytes).unwrap();
        }
        if link {
            let mut header = tar::Header::new_gnu();
            header.set_entry_type(tar::EntryType::Symlink);
            header.set_mode(0o777);
            header.set_size(0);
            header.set_link_name("AppRun").unwrap();
            archive
                .append_data(&mut header, "MK8-online/unmanaged-link", io::empty())
                .unwrap();
        }
        archive.into_inner().unwrap().finish().unwrap();
    }

    #[test]
    fn extracts_package_root_and_rejects_non_package_paths_and_links() {
        let fixture = Fixture::new();
        let archive = fixture.root.join("update.tar.gz");
        let contents: &[(&str, &[u8], u32)] = &[
            ("MK8-online/AppRun", b"#!/bin/sh\n", 0o755),
            (
                "MK8-online/.mk8-package.json",
                br#"{"format":"tar.gz","arch":"x86_64","version":"0.4.0","entries":["AppRun"]}"#,
                0o644,
            ),
        ];
        build_tar(&archive, contents, false);
        let payload = fixture.package.payload();
        extract_tar(&archive, &payload).unwrap();
        Format::Tar.validate(&payload, Some("0.4.0")).unwrap();
        validate_inventory(&payload).unwrap();
        assert!(Format::Tar.validate(&payload, Some("0.3.0")).is_err());
        fs::write(payload.join("private-save"), b"private").unwrap();
        assert!(validate_inventory(&payload).is_err());
        assert_eq!(fs::read(payload.join("private-save")).unwrap(), b"private");

        build_tar(&archive, &[("Other/AppRun", b"", 0o755)], false);
        assert!(extract_tar(&archive, &fixture.root.join("outside")).is_err());

        build_tar(&archive, contents, true);
        let links = fixture.root.join("links");
        assert!(extract_tar(&archive, &links).is_err());
        assert!(!links.join("unmanaged-link").exists());
    }

    #[test]
    fn appimage_requires_type_two_elf_x86_64_header() {
        let fixture = Fixture::new();
        let app = fixture.package.payload();
        let mut header = [0; 20];
        header[..6].copy_from_slice(b"\x7fELF\x02\x01");
        header[8..11].copy_from_slice(b"AI\x02");
        header[18..20].copy_from_slice(&62u16.to_le_bytes());
        fs::write(&app, header).unwrap();
        Format::AppImage.validate(&app, None).unwrap();

        header[18..20].copy_from_slice(&183u16.to_le_bytes());
        fs::write(&app, header).unwrap();
        assert!(Format::AppImage.validate(&app, None).is_err());
    }
}
