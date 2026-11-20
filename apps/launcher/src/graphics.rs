//! Optional Windows Vulkan upscalers. The official provider is verified again in the
//! supervisor before launch. Probing a driver or a provider runs in a separate process.
use mk8_manifest::{hash_file, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread::JoinHandle,
    time::{Duration, Instant},
};

struct File {
    name: &'static str,
    url: &'static str,
    bytes: u64,
    sha: &'static str,
}
const AMD: File = File {
    name: "amd_fidelityfx_upscaler_vk.dll",
    url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
    bytes: 36_135_696,
    sha: "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7",
};
const AMD_LICENSE: File = File {
    name: "LICENSE-AMD.md",
    url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/docs/license.md",
    bytes: 65_681, sha: "410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe",
};
const NVIDIA: File = File {
    name: "nvngx_dlss.dll",
    url: "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll",
    bytes: 58_956_912, sha: "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
};
const NVIDIA_LICENSE: File = File {
    name: "LICENSE-NVIDIA.txt",
    url: "https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/LICENSE.txt",
    bytes: 26_620,
    sha: "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
};

fn directory(local: &Path) -> PathBuf {
    local.join("graphics").join("providers")
}
impl File {
    fn valid(&self, path: &Path) -> bool {
        crate::debug_options::plain(path).is_ok()
            && fs::metadata(path).is_ok_and(|m| m.is_file() && m.len() == self.bytes)
            && hash_file(path).is_ok_and(|id| id.sha256 == self.sha)
    }
    fn install(&self, dir: &Path) -> Result<()> {
        let target = dir.join(self.name);
        crate::debug_options::plain(&target)?;
        if self.valid(&target) {
            return Ok(());
        }
        let stamp = u64::from_le_bytes(mk8_transport::random());
        let partial = dir.join(format!("{}.{stamp:x}.partial", self.name));
        fs::File::create_new(&partial).map_err(|e| e.to_string())?;
        let result = (|| {
            let status = crate::update::curl()
                .args([
                    "--connect-timeout",
                    "15",
                    "--max-time",
                    "180",
                    "--max-filesize",
                    &self.bytes.to_string(),
                    "--output",
                ])
                .arg(&partial)
                .args(["--url", self.url])
                .status()
                .map_err(|e| e.to_string())?;
            if !status.success() || !self.valid(&partial) {
                return Err(format!("Download ou SHA-256 inválido: {}", self.name));
            }
            fs::rename(&partial, &target).map_err(|e| e.to_string())
        })();
        if partial.exists() {
            let _ = fs::remove_file(&partial);
        }
        result
    }
}

#[derive(Default, Deserialize, Serialize)]
pub struct Capabilities {
    gpu: String,
    rdna4: bool,
    rtx: bool,
    amd: bool,
    nvidia: bool,
    versions: Vec<String>,
}

impl Capabilities {
    fn ready(&self, mode: &str) -> bool {
        match mode {
            "fsr3" => self.amd && self.versions.iter().any(|v| v == "3.1.6"),
            "fsr4" => self.amd && self.rdna4 && self.versions.iter().any(|v| v == "4.1.2"),
            "dlss" => self.rtx && self.nvidia,
            _ => false,
        }
    }
}

pub fn probe(local: &Path) -> Capabilities {
    let mut caps = Capabilities::default();
    if let Ok(gpu) = crate::vulkan::probe_gpu() {
        caps.rdna4 = gpu.is_amd_rdna4();
        caps.rtx = gpu.vendor_id == 0x10de && gpu.device_name.to_uppercase().contains("RTX");
        caps.gpu = gpu.device_name;
    }
    let dir = directory(local);
    let amd = dir.join(AMD.name);
    caps.amd = AMD.valid(&amd) && AMD_LICENSE.valid(&dir.join(AMD_LICENSE.name));
    if caps.amd {
        caps.versions = provider_versions_for(&amd, 0x0001_0000).unwrap_or_default();
    }
    caps.nvidia = NVIDIA.valid(&dir.join(NVIDIA.name))
        && NVIDIA_LICENSE.valid(&dir.join(NVIDIA_LICENSE.name));
    caps
}

fn inspect(local: &Path) -> Result<Capabilities> {
    use std::os::windows::process::CommandExt;
    let mut child = Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .arg("--graphics-gpu")
        .arg(local)
        .creation_flags(0x08000000)
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| e.to_string())?;
    let start = Instant::now();
    while child.try_wait().map_err(|e| e.to_string())?.is_none() {
        if start.elapsed() > Duration::from_secs(12) {
            let _ = child.kill();
            let _ = child.wait();
            return Err("Tempo esgotado na detecção gráfica.".into());
        }
        std::thread::sleep(Duration::from_millis(25));
    }
    let output = child.wait_with_output().map_err(|e| e.to_string())?;
    if !output.status.success() {
        return Err("Falha na detecção gráfica.".into());
    }
    serde_json::from_slice(&output.stdout).map_err(|e| e.to_string())
}

pub fn advanced(mode: &str) -> bool {
    matches!(mode, "fsr3" | "fsr4" | "dlss")
}

pub fn validate(local: &Path, mode: &str) -> Result<()> {
    if advanced(mode) && !inspect(local)?.ready(mode) {
        return Err("O componente gráfico está ausente ou indisponível nesta GPU.".into());
    }
    Ok(())
}

pub fn configure(local: &Path) -> Result<()> {
    for key in [
        "MK8_FSR4",
        "MK8_FSR_VERSION",
        "MK8_FSR_LIB",
        "MK8_FSR_FG",
        "MK8_FSR_LEGACY_ABI",
        "MK8_DLSS",
        "MK8_DLSS_LIB",
        "MK8_DLSS_CORE",
        "MK8_DLSS_DIR",
        "MK8_DLSS_FG",
    ] {
        std::env::remove_var(key);
    }
    let schema = crate::game_settings::read(local)?;
    let mode = schema["values"]["upscaler"].as_str().unwrap_or("");
    if !advanced(mode) || !inspect(local).is_ok_and(|caps| caps.ready(mode)) {
        return Ok(());
    }
    let dir = crate::canonical(&directory(local)).map_err(|e| e.to_string())?;
    if mode == "dlss" {
        std::env::set_var("MK8_DLSS", "1");
        std::env::set_var("MK8_DLSS_LIB", dir.join(NVIDIA.name));
        std::env::set_var("MK8_DLSS_DIR", &dir);
    } else {
        std::env::set_var("MK8_FSR4", "1");
        std::env::set_var(
            "MK8_FSR_VERSION",
            if mode == "fsr4" { "4.1.2" } else { "3.1.6" },
        );
        std::env::set_var("MK8_FSR_LIB", dir.join(AMD.name));
    }
    Ok(())
}

pub fn license(local: &Path, vendor: &str) -> Result<Value> {
    let file = match vendor {
        "amd" => &AMD_LICENSE,
        "nvidia" => &NVIDIA_LICENSE,
        _ => return Err("Provedor desconhecido.".into()),
    };
    let dir = directory(local);
    crate::debug_options::plain(&dir)?;
    fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    file.install(&dir)?;
    let text = fs::read_to_string(dir.join(file.name)).map_err(|e| e.to_string())?;
    Ok(json!({"text":text}))
}

#[derive(Default)]
pub struct Components {
    worker: Option<JoinHandle<Result<()>>>,
    error: Option<String>,
}

impl Components {
    pub fn status(&mut self, local: &Path) -> Result<Value> {
        if self.worker.as_ref().is_some_and(|w| w.is_finished()) {
            self.error = self
                .worker
                .take()
                .unwrap()
                .join()
                .unwrap_or_else(|_| Err("Instalação interrompida.".into()))
                .err();
        }
        if self.worker.is_some() {
            return Ok(json!({"running":true}));
        }
        Ok(json!({"running":false,"caps":inspect(local)?,"error":self.error}))
    }

    pub fn start(&mut self, local: &Path, body: &Value) -> Result<Value> {
        if body["accept_terms"] != true {
            return Err("Leia e aceite a licença do componente.".into());
        }
        if self.worker.is_some() {
            return self.status(local);
        }
        let caps = inspect(local)?;
        let (provider, license) = match body["vendor"].as_str() {
            Some("amd") if !caps.gpu.is_empty() => (&AMD, &AMD_LICENSE),
            Some("nvidia") if caps.rtx => (&NVIDIA, &NVIDIA_LICENSE),
            _ => return Err("Provedor indisponível para esta GPU Vulkan.".into()),
        };
        let dir = directory(local);
        crate::debug_options::plain(&dir)?;
        fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
        if crate::installer::free_gib(&dir).is_none_or(|n| n < 4.1) {
            return Err("Mantenha 4 GiB livres após a instalação.".into());
        }
        self.error = None;
        self.worker = Some(
            std::thread::Builder::new()
                .name("graphics-provider".into())
                .spawn(move || {
                    license.install(&dir)?;
                    provider.install(&dir)
                })
                .map_err(|e| e.to_string())?,
        );
        Ok(json!({"running":true}))
    }
}

use std::ffi::{c_char, c_void, CStr};
#[repr(C)]
struct FfxHeader {
    type_: u64,
    next: *mut FfxHeader,
}

#[repr(C)]
struct FfxQueryDescGetVersions {
    header: FfxHeader,
    create_desc_type: u64,
    device: *mut c_void,
    output_count: *mut u64,
    version_ids: *mut u64,
    version_names: *mut *const c_char,
}

type FfxQuery = unsafe extern "C" fn(*mut *mut c_void, *mut FfxHeader) -> u32;

fn provider_versions_for(path: &Path, create_desc_type: u64) -> Result<Vec<String>> {
    unsafe {
        let library = libloading::os::windows::Library::load_with_flags(
            path,
            windows_sys::Win32::System::LibraryLoader::LOAD_LIBRARY_SEARCH_SYSTEM32
                | windows_sys::Win32::System::LibraryLoader::LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
        )
        .map(libloading::Library::from)
        .map_err(|error| {
            format!(
                "Não foi possível abrir o provedor {}: {error}",
                path.display()
            )
        })?;
        let query: libloading::Symbol<FfxQuery> = library
            .get(b"ffxQuery\0")
            .map_err(|error| format!("O provedor não exporta ffxQuery: {error}"))?;
        let mut count = 0u64;
        let mut desc = FfxQueryDescGetVersions {
            header: FfxHeader {
                type_: 4,
                next: std::ptr::null_mut(),
            },
            create_desc_type,
            device: std::ptr::null_mut(),
            output_count: &mut count,
            version_ids: std::ptr::null_mut(),
            version_names: std::ptr::null_mut(),
        };
        let rc = query(std::ptr::null_mut(), &mut desc.header);
        if rc != 0 {
            return Err(format!(
                "ffxQuery não enumerou versões do provedor (código 0x{rc:x})."
            ));
        }
        if count == 0 {
            return Err("O provedor compilado não anuncia nenhuma versão.".into());
        }
        let capacity = count.min(16);
        let mut ids = vec![0u64; capacity as usize];
        let mut names: Vec<*const c_char> = vec![std::ptr::null(); capacity as usize];
        let mut available = capacity;
        desc.output_count = &mut available;
        desc.version_ids = ids.as_mut_ptr();
        desc.version_names = names.as_mut_ptr();
        let rc = query(std::ptr::null_mut(), &mut desc.header);
        if rc != 0 {
            return Err(format!(
                "ffxQuery falhou ao ler o nome da versão do provedor (código 0x{rc:x})."
            ));
        }
        let mut versions = Vec::new();
        for name in names.iter().take(available as usize) {
            if name.is_null() {
                continue;
            }
            let value = CStr::from_ptr(*name).to_string_lossy().into_owned();
            if !value.is_empty() {
                versions.push(value);
            }
        }
        if versions.is_empty() {
            Err("O provedor compilado não informou o nome da versão.".into())
        } else {
            Ok(versions)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vendor_modes_require_the_matching_hardware_and_component() {
        let mut caps = Capabilities {
            gpu: "Radeon RX 580".into(),
            amd: true,
            versions: vec!["3.1.6".into(), "4.1.2".into()],
            ..Default::default()
        };
        assert!(caps.ready("fsr3"));
        assert!(!caps.ready("fsr4"));
        caps.rdna4 = true;
        assert!(caps.ready("fsr4"));
        caps.versions.retain(|version| version != "4.1.2");
        assert!(!caps.ready("fsr4"));
        caps.nvidia = true;
        assert!(!caps.ready("dlss"));
        caps.rtx = true;
        assert!(caps.ready("dlss"));
        caps.nvidia = false;
        assert!(!caps.ready("dlss"));
    }

    #[test]
    fn component_rejects_a_same_size_replacement() {
        let name = std::env::temp_dir().join(format!(
            "mk8-graphics-{:x}",
            u64::from_le_bytes(mk8_transport::random())
        ));
        let sample = File {
            name: "sample",
            bytes: 3,
            url: "",
            sha: "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        };
        assert!(!sample.valid(&name));
        fs::write(&name, b"abc").unwrap();
        assert!(sample.valid(&name));
        fs::write(&name, b"abd").unwrap();
        assert!(!sample.valid(&name));
        fs::remove_file(name).unwrap();
    }
}
