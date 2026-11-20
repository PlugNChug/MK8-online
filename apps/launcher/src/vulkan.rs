//! Runs in a separate process: a broken DLL must not crash the launcher or touch saves.
use mk8_manifest::Result;
#[cfg(windows)]
use std::ffi::CStr;
use std::path::Path;

// SDL's legacy debugger notification can escape its fallback, as in the runtime.
#[cfg(windows)]
unsafe extern "system" fn thread_name_notification(
    pointers: *mut windows_sys::Win32::System::Diagnostics::Debug::EXCEPTION_POINTERS,
) -> i32 {
    let Some(pointers) = (unsafe { pointers.as_ref() }) else {
        return 0;
    };
    let Some(record) = (unsafe { pointers.ExceptionRecord.as_ref() }) else {
        return 0;
    };
    if record.ExceptionCode == 0x406d1388
        && record.ExceptionFlags & 1 == 0
        && record.NumberParameters >= 2
        && record.ExceptionInformation[0] as u32 == 0x1000
    {
        -1 // EXCEPTION_CONTINUE_EXECUTION; all other exceptions keep their normal handling.
    } else {
        0
    }
}

#[cfg(windows)]
pub(crate) struct ThreadNameHandler {
    vectored: *mut std::ffi::c_void,
    previous: windows_sys::Win32::System::Diagnostics::Debug::LPTOP_LEVEL_EXCEPTION_FILTER,
}
#[cfg(windows)]
unsafe extern "system" fn unhandled_thread_name(
    pointers: *const windows_sys::Win32::System::Diagnostics::Debug::EXCEPTION_POINTERS,
) -> i32 {
    unsafe { thread_name_notification(pointers.cast_mut()) }
}
#[cfg(windows)]
impl ThreadNameHandler {
    pub(crate) fn register() -> Result<Self> {
        let handler = unsafe {
            windows_sys::Win32::System::Diagnostics::Debug::AddVectoredExceptionHandler(
                1,
                Some(thread_name_notification),
            )
        };
        if handler.is_null() {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let previous = unsafe {
            windows_sys::Win32::System::Diagnostics::Debug::SetUnhandledExceptionFilter(Some(
                unhandled_thread_name,
            ))
        };
        Ok(Self {
            vectored: handler,
            previous,
        })
    }
}
#[cfg(windows)]
impl Drop for ThreadNameHandler {
    fn drop(&mut self) {
        unsafe {
            windows_sys::Win32::System::Diagnostics::Debug::SetUnhandledExceptionFilter(
                self.previous,
            );
            if !self.vectored.is_null() {
                windows_sys::Win32::System::Diagnostics::Debug::RemoveVectoredExceptionHandler(
                    self.vectored,
                );
            }
        }
    }
}

/// A Vulkan physical device, reduced to what the FSR page needs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GpuInfo {
    pub api_version: u32,
    pub vendor_id: u32,
    pub device_id: u32,
    pub device_type: u32,
    pub device_name: String,
}

impl GpuInfo {
    pub fn vendor(&self) -> &'static str {
        match self.vendor_id {
            0x1002 => "AMD",
            0x10de => "NVIDIA",
            0x8086 => "Intel",
            _ => "desconhecido",
        }
    }

    /// AMD's native FSR 4 path is gated to RDNA 4. The shipped Windows DLLs
    /// also hide 4.1.2 on other hardware. Device names are the only stable
    /// signal exposed by both AMD's Windows driver and RADV.
    pub fn is_amd_rdna4(&self) -> bool {
        let name = self.device_name.to_ascii_lowercase();
        self.vendor_id == 0x1002
            && ["rx 9", "radeon 9", "rdna 4", "gfx12"]
                .iter()
                .any(|marker| name.contains(marker))
    }
}

static GPU: std::sync::OnceLock<Option<GpuInfo>> = std::sync::OnceLock::new();

/// Probes the first usable Vulkan device once per launcher process. `None`
/// means there is no Vulkan loader/device (or the probe was refused).
pub fn gpu_info() -> Option<GpuInfo> {
    GPU.get_or_init(|| probe_gpu().ok()).clone()
}

#[cfg(windows)]
const VULKAN_LOADER: &str = "vulkan-1.dll";
#[cfg(not(windows))]
const VULKAN_LOADER: &str = "libvulkan.so.1";

#[repr(C)]
struct VkApplicationInfo {
    s_type: u32,
    p_next: *const std::ffi::c_void,
    p_application_name: *const std::ffi::c_char,
    application_version: u32,
    p_engine_name: *const std::ffi::c_char,
    engine_version: u32,
    api_version: u32,
}

#[repr(C)]
struct VkInstanceCreateInfo {
    s_type: u32,
    p_next: *const std::ffi::c_void,
    flags: u32,
    p_application_info: *const VkApplicationInfo,
    enabled_layer_count: u32,
    pp_enabled_layer_names: *const *const std::ffi::c_char,
    enabled_extension_count: u32,
    pp_enabled_extension_names: *const *const std::ffi::c_char,
}

#[repr(C, align(8))]
struct VkPhysicalDevicePropertiesRaw {
    bytes: [u8; 1024],
}

fn probe_gpu() -> Result<GpuInfo> {
    use std::ffi::c_void;
    unsafe {
        let loader = libloading::Library::new(VULKAN_LOADER)
            .map_err(|error| format!("Vulkan indisponível: {error}"))?;
        let create: libloading::Symbol<
            unsafe extern "system" fn(
                *const VkInstanceCreateInfo,
                *const c_void,
                *mut *mut c_void,
            ) -> i32,
        > = loader
            .get(b"vkCreateInstance\0")
            .map_err(|error| error.to_string())?;
        let enumerate: libloading::Symbol<
            unsafe extern "system" fn(*mut c_void, *mut u32, *mut *mut c_void) -> i32,
        > = loader
            .get(b"vkEnumeratePhysicalDevices\0")
            .map_err(|error| error.to_string())?;
        let properties: libloading::Symbol<
            unsafe extern "system" fn(*mut c_void, *mut VkPhysicalDevicePropertiesRaw),
        > = loader
            .get(b"vkGetPhysicalDeviceProperties\0")
            .map_err(|error| error.to_string())?;
        let destroy: libloading::Symbol<unsafe extern "system" fn(*mut c_void, *const c_void)> =
            loader
                .get(b"vkDestroyInstance\0")
                .map_err(|error| error.to_string())?;
        let app_name = b"MK8-online\0";
        let app = VkApplicationInfo {
            s_type: 0,
            p_next: std::ptr::null(),
            p_application_name: app_name.as_ptr().cast(),
            application_version: 0,
            p_engine_name: std::ptr::null(),
            engine_version: 0,
            api_version: 1 << 22,
        };
        let create_info = VkInstanceCreateInfo {
            s_type: 1,
            p_next: std::ptr::null(),
            flags: 0,
            p_application_info: &app,
            enabled_layer_count: 0,
            pp_enabled_layer_names: std::ptr::null(),
            enabled_extension_count: 0,
            pp_enabled_extension_names: std::ptr::null(),
        };
        let mut instance = std::ptr::null_mut();
        let rc = create(&create_info, std::ptr::null(), &mut instance);
        if rc != 0 || instance.is_null() {
            return Err(format!("vkCreateInstance retornou {rc}."));
        }
        let mut count = 0u32;
        let rc = enumerate(instance, &mut count, std::ptr::null_mut());
        if rc != 0 || count == 0 {
            destroy(instance, std::ptr::null());
            return Err(format!("Nenhum dispositivo Vulkan (código {rc})."));
        }
        let mut devices = vec![std::ptr::null_mut(); count as usize];
        let rc = enumerate(instance, &mut count, devices.as_mut_ptr());
        if rc != 0 {
            destroy(instance, std::ptr::null());
            return Err(format!("vkEnumeratePhysicalDevices retornou {rc}."));
        }
        let mut best: Option<(GpuInfo, u8)> = None;
        for device in devices.into_iter().take(count as usize) {
            let mut raw = VkPhysicalDevicePropertiesRaw { bytes: [0; 1024] };
            properties(device, &mut raw);
            let read = |offset: usize| {
                u32::from_ne_bytes(
                    raw.bytes[offset..offset + 4]
                        .try_into()
                        .expect("quatro bytes"),
                )
            };
            let name_end = raw.bytes[20..276]
                .iter()
                .position(|byte| *byte == 0)
                .unwrap_or(256);
            let info = GpuInfo {
                api_version: read(0),
                vendor_id: read(8),
                device_id: read(12),
                device_type: read(16),
                device_name: String::from_utf8_lossy(&raw.bytes[20..20 + name_end]).into_owned(),
            };
            let rank = match info.device_type {
                2 => 3, // discrete
                1 => 2, // integrated
                _ => 1,
            };
            if best.as_ref().is_none_or(|(_, current)| rank > *current) {
                best = Some((info, rank));
            }
        }
        destroy(instance, std::ptr::null());
        best.map(|(info, _)| info)
            .ok_or_else(|| "Nenhum dispositivo Vulkan utilizável.".into())
    }
}

/// The thread-name notification is a Win32 debugger event; other systems have none to intercept.
#[cfg(not(windows))]
pub(crate) struct ThreadNameHandler;

#[cfg(not(windows))]
impl ThreadNameHandler {
    pub(crate) fn register() -> Result<Self> {
        Ok(Self)
    }
}

#[cfg(windows)]
mod windows {
    use super::*;
    use windows_sys::Win32::{
        Foundation::FreeLibrary,
        System::LibraryLoader::{
            GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
        },
    };

    struct Library(windows_sys::Win32::Foundation::HMODULE);
    impl Drop for Library {
        fn drop(&mut self) {
            unsafe {
                FreeLibrary(self.0);
            }
        }
    }
    impl Library {
        fn open(path: &Path) -> Result<Self> {
            use std::os::windows::ffi::OsStrExt;
            if !path.is_file() {
                return Err(format!(
                "DLL ausente ou inacessível: {}. Execute Ferramentas e Build; preserve os saves.",
                path.display()
            ));
            }
            let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
            let handle = unsafe {
                LoadLibraryExW(
                    wide.as_ptr(),
                    std::ptr::null_mut(),
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
                )
            };
            if handle.is_null() {
                let error = std::io::Error::last_os_error();
                let reason = match error.raw_os_error() {
                    Some(193) => "Formato de DLL incompatível ou inválido (arquivo principal ou dependência); confira arquitetura x64 e integridade.",
                    Some(126) => "Um módulo não foi encontrado; pode ser uma dependência da DLL indicada.",
                    _ => "O código do Windows não identifica sozinho qual dependência falhou.",
                };
                return Err(format!("Falha ao carregar {} ou suas dependências: {error}. {reason} Repare Ferramentas/Build sem apagar os dados.", path.display()));
            }
            Ok(Self(handle))
        }
        fn symbol(&self, name: &CStr) -> Result<unsafe extern "system" fn() -> isize> {
            unsafe { GetProcAddress(self.0, name.as_ptr().cast()) }
                .ok_or_else(|| format!("DLL sem função exigida: {}", name.to_string_lossy()))
        }
    }

    pub fn probe(folder: &Path) -> Result<()> {
        eprintln!("Vulkan preflight: loading local libraries");
        let folder = crate::canonical(folder).map_err(|e| e.to_string())?;
        let loader = folder.join("vulkan-1.dll");
        let vk = Library::open(&loader)?;
        vk.symbol(c"vkGetInstanceProcAddr")?;
        let sdl = Library::open(&folder.join("SDL3.dll"))?;
        sdl.symbol(c"SDL_CreateWindow")?;
        // Resolve every entry before initialization so every initialized path can call SDL_Quit.
        let init: unsafe extern "C" fn(u32) -> bool =
            unsafe { std::mem::transmute(sdl.symbol(c"SDL_Init")?) };
        let quit: unsafe extern "C" fn() = unsafe { std::mem::transmute(sdl.symbol(c"SDL_Quit")?) };
        let load: unsafe extern "C" fn(*const std::ffi::c_char) -> bool =
            unsafe { std::mem::transmute(sdl.symbol(c"SDL_Vulkan_LoadLibrary")?) };
        let unload: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(sdl.symbol(c"SDL_Vulkan_UnloadLibrary")?) };
        let error: unsafe extern "C" fn() -> *const std::ffi::c_char =
            unsafe { std::mem::transmute(sdl.symbol(c"SDL_GetError")?) };
        let loader = std::ffi::CString::new(loader.to_string_lossy().as_bytes())
            .map_err(|e| e.to_string())?;
        let failure = |stage: &str| {
            let ptr = unsafe { error() };
            let detail = if ptr.is_null() {
                "SDL não forneceu detalhes".into()
            } else {
                unsafe { CStr::from_ptr(ptr) }
                    .to_string_lossy()
                    .into_owned()
            };
            format!("{stage}: {detail}")
        };
        eprintln!("Vulkan preflight: initializing SDL video");
        if !unsafe { init(0x20) } {
            // SDL_INIT_VIDEO, no window created.
            let detail = failure("SDL_Init(VIDEO) falhou");
            unsafe { quit() };
            return Err(detail);
        }
        eprintln!("Vulkan preflight: loading Vulkan through SDL");
        let result = if unsafe { load(loader.as_ptr()) } {
            unsafe { unload() };
            Ok(())
        } else {
            Err(failure("SDL_Vulkan_LoadLibrary falhou"))
        };
        eprintln!("Vulkan preflight: stopping SDL video");
        unsafe { quit() };
        eprintln!("Vulkan preflight: completed");
        result
    }
}

#[cfg(windows)]
pub use windows::probe;

#[cfg(target_os = "linux")]
pub fn probe(_: &Path) -> Result<()> {
    unsafe {
        let vk = libloading::Library::new("libvulkan.so.1").map_err(|e| e.to_string())?;
        let enumerate = vk
            .get::<unsafe extern "C" fn(*mut u32) -> i32>(b"vkEnumerateInstanceVersion\0")
            .map_err(|e| e.to_string())?;
        let mut version = 0;
        if enumerate(&mut version) != 0 || version < ((1 << 22) | (3 << 12)) {
            return Err("O loader Vulkan 1.3 é necessário.".into());
        }
        let sdk = std::env::var_os("MK8_NATIVE_TOOLS")
            .map(|path| std::path::PathBuf::from(path).join("linux-sdk/lib/libSDL3.so.0"))
            .filter(|path| path.is_file());
        let sdl = libloading::Library::new(sdk.as_deref().unwrap_or(Path::new("libSDL3.so.0")))
            .map_err(|e| e.to_string())?;
        sdl.get::<unsafe extern "C" fn()>(b"SDL_Vulkan_LoadLibrary\0")
            .map_err(|e| e.to_string())?;
        sdl.get::<unsafe extern "C" fn()>(b"SDL_CreateWindow\0")
            .map_err(|e| e.to_string())?;
    }
    Ok(())
}

pub fn check(install: &mk8_manifest::Installation, local: &Path) -> Result<()> {
    use std::{
        fs,
        process::{Command, Stdio},
        time::{Duration, Instant},
    };
    let folder = install
        .executable
        .path
        .parent()
        .ok_or("Pasta do runtime ausente")?;
    let log_path = local.join("vulkan-check.log");
    let log = fs::File::create(&log_path).map_err(|e| e.to_string())?;
    // Check the exact local files, even when Windows has a different system loader.
    #[cfg(windows)]
    for name in ["vulkan-1.dll", "SDL3.dll"] {
        let result = install
            .files
            .get(name)
            .ok_or_else(|| {
                format!("Manifesto sem {name}; execute Ferramentas e Build sem apagar os dados.")
            })
            .and_then(|file| {
                if file.path != folder.join(name) {
                    return Err(format!("Caminho inesperado para {name}"));
                }
                mk8_manifest::verify(file)
            });
        if let Err(error) = result {
            fs::write(&log_path, &error).map_err(|e| e.to_string())?;
            return Err(error);
        }
    }
    let mut command = Command::new(std::env::current_exe().map_err(|e| e.to_string())?);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        command.creation_flags(0x08000000);
    }
    let mut child = command
        .arg("--check-vulkan")
        .arg(folder)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(log))
        .spawn()
        .map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(20);
    loop {
        if let Some(status) = child.try_wait().map_err(|e| e.to_string())? {
            if status.success() {
                return Ok(());
            }
            let detail = fs::read_to_string(&log_path).unwrap_or_default();
            return Err(format!("Verificação Vulkan falhou ({status}): {detail}. Consulte Suporte. Jogo e saves preservados."));
        }
        if Instant::now() >= deadline {
            child.kill().map_err(|e| e.to_string())?;
            child.wait().map_err(|e| e.to_string())?;
            let error = "Verificação Vulkan excedeu 20 segundos; jogo não iniciado. Confira drivers e bloqueios. Saves preservados.";
            fs::write(&log_path, error).map_err(|e| e.to_string())?;
            return Err(error.into());
        }
        std::thread::sleep(Duration::from_millis(50));
    }
}

#[cfg(all(test, windows))]
mod tests {
    use super::*;
    use windows_sys::Win32::System::Diagnostics::Debug::{
        RaiseException, EXCEPTION_POINTERS, EXCEPTION_RECORD,
    };

    #[test]
    fn handles_real_thread_notification_without_swallowing_faults() {
        let mut handler = ThreadNameHandler::register().unwrap();
        let arguments = [
            0x1000usize,
            c"probe-thread".as_ptr() as usize,
            usize::MAX,
            0,
        ];
        unsafe { RaiseException(0x406d1388, 0, arguments.len() as u32, arguments.as_ptr()) };
        unsafe {
            windows_sys::Win32::System::Diagnostics::Debug::RemoveVectoredExceptionHandler(
                handler.vectored,
            );
            handler.vectored = std::ptr::null_mut();
            RaiseException(0x406d1388, 0, arguments.len() as u32, arguments.as_ptr());
        }

        let mut record: EXCEPTION_RECORD = unsafe { std::mem::zeroed() };
        let check = |record: &mut EXCEPTION_RECORD| {
            let mut pointers = EXCEPTION_POINTERS {
                ExceptionRecord: record,
                ContextRecord: std::ptr::null_mut(),
            };
            unsafe { thread_name_notification(&mut pointers) }
        };
        record.ExceptionCode = 0x406d1388;
        record.NumberParameters = 4;
        record.ExceptionInformation[..4].copy_from_slice(&arguments);
        assert_eq!(check(&mut record), -1);
        record.ExceptionFlags = 1;
        assert_eq!(check(&mut record), 0);
        record.ExceptionFlags = 0;
        record.ExceptionCode = 0xc0000005u32 as i32;
        assert_eq!(check(&mut record), 0);
        record.ExceptionCode = 0x406d1388;
        record.NumberParameters = 1;
        assert_eq!(check(&mut record), 0);
        record.NumberParameters = 4;
        record.ExceptionInformation[0] = 0;
        assert_eq!(check(&mut record), 0);
    }
}

#[cfg(test)]
mod gpu_tests {
    use super::*;

    fn gpu(vendor_id: u32, name: &str) -> GpuInfo {
        GpuInfo {
            api_version: 0,
            vendor_id,
            device_id: 0,
            device_type: 2,
            device_name: name.into(),
        }
    }

    #[test]
    fn classifies_vendors_and_only_amd_rdna4_for_the_native_ml_path() {
        assert_eq!(gpu(0x1002, "AMD Radeon RX 9070 XT").vendor(), "AMD");
        assert_eq!(gpu(0x10de, "NVIDIA GeForce RTX 5080").vendor(), "NVIDIA");
        assert_eq!(gpu(0x8086, "Intel Arc B580").vendor(), "Intel");
        assert!(gpu(0x1002, "AMD Radeon RX 9070 XT").is_amd_rdna4());
        assert!(gpu(0x1002, "AMD Radeon RX 9060 (RDNA 4)").is_amd_rdna4());
        assert!(!gpu(0x1002, "AMD Radeon RX 7900 XTX").is_amd_rdna4());
        assert!(!gpu(0x10de, "NVIDIA GeForce RTX 5080").is_amd_rdna4());
        assert!(!gpu(0x8086, "Intel Arc B580").is_amd_rdna4());
    }
}
