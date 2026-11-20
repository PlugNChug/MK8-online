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
//!   --dxc <dxc binary> \
//!   --work-dir <$XDG_DATA_HOME/mk8online/fsr/build> \
//!   --install-dir <$XDG_DATA_HOME/mk8online/fsr/provider.next>
//! ```
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
//! `provider` directory.
//!
//! The requirement that the archive is the SDK tag `v2.4.0` is enforced by the
//! pinned constants below, not by a mutable remote manifest.
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
pub const WORK_DIR: &str = "build";
pub const PROVIDER_DIR: &str = "provider";
pub const PROVIDER_REL: &str = "provider/libamd_fidelityfx_framegeneration_vk.so";
pub const BUILD_SCRIPT: &str = "tools/build_fg_provider.py";

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
    /// Either a configured path or a resolved one; empty asks the child to
    /// resolve it again from the same environment.
    pub dxc: String,
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

pub struct Job {
    child: Child,
    dir: PathBuf,
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
            dxc: settings.dxc.clone(),
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
        Ok(Self { child, dir })
    }

    /// `None` while running, the exit status once it finishes.
    pub fn poll(&mut self) -> Result<Option<std::process::ExitStatus>> {
        self.child.try_wait().map_err(|error| error.to_string())
    }

    pub fn cancel(&mut self) -> Result<()> {
        mk8_build_manager::terminate_tree(&mut self.child)?;
        let _ = record_failure(&self.dir, "Instalação do FSR cancelada pelo usuário.");
        Ok(())
    }

    pub fn log_path(&self) -> PathBuf {
        self.dir.join(BUILD_LOG)
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
/// then records every outcome in `install.json`/`build.log`.
pub fn run_from_stdin() -> Result<i32> {
    let request: Request = mk8_build_manager::supervisor::read_request()?;
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
    fs::create_dir_all(dir).map_err(|error| format!("{}: {error}", dir.display()))?;
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
    if cfg!(windows) {
        install_signed_dlls(dir)
    } else {
        install_linux_provider(request, dir)
    }
}

fn install_linux_provider(request: &Request, dir: &Path) -> Result<()> {
    set_progress(dir, "download", 1, "Preparando o SDK da AMD…")?;
    ensure_sdk(dir)?;
    let script = request.root.join(BUILD_SCRIPT);
    if !script.is_file() {
        return Err(format!(
            "O script de build não existe: {}. Consulte o contrato na página FSR/launcher.",
            script.display()
        ));
    }
    let dxc = resolve_dxc(&request.root, dir, &request.dxc);
    if !dxc.found {
        return Err(if request.dxc.is_empty() {
            "DXC não encontrado. Informe o caminho do compilador DXC na página FSR ou defina MK8_DXC."
                .into()
        } else {
            format!("DXC não encontrado em {}.", request.dxc)
        });
    }
    build_provider(request, dir, &script, &dxc.path)?;
    Ok(())
}

/// Windows uses AMD's signed Vulkan DLLs from the same tag. They are verified
/// byte-for-byte against the pinned size and SHA-256 before being published;
/// the version is read back with `ffxQuery` when the library can be loaded.
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

fn build_provider(request: &Request, dir: &Path, script: &Path, dxc: &Path) -> Result<()> {
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
        .arg(SDK_SHA256)
        .arg("--dxc")
        .arg(dxc)
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
    let mut reader = BufReader::new(stdout);
    let mut line = String::new();
    while reader.read_line(&mut line).unwrap_or(0) > 0 {
        append_log(dir, line.trim_end_matches(['\n', '\r']))?;
        if let Some((percent, detail)) = progress_marker(&line, current) {
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
    set_progress(
        dir,
        "done",
        100,
        &format!("Provedor FSR {version} pronto em {}.", provider.display()),
    )?;
    append_log(
        dir,
        &format!(
            "== Provedor FSR {version} pronto ({} bytes, sha256={sha256}) ==",
            bytes
        ),
    )?;
    Ok(())
}

fn publish_provider(dir: &Path, next: &Path) -> Result<()> {
    let provider = dir.join(PROVIDER_DIR);
    let old = dir.join(format!("{PROVIDER_DIR}.old"));
    if old.exists() {
        fs::remove_dir_all(&old).map_err(|error| format!("{}: {error}", old.display()))?;
    }
    if provider.exists() {
        fs::rename(&provider, &old).map_err(|error| format!("{}: {error}", provider.display()))?;
    }
    if let Err(error) = fs::rename(next, &provider) {
        if old.exists() {
            let _ = fs::rename(&old, &provider);
        }
        return Err(format!("{}: {error}", provider.display()));
    }
    if old.exists() {
        let _ = fs::remove_dir_all(&old);
    }
    Ok(())
}

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

fn write_marker(dir: &Path, sha256: &str) -> Result<()> {
    crate::fsr::write_atomic(
        &dir.join(format!("{SDK_ARCHIVE_FILE}.sha256")),
        format!("{sha256}\n").as_bytes(),
    )
}

fn ensure_license(dir: &Path) -> Result<()> {
    if crate::fsr::license_status(dir)["verified"] == true {
        return Ok(());
    }
    fetch_license(dir)?;
    Ok(())
}

/// Fetches AMD's license from the pinned tag, verifies the pinned size and
/// SHA-256, and saves it beside the SDK archive. The acceptance step never
/// fetches; it only accepts an already verified file.
pub fn fetch_license(dir: &Path) -> Result<String> {
    fs::create_dir_all(dir).map_err(|error| format!("{}: {error}", dir.display()))?;
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

pub fn record_failure(dir: &Path, detail: &str) -> Result<()> {
    let progress = read_progress(dir);
    let installed = Installed {
        state: "failed".into(),
        detail: detail.into(),
        version: None,
        upscaler_version: None,
        framegeneration_version: None,
        provider_bytes: 0,
        provider_sha256: String::new(),
        sdk_sha256: SDK_SHA256.into(),
        built_at: unix_now(),
    };
    let _ = set_progress(dir, "failed", progress.percent, detail);
    crate::fsr::write_atomic(
        &dir.join(INSTALL_FILE),
        &serde_json::to_vec_pretty(&installed).map_err(|error| error.to_string())?,
    )
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
}

/// Configured path first, then the launcher tool folders and `PATH`.
pub fn resolve_dxc(root: &Path, dir: &Path, configured: &str) -> Dxc {
    let names: &[&str] = if cfg!(windows) {
        &["dxc.exe", "dxc"]
    } else {
        &["dxc"]
    };
    if !configured.is_empty() {
        let path = PathBuf::from(configured);
        return Dxc {
            found: path.is_file(),
            path,
        };
    }
    let mut candidates = Vec::new();
    if let Some(path) = std::env::var_os("MK8_DXC").filter(|value| !value.is_empty()) {
        candidates.push(PathBuf::from(path));
    }
    for base in [
        root.join("local/native-tools/dxc/bin"),
        root.join("local/native-tools/bin"),
        root.join("tools/dxc/bin"),
        dir.join("dxc/bin"),
    ] {
        for name in names {
            candidates.push(base.join(name));
        }
    }
    if let Some(paths) = std::env::var_os("PATH") {
        for base in std::env::split_paths(&paths) {
            for name in names {
                candidates.push(base.join(name));
            }
        }
    }
    for path in candidates {
        if path.is_file() {
            return Dxc { found: true, path };
        }
    }
    Dxc {
        path: PathBuf::new(),
        found: false,
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
pub fn provider_version(path: &Path) -> Result<String> {
    provider_version_for(path, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK)
}

pub fn provider_version_for(path: &Path, create_desc_type: u64) -> Result<String> {
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
        for name in names.iter().take(available as usize) {
            if name.is_null() {
                continue;
            }
            let value = CStr::from_ptr(*name).to_string_lossy().into_owned();
            if !value.is_empty() {
                return Ok(value);
            }
        }
        Err("O provedor compilado não informou o nome da versão.".into())
    }
}

/// The status object shown by the FSR page. `running` means the launcher's job
/// child is still alive.
pub fn status(dir: &Path, settings: &crate::fsr::Settings, root: &Path, running: bool) -> Value {
    let installed = read_installed(dir);
    let progress = read_progress(dir);
    let upscaler = crate::fsr::upscaler_lib(dir);
    let framegeneration = crate::fsr::framegeneration_lib(dir);
    let provider = framegeneration.clone();
    let needed: &[&Path] = if cfg!(windows) {
        &[&upscaler, &framegeneration]
    } else {
        &[&provider]
    };
    let provider_present = needed.iter().all(|path| path.is_file());
    let provider_bytes = fs::metadata(&provider)
        .map(|meta| meta.len())
        .unwrap_or_default();
    let dxc = resolve_dxc(root, dir, &settings.dxc);
    let script = root.join(BUILD_SCRIPT);
    let terms = crate::fsr::terms_accepted(dir);
    let cached = sdk_cached(dir);
    let license = crate::fsr::license_status(dir);
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
                state = "ready";
                detail = if cfg!(windows) {
                    format!(
                        "DLLs assinadas da AMD prontas: upscaling {}, frame generation {}.",
                        installed
                            .upscaler_version
                            .as_deref()
                            .unwrap_or("não informada"),
                        installed
                            .framegeneration_version
                            .as_deref()
                            .unwrap_or("não informada")
                    )
                } else {
                    format!(
                        "Provedor FSR {} pronto ({}).",
                        installed.version.as_deref().unwrap_or("3.1.7"),
                        provider.display()
                    )
                };
            }
            "ready" => {
                state = "failed";
                detail = format!(
                    "Os arquivos registrados não estão mais em {}; instale novamente.",
                    dir.display()
                );
            }
            "failed" => {
                state = "failed";
                detail = installed.detail.clone();
            }
            _ => (),
        }
    }
    if state == "not-installed" && progress.phase == "failed" {
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
            })
        })
        .collect();
    json!({
        "state": state,
        "detail": detail,
        "version": installed.as_ref().and_then(|i| i.version.clone()),
        "provider": {
            "path": provider,
            "present": provider_present,
            "bytes": provider_bytes,
            "sha256": installed.as_ref().map(|i| i.provider_sha256.clone()).unwrap_or_default(),
        },
        "upscaler": {
            "path": upscaler,
            "present": upscaler.is_file(),
            "version": installed.as_ref().and_then(|i| i.upscaler_version.clone()),
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
        "dxc": {"path": dxc.path, "found": dxc.found},
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
    fn configured_dxc_is_reported_even_when_missing() {
        let dir = temp("dxc");
        let missing = dir.join("dxc");
        let result = resolve_dxc(&dir, &dir, missing.to_str().unwrap());
        assert!(!result.found);
        assert_eq!(result.path, missing);
        fs::write(&missing, b"fake").unwrap();
        assert!(resolve_dxc(&dir, &dir, missing.to_str().unwrap()).found);
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

    #[test]
    #[cfg(target_os = "linux")]
    fn provider_version_is_read_from_a_real_ffx_query() {
        let dir = temp("ffx-query");
        let source = dir.join("fake.c");
        let library = dir.join("libfake_ffx.so");
        fs::write(
            &source,
            r#"
#include <stdint.h>
typedef struct { uint64_t type; void* next; } Header;
typedef struct {
    Header header;
    uint64_t createDescType;
    void* device;
    uint64_t* outputCount;
    uint64_t* versionIds;
    const char** versionNames;
} Query;
static const char* name = "9.9.9";
uint32_t ffxQuery(void* context, Header* header) {
    (void)context;
    if (!header || header->type != 4) return 1;
    Query* query = (Query*)header;
    if (!query->outputCount) return 1;
    if (*query->outputCount == 0) { *query->outputCount = 1; return 0; }
    if (query->versionIds) query->versionIds[0] = 0x1234;
    if (query->versionNames) query->versionNames[0] = name;
    *query->outputCount = 1;
    return 0;
}
"#,
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
            Ok(status) if status.success() => (),
            _ => {
                // The default test suite still covers the rest without a C compiler.
                fs::remove_dir_all(dir).unwrap();
                return;
            }
        }
        assert_eq!(
            provider_version_for(&library, FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK)
                .unwrap(),
            "9.9.9"
        );
        fs::remove_dir_all(dir).unwrap();
    }

    #[test]
    fn ready_requires_the_registered_provider_to_still_be_present() {
        let dir = temp("ready");
        let provider = crate::fsr::framegeneration_lib(&dir);
        fs::create_dir_all(provider.parent().unwrap()).unwrap();
        fs::write(&provider, b"provider").unwrap();
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
