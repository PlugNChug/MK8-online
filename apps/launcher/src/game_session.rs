//! Keeps the game and its resource locks alive after the UI process exits.
use mk8_build_manager::{acquire_game, supervisor};
use mk8_manifest::{Installation, Result};
use serde::{Deserialize, Serialize};
use std::{path::PathBuf, process::Child};

#[derive(Serialize, Deserialize)]
pub struct Request {
    pub root: PathBuf,
    pub local: PathBuf,
    #[serde(default)]
    pub browser_language: String,
    pub install: Installation,
    pub network: Option<(String, u8)>,
    pub username: String,
    pub candidate: bool,
    /// "host" or "guest": the runtime enters the LAN room by itself (MK8_AUTO_LAN, runtime/mk8/net/auto_lan.c).
    #[serde(default)]
    pub auto_lan: Option<String>,
    #[serde(default)]
    pub nextendo: bool,
    #[serde(default)]
    pub nextendo_session: Option<String>,
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

    crate::graphics::configure(&request.local, (&request.install).into())?;
    crate::fonts::configure(&request.local)?;
    let wrench_layout =
        crate::game_settings::configure_menu(&request.local, (&request.install).into())?;
    crate::debug_options::configure(&request.local)?;
    let saved = crate::language::read(&request.local);
    let overlay_language = if !saved.is_empty() {
        saved
    } else if crate::language::valid(&request.browser_language) {
        request.browser_language.clone()
    } else {
        crate::language::effective(&request.local)
    };
    std::env::set_var("MK8_UI_LANGUAGE", &overlay_language);
    std::env::set_var("MK8_SETTINGS_LANGUAGE", overlay_language);
    std::env::remove_var("MK8_SETTINGS_ROMFS");
    if !request.nextendo && wrench_layout {
        if let Some(pack) = mk8_installer::ctgp::launch_overlay(&request.local, &request.install)? {
            if let Some(output) = ctgp_menu_overlay(&request, &pack)? {
                std::env::set_var("MK8_SETTINGS_ROMFS", output);
            }
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
        if let Some(name) = request.nextendo_session.as_ref() {
            std::env::set_var("MK8_NEXTENDO_SESSION", name);
        } else {
            std::env::remove_var("MK8_NEXTENDO_SESSION");
        }
        mk8_installer::launch_nextendo(
            &request.install,
            &request.local,
            &request.username,
            request.candidate,
        )?
    } else {
        std::env::remove_var("MK8_NEXTENDO_SESSION");
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

fn ctgp_menu_overlay(request: &Request, pack: &std::path::Path) -> Result<Option<PathBuf>> {
    let pack_menu = pack.join("UI/cmn/menu.sarc");
    let custom_menu = pack_menu.try_exists().map_err(|e| e.to_string())?;
    if !custom_menu && request.install.menu_themes_supported {
        return Ok(None);
    }
    let menu = request
        .install
        .files
        .get("settings-layout")
        .ok_or("Atualize a build do jogo para combinar o CTGP com o menu da chave inglesa.")?;
    if !custom_menu {
        // The legacy host requires an explicit layout even if the pack keeps the original menu.
        return menu
            .path
            .ancestors()
            .nth(3)
            .map(|p| Some(p.to_path_buf()))
            .ok_or_else(|| "Caminho do layout do menu inválido.".into());
    }
    let output = request.local.join("derived/settings-ctgp");
    let python = if cfg!(windows) {
        "local/python/Scripts/python.exe"
    } else {
        "local/python/bin/python"
    };
    let mut command = std::process::Command::new(request.root.join(python));
    command
        .arg(request.root.join("toolchain/settings_layout.py"))
        .arg(&menu.path)
        .arg(pack_menu)
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
    Ok(Some(output))
}

/// What to do when Nextendo refused online play for the account; the reason is the one the
/// runtime logs (`[nextendo] online status: refused: <reason>`, runtime net/nextendo.c).
pub fn nextendo_refusal_code(log: &str) -> Option<&str> {
    let status = log
        .lines()
        .rev()
        .find_map(|line| line.strip_prefix("[nextendo] online status: "))?;
    if status == "refused" {
        return Some("");
    }
    let code = status.strip_prefix("refused: ")?.trim();
    (code.len() <= 64
        && code
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'_'))
    .then_some(code)
}

pub fn nextendo_message(reason: &str, language: &str) -> String {
    let pick = |pt, en, es| crate::language::pick(language, pt, en, es);
    match reason {
        "discord_unlinked" => pick(
            "O Nextendo recusou o jogo online desta conta: vincule uma conta do Discord em nextendo.network (página da conta) e tente de novo.",
            "Nextendo refused online play for this account: link a Discord account at nextendo.network (account page) and try again.",
            "Nextendo rechazó el juego en línea de esta cuenta: vincula una cuenta de Discord en nextendo.network (página de la cuenta) e inténtalo de nuevo.",
        ),
        "unverified" => pick(
            "O Nextendo recusou o jogo online desta conta: confirme o e-mail pela mensagem do Nextendo ou em nextendo.network.",
            "Nextendo refused online play for this account: verify its e-mail address (Nextendo's message or nextendo.network).",
            "Nextendo rechazó el juego en línea de esta cuenta: verifica el correo electrónico (mensaje de Nextendo o nextendo.network).",
        ),
        "elsewhere" => pick(
            "O Nextendo recusou o jogo online desta conta: ela está jogando em outro dispositivo agora.",
            "Nextendo refused online play for this account: it is playing on another device right now.",
            "Nextendo rechazó el juego en línea de esta cuenta: está jugando en otro dispositivo ahora.",
        ),
        "disabled" => pick(
            "O Nextendo recusou o jogo online: a conta está desativada.",
            "Nextendo refused online play: the account is disabled.",
            "Nextendo rechazó el juego en línea: la cuenta está desactivada.",
        ),
        "unknown" => pick(
            "O Nextendo não reconhece esta conta: entre novamente pelo navegador.",
            "Nextendo does not know this account: sign in again through the browser.",
            "Nextendo no reconoce esta cuenta: vuelve a iniciar sesión en el navegador.",
        ),
        "" => pick(
            "O Nextendo recusou o jogo online desta conta.",
            "Nextendo refused online play for this account.",
            "Nextendo rechazó el juego en línea de esta cuenta.",
        ),
        code => return format!("{} {code}", pick("O Nextendo recusou o acesso online:", "Nextendo refused online access:", "Nextendo rechazó el acceso en línea:")),
    }.to_owned()
}

pub fn exit_reason(log: &str, code: Option<i32>, language: &str) -> Option<String> {
    let pick = |pt, en, es| crate::language::pick(language, pt, en, es);
    if let Some(fatal) = log.lines().rev().find_map(|line| {
        line.strip_prefix("FATAL:")
            .map(str::trim)
            .filter(|s| !s.is_empty())
    }) {
        return Some(fatal.chars().take(400).collect());
    }
    let reason = match code.map(|n| n as u32) {
        Some(0xc0000135) => pick(
            "O Windows não encontrou uma DLL necessária para iniciar o jogo.",
            "Windows could not find a DLL required to start the game.",
            "Windows no encontró una DLL necesaria para iniciar el juego.",
        ),
        Some(0xc000007b) => pick(
            "O Windows recusou o formato de um executável ou de uma DLL do jogo.",
            "Windows rejected the format of a game executable or DLL.",
            "Windows rechazó el formato de un ejecutable o una DLL del juego.",
        ),
        _ => return None,
    };
    Some(reason.to_owned())
}

#[cfg(test)]
mod tests {
    use super::{exit_reason, nextendo_message, nextendo_refusal_code};

    #[test]
    fn reports_the_last_fatal_or_windows_loader_status_without_guessing() {
        let refusal = "[nextendo] online status: refused: discord_unlinked\n";
        let log = format!(
            "{refusal}FATAL: older error\nFATAL: SDL_Vulkan_CreateSurface: VK_KHR_win32_surface\n"
        );
        assert_eq!(
            exit_reason(&log, Some(1), "en").unwrap(),
            "SDL_Vulkan_CreateSurface: VK_KHR_win32_surface"
        );
        assert!(exit_reason("", Some(0xc0000135u32 as i32), "en")
            .unwrap()
            .contains("DLL"));
        assert!(exit_reason("", Some(0xc000007bu32 as i32), "es")
            .unwrap()
            .contains("formato"));
        assert_eq!(exit_reason(refusal, Some(1), "en"), None);
        assert_eq!(
            exit_reason(
                &format!("{refusal}[nextendo] online status: allowed"),
                Some(1),
                "en"
            ),
            None
        );
        assert_eq!(exit_reason("[vk] loader initialized", Some(1), "en"), None);
    }

    #[test]
    fn explains_nextendo_refusal_from_the_game_log() {
        let nextendo_refusal =
            |log, lang| nextendo_refusal_code(log).map(|code| nextendo_message(code, lang));
        let log = "[nextendo] game token ok (87 chars, 86400s)\n\
                   [nextendo] online status: refused: discord_unlinked\n\
                   BAAS adapter unavailable or online access refused.\n";
        assert!(nextendo_refusal(log, "en")
            .unwrap()
            .contains("link a Discord account"));
        assert!(nextendo_refusal(log, "pt-BR")
            .unwrap()
            .contains("vincule uma conta do Discord"));
        assert!(nextendo_refusal(log, "es")
            .unwrap()
            .contains("vincula una cuenta de Discord"));
        let unverified = "[nextendo] online status: refused: unverified\n";
        assert!(nextendo_refusal(unverified, "en")
            .unwrap()
            .contains("e-mail"));
        let bare = "[nextendo] online status: refused\n";
        assert_eq!(
            nextendo_refusal(bare, "en"),
            Some("Nextendo refused online play for this account.".to_owned())
        );
        assert_eq!(
            nextendo_refusal("[nextendo] online status: allowed\n", "en"),
            None
        );
        assert_eq!(nextendo_refusal("", "en"), None);
    }
}
