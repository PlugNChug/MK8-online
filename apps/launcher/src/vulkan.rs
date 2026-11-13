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
            return Err(format!("Falha ao carregar {} ou suas dependências: {}. O código do Windows não identifica sozinho a DLL dependente ausente. Confira bloqueios e repare Ferramentas/Build sem apagar os dados.", path.display(), std::io::Error::last_os_error()));
        }
        Ok(Self(handle))
    }
    fn symbol(&self, name: &CStr) -> Result<unsafe extern "system" fn() -> isize> {
        unsafe { GetProcAddress(self.0, name.as_ptr().cast()) }
            .ok_or_else(|| format!("DLL sem função exigida: {}", name.to_string_lossy()))
    }
}

pub fn probe(folder: &Path) -> Result<()> {
    let folder = crate::canonical(folder).map_err(|e| e.to_string())?;
    let loader = folder.join("vulkan-1.dll");
    let vk = Library::open(&loader)?;
    vk.symbol(c"vkGetInstanceProcAddr")?;
    let sdl = Library::open(&folder.join("SDL3.dll"))?;
    sdl.symbol(c"SDL_Vulkan_LoadLibrary")?;
    sdl.symbol(c"SDL_CreateWindow")?;
    Ok(())
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
