//! Terms-gated AMD FidelityFX SDK download and local provider build.
//!
//! # What the launcher does and does not do
//!
//! On Linux it downloads exactly one pinned official source archive, verifies
//! its size and SHA-256, and hands it to the repository's local build script.
//! It never embeds, bundles or mirrors an AMD binary or SPIR-V blob, and it
//! never marks the provider ready unless the built library reports its version
//! through `ffxQuery`.
//!
//! # Contract with `tools/build_fg_provider.py`
//!
//! The launcher invokes the repository's script with:
//!
//! ```text
//! python3 -u tools/build_fg_provider.py \
//!   --sdk-archive <verified FidelityFX-SDK-2.4.0.tar.gz> \
//!   --sdk-size <bytes> --sdk-sha256 <hex> \
//!   --dxc-archive <verified linux_dxc_2026_09_28.x86_x64.tar.gz> \
//!   --dxc-size <bytes> --dxc-sha256 <hex> \
//!   --work-dir <$XDG_DATA_HOME/mk8online/fsr/build> \
//!   --install-dir <$XDG_DATA_HOME/mk8online/fsr/provider.next>
//! ```
//!
//! When the user configured a DXC binary (page field, `MK8_DXC`, tools folder
//! or `PATH`), the launcher passes `--dxc <path>` instead; the DXC archive
//! arguments are mutually exclusive with it.
//!
//! The script must:
//!
//! - verify the archive against `--sdk-size`/`--sdk-sha256` before building
//!   (the launcher verifies it again before and after the call);
//! - write `libamd_fidelityfx_framegeneration_vk.so` into `--install-dir`;
//! - exit non-zero on any failure, leaving a diagnostic on stdout/stderr.
//!
//! It must not write outside `--work-dir`/`--install-dir`, and it must not
//! fetch anything itself. The launcher then loads the new library with
//! `ffxQuery(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)` to read the provider
//! version, copies the verified license beside it, and only then publishes the
//! provider library.
//!
//! # Contract with `tools/build_upscaler_provider.py`
//!
//! For FSR 3.1.6 upscaling on Linux the same SDK and DXC inputs are passed to
//! the upscaler script, with `--install-dir <.../upscaler.next>`. It must write
//! `libamd_fidelityfx_upscaler_vk.so` there, verify the same pinned SDK/DXC
//! values and fail non-zero with a diagnostic. The launcher queries
//! `FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE` through `ffxQuery`, requires the
//! reported name to be exactly `3.1.6`, then atomically replaces
//! `provider/libamd_fidelityfx_upscaler_vk.so`. An already-installed library
//! that reports another version is rejected and rebuilt.
//!
//! The requirement that the archive is the SDK tag `v2.4.0` is enforced by the
//! pinned constants below, not by a mutable remote manifest.
//!
//! # Contract with `tools/build_fsr4_provider.py`
//!
//! The FSR 4.1.2 GPL-3.0 port is built from the same pinned SDK archive with
//! `--work-dir <fsr>/fsr4-build --install-dir <fsr>/fsr4.next --reuse-assets`.
//! The staging install directory is deliberate: the script writes
//! `libamd_fidelityfx_upscaler_vk.so`, `fsr4-assets/`, the GPL/AMD notices and
//! `SOURCE-OFFER.md` there, so the 3.1.6 upscaler at `provider/` is never
//! overwritten. The launcher verifies the reported version is exactly `4.1.2`
//! and the licence/asset layout is present, then atomically replaces
//! `provider/fsr4.1.2/`. DXC is not used by this provider.
//!
//! # Windows
//!
//! The same acceptance and verification gate downloads AMD's signed
//! `amd_fidelityfx_{loader,upscaler,framegeneration}_vk.dll` files from the
//! `Kits/FidelityFX/signedbin` folder of the same `v2.4.0` tag. Each has its
//! own pinned size and SHA-256; a mismatch removes the partial file and leaves
//! the previous install untouched. No build script or DXC is used on Windows.
//! The provider version is read with `ffxQuery` when the DLL can be loaded and
//! is reported as "not reported" otherwise (the signed file itself was still
//! verified against AMD's pinned hash).

use mk8_manifest::Result;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    ffi::{c_char, c_void, CStr},
    fs,
    io::{BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    time::{SystemTime, UNIX_EPOCH},
};

/// Official SDK tag and source archive. Both values are pinned in code.
pub const SDK_TAG: &str = "v2.4.0";
pub const SDK_ARCHIVE_FILE: &str = "FidelityFX-SDK-2.4.0.tar.gz";
pub const SDK_URL: &str =
    "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.4.0.tar.gz";
pub const SDK_BYTES: u64 = 253_592_010;
pub const SDK_SHA256: &str = "111f9b19c0c9b8a1f24d2a5104095b8e626f12deb4eca53cd0c29921aa1fd43a";

/// Pinned DXC release used only when no configured/system compiler exists.
/// DXC is open source (LLVM/Apache-2.0 with LLVM exceptions), not an AMD/FSR
/// artifact, so it is cached outside the FSR output and has no AMD gate.
pub const DXC_VERSION: &str = "1.9.2609.5";
pub const DXC_LICENSE_FILES: [&str; 2] = ["LICENSE-LLVM.txt", "LICENSE-MS.txt"];
pub const DXC_PINNED: PinnedFile = PinnedFile {
    file: "linux_dxc_2026_09_28.x86_x64.tar.gz",
    url: "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/linux_dxc_2026_09_28.x86_x64.tar.gz",
    bytes: 13_297_763,
    sha256: "96faadc7f5c282d2ffda49804beb4c3ee38127bc252b723234e3c5cdf7aa39a1",
};

pub struct PinnedFile {
    pub file: &'static str,
    pub url: &'static str,
    pub bytes: u64,
    pub sha256: &'static str,
}

/// AMD's `docs/license.md` at the same tag (`raw.githubusercontent.com`).
pub const LICENSE_FILE: &str = "LICENSE-AMD-FSR-SDK.md";
pub const LICENSE_URL: &str =
    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/docs/license.md";
pub const LICENSE_BYTES: u64 = 65_681;
pub const LICENSE_SHA256: &str = "410f9c138e766e846acf2dc93db768198def0293a017d7e3296815b451688dbe";

pub const TERMS_FILE: &str = "terms.json";
pub const INSTALL_FILE: &str = "install.json";
pub const PROGRESS_FILE: &str = "progress.json";
pub const BUILD_LOG: &str = "build.log";
#[cfg(target_os = "linux")]
pub const WORK_DIR: &str = "build";
#[cfg(target_os = "linux")]
pub const PROVIDER_DIR: &str = "provider";
pub const PROVIDER_REL: &str = "provider/libamd_fidelityfx_framegeneration_vk.so";
pub const BUILD_SCRIPT: &str = "tools/build_fg_provider.py";
/// Native Linux FSR 3.1.6 upscaler, built from the same pinned SDK and DXC.
pub const UPSCALER_VERSION: &str = "3.1.6";
#[cfg(target_os = "linux")]
pub const UPSCALER_FILE: &str = "libamd_fidelityfx_upscaler_vk.so";
pub const UPSCALER_REL: &str = "provider/libamd_fidelityfx_upscaler_vk.so";
pub const UPSCALER_STATE_FILE: &str = "upscaler.json";
#[cfg(target_os = "linux")]
pub const BUILD_UPSCALER_SCRIPT: &str = "tools/build_upscaler_provider.py";
/// Native Linux FSR 4.1.2 provider (GPL-3.0 port), in its own folder so the
/// 3.1.6 upscaler can be installed at the same time.
pub const FSR4_VERSION: &str = "4.1.2";
pub const FSR4_UPSCALER_REL: &str = "provider/fsr4.1.2/libamd_fidelityfx_upscaler_vk.so";
pub const FSR4_ASSETS_REL: &str = "provider/fsr4.1.2/fsr4-assets";
pub const FSR4_STATE_FILE: &str = "upscaler-412.json";
#[cfg(target_os = "linux")]
pub const BUILD_FSR4_SCRIPT: &str = "tools/build_fsr4_provider.py";

/// AMD's signed Vulkan DLLs for the Windows path, from the same pinned SDK
/// tag. They are downloaded and verified individually; the launcher never
/// bundles them.
pub struct SignedDll {
    pub name: &'static str,
    pub url: &'static str,
    pub bytes: u64,
    pub sha256: &'static str,
}

pub const SIGNED_DLLS: [SignedDll; 3] = [
    SignedDll {
        name: "amd_fidelityfx_loader_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll",
        bytes: 39_184,
        sha256: "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9",
    },
    SignedDll {
        name: "amd_fidelityfx_upscaler_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
        bytes: 36_135_696,
        sha256: "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7",
    },
    SignedDll {
        name: "amd_fidelityfx_framegeneration_vk.dll",
        url: "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_vk.dll",
        bytes: 36_806_928,
        sha256: "d002dabff8c5e71eaa715fc99438cd4dc81230262a7897b16bc402efec3498b4",
    },
];

const FFX_API_RETURN_OK: u32 = 0;
/// `FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK`
/// (`FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK | 0x01`).
const FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK: u64 = 0x0004_0001;
/// `FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE`
/// (`FFX_API_EFFECT_ID_UPSCALE | 0x00`).
const FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE: u64 = 0x0001_0000;
const FFX_API_QUERY_DESC_TYPE_GET_VERSIONS: u64 = 4;

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Request {
    pub root: PathBuf,
    pub local: PathBuf,
    pub fsr_dir: PathBuf,
    pub job_id: String,
    /// Either a configured path or a resolved one; empty asks the child to
    /// resolve it again from the same environment.
    pub dxc: String,
    /// Linux only: build the native FSR 3.1.6 upscaler.
    #[serde(default)]
    pub build_upscaler: bool,
    /// Linux only: build the native FSR 4.1.2 GPL provider.
    #[serde(default)]
    pub build_fsr4: bool,
    /// Linux only: build the FSR 3 frame-generation provider.
    #[serde(default)]
    pub build_framegeneration: bool,
}

#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct Progress {
    pub phase: String,
    pub percent: u8,
    pub detail: String,
}

#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct Installed {
    pub state: String,
    pub detail: String,
    pub version: Option<String>,
    #[serde(default)]
    pub upscaler_version: Option<String>,
    #[serde(default)]
    pub framegeneration_version: Option<String>,
    pub provider_bytes: u64,
    pub provider_sha256: String,
    pub sdk_sha256: String,
    pub built_at: u64,
}

#[derive(Serialize, Deserialize, Default, Clone, Debug)]
pub struct UpscalerState {
    pub version: String,
    pub bytes: u64,
    pub sha256: String,
    pub built_at: u64,
}

fn cache_lock(dir: &Path) -> Result<fs::File> {
    fs::create_dir_all(dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    let lock = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .create(true)
        .truncate(false)
        .open(dir.join(".install.lock"))
        .map_err(|error| error.to_string())?;
    lock.try_lock()
        .map_err(|error| format!("Outra instância está atualizando o cache FidelityFX: {error}"))?;
    Ok(lock)
}

pub struct Job {
    child: Child,
    dir: PathBuf,
    id: String,
}

impl Job {
    /// Spawns the installer in its own process, so a long download/build does
    /// not block the single-threaded launcher server.
    pub fn start(root: &Path, local: &Path, settings: &crate::fsr::Settings) -> Result<Self> {
        let dir = crate::fsr::data_dir();
        fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
        let request = Request {
            root: root.into(),
            local: local.into(),
            fsr_dir: dir.clone(),
            job_id: mk8_transport::random::<16>()
                .iter()
                .map(|byte| format!("{byte:02x}"))
                .collect(),
            dxc: settings.dxc.clone(),
            build_upscaler: cfg!(target_os = "linux") && settings.upscaling == "3.1.6",
            build_fsr4: cfg!(target_os = "linux") && settings.upscaling == "4.1.2",
            build_framegeneration: settings.frame_generation == "fsr3",
        };
        let mut command = Command::new(std::env::current_exe().map_err(|error| error.to_string())?);
        command
            .arg("--fsr-install")
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x08000000);
        }
        #[cfg(target_os = "linux")]
        mk8_build_manager::isolate_process_group(&mut command);
        let mut child = command
            .spawn()
            .map_err(|error| format!("Não foi possível iniciar a instalação do FSR: {error}"))?;
        let mut input = child
            .stdin
            .take()
            .ok_or("O processo de instalação do FSR não aceitou a solicitação.")?;
        serde_json::to_writer(&mut input, &request).map_err(|error| error.to_string())?;
        input.write_all(b"\n").map_err(|error| error.to_string())?;
        drop(input);
        let mut reply = String::new();
        BufReader::new(
            child
                .stdout
                .take()
                .ok_or("O processo de instalação do FSR não respondeu.")?,
        )
        .read_line(&mut reply)
        .map_err(|error| error.to_string())?;
        if reply.trim() != "READY" {
            let _ = child.wait();
            return Err(format!(
                "Não foi possível iniciar a instalação do FSR: {}",
                reply.trim()
            ));
        }
        Ok(Self {
            child,
            dir,
            id: request.job_id,
        })
    }

    /// `None` while running, the exit status once it finishes.
    pub fn poll(&mut self) -> Result<Option<std::process::ExitStatus>> {
        self.child.try_wait().map_err(|error| error.to_string())
    }

    pub fn cancel(&mut self) -> Result<()> {
        mk8_build_manager::terminate_tree(&mut self.child)?;
        self.record_interruption("Instalação do FSR cancelada pelo usuário.");
        Ok(())
    }

    pub fn record_interruption(&self, detail: &str) {
        let Ok(_lock) = cache_lock(&self.dir) else {
            return;
        };
        record_interrupted_job(&self.dir, &self.id, detail);
    }
}

impl Drop for Job {
    fn drop(&mut self) {
        // The launcher owns the download/build; an abandoned job must not keep
        // running curl/cmake after the window closes.
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = mk8_build_manager::terminate_tree(&mut self.child);
        }
    }
}

/// Entry point for `--fsr-install` children. Prints `READY` for the parent,
/// records progress and failures separately from the verified installation receipt.
pub fn run_from_stdin() -> Result<i32> {
    let request: Request = mk8_build_manager::supervisor::read_request()?;
    let _lock = match cache_lock(&request.fsr_dir) {
        Ok(lock) => lock,
        Err(error) => {
            println!("{error}");
            return Ok(1);
        }
    };
    set_progress(&request.fsr_dir, "starting", 0, "Iniciando instalação…")?;
    crate::fsr::write_atomic(&request.fsr_dir.join("job-id"), request.job_id.as_bytes())?;
    mk8_build_manager::supervisor::ready()?;
    match install(&request) {
        Ok(()) => Ok(0),
        Err(error) => {
            let _ = record_failure(&request.fsr_dir, &error);
            let _ = append_log(&request.fsr_dir, &format!("erro: {error}"));
            Ok(1)
        }
    }
}

fn install(request: &Request) -> Result<()> {
    let dir = &request.fsr_dir;
    let _ = fs::remove_file(dir.join(BUILD_LOG));
    append_log(
        dir,
        &format!(
            "== FSR: FidelityFX SDK {SDK_TAG} → {} ==",
            if cfg!(windows) {
                "DLLs assinadas da AMD"
            } else {
                "provedor Linux local"
            }
        ),
    )?;
    if !crate::fsr::terms_accepted(dir) {
        return Err(
            "Aceite os termos da licença da AMD nesta página antes de baixar o SDK.".into(),
        );
    }
    ensure_license(dir)?;
    #[cfg(windows)]
    return install_signed_dlls(dir);
    #[cfg(target_os = "linux")]
    return install_linux_provider(request, dir);
    #[cfg(not(any(windows, target_os = "linux")))]
    return Err("A instalação do FSR só está disponível no Linux e no Windows.".into());
}

#[cfg(target_os = "linux")]
enum DxcInput {
    Binary(PathBuf),
    Archive(PathBuf),
}

#[cfg(target_os = "linux")]
fn install_linux_provider(request: &Request, dir: &Path) -> Result<()> {
    if request.build_upscaler && request.build_fsr4 {
        return Err("Selecione apenas uma versão de ampliação para instalar.".into());
    }
    if !request.build_upscaler && !request.build_fsr4 && !request.build_framegeneration {
        return Err(
            "Nada para instalar: selecione FSR 3.1.6, FSR 4.1.2 ou FSR 3 nos componentes locais."
                .into(),
        );
    }
    if request.build_framegeneration && !request.root.join(BUILD_SCRIPT).is_file() {
        return Err(format!(
            "O script de build não existe: {}. Consulte o contrato na página FSR/launcher.",
            request.root.join(BUILD_SCRIPT).display()
        ));
    }
    if request.build_upscaler && !request.root.join(BUILD_UPSCALER_SCRIPT).is_file() {
        return Err(format!(
            "O script do upscaler não existe: {}. Consulte o contrato na página FSR/launcher.",
            request.root.join(BUILD_UPSCALER_SCRIPT).display()
        ));
    }
    if request.build_fsr4 && !request.root.join(BUILD_FSR4_SCRIPT).is_file() {
        return Err(format!(
            "O script do FSR 4.1.2 não existe: {}. Consulte o contrato na página FSR/launcher.",
            request.root.join(BUILD_FSR4_SCRIPT).display()
        ));
    }
    let reuse_upscaler = request.build_upscaler && upscaler_ready(dir, UPSCALER_VERSION);
    if reuse_upscaler {
        record_upscaler_state(dir, UPSCALER_VERSION)?;
        append_log(
            dir,
            &format!("provedor FSR {UPSCALER_VERSION} já instalado; reutilizando."),
        )?;
    }
    let reuse_fsr4 = request.build_fsr4 && upscaler_ready(dir, FSR4_VERSION);
    if reuse_fsr4 {
        record_upscaler_state(dir, FSR4_VERSION)?;
        append_log(
            dir,
            &format!("provedor FSR {FSR4_VERSION} já instalado; reutilizando."),
        )?;
    }
    let build_up = request.build_upscaler && !reuse_upscaler;
    let build_four = request.build_fsr4 && !reuse_fsr4;
    if !build_up && !build_four && !request.build_framegeneration {
        set_progress(
            dir,
            "done",
            100,
            "Os componentes FSR selecionados já estão instalados.",
        )?;
        return Ok(());
    }

    set_progress(dir, "download", 1, "Preparando o SDK da AMD…")?;
    ensure_sdk(dir)?;
    let needs_dxc = request.build_upscaler || request.build_framegeneration;
    let dxc_input = if needs_dxc {
        let dxc = resolve_dxc(&request.root, dir, &request.dxc);
        Some(if dxc.found && !dxc.archive {
            DxcInput::Binary(dxc.path)
        } else if !request.dxc.is_empty() && !dxc.found {
            return Err(format!("DXC não encontrado em {}.", request.dxc));
        } else {
            set_progress(
                dir,
                "dxc",
                12,
                "Preparando o compilador DXC (fonte oficial fixada)…",
            )?;
            DxcInput::Archive(ensure_dxc(dir)?)
        })
    } else {
        None
    };
    if request.build_framegeneration {
        build_provider(
            request,
            dir,
            &request.root.join(BUILD_SCRIPT),
            dxc_input
                .as_ref()
                .ok_or("Compilador DXC ausente para a geração de quadros.")?,
            build_up || build_four,
        )?;
    }
    if build_up {
        build_upscaler(
            request,
            dir,
            dxc_input
                .as_ref()
                .ok_or("Compilador DXC ausente para o upscaler.")?,
        )?;
    }
    if build_four {
        build_fsr4(request, dir)?;
    }
    Ok(())
}

/// Windows uses AMD's signed Vulkan DLLs from the same tag. They are verified
/// byte-for-byte against the pinned size and SHA-256 before being published;
/// the version is read back with `ffxQuery` when the library can be loaded.
#[cfg(windows)]
fn install_signed_dlls(dir: &Path) -> Result<()> {
    for (index, dll) in SIGNED_DLLS.iter().enumerate() {
        let target = dir.join(dll.name);
        let valid = matches!(fs::metadata(&target), Ok(meta) if meta.len() == dll.bytes)
            && crate::fsr::sha256_file(&target).is_ok_and(|hash| hash == dll.sha256);
        let base = 5 + (index as u8 * 25);
        if valid {
            append_log(dir, &format!("{}: já em cache e conferido", dll.name))?;
            set_progress(
                dir,
                "download",
                base,
                &format!("{} já está em cache e confere.", dll.name),
            )?;
            continue;
        }
        if target.exists() {
            fs::remove_file(&target).map_err(|error| format!("{}: {error}", target.display()))?;
        }
        let part = dir.join(format!("{}.part", dll.name));
        let _ = fs::remove_file(&part);
        set_progress(
            dir,
            "download",
            base,
            &format!(
                "Baixando {} ({} bytes) da fonte fixada…",
                dll.name, dll.bytes
            ),
        )?;
        let output = crate::update::curl()
            .args(["--max-time", "900", "--retry", "2", "--retry-delay", "1"])
            .args(["--max-filesize", &(dll.bytes + 1).to_string(), "--output"])
            .arg(&part)
            .args(["--url", dll.url])
            .output()
            .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).".to_string())?;
        if !output.status.success() {
            let _ = fs::remove_file(&part);
            return Err(format!(
                "Download de {} falhou: {}",
                dll.name,
                String::from_utf8_lossy(&output.stderr).trim()
            ));
        }
        verify_and_publish(&part, &target, dll.bytes, dll.sha256)?;
        append_log(
            dir,
            &format!("{}: {} bytes, sha256={}", dll.name, dll.bytes, dll.sha256),
        )?;
    }
    set_progress(dir, "verify", 90, "Lendo as versões das DLLs assinadas…")?;
    let upscaler = provider_version_for(
        &crate::fsr::upscaler_lib(dir),
        FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE,
    )
    .ok();
    let framegeneration = provider_version_for(
        &crate::fsr::framegeneration_lib(dir),
        FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK,
    )
    .ok();
    let version = format!(
        "upscaling {} · frame generation {}",
        upscaler.as_deref().unwrap_or("não informada"),
        framegeneration.as_deref().unwrap_or("não informada")
    );
    let provider = crate::fsr::framegeneration_lib(dir);
    let sha256 = crate::fsr::sha256_file(&provider)?;
    let bytes = fs::metadata(&provider)
        .map(|meta| meta.len())
        .map_err(|error| error.to_string())?;
    record_installed(
        dir,
        &version,
        upscaler.as_deref(),
        framegeneration.as_deref(),
        &sha256,
        bytes,
    )?;
    set_progress(
        dir,
        "done",
        100,
        &format!("DLLs assinadas da AMD prontas em {}.", dir.display()),
    )?;
    append_log(dir, &format!("== DLLs assinadas prontas: {version} =="))?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn build_provider(
    request: &Request,
    dir: &Path,
    script: &Path,
    dxc: &DxcInput,
    followed_by_upscaler: bool,
) -> Result<()> {
    let next = dir.join("provider.next");
    if next.exists() {
        fs::remove_dir_all(&next).map_err(|error| format!("{}: {error}", next.display()))?;
    }
    fs::create_dir_all(&next).map_err(|error| format!("{}: {error}", next.display()))?;
    let archive = dir.join(SDK_ARCHIVE_FILE);
    set_progress(
        dir,
        "build",
        15,
        "Compilando o provedor FSR (pode levar vários minutos)…",
    )?;
    let mut command = Command::new(python(&request.root));
    command
        .arg("-u")
        .arg(script)
        .arg("--sdk-archive")
        .arg(&archive)
        .arg("--sdk-size")
        .arg(SDK_BYTES.to_string())
        .arg("--sdk-sha256")
        .arg(SDK_SHA256);
    match dxc {
        DxcInput::Binary(path) => {
            command.arg("--dxc").arg(path);
        }
        DxcInput::Archive(path) => {
            command
                .arg("--dxc-archive")
                .arg(path)
                .arg("--dxc-size")
                .arg(DXC_PINNED.bytes.to_string())
                .arg("--dxc-sha256")
                .arg(DXC_PINNED.sha256);
        }
    }
    command
        .arg("--work-dir")
        .arg(dir.join(WORK_DIR))
        .arg("--install-dir")
        .arg(&next)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    append_log(
        dir,
        &format!(
            "$ {} {:?}",
            python(&request.root).display(),
            command.get_args().collect::<Vec<_>>()
        ),
    )?;
    let mut child = command.spawn().map_err(|error| {
        format!(
            "Não foi possível executar {}: {error}",
            python(&request.root).display()
        )
    })?;
    let stdout = child.stdout.take().ok_or("Build sem saída padrão.")?;
    let stderr = child.stderr.take().ok_or("Build sem saída de erro.")?;
    let log = dir.join(BUILD_LOG);
    let stderr_log = log.clone();
    let stderr_thread = std::thread::spawn(move || {
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&stderr_log)
            .ok();
        let mut reader = BufReader::new(stderr);
        let mut line = String::new();
        while reader.read_line(&mut line).unwrap_or(0) > 0 {
            if let Some(file) = file.as_mut() {
                let _ = file.write_all(line.as_bytes());
            }
            line.clear();
        }
    });
    let mut current = 15u8;
    let cap = if followed_by_upscaler { 70 } else { 95 };
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    while reader.read_line(&mut line).unwrap_or(0) > 0 {
        append_log(dir, line.trim_end_matches(['\n', '\r']))?;
        if let Some((percent, detail)) = progress_marker(&line, current) {
            let percent = percent.min(cap);
            if percent > current || detail != progress(dir).detail {
                current = percent;
                set_progress(dir, "build", current, &detail)?;
            }
        }
        line.clear();
    }
    let _ = stderr_thread.join();
    let status = child.wait().map_err(|error| error.to_string())?;
    if !status.success() {
        let tail = log_tail(dir, 8);
        return Err(format!(
            "A build do provedor FSR falhou ({status}).{}",
            if tail.is_empty() {
                String::new()
            } else {
                format!("\n{tail}")
            }
        ));
    }
    let produced = next.join(
        Path::new(PROVIDER_REL)
            .file_name()
            .ok_or("Caminho do provedor inválido.")?,
    );
    let meta = fs::metadata(&produced)
        .map_err(|_| format!("A build terminou sem criar {}.", produced.display()))?;
    if !meta.is_file() || meta.len() == 0 {
        return Err(format!(
            "A build criou um provedor vazio ou inválido: {}.",
            produced.display()
        ));
    }
    set_progress(dir, "verify", 96, "Lendo a versão do provedor compilado…")?;
    let version = provider_version(&produced)?;
    ensure_license(dir)?;
    fs::copy(crate::fsr::license_path(dir), next.join(LICENSE_FILE))
        .map_err(|error| format!("Não foi possível copiar a licença para o provedor: {error}"))?;
    publish_provider(dir, &next)?;
    let provider = crate::fsr::provider_path(dir);
    let sha256 = crate::fsr::sha256_file(&provider)?;
    let bytes = fs::metadata(&provider)
        .map(|meta| meta.len())
        .map_err(|error| error.to_string())?;
    record_installed(dir, &version, None, Some(&version), &sha256, bytes)?;
    if followed_by_upscaler {
        set_progress(
            dir,
            "build",
            70,
            "Provedor de frame generation pronto; continuando a instalação…",
        )?;
    } else {
        set_progress(
            dir,
            "done",
            100,
            &format!("Provedor FSR {version} pronto em {}.", provider.display()),
        )?;
    }
    append_log(
        dir,
        &format!(
            "== Provedor FSR {version} pronto ({} bytes, sha256={sha256}) ==",
            bytes
        ),
    )?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn installed_upscaler_version(dir: &Path, version: &str) -> Option<String> {
    let path = crate::fsr::upscaler_for(dir, version);
    if !path.is_file() {
        return None;
    }
    match provider_version_for(&path, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE) {
        Ok(reported) => Some(reported),
        Err(error) => {
            let _ = append_log(
                dir,
                &format!("O upscaler instalado não pôde ser consultado: {error}"),
            );
            None
        }
    }
}

#[cfg(target_os = "linux")]
fn upscaler_version_ok(version: &str, expected: &str) -> bool {
    version == expected
}

#[cfg(target_os = "linux")]
fn fsr4_dispatch_supported(path: &Path) -> bool {
    unsafe {
        libloading::Library::new(path).is_ok_and(|lib| {
            lib.get::<unsafe extern "C" fn()>(b"mk8Fsr4DispatchV1\0")
                .is_ok()
        })
    }
}

#[cfg(target_os = "linux")]
fn upscaler_ready(dir: &Path, version: &str) -> bool {
    if !installed_upscaler_version(dir, version)
        .is_some_and(|reported| upscaler_version_ok(&reported, version))
    {
        return false;
    }
    if version == FSR4_VERSION {
        let provider = crate::fsr::upscaler_for(dir, version);
        return provider.parent().is_some_and(fsr4_licenses_present)
            && fsr4_dispatch_supported(&provider);
    }
    true
}

fn upscaler_state_file(version: &str) -> &'static str {
    if version == FSR4_VERSION {
        FSR4_STATE_FILE
    } else {
        UPSCALER_STATE_FILE
    }
}

#[cfg(target_os = "linux")]
fn record_upscaler_state(dir: &Path, version: &str) -> Result<UpscalerState> {
    let path = crate::fsr::upscaler_for(dir, version);
    let bytes = fs::metadata(&path)
        .map(|meta| meta.len())
        .map_err(|error| format!("{}: {error}", path.display()))?;
    let state = UpscalerState {
        version: version.into(),
        bytes,
        sha256: crate::fsr::sha256_file(&path)?,
        built_at: unix_now(),
    };
    crate::fsr::write_atomic(
        &dir.join(upscaler_state_file(version)),
        &serde_json::to_vec_pretty(&state).map_err(|error| error.to_string())?,
    )?;
    Ok(state)
}

pub fn read_upscaler_state(dir: &Path, version: &str) -> Option<UpscalerState> {
    fs::read_to_string(dir.join(upscaler_state_file(version)))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
}

#[cfg(target_os = "linux")]
fn publish_upscaler(dir: &Path, next: &Path) -> Result<()> {
    let provider = dir.join(PROVIDER_DIR);
    fs::create_dir_all(&provider).map_err(|error| format!("{}: {error}", provider.display()))?;
    let source = next.join(UPSCALER_FILE);
    let target = provider.join(UPSCALER_FILE);
    fs::rename(&source, &target).map_err(|error| format!("{}: {error}", target.display()))?;
    let license = next.join(LICENSE_FILE);
    if license.is_file() {
        fs::copy(&license, provider.join(LICENSE_FILE))
            .map_err(|error| format!("{}: {error}", provider.display()))?;
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn build_upscaler(request: &Request, dir: &Path, dxc: &DxcInput) -> Result<()> {
    if let Some(version) = installed_upscaler_version(dir, UPSCALER_VERSION) {
        if upscaler_version_ok(&version, UPSCALER_VERSION) {
            record_upscaler_state(dir, &version)?;
            set_progress(
                dir,
                "done",
                100,
                &format!("Provedor FSR {version} já está instalado."),
            )?;
            return Ok(());
        }
        append_log(
            dir,
            &format!(
                "O upscaler instalado reporta {version}; esperado {UPSCALER_VERSION}. Recompilando."
            ),
        )?;
    }
    let next = dir.join("upscaler.next");
    if next.exists() {
        fs::remove_dir_all(&next).map_err(|error| format!("{}: {error}", next.display()))?;
    }
    fs::create_dir_all(&next).map_err(|error| format!("{}: {error}", next.display()))?;
    let script = request.root.join(BUILD_UPSCALER_SCRIPT);
    if !script.is_file() {
        return Err(format!(
            "O script do upscaler não existe: {}.",
            script.display()
        ));
    }
    set_progress(
        dir,
        "build",
        72,
        "Compilando o upscaler FSR 3.1.6 (pode levar vários minutos)…",
    )?;
    let mut command = Command::new(python(&request.root));
    command
        .arg("-u")
        .arg(&script)
        .arg("--sdk-archive")
        .arg(dir.join(SDK_ARCHIVE_FILE))
        .arg("--sdk-size")
        .arg(SDK_BYTES.to_string())
        .arg("--sdk-sha256")
        .arg(SDK_SHA256);
    match dxc {
        DxcInput::Binary(path) => {
            command.arg("--dxc").arg(path);
        }
        DxcInput::Archive(path) => {
            command
                .arg("--dxc-archive")
                .arg(path)
                .arg("--dxc-size")
                .arg(DXC_PINNED.bytes.to_string())
                .arg("--dxc-sha256")
                .arg(DXC_PINNED.sha256);
        }
    }
    command
        .arg("--work-dir")
        .arg(dir.join("upscaler-build"))
        .arg("--install-dir")
        .arg(&next)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8");
    append_log(
        dir,
        &format!(
            "$ {} {:?}",
            python(&request.root).display(),
            command.get_args().collect::<Vec<_>>()
        ),
    )?;
    let mut child = command.spawn().map_err(|error| {
        format!(
            "Não foi possível executar {}: {error}",
            python(&request.root).display()
        )
    })?;
    let stdout = child.stdout.take().ok_or("Build sem saída padrão.")?;
    let stderr = child.stderr.take().ok_or("Build sem saída de erro.")?;
    let log = dir.join(BUILD_LOG);
    let stderr_log = log.clone();
    let stderr_thread = std::thread::spawn(move || {
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&stderr_log)
            .ok();
        let mut reader = BufReader::new(stderr);
        let mut line = String::new();
        while reader.read_line(&mut line).unwrap_or(0) > 0 {
            if let Some(file) = file.as_mut() {
                let _ = file.write_all(line.as_bytes());
            }
            line.clear();
        }
    });
    let mut current = 72u8;
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    while reader.read_line(&mut line).unwrap_or(0) > 0 {
        append_log(dir, line.trim_end_matches(['\n', '\r']))?;
        if let Some((percent, detail)) = upscaler_progress_marker(&line, current) {
            if percent > current || detail != progress(dir).detail {
                current = percent;
                set_progress(dir, "build", current, &detail)?;
            }
        }
        line.clear();
    }
    let _ = stderr_thread.join();
    let status = child.wait().map_err(|error| error.to_string())?;
    if !status.success() {
        let tail = log_tail(dir, 8);
        return Err(format!(
            "A build do upscaler FSR 3.1.6 falhou ({status}).{}",
            if tail.is_empty() {
                String::new()
            } else {
                format!("\n{tail}")
            }
        ));
    }
    let produced = next.join(UPSCALER_FILE);
    let meta = fs::metadata(&produced)
        .map_err(|_| format!("A build do upscaler não criou {}.", produced.display()))?;
    if !meta.is_file() || meta.len() == 0 {
        return Err(format!(
            "A build do upscaler criou um arquivo vazio ou inválido: {}.",
            produced.display()
        ));
    }
    set_progress(dir, "verify", 96, "Lendo a versão do upscaler compilado…")?;
    let version = provider_version_for(&produced, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)?;
    if !upscaler_version_ok(&version, UPSCALER_VERSION) {
        return Err(format!(
            "O upscaler compilado reporta a versão {version}; esperado {UPSCALER_VERSION}. O provedor não foi instalado."
        ));
    }
    ensure_license(dir)?;
    fs::copy(crate::fsr::license_path(dir), next.join(LICENSE_FILE))
        .map_err(|error| format!("Não foi possível copiar a licença para o upscaler: {error}"))?;
    publish_upscaler(dir, &next)?;
    let state = record_upscaler_state(dir, &version)?;
    set_progress(
        dir,
        "done",
        100,
        &format!(
            "Provedor FSR {version} pronto em {}.",
            crate::fsr::upscaler_lib(dir).display()
        ),
    )?;
    append_log(
        dir,
        &format!(
            "== Provedor FSR {version} pronto ({} bytes, sha256={}) ==",
            state.bytes, state.sha256
        ),
    )?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn upscaler_progress_marker(line: &str, current: u8) -> Option<(u8, String)> {
    let line = line.trim();
    if line.starts_with("sdk archive ok") {
        Some((74, "SDK da AMD verificado; compilando o upscaler…".into()))
    } else if line.starts_with("sdk tree:") {
        Some((78, "Fontes do SDK preparadas; compilando shaders…".into()))
    } else if line.starts_with("+ cmake -S") {
        Some((82, "Configurando a build do upscaler…".into()))
    } else if line.starts_with("+ cmake --build") {
        Some((85, "Compilando o upscaler FSR 3.1.6…".into()))
    } else if let Some(rest) = line.strip_prefix('[') {
        let (done, rest) = rest.split_once('/')?;
        let (total, _) = rest.split_once(']')?;
        let done: u32 = done.trim().parse().ok()?;
        let total: u32 = total.trim().parse().ok()?;
        (total > 0).then(|| {
            (
                (85 + (done.saturating_mul(10) / total) as u8).min(95),
                format!("Compilando o upscaler FSR 3.1.6… {done}/{total}"),
            )
        })
    } else if line.starts_with("installed ") {
        Some((98, "Upscaler compilado; verificando a versão…".into()))
    } else if line.starts_with("error:") {
        Some((current, line.into()))
    } else {
        None
    }
}

fn fsr4_licenses_present(dir: &Path) -> bool {
    let has_gpl = dir.join("LICENSE-GPL-3.0.txt").is_file();
    let has_amd = fs::read_dir(dir).is_ok_and(|entries| {
        entries.flatten().any(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .starts_with("LICENSE-AMD-")
        })
    });
    has_gpl
        && has_amd
        && dir.join("SOURCE-OFFER.md").is_file()
        && dir.join("fsr4-assets/models").is_dir()
        && dir.join("fsr4-assets/shaders").is_dir()
}

#[cfg(target_os = "linux")]
fn publish_fsr4(dir: &Path, next: &Path) -> Result<()> {
    if !fsr4_licenses_present(next) {
        return Err(
            "A build do FSR 4.1.2 não instalou as licenças GPL/AMD, a nota de oferta de fonte e os assets esperados."
                .into(),
        );
    }
    let target = crate::fsr::upscaler_for(dir, FSR4_VERSION)
        .parent()
        .ok_or("Caminho do provedor FSR 4.1.2 inválido.")?
        .to_path_buf();
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    let old = target.with_file_name("fsr4.1.2.old");
    if old.exists() {
        fs::remove_dir_all(&old).map_err(|error| format!("{}: {error}", old.display()))?;
    }
    if target.exists() {
        fs::rename(&target, &old).map_err(|error| format!("{}: {error}", target.display()))?;
    }
    if let Err(error) = fs::rename(next, &target) {
        if old.exists() {
            let _ = fs::rename(&old, &target);
        }
        return Err(format!("{}: {error}", target.display()));
    }
    if old.exists() {
        let _ = fs::remove_dir_all(&old);
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn build_fsr4(request: &Request, dir: &Path) -> Result<()> {
    if upscaler_ready(dir, FSR4_VERSION) {
        record_upscaler_state(dir, FSR4_VERSION)?;
        set_progress(
            dir,
            "done",
            100,
            &format!("Provedor FSR {FSR4_VERSION} já está instalado."),
        )?;
        return Ok(());
    }
    let script = request.root.join(BUILD_FSR4_SCRIPT);
    if !script.is_file() {
        return Err(format!(
            "O script do FSR 4.1.2 não existe: {}.",
            script.display()
        ));
    }
    let next = dir.join("fsr4.next");
    if next.exists() {
        fs::remove_dir_all(&next).map_err(|error| format!("{}: {error}", next.display()))?;
    }
    set_progress(
        dir,
        "build",
        72,
        "Compilando o provedor FSR 4.1.2 (GPL-3.0, pode levar vários minutos)…",
    )?;
    let mut command = Command::new(python(&request.root));
    command
        .arg("-u")
        .arg(&script)
        .arg("--sdk-archive")
        .arg(dir.join(SDK_ARCHIVE_FILE))
        .arg("--sdk-size")
        .arg(SDK_BYTES.to_string())
        .arg("--sdk-sha256")
        .arg(SDK_SHA256)
        .arg("--work-dir")
        .arg(dir.join("fsr4-build"))
        .arg("--install-dir")
        .arg(&next)
        .arg("--reuse-assets")
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8");
    append_log(
        dir,
        &format!(
            "$ {} {:?}",
            python(&request.root).display(),
            command.get_args().collect::<Vec<_>>()
        ),
    )?;
    let mut child = command.spawn().map_err(|error| {
        format!(
            "Não foi possível executar {}: {error}",
            python(&request.root).display()
        )
    })?;
    let stdout = child.stdout.take().ok_or("Build sem saída padrão.")?;
    let stderr = child.stderr.take().ok_or("Build sem saída de erro.")?;
    let log = dir.join(BUILD_LOG);
    let stderr_log = log.clone();
    let stderr_thread = std::thread::spawn(move || {
        let mut file = fs::OpenOptions::new()
            .create(true)
            .append(true)
            .open(&stderr_log)
            .ok();
        let mut reader = BufReader::new(stderr);
        let mut line = String::new();
        while reader.read_line(&mut line).unwrap_or(0) > 0 {
            if let Some(file) = file.as_mut() {
                let _ = file.write_all(line.as_bytes());
            }
            line.clear();
        }
    });
    let mut current = 72u8;
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    while reader.read_line(&mut line).unwrap_or(0) > 0 {
        append_log(dir, line.trim_end_matches(['\n', '\r']))?;
        if let Some((percent, detail)) = fsr4_progress_marker(&line, current) {
            if percent > current || detail != progress(dir).detail {
                current = percent;
                set_progress(dir, "build", current, &detail)?;
            }
        }
        line.clear();
    }
    let _ = stderr_thread.join();
    let status = child.wait().map_err(|error| error.to_string())?;
    if !status.success() {
        let tail = log_tail(dir, 8);
        return Err(format!(
            "A build do FSR 4.1.2 falhou ({status}).{}",
            if tail.is_empty() {
                String::new()
            } else {
                format!("\n{tail}")
            }
        ));
    }
    let produced = next.join(UPSCALER_FILE);
    let meta = fs::metadata(&produced)
        .map_err(|_| format!("A build do FSR 4.1.2 não criou {}.", produced.display()))?;
    if !meta.is_file() || meta.len() == 0 {
        return Err(format!(
            "A build do FSR 4.1.2 criou um arquivo vazio ou inválido: {}.",
            produced.display()
        ));
    }
    set_progress(dir, "verify", 96, "Lendo a versão do provedor FSR 4.1.2…")?;
    let version = provider_version_for(&produced, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE)?;
    if !upscaler_version_ok(&version, FSR4_VERSION) {
        return Err(format!(
            "O provedor compilado reporta a versão {version}; esperado {FSR4_VERSION}. O provedor não foi instalado."
        ));
    }
    if !fsr4_dispatch_supported(&produced) {
        return Err("O provedor compilado não exporta mk8Fsr4DispatchV1.".into());
    }
    publish_fsr4(dir, &next)?;
    let state = record_upscaler_state(dir, FSR4_VERSION)?;
    set_progress(
        dir,
        "done",
        100,
        &format!(
            "Provedor FSR {FSR4_VERSION} pronto em {}.",
            crate::fsr::upscaler_for(dir, FSR4_VERSION).display()
        ),
    )?;
    append_log(
        dir,
        &format!(
            "== Provedor FSR {FSR4_VERSION} pronto ({} bytes, sha256={}) ==",
            state.bytes, state.sha256
        ),
    )?;
    Ok(())
}

#[cfg(target_os = "linux")]
fn fsr4_progress_marker(line: &str, current: u8) -> Option<(u8, String)> {
    let line = line.trim();
    if line.starts_with("sdk tree:") {
        Some((
            74,
            "Fontes do SDK preparadas; extraindo o DLL assinado…".into(),
        ))
    } else if line.contains("upscaler DLL ok:") {
        Some((
            76,
            "DLL assinada da AMD verificada; extraindo shaders e modelos…".into(),
        ))
    } else if line.starts_with("assets ok:") || line.starts_with("reusing extracted assets") {
        Some((
            80,
            "Assets do FSR 4.1.2 extraídos; compilando o provedor…".into(),
        ))
    } else if line.starts_with("+ cmake -S") {
        Some((82, "Configurando a build do FSR 4.1.2…".into()))
    } else if line.starts_with("+ cmake --build") {
        Some((85, "Compilando o provedor FSR 4.1.2…".into()))
    } else if let Some(rest) = line.strip_prefix('[') {
        let (done, rest) = rest.split_once('/')?;
        let (total, _) = rest.split_once(']')?;
        let done: u32 = done.trim().parse().ok()?;
        let total: u32 = total.trim().parse().ok()?;
        (total > 0).then(|| {
            (
                (85 + (done.saturating_mul(10) / total) as u8).min(95),
                format!("Compilando o provedor FSR 4.1.2… {done}/{total}"),
            )
        })
    } else if line.starts_with("installed runtime assets:") {
        Some((
            96,
            "Assets de execução instalados; verificando a versão…".into(),
        ))
    } else if line.starts_with("installed ") {
        Some((
            98,
            "Provedor FSR 4.1.2 instalado; conferindo licenças…".into(),
        ))
    } else if line == "done" {
        Some((99, "Provedor FSR 4.1.2 compilado; publicando…".into()))
    } else if line.starts_with("error:") {
        Some((current, line.into()))
    } else {
        None
    }
}

#[cfg(target_os = "linux")]
fn publish_provider(dir: &Path, next: &Path) -> Result<()> {
    let provider = dir.join(PROVIDER_DIR);
    fs::create_dir_all(&provider).map_err(|error| format!("{}: {error}", provider.display()))?;
    let name = Path::new(PROVIDER_REL)
        .file_name()
        .ok_or("Caminho do provedor inválido.")?;
    fs::rename(next.join(name), provider.join(name))
        .map_err(|error| format!("{}: {error}", provider.display()))?;
    // Keep the verified AMD licence beside the provider; replacing only the
    // library preserves a previously installed upscaler in the same folder.
    let license = next.join(LICENSE_FILE);
    if license.is_file() {
        fs::copy(&license, provider.join(LICENSE_FILE))
            .map_err(|error| format!("{}: {error}", provider.display()))?;
    }
    Ok(())
}

#[cfg(target_os = "linux")]
fn ensure_sdk(dir: &Path) -> Result<()> {
    let archive = dir.join(SDK_ARCHIVE_FILE);
    if archive.is_file() {
        let bytes = fs::metadata(&archive)
            .map(|meta| meta.len())
            .unwrap_or_default();
        if bytes == SDK_BYTES && crate::fsr::sha256_file(&archive)? == SDK_SHA256 {
            write_marker(dir, SDK_SHA256)?;
            set_progress(
                dir,
                "download",
                10,
                "SDK 2.4.0 já está em cache e confere com o SHA-256 fixado.",
            )?;
            return Ok(());
        }
        append_log(
            dir,
            &format!("SDK em cache não confere ({bytes} bytes); baixando novamente."),
        )?;
        fs::remove_file(&archive).map_err(|error| format!("{}: {error}", archive.display()))?;
    }
    let part = dir.join(format!("{SDK_ARCHIVE_FILE}.part"));
    let _ = fs::remove_file(&part);
    set_progress(
        dir,
        "download",
        2,
        "Baixando o SDK oficial da AMD (253 MB, fonte fixada)…",
    )?;
    let output = crate::update::curl()
        .args(["--max-time", "1800", "--retry", "2", "--retry-delay", "1"])
        // +1 byte so a too-large response is detectable before hashing.
        .args(["--max-filesize", &(SDK_BYTES + 1).to_string(), "--output"])
        .arg(&part)
        .args(["--url", SDK_URL])
        .output()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).".to_string())?;
    if !output.status.success() {
        let _ = fs::remove_file(&part);
        return Err(format!(
            "Download do SDK da AMD falhou: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    verify_and_publish(&part, &archive, SDK_BYTES, SDK_SHA256)?;
    write_marker(dir, SDK_SHA256)?;
    append_log(
        dir,
        &format!("SDK 2.4.0 verificado: {SDK_BYTES} bytes, sha256={SDK_SHA256}"),
    )?;
    set_progress(
        dir,
        "download",
        10,
        "SDK da AMD verificado (tamanho e SHA-256).",
    )?;
    Ok(())
}

#[cfg(any(target_os = "linux", test))]
fn write_marker(dir: &Path, sha256: &str) -> Result<()> {
    crate::fsr::write_atomic(
        &dir.join(format!("{SDK_ARCHIVE_FILE}.sha256")),
        format!("{sha256}\n").as_bytes(),
    )
}

/// Downloads (if needed) and verifies any pinned file, atomically. The marker
/// records the verified hash so status polling does not re-hash the archive.
#[cfg(any(target_os = "linux", test))]
fn ensure_pinned(
    dir: &Path,
    pinned: &PinnedFile,
    download: &mut dyn FnMut(&Path) -> Result<()>,
) -> Result<PathBuf> {
    fs::create_dir_all(dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    let target = dir.join(pinned.file);
    if pinned_valid(&target, pinned) {
        write_pinned_marker(dir, pinned)?;
        return Ok(target);
    }
    if target.exists() {
        fs::remove_file(&target).map_err(|error| format!("{}: {error}", target.display()))?;
    }
    let part = dir.join(format!("{}.part", pinned.file));
    let _ = fs::remove_file(&part);
    download(&part)?;
    verify_and_publish(&part, &target, pinned.bytes, pinned.sha256)?;
    write_pinned_marker(dir, pinned)?;
    Ok(target)
}

#[cfg(any(target_os = "linux", test))]
fn pinned_valid(path: &Path, pinned: &PinnedFile) -> bool {
    matches!(fs::metadata(path), Ok(meta) if meta.len() == pinned.bytes)
        && crate::fsr::sha256_file(path).is_ok_and(|hash| hash == pinned.sha256)
}

fn pinned_cached(dir: &Path, pinned: &PinnedFile) -> bool {
    let path = dir.join(pinned.file);
    if !matches!(fs::metadata(&path), Ok(meta) if meta.len() == pinned.bytes) {
        return false;
    }
    fs::read_to_string(dir.join(format!("{}.sha256", pinned.file)))
        .is_ok_and(|text| text.trim() == pinned.sha256)
}

#[cfg(any(target_os = "linux", test))]
fn write_pinned_marker(dir: &Path, pinned: &PinnedFile) -> Result<()> {
    crate::fsr::write_atomic(
        &dir.join(format!("{}.sha256", pinned.file)),
        format!("{}\n", pinned.sha256).as_bytes(),
    )
}

#[cfg(target_os = "linux")]
fn download_pinned(pinned: &PinnedFile, part: &Path, label: &str) -> Result<()> {
    let output = crate::update::curl()
        .args(["--max-time", "600", "--retry", "2", "--retry-delay", "1"])
        .args([
            "--max-filesize",
            &(pinned.bytes + 1).to_string(),
            "--output",
        ])
        .arg(part)
        .args(["--url", pinned.url])
        .output()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).".to_string())?;
    if !output.status.success() {
        let _ = fs::remove_file(part);
        return Err(format!(
            "Download de {label} falhou: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(())
}

/// One-click Linux installs no longer depend on a pre-installed DXC. The
/// archive and its licence texts are cached in `$XDG_DATA_HOME/mk8online/dxc`,
/// outside the FSR output; the build script receives the archive and its pins.
#[cfg(target_os = "linux")]
fn ensure_dxc(progress_dir: &Path) -> Result<PathBuf> {
    let dir = crate::fsr::dxc_dir();
    let _lock = cache_lock(&dir)?;
    fs::create_dir_all(&dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    if !pinned_cached(&dir, &DXC_PINNED) {
        set_progress(
            progress_dir,
            "dxc",
            2,
            "Baixando o compilador DXC (13 MB, fonte oficial fixada)…",
        )?;
    }
    let archive = ensure_pinned(&dir, &DXC_PINNED, &mut |part| {
        download_pinned(&DXC_PINNED, part, "DXC")
    })?;
    extract_dxc_licenses(&archive, &dir)?;
    append_log(
        progress_dir,
        &format!(
            "DXC {DXC_VERSION} verificado: {} bytes, sha256={}",
            DXC_PINNED.bytes, DXC_PINNED.sha256
        ),
    )?;
    set_progress(
        progress_dir,
        "dxc",
        18,
        &format!("DXC {DXC_VERSION} verificado e pronto."),
    )?;
    Ok(archive)
}

#[cfg(target_os = "linux")]
fn extract_dxc_licenses(archive: &Path, dir: &Path) -> Result<()> {
    use flate2::read::GzDecoder;
    use std::io::Read;
    use tar::Archive;
    let file =
        fs::File::open(archive).map_err(|error| format!("{}: {error}", archive.display()))?;
    let mut archive = Archive::new(GzDecoder::new(file));
    let mut found = [false; DXC_LICENSE_FILES.len()];
    let entries = archive
        .entries()
        .map_err(|error| format!("Arquivo do DXC inválido: {error}"))?;
    for entry in entries {
        let mut entry = entry.map_err(|error| error.to_string())?;
        let path = entry.path().map_err(|error| error.to_string())?;
        let Some(name) = path
            .file_name()
            .and_then(|name| name.to_str())
            .map(str::to_owned)
        else {
            continue;
        };
        let Some(index) = DXC_LICENSE_FILES.iter().position(|file| *file == name) else {
            continue;
        };
        let mut content = Vec::new();
        entry
            .read_to_end(&mut content)
            .map_err(|error| error.to_string())?;
        crate::fsr::write_atomic(&dir.join(&name), &content)?;
        found[index] = true;
    }
    if found.iter().all(|present| *present) {
        Ok(())
    } else {
        Err(format!(
            "O arquivo do DXC não contém as licenças esperadas ({}); o provedor não foi instalado.",
            DXC_LICENSE_FILES.join(", ")
        ))
    }
}

fn ensure_license(dir: &Path) -> Result<()> {
    if crate::fsr::license_status(dir)["verified"] == true {
        return Ok(());
    }
    fetch_license_locked(dir)?;
    Ok(())
}

/// Fetches AMD's license from the pinned tag, verifies the pinned size and
/// SHA-256, and saves it beside the SDK archive. The acceptance step never
/// fetches; it only accepts an already verified file.
pub fn fetch_license(dir: &Path) -> Result<String> {
    let _lock = cache_lock(dir)?;
    fetch_license_locked(dir)
}

fn fetch_license_locked(dir: &Path) -> Result<String> {
    let target = crate::fsr::license_path(dir);
    if crate::fsr::license_status(dir)["verified"] == true {
        return fs::read_to_string(&target)
            .map_err(|error| format!("{}: {error}", target.display()));
    }
    let part = dir.join(format!("{LICENSE_FILE}.part"));
    let _ = fs::remove_file(&part);
    let output = crate::update::curl()
        .args(["--max-time", "60", "--retry", "2", "--retry-delay", "1"])
        // +1 byte so an oversized response is rejected instead of truncated.
        .args([
            "--max-filesize",
            &(LICENSE_BYTES + 1).to_string(),
            "--output",
        ])
        .arg(&part)
        .args(["--url", LICENSE_URL])
        .output()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).".to_string())?;
    if !output.status.success() {
        let _ = fs::remove_file(&part);
        return Err(format!(
            "Não foi possível baixar a licença da AMD: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let bytes = fs::read(&part).map_err(|error| format!("{}: {error}", part.display()))?;
    let result = verify_bytes(&bytes, LICENSE_BYTES, LICENSE_SHA256, "Licença da AMD");
    if let Err(error) = result {
        let _ = fs::remove_file(&part);
        return Err(error);
    }
    fs::rename(&part, &target).map_err(|error| format!("{}: {error}", target.display()))?;
    append_log(
        dir,
        &format!("licença da AMD verificada: {LICENSE_BYTES} bytes, sha256={LICENSE_SHA256}"),
    )?;
    String::from_utf8(bytes).map_err(|_| "A licença da AMD não está em UTF-8.".into())
}

/// Explicit acceptance of the text the page displayed. Requires the verified
/// file, so accepting without the fetched official text is impossible.
pub fn accept_terms(dir: &Path) -> Result<()> {
    let _lock = cache_lock(dir)?;
    if crate::fsr::license_status(dir)["verified"] != true {
        return Err(
            "Leia a licença da AMD (texto oficial da tag v2.4.0) antes de aceitar os termos."
                .into(),
        );
    }
    let accepted_at = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default();
    let bytes = serde_json::to_vec_pretty(&json!({
        "sdk": SDK_TAG,
        "license_sha256": LICENSE_SHA256,
        "license_bytes": LICENSE_BYTES,
        "accepted_at_unix": accepted_at,
    }))
    .map_err(|error| error.to_string())?;
    crate::fsr::write_atomic(&dir.join(TERMS_FILE), &bytes)
}

/// Rejects a fake download before anything is published. The error names both
/// expected and received size/hash; the partial file is removed.
pub fn verify_and_publish(
    part: &Path,
    target: &Path,
    expected_bytes: u64,
    expected_sha256: &str,
) -> Result<()> {
    let bytes = fs::metadata(part)
        .map_err(|error| format!("{}: {error}", part.display()))?
        .len();
    if bytes != expected_bytes {
        let _ = fs::remove_file(part);
        return Err(format!(
            "Tamanho do download não confere: esperado {expected_bytes} bytes, recebido {bytes}. Nada foi instalado."
        ));
    }
    let actual = crate::fsr::sha256_file(part)?;
    if actual != expected_sha256 {
        let _ = fs::remove_file(part);
        return Err(format!(
            "SHA-256 do download não confere: esperado {expected_sha256}, recebido {actual}. Nada foi instalado."
        ));
    }
    if let Some(parent) = target.parent() {
        fs::create_dir_all(parent).map_err(|error| format!("{}: {error}", parent.display()))?;
    }
    fs::rename(part, target).map_err(|error| format!("{}: {error}", target.display()))
}

/// Pure license check used before saving the fetched text.
pub fn verify_bytes(
    bytes: &[u8],
    expected_bytes: u64,
    expected_sha256: &str,
    what: &str,
) -> Result<()> {
    if bytes.len() as u64 != expected_bytes {
        return Err(format!(
            "{what} com tamanho inesperado: esperado {expected_bytes} bytes, recebido {}. Nada foi salvo.",
            bytes.len()
        ));
    }
    let actual = crate::fsr::sha256_bytes(bytes);
    if actual != expected_sha256 {
        return Err(format!(
            "{what} com SHA-256 inesperado: esperado {expected_sha256}, recebido {actual}. Nada foi salvo."
        ));
    }
    Ok(())
}

pub fn sdk_cached(dir: &Path) -> bool {
    let archive = dir.join(SDK_ARCHIVE_FILE);
    if !matches!(fs::metadata(&archive), Ok(meta) if meta.len() == SDK_BYTES) {
        return false;
    }
    fs::read_to_string(dir.join(format!("{SDK_ARCHIVE_FILE}.sha256")))
        .is_ok_and(|text| text.trim() == SDK_SHA256)
}

pub fn read_installed(dir: &Path) -> Option<Installed> {
    let text = fs::read_to_string(dir.join(INSTALL_FILE)).ok()?;
    serde_json::from_str(&text).ok()
}

pub fn read_progress(dir: &Path) -> Progress {
    fs::read_to_string(dir.join(PROGRESS_FILE))
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_else(|| Progress {
            phase: "idle".into(),
            percent: 0,
            detail: String::new(),
        })
}

#[cfg(target_os = "linux")]
fn progress(dir: &Path) -> Progress {
    read_progress(dir)
}

fn set_progress(dir: &Path, phase: &str, percent: u8, detail: &str) -> Result<()> {
    let bytes = serde_json::to_vec_pretty(&Progress {
        phase: phase.into(),
        percent,
        detail: detail.into(),
    })
    .map_err(|error| error.to_string())?;
    crate::fsr::write_atomic(&dir.join(PROGRESS_FILE), &bytes)
}

fn record_interrupted_job(dir: &Path, id: &str, detail: &str) {
    if fs::read_to_string(dir.join("job-id")).is_ok_and(|current| current == id)
        && !matches!(
            read_progress(dir).phase.as_str(),
            "done" | "ready" | "failed"
        )
    {
        let _ = record_failure(dir, detail);
    }
}

fn record_failure(dir: &Path, detail: &str) -> Result<()> {
    set_progress(dir, "failed", read_progress(dir).percent, detail)
}

fn record_installed(
    dir: &Path,
    version: &str,
    upscaler: Option<&str>,
    framegeneration: Option<&str>,
    sha256: &str,
    bytes: u64,
) -> Result<()> {
    let installed = Installed {
        state: "ready".into(),
        detail: format!("Provedor FSR {version} pronto."),
        version: Some(version.into()),
        upscaler_version: upscaler.map(str::to_owned),
        framegeneration_version: framegeneration.map(str::to_owned),
        provider_bytes: bytes,
        provider_sha256: sha256.into(),
        sdk_sha256: SDK_SHA256.into(),
        built_at: unix_now(),
    };
    crate::fsr::write_atomic(
        &dir.join(INSTALL_FILE),
        &serde_json::to_vec_pretty(&installed).map_err(|error| error.to_string())?,
    )
}

fn unix_now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_secs())
        .unwrap_or_default()
}

fn append_log(dir: &Path, line: &str) -> Result<()> {
    fs::create_dir_all(dir).map_err(|error| format!("{}: {error}", dir.display()))?;
    let mut file = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(dir.join(BUILD_LOG))
        .map_err(|error| error.to_string())?;
    writeln!(file, "{line}").map_err(|error| error.to_string())
}

pub fn log_tail(dir: &Path, lines: usize) -> String {
    let bytes = fs::read(dir.join(BUILD_LOG)).unwrap_or_default();
    let start = bytes.len().saturating_sub(256 * 1024);
    let text = String::from_utf8_lossy(&bytes[start..]);
    let tail: Vec<&str> = text.lines().rev().take(lines).collect();
    tail.into_iter().rev().collect::<Vec<_>>().join("\n")
}

#[cfg(target_os = "linux")]
fn progress_marker(line: &str, current: u8) -> Option<(u8, String)> {
    let line = line.trim();
    if line.starts_with("sdk archive ok") {
        Some((20, "SDK da AMD verificado; aplicando ajustes Linux…".into()))
    } else if line.starts_with("sdk tree:") {
        Some((30, "Fontes do SDK preparadas; compilando shaders…".into()))
    } else if line.starts_with("+ cmake -S") {
        Some((40, "Configurando a build do provedor…".into()))
    } else if line.starts_with("+ cmake --build") {
        Some((55, "Compilando o provedor FSR…".into()))
    } else if let Some(rest) = line.strip_prefix('[') {
        let (done, rest) = rest.split_once('/')?;
        let (total, _) = rest.split_once(']')?;
        let done: u32 = done.trim().parse().ok()?;
        let total: u32 = total.trim().parse().ok()?;
        (total > 0).then(|| {
            (
                (55 + (done.saturating_mul(35) / total) as u8).min(90),
                format!("Compilando o provedor FSR… {done}/{total}"),
            )
        })
    } else if line.starts_with("installed ") {
        Some((95, "Provedor compilado; verificando a versão…".into()))
    } else if line.starts_with("error:") {
        Some((current, line.into()))
    } else {
        None
    }
}

#[cfg(target_os = "linux")]
fn python(root: &Path) -> PathBuf {
    if let Some(path) = std::env::var_os("MK8_PYTHON312").filter(|value| !value.is_empty()) {
        return path.into();
    }
    let local = root.join(if cfg!(windows) {
        "local/python/Scripts/python.exe"
    } else {
        "local/python/bin/python"
    });
    if local.is_file() {
        return local;
    }
    PathBuf::from(if cfg!(windows) { "python" } else { "python3" })
}

pub struct Dxc {
    pub path: PathBuf,
    pub found: bool,
    pub source: &'static str,
    /// True when `path` is the pinned release archive rather than a binary.
    pub archive: bool,
}

/// Configured path first, then the launcher tool folders and `PATH`, then the
/// pinned DXC archive cache. A configured path that does not exist is reported
/// as missing instead of silently falling back to the automatic download.
pub fn resolve_dxc(root: &Path, dir: &Path, configured: &str) -> Dxc {
    resolve_dxc_in(root, dir, configured, &crate::fsr::dxc_dir())
}

fn resolve_dxc_in(root: &Path, dir: &Path, configured: &str, dxc_cache: &Path) -> Dxc {
    let names: &[&str] = if cfg!(windows) {
        &["dxc.exe", "dxc"]
    } else {
        &["dxc"]
    };
    if !configured.is_empty() {
        let path = PathBuf::from(configured);
        let found = path.is_file();
        return Dxc {
            found,
            path,
            source: if found {
                "configured"
            } else {
                "configured-missing"
            },
            archive: false,
        };
    }
    let mut candidates: Vec<(PathBuf, &'static str)> = Vec::new();
    if let Some(path) = std::env::var_os("MK8_DXC").filter(|value| !value.is_empty()) {
        candidates.push((PathBuf::from(path), "MK8_DXC"));
    }
    for base in [
        root.join("local/native-tools/dxc/bin"),
        root.join("local/native-tools/bin"),
        root.join("tools/dxc/bin"),
        dir.join("dxc/bin"),
    ] {
        for name in names {
            candidates.push((base.join(name), "tools"));
        }
    }
    if let Some(paths) = std::env::var_os("PATH") {
        for base in std::env::split_paths(&paths) {
            for name in names {
                candidates.push((base.join(name), "PATH"));
            }
        }
    }
    for (path, source) in candidates {
        if path.is_file() {
            return Dxc {
                found: true,
                path,
                source,
                archive: false,
            };
        }
    }
    let archive = dxc_cache.join(DXC_PINNED.file);
    if pinned_cached(dxc_cache, &DXC_PINNED) {
        return Dxc {
            found: true,
            path: archive,
            source: "archive",
            archive: true,
        };
    }
    Dxc {
        path: archive,
        found: false,
        source: "pinned",
        archive: true,
    }
}

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

/// Reads the version the built library reports through
/// `ffxQuery(FFX_API_QUERY_DESC_TYPE_GET_VERSIONS)`, not a sidecar file.
#[cfg(target_os = "linux")]
pub fn provider_version(path: &Path) -> Result<String> {
    provider_version_for(path, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK)
}

pub fn provider_version_for(path: &Path, create_desc_type: u64) -> Result<String> {
    provider_versions_for(path, create_desc_type)?
        .into_iter()
        .next()
        .ok_or_else(|| "O provedor compilado não informou o nome da versão.".into())
}

/// All versions a provider advertises for one FFX effect descriptor. Windows
/// uses this to distinguish AMD's 3.1.6/2.3.4 DLLs from a provider that really
/// exposes 4.1.2.
pub fn provider_versions_for(path: &Path, create_desc_type: u64) -> Result<Vec<String>> {
    unsafe {
        let library = libloading::Library::new(path).map_err(|error| {
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
                type_: FFX_API_QUERY_DESC_TYPE_GET_VERSIONS,
                next: std::ptr::null_mut(),
            },
            create_desc_type,
            device: std::ptr::null_mut(),
            output_count: &mut count,
            version_ids: std::ptr::null_mut(),
            version_names: std::ptr::null_mut(),
        };
        let rc = query(std::ptr::null_mut(), &mut desc.header);
        if rc != FFX_API_RETURN_OK {
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
        if rc != FFX_API_RETURN_OK {
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

/// The status object shown by the FSR page. `running` means the launcher's job
/// child is still alive.
pub fn status(dir: &Path, settings: &crate::fsr::Settings, root: &Path, running: bool) -> Value {
    let installed = read_installed(dir);
    let progress = read_progress(dir);
    let windows = cfg!(windows);
    let selected_upscaler = if settings.upscaling == FSR4_VERSION {
        FSR4_VERSION
    } else {
        UPSCALER_VERSION
    };
    let upscaler = crate::fsr::upscaler_for(dir, selected_upscaler);
    let framegeneration = crate::fsr::framegeneration_lib(dir);
    let provider = framegeneration.clone();
    let needed: &[&Path] = if windows {
        &[&upscaler, &framegeneration]
    } else {
        &[&provider]
    };
    let provider_present = needed.iter().all(|path| path.is_file());
    let provider_bytes = fs::metadata(&provider)
        .map(|meta| meta.len())
        .unwrap_or_default();
    let dxc = resolve_dxc(root, dir, &settings.dxc);
    let dxc_cache = crate::fsr::dxc_dir();
    let dxc_licenses: Vec<PathBuf> = DXC_LICENSE_FILES
        .iter()
        .map(|name| dxc_cache.join(name))
        .collect();
    let script = root.join(BUILD_SCRIPT);
    let terms = crate::fsr::terms_accepted(dir);
    let cached = sdk_cached(dir);
    let license = crate::fsr::license_status(dir);
    let up_requested = !windows && matches!(settings.upscaling.as_str(), "3.1.6" | "4.1.2");
    let fg_requested = settings.frame_generation == "fsr3";
    let upscaler_state = read_upscaler_state(dir, selected_upscaler);
    let upscaler_bytes = fs::metadata(&upscaler)
        .map(|meta| meta.len())
        .unwrap_or_default();
    let upscaler_version = if windows {
        installed.as_ref().and_then(|i| i.upscaler_version.clone())
    } else {
        upscaler_state
            .as_ref()
            .filter(|state| {
                state.version == selected_upscaler
                    && (state.bytes == 0 || state.bytes == upscaler_bytes)
                    && crate::fsr::sha256_file(&upscaler)
                        .is_ok_and(|hash| state.sha256.is_empty() || hash == state.sha256)
            })
            .map(|state| state.version.clone())
    };
    let upscaler_ready = if windows {
        upscaler.is_file()
    } else {
        let fsr4_files_ok = selected_upscaler != FSR4_VERSION
            || (upscaler.parent().is_some_and(fsr4_licenses_present)
                && fsr4_dispatch_supported(&upscaler));
        upscaler.is_file()
            && upscaler_version.as_deref() == Some(selected_upscaler)
            && fsr4_files_ok
    };
    let mut state = "not-installed";
    let mut detail = String::from("Nada instalado. Leia e aceite a licença da AMD para começar.");
    if running {
        state = "building";
        detail = if progress.detail.is_empty() {
            "Instalando…".into()
        } else {
            progress.detail.clone()
        };
    } else if let Some(installed) = &installed {
        match installed.state.as_str() {
            "ready"
                if provider_present
                    && (installed.provider_bytes == 0
                        || installed.provider_bytes == provider_bytes) =>
            {
                if windows {
                    state = "ready";
                    detail = format!(
                        "DLLs assinadas da AMD prontas: upscaling {}, frame generation {}.",
                        installed
                            .upscaler_version
                            .as_deref()
                            .unwrap_or("não informada"),
                        installed
                            .framegeneration_version
                            .as_deref()
                            .unwrap_or("não informada")
                    );
                } else if up_requested && !upscaler_ready {
                    state = "failed";
                    detail = format!(
                        "O provedor FSR {selected_upscaler} não está instalado; execute a instalação novamente."
                    );
                } else if up_requested {
                    state = "ready";
                    detail = if fg_requested {
                        format!(
                            "Provedores FSR {selected_upscaler} (upscaling) e frame generation {} prontos.",
                            installed.version.as_deref().unwrap_or("3.1.7")
                        )
                    } else {
                        format!(
                            "Provedor FSR {selected_upscaler} pronto ({}).",
                            upscaler.display()
                        )
                    };
                } else {
                    state = "ready";
                    detail = format!(
                        "Provedor FSR {} pronto ({}).",
                        installed.version.as_deref().unwrap_or("3.1.7"),
                        provider.display()
                    );
                }
            }
            "ready" => {
                state = "failed";
                detail = format!(
                    "Os arquivos registrados não estão mais em {}; instale novamente.",
                    dir.display()
                );
            }
            _ => (),
        }
    }
    if state != "ready" && !running && up_requested && upscaler_ready && !fg_requested {
        state = "ready";
        detail = format!(
            "Provedor FSR {selected_upscaler} pronto ({}).",
            upscaler.display()
        );
    }
    if state != "ready" && !running && progress.phase == "failed" {
        state = "failed";
        detail = progress.detail.clone();
    }
    if state == "not-installed" && terms && cached {
        state = "sdk-cached";
        detail = "Termos aceitos e SDK 2.4.0 conferido em cache. Pronto para compilar.".into();
    }
    if state == "not-installed" && terms {
        state = "terms-accepted";
        detail = if cfg!(windows) {
            "Termos aceitos. As DLLs assinadas da AMD ainda serão baixadas.".into()
        } else {
            "Termos aceitos. O SDK ainda será baixado e compilado.".into()
        };
    }
    let signed: Vec<Value> = SIGNED_DLLS
        .iter()
        .map(|dll| {
            let path = dir.join(dll.name);
            json!({
                "name": dll.name,
                "path": path,
                "present": path.is_file(),
                "bytes": dll.bytes,
                "sha256": dll.sha256,
                "url": dll.url,
            })
        })
        .collect();
    json!({
        "state": state,
        "detail": detail,
        "version": installed
            .as_ref()
            .and_then(|i| i.version.clone())
            .or_else(|| upscaler_ready.then(|| selected_upscaler.to_string())),
        "provider": {
            "path": provider,
            "present": provider_present,
            "bytes": provider_bytes,
            "sha256": installed.as_ref().map(|i| i.provider_sha256.clone()).unwrap_or_default(),
        },
        "upscaler": {
            "path": upscaler,
            "present": upscaler.is_file(),
            "version": upscaler_version,
            "ready": upscaler_ready,
            "bytes": upscaler_bytes,
            "sha256": upscaler_state.as_ref().map(|s| s.sha256.clone()).unwrap_or_default(),
        },
        "framegeneration": {
            "path": framegeneration,
            "present": framegeneration.is_file(),
            "version": installed.as_ref().and_then(|i| i.framegeneration_version.clone()),
        },
        "signed_dlls": signed,
        "sdk": {
            "cached": cached,
            "archive": dir.join(SDK_ARCHIVE_FILE),
            "bytes": SDK_BYTES,
            "sha256": SDK_SHA256,
            "url": SDK_URL,
        },
        "license": license,
        "terms_accepted": terms,
        "progress": progress,
        "dxc": {
            "path": dxc.path,
            "found": dxc.found,
            "source": dxc.source,
            "archive": dxc.archive,
            "version": DXC_VERSION,
            "pinned": {
                "file": DXC_PINNED.file,
                "url": DXC_PINNED.url,
                "bytes": DXC_PINNED.bytes,
                "sha256": DXC_PINNED.sha256,
                "dir": dxc_cache,
                "cached": pinned_cached(&dxc_cache, &DXC_PINNED),
                "licenses": dxc_licenses,
            },
        },
        "build_script": {"path": script, "present": script.is_file()},
        "install_dir": dir,
        "sdk_tag": SDK_TAG,
        "platform": if cfg!(windows) { "windows" } else { "linux" },
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static TEMP: AtomicU64 = AtomicU64::new(0);

    fn temp(name: &str) -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "mk8-fsr-install-{name}-{}-{}",
            std::process::id(),
            TEMP.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn install_lock_and_stale_failures_preserve_the_verified_receipt() {
        let dir = temp("install-retry");
        let lock = cache_lock(&dir).unwrap();
        assert!(cache_lock(&dir).is_err());
        fs::write(dir.join("job-id"), "failed-attempt").unwrap();
        set_progress(&dir, "build", 20, "building").unwrap();
        record_interrupted_job(&dir, "failed-attempt", "interrupted");
        assert_eq!(read_progress(&dir).phase, "failed");
        assert!(read_installed(&dir).is_none());
        drop(lock);

        let retry_lock = cache_lock(&dir).unwrap();
        fs::write(dir.join("job-id"), "retry").unwrap();
        set_progress(&dir, "build", 70, "building").unwrap();
        record_interrupted_job(&dir, "failed-attempt", "stale");
        assert_eq!(read_progress(&dir).percent, 70);
        assert_ne!(read_progress(&dir).phase, "failed");
        record_installed(&dir, "3.1.7", None, Some("3.1.7"), "hash", 123).unwrap();
        record_failure(&dir, "later attempt failed").unwrap();
        assert_eq!(read_installed(&dir).unwrap().provider_bytes, 123);
        drop(retry_lock);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn fake_download_size_and_hash_mismatches_are_never_published() {
        let dir = temp("download");
        let part = dir.join("sdk.part");
        let target = dir.join("sdk.tar.gz");
        let fake = b"not the official SDK";
        fs::write(&part, fake).unwrap();

        let error = verify_and_publish(&part, &target, 1234, SDK_SHA256).unwrap_err();
        assert!(error.contains("1234") && error.contains(&fake.len().to_string()));
        assert!(!target.exists(), "a mismatched size must not be installed");
        assert!(
            !part.exists(),
            "the rejected partial download must be removed"
        );

        fs::write(&part, fake).unwrap();
        let wrong = "0".repeat(64);
        let error = verify_and_publish(&part, &target, fake.len() as u64, &wrong).unwrap_err();
        assert!(error.contains(&wrong) && error.contains("SHA-256"));
        assert!(!target.exists(), "a mismatched hash must not be installed");
        assert!(!part.exists());

        fs::write(&part, fake).unwrap();
        let hash = crate::fsr::sha256_bytes(fake);
        verify_and_publish(&part, &target, fake.len() as u64, &hash).unwrap();
        assert_eq!(fs::read(&target).unwrap(), fake);
        assert!(!part.exists());
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn license_bytes_must_match_the_pinned_size_and_hash() {
        let fake = vec![b'x'; LICENSE_BYTES as usize];
        let error =
            verify_bytes(&fake, LICENSE_BYTES, LICENSE_SHA256, "Licença da AMD").unwrap_err();
        assert!(error.contains("SHA-256") && error.contains(LICENSE_SHA256));
        let error =
            verify_bytes(b"short", LICENSE_BYTES, LICENSE_SHA256, "Licença da AMD").unwrap_err();
        assert!(error.contains(&LICENSE_BYTES.to_string()) && error.contains('5'));
    }

    #[test]
    fn accepting_terms_requires_the_verified_license_text() {
        let dir = temp("terms");
        assert!(accept_terms(&dir).is_err());
        assert!(!dir.join(TERMS_FILE).exists());
        // Same size, wrong content: still rejected.
        fs::write(
            crate::fsr::license_path(&dir),
            vec![b'x'; LICENSE_BYTES as usize],
        )
        .unwrap();
        assert!(accept_terms(&dir).is_err());
        assert!(!dir.join(TERMS_FILE).exists());
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn progress_markers_follow_cmake_without_exceeding_the_cap() {
        assert_eq!(progress_marker("sdk archive ok: 1 bytes", 0).unwrap().0, 20);
        assert_eq!(
            progress_marker("[10/100] Building CXX object", 15)
                .unwrap()
                .0,
            58
        );
        assert_eq!(progress_marker("[100/100] Linking", 15).unwrap().0, 90);
        assert_eq!(
            progress_marker("installed /x/lib.so (1 bytes)", 90)
                .unwrap()
                .0,
            95
        );
        assert!(progress_marker("unrelated", 0).is_none());
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn publishing_the_fg_library_preserves_an_installed_upscaler() {
        let dir = temp("provider-merge");
        let provider = dir.join(PROVIDER_DIR);
        fs::create_dir_all(&provider).unwrap();
        let upscaler = crate::fsr::upscaler_lib(&dir);
        fs::write(&upscaler, b"upscaler").unwrap();
        let next = dir.join("provider.next");
        fs::create_dir_all(&next).unwrap();
        let fg_name = Path::new(PROVIDER_REL).file_name().unwrap();
        fs::write(next.join(fg_name), b"framegeneration").unwrap();
        fs::write(next.join(LICENSE_FILE), b"license").unwrap();
        publish_provider(&dir, &next).unwrap();
        assert_eq!(
            fs::read(crate::fsr::provider_path(&dir)).unwrap(),
            b"framegeneration".to_vec()
        );
        assert_eq!(fs::read(&upscaler).unwrap(), b"upscaler".to_vec());
        assert_eq!(
            fs::read(provider.join(LICENSE_FILE)).unwrap(),
            b"license".to_vec()
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn upscaler_progress_markers_stay_in_the_second_build_range() {
        assert_eq!(
            upscaler_progress_marker("[10/100] Building", 72).unwrap().0,
            86
        );
        assert_eq!(
            upscaler_progress_marker("installed /x/lib.so (1 bytes)", 90)
                .unwrap()
                .0,
            98
        );
        assert!(upscaler_progress_marker("unrelated", 72).is_none());
    }

    #[test]
    fn configured_dxc_is_reported_even_when_missing() {
        let dir = temp("dxc");
        let missing = dir.join("dxc");
        let result = resolve_dxc_in(&dir, &dir, missing.to_str().unwrap(), &dir.join("cache"));
        assert!(!result.found);
        assert_eq!(result.path, missing);
        assert_eq!(result.source, "configured-missing");
        fs::write(&missing, b"fake").unwrap();
        let result = resolve_dxc_in(&dir, &dir, missing.to_str().unwrap(), &dir.join("cache"));
        assert!(result.found);
        assert_eq!(result.source, "configured");
        assert!(!result.archive);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn dxc_archive_size_and_hash_mismatches_are_rejected() {
        let dir = temp("dxc-verify");
        let part = dir.join("dxc.part");
        let target = dir.join(DXC_PINNED.file);
        let fake = b"not the pinned DXC release";
        fs::write(&part, fake).unwrap();
        let error =
            verify_and_publish(&part, &target, DXC_PINNED.bytes, DXC_PINNED.sha256).unwrap_err();
        assert!(error.contains(&DXC_PINNED.bytes.to_string()));
        assert!(error.contains(&fake.len().to_string()));
        assert!(!target.exists());
        assert!(!part.exists());

        fs::write(&part, fake).unwrap();
        let wrong = "0".repeat(64);
        let error = verify_and_publish(&part, &target, fake.len() as u64, &wrong).unwrap_err();
        assert!(error.contains("SHA-256") && error.contains(&wrong));
        assert!(!target.exists());
        assert!(!part.exists());
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn pinned_dxc_archive_is_reused_without_download_and_revalidated_after_tamper() {
        const BYTES: &[u8] = b"fake dxc release";
        const SHA: &str = "4f10dfd9fb82858c159b00434c5412bddf6dfebd8967294d37c491029935e18a";
        let pinned = PinnedFile {
            file: "dxc-test.tar.gz",
            url: "https://example.invalid/dxc",
            bytes: BYTES.len() as u64,
            sha256: SHA,
        };
        let dir = temp("dxc-reuse");
        let downloads = std::rc::Rc::new(std::cell::Cell::new(0));
        let counter = downloads.clone();
        let mut download = move |part: &Path| {
            counter.set(counter.get() + 1);
            fs::write(part, BYTES).map_err(|error| error.to_string())
        };
        let archive = ensure_pinned(&dir, &pinned, &mut download).unwrap();
        assert_eq!(downloads.get(), 1);
        assert_eq!(fs::read(&archive).unwrap(), BYTES);
        assert!(dir.join(format!("{}.sha256", pinned.file)).is_file());

        // A cached, verified archive is reused.
        let again = ensure_pinned(&dir, &pinned, &mut download).unwrap();
        assert_eq!(again, archive);
        assert_eq!(downloads.get(), 1);

        // A same-size tamper is detected and re-downloaded.
        fs::write(&archive, b"tampered dxc rel").unwrap();
        let restored = ensure_pinned(&dir, &pinned, &mut download).unwrap();
        assert_eq!(downloads.get(), 2);
        assert_eq!(fs::read(restored).unwrap(), BYTES);
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn dxc_license_texts_are_extracted_from_the_verified_archive() {
        use flate2::{write::GzEncoder, Compression};
        let dir = temp("dxc-licenses");
        let archive = dir.join("dxc.tar.gz");
        {
            let file = fs::File::create(&archive).unwrap();
            let encoder = GzEncoder::new(file, Compression::default());
            let mut builder = tar::Builder::new(encoder);
            for (name, body) in [
                ("LICENSE-LLVM.txt", b"LLVM license text".as_slice()),
                ("LICENSE-MS.txt", b"Microsoft license text".as_slice()),
            ] {
                let mut header = tar::Header::new_gnu();
                header.set_size(body.len() as u64);
                header.set_mode(0o644);
                header.set_cksum();
                builder.append_data(&mut header, name, body).unwrap();
            }
            builder.finish().unwrap();
        }
        extract_dxc_licenses(&archive, &dir).unwrap();
        assert_eq!(
            fs::read_to_string(dir.join("LICENSE-LLVM.txt")).unwrap(),
            "LLVM license text"
        );
        assert_eq!(
            fs::read_to_string(dir.join("LICENSE-MS.txt")).unwrap(),
            "Microsoft license text"
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn signed_dll_pins_cover_upscaling_and_frame_generation() {
        let find = |name: &str| {
            SIGNED_DLLS
                .iter()
                .find(|dll| dll.name == name)
                .unwrap_or_else(|| panic!("missing pin for {name}"))
        };
        for name in [
            "amd_fidelityfx_loader_vk.dll",
            "amd_fidelityfx_upscaler_vk.dll",
            "amd_fidelityfx_framegeneration_vk.dll",
        ] {
            let dll = find(name);
            assert_eq!(dll.sha256.len(), 64, "{name}");
            assert!(dll.sha256.chars().all(|c| c.is_ascii_hexdigit()), "{name}");
            assert!(dll.bytes > 0, "{name}");
            assert!(
                dll.url.starts_with(
                    "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/"
                ),
                "{name}: {}",
                dll.url
            );
        }
        assert_eq!(find("amd_fidelityfx_upscaler_vk.dll").bytes, 36_135_696);
        assert_eq!(
            find("amd_fidelityfx_framegeneration_vk.dll").bytes,
            36_806_928
        );
    }

    #[cfg(target_os = "linux")]
    fn fake_ffx_library(dir: &Path, version: &str) -> Option<PathBuf> {
        let source = dir.join("fake.c");
        let library = dir.join("libfake_ffx.so");
        fs::write(
            &source,
            format!(
                r#"
#include <stdint.h>
typedef struct {{ uint64_t type; void* next; }} Header;
typedef struct {{
    Header header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
}} Query;
static const char* name = "{version}";
uint32_t ffxQuery(void* context, Header* header) {{
    (void)context;
    if (!header || header->type != 4) return 1;
    Query* query = (Query*)header;
    if (!query->outputCount) return 1;
    if (*query->outputCount == 0) {{ *query->outputCount = 1; return 0; }}
    if (query->versionIds) query->versionIds[0] = 0x1234;
    if (query->versionNames) query->versionNames[0] = name;
    *query->outputCount = 1;
    return 0;
}}
"#
            ),
        )
        .unwrap();
        let compiled = Command::new("cc")
            .arg("-shared")
            .arg("-fPIC")
            .arg("-o")
            .arg(&library)
            .arg(&source)
            .status();
        match compiled {
            Ok(status) if status.success() => Some(library),
            _ => {
                // The default test suite still covers the rest without a C compiler.
                None
            }
        }
    }

    #[cfg(target_os = "linux")]
    fn fake_ffx_library_versions(dir: &Path, versions: &[&str]) -> Option<PathBuf> {
        let source = dir.join("fake-versions.c");
        let library = dir.join("libfake_ffx_versions.so");
        let names = versions
            .iter()
            .map(|version| format!("\"{version}\""))
            .collect::<Vec<_>>()
            .join(", ");
        fs::write(
            &source,
            format!(
                r#"
#include <stdint.h>
typedef struct {{ uint64_t type; void* next; }} Header;
typedef struct {{
    Header header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
}} Query;
static const char* names[] = {{ {names} }};
static const uint32_t total = {total};
uint32_t ffxQuery(void* context, Header* header) {{
    (void)context;
    if (!header || header->type != 4) return 1;
    Query* query = (Query*)header;
    if (!query->outputCount) return 1;
    if (*query->outputCount == 0) {{ *query->outputCount = total; return 0; }}
    uint32_t limit = *query->outputCount;
    for (uint32_t i = 0; i < total && i < limit; i++) {{
        if (query->versionIds) query->versionIds[i] = 0x1000 + i;
        if (query->versionNames) query->versionNames[i] = names[i];
    }}
    *query->outputCount = total < limit ? total : limit;
    return 0;
}}
"#,
                total = versions.len()
            ),
        )
        .unwrap();
        let compiled = Command::new("cc")
            .arg("-shared")
            .arg("-fPIC")
            .arg("-o")
            .arg(&library)
            .arg(&source)
            .status();
        match compiled {
            Ok(status) if status.success() => Some(library),
            _ => None,
        }
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn provider_version_lists_expose_every_offered_version() {
        let dir = temp("ffx-versions");
        let Some(library) = fake_ffx_library_versions(&dir, &["3.1.6", "2.3.4"]) else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        assert_eq!(
            provider_versions_for(&library, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE).unwrap(),
            vec!["3.1.6".to_string(), "2.3.4".to_string()]
        );
        assert_eq!(
            provider_version_for(&library, FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE).unwrap(),
            "3.1.6"
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn provider_version_is_read_from_a_real_ffx_query() {
        let dir = temp("ffx-query");
        let Some(library) = fake_ffx_library(&dir, "9.9.9") else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        assert_eq!(
            provider_version_for(&library, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK)
                .unwrap(),
            "9.9.9"
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn fsr4_reuse_requires_4_1_2_and_rejects_3_1_6() {
        let dir = temp("fsr4-version");
        let provider = crate::fsr::upscaler_for(&dir, FSR4_VERSION);
        fs::create_dir_all(provider.parent().unwrap()).unwrap();
        let Some(library) = fake_ffx_library(&dir, "3.1.6") else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        fs::copy(&library, &provider).unwrap();
        assert_eq!(
            installed_upscaler_version(&dir, FSR4_VERSION).as_deref(),
            Some("3.1.6")
        );
        assert!(!upscaler_ready(&dir, FSR4_VERSION));
        assert!(!upscaler_version_ok("3.1.6", FSR4_VERSION));

        let Some(library) = fake_ffx_library(&dir, "4.1.2") else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        fs::copy(&library, &provider).unwrap();
        let provider_dir = provider.parent().unwrap();
        fs::write(provider_dir.join("LICENSE-GPL-3.0.txt"), b"GPL").unwrap();
        fs::write(provider_dir.join("LICENSE-AMD-FidelityFX-SDK.md"), b"AMD").unwrap();
        fs::write(provider_dir.join("SOURCE-OFFER.md"), b"offer").unwrap();
        fs::create_dir_all(provider_dir.join("fsr4-assets/models")).unwrap();
        fs::create_dir_all(provider_dir.join("fsr4-assets/shaders")).unwrap();
        assert_eq!(
            installed_upscaler_version(&dir, FSR4_VERSION).as_deref(),
            Some(FSR4_VERSION)
        );
        assert!(
            !upscaler_ready(&dir, FSR4_VERSION),
            "old 4.1.2 lacks the private dispatch export"
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn fsr4_publication_keeps_gpl_amd_licenses_assets_and_the_316_provider() {
        let dir = temp("fsr4-publish");
        let provider = dir.join(PROVIDER_DIR);
        fs::create_dir_all(&provider).unwrap();
        let upscaler_316 = crate::fsr::upscaler_lib(&dir);
        fs::write(&upscaler_316, b"3.1.6 provider").unwrap();

        let next = dir.join("fsr4.next");
        fs::create_dir_all(next.join("fsr4-assets/models")).unwrap();
        fs::create_dir_all(next.join("fsr4-assets/shaders")).unwrap();
        fs::write(next.join(UPSCALER_FILE), b"4.1.2 provider").unwrap();
        fs::write(next.join("LICENSE-GPL-3.0.txt"), b"GPL text").unwrap();
        fs::write(next.join("LICENSE-AMD-FidelityFX-SDK.md"), b"AMD text").unwrap();
        fs::write(next.join("SOURCE-OFFER.md"), b"source offer").unwrap();
        fs::write(next.join("fsr4-assets/models/FP8_standard.bin"), b"model").unwrap();
        fs::write(next.join("fsr4-assets/shaders/pass.spv"), b"shader").unwrap();
        publish_fsr4(&dir, &next).unwrap();

        let fsr4_dir = provider.join("fsr4.1.2");
        assert_eq!(
            fs::read(crate::fsr::upscaler_for(&dir, FSR4_VERSION)).unwrap(),
            b"4.1.2 provider".to_vec()
        );
        assert_eq!(
            fs::read(crate::fsr::upscaler_lib(&dir)).unwrap(),
            b"3.1.6 provider".to_vec()
        );
        assert!(fsr4_dir.join("LICENSE-GPL-3.0.txt").is_file());
        assert!(fsr4_dir.join("LICENSE-AMD-FidelityFX-SDK.md").is_file());
        assert!(fsr4_dir.join("SOURCE-OFFER.md").is_file());
        assert!(fsr4_dir
            .join("fsr4-assets/models/FP8_standard.bin")
            .is_file());
        assert!(fsr4_dir.join("fsr4-assets/shaders/pass.spv").is_file());
        assert!(fsr4_licenses_present(&fsr4_dir));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn installed_upscaler_is_reused_only_with_the_expected_version() {
        let dir = temp("upscaler-version");
        let provider = crate::fsr::upscaler_lib(&dir);
        fs::create_dir_all(provider.parent().unwrap()).unwrap();
        let Some(library) = fake_ffx_library(&dir, "3.1.6") else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        fs::copy(&library, &provider).unwrap();
        assert_eq!(
            installed_upscaler_version(&dir, UPSCALER_VERSION).as_deref(),
            Some(UPSCALER_VERSION)
        );
        assert!(upscaler_ready(&dir, UPSCALER_VERSION));

        let Some(library) = fake_ffx_library(&dir, "9.9.9") else {
            fs::remove_dir_all(dir).unwrap();
            return;
        };
        fs::copy(&library, &provider).unwrap();
        assert_eq!(
            installed_upscaler_version(&dir, UPSCALER_VERSION).as_deref(),
            Some("9.9.9")
        );
        assert!(!upscaler_ready(&dir, UPSCALER_VERSION));
        assert!(!upscaler_version_ok("9.9.9", UPSCALER_VERSION));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn ready_requires_the_registered_provider_to_still_be_present() {
        let dir = temp("ready");
        let provider = crate::fsr::framegeneration_lib(&dir);
        fs::create_dir_all(provider.parent().unwrap()).unwrap();
        fs::write(&provider, b"provider").unwrap();
        // Windows status requires the upscaler DLL as well as the frame-generation DLL.
        fs::write(crate::fsr::upscaler_lib(&dir), b"upscaler").unwrap();
        let installed = Installed {
            state: "ready".into(),
            detail: String::new(),
            version: Some("3.1.7".into()),
            upscaler_version: None,
            framegeneration_version: Some("3.1.7".into()),
            provider_bytes: 8,
            provider_sha256: String::new(),
            sdk_sha256: SDK_SHA256.into(),
            built_at: 0,
        };
        crate::fsr::write_atomic(
            &dir.join(INSTALL_FILE),
            &serde_json::to_vec_pretty(&installed).unwrap(),
        )
        .unwrap();
        let value = status(&dir, &crate::fsr::Settings::default(), &dir, false);
        assert_eq!(value["state"], "ready");
        assert_eq!(value["version"], "3.1.7");
        assert_eq!(value["framegeneration"]["version"], "3.1.7");

        fs::remove_file(&provider).unwrap();
        let value = status(&dir, &crate::fsr::Settings::default(), &dir, false);
        assert_eq!(value["state"], "failed");
        assert!(value["detail"]
            .as_str()
            .unwrap()
            .contains("instale novamente"));
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn sdk_cache_requires_the_pinned_size_and_sha_marker() {
        let dir = temp("cache");
        assert!(!sdk_cached(&dir));
        let archive = dir.join(SDK_ARCHIVE_FILE);
        // Do not create a 253 MB fixture: a small file must not count as cached.
        fs::write(&archive, b"x").unwrap();
        write_marker(&dir, SDK_SHA256).unwrap();
        assert!(!sdk_cached(&dir));
        fs::remove_dir_all(dir).unwrap();
    }
}
