//! FSR page: launcher settings, the environment handed to the game, and the
//! terms-gated AMD FidelityFX SDK download/build flow.
//!
//! The launcher stores only user choices and never ships, embeds or mirrors AMD
//! binaries or SPIR-V. On Linux the FSR 3.1.6 upscaler and the FSR 3
//! frame-generation provider are built locally by
//! `tools/build_upscaler_provider.py` / `tools/build_fsr4_provider.py` /
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
/// Optional explicit cache for the pinned DXC release. The default is the
/// documented `$XDG_DATA_HOME/mk8online/dxc`, outside the FSR output.
pub const DXC_DIR_OVERRIDE: &str = "MK8_DXC_DIR";

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

    /// A path saved on Linux is not a usable Windows provider. Keep the file
    /// readable so the page can show the problem and the user can fix it, but
    /// reject it when saving and before starting the game.
    pub fn compatibility_error(&self) -> Option<String> {
        provider_platform_error(&self.provider_lib, cfg!(windows))
    }
}

fn provider_platform_error(provider_lib: &str, windows: bool) -> Option<String> {
    (windows && provider_lib.to_ascii_lowercase().ends_with(".so")).then(|| {
        "O provedor FSR salvo é um arquivo .so de Linux; no Windows escolha uma DLL (.dll) ou deixe o campo vazio para usar a DLL assinada da AMD."
            .into()
    })
}

/// Windows FSR 4.1.2/MLFI availability. AMD's shipped DLLs gate ML upscaling
/// to RDNA 4 and only expose 3.1.6/2.3.4 elsewhere.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Fsr4Gate {
    /// The hardware or the installed DLLs do not offer 4.1.2.
    pub blocked: bool,
    /// A user-supplied provider bypasses the hardware gate.
    pub override_active: bool,
    pub reason_code: &'static str,
    pub reason: String,
    pub gpu: Option<String>,
    pub vendor: Option<String>,
    pub versions: Vec<String>,
    pub framegeneration_versions: Vec<String>,
}

impl Fsr4Gate {
    pub fn allowed(&self) -> bool {
        !self.blocked || self.override_active
    }
}

/// Only the 4.1.2 selection is affected; FSR 3.1.6 remains selectable on
/// NVIDIA/Intel and on AMD hardware older than RDNA 4.
pub fn fsr4_selection_blocked(settings: &Settings, gate: &Fsr4Gate) -> bool {
    settings.upscaling == FSR_412 && !gate.allowed()
}

fn gpu_label(gpu: &crate::vulkan::GpuInfo) -> String {
    let vendor = gpu.vendor();
    let name = gpu.device_name.trim();
    if name
        .to_ascii_lowercase()
        .starts_with(&vendor.to_ascii_lowercase())
    {
        name.to_owned()
    } else {
        format!("{vendor} {name}").trim().to_owned()
    }
}

pub fn windows_fsr4_gate(
    settings: &Settings,
    gpu: Option<&crate::vulkan::GpuInfo>,
    upscaler_versions: &[String],
    framegeneration_versions: &[String],
) -> Fsr4Gate {
    let override_active = !settings.provider_lib.is_empty();
    let vendor = gpu.map(|gpu| gpu.vendor().to_owned());
    let gpu_label = gpu.map(gpu_label);
    let rdna4 = gpu.is_some_and(crate::vulkan::GpuInfo::is_amd_rdna4);
    let (blocked, reason_code, reason) = if gpu.is_none() {
        (
            true,
            "gpu-missing",
            "Não foi possível identificar a GPU Vulkan. FSR 4.1.2 exige AMD RDNA 4; sem identificação, use um provedor manual ou selecione FSR 3.1.6.".to_owned(),
        )
    } else if !rdna4 {
        (
            true,
            "not-rdna4",
            format!(
                "FSR 4.1.2 exige AMD RDNA 4; as DLLs assinadas da AMD expõem FSR 3.1.6 nesta GPU ({}).",
                gpu_label.as_deref().unwrap_or("GPU desconhecida")
            ),
        )
    // The installed DLL's null-device list is informational. The runtime
    // queries the selected live Vulkan device before creating the FFX context.
    } else {
        (false, "ok", String::new())
    };
    Fsr4Gate {
        blocked,
        override_active,
        reason_code,
        reason,
        gpu: gpu_label,
        vendor,
        versions: upscaler_versions.to_vec(),
        framegeneration_versions: framegeneration_versions.to_vec(),
    }
}

/// Capability used by the page and by the game-launch guard. Linux ports are
/// native and are not gated by Windows driver capabilities.
pub fn fsr4_gate(settings: &Settings) -> Fsr4Gate {
    if !cfg!(windows) {
        return Fsr4Gate {
            blocked: false,
            override_active: !settings.provider_lib.is_empty(),
            reason_code: "native",
            reason: "FSR 4.1.2 é o provedor nativo Linux deste projeto.".into(),
            gpu: None,
            vendor: None,
            versions: Vec::new(),
            framegeneration_versions: Vec::new(),
        };
    }
    let dir = data_dir();
    let upscaler = upscaler_for(&dir, FSR_412);
    let framegeneration = framegeneration_lib(&dir);
    let (versions, framegeneration_versions) = signed_dll_versions(&upscaler, &framegeneration);
    let gpu = crate::vulkan::gpu_info();
    windows_fsr4_gate(settings, gpu.as_ref(), &versions, &framegeneration_versions)
}

/// The signed DLLs are tens of megabytes; the page polls status every two
/// seconds. Load them once when they exist, then reuse the version lists.
fn signed_dll_versions(upscaler: &Path, framegeneration: &Path) -> (Vec<String>, Vec<String>) {
    static VERSIONS: std::sync::OnceLock<(Vec<String>, Vec<String>)> = std::sync::OnceLock::new();
    if let Some(cached) = VERSIONS.get() {
        return cached.clone();
    }
    if !upscaler.is_file() && !framegeneration.is_file() {
        return (Vec::new(), Vec::new());
    }
    let versions = install::provider_versions_for(upscaler, 0x0001_0000).unwrap_or_default();
    let framegeneration_versions =
        install::provider_versions_for(framegeneration, 0x0004_0001).unwrap_or_default();
    let value = (versions, framegeneration_versions);
    let _ = VERSIONS.set(value.clone());
    value
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
    if let Some(error) = settings.compatibility_error() {
        return Err(error);
    }
    if cfg!(windows) && settings.upscaling == FSR_412 {
        let gate = fsr4_gate(settings);
        if fsr4_selection_blocked(settings, &gate) {
            return Err(format!(
                "{} Se você tem um provedor AMD RDNA 4, informe o caminho manual.",
                gate.reason
            ));
        }
    }
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
    data_base().join("fsr")
}

/// `$XDG_DATA_HOME/mk8online/dxc`, kept separate from the FSR output because
/// DXC is an independent open-source tool, not an AMD/FSR artifact.
pub fn dxc_dir() -> PathBuf {
    if let Some(dir) = std::env::var_os(DXC_DIR_OVERRIDE).filter(|value| !value.is_empty()) {
        return PathBuf::from(dir);
    }
    data_base().join("dxc")
}

fn data_base() -> PathBuf {
    if let Some(dir) = std::env::var_os("XDG_DATA_HOME").filter(|value| !value.is_empty()) {
        return PathBuf::from(dir).join("mk8online");
    }
    #[cfg(windows)]
    if let Some(dir) = std::env::var_os("LOCALAPPDATA").filter(|value| !value.is_empty()) {
        return PathBuf::from(dir).join("mk8online");
    }
    if let Some(home) = std::env::var_os("HOME").filter(|value| !value.is_empty()) {
        return PathBuf::from(home)
            .join(".local")
            .join("share")
            .join("mk8online");
    }
    std::env::temp_dir().join("mk8online")
}

pub fn provider_path(dir: &Path) -> PathBuf {
    framegeneration_lib(dir)
}

/// The runtime's upscaling library for the selected provider version.
pub fn upscaler_for(dir: &Path, version: &str) -> PathBuf {
    upscaler_for_version(dir, version, cfg!(windows))
}

/// The 3.1.6 upscaler path (or AMD's signed DLL on Windows).
pub fn upscaler_lib(dir: &Path) -> PathBuf {
    upscaler_for_version(dir, FSR_316, cfg!(windows))
}

fn upscaler_for_version(dir: &Path, version: &str, windows: bool) -> PathBuf {
    if windows {
        dir.join("amd_fidelityfx_upscaler_vk.dll")
    } else if version == FSR_412 {
        dir.join(install::FSR4_UPSCALER_REL)
    } else {
        dir.join(install::UPSCALER_REL)
    }
}

/// Runtime assets extracted from AMD's signed DLL for the native 4.1.2 port.
pub fn fsr4_assets_dir(dir: &Path) -> PathBuf {
    dir.join(install::FSR4_ASSETS_REL)
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
    // (Linux launches apply it; Windows uses the verified components in graphics.rs.)
    #[cfg_attr(not(target_os = "linux"), allow(dead_code))]
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
            "FSR4_ASSET_DIR",
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
                // Windows uses AMD's signed DLL for both versions.
                upscaler_for_version(dir, settings.upscaling.as_str(), windows)
            } else if settings.upscaling == FSR_316 || settings.upscaling == FSR_412 {
                // Both native providers are built under provider/ by the
                // install flow (3.1.6 at the top level, 4.1.2 in its own dir).
                upscaler_for_version(dir, settings.upscaling.as_str(), false)
            } else {
                PathBuf::new()
            };
            if !library.as_os_str().is_empty() {
                result.set.push(("MK8_FSR_LIB", library.into()));
            }
            // The GPL 4.1.2 port was built against the deprecated 1.x backend
            // descriptor; the source-built 3.1.6 upscaler uses the current ABI.
            if !windows && settings.upscaling == FSR_412 {
                result.set.push(("MK8_FSR_LEGACY_ABI", "1".into()));
                // The native port loads its extracted models/shaders from here.
                if settings.provider_lib.is_empty() {
                    result
                        .set
                        .push(("FSR4_ASSET_DIR", fsr4_assets_dir(dir).into()));
                }
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
#[cfg_attr(not(target_os = "linux"), allow(dead_code))]
pub fn configure(local: &Path) -> Result<()> {
    let settings = read_settings(local)?;
    if let Some(error) = settings.compatibility_error() {
        return Err(error);
    }
    if cfg!(windows) && settings.upscaling == FSR_412 {
        let gate = fsr4_gate(&settings);
        if fsr4_selection_blocked(&settings, &gate) {
            return Err(format!(
                "{} Selecione FSR 3.1.6 ou informe um provedor RDNA 4 manual.",
                gate.reason
            ));
        }
    }
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
    value["settings_error"] = json!(settings.compatibility_error());
    let gate = fsr4_gate(&settings);
    value["fsr4"] = json!({
        "blocked": gate.blocked,
        "allowed": gate.allowed(),
        "override": gate.override_active,
        "reason_code": gate.reason_code,
        "reason": gate.reason,
        "gpu": gate.gpu,
        "vendor": gate.vendor,
        "versions": gate.versions,
        "framegeneration_versions": gate.framegeneration_versions,
    });
    value["license"] = license_status(&dir);
    value["data_dir"] = json!(dir);
    value["provider_expected"] = json!(provider_path(&dir));
    let upscaler_expected = if settings.upscaling == FSR_412 {
        upscaler_for(&data_dir(), FSR_412)
    } else {
        upscaler_lib(&data_dir())
    };
    value["upscaler_expected"] = json!(upscaler_expected);
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
    fn windows_rejects_a_saved_linux_provider_path() {
        assert!(provider_platform_error("/home/u/libfsr.so", true).is_some());
        assert!(provider_platform_error("C:\\fsr\\amd_fidelityfx_upscaler_vk.dll", true).is_none());
        assert!(provider_platform_error("", true).is_none());
        assert!(provider_platform_error("/home/u/libfsr.so", false).is_none());
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
            provider_lib: if cfg!(windows) {
                "C:\\fsr\\provider.dll".into()
            } else {
                "/tmp/provider.so".into()
            },
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
        assert_eq!(value_of(&windows, "MK8_FSR_LEGACY_ABI"), None);

        // Linux 3.1.6 uses the source-built upscaler; the deprecated ABI
        // switch belongs only to the GPL 4.1.2 provider.
        let native = Settings {
            upscaling: FSR_316.into(),
            provider_lib: String::new(),
            frame_generation: FG_FSR3.into(),
            dxc: String::new(),
            legacy_abi: true,
        };
        let env = environment_for(&native, Path::new("/data/fsr"), false);
        assert_eq!(value_of(&env, "MK8_FSR_VERSION").as_deref(), Some(FSR_316));
        assert!(value_of(&env, "MK8_FSR_LIB")
            .unwrap()
            .ends_with("provider/libamd_fidelityfx_upscaler_vk.so"));
        assert_eq!(value_of(&env, "MK8_FSR_LEGACY_ABI"), None);
        assert_eq!(value_of(&env, "MK8_FSR_FG").as_deref(), Some("fsr3"));
        assert!(value_of(&env, "MK8_FSR_FG_LIB")
            .unwrap()
            .ends_with("provider/libamd_fidelityfx_framegeneration_vk.so"));

        // Linux 4.1.2 uses the GPL port, its asset directory and the legacy
        // backend ABI; frame generation remains independent.
        let fsr4 = Settings {
            upscaling: FSR_412.into(),
            provider_lib: String::new(),
            frame_generation: FG_FSR3.into(),
            dxc: String::new(),
            legacy_abi: false,
        };
        let env = environment_for(&fsr4, Path::new("/data/fsr"), false);
        assert_eq!(value_of(&env, "MK8_FSR_VERSION").as_deref(), Some(FSR_412));
        assert!(value_of(&env, "MK8_FSR_LIB")
            .unwrap()
            .ends_with("provider/fsr4.1.2/libamd_fidelityfx_upscaler_vk.so"));
        assert_eq!(value_of(&env, "MK8_FSR_LEGACY_ABI").as_deref(), Some("1"));
        assert!(value_of(&env, "FSR4_ASSET_DIR")
            .unwrap()
            .ends_with("provider/fsr4.1.2/fsr4-assets"));
        assert_eq!(value_of(&env, "MK8_FSR_FG").as_deref(), Some("fsr3"));
        assert!(value_of(&env, "MK8_FSR_FG_LIB")
            .unwrap()
            .ends_with("provider/libamd_fidelityfx_framegeneration_vk.so"));
    }

    #[test]
    fn windows_gpu_label_does_not_repeat_the_vendor() {
        fn gpu(vendor_id: u32, name: &str) -> crate::vulkan::GpuInfo {
            crate::vulkan::GpuInfo {
                api_version: 0,
                vendor_id,
                device_id: 0,
                device_type: 2,
                device_name: name.into(),
            }
        }
        let settings = Settings::default();
        let full = windows_fsr4_gate(
            &settings,
            Some(&gpu(0x10de, "NVIDIA GeForce RTX 5080")),
            &[],
            &[],
        );
        assert_eq!(full.gpu.as_deref(), Some("NVIDIA GeForce RTX 5080"));
        assert!(!full.reason.contains("NVIDIA NVIDIA"));

        let short = windows_fsr4_gate(&settings, Some(&gpu(0x10de, "GeForce RTX 5080")), &[], &[]);
        assert_eq!(short.gpu.as_deref(), Some("NVIDIA GeForce RTX 5080"));

        let lower = windows_fsr4_gate(
            &settings,
            Some(&gpu(0x10de, "nvidia GeForce RTX 5080")),
            &[],
            &[],
        );
        assert_eq!(lower.gpu.as_deref(), Some("nvidia GeForce RTX 5080"));

        let amd = windows_fsr4_gate(
            &settings,
            Some(&gpu(0x1002, "AMD Radeon RX 9070 XT")),
            &[],
            &[],
        );
        assert_eq!(amd.gpu.as_deref(), Some("AMD Radeon RX 9070 XT"));
    }

    #[test]
    fn windows_fsr4_gate_blocks_non_rdna4_and_keeps_316_selectable() {
        fn gpu(vendor_id: u32, name: &str) -> crate::vulkan::GpuInfo {
            crate::vulkan::GpuInfo {
                api_version: 0,
                vendor_id,
                device_id: 0,
                device_type: 2,
                device_name: name.into(),
            }
        }
        let fsr4 = Settings {
            upscaling: FSR_412.into(),
            provider_lib: String::new(),
            frame_generation: FG_OFF.into(),
            dxc: String::new(),
            legacy_abi: false,
        };
        let nvidia = gpu(0x10de, "NVIDIA GeForce RTX 5080");
        let gate = windows_fsr4_gate(&fsr4, Some(&nvidia), &[], &[]);
        assert!(gate.blocked);
        assert!(!gate.allowed());
        assert_eq!(gate.reason_code, "not-rdna4");
        assert!(gate.reason.contains("RDNA 4"));
        assert!(!nvidia.is_amd_rdna4());

        let fsr3 = Settings {
            upscaling: FSR_316.into(),
            ..fsr4.clone()
        };
        assert!(!fsr4_selection_blocked(&fsr3, &gate));

        let amd = gpu(0x1002, "AMD Radeon RX 9070 XT");
        assert!(amd.is_amd_rdna4());
        assert!(!gpu(0x1002, "AMD Radeon RX 7900 XTX").is_amd_rdna4());
        let gate = windows_fsr4_gate(
            &fsr4,
            Some(&amd),
            &["3.1.6".into(), "2.3.4".into()],
            &["3.1.7".into()],
        );
        assert!(gate.allowed());
        assert_eq!(gate.reason_code, "ok");
        assert!(!fsr4_selection_blocked(&fsr3, &gate));

        let gate = windows_fsr4_gate(
            &fsr4,
            Some(&amd),
            &["4.1.2".into(), "3.1.6".into()],
            &["4.0.2".into()],
        );
        assert!(!gate.blocked);
        assert!(gate.allowed());

        let override_settings = Settings {
            provider_lib: "C:\\fsr\\amd_fidelityfx_upscaler_vk.dll".into(),
            ..fsr4.clone()
        };
        let gate = windows_fsr4_gate(&override_settings, Some(&nvidia), &[], &[]);
        assert!(gate.blocked);
        assert!(gate.override_active);
        assert!(gate.allowed());
        assert_eq!(gate.reason_code, "not-rdna4");

        let missing = windows_fsr4_gate(&fsr4, None, &[], &[]);
        assert!(missing.blocked);
        assert_eq!(missing.reason_code, "gpu-missing");
        assert!(!missing.allowed());

        // Linux keeps the native 4.1.2/MLFI path and is not hardware-gated.
        #[cfg(not(windows))]
        {
            let native = fsr4_gate(&fsr4);
            assert!(!native.blocked);
            assert!(native.allowed());
            assert_eq!(native.reason_code, "native");
        }
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
