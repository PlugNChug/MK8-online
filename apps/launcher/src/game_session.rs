//! Keeps the game and its resource locks alive after the UI process exits.
use mk8_build_manager::{acquire_game, supervisor};
use mk8_manifest::{Installation, Result};
use serde::{Deserialize, Serialize};
use std::{path::PathBuf, process::Child};

#[derive(Serialize, Deserialize)]
pub struct Request {
    pub root: PathBuf,
    pub local: PathBuf,
    pub install: Installation,
    pub network: Option<(String, u8)>,
    pub username: String,
    pub candidate: bool,
    /// "host" or "guest": the runtime enters the LAN room by itself (MK8_AUTO_LAN, runtime/mk8/net/auto_lan.c).
    #[serde(default)]
    pub auto_lan: Option<String>,
    #[serde(default)]
    pub nextendo: bool,
}

pub fn spawn(request: &Request) -> Result<Child> {
    supervisor::spawn_request("--game-supervisor", request)
}

pub fn run() -> Result<i32> {
    let request: Request = supervisor::read_request()?;
    let _guards = acquire_game(&request.root, &request.local)?;
    if request.candidate {
        mk8_installer::validate_candidate(&request.install)?;
    } else {
        mk8_installer::validate_installation(&request.install)?;
    }
    crate::vulkan::check(&request.install, &request.local)?;
    std::env::set_var(
        "MK8_MANUAL_VIEWER",
        std::env::current_exe().map_err(|e| e.to_string())?,
    );

    crate::fonts::configure(&request.local)?;
    crate::fsr::configure(&request.local)?;
    crate::debug_options::configure(&request.local)?;
    std::env::remove_var("MK8_SETTINGS_ROMFS");
    if !request.nextendo && std::env::var("MK8_SETTINGS_EXPERIMENT").as_deref() == Ok("1") {
        if let Some(pack) = mk8_installer::ctgp::launch_overlay(&request.local, &request.install)? {
            let menu = request
                .install
                .files
                .get("settings-layout")
                .ok_or("Rebuild the game to combine CTGP with native Settings.")?;
            let output = request.local.join("derived/settings-ctgp");
            let mut command = std::process::Command::new(crate::setup::local_python(&request.root));
            command
                .arg(request.root.join("toolchain/settings_layout.py"))
                .arg(&menu.path)
                .arg(pack.join("UI/cmn/menu.sarc"))
                .arg(&output);
            #[cfg(windows)]
            {
                use std::os::windows::process::CommandExt;
                command.creation_flags(0x08000000);
            }
            let result = command.output().map_err(|e| e.to_string())?;
            if !result.status.success() {
                return Err(format!(
                    "Could not combine CTGP and Settings layouts: {}",
                    String::from_utf8_lossy(&result.stderr)
                ));
            }
            std::env::set_var("MK8_SETTINGS_ROMFS", output);
        }
    }
    let signal = request.local.join("open-rooms");
    if signal.exists() {
        std::fs::remove_file(signal).map_err(|e| e.to_string())?;
    }
    // (inherited by the game the launch below starts; never left over from the launcher's own environment)
    match request.auto_lan.as_deref() {
        Some(role @ ("host" | "guest")) => std::env::set_var("MK8_AUTO_LAN", role),
        _ => std::env::remove_var("MK8_AUTO_LAN"),
    }
    let launch = if request.candidate {
        mk8_installer::launch_candidate
    } else {
        mk8_installer::launch
    };
    if request.nextendo && (request.network.is_some() || request.auto_lan.is_some()) {
        return Err("Nextendo não pode usar uma sala LAN/Steam.".into());
    }
    let mut game = if request.nextendo {
        mk8_installer::launch_nextendo(
            &request.install,
            &request.local,
            &request.username,
            request.candidate,
        )?
    } else {
        launch(
            &request.install,
            &request.local,
            request.network,
            &request.username,
        )?
    };
    // Once the game exists, a closed UI pipe must not kill it or release its locks.
    let _ = supervisor::ready();
    let exit = game.wait().map_err(|e| e.to_string())?;
    Ok(exit.code().unwrap_or(1))
}
