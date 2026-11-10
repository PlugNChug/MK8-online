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
    let signal = request.local.join("open-rooms");
    if signal.exists() {
        std::fs::remove_file(signal).map_err(|e| e.to_string())?;
    }
    let launch = if request.candidate {
        mk8_installer::launch_candidate
    } else {
        mk8_installer::launch
    };
    let mut game = launch(
        &request.install,
        &request.local,
        request.network,
        &request.username,
    )?;
    // Once the game exists, a closed UI pipe must not kill it or release its locks.
    let _ = supervisor::ready();
    let exit = game.wait().map_err(|e| e.to_string())?;
    Ok(exit.code().unwrap_or(1))
}
