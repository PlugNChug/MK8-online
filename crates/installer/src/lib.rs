use mk8_manifest::{verify, Config, Installation, Result};
pub mod ctgp;
pub mod dlc;
use serde::Serialize;
pub mod avatar;
pub mod mii;
pub mod mii_body;
pub mod mii_composer;
pub mod mii_data;
pub mod mii_native;
pub mod mii_resources;
use std::{
    fs,
    path::Path,
    process::{Child, Command, Stdio},
};

#[derive(Serialize)]
pub struct Check {
    pub name: String,
    pub ok: bool,
    pub detail: String,
}

pub fn executable_architecture(exefs: &Path) -> Result<&'static str> {
    let raw = fs::read(exefs.join("main.npdm")).map_err(|e| format!("NPDM indisponível: {e}"))?;
    npdm_architecture(&raw)
}

fn npdm_architecture(raw: &[u8]) -> Result<&'static str> {
    if raw.len() < 0x80 || &raw[..4] != b"META" {
        return Err("main.npdm inválido ou truncado.".into());
    }
    let offset = u32::from_le_bytes(raw[0x70..0x74].try_into().unwrap()) as usize;
    let size = u32::from_le_bytes(raw[0x74..0x78].try_into().unwrap()) as usize;
    if offset < 0x80
        || size < 0x38
        || offset.checked_add(size).is_none_or(|end| end > raw.len())
        || raw.get(offset..offset + 4) != Some(b"ACI0".as_slice())
    {
        return Err("Seção ACI0 inválida no NPDM.".into());
    }
    let title = u64::from_le_bytes(raw[offset + 0x10..offset + 0x18].try_into().unwrap());
    if title != 0x0100152000022000 {
        return Err(format!(
            "Title ID {title:016X} não corresponde ao MK8 Deluxe."
        ));
    }
    Ok(if raw[0xc] & 1 == 1 {
        "aarch64"
    } else {
        "aarch32"
    })
}

pub fn architecture_check(config: &Config) -> Check {
    let (ok, detail) = match executable_architecture(Path::new(&config.exefs)) {
        Ok("aarch64") => (true, "ExeFS AArch64 (64 bits); arquitetura compatível com o tradutor. Gameplay ainda não qualificado.".into()),
        Ok(_) => (false, "Este ExeFS é AArch32 (32 bits). O tradutor exige AArch64. Importe o ExeFS de uma atualização de 64 bits do MK8.".into()),
        Err(e) => (false, e),
    };
    Check {
        name: "Arquitetura do executável".into(),
        ok,
        detail,
    }
}

pub fn inspect(config: &Config) -> Vec<Check> {
    let mut checks = vec![architecture_check(config)];
    for (name, raw, directory) in [
        ("Fontes do runtime", &config.upstream, true),
        ("ExeFS", &config.exefs, true),
        ("RomFS extraído", &config.romfs, true),
        ("Metadados NACP", &config.nacp, false),
        ("Fontes do jogo", &config.fonts, true),
    ] {
        let p = Path::new(raw);
        let ok = !raw.is_empty() && if directory { p.is_dir() } else { p.is_file() };
        checks.push(Check {
            name: name.into(),
            ok,
            detail: if ok {
                "Encontrado; identidade será validada na preparação.".into()
            } else {
                "Informe um caminho existente.".into()
            },
        });
    }
    for module in ["main", "sdk", "subsdk0", "main.npdm"] {
        let p = Path::new(&config.exefs).join(module);
        checks.push(Check {
            name: format!("ExeFS / {module}"),
            ok: !config.exefs.is_empty() && p.is_file(),
            detail: p.display().to_string(),
        });
    }
    checks
}

pub fn validate_installation(install: &Installation) -> Result<()> {
    if install.schema != 2 || install.profile != "mk8d-smm2-qualified" || !install.gameplay_verified
    {
        return Err("Instalação ainda não qualificada para o runtime MK8.".into());
    }
    validate_files(install)
}

pub fn validate_candidate(install: &Installation) -> Result<()> {
    if install.schema != 2
        || install.profile != "mk8d-smm2-experimental"
        || install.gameplay_verified
    {
        return Err("Manifesto experimental inválido.".into());
    }
    validate_files(install)
}

fn validate_files(install: &Installation) -> Result<()> {
    verify(&install.executable)?;
    for key in ["npdm", "main", "sdk", "subsdk0", "gamelib"] {
        if !install.files.contains_key(key) {
            return Err(format!("Instalação sem {key}"));
        }
    }
    for file in install.files.values() {
        verify(file)?;
    }
    let romfs = install.data_root.join("extracted").join("romfs");
    if !romfs.is_dir() {
        // Builds before the hard-link mirror left a junction here, which the launcher's
        // RedirectionTrust mitigation refuses to follow (see toolchain/build.py `mirror`).
        if fs::symlink_metadata(&romfs).is_ok_and(|m| m.file_type().is_symlink()) {
            return Err("RomFS ligado por um atalho que o Windows bloqueia para o launcher. Clique em Preparar novamente.".into());
        }
        return Err("Instalação incompleta: RomFS ausente".into());
    }
    Ok(())
}

pub fn launch(
    install: &Installation,
    local: &Path,
    network: Option<(String, u8)>,
    username: &str,
) -> Result<Child> {
    validate_installation(install)?;
    spawn_game(install, local, network, username, false)
}

pub fn launch_candidate(
    install: &Installation,
    local: &Path,
    network: Option<(String, u8)>,
    username: &str,
) -> Result<Child> {
    validate_candidate(install)?;
    spawn_game(install, local, network, username, false)
}

pub const NEXTENDO_GAME_KEY: &str =
    "770551f13152f3d55370de6c039d3859cff0d39882f27c6c9fc6cfdb327d74b8";

pub fn nextendo_test_configured() -> bool {
    std::env::var("MK8_NEXTENDO_TEST").as_deref() == Ok("1")
        && ["RECOMP_NEXTENDO_TLS_PIN", "RECOMP_NEXTENDO_ZNC_TLS_PIN"]
            .iter()
            .all(|key| {
                std::env::var(key)
                    .is_ok_and(|v| v.len() == 64 && v.bytes().all(|c| c.is_ascii_hexdigit()))
            })
}

pub fn nextendo_trusted(local: &Path) -> bool {
    nextendo_test_configured()
        || mk8_manifest::load::<String>(&local.join("nextendo-trust.json")).as_deref()
            == Ok(NEXTENDO_GAME_KEY)
}

pub fn trust_nextendo(local: &Path, key: &str) -> Result<()> {
    if key != NEXTENDO_GAME_KEY {
        return Err("Chave de servidor Nextendo desconhecida.".into());
    }
    mk8_manifest::save(&local.join("nextendo-trust.json"), &key)
}

pub fn launch_nextendo(
    install: &Installation,
    local: &Path,
    username: &str,
    candidate: bool,
) -> Result<Child> {
    if !nextendo_trusted(local) {
        return Err("Confirme a chave do servidor Nextendo no launcher.".into());
    }
    if !install.nextendo_supported && !nextendo_test_configured() {
        return Err("Atualize a build do jogo para usar o Nextendo.".into());
    }
    if candidate {
        validate_candidate(install)?;
    } else {
        validate_installation(install)?;
    }
    spawn_game(install, local, None, username, true)
}

/// Number of previous game logs kept in `logs/` (the current one stays at `game.log`).
pub const GAME_LOG_HISTORY: usize = 10;

/// Moves the previous session's `game.log` to `logs/game-<unix time>.log` and keeps the newest
/// [`GAME_LOG_HISTORY`] of them, so a crash report can still be collected after the next launch.
pub fn rotate_game_log(local: &Path) -> Result<()> {
    let current = local.join("game.log");
    let Ok(meta) = fs::metadata(&current) else {
        return Ok(());
    };
    if meta.len() == 0 {
        return Ok(());
    }
    let logs = local.join("logs");
    fs::create_dir_all(&logs).map_err(|e| e.to_string())?;
    let stamp = meta
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_secs());
    let mut target = logs.join(format!("game-{stamp}.log"));
    let mut n = 1;
    while target.exists() {
        target = logs.join(format!("game-{stamp}-{n}.log"));
        n += 1;
    }
    fs::rename(&current, &target).map_err(|e| e.to_string())?;
    let mut old: Vec<_> = fs::read_dir(&logs)
        .map_err(|e| e.to_string())?
        .filter_map(|entry| entry.ok().map(|e| e.path()))
        .filter(|p| {
            p.file_name()
                .and_then(|n| n.to_str())
                .is_some_and(|n| n.starts_with("game-") && n.ends_with(".log"))
        })
        .collect();
    old.sort_by_key(|p| fs::metadata(p).and_then(|m| m.modified()).ok());
    let excess = old.len().saturating_sub(GAME_LOG_HISTORY);
    for path in old.into_iter().take(excess) {
        fs::remove_file(path).map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn spawn_game(
    install: &Installation,
    local: &Path,
    network: Option<(String, u8)>,
    username: &str,
    nextendo: bool,
) -> Result<Child> {
    let mii_directory = mii_resources::launch_directory(local)?;
    let native_head = mii_native::launch_directory(local, network.is_some() || nextendo)?;
    if native_head.is_some() && mii_directory.is_some() {
        return Err("Desative os recursos MiiModel antes de usar a cabeça convertida.".into());
    }
    mii::read(local)?;
    let dlc_root = dlc::launch_root(local, install)?;
    let pack = if nextendo {
        None
    } else {
        ctgp::launch_overlay(local, install)?
    };
    // Online, the added cups stay off: each machine would decide alone which races are a CTGP cup's (the game sends
    // course IDs, which the added cups borrow), so all play the game's cups and courses with the pack's files.
    let cups = if nextendo {
        String::new()
    } else {
        match network {
            Some(_) => String::new(),
            None => ctgp::launch_cups(local, install)?,
        }
    };
    let saves = local.join("user-no-mii");
    fs::create_dir_all(&saves).map_err(|e| e.to_string())?;
    // The CTGP pack keeps its own saves (records and ghosts of its courses), seeded once from the regular ones.
    let save_dir = match &pack {
        Some(_) => ctgp::save_folder(&saves)?,
        None => saves.join("save"),
    };
    rotate_game_log(local)?;
    let log = fs::File::create(local.join("game.log")).map_err(|e| e.to_string())?;
    let mut cmd = Command::new(&install.executable.path);
    cmd.env_remove("MK8_NEXTENDO_COMMUNITY");
    cmd.env("MK8_NEXTENDO_TEST", if nextendo { "1" } else { "0" });
    if nextendo {
        cmd.arg("--nextendo-login-play").env_remove("MK8_AUTO_LAN");
        if !nextendo_test_configured() {
            use std::net::ToSocketAddrs;
            for (key, host) in [
                ("RECOMP_NEXTENDO_SERVER", "nex.nextendo.network"),
                ("RECOMP_NEXTENDO_NAT", "nat.nextendo.network"),
            ] {
                let ip = (host, 443)
                    .to_socket_addrs()
                    .map_err(|e| format!("Nextendo DNS ({host}): {e}"))?
                    .find(|addr| addr.is_ipv4())
                    .ok_or_else(|| format!("Nextendo DNS ({host}): IPv4 indisponível."))?;
                cmd.env(key, ip.ip().to_string());
            }
            for key in [
                "RECOMP_NEXTENDO_API",
                "RECOMP_NEXTENDO_ZNC_TLS_PIN",
                "RECOMP_NET_HOSTS",
                "RECOMP_NET_BACKEND",
                "RECOMP_NET_NO_PROBE",
                "RECOMP_NET_ID_TOKEN_FILE",
                "RECOMP_NET_NEX_TOKEN_FILE",
                "RECOMP_NET_TEST_SERVER",
            ] {
                cmd.env_remove(key);
            }
            cmd.env("MK8_NEXTENDO_COMMUNITY", "1")
                .env("RECOMP_NEXTENDO_TLS_PIN", NEXTENDO_GAME_KEY);
        }
    }
    // The runtime uses LARECOMP_DATA_ROOT as cwd; keep saves out of versioned builds.
    cmd.current_dir(&saves)
        // Steam's launcher-side SDK installs SDL device exclusion lists for Steam Input.
        // The separate MK8 runtime reads native SDL controllers and does not initialize
        // Steam Input, so those lists can hide its physical XInput controller entirely.
        .env("SDL_GAMECONTROLLER_IGNORE_DEVICES", "")
        .env("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "")
        .env("MK8_EXPERIMENTAL_NO_MII", "1")
        .env("RECOMP_USER_NAME", username)
        .env("MK8_ROOM_MENU_SIGNAL", local.join("open-rooms"))
        .env("LARECOMP_DATA_ROOT", &saves)
        .env("LARECOMP_SAVE_DIR", &save_dir)
        .env("LARECOMP_GAME_LIB", &install.files["gamelib"].path)
        .env(
            "SDL_VULKAN_LIBRARY",
            install
                .executable
                .path
                .parent()
                .ok_or("Pasta do runtime ausente")?
                .join("vulkan-1.dll"),
        )
        .env(
            "MK8_DLC_ROOT",
            dlc_root.as_deref().unwrap_or_else(|| Path::new("")),
        )
        .env(
            "MK8_ROMFS_OVERLAY",
            pack.as_deref().unwrap_or_else(|| Path::new("")),
        )
        .env("MK8_CUPS_EXTRA", &cups)
        .arg(install.data_root.join("extracted"))
        .stdin(Stdio::null())
        .stdout(Stdio::from(log.try_clone().map_err(|e| e.to_string())?))
        .stderr(Stdio::from(log));
    for env in [
        "MK8_MII_PROFILE_PATH",
        "MK8_MII_ID",
        "MK8_MII_NAME",
        "MK8_MII_COLOR",
        "MK8_MII_GENDER",
        "MK8_MII_FACE",
        "NX_LAN_RELAY",
        "NX_LAN_ROOM",
        "NX_LAN_SLOT",
        "LARECOMP_LAN_RELAY",
        "LARECOMP_LAN_ROOM",
        "LARECOMP_LAN_SLOT",
    ] {
        cmd.env_remove(env);
    }
    if let Some((addr, slot)) = network {
        cmd.env("LARECOMP_LAN_RELAY", addr)
            .env("LARECOMP_LAN_ROOM", "MK8ONLINE")
            .env("LARECOMP_LAN_SLOT", slot.to_string());
    }
    mii::configure(&mut cmd, local)?;
    mii_native::configure_command(&mut cmd, native_head.as_deref())?;
    cmd.env_remove("MK8_MII_RESOURCE_DIR");
    if let Some(directory) = mii_directory {
        cmd.env("MK8_MII_RESOURCE_DIR", directory)
            .env("MK8_EXPERIMENTAL_NO_MII", "0");
    }
    cmd.env("MK8_MII_MENU_SIGNAL", local.join("open-miis"));
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        // DETACHED_PROCESS: no console at all. CREATE_NO_WINDOW gave the runtime a hidden console of its own, and
        // the runtime (platform/windows.c log_to_file_without_console) then sent stdout/stderr to mk8drecomp.log
        // beside the exe instead of game.log: every session's game.log, and the logs in Support reports, were empty.
        cmd.creation_flags(0x0000_0008);
    }
    cmd.spawn().map_err(|e| format!("Falha ao abrir jogo: {e}"))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn game_log_rotation_keeps_a_bounded_history() {
        let root = std::env::temp_dir().join(format!("mk8-logs-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        rotate_game_log(&root).unwrap(); // nothing to rotate yet
        fs::write(root.join("game.log"), b"").unwrap();
        rotate_game_log(&root).unwrap(); // an empty log is not history
        assert!(!root.join("logs").exists());
        for i in 0..GAME_LOG_HISTORY + 3 {
            fs::write(root.join("game.log"), format!("session {i}")).unwrap();
            rotate_game_log(&root).unwrap();
            assert!(!root.join("game.log").exists());
        }
        let kept: Vec<String> = fs::read_dir(root.join("logs"))
            .unwrap()
            .map(|e| fs::read_to_string(e.unwrap().path()).unwrap())
            .collect();
        assert_eq!(kept.len(), GAME_LOG_HISTORY);
        assert!(kept.contains(&format!("session {}", GAME_LOG_HISTORY + 2)));
        assert!(!kept.contains(&"session 0".to_string()));
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn experimental_build_never_passes_qualified_gate_and_checks_hashes() {
        let root = std::env::temp_dir().join(format!("mk8-candidate-{}", std::process::id()));
        fs::create_dir_all(root.join("extracted/romfs")).unwrap();
        let path = root.join("fixture");
        fs::write(&path, b"authored test fixture").unwrap();
        let file = mk8_manifest::hash_file(&path).unwrap();
        let mut install = Installation {
            schema: 2,
            identity: "fixture".into(),
            profile: "mk8d-smm2-experimental".into(),
            executable: file.clone(),
            files: ["npdm", "main", "sdk", "subsdk0", "gamelib"]
                .into_iter()
                .map(|key| (key.into(), file.clone()))
                .collect(),
            data_root: root.clone(),
            shader_root: root.clone(),
            gameplay_verified: false,
            dlc_supported: false,
            pack_supported: false,
            cups_supported: false,
            nextendo_supported: false,
            temporal_upscaling_supported: false,
            romfs_long_paths_supported: false,
            nextendo_session_supported: false,
            live_settings_supported: false,
            menu_themes_supported: false,
            menu_light_themes_supported: false,
            split_screen_60fps_supported: false,
            bloom_supported: false,
            direct_lan_guest_supported: false,
        };
        assert!(validate_candidate(&install).is_ok());
        assert!(validate_installation(&install).is_err());
        install.gameplay_verified = true;
        assert!(validate_candidate(&install).is_err());
        install.gameplay_verified = false;
        fs::write(&path, b"corrupted").unwrap();
        assert!(validate_candidate(&install).is_err());
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn npdm_rejects_wrong_title_and_distinguishes_architectures() {
        let mut raw = vec![0u8; 0xc0];
        raw[..4].copy_from_slice(b"META");
        raw[0x70..0x74].copy_from_slice(&0x80u32.to_le_bytes());
        raw[0x74..0x78].copy_from_slice(&0x40u32.to_le_bytes());
        raw[0x80..0x84].copy_from_slice(b"ACI0");
        raw[0x90..0x98].copy_from_slice(&0x0100152000022000u64.to_le_bytes());
        assert_eq!(npdm_architecture(&raw).unwrap(), "aarch32");
        raw[0xc] = 1;
        assert_eq!(npdm_architecture(&raw).unwrap(), "aarch64");
        raw[0x90] = 1;
        assert!(npdm_architecture(&raw).is_err());
        raw[0x70..0x74].copy_from_slice(&u32::MAX.to_le_bytes());
        assert!(npdm_architecture(&raw).is_err());
    }
}
