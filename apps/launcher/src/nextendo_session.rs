//! The UI owns the refresh-token slot. Only its random name crosses the supervisor pipe.
#[derive(Default)]
pub struct Session {
    #[cfg(windows)]
    slot: Option<(String, std::os::windows::io::OwnedHandle)>,
}

impl Session {
    pub fn name(&mut self) -> Result<Option<String>, String> {
        #[cfg(windows)]
        {
            use std::os::windows::io::{FromRawHandle, OwnedHandle};
            use windows_sys::Win32::{
                Foundation::{GetLastError, ERROR_ALREADY_EXISTS, INVALID_HANDLE_VALUE},
                System::Memory::{CreateFileMappingW, PAGE_READWRITE},
            };
            if let Some((name, _)) = &self.slot {
                return Ok(Some(name.clone()));
            }
            let id = u128::from_le_bytes(mk8_transport::random());
            let name = format!(r"Local\MK8-Nextendo-{id:032x}");
            let wide: Vec<_> = name.encode_utf16().chain([0]).collect();
            // Non-inheritable handle, caller's default Windows security descriptor.
            let handle = unsafe {
                CreateFileMappingW(
                    INVALID_HANDLE_VALUE,
                    std::ptr::null(),
                    PAGE_READWRITE,
                    0,
                    600,
                    wide.as_ptr(),
                )
            };
            if handle.is_null() {
                return Err(std::io::Error::last_os_error().to_string());
            }
            let existed = unsafe { GetLastError() } == ERROR_ALREADY_EXISTS;
            let owner = unsafe { OwnedHandle::from_raw_handle(handle) };
            if existed {
                return Err("Não foi possível criar a sessão privada do Nextendo.".into());
            }
            self.slot = Some((name.clone(), owner));
            Ok(Some(name))
        }
        #[cfg(not(windows))]
        Ok(None)
    }
}
