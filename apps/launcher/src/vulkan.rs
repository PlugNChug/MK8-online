//! Runs in a separate process: a broken DLL must not crash the launcher or touch saves.
use mk8_manifest::Result;
use std::{ffi::CStr, path::Path};
use windows_sys::Win32::{
    Foundation::FreeLibrary,
    System::LibraryLoader::{
        GetProcAddress, LoadLibraryExW, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
    },
};

// SDL's legacy debugger notification can escape its fallback, as in the runtime.
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

pub(crate) struct ThreadNameHandler {
    vectored: *mut std::ffi::c_void,
    previous: windows_sys::Win32::System::Diagnostics::Debug::LPTOP_LEVEL_EXCEPTION_FILTER,
}
unsafe extern "system" fn unhandled_thread_name(
    pointers: *const windows_sys::Win32::System::Diagnostics::Debug::EXCEPTION_POINTERS,
) -> i32 {
    unsafe { thread_name_notification(pointers.cast_mut()) }
}
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
    let loader =
        std::ffi::CString::new(loader.to_string_lossy().as_bytes()).map_err(|e| e.to_string())?;
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

pub fn check(install: &mk8_manifest::Installation, local: &Path) -> Result<()> {
    use std::{
        fs,
        os::windows::process::CommandExt,
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
    let mut child = Command::new(std::env::current_exe().map_err(|e| e.to_string())?)
        .arg("--check-vulkan")
        .arg(folder)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(log))
        .creation_flags(0x08000000)
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

#[cfg(test)]
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
