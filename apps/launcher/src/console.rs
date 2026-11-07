//! The window build uses the GUI subsystem, so it has no console. Console
//! children (the game, taskkill) would each open a visible console window;
//! a hidden console allocated once is inherited by them instead.
#[cfg(windows)]
mod imp {
    use std::{ffi::c_void, sync::Once};
    #[link(name = "kernel32")]
    extern "system" {
        fn AllocConsole() -> i32;
        fn AttachConsole(process: u32) -> i32;
        fn GetConsoleWindow() -> *mut c_void;
    }
    #[link(name = "user32")]
    extern "system" {
        fn ShowWindow(window: *mut c_void, command: i32) -> i32;
    }
    static HIDDEN: Once = Once::new();

    pub fn ensure_hidden() {
        HIDDEN.call_once(|| {
            // SAFETY: plain Win32 calls without pointers owned by Rust.
            unsafe {
                if GetConsoleWindow().is_null() && AllocConsole() != 0 {
                    let window = GetConsoleWindow();
                    if !window.is_null() {
                        ShowWindow(window, 0); // SW_HIDE
                    }
                }
            }
        });
    }

    pub fn attach_parent() {
        // SAFETY: ATTACH_PARENT_PROCESS; failure just means no terminal output.
        unsafe {
            AttachConsole(u32::MAX);
        }
    }
}

#[cfg(not(windows))]
mod imp {
    pub fn ensure_hidden() {}
    pub fn attach_parent() {}
}

pub use imp::{attach_parent, ensure_hidden};
