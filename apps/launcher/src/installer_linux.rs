use std::path::Path;
#[cfg(mk8_portable)]
use std::{fs, path::PathBuf};

pub fn free_gib(path: &Path) -> Option<f64> {
    use std::{ffi::CString, os::unix::ffi::OsStrExt};
    let existing = path.ancestors().find(|p| p.is_dir())?;
    let path = CString::new(existing.as_os_str().as_bytes()).ok()?;
    let mut info = std::mem::MaybeUninit::<libc::statvfs>::uninit();
    if unsafe { libc::statvfs(path.as_ptr(), info.as_mut_ptr()) } != 0 {
        return None;
    }
    let info = unsafe { info.assume_init() };
    Some(info.f_bavail as f64 * info.f_frsize as f64 / (1u64 << 30) as f64)
}

#[cfg(mk8_portable)]
fn default_data_root() -> Result<PathBuf, crate::Error> {
    let base = std::env::var_os("XDG_DATA_HOME")
        .map(PathBuf::from)
        .filter(|p| p.is_absolute())
        .or_else(|| std::env::var_os("HOME").map(|p| PathBuf::from(p).join(".local/share")))
        .ok_or("XDG_DATA_HOME e HOME indisponíveis")?;
    Ok(base.join("MK8-online"))
}

#[cfg(mk8_portable)]
fn metadata_file() -> Result<PathBuf, crate::Error> {
    Ok(default_data_root()?.join("installation.json"))
}

#[cfg(mk8_portable)]
fn installed() -> Option<serde_json::Value> {
    serde_json::from_slice(&fs::read(metadata_file().ok()?).ok()?).ok()
}

#[cfg(mk8_portable)]
pub fn data_root() -> Result<PathBuf, crate::Error> {
    if let Some(data) = installed()
        .and_then(|state| state["data"].as_str().map(PathBuf::from))
        .filter(|path| path.is_absolute())
    {
        return Ok(data);
    }
    default_data_root()
}

#[cfg(mk8_portable)]
pub fn project_root() -> Result<std::path::PathBuf, crate::Error> {
    let base = default_data_root()?;
    let root = base
        .join("versions")
        .join(&env!("MK8_LAUNCHER_PAYLOAD_ID")[..16]);
    if root.is_dir() {
        return Ok(root);
    }
    std::fs::create_dir_all(root.parent().ok_or("Pasta das versões indisponível")?)?;
    let stage = root.with_extension(std::process::id().to_string());
    std::fs::create_dir(&stage)?;
    let archive = stage.join("payload.zip");
    std::fs::write(&archive, include_bytes!(env!("MK8_LAUNCHER_PAYLOAD")))?;
    let output = std::process::Command::new("bsdtar")
        .args(["-xf"])
        .arg(&archive)
        .arg("-C")
        .arg(&stage)
        .output()?;
    std::fs::remove_file(&archive)?;
    if !output.status.success() {
        return Err(format!(
            "Falha ao preparar o launcher: {}",
            String::from_utf8_lossy(&output.stderr)
        )
        .into());
    }
    crate::publish_payload(&stage, &root, std::time::Duration::from_secs(15))?;
    Ok(root)
}

#[cfg(mk8_portable)]
mod package {
    use super::{data_root, default_data_root, installed, metadata_file, project_root};
    use crate::Error;
    use serde_json::json;
    use std::{
        fs,
        path::{Path, PathBuf},
        sync::{Mutex, OnceLock},
    };

    static ACTIVE_COPY: OnceLock<(PathBuf, bool)> = OnceLock::new();

    pub fn installed_target_for_process() -> Option<(&'static Path, bool)> {
        ACTIVE_COPY
            .get()
            .map(|(path, image)| (path.as_path(), *image))
    }

    pub fn initialize() -> Result<(), Error> {
        let tools = default_data_root()?.join("native-tools");
        fs::create_dir_all(&tools)?;
        std::env::set_var("MK8_NATIVE_TOOLS", tools);
        Ok(())
    }

    pub const NEEDED_GIB: f64 = 30.0;

    pub struct Options {
        pub app: PathBuf,
        pub data: PathBuf,
        pub desktop: bool,
    }

    pub struct Previous {
        pub app: PathBuf,
        pub data: PathBuf,
        pub desktop: bool,
        pub found: bool,
    }

    #[derive(Clone, Default)]
    pub struct Progress {
        pub percent: f64,
        pub step: String,
        pub state: String,
        pub error: Option<String>,
    }

    pub fn launcher_data(base: &Path) -> PathBuf {
        base.to_path_buf()
    }

    fn application() -> Option<(PathBuf, bool)> {
        if let Some(path) = std::env::var_os("APPIMAGE")
            .map(PathBuf::from)
            .filter(|p| p.is_file())
        {
            return Some((path, true));
        }
        let root = std::env::var_os("MK8_PACKAGE_ROOT").map(PathBuf::from)?;
        let entry = root.join("AppRun");
        (entry.is_file() && root.join(".mk8-package.json").is_file()).then_some((entry, false))
    }

    pub fn should_setup(args: &[String]) -> bool {
        application().is_some()
            && (args.iter().any(|arg| arg == "--install")
                || (installed().is_none() && crate::argument(args, "--data-dir").is_none()))
    }

    impl Previous {
        pub fn find() -> Self {
            let previous = installed();
            let package = application();
            let app = package
                .and_then(|(entry, _)| entry.parent().map(Path::to_path_buf))
                .unwrap_or_default();
            Self {
                app,
                data: data_root().unwrap_or_default(),
                desktop: previous
                    .as_ref()
                    .and_then(|v| v["desktop"].as_bool())
                    .unwrap_or(true),
                found: previous.is_some(),
            }
        }
    }

    pub fn hard_disk(_: &Path) -> Option<bool> {
        None
    }

    fn desktop_entry(entry: &Path) -> Result<String, Error> {
        let path = entry
            .to_str()
            .ok_or("O caminho do programa deve ser UTF-8.")?;
        if path.chars().any(char::is_control) {
            return Err("Caminho do programa inválido.".into());
        }
        let mut quoted = String::new();
        for c in path.chars() {
            match c {
                '%' => quoted.push_str("%%"),
                '\\' => quoted.push_str(r"\\\\"),
                '"' | '`' | '$' => {
                    quoted.push_str(r"\\");
                    quoted.push(c);
                }
                _ => quoted.push(c),
            }
        }
        Ok(format!("[Desktop Entry]\nType=Application\nName=MK8-online\nExec=\"{quoted}\"\nCategories=Game;\nTerminal=false\n"))
    }

    fn copy_package(
        source: &Path,
        entry: &Path,
        app: &Path,
        target: &Path,
        image: bool,
    ) -> Result<(), Error> {
        let parent = if image {
            app
        } else {
            app.parent().ok_or("Pasta do programa indisponível.")?
        };
        if super::free_gib(parent).is_none_or(|free| free < 4.25) {
            return Err("A cópia do programa precisa preservar a reserva de 4 GiB.".into());
        }
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)?
            .as_nanos();
        let stage = parent.join(format!(".mk8-install-{}-{stamp}", std::process::id()));
        if image {
            if target.exists()
                && mk8_manifest::hash_file(target)?.sha256 == mk8_manifest::hash_file(entry)?.sha256
            {
                return Ok(());
            }
            let mut file = fs::OpenOptions::new()
                .write(true)
                .create_new(true)
                .open(&stage)?;
            let published = (|| -> Result<(), Error> {
                std::io::copy(&mut fs::File::open(entry)?, &mut file)?;
                file.set_permissions(fs::metadata(entry)?.permissions())?;
                file.sync_all()?;
                fs::hard_link(&stage, target)?;
                Ok(())
            })();
            let _ = fs::remove_file(stage);
            return published;
        }
        if app.starts_with(source) || fs::read_dir(app)?.next().is_some() {
            return Err("Escolha uma pasta vazia fora da origem do pacote.".into());
        }
        fs::create_dir(&stage)?;
        let published = (|| -> Result<(), Error> {
            let status = std::process::Command::new("cp")
                .arg("-a")
                .arg("--")
                .arg(source.join("."))
                .arg(&stage)
                .status()?;
            if !status.success() {
                return Err("A cópia do programa falhou.".into());
            }
            fs::rename(&stage, app)?;
            Ok(())
        })();
        if published.is_err() {
            let _ = fs::remove_dir_all(stage);
        }
        published
    }

    fn install_steps(options: &Options, progress: &Mutex<Progress>) -> Result<(), Error> {
        let (entry, image) = application().ok_or("Pacote Linux indisponível.")?;
        let source = entry
            .parent()
            .ok_or("Pasta do programa indisponível.")?
            .canonicalize()?;
        fs::create_dir_all(&options.app)?;
        let app = options.app.canonicalize()?;
        fs::create_dir_all(&options.data)?;
        let data = options.data.canonicalize()?;
        if !image && data.starts_with(&app) {
            return Err("Escolha uma pasta de dados fora da pasta do programa.".into());
        }
        let target = app.join(entry.file_name().ok_or("Nome do programa indisponível.")?);
        project_root()?;
        if app != source {
            {
                let mut state = progress.lock().unwrap();
                state.percent = 45.0;
                state.step = "Copiar o MK8-online".to_owned();
            }
            copy_package(&source, &entry, &app, &target, image)?;
        }
        let entry_text = desktop_entry(&target)?;
        let folder = default_data_root()?
            .parent()
            .ok_or("Pasta XDG indisponível.")?
            .join("applications");
        let desktop = folder.join("io.github.mk8online.launcher.desktop");
        if options.desktop {
            fs::create_dir_all(&folder)?;
            fs::write(&desktop, entry_text)?;
        } else if desktop.exists() {
            fs::remove_file(&desktop)?;
        }
        mk8_manifest::save(
            &metadata_file()?,
            &json!({"data": data, "desktop": options.desktop}),
        )?;
        let _ = ACTIVE_COPY.set((if image { target } else { app }, image));
        Ok(())
    }

    pub fn install(options: &Options, progress: &Mutex<Progress>) -> Result<(), Error> {
        let result = install_steps(options, progress);
        let mut state = progress.lock().unwrap();
        match &result {
            Ok(()) => {
                state.percent = 100.0;
                state.step = "Tudo pronto".into();
                state.state = "done".into();
            }
            Err(error) => {
                state.state = "failed".into();
                state.error = Some(error.to_string());
            }
        }
        result
    }
}

#[cfg(mk8_portable)]
pub use package::*;
