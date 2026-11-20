//! FSR page: launcher settings, the environment handed to the game, and the
//! terms-gated AMD FidelityFX SDK download/build flow.
//!
//! The launcher stores only user choices and never ships, embeds or mirrors AMD
//! binaries or SPIR-V. On Linux the provider is built locally by
//! `tools/build_fg_provider.py` from the pinned, hash-verified SDK source
//! archive (see `install.rs` for the exact interface contract). On Windows the
//! same terms-gated step downloads AMD's signed Vulkan DLLs from the same
//! pinned tag, verifies each pinned size and SHA-256, and points the runtime at
//! them; nothing is compiled there.

use mk8_manifest::Result;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    ffi::OsString,
    fs,
    io::{Read, Write},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
};

pub mod install;

/// Launcher settings live here, beside the other per-user launcher JSON files.
pub const SETTINGS_FILE: &str = "launcher-settings.json";
/// Optional explicit data directory for the FSR results. The default is the
/// documented `$XDG_DATA_HOME/mk8online/fsr` (or the platform equivalent).
pub const DATA_DIR_OVERRIDE: &str = "MK8_FSR_DIR";

const FSR_OFF: &str = "off";
const FSR_316: &str = "3.1.6";
const FSR_412: &str = "4.1.2";
const FG_OFF: &str = "off";
const FG_FSR3: &str = "fsr3";

fn default_upscaling() -> String {
    FSR_OFF.into()
}

fn default_frame_generation() -> String {
    FG_OFF.into()
}

/// What the FSR page persists in `launcher-settings.json` under `"fsr"`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
pub struct Settings {
    #[serde(default = "default_upscaling")]
    pub upscaling: String,
    /// Provider `.so` the runtime loads through `MK8_FSR_LIB`. Empty keeps the
    /// runtime default; the launcher never supplies a bundled provider.
    #[serde(default)]
    pub provider_lib: String,
    #[serde(default = "default_frame_generation")]
    pub frame_generation: String,
    /// DXC used by the local provider build. Empty asks the launcher to find it.
    #[serde(default)]
    pub dxc: String,
    /// The GPL 4.1.2 port was built against the deprecated 1.x backend
    /// descriptor; the 2.4 SDK provider uses the current one.
    #[serde(default)]
    pub legacy_abi: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            upscaling: default_upscaling(),
            provider_lib: String::new(),
            frame_generation: default_frame_generation(),
            dxc: String::new(),
            legacy_abi: false,
        }
    }
}

impl Settings {
    pub fn validate(&self) -> Result<()> {
        match self.upscaling.as_str() {
            FSR_OFF | FSR_316 | FSR_412 => (),
            value => return Err(format!("Versão de ampliação FSR inválida: {value}.")),
        }
        match self.frame_generation.as_str() {
            FG_OFF | FG_FSR3 => (),
            "fsr4" => {
                return Err(
                    "FSR 4 MLFI ainda não está disponível: faltam os modelos do provedor.".into(),
                )
            }
            value => return Err(format!("Geração de quadros FSR inválida: {value}.")),
        }
        if self.provider_lib.chars().any(char::is_control) {
            return Err("Caminho do provedor FSR inválido.".into());
        }
        if self.dxc.chars().any(char::is_control) {
            return Err("Caminho do DXC inválido.".into());
        }
        Ok(())
    }
}

pub fn settings_path(local: &Path) -> PathBuf {
    local.join(SETTINGS_FILE)
}

pub fn read_settings(local: &Path) -> Result<Settings> {
    let path = settings_path(local);
    let text = match fs::read_to_string(&path) {
        Ok(text) => text,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return Ok(Settings::default())
        }
        Err(error) => return Err(format!("{}: {error}", path.display())),
    };
    let value: Value = serde_json::from_str(&text)
        .map_err(|error| format!("{}: JSON inválido ({error})", path.display()))?;
    let fsr = value.get("fsr").cloned().unwrap_or_else(|| json!({}));
    let settings: Settings = serde_json::from_value(fsr)
        .map_err(|error| format!("{}: configurações FSR inválidas ({error})", path.display()))?;
    settings.validate()?;
    Ok(settings)
}

/// Writes only the `"fsr"` object, preserving fields other launcher versions
/// may have stored in the same file.
pub fn write_settings(local: &Path, settings: &Settings) -> Result<()> {
    settings.validate()?;
    let path = settings_path(local);
    let mut root: Value = match fs::read_to_string(&path) {
        Ok(text) => serde_json::from_str(&text)
            .map_err(|error| format!("{}: JSON inválido ({error})", path.display()))?,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => json!({}),
        Err(error) => return Err(format!("{}: {error}", path.display())),
    };
    if !root.is_object() {
        return Err(format!(
            "{}: o arquivo de configurações do launcher não é um objeto JSON.",
            path.display()
        ));
    }
    root["fsr"] = serde_json::to_value(settings).map_err(|error| error.to_string())?;
    let bytes = serde_json::to_vec_pretty(&root).map_err(|error| error.to_string())?;
    write_atomic(&path, &bytes)
}

static TEMP: AtomicU64 = AtomicU64::new(0);

pub(crate) fn write_atomic(path: &Path, bytes: &[u8]) -> Result<()> {
    let parent = path
        .parent()
        .ok_or_else(|| format!("Caminho sem pasta: {}", path.display()))?;
    fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default();
    let temp = parent.join(format!(
        ".{}.{}.{}.{stamp}.tmp",
        path.file_name()
            .and_then(|name| name.to_str())
            .unwrap_or("fsr"),
        std::process::id(),
        TEMP.fetch_add(1, Ordering::Relaxed)
    ));
    let mut file = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temp)
        .map_err(|error| format!("{}: {error}", temp.display()))?;
    let result = file.write_all(bytes).and_then(|_| file.sync_all());
    drop(file);
    if let Err(error) = result {
        let _ = fs::remove_file(&temp);
        return Err(format!("{}: {error}", temp.display()));
    }
    fs::rename(&temp, path).map_err(|error| {
        let _ = fs::remove_file(&temp);
        format!("{}: {error}", path.display())
    })
}

pub(crate) fn sha256_file(path: &Path) -> Result<String> {
    let mut file = fs::File::open(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut hash = Sha256::new();
    let mut buffer = [0u8; 64 * 1024];
    loop {
        let read = file
            .read(&mut buffer)
            .map_err(|error| format!("{}: {error}", path.display()))?;
        if read == 0 {
            break;
        }
        hash.update(&buffer[..read]);
    }
    Ok(format!("{:x}", hash.finalize()))
}

pub(crate) fn sha256_bytes(bytes: &[u8]) -> String {
    let mut hash = Sha256::new();
    hash.update(bytes);
    format!("{:x}", hash.finalize())
}

/// `$XDG_DATA_HOME/mk8online/fsr`, or the platform equivalent. The override is
/// only for tests and the design preview; normal installs use the documented
/// XDG location.
pub fn data_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os(DATA_DIR_OVERRIDE).filter(|value| !value.is_empty()) {
        return PathBuf::from(dir);
    }
    if let Some(dir) = std::env::var_os("XDG_DATA_HOME").filter(|value| !value.is_empty()) {
        return PathBuf::from(dir).join("mk8online").join("fsr");
    }
    #[cfg(windows)]
    if let Some(dir) = std::env::var_os("LOCALAPPDATA").filter(|value| !value.is_empty()) {
        return PathBuf::from(dir).join("mk8online").join("fsr");
    }
    if let Some(home) = std::env::var_os("HOME").filter(|value| !value.is_empty()) {
        return PathBuf::from(home)
            .join(".local")
            .join("share")
            .join("mk8online")
            .join("fsr");
    }
    std::env::temp_dir().join("mk8online").join("fsr")
}

pub fn provider_path(dir: &Path) -> PathBuf {
    framegeneration_lib(dir)
}

/// The runtime's upscaling library: AMD's signed DLL on Windows, or a
/// user-supplied provider on Linux (the launcher never supplies one).
pub fn upscaler_lib(dir: &Path) -> PathBuf {
    upscaler_lib_for(dir, cfg!(windows))
}

fn upscaler_lib_for(dir: &Path, windows: bool) -> PathBuf {
    if windows {
        dir.join("amd_fidelityfx_upscaler_vk.dll")
    } else {
        dir.join("provider/libamd_fidelityfx_upscaler_vk.so")
    }
}

/// The runtime's frame-generation library. Linux builds it locally from the
/// pinned SDK; Windows uses AMD's signed DLL with the same pinned hash.
pub fn framegeneration_lib(dir: &Path) -> PathBuf {
    framegeneration_lib_for(dir, cfg!(windows))
}

fn framegeneration_lib_for(dir: &Path, windows: bool) -> PathBuf {
    if windows {
        dir.join("amd_fidelityfx_framegeneration_vk.dll")
    } else {
        dir.join(install::PROVIDER_REL)
    }
}

pub fn license_path(dir: &Path) -> PathBuf {
    dir.join(install::LICENSE_FILE)
}

/// Environment changes for the game process. Kept pure so the mapping can be
/// tested without mutating the test process.
pub struct Environment {
    pub set: Vec<(&'static str, OsString)>,
    pub remove: Vec<&'static str>,
}

impl Environment {
    pub fn apply(&self) {
        for key in &self.remove {
            std::env::remove_var(key);
        }
        for (key, value) in &self.set {
            std::env::set_var(key, value);
        }
    }
}

pub fn environment(settings: &Settings) -> Environment {
    environment_for(settings, &data_dir(), cfg!(windows))
}

fn environment_for(settings: &Settings, dir: &Path, windows: bool) -> Environment {
    let mut result = Environment {
        set: Vec::new(),
        remove: vec![
            "MK8_FSR4",
            "MK8_FSR_VERSION",
            "MK8_FSR_LIB",
            "MK8_FSR_LEGACY_ABI",
            "MK8_FSR_FG",
            "MK8_FSR_FG_LIB",
        ],
    };
    match settings.upscaling.as_str() {
        FSR_316 | FSR_412 => {
            result.set.push(("MK8_FSR4", "1".into()));
            result
                .set
                .push(("MK8_FSR_VERSION", settings.upscaling.clone().into()));
            let library = if !settings.provider_lib.is_empty() {
                PathBuf::from(&settings.provider_lib)
            } else if windows {
                // On Windows the terms-gated step installs AMD's signed
                // upscaler beside the data; the runtime is pointed at it.
                upscaler_lib_for(dir, windows)
            } else {
                // Linux has no bundled upscaler provider: keep the runtime
                // default unless the user chose a library.
                PathBuf::new()
            };
            if !library.as_os_str().is_empty() {
                result.set.push(("MK8_FSR_LIB", library.into()));
            }
            if settings.legacy_abi {
                result.set.push(("MK8_FSR_LEGACY_ABI", "1".into()));
            }
        }
        _ => (),
    }
    if settings.frame_generation == FG_FSR3 {
        result.set.push(("MK8_FSR_FG", "fsr3".into()));
        result.set.push((
            "MK8_FSR_FG_LIB",
            framegeneration_lib_for(dir, windows).into(),
        ));
    }
    result
}

/// Called in the game supervisor before `mk8_installer::launch`. Removes stale
/// values first, so a diagnostic shell cannot leak FSR switches into a normal
/// launch.
pub fn configure(local: &Path) -> Result<()> {
    let settings = read_settings(local)?;
    environment(&settings).apply();
    Ok(())
}

/// Reads the AMD license text that was already fetched and verified. The page
/// calls `install::fetch_license` to populate it.
/// True only when the saved text is byte-for-byte the pinned AMD license.
pub fn license_verified(dir: &Path) -> bool {
    let path = license_path(dir);
    fs::metadata(&path).is_ok_and(|meta| meta.len() == install::LICENSE_BYTES)
        && sha256_file(&path).is_ok_and(|hash| hash == install::LICENSE_SHA256)
}

pub fn license_status(dir: &Path) -> Value {
    let path = license_path(dir);
    let present = path.is_file();
    let bytes = fs::metadata(&path).map(|meta| meta.len()).unwrap_or(0);
    let verified = license_verified(dir);
    json!({
        "present": present,
        "verified": verified,
        "accepted": terms_accepted(dir),
        "bytes": bytes,
        "expected_bytes": install::LICENSE_BYTES,
        "sha256": install::LICENSE_SHA256,
        "path": path,
    })
}

pub fn terms_accepted(dir: &Path) -> bool {
    let Ok(text) = fs::read_to_string(dir.join(install::TERMS_FILE)) else {
        return false;
    };
    let Ok(value) = serde_json::from_str::<Value>(&text) else {
        return false;
    };
    value["sdk"] == install::SDK_TAG
        && value["license_sha256"] == install::LICENSE_SHA256
        && license_verified(dir)
}

/// Everything the FSR page needs. `running` comes from the launcher's child job.
pub fn page(root: &Path, local: &Path, running: bool) -> Result<Value> {
    let settings = read_settings(local)?;
    let dir = data_dir();
    let mut value = install::status(&dir, &settings, root, running);
    value["settings"] = serde_json::to_value(&settings).map_err(|error| error.to_string())?;
    value["license"] = license_status(&dir);
    value["data_dir"] = json!(dir);
    value["provider_expected"] = json!(provider_path(&dir));
    value["upscaler_expected"] = json!(upscaler_lib(&data_dir()));
    value["platform"] = json!(if cfg!(windows) { "windows" } else { "linux" });
    value["running"] = json!(running);
    Ok(value)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn temp(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "mk8-fsr-{name}-{}-{}",
            std::process::id(),
            TEMP.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn settings_roundtrip_preserves_other_launcher_fields_and_rejects_invalid_values() {
        let local = temp("settings");
        fs::write(
            settings_path(&local),
            r#"{"future":{"keep":true},"fsr":{"upscaling":"off","frame_generation":"off"}}"#,
        )
        .unwrap();
        let defaults = read_settings(&local).unwrap();
        assert_eq!(defaults, Settings::default());

        let settings = Settings {
            upscaling: FSR_316.into(),
            provider_lib: "/tmp/provider.so".into(),
            frame_generation: FG_FSR3.into(),
            dxc: "/tmp/dxc".into(),
            legacy_abi: true,
        };
        write_settings(&local, &settings).unwrap();
        assert_eq!(read_settings(&local).unwrap(), settings);
        let raw: Value =
            serde_json::from_str(&fs::read_to_string(settings_path(&local)).unwrap()).unwrap();
        assert_eq!(raw["future"]["keep"], true);
        assert_eq!(raw["fsr"]["upscaling"], FSR_316);

        let before = fs::read(settings_path(&local)).unwrap();
        for invalid in [
            Settings {
                upscaling: "5.0".into(),
                ..settings.clone()
            },
            Settings {
                frame_generation: "fsr4".into(),
                ..settings.clone()
            },
            Settings {
                frame_generation: "mg".into(),
                ..settings.clone()
            },
            Settings {
                provider_lib: "bad\npath".into(),
                ..settings.clone()
            },
        ] {
            assert!(write_settings(&local, &invalid).is_err(), "{invalid:?}");
            assert_eq!(fs::read(settings_path(&local)).unwrap(), before);
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn missing_settings_file_reads_defaults_without_creating_it() {
        let local = temp("settings-missing");
        assert_eq!(read_settings(&local).unwrap(), Settings::default());
        assert!(!settings_path(&local).exists());
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn environment_maps_upscaling_and_frame_generation_to_the_runtime_contract() {
        let off = environment(&Settings::default());
        assert!(off.set.is_empty());
        assert!(off.remove.contains(&"MK8_FSR4"));
        assert!(off.remove.contains(&"MK8_FSR_VERSION"));
        assert!(off.remove.contains(&"MK8_FSR_LIB"));
        assert!(off.remove.contains(&"MK8_FSR_FG"));
        assert!(off.remove.contains(&"MK8_FSR_FG_LIB"));

        let settings = Settings {
            upscaling: FSR_412.into(),
            provider_lib: "/opt/fsr/lib.so".into(),
            frame_generation: FG_FSR3.into(),
            dxc: String::new(),
            legacy_abi: true,
        };
        let value_of = |env: &Environment, key: &str| {
            env.set
                .iter()
                .find(|(name, _)| *name == key)
                .map(|(_, value)| value.to_string_lossy().into_owned())
        };
        let env = environment_for(&settings, Path::new("/data/fsr"), false);
        assert_eq!(value_of(&env, "MK8_FSR4").as_deref(), Some("1"));
        assert_eq!(value_of(&env, "MK8_FSR_VERSION").as_deref(), Some(FSR_412));
        assert_eq!(
            value_of(&env, "MK8_FSR_LIB").as_deref(),
            Some("/opt/fsr/lib.so")
        );
        assert_eq!(value_of(&env, "MK8_FSR_LEGACY_ABI").as_deref(), Some("1"));
        assert_eq!(value_of(&env, "MK8_FSR_FG").as_deref(), Some("fsr3"));
        assert!(value_of(&env, "MK8_FSR_FG_LIB")
            .unwrap()
            .ends_with("provider/libamd_fidelityfx_framegeneration_vk.so"));

        // Windows points at AMD's signed DLLs; no local provider is invented.
        let windows = environment_for(
            &Settings {
                provider_lib: String::new(),
                ..settings
            },
            Path::new("/data/fsr"),
            true,
        );
        assert!(value_of(&windows, "MK8_FSR_LIB")
            .unwrap()
            .ends_with("amd_fidelityfx_upscaler_vk.dll"));
        assert!(value_of(&windows, "MK8_FSR_FG_LIB")
            .unwrap()
            .ends_with("amd_fidelityfx_framegeneration_vk.dll"));
    }

    #[test]
    fn launcher_tree_contains_no_amd_binaries_assets_or_embeds() {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        fn walk(dir: &Path, bad: &mut Vec<PathBuf>) {
            let Ok(entries) = fs::read_dir(dir) else {
                return;
            };
            for entry in entries.flatten() {
                let path = entry.path();
                let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
                if path.is_dir() {
                    if !matches!(name.as_str(), "target" | ".git" | "node_modules") {
                        walk(&path, bad);
                    }
                    continue;
                }
                if name.starts_with("libamd_fidelityfx_") || name.starts_with("fsr4-assets") {
                    bad.push(path.clone());
                }
                if name.ends_with(".rs") {
                    if let Ok(source) = fs::read_to_string(&path) {
                        for line in source.lines() {
                            let lower = line.to_ascii_lowercase();
                            if (lower.contains("include_bytes!") || lower.contains("include_str!"))
                                && (lower.contains("libamd_fidelityfx_")
                                    || lower.contains("fsr4-assets"))
                            {
                                bad.push(path.clone());
                                break;
                            }
                        }
                    }
                }
            }
        }
        let mut bad = Vec::new();
        walk(root, &mut bad);
        assert!(
            bad.is_empty(),
            "AMD binaries/assets must never be part of the launcher: {bad:?}"
        );
    }
}
