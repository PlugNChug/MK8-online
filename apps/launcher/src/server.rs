use mk8_build_manager::Manager;
use mk8_manifest::{load, save, Config, Installation, Result};
use mk8_session::{
    discover,
    p2p::{self, RoomTicket},
    steam::{self, SteamEvent, SteamRoom},
    Gateway, Host, Invite, DISCOVERY_PORT,
};
use serde_json::{json, Value};
use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    net::SocketAddr,
    path::{Path, PathBuf},
    process::Child,
    sync::{Arc, OnceLock},
    time::{Duration, Instant},
};
use tiny_http::{Header, Method, Request, Response, Server};

use crate::{
    console,
    relay::{self, InternetRoom},
    report, setup, update,
};

/// Window used as parent of native file dialogs; empty in headless mode.
pub type DialogParent = Arc<OnceLock<tauri::WebviewWindow>>;

#[derive(Clone, Copy)]
struct PendingPlay {
    role: &'static str,
    automatic: bool,
    two_player_guest: bool,
}

impl PendingPlay {
    fn auto_lan(self) -> Option<&'static str> {
        self.automatic.then_some(self.role)
    }
}

pub struct App {
    root: PathBuf,
    local: PathBuf,
    config: Config,
    build: Manager,
    setup: setup::Setup,
    dlc_import: setup::Setup,
    ctgp_import: setup::Setup,
    tools: Vec<Value>,
    host: Option<Host>,
    internet_room: Option<InternetRoom>,
    steam_room: Option<SteamRoom>,
    gateway: Option<Gateway>,
    game: Option<Child>,
    nextendo_session: crate::nextendo_session::Session,
    game_exit: Option<String>,
    nextendo_log: Option<fs::File>,
    nextendo_refusal: Option<String>,
    username: String,
    browser_language: String,
    prepare_after_setup: bool,
    prepare_dlc: Option<PathBuf>,
    prepare_dlc_running: bool,
    room_menu: u64,
    mii_menu: u64,
    window: Option<DialogParent>,
    /// Set by a started update: the server exits after answering, so the installer can replace the launcher.
    exit_requested: bool,
    last_report: Option<String>,
    native_job: Option<(Child, mk8_installer::mii_native::Pending)>,
    native_state: &'static str,
    native_detail: String,
    resources_job: Option<(Child, mk8_installer::mii_resources::Generation)>,
    resources_state: &'static str,
    resources_detail: String,
    mii_download: crate::mii_download::Download,
    graphics: crate::graphics::Components,
    /// The child running the terms-gated FSR SDK download/build.
    fsr_job: Option<crate::fsr::install::Job>,
    /// Steam lobby invites received from friends (lobby, friend), until accepted or the room is gone.
    steam_invites: Vec<(u64, u64)>,
    /// After creating or entering a Steam room: open the game in it as soon as the transport connects.
    pending_play: Option<PendingPlay>,
    steam_auto_join: bool,
    /// Invite opened in Steam before a first Mii exists; claimed once by the editor.
    steam_join_after_mii: Option<String>,
    /// The last Steam failure that happened outside a request (an accepted invite that could not be joined).
    steam_error: Option<String>,
    /// Steam was used before on this PC: connect again at start, so invites reach the launcher.
    steam_retry: Option<Instant>,
}
/// The console keys beside the base package (or in its `keys` folder) or in `%USERPROFILE%\.switch`.
fn detect_keys(config: &Config) -> Option<PathBuf> {
    let base = Path::new(&config.base_package);
    [
        base.parent().map(|p| p.join("prod.keys")),
        base.parent().map(|p| p.join("keys/prod.keys")),
        std::env::var_os("USERPROFILE")
            .map(PathBuf::from)
            .map(|p| p.join(".switch/prod.keys")),
    ]
    .into_iter()
    .flatten()
    .find(|p| p.is_file())
}

fn dlc_keys(config: &Config, package: &str, keys: &str) -> Option<PathBuf> {
    if !keys.is_empty() {
        return Some(keys.into());
    }
    if !config.prod_keys.is_empty() {
        return Some((&config.prod_keys).into());
    }
    let mut config = config.clone();
    config.base_package = package.into();
    detect_keys(&config)
}

fn is_dlc_package(path: &Path) -> bool {
    path.is_file()
        && path
            .extension()
            .is_some_and(|ext| ext.eq_ignore_ascii_case("nsp") || ext.eq_ignore_ascii_case("nsz"))
}

fn dlc_files(config: &Config, package: &str, keys: &str) -> Value {
    let selected = dlc_keys(config, package, keys);
    let package_path = Path::new(package);
    json!({
        "package_ok": is_dlc_package(package_path),
        "keys_ok": selected.as_ref().is_some_and(|path| path.is_file()),
        "keys_detected": keys.is_empty(),
        "keys": selected.map(|path| path.to_string_lossy().into_owned()),
    })
}

/// The three files of step 2 of the install screen, as the page shows them.
fn game_files(config: &Config, imported: bool) -> Value {
    let file = |path: &str| !path.is_empty() && Path::new(path).is_file();
    let keys = if config.prod_keys.is_empty() {
        detect_keys(config).map(|p| p.to_string_lossy().into_owned())
    } else {
        Some(config.prod_keys.clone())
    };
    let keys_ok = keys.as_deref().is_some_and(file);
    let packages = file(&config.base_package)
        && file(&config.update_package)
        && config.base_package != config.update_package;
    json!({
        "base_ok": file(&config.base_package),
        "update_ok": file(&config.update_package),
        "keys": keys,
        "keys_ok": keys_ok,
        "keys_detected": config.prod_keys.is_empty() && keys_ok,
        "folder": config.base_package.is_empty() && config.update_package.is_empty() && !config.exefs.is_empty(),
        "imported": imported,
        "ready": imported || (packages && keys_ok),
    })
}

fn valid_username(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 32
        && name.chars().count() <= 24
        && !name.chars().any(char::is_control)
}

impl App {
    pub fn new(
        root: PathBuf,
        local: PathBuf,
        config: Config,
        window: Option<DialogParent>,
    ) -> Self {
        let tools = setup::tool_checks(&root);
        let steam_auto_join = load(&local.join("steam-navigation.json")).unwrap_or(true);
        let username = load(&local.join("username.json"))
            .ok()
            .filter(|name: &String| valid_username(name))
            .unwrap_or_else(|| "Jogador".into());
        Self {
            root,
            local,
            config,
            build: Manager::default(),
            setup: setup::Setup::default(),
            dlc_import: setup::Setup::default(),
            ctgp_import: setup::Setup::default(),
            tools,
            host: None,
            internet_room: None,
            steam_room: None,
            gateway: None,
            game: None,
            nextendo_session: Default::default(),
            game_exit: None,
            nextendo_log: None,
            nextendo_refusal: None,
            username,
            browser_language: String::new(),
            prepare_after_setup: false,
            prepare_dlc: None,
            prepare_dlc_running: false,
            room_menu: 0,
            mii_menu: 0,
            window,
            exit_requested: false,
            last_report: None,
            native_job: None,
            native_state: "idle",
            native_detail: String::new(),
            resources_job: None,
            resources_state: "idle",
            resources_detail: String::new(),
            mii_download: Default::default(),
            graphics: Default::default(),
            fsr_job: None,
            steam_invites: Vec::new(),
            pending_play: None,
            steam_auto_join,
            steam_join_after_mii: None,
            steam_error: None,
            steam_retry: None,
        }
    }

    /// Rooms are entered as the player's Mii: its name is the name other players see.
    fn mii_name(&mut self) -> Result<String> {
        let profile = mk8_installer::mii::read(&self.local)?
            .ok_or("Crie um Mii em Perfil Mii para continuar.")?;
        let name = profile.appearance.name;
        if valid_username(&name) && name != self.username {
            save(&self.local.join("username.json"), &name)?;
            self.username = name.clone();
        }
        Ok(name)
    }

    fn session_busy(&self) -> bool {
        self.gateway.is_some()
            || self.host.is_some()
            || self.internet_room.is_some()
            || self.steam_room.is_some()
            || self.game.is_some()
    }

    fn imports_running(&self) -> bool {
        self.dlc_import.running() || self.ctgp_import.running()
    }

    /// The course pack the game receives at launch: its identity, empty with the pack off.
    fn pack_active(&self) -> Result<String> {
        match self.installation().or_else(|| self.candidate()) {
            Some(install) => Ok(
                mk8_installer::ctgp::launch_identity(&self.local, &install)?.unwrap_or_default()
            ),
            None => Ok(String::new()),
        }
    }

    /// Same Booster Course Pass state the game receives at launch (installed and enabled).
    fn dlc_active(&self) -> Result<bool> {
        match self.installation().or_else(|| self.candidate()) {
            Some(install) => Ok(mk8_installer::dlc::launch_root(&self.local, &install)?.is_some()),
            None => Ok(false),
        }
    }

    fn steam_host(&mut self, automatic: bool) -> Result<Value> {
        self.poll_game();
        self.check_room_preparation()?;
        if self.session_busy() {
            return Err("Encerre o jogo e a sessão atual antes de criar uma sala.".into());
        }
        self.mii_name()?;
        let (room, host, invite) = SteamRoom::host_live(
            &self.local,
            &self.identity(),
            &self.username,
            self.dlc_active()?,
            &self.pack_active()?,
        )?;
        let gateway = Gateway::start(invite)?;
        let code = room.code.clone();
        let lobby_id = room.lobby_id.to_string();
        self.gateway = Some(gateway);
        self.host = Some(host);
        self.steam_room = Some(room);
        self.pending_play = Some(PendingPlay {
            role: "host",
            automatic,
            two_player_guest: false,
        });
        self.steam_error = None;
        let _ = fs::write(self.local.join("steam-enabled"), b"1");
        Ok(
            json!({"code":code,"lobby_id":lobby_id,"message": if automatic {
                "Sala criada! Convide seus amigos da Steam. O jogo abre sozinho na sala."
            } else {
                "Sala Steam criada. No jogo, escolha seu Mii e abra LAN Play → Create Room. Depois convide seus amigos."
            }}),
        )
    }

    fn steam_join(&mut self, lobby: &str, two_player_guest: bool) -> Result<()> {
        self.check_room_preparation()?;
        self.mii_name()?;
        let install = self.installation();
        let qualified = install.is_some();
        let direct = install
            .or_else(|| self.candidate())
            .is_some_and(|game| game.direct_lan_guest_supported);
        let automatic = self.steam_auto_join && direct;
        if two_player_guest && !automatic {
            return Err(if direct {
                "Ative a entrada automática para entrar diretamente com 2 jogadores neste PC."
            } else if qualified {
                "Para entrar diretamente com 2 jogadores, atualize a instalação qualificada e ative a entrada automática. A instalação qualificada tem prioridade sobre a candidata."
            } else {
                "Para abrir a lista LAN de 2 jogadores, atualize o jogo preparado e ative a entrada automática."
            }.into());
        }
        self.prepare_steam_join()?;
        let identity = self.identity();
        let (room, gateway) = SteamRoom::join_live(
            &self.local,
            lobby,
            &identity,
            self.dlc_active()?,
            &self.pack_active()?,
        )?;
        self.steam_invites
            .retain(|(l, _)| l.to_string() != lobby.trim());
        self.steam_room = Some(room);
        self.gateway = Some(gateway);
        self.pending_play = Some(PendingPlay {
            role: "guest",
            automatic,
            two_player_guest,
        });
        self.steam_error = None;
        let _ = fs::write(self.local.join("steam-enabled"), b"1");
        Ok(())
    }

    fn clear_steam_session(&mut self) {
        self.pending_play = None;
        self.gateway = None;
        self.steam_room = None;
        self.host = None;
    }

    fn prepare_steam_join(&mut self) -> Result<()> {
        self.poll_game();
        // A closed game can leave an idle guest transport behind when the host
        // recreates its lobby. Stop its packet reader before joining the new one.
        if self.game.is_none()
            && self.pending_play.is_none()
            && self.host.is_none()
            && self.steam_room.is_some()
        {
            self.clear_steam_session();
        }
        if self.session_busy() {
            return Err("Feche o jogo e encerre a sala atual antes de entrar em outra.".into());
        }
        Ok(())
    }

    fn check_room_preparation(&mut self) -> Result<()> {
        if self.setup.running()
            || self.build.poll().state == "running"
            || self.imports_running()
            || self.native_job.is_some()
            || self.resources_job.is_some()
            || self.prepare_dlc.is_some()
        {
            return Err(
                "Aguarde a preparação terminar antes de abrir ou entrar em uma sala.".into(),
            );
        }
        Ok(())
    }

    /// Accepted invites, friends' join requests and received invites, from the Steam client.
    fn poll_steam(&mut self) {
        if self.local.join("steam-enabled").is_file()
            && !steam::probe_status(&self.local).ready
            && self
                .steam_retry
                .is_none_or(|at| at.elapsed() > Duration::from_secs(30))
        {
            self.steam_retry = Some(Instant::now());
            let _ = steam::friends(&self.local);
        }
        for event in steam::take_events() {
            match event {
                SteamEvent::Join { lobby } => {
                    self.auto_join_steam_lobby(&lobby.to_string());
                    if let Some(window) = self.window.as_ref().and_then(|parent| parent.get()) {
                        let _ = window.show();
                        let _ = window.unminimize();
                        let _ = window.set_focus();
                    }
                }
                SteamEvent::Invited { lobby, friend } => {
                    if !self.steam_invites.contains(&(lobby, friend)) {
                        self.steam_invites.push((lobby, friend));
                        self.steam_invites.truncate(8);
                    }
                }
            }
        }
        if let Some(play) = self.pending_play {
            if self.game.is_some() {
                self.pending_play = None;
            } else if self.gateway.as_ref().is_some_and(|g| g.connected()) {
                self.pending_play = None;
                if let Err(error) = self.start_game(true, Some(play), false) {
                    self.steam_error = Some(error);
                }
            }
        }
    }

    /// Starts the game (in the connected room when `online`); `auto_lan` makes it enter the LAN room by itself.
    pub fn play_direct(&mut self) -> Result<()> {
        self.start_game(false, None, false)?;
        let mut child = self.game.take().ok_or("O jogo não iniciou")?;
        let status = child.wait().map_err(|e| e.to_string())?;
        let shown = self.finish_game(status);
        if !status.success() {
            return Err(format!(
                "O jogo encerrou com erro: {shown}. Consulte game.log na pasta de dados."
            ));
        }
        Ok(())
    }

    fn start_game(
        &mut self,
        online: bool,
        play: Option<PendingPlay>,
        nextendo: bool,
    ) -> Result<u32> {
        let pilot = self.mii_name()?;
        if nextendo && (!mk8_installer::nextendo_trusted(&self.local) || online || play.is_some()) {
            return Err("Nextendo indisponível nesta sessão.".into());
        }
        self.dlc_import.poll();
        self.ctgp_import.poll();
        if self.imports_running() {
            return Err("Aguarde a importação do DLC ou do CTGP.".into());
        }
        self.poll_native();
        self.poll_resources();
        self.poll_fsr();
        if self.native_job.is_some() || self.resources_job.is_some() {
            return Err("Aguarde a conversão Mii terminar.".into());
        }
        if self.fsr_job.is_some() {
            return Err("Aguarde a instalação do FSR terminar.".into());
        }
        if self.game.is_some() {
            return Err("O jogo já está aberto.".into());
        }
        if self.setup.running() || self.imports_running() || self.build.poll().state == "running" {
            return Err("Aguarde a preparação.".into());
        }
        let install = self.installation();
        let net = if online {
            let gateway = self.gateway.as_ref().ok_or("Crie ou entre em uma sala")?;
            if !gateway.connected() {
                return Err("Anfitrião ainda não confirmou a conexão".into());
            }
            gateway.reset_game();
            Some((gateway.local_addr.to_string(), gateway.slot))
        } else {
            None
        };
        console::ensure_hidden();
        let candidate = install.is_none();
        let install = match install {
            Some(install) => install,
            None => load(&self.local.join("candidate.json"))?,
        };
        let child = crate::game_session::spawn(&crate::game_session::Request {
            root: self.root.clone(),
            local: self.local.clone(),
            install,
            network: net,
            username: pilot,
            candidate,
            browser_language: self.browser_language.clone(),
            auto_lan: play.and_then(PendingPlay::auto_lan).map(str::to_owned),
            two_player_guest: play.is_some_and(|p| p.two_player_guest),
            nextendo,
            nextendo_session: if nextendo {
                self.nextendo_session.name()?
            } else {
                None
            },
        })?;
        let pid = child.id();
        self.game_exit = None;
        self.nextendo_refusal = None;
        // Keep this session's log even if another launcher later rotates game.log.
        self.nextendo_log = if nextendo {
            fs::File::open(self.local.join("game.log")).ok()
        } else {
            None
        };
        self.game = Some(child);
        Ok(pid)
    }

    pub fn auto_join_steam_lobby(&mut self, lobby_id: &str) {
        if matches!(mk8_installer::mii::read(&self.local), Ok(None)) {
            self.steam_join_after_mii = Some(lobby_id.to_owned());
            return;
        }
        match self.steam_join(lobby_id, false) {
            Ok(()) => self.room_menu += 1,
            Err(error) => self.steam_error = Some(error),
        }
    }

    fn poll_resources(&mut self) {
        let exit = match self.resources_job.as_mut() {
            Some((child, _)) => child.try_wait(),
            None => return,
        };
        let exit = match exit {
            Ok(Some(exit)) => exit,
            Ok(None) => return,
            Err(error) => {
                self.resources_detail = error.to_string();
                return;
            }
        };
        let (_, generation) = self.resources_job.take().unwrap();
        let result = if exit.success() {
            mk8_installer::mii_resources::finish(&self.local, &generation).map(|_| ())
        } else {
            let log = fs::read_to_string(&generation.log).unwrap_or_default();
            let tail = log.lines().rev().take(3).collect::<Vec<_>>();
            Err(format!(
                "Falha ao gerar recursos Mii ({exit}). {}",
                tail.into_iter().rev().collect::<Vec<_>>().join("\n")
            ))
        };
        match result {
            Ok(()) => {
                self.resources_state = "done";
                self.resources_detail =
                    "Recursos Mii gerados e ativados para a próxima abertura do jogo.".into();
            }
            Err(error) => {
                self.resources_state = "failed";
                self.resources_detail = error;
            }
        }
    }

    fn resources_status(&mut self) -> Result<Value> {
        self.poll_resources();
        Ok(
            json!({"state":self.resources_state, "detail":self.resources_detail,
            "resources":mk8_installer::mii_resources::read(&self.local)?}),
        )
    }

    fn poll_native(&mut self) {
        let exit = match self.native_job.as_mut() {
            Some((child, _)) => child.try_wait(),
            None => return,
        };
        let exit = match exit {
            Ok(Some(exit)) => exit,
            Ok(None) => return,
            Err(error) => {
                self.native_detail = error.to_string();
                return;
            }
        };
        let (_, pending) = self.native_job.take().unwrap();
        let result = if exit.success() {
            mk8_installer::mii_native::publish(&self.local, pending)
        } else {
            let log = fs::read_to_string(&pending.log).unwrap_or_default();
            let tail = log
                .lines()
                .rev()
                .take(5)
                .collect::<Vec<_>>()
                .into_iter()
                .rev()
                .collect::<Vec<_>>()
                .join("\n");
            Err(format!("Falha na conversão Mii ({exit}). {tail}"))
        };
        match result {
            Ok(()) => {
                self.native_state = "done";
                self.native_detail = "Modelo preparado para a próxima abertura do jogo.".into();
            }
            Err(error) => {
                self.native_state = "failed";
                self.native_detail = error;
            }
        }
    }

    fn native_status(&mut self) -> Result<Value> {
        self.poll_native();
        let mut value = mk8_installer::mii_native::status(&self.root, &self.local)?;
        value["state"] = json!(self.native_state);
        value["detail"] = json!(self.native_detail);
        Ok(value)
    }

    fn poll_fsr(&mut self) {
        let Some(job) = self.fsr_job.as_mut() else {
            return;
        };
        match job.poll() {
            Ok(None) => (),
            Ok(Some(exit)) => {
                if !exit.success() {
                    job.record_interruption(&format!(
                        "A instalação do FSR terminou com erro ({exit}). Consulte o log."
                    ));
                }
                self.fsr_job = None;
            }
            Err(error) => {
                let _ = job.cancel();
                job.record_interruption(&format!(
                    "Não foi possível consultar a instalação do FSR: {error}"
                ));
                self.fsr_job = None;
            }
        }
    }

    fn fsr_page(&mut self) -> Result<Value> {
        self.poll_fsr();
        crate::fsr::page(&self.root, &self.local, self.fsr_job.is_some())
    }

    fn store_config(&mut self, config: Config) -> Result<()> {
        if self.prepare_after_setup
            || self.build.poll().state == "running"
            || self.imports_running()
        {
            return Err("Aguarde a preparação para alterar os caminhos.".into());
        }
        let _guards = mk8_build_manager::acquire_operation(&self.root, &self.local)?;
        save(&self.local.join("config.json"), &config)?;
        self.config = config;
        Ok(())
    }

    /// The upstream DLC importer verifies and updates a completed candidate.
    /// Keep optional import in this preparation flow, after a successful build.
    fn finish_optional_dlc(&mut self, state: &str) {
        if self.prepare_after_setup || self.setup.running() {
            return;
        }
        if self.prepare_dlc_running && !self.dlc_import.running() {
            self.prepare_dlc_running = false;
            if self.dlc_import.status()["state"] != "done" {
                self.build.record_start(Err(
                    "Falha na importação opcional de DLC. Confira o log de DLC em Instalar.".into(),
                ));
            }
        }
        if matches!(state, "failed" | "cancelled") {
            self.prepare_dlc = None;
        }
        if state != "candidate" {
            return;
        }
        let Some(package) = self.prepare_dlc.take() else {
            return;
        };
        let result = (|| {
            let keys = dlc_keys(
                &self.config,
                package.to_str().ok_or("Caminho do DLC inválido.")?,
                "",
            )
            .filter(|p| p.is_file())
            .ok_or("Selecione as chaves do seu console para importar o DLC.")?;
            self.dlc_import
                .start_dlc(&self.root, &self.local, &package, &keys, false)
        })();
        match result {
            Ok(()) => self.prepare_dlc_running = true,
            Err(error) => self.build.record_start(Err(error)),
        }
    }

    fn start_build(&mut self) -> Result<()> {
        self.tools = setup::tool_checks(&self.root);
        if !self.tools.iter().all(|check| check["ok"] == true) {
            return Err("Instale as ferramentas antes de iniciar a build.".into());
        }
        if self.imports_running() {
            return Err("Aguarde a importação do DLC ou do CTGP.".into());
        }
        {
            let _guards = mk8_build_manager::acquire_operation(&self.root, &self.local)?;
            save(&self.local.join("config.json"), &self.config)?;
        }
        self.build
            .start(&self.root, &self.local.join("config.json"), &self.local)
    }

    fn log_tail(&self, name: &str) -> Result<String> {
        match fs::File::open(self.local.join(name)) {
            Ok(file) => Self::read_log_tail(file),
            Err(_) => Ok(String::new()),
        }
    }

    fn read_log_tail(mut file: fs::File) -> Result<String> {
        let size = file.metadata().map_err(|e| e.to_string())?.len();
        file.seek(SeekFrom::Start(size.saturating_sub(32000)))
            .map_err(|e| e.to_string())?;
        let mut bytes = Vec::new();
        file.take(32000)
            .read_to_end(&mut bytes)
            .map_err(|e| e.to_string())?;
        Ok(String::from_utf8_lossy(&bytes).into_owned())
    }

    fn installation(&self) -> Option<Installation> {
        load::<Installation>(&self.local.join("active.json"))
            .ok()
            .filter(|install| {
                install.schema == 2
                    && install.profile == "mk8d-smm2-qualified"
                    && install.gameplay_verified
            })
    }
    fn menu_build(&self) -> crate::game_settings::MenuBuild {
        self.installation()
            .or_else(|| self.candidate())
            .as_ref()
            .map_or(crate::game_settings::MenuBuild::FiveThemes, |build| {
                build.into()
            })
    }

    fn split_screen_60fps_supported(&self) -> bool {
        self.installation()
            .or_else(|| self.candidate())
            .is_none_or(|game| game.split_screen_60fps_supported)
    }

    fn bloom_supported(&self) -> bool {
        self.installation()
            .or_else(|| self.candidate())
            .is_none_or(|game| game.bloom_supported)
    }

    fn controller_vibration_supported(&self) -> bool {
        self.installation()
            .or_else(|| self.candidate())
            .is_none_or(|game| game.controller_vibration_supported)
    }

    fn identity(&self) -> String {
        self.installation()
            .or_else(|| self.candidate())
            .and_then(|install| {
                // The build identity contains local paths and differs between PCs.
                let hashes = ["npdm", "main", "sdk", "subsdk0"]
                    .iter()
                    .map(|key| install.files.get(*key).map(|file| file.sha256.as_str()))
                    .collect::<Option<Vec<_>>>()?;
                Some(format!("mk8-lan-v1:{}", hashes.join(":")))
            })
            .unwrap_or_else(|| "transport-fixture-v1".into())
    }
    fn candidate(&self) -> Option<Installation> {
        load::<Installation>(&self.local.join("candidate.json"))
            .ok()
            .filter(|install| {
                install.schema == 2
                    && install.profile == "mk8d-smm2-experimental"
                    && !install.gameplay_verified
            })
    }
    fn poll_game(&mut self) {
        if let Some(exit) = self.game.as_mut().and_then(|c| c.try_wait().ok().flatten()) {
            self.game = None;
            self.finish_game(exit);
        }
    }

    fn finish_game(&mut self, exit: std::process::ExitStatus) -> String {
        if self.steam_room.is_some() {
            self.clear_steam_session();
        }
        let session_log = self.nextendo_log.take();
        self.nextendo_refusal = None;
        let mut shown = exit.to_string();
        if !exit.success() {
            let nextendo = session_log.is_some();
            let log = match session_log {
                Some(file) => Self::read_log_tail(file),
                None => self.log_tail("game.log"),
            }
            .unwrap_or_default();
            let language = crate::language::effective(&self.local);
            let detail =
                crate::game_session::exit_reason(&log, exit.code(), &language).or_else(|| {
                    if !nextendo {
                        return None;
                    }
                    let code = crate::game_session::nextendo_refusal_code(&log)?;
                    if exit.code() == Some(2) {
                        self.nextendo_refusal = Some(code.to_owned());
                    }
                    Some(crate::game_session::nextendo_message(code, &language))
                });
            if let Some(reason) = &detail {
                shown = format!("{shown}. {reason}");
            }
            let summary = json!({"game_exit": exit.to_string(), "game_exit_detail": detail, "build": self.build.poll().state});
            let install = self.installation().or_else(|| self.candidate());
            let runtime = install.as_ref().and_then(|i| i.executable.path.parent());
            self.last_report = report::create(&self.local, runtime, "game-exit", summary)
                .ok()
                .map(|p| p.display().to_string());
        }
        self.game_exit = Some(shown.clone());
        shown
    }

    fn status(&mut self) -> Value {
        self.dlc_import.poll();
        self.ctgp_import.poll();
        self.poll_native();
        self.poll_resources();
        let mii_preparation = json!({
            "native": {"state": self.native_state, "detail": self.native_detail},
            "resources": {"state": self.resources_state, "detail": self.resources_detail}
        });
        self.poll_game();
        self.poll_steam();
        let signal = self.local.join("open-rooms");
        if self.game.is_some() && fs::remove_file(self.local.join("open-miis")).is_ok() {
            self.mii_menu += 1;
            if let Some(window) = self.window.as_ref().and_then(|parent| parent.get()) {
                let _ = window.show();
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
        }
        if self.game.is_some() && fs::remove_file(signal).is_ok() {
            self.room_menu += 1;
            if let Some(window) = self.window.as_ref().and_then(|parent| parent.get()) {
                let _ = window.show();
                let _ = window.unminimize();
                let _ = window.set_focus();
            }
        }
        if self.setup.poll() {
            self.tools = setup::tool_checks(&self.root);
            if std::mem::take(&mut self.prepare_after_setup)
                && self.setup.status()["state"] == "done"
            {
                let started = self.start_build();
                self.build.record_start(started);
            }
            if self.setup.status()["state"] != "done" {
                self.prepare_dlc = None;
            }
        }
        // A build records the launcher version that made it: the runtime fixes come with each version, so a game built
        // by an earlier one (an update kept the data folder) is offered for a rebuild, which reuses the finished stages.
        let build = self.build.poll();
        self.finish_optional_dlc(&build.state);
        let build = self.build.poll();
        let built_with = self.local.join("built-with.json");
        if build.state == "candidate"
            && load::<Value>(&built_with)
                .ok()
                .and_then(|v| v["version"].as_str().map(str::to_owned))
                != Some(update::CURRENT.to_owned())
        {
            let _ = save(&built_with, &json!({ "version": update::CURRENT }));
        }
        let candidate_exists = self.local.join("candidate.json").is_file();
        let candidate_version = load::<Value>(&built_with)
            .ok()
            .and_then(|v| v["version"].as_str().map(str::to_owned));
        let install = self.installation();
        let candidate = self.candidate();
        let nextendo_supported = install
            .as_ref()
            .or(candidate.as_ref())
            .is_some_and(|game| game.nextendo_supported);
        let runtime_upgrade = candidate.as_ref().is_some_and(|game| {
            if !game.menu_themes_supported || !game.menu_light_themes_supported {
                return true;
            }
            // An existing host can predate the capability fields within the same test version.
            // A matching host alone does not establish that the guest calls the cadence hook.
            !game.split_screen_60fps_supported
                || !game.bloom_supported
                || !game.controller_vibration_supported
                || (!game.temporal_upscaling_supported
                    || !game.romfs_long_paths_supported
                    || !game.nextendo_session_supported
                    || !game.live_settings_supported)
                    && !load::<Value>(&self.root.join("prebuilt/host/manifest.json")).is_ok_and(
                        |host| {
                            host["files"]["mk8drecomp.exe"].as_str()
                                == Some(game.executable.sha256.as_str())
                        },
                    )
        });
        let candidate_stale = candidate_exists
            && (candidate_version.as_deref() != Some(update::CURRENT)
                || !nextendo_supported
                || candidate
                    .as_ref()
                    .is_some_and(|game| !game.direct_lan_guest_supported)
                || runtime_upgrade);
        let dlc =
            mk8_installer::dlc::status(&self.local, install.as_ref().or(self.candidate().as_ref()));
        let ctgp = mk8_installer::ctgp::status(
            &self.local,
            install.as_ref().or(self.candidate().as_ref()),
        );
        let package_import =
            !self.config.base_package.is_empty() || !self.config.update_package.is_empty();
        let imported = self.local.join("imports/mk8-4.0.1");
        let mut paths = self.config.clone();
        if package_import && imported.is_dir() {
            paths.exefs = imported.join("exefs").to_string_lossy().into_owned();
            paths.romfs = imported.join("romfs").to_string_lossy().into_owned();
            paths.nacp = imported
                .join("control/control.nacp")
                .to_string_lossy()
                .into_owned();
        }
        let steam_status = steam::probe_status(&self.local);
        let p2p_code = self.host.as_ref().and_then(|host| {
            if self.internet_room.is_some() || self.steam_room.is_some() {
                None
            } else {
                host.room_ticket(None).ok().map(|t| t.encode())
            }
        });
        let room_code = self
            .internet_room
            .as_ref()
            .map(|room| room.code.clone())
            .or_else(|| self.steam_room.as_ref().map(|room| room.code.clone()))
            .or(p2p_code);
        let mut status = json!({"dlc":dlc,"dlc_import":self.dlc_import.status(),"ctgp":ctgp,"ctgp_import":self.ctgp_import.status(),"mii_preparation":mii_preparation,"package_import":package_import,"import_ready":package_import && imported.is_dir(),"room_menu":self.room_menu,"setup":self.setup.status(),"tools":self.tools,"tools_ready":self.tools.iter().all(|c|c["ok"]==true),"native_dialogs":self.window.is_some(),"build":build,"candidate":candidate_exists,"candidate_stale":candidate_stale,"candidate_version":candidate_version,"game_exit":self.game_exit,"installed":install.is_some(),"profile":install.as_ref().map(|i|&i.profile),"gameplay_verified":install.as_ref().is_some_and(|i|i.gameplay_verified),"game_pid":self.game.as_ref().map(|c|c.id()),"session":{"hosting":self.host.is_some(),"internet_available":relay::url().is_some(),"room_code":room_code,"stun_endpoint":self.host.as_ref().and_then(|h|h.stun_endpoint.map(|e|e.to_string())),"steam_available":steam_status.dll_found,"steam_active":self.steam_room.is_some(),"steam_lobby_id":self.steam_room.as_ref().map(|r|r.lobby_id.to_string()),"steam_persona":steam_status.persona,"steam_ready":steam_status.ready,"steam_invites":self.steam_invites.len(),"steam_error":self.steam_error,"pending_play":self.pending_play.map(|play| play.role),"steam_auto_join":self.steam_auto_join,"relay":self.host.as_ref().map(|h|h.address.to_string()),"participants":self.host.as_ref().map(|h|h.connected()).unwrap_or(0),"connected":self.gateway.as_ref().is_some_and(|g|g.connected()),"slot":self.gateway.as_ref().map(|g|g.slot),"bridge_port":self.gateway.as_ref().map(|g|g.local_addr.port()),"diagnostic_only":install.is_none() && self.candidate().is_none()},"checks":mk8_installer::inspect(&paths),"files":game_files(&self.config, package_import && imported.is_dir()),"upstream":"Runtime local","root":self.root,"version":update::CURRENT,"updates":{"configured":update::repository().is_some(),"installable":update::installable()},"last_report":self.last_report,"data_dir":self.local,"windowed":self.window.is_some(),"platform":std::env::consts::OS});
        status["session"]["direct_lan_guest_supported"] = json!(install
            .as_ref()
            .or(candidate.as_ref())
            .is_some_and(|game| game.direct_lan_guest_supported));
        status["nextendo_trusted"] = json!(mk8_installer::nextendo_trusted(&self.local));
        status["nextendo_refusal_message"] = json!(self.nextendo_refusal.as_deref().map(|code| {
            crate::game_session::nextendo_message(code, &crate::language::effective(&self.local))
        }));
        status["steam_join_after_mii"] = json!(self.steam_join_after_mii.is_some());
        status["nextendo_supported"] =
            json!(nextendo_supported || mk8_installer::nextendo_test_configured());
        status["nextendo_game_key"] = json!(mk8_installer::NEXTENDO_GAME_KEY);
        status
    }
    fn handle(&mut self, method: &Method, path: &str, body: &str) -> Result<Value> {
        self.dlc_import.poll();
        self.ctgp_import.poll();
        self.poll_fsr();
        if method == &Method::Post
            && self.imports_running()
            && !matches!(path, "/api/language" | "/api/browse" | "/api/dlc/files")
        {
            return Err("Aguarde a importação do DLC ou do CTGP.".into());
        }
        match (method.as_str(), path) {
            ("POST", "/api/dlc/files") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(dlc_files(
                    &self.config,
                    value["package"].as_str().unwrap_or(""),
                    value["keys"].as_str().unwrap_or(""),
                ))
            }
            ("GET", "/api/graphics") => self.graphics.status(&self.local),
            ("POST", "/api/graphics/license") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                crate::graphics::license(&self.local, value["vendor"].as_str().unwrap_or(""))
            }
            ("POST", "/api/graphics/install") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                self.graphics.start(&self.local, &value)
            }
            ("GET", "/api/settings") => {
                let mut settings = crate::game_settings::read(&self.local, self.menu_build())?;
                settings["split_screen_60fps_supported"] =
                    json!(self.split_screen_60fps_supported());
                settings["bloom_supported"] = json!(self.bloom_supported());
                settings["controller_vibration_supported"] =
                    json!(self.controller_vibration_supported());
                Ok(settings)
            }
            ("GET", "/api/debug-options") => Ok(json!(crate::debug_options::read(&self.local)?)),
            ("GET", "/api/fonts") => crate::fonts::status(&self.local),
            ("POST", "/api/settings")
            | ("POST", "/api/debug-options")
            | ("POST", "/api/fonts")
            | ("POST", "/api/fsr/settings")
            | ("POST", "/api/dlc/toggle")
            | ("POST", "/api/dlc/import")
            | ("POST", "/api/ctgp/toggle")
            | ("POST", "/api/ctgp/import") => {
                self.status();
                if self.session_busy()
                    || self.setup.running()
                    || self.imports_running()
                    || self.build.poll().state == "running"
                {
                    return Err("Feche o jogo ou aguarde a operação atual.".into());
                }
                let guards = mk8_build_manager::acquire_operation(&self.root, &self.local)?;
                let pipeline = fs::OpenOptions::new()
                    .create(true)
                    .truncate(false)
                    .read(true)
                    .write(true)
                    .open(self.local.join(".build.lock"))
                    .map_err(|e| e.to_string())?;
                pipeline
                    .try_lock()
                    .map_err(|_| "Já existe uma preparação usando esta pasta de dados.")?;
                if path == "/api/debug-options" {
                    let mut options: crate::debug_options::Options =
                        serde_json::from_str(body).map_err(|e| e.to_string())?;
                    let previous = crate::debug_options::read(&self.local)?.legacy_native;
                    if options
                        .legacy_native
                        .is_some_and(|value| value != previous.unwrap_or(false))
                    {
                        return Err("Selecione o tema do menu em Configurações → Vídeo.".into());
                    }
                    options.legacy_native = previous;
                    crate::debug_options::write(&self.local, &options)?;
                    return Ok(
                        json!({"message":"Preferência salva. A alteração vale ao iniciar o jogo."}),
                    );
                }
                if path == "/api/fonts" {
                    let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                    let result = if let Some(directory) = value["directory"].as_str() {
                        crate::fonts::import(&self.local, Path::new(directory))?
                    } else {
                        crate::fonts::select(
                            &self.local,
                            value["enabled"]
                                .as_bool()
                                .ok_or("Preferência de fontes inválida.")?,
                        )?
                    };
                    return Ok(
                        json!({"fonts":result,"message":"Fontes salvas. As alterações valem ao iniciar o jogo."}),
                    );
                }
                if path == "/api/settings" {
                    let mut value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                    if let Some(mode) = value["upscaler"].as_str() {
                        crate::graphics::validate(&self.local, mode)?;
                    }
                    let build_options = [
                        ("bloom", Self::bloom_supported as fn(&Self) -> bool, "Atualize o runtime do jogo para ajustar o bloom."),
                        ("split_screen_60fps", Self::split_screen_60fps_supported, "Atualize a build usada para jogar para ajustar os 60 FPS em tela dividida."),
                        ("controller_vibration", Self::controller_vibration_supported, "Atualize a build do jogo para ajustar a vibração do controle."),
                    ];
                    for (key, supported, update_message) in build_options {
                        if value.get(key).is_some() && !supported(self) {
                            // Full-form clients can echo a disabled field while updating unrelated settings.
                            let current =
                                crate::game_settings::read(&self.local, self.menu_build())?;
                            if value[key] != current["values"][key] {
                                return Err(update_message.into());
                            }
                            if let Some(patch) = value.as_object_mut() {
                                patch.remove(key);
                            }
                        }
                    }
                    crate::game_settings::write(&self.local, &value, self.menu_build())?;
                    return Ok(
                        json!({"message":"Configurações salvas. As alterações valem ao iniciar o jogo."}),
                    );
                }
                if path == "/api/fsr/settings" {
                    let settings: crate::fsr::Settings =
                        serde_json::from_str(body).map_err(|_| "Preferências FSR inválidas.")?;
                    crate::fsr::write_settings(&self.local, &settings)?;
                    return Ok(json!({
                        "settings": settings,
                        "message": "Preferências FSR salvas. Valem ao iniciar o jogo.",
                    }));
                }
                let install = self.installation().or_else(|| self.candidate()).ok_or(
                    if path.starts_with("/api/ctgp/") {
                        "Prepare o jogo antes de importar o CTGP."
                    } else {
                        "Prepare o jogo antes de importar DLC."
                    },
                )?;
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                if path == "/api/ctgp/toggle" {
                    let enabled = value["enabled"]
                        .as_bool()
                        .ok_or("Preferência do CTGP inválida.")?;
                    mk8_installer::ctgp::set_enabled(&self.local, &install, enabled)?;
                    return Ok(json!({"message": "Preferência do CTGP salva."}));
                }
                if path == "/api/ctgp/import" {
                    if !install.pack_supported {
                        return Err("Atualize a build para usar o CTGP.".into());
                    }
                    let source = value["source"]
                        .as_str()
                        .filter(|s| !s.is_empty())
                        .map(PathBuf::from)
                        .filter(|p| p.is_dir())
                        .ok_or("Selecione a pasta extraída do CTGP Deluxe.")?;
                    let romfs = install.data_root.join("extracted/romfs");
                    if !romfs.join("Course").is_dir() {
                        return Err("Prepare o jogo antes de importar o CTGP.".into());
                    }
                    let reinstall = value
                        .get("reinstall")
                        .map(|value| value.as_bool().ok_or("Opção de reinstalação inválida."))
                        .transpose()?
                        .unwrap_or(false);
                    let layout = match value
                        .get("layout")
                        .and_then(Value::as_str)
                        .unwrap_or("base")
                    {
                        "base" => "base",
                        "dlc" => "dlc",
                        "dlc-late" => "dlc-late",
                        "added" => "added",
                        _ => return Err("Disposição das copas do CTGP inválida.".into()),
                    };
                    drop(guards);
                    drop(pipeline);
                    self.ctgp_import.start_ctgp(
                        &self.root,
                        &self.local,
                        &source,
                        &romfs,
                        reinstall,
                        layout,
                    )?;
                    return Ok(json!({"message": "Importação do CTGP iniciada."}));
                }
                if path == "/api/dlc/toggle" {
                    let enabled = value["enabled"]
                        .as_bool()
                        .ok_or("Preferência de DLC inválida.")?;
                    mk8_installer::dlc::set_enabled(&self.local, &install, enabled)?;
                    return Ok(json!({"message": "Preferência de DLC salva."}));
                }
                let package = value["package"]
                    .as_str()
                    .filter(|s| !s.is_empty())
                    .ok_or("Selecione o NSP/NSZ do DLC.")?;
                if !install.dlc_supported {
                    return Err("Atualize a build para usar DLC.".into());
                }
                let package = Path::new(package);
                if !is_dlc_package(package) {
                    return Err("Selecione o NSP/NSZ do DLC.".into());
                }
                let keys = dlc_keys(
                    &self.config,
                    package.to_str().ok_or("Caminho do DLC inválido.")?,
                    value["keys"].as_str().unwrap_or(""),
                )
                .filter(|p| p.is_file())
                .ok_or("Selecione as chaves do seu console para importar o DLC.")?;
                let reinstall = value
                    .get("reinstall")
                    .map(|value| value.as_bool().ok_or("Opção de reinstalação inválida."))
                    .transpose()?
                    .unwrap_or(false);
                drop(guards);
                drop(pipeline);
                self.dlc_import
                    .start_dlc(&self.root, &self.local, package, &keys, reinstall)?;
                Ok(json!({"message": "Importação do DLC iniciada."}))
            }
            ("GET", "/api/dlc/log") => Ok(json!({"text":self.log_tail("dlc.log")?})),
            ("GET", "/api/ctgp/log") => Ok(json!({"text":self.log_tail("ctgp.log")?})),
            ("GET", "/api/fsr") => self.fsr_page(),
            ("POST", "/api/fsr/license") => {
                let dir = crate::fsr::data_dir();
                let text = crate::fsr::install::fetch_license(&dir)?;
                Ok(json!({
                    "text": text,
                    "license": crate::fsr::license_status(&dir),
                    "message": "Licença da AMD carregada da fonte oficial (SDK v2.4.0).",
                }))
            }
            ("POST", "/api/fsr/accept") => {
                let dir = crate::fsr::data_dir();
                crate::fsr::install::accept_terms(&dir)?;
                Ok(json!({
                    "license": crate::fsr::license_status(&dir),
                    "message": "Termos da AMD aceitos. O download só começa quando você pedir.",
                }))
            }
            ("POST", "/api/fsr/install") => {
                self.poll_fsr();
                if self.fsr_job.is_some() {
                    return Err("A instalação do FSR já está em andamento.".into());
                }
                self.status();
                if self.session_busy()
                    || self.setup.running()
                    || self.imports_running()
                    || self.build.poll().state == "running"
                {
                    return Err(
                        "Feche o jogo e aguarde a operação atual antes de instalar o FSR.".into(),
                    );
                }
                let settings = crate::fsr::read_settings(&self.local)?;
                if cfg!(windows) {
                    let gate = crate::fsr::fsr4_gate(&settings);
                    if crate::fsr::fsr4_selection_blocked(&settings, &gate) {
                        return Err(gate.reason);
                    }
                }
                if cfg!(target_os = "linux")
                    && settings.upscaling != "3.1.6"
                    && settings.upscaling != "4.1.2"
                    && settings.frame_generation != "fsr3"
                {
                    return Err(
                        "Nada para instalar: selecione FSR 3.1.6, FSR 4.1.2 ou FSR 3 nos componentes locais."
                            .into(),
                    );
                }
                if !crate::fsr::terms_accepted(&crate::fsr::data_dir()) {
                    return Err("Leia e aceite a licença da AMD antes de baixar o SDK.".into());
                }
                self.fsr_job = Some(crate::fsr::install::Job::start(
                    &self.root,
                    &self.local,
                    &settings,
                )?);
                self.fsr_page()
            }
            ("POST", "/api/fsr/cancel") => {
                if let Some(mut job) = self.fsr_job.take() {
                    job.cancel()?;
                }
                Ok(json!({"message":"Instalação do FSR cancelada."}))
            }
            ("GET", "/api/fsr/log") => Ok(json!({
                "text": crate::fsr::install::log_tail(&crate::fsr::data_dir(), 200),
            })),
            ("GET", "/api/status") => Ok(self.status()),
            ("GET", "/api/config") => Ok(serde_json::to_value(&self.config).unwrap()),
            ("GET", "/api/username") => Ok(json!(self.username)),
            ("GET", "/api/mii/active") => Ok(json!(
                mk8_installer::mii::read(&self.local)?.map(|profile| profile.appearance.name)
            )),
            ("GET", "/api/mii") => Ok(json!({"active":mk8_installer::mii::read(&self.local)?,
                "profiles":mk8_installer::mii::list(&self.local)?, "menu":self.mii_menu})),
            ("GET", "/api/mii/menu") => Ok(json!({"menu":self.mii_menu})),
            ("GET", "/api/mii/fields") => Ok(json!(mk8_installer::mii_data::fields())),
            ("GET", "/api/mii/native") => self.native_status(),
            ("POST", "/api/mii/native/prepare") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let expected: Vec<u8> = serde_json::from_value(value["expected"].clone())
                    .map_err(|_| "Reabra o editor Mii antes de preparar.")?;
                mk8_installer::mii_native::check_profile(&self.local, &expected)?;
                self.status();
                self.poll_native();
                if self.native_job.is_some()
                    || self.game.is_some()
                    || self.setup.running()
                    || self.build.poll().state == "running"
                {
                    return Err(
                        "Feche o jogo e aguarde a operação atual antes de preparar o Mii.".into(),
                    );
                }
                let (command, pending) =
                    mk8_installer::mii_native::prepare(&self.root, &self.local)?;
                let child = mk8_build_manager::supervisor::spawn(
                    &command,
                    &self.root,
                    &self.local,
                    &pending.log,
                )?;
                self.native_job = Some((child, pending));
                self.native_state = "running";
                self.native_detail = "Convertendo a cabeça para o jogo…".into();
                self.native_status()
            }
            ("POST", "/api/mii/native/enabled") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let expected: Vec<u8> = serde_json::from_value(value["expected"].clone())
                    .map_err(|_| "Reabra o editor Mii antes de ativar.")?;
                mk8_installer::mii_native::check_profile(&self.local, &expected)?;
                if self.native_job.is_some() {
                    return Err("Aguarde a conversão Mii terminar.".into());
                }
                mk8_installer::mii_native::set_enabled(
                    &self.local,
                    value["enabled"].as_bool().ok_or("Opção inválida")?,
                )?;
                self.native_state = "idle";
                self.native_detail.clear();
                self.native_status()
            }
            ("GET", "/api/mii/composer/download") => Ok(self.mii_download.status(&self.local)),
            ("POST", "/api/mii/composer/download") => self.mii_download.start(&self.local),
            ("GET", "/api/mii/composer") => Ok(
                json!({"resource":mk8_installer::mii_composer::resource_config(&self.local)?, "head":mk8_installer::mii_composer::saved(&self.local)?}),
            ),
            ("POST", "/api/mii/composer/resource") => {
                let value: mk8_installer::mii_composer::Resource =
                    serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii_composer::configure(
                    &self.local,
                    &value.path
                )?))
            }
            ("POST", "/api/mii/composer/save") => {
                let value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii_composer::store(
                    &self.local,
                    value
                )?))
            }
            ("POST", "/api/mii/data") => {
                let data: Vec<u8> = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii::update_data(&self.local, &data)?))
            }
            ("GET", "/api/mii/export") => {
                let profile = mk8_installer::mii::read(&self.local)?
                    .ok_or("Crie ou importe um Mii primeiro.")?;
                Ok(json!({"data":profile.data()?}))
            }
            ("GET", "/api/mii/resources") => {
                Ok(json!(mk8_installer::mii_resources::read(&self.local)?))
            }
            ("GET", "/api/mii/resources/job") => self.resources_status(),
            ("POST", "/api/mii/resources/generate") => {
                self.status();
                self.poll_resources();
                self.poll_native();
                if self.resources_job.is_some()
                    || self.native_job.is_some()
                    || self.game.is_some()
                    || self.setup.running()
                    || self.build.poll().state == "running"
                {
                    return Err(
                        "Feche o jogo e aguarde a operação atual antes de gerar os recursos Mii."
                            .into(),
                    );
                }
                let (command, generation) =
                    mk8_installer::mii_resources::generate(&self.root, &self.local)?;
                let child = mk8_build_manager::supervisor::spawn(
                    &command,
                    &self.root,
                    &self.local,
                    &generation.log,
                )?;
                self.resources_job = Some((child, generation));
                self.resources_state = "running";
                self.resources_detail = "Gerando recursos Mii a partir da biblioteca local…".into();
                self.resources_status()
            }
            ("POST", "/api/mii/resources") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii_resources::configure(
                    &self.local,
                    Path::new(value["directory"].as_str().unwrap_or("")),
                    value["enabled"].as_bool().unwrap_or(false)
                )?))
            }
            ("POST", "/api/mii/preview") => {
                let face = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::avatar::head(&face)?))
            }
            ("POST", "/api/mii/save" | "/api/mii/create") => {
                let appearance = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let profile = if path.ends_with("create") {
                    mk8_installer::mii::create(&self.local, appearance)?
                } else {
                    mk8_installer::mii::store(&self.local, appearance)?
                };
                Ok(
                    json!({"profile":profile,"message":"Perfil salvo. Alterações serão usadas na próxima abertura do jogo."}),
                )
            }
            ("POST", "/api/mii/select") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii::select(
                    &self.local,
                    value["id"].as_str().unwrap_or("")
                )?))
            }
            ("POST", "/api/mii/import") => {
                let data: Vec<u8> = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(mk8_installer::mii::import(&self.local, &data)?))
            }
            ("POST", "/api/language") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                Ok(json!(crate::language::write(&self.local, &value)?))
            }
            ("POST", "/api/username") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let name = value["name"].as_str().unwrap_or("").trim();
                if !valid_username(name) {
                    return Err("Informe um nome de até 24 caracteres e 32 bytes.".into());
                }
                if self.host.is_some() {
                    return Err("Encerre sua sala antes de alterar o nome.".into());
                }
                save(&self.local.join("username.json"), &name)?;
                self.username = name.into();
                Ok(json!({"message":"Nome salvo neste computador."}))
            }
            ("POST", "/api/game-folder") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let path = value["path"]
                    .as_str()
                    .filter(|p| !p.is_empty())
                    .ok_or("Escolha uma pasta")?;
                let root = crate::canonical(Path::new(path)).map_err(|e| e.to_string())?;
                let exefs = root.join("exefs");
                let romfs = root.join("romfs");
                let nacp = [root.join("control.nacp"), root.join("control/control.nacp")]
                    .into_iter()
                    .find(|p| p.is_file())
                    .ok_or("control.nacp ausente na pasta selecionada.")?;
                if !exefs.join("main.npdm").is_file() || !romfs.join("Course").is_dir() {
                    return Err("A pasta deve conter exefs/main.npdm e romfs/Course.".into());
                }
                let mut config = self.config.clone();
                config.exefs = exefs.to_string_lossy().into_owned();
                config.romfs = romfs.to_string_lossy().into_owned();
                config.nacp = nacp.to_string_lossy().into_owned();
                config.base_package.clear();
                config.update_package.clear();
                self.store_config(config)?;
                Ok(json!({"config":self.config,"message":"Pasta do jogo salva."}))
            }
            ("POST", "/api/config") => {
                self.store_config(serde_json::from_str(body).map_err(|e| e.to_string())?)?;
                Ok(json!({"message":"Caminhos salvos."}))
            }
            ("POST", "/api/prepare") => {
                self.status();
                if self.session_busy()
                    || self.setup.running()
                    || self.imports_running()
                    || self.build.poll().state == "running"
                    || self.native_job.is_some()
                    || self.resources_job.is_some()
                {
                    return Err("Feche o jogo ou aguarde a operação atual.".into());
                }
                if self.config.base_package.is_empty() && self.config.update_package.is_empty() {
                    let check = mk8_installer::architecture_check(&self.config);
                    if !check.ok {
                        return Err(check.detail);
                    }
                } else if !self.local.join("imports/mk8-4.0.1").is_dir() {
                    let base = Path::new(&self.config.base_package);
                    let patch = Path::new(&self.config.update_package);
                    if !base.is_file() || !patch.is_file() || base == patch {
                        return Err(
                            "Selecione dois arquivos: jogo base e atualização 4.0.1.".into()
                        );
                    }
                    if self.config.prod_keys.is_empty() {
                        if let Some(keys) = detect_keys(&self.config) {
                            self.config.prod_keys = keys.to_string_lossy().into_owned();
                        }
                    }
                    if !Path::new(&self.config.prod_keys).is_file() {
                        return Err("prod.keys não encontrado. Selecione as chaves do seu console no campo da importação.".into());
                    }
                }
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let package = value["dlc_package"].as_str().unwrap_or("").trim();
                let dlc = if package.is_empty() {
                    None
                } else {
                    if !is_dlc_package(Path::new(package)) {
                        return Err(
                            "Selecione um NSP/NSZ válido para a DLC opcional ou deixe o campo vazio."
                                .into(),
                        );
                    }
                    dlc_keys(&self.config, package, "")
                        .filter(|p| p.is_file())
                        .ok_or("Selecione as chaves do seu console para importar o DLC.")?;
                    Some(PathBuf::from(package))
                };
                self.tools = setup::tool_checks(&self.root);
                if self.tools.iter().all(|tool| tool["ok"] == true) {
                    self.start_build()?;
                    self.prepare_dlc = dlc;
                    Ok(json!({"message":"Preparando o jogo."}))
                } else {
                    self.setup.start_guarded(&self.root, &self.local)?;
                    self.prepare_dlc = dlc;
                    self.prepare_after_setup = true;
                    Ok(json!({"message":"Instalando ferramentas. A preparação começa em seguida."}))
                }
            }
            ("POST", "/api/setup") => {
                self.status();
                if self.game.is_some() {
                    return Err("Feche o jogo antes de instalar ferramentas.".into());
                }
                if self.build.poll().state == "running" || self.imports_running() {
                    return Err(
                        "Aguarde ou cancele a preparação antes de instalar ferramentas.".into(),
                    );
                }
                self.setup.start_guarded(&self.root, &self.local)?;
                Ok(json!({"message":"Instalação de ferramentas iniciada. Acompanhe o progresso."}))
            }
            ("POST", "/api/setup/cancel") => {
                self.setup.cancel()?;
                self.prepare_after_setup = false;
                self.prepare_dlc = None;
                self.tools = setup::tool_checks(&self.root);
                Ok(json!({"message":"Instalação de ferramentas cancelada."}))
            }
            ("POST", "/api/setup/check") => {
                if self.setup.running() || self.imports_running() {
                    return Err("Aguarde a operação atual terminar.".into());
                }
                self.tools = setup::tool_checks(&self.root);
                let missing = self.tools.iter().filter(|c| c["ok"] != true).count();
                Ok(json!({"message": if missing == 0 {
                    "Ferramentas encontradas.".to_string()
                } else {
                    format!("{missing} item(ns) de ferramentas ausente(s) ou incompleto(s).")
                }}))
            }
            ("GET", "/api/setup/log") => Ok(json!({"text":self.log_tail("setup.log")?})),
            ("POST", "/api/build") => {
                if self.game.is_some() {
                    return Err("Feche o jogo antes de preparar uma instalação.".into());
                }
                if self.setup.running() || self.imports_running() {
                    return Err("Aguarde a operação atual terminar.".into());
                }
                self.start_build()?;
                Ok(json!({"message":"Preparação iniciada. Acompanhe o diagnóstico."}))
            }
            ("POST", "/api/build/cancel") => {
                console::ensure_hidden();
                self.build.cancel()?;
                Ok(json!({"message":"Preparação cancelada."}))
            }
            ("GET", "/api/log") | ("GET", "/api/game/log") => {
                let text = self.log_tail(if path == "/api/game/log" {
                    "game.log"
                } else {
                    "build.log"
                })?;
                Ok(json!({"text":text}))
            }
            ("POST", "/api/session/discover") => Ok(json!(discover(&self.identity())?)),
            ("POST", "/api/session/host") => {
                if self.gateway.is_some() || self.host.is_some() || self.game.is_some() {
                    return Err("Encerre a sessão atual antes de criar outra.".into());
                }
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let explicit = value["bind"].as_str();
                let default = format!("0.0.0.0:{DISCOVERY_PORT}");
                let bind = explicit
                    .unwrap_or(&default)
                    .parse::<SocketAddr>()
                    .map_err(|e| e.to_string())?;
                let host =
                    Host::start_named(bind, &self.identity(), &self.username, explicit.is_none())?;
                let local = SocketAddr::new(
                    if !host.address.ip().is_unspecified() {
                        host.address.ip()
                    } else if host.address.is_ipv4() {
                        "127.0.0.1".parse().unwrap()
                    } else {
                        "::1".parse().unwrap()
                    },
                    host.address.port(),
                );
                let gateway = Gateway::start(host.invite(local)?)?;
                self.gateway = Some(gateway);
                self.host = Some(host);
                Ok(
                    json!({"message":"Sala de transporte criada. Isso ainda não confirma uma corrida."}),
                )
            }
            ("POST", "/api/session/host-internet") => {
                if self.gateway.is_some()
                    || self.host.is_some()
                    || self.internet_room.is_some()
                    || self.steam_room.is_some()
                    || self.game.is_some()
                {
                    return Err("Encerre o jogo e a sessão atual antes de criar uma sala.".into());
                }
                if relay::url().is_some() {
                    let (room, invite) = InternetRoom::create(&self.identity())?;
                    let gateway = match Gateway::start(invite) {
                        Ok(gateway) => gateway,
                        Err(error) => {
                            let _ = room.close();
                            return Err(error);
                        }
                    };
                    let code = room.code.clone();
                    self.gateway = Some(gateway);
                    self.internet_room = Some(room);
                    return Ok(
                        json!({"code":code,"message":"Sala criada no relay. Aguarde a confirmação da conexão."}),
                    );
                }
                let default = format!("0.0.0.0:{DISCOVERY_PORT}");
                let bind = default.parse::<SocketAddr>().map_err(|e| e.to_string())?;
                let host = Host::start_named(bind, &self.identity(), &self.username, true)
                    .or_else(|_| {
                        Host::start_named(
                            "0.0.0.0:0".parse().unwrap(),
                            &self.identity(),
                            &self.username,
                            true,
                        )
                    })?;
                let local = SocketAddr::new("127.0.0.1".parse().unwrap(), host.address.port());
                let gateway = Gateway::start(host.invite(local)?)?;
                let code = host.room_ticket(None)?.encode();
                self.gateway = Some(gateway);
                self.host = Some(host);
                Ok(
                    json!({"code":code,"message":"Sala P2P criada! Código único copiado — envie aos seus amigos."}),
                )
            }
            ("POST", "/api/session/host-steam" | "/api/steam/host") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                self.steam_host(value["automatic"].as_bool().unwrap_or(true))
            }
            ("GET", "/api/steam/navigation") => Ok(json!({"automatic":self.steam_auto_join})),
            ("POST", "/api/steam/navigation") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let automatic = value["automatic"].as_bool().ok_or("Opção inválida.")?;
                save(&self.local.join("steam-navigation.json"), &automatic)?;
                self.steam_auto_join = automatic;
                Ok(
                    json!({"automatic":automatic,"message":"Preferência de convites salva neste computador."}),
                )
            }
            ("GET", "/api/steam/friends") => {
                let friends = steam::friends(&self.local)?;
                let _ = fs::write(self.local.join("steam-enabled"), b"1");
                let name = |id: u64| {
                    friends
                        .iter()
                        .find(|f| f.id == id.to_string())
                        .map(|f| f.name.clone())
                        .unwrap_or_else(|| id.to_string())
                };
                let invites: Vec<Value> = self
                    .steam_invites
                    .iter()
                    .map(
                        |(lobby, friend)| json!({"lobby":lobby.to_string(),"friend":name(*friend)}),
                    )
                    .collect();
                Ok(
                    json!({"persona":steam::probe_status(&self.local).persona,"friends":friends,"invites":invites,"room":self.steam_room.as_ref().map(|r|r.lobby_id.to_string()),"callbacks":steam::callbacks_delivered()}),
                )
            }
            ("POST", "/api/steam/invite") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let room = self
                    .steam_room
                    .as_ref()
                    .filter(|_| self.host.is_some())
                    .ok_or("Crie sua sala antes de convidar.")?;
                let friend = value["friend"]
                    .as_str()
                    .and_then(|v| v.parse::<u64>().ok())
                    .ok_or("Amigo inválido.")?;
                steam::invite(&self.local, room.lobby_id, friend)?;
                Ok(json!({"message":"Convite enviado pela Steam."}))
            }
            ("POST", "/api/steam/claim-join") => Ok(json!(self.steam_join_after_mii.take())),
            ("POST", "/api/steam/join") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let lobby = value["lobby"].as_str().ok_or("Sala inválida.")?.to_owned();
                if lobby.is_empty() || !lobby.chars().all(|c| c.is_ascii_digit()) {
                    return Err("Sala inválida.".into());
                }
                let two_player_guest = match value.get("players").map(Value::as_u64) {
                    None | Some(Some(1)) => false,
                    Some(Some(2)) => true,
                    _ => return Err("Escolha 1 ou 2 jogadores neste PC.".into()),
                };
                self.steam_join(&lobby, two_player_guest)?;
                Ok(
                    json!({"message": if self.pending_play.is_some_and(|play| play.automatic) {
                        "Conectado pela Steam. Escolha a sala do anfitrião na lista LAN."
                    } else {
                        "Conectado pela Steam. No jogo, escolha seu Mii e abra LAN Play → Lobby para entrar na sala do amigo."
                    }}),
                )
            }
            ("POST", "/api/steam/dismiss") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let lobby = value["lobby"].as_str().unwrap_or("");
                self.steam_invites.retain(|(l, _)| l.to_string() != lobby);
                Ok(json!({}))
            }
            ("POST", "/api/session/steam-overlay-invite") => {
                let room = self
                    .steam_room
                    .as_ref()
                    .ok_or("Crie ou entre em uma sala Steam primeiro.")?;
                room.open_invite_overlay()?;
                Ok(json!({"message":"Lista de convites da Steam solicitada (Shift+Tab)."}))
            }
            ("POST", "/api/session/invite") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let host = self.host.as_ref().ok_or("Crie uma sala primeiro")?;
                let endpoint = match value["endpoint"].as_str().filter(|s| !s.is_empty()) {
                    Some(value) => value.parse::<SocketAddr>().map_err(|e| e.to_string())?,
                    None => host.invite_endpoint().ok_or(
                        "Sem endereço automático. Confira a rede ou use Endereço alternativo.",
                    )?,
                };
                let invite = host.invite(endpoint)?;
                let code = host.room_ticket(Some(endpoint)).ok().map(|t| t.encode());
                Ok(
                    json!({"invite":invite.encode(),"code":code,"message":"Convite individual criado. Válido por quatro horas; não publique."}),
                )
            }
            ("POST", "/api/session/join") => {
                if self.gateway.is_some()
                    || self.host.is_some()
                    || self.steam_room.is_some()
                    || self.game.is_some()
                {
                    return Err("Encerre a sessão atual antes de entrar em outra.".into());
                }
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let text = value["invite"]
                    .as_str()
                    .ok_or("Convite ou código ausente")?
                    .trim();
                let identity = self.identity();
                if text.starts_with("MK8S-") || text.starts_with("steam://") {
                    let (room, gateway) = SteamRoom::join_live(
                        &self.local,
                        text,
                        &identity,
                        self.dlc_active()?,
                        &self.pack_active()?,
                    )?;
                    self.steam_room = Some(room);
                    self.gateway = Some(gateway);
                    return Ok(
                        json!({"message":"Conectado via Steam Relay! Use Jogar na sala para entrar no lobby."}),
                    );
                }
                if text.starts_with("MK8P-") {
                    let ticket = RoomTicket::decode(text)?;
                    let gateway = p2p::join_with_ticket(&ticket, &identity)?;
                    self.gateway = Some(gateway);
                    return Ok(
                        json!({"message":"Conectando pelo código único da sala. Aguarde confirmação do transporte."}),
                    );
                }
                let invite = if text.starts_with("MK8-") {
                    InternetRoom::join(text, &identity)?
                } else {
                    Invite::decode(text, &identity)?
                };
                self.gateway = Some(Gateway::start(invite)?);
                Ok(json!({"message":"Conectando ao anfitrião. Aguarde confirmação do transporte."}))
            }
            ("POST", "/api/session/stop") => {
                if self.game.is_some() {
                    return Err("Feche o jogo antes de encerrar seu transporte.".into());
                }
                self.gateway = None;
                self.host = None;
                self.steam_room = None;
                self.pending_play = None;
                if let Some(room) = self.internet_room.take() {
                    room.close().map_err(|_| {
                        "Sessão local encerrada. O relay não confirmou o fechamento da sala."
                    })?;
                }
                Ok(json!({"message":"Sessão encerrada."}))
            }
            ("POST", "/api/nextendo/trust") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                mk8_installer::trust_nextendo(&self.local, value["key"].as_str().unwrap_or(""))?;
                Ok(json!({}))
            }
            ("POST", "/api/play") => {
                self.status();
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let nextendo = value["nextendo"].as_bool().unwrap_or(false);
                let pid =
                    self.start_game(value["online"].as_bool().unwrap_or(false), None, nextendo)?;
                if nextendo {
                    return Ok(
                        json!({"message":"Abrindo o Nextendo. No jogo, selecione Online Play."}),
                    );
                }
                Ok(
                    json!({"message":format!("Processo iniciado ({pid}). Corrida ainda não verificada.")}),
                )
            }
            ("POST", "/api/report") => {
                let summary = self.status();
                let install = self.installation().or_else(|| self.candidate());
                let runtime = install.as_ref().and_then(|i| i.executable.path.parent());
                let path = report::create(&self.local, runtime, "manual", summary)?;
                self.last_report = Some(path.display().to_string());
                Ok(
                    json!({"path":path,"message":"Relatório de logs criado. Envie o arquivo .zip junto com a descrição do problema."}),
                )
            }
            ("POST", "/api/open") => {
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let folder = match value["what"].as_str() {
                    Some("reports") => report::folder(&self.local),
                    Some("logs") => self.local.join("logs"),
                    Some("data") => self.local.clone(),
                    _ => return Err("Pasta desconhecida.".into()),
                };
                if self.window.is_none() {
                    return Err("Disponível apenas na janela do Launcher.".into());
                }
                fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
                std::process::Command::new(if cfg!(windows) {
                    "explorer.exe"
                } else {
                    "xdg-open"
                })
                .arg(&folder)
                .spawn()
                .map_err(|e| e.to_string())?;
                Ok(json!({"message":"Pasta aberta."}))
            }
            ("POST", "/api/support/github" | "/api/nextendo/account") => {
                if self.window.is_none() {
                    return Err("Na prévia web, use o link diretamente.".into());
                }
                let value: Value = serde_json::from_str(body).map_err(|e| e.to_string())?;
                let nextendo = path == "/api/nextendo/account";
                let url = if nextendo {
                    tauri::Url::parse(if value["register"] == true {
                        "https://nextendo.network/register"
                    } else {
                        "https://nextendo.network/account"
                    })
                    .map_err(|e| e.to_string())?
                } else if value["list"] == true {
                    tauri::Url::parse("https://github.com/Vnnc1/MK8-online/issues")
                        .map_err(|e| e.to_string())?
                } else {
                    report::github_issue_url(
                        value["title"].as_str().unwrap_or(""),
                        value["body"].as_str().unwrap_or(""),
                    )?
                };
                #[cfg(target_os = "linux")]
                let mut command = {
                    let mut command = std::process::Command::new("xdg-open");
                    command.arg(url.as_str());
                    command
                };
                #[cfg(windows)]
                let mut command = {
                    let executable = std::env::var_os("SystemRoot")
                        .map(|root| PathBuf::from(root).join("System32/rundll32.exe"))
                        .filter(|path| path.is_file())
                        .ok_or("Não foi possível localizar o abridor de links do Windows.")?;
                    let mut command = std::process::Command::new(executable);
                    command.args(["url.dll,FileProtocolHandler", url.as_str()]);
                    #[cfg(windows)]
                    {
                        use std::os::windows::process::CommandExt;
                        command.creation_flags(0x08000000);
                    }
                    command
                };
                command.spawn().map_err(|e| e.to_string())?;
                if nextendo {
                    return Ok(json!({}));
                }
                Ok(json!({"message":"Abrindo o GitHub no navegador. Revise antes de publicar."}))
            }
            ("POST", "/api/update/check") => update::check(),
            ("POST", "/api/update/install") => {
                self.status();
                if self.session_busy()
                    || self.setup.running()
                    || self.imports_running()
                    || self.build.poll().state == "running"
                {
                    return Err("Feche o jogo e aguarde a preparação antes de atualizar.".into());
                }
                if self.host.is_some() || self.gateway.is_some() {
                    return Err("Encerre a sessão multijogador antes de atualizar.".into());
                }
                let _guards = mk8_build_manager::acquire_operation(&self.root, &self.local)?;
                let setup = update::download_and_start(&self.local)?;
                self.exit_requested = true;
                Ok(
                    json!({"path":setup,"message":"Instalando a atualização. O Launcher vai reabrir sozinho."}),
                )
            }
            _ => Err("Rota não encontrada".into()),
        }
    }
}

pub(crate) fn response(req: Request, code: u16, text: String, mime: &str) {
    binary_response(req, code, text.into_bytes(), mime);
}
fn binary_response(req: Request, code: u16, bytes: Vec<u8>, mime: &str) {
    let mut res = Response::from_data(bytes).with_status_code(code);
    for (key,value) in [("Content-Type",mime),("Cache-Control","no-store"),("X-Content-Type-Options","nosniff"),("Referrer-Policy","no-referrer"),("Content-Security-Policy","default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; style-src 'self'; connect-src 'self' blob:; img-src 'self' data: blob:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'")] {res.add_header(Header::from_bytes(key,value).unwrap());}
    // The loop serves one request at a time. A body larger than the socket buffers waits for the page to read
    // it; the Mii creator reads the AFL only after FFL's wasm arrives from this same loop, so writing it here
    // froze every launcher request (issues #15/#16). Large bodies are written on their own thread.
    if res.data_length().unwrap_or(0) > 32 * 1024 {
        std::thread::spawn(move || {
            let _ = req.respond(res);
        });
    } else {
        let _ = req.respond(res);
    }
}
/// Opens a native folder/file picker on its own thread so the API keeps
/// answering status requests while the dialog is open.
fn browse(req: Request, body: &str, parent: &DialogParent) {
    let value: Value = serde_json::from_str(body).unwrap_or_default();
    let language = value["language"].as_str().unwrap_or("en").to_string();
    let current = value["current"].as_str().unwrap_or("").to_string();
    let filter = match value["kind"].as_str() {
        Some("dlc") => Some((
            crate::language::pick(
                &language,
                "NSP/NSZ do DLC",
                "DLC NSP/NSZ",
                "NSP/NSZ del DLC",
            ),
            &["nsp", "nsz"][..],
        )),
        Some("nacp") => Some((
            crate::language::pick(
                &language,
                "Metadados NACP",
                "NACP metadata",
                "Metadatos NACP",
            ),
            &["nacp"][..],
        )),
        Some("package") => Some((
            crate::language::pick(&language, "Pacotes MK8", "MK8 packages", "Paquetes de MK8"),
            &["nsz", "nsp"][..],
        )),
        Some("keys") => Some((
            crate::language::pick(
                &language,
                "Chaves do console",
                "Console keys",
                "Claves de la consola",
            ),
            &["keys"][..],
        )),
        Some("afl") => Some((
            crate::language::pick(
                &language,
                "Biblioteca de peças Mii",
                "Mii parts library",
                "Biblioteca de partes Mii",
            ),
            &["dat"][..],
        )),
        Some("fsr-library") => Some((
            crate::language::pick(&language, "Provedor FFX", "FFX provider", "Proveedor FFX"),
            if cfg!(windows) {
                &["dll"][..]
            } else {
                &["so"][..]
            },
        )),
        Some("dxc") => Some((
            crate::language::pick(
                &language,
                "Compilador DXC",
                "DXC compiler",
                "Compilador DXC",
            ),
            &["*"][..],
        )),
        _ => None,
    };
    let title = value["title"]
        .as_str()
        .unwrap_or(crate::language::pick(
            &language,
            "Selecionar",
            "Select",
            "Seleccionar",
        ))
        .chars()
        .take(120)
        .collect::<String>();
    let parent = parent.clone();
    std::thread::spawn(move || {
        let mut dialog = rfd::FileDialog::new().set_title(title);
        if let Some(window) = parent.get() {
            dialog = dialog.set_parent(window);
        }
        let start = Path::new(&current);
        let start = if start.is_dir() {
            Some(start)
        } else {
            start.parent().filter(|p| p.is_dir())
        };
        if let Some(dir) = start {
            dialog = dialog.set_directory(dir);
        }
        let picked = if let Some((name, extensions)) = filter {
            dialog
                .add_filter(name, extensions)
                .add_filter(
                    crate::language::pick(
                        &language,
                        "Todos os arquivos",
                        "All files",
                        "Todos los archivos",
                    ),
                    &["*"],
                )
                .pick_file()
        } else {
            dialog.pick_folder()
        };
        let data = json!({"ok":true,"data":{"path":picked.map(|p| p.display().to_string())}});
        response(
            req,
            200,
            data.to_string(),
            "application/json; charset=utf-8",
        );
    });
}

pub fn serve(mut app: App, server: Server, authority: String, url: String, token: String) {
    #[cfg(debug_assertions)]
    let web_root = std::env::var_os("MK8_WEB_ROOT").map(PathBuf::from);
    for mut req in server.incoming_requests() {
        let host = req
            .headers()
            .iter()
            .find(|h| h.field.equiv("Host"))
            .map(|h| h.value.as_str());
        if host != Some(authority.as_str()) {
            response(req, 403, "Host inválido".into(), "text/plain");
            continue;
        }
        let path = req.url().to_string();
        let method = req.method().clone();
        #[cfg(debug_assertions)]
        if let (Method::Get, Some(root)) = (&method, &web_root) {
            if let Some(result) =
                crate::debug_web::asset(root, &path, &token, &crate::language::read(&app.local))
            {
                match result {
                    Ok((source, mime)) => response(req, 200, source, mime),
                    Err(error) => response(req, 500, format!("Debug asset: {error}"), "text/plain"),
                }
                continue;
            }
        }
        if method == Method::Get && path == "/" {
            response(
                req,
                200,
                include_str!("../web/index.html")
                    .replace("__TOKEN__", &token)
                    .replace("__LANGUAGE__", &crate::language::read(&app.local)),
                "text/html; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/i18n.js" {
            response(
                req,
                200,
                include_str!("../web/i18n.js").into(),
                "text/javascript; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/en.json" {
            response(
                req,
                200,
                include_str!("../web/en.json").into(),
                "application/json; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/es.json" {
            response(
                req,
                200,
                include_str!("../web/es.json").into(),
                "application/json; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/workspace.js" {
            response(
                req,
                200,
                include_str!("../web/workspace.js").into(),
                "text/javascript; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/installation.js" {
            response(
                req,
                200,
                include_str!("../web/installation.js").into(),
                "text/javascript; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/workspace.css" {
            response(
                req,
                200,
                include_str!("../web/workspace.css").into(),
                "text/css; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/installation.css" {
            response(
                req,
                200,
                include_str!("../web/installation.css").into(),
                "text/css; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/fsr.css" {
            response(
                req,
                200,
                include_str!("../web/fsr.css").into(),
                "text/css; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/fsr.js" {
            response(
                req,
                200,
                include_str!("../web/fsr.js").into(),
                "text/javascript; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get && path == "/app.js" {
            response(
                req,
                200,
                include_str!("../web/app.js").into(),
                "text/javascript; charset=utf-8",
            );
            continue;
        }
        if method == Method::Get {
            let asset: Option<(&[u8], &str)> = match path.as_str() {
                "/menu-themes/default.webp" => Some((
                    include_bytes!("../web/menu-themes/default.webp"),
                    "image/webp",
                )),
                "/menu-themes/nes.webp" => {
                    Some((include_bytes!("../web/menu-themes/nes.webp"), "image/webp"))
                }
                "/menu-themes/simple-light.webp" => Some((
                    include_bytes!("../web/menu-themes/simple-light.webp"),
                    "image/webp",
                )),
                "/menu-themes/nes-light.webp" => Some((
                    include_bytes!("../web/menu-themes/nes-light.webp"),
                    "image/webp",
                )),
                "/menu-themes/native.webp" => Some((
                    include_bytes!("../web/menu-themes/native.webp"),
                    "image/webp",
                )),
                "/vendor/ffl/ffl-emscripten.wasm" => Some((
                    include_bytes!("../web/vendor/ffl/ffl-emscripten.wasm"),
                    "application/wasm",
                )),
                _ => None,
            };
            if let Some((bytes, mime)) = asset {
                binary_response(req, 200, bytes.to_vec(), mime);
                continue;
            }
            let model_asset = match path.as_str() {
                "/mii-composer.js" => Some((
                    include_str!("../web/mii-composer.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/GLTFExporter.js" => Some((
                    include_str!("../web/vendor/three/GLTFExporter.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/ffl/ffl.js" => Some((
                    include_str!("../web/vendor/ffl/ffl.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/ffl/ffl-emscripten.cjs" => Some((
                    include_str!("../web/vendor/ffl/ffl-emscripten.cjs"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/ffl/helpers/GeometryConverter.js" => Some((
                    include_str!("../web/vendor/ffl/helpers/GeometryConverter.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/ffl/materials/FFLShaderMaterial.js" => Some((
                    include_str!("../web/vendor/ffl/materials/FFLShaderMaterial.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/ffl/LICENSE" => Some((
                    include_str!("../web/vendor/ffl/LICENSE"),
                    "text/plain; charset=utf-8",
                )),
                "/vendor/ffl/README.md" => Some((
                    include_str!("../web/vendor/ffl/README.md"),
                    "text/plain; charset=utf-8",
                )),
                "/vendor/ffl/manifest.json" => Some((
                    include_str!("../web/vendor/ffl/manifest.json"),
                    "text/plain; charset=utf-8",
                )),
                "/mii-model.html" => Some((
                    include_str!("../web/mii-model.html"),
                    "text/html; charset=utf-8",
                )),
                "/mii-model.css" => Some((
                    include_str!("../web/mii-model.css"),
                    "text/css; charset=utf-8",
                )),
                "/mii-model.js" => Some((
                    include_str!("../web/mii-model.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/mii-glb.js" => Some((
                    include_str!("../web/mii-glb.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/three.module.js" => Some((
                    include_str!("../web/vendor/three/three.module.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/three.core.js" => Some((
                    include_str!("../web/vendor/three/three.core.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/GLTFLoader.js" => Some((
                    include_str!("../web/vendor/three/GLTFLoader.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/BufferGeometryUtils.js" => Some((
                    include_str!("../web/vendor/three/BufferGeometryUtils.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/OrbitControls.js" => Some((
                    include_str!("../web/vendor/three/OrbitControls.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/SkeletonUtils.js" => Some((
                    include_str!("../web/vendor/three/SkeletonUtils.js"),
                    "text/javascript; charset=utf-8",
                )),
                "/vendor/three/LICENSE.txt" => Some((
                    include_str!("../web/vendor/three/LICENSE.txt"),
                    "text/plain; charset=utf-8",
                )),
                _ => None,
            };
            if let Some((source, mime)) = model_asset {
                response(
                    req,
                    200,
                    source
                        .replace("__LANGUAGE__", &crate::language::read(&app.local))
                        .replace("from 'three'", "from '/vendor/three/three.module.js'"),
                    mime,
                );
                continue;
            }
        }
        if method == Method::Get && path == "/style.css" {
            response(
                req,
                200,
                include_str!("../web/style.css").into(),
                "text/css; charset=utf-8",
            );
            continue;
        }
        let authenticated = req
            .headers()
            .iter()
            .any(|h| h.field.equiv("X-MK8-Token") && h.value.as_str() == token);
        let origin = req
            .headers()
            .iter()
            .find(|h| h.field.equiv("Origin"))
            .map(|h| h.value.as_str());
        if !authenticated || origin.is_some_and(|value| value != url) {
            response(
                req,
                403,
                "{\"ok\":false,\"error\":\"Sessão local inválida\"}".into(),
                "application/json",
            );
            continue;
        }
        if let Some(language) = req
            .headers()
            .iter()
            .find(|h| h.field.equiv("X-MK8-Language"))
        {
            let language = language.value.as_str();
            if crate::language::valid(language) {
                language.clone_into(&mut app.browser_language);
            }
        }
        if method == Method::Get && path == "/api/report/download" {
            match report::download(&app.local, app.last_report.as_deref()) {
                Ok(bytes) => binary_response(req, 200, bytes, "application/zip"),
                Err(error) => response(
                    req,
                    400,
                    json!({"ok":false,"error":error}).to_string(),
                    "application/json",
                ),
            }
            continue;
        }
        if method == Method::Get
            && matches!(
                path.as_str(),
                "/api/mii/composer/resource" | "/api/mii/composer/model" | "/api/mii/body"
            )
        {
            let result = if path == "/api/mii/body" {
                // The game's own body, read from the prepared game (never copied out of it).
                app.installation()
                    .or_else(|| app.candidate())
                    .ok_or_else(|| "Prepare o jogo para ver o corpo do Mii.".to_string())
                    .and_then(|install| {
                        mk8_installer::mii_body::glb(&install.data_root.join("extracted"))
                    })
            } else if path.ends_with("resource") {
                mk8_installer::mii_composer::resource(&app.local)
            } else {
                mk8_installer::mii_composer::saved_bytes(&app.local)
            };
            match result {
                Ok(bytes) => binary_response(req, 200, bytes, "application/octet-stream"),
                Err(error) => response(
                    req,
                    400,
                    json!({"ok":false,"error":error}).to_string(),
                    "application/json",
                ),
            }
            continue;
        }
        let limit = if path == "/api/mii/composer/save" {
            9 * 1024 * 1024
        } else {
            32768
        };
        if req.body_length().is_some_and(|n| n > limit) {
            response(req, 413, "{}".into(), "application/json");
            continue;
        }
        let mut body = String::new();
        if req
            .as_reader()
            .take((limit + 1) as u64)
            .read_to_string(&mut body)
            .is_err()
            || body.len() > limit
        {
            response(req, 400, "{}".into(), "application/json");
            continue;
        }
        if method == Method::Post && path == "/api/browse" {
            match app.window.as_ref() {
                Some(parent) => browse(req, &body, parent),
                None => response(
                    req,
                    400,
                    json!({"ok":false,"error":"Seletor de pastas disponível apenas na janela do Launcher."}).to_string(),
                    "application/json; charset=utf-8",
                ),
            }
            continue;
        }
        let (code, data) = match app.handle(&method, &path, &body) {
            Ok(value) => (200, json!({"ok":true,"data":value})),
            Err(error) => (400, json!({"ok":false,"error":error})),
        };
        response(
            req,
            code,
            data.to_string(),
            "application/json; charset=utf-8",
        );
        if app.exit_requested {
            // (the installer waits for this process; give the answer time to reach the page)
            std::thread::sleep(std::time::Duration::from_millis(500));
            std::process::exit(0);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(windows)]
    #[test]
    fn failed_game_keeps_the_reason_in_status_and_a_report() {
        let root = std::env::temp_dir().join(format!("mk8-failed-game-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        let fatal = "SDL_Vulkan_CreateSurface: VK_KHR_win32_surface extension is not enabled";
        fs::write(
            root.join("game.log"),
            format!("{}\nFATAL: {fatal}\n", "x".repeat(40_000)),
        )
        .unwrap();
        let exit = std::process::Command::new("cmd.exe")
            .args(["/d", "/c", "exit 1"])
            .status()
            .unwrap();
        assert!(app.finish_game(exit).contains(fatal));
        assert!(app.game_exit.as_deref().unwrap().contains(fatal));
        let extracted = std::process::Command::new("tar.exe")
            .arg("-xOf")
            .arg(app.last_report.as_ref().unwrap())
            .arg("report.json")
            .output()
            .unwrap();
        assert!(extracted.status.success());
        let report: Value = serde_json::from_slice(&extracted.stdout).unwrap();
        assert_eq!(report["launcher"]["game_exit_detail"], fatal);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn dlc_file_readiness_uses_real_files_and_the_importers_key_selection() {
        let root = std::env::temp_dir().join(format!("mk8-dlc-fields-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let package = root.join("dlc.nsp");
        let keys = root.join("prod.keys");
        fs::write(&package, b"pacote autoral").unwrap();
        fs::write(&keys, b"chaves autorais").unwrap();
        let config = Config {
            prod_keys: keys.to_string_lossy().into_owned(),
            ..Config::default()
        };
        let state = dlc_files(&config, package.to_str().unwrap(), "");
        assert_eq!(state["package_ok"], true);
        assert_eq!(state["keys_ok"], true);
        assert_eq!(state["keys_detected"], true);
        assert_eq!(state["keys"], config.prod_keys);
        let detected = dlc_files(&Config::default(), package.to_str().unwrap(), "");
        assert_eq!(detected["keys_ok"], true);
        assert_eq!(detected["keys"], config.prod_keys);
        let missing = root.join("missing.keys");
        assert_eq!(
            dlc_files(
                &config,
                package.to_str().unwrap(),
                missing.to_str().unwrap()
            )["keys_ok"],
            false
        );
        fs::remove_file(&package).unwrap();
        fs::remove_file(&keys).unwrap();
        let state = dlc_files(&config, package.to_str().unwrap(), "");
        assert_eq!(state["package_ok"], false);
        assert_eq!(state["keys_ok"], false);
        fs::remove_dir_all(root).unwrap();
    }
    #[cfg(windows)]
    #[test]
    fn nextendo_exit_reports_only_the_current_login_refusal() {
        let root = std::env::temp_dir().join(format!("mk8-nextendo-exit-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        let path = root.join("game.log");
        let denied = "[nextendo] online status: refused: discord_unlinked\r\n";
        for (nextendo, exit, log, expected) in [
            (true, "2", denied, Some("discord_unlinked")),
            (true, "2", "[nextendo] online status: refused: maintenance\n", Some("maintenance")),
            (true, "2", "[nextendo] online status: refused: discord_unlinked\n[nextendo] online status: ok\n", None),
            (true, "2", "BAAS adapter unavailable or online access refused.\n", None),
            (true, "2", "[nextendo] online status: refused: discord_unlinked\nFATAL: runtime failed\n", None),
            (true, "2", "[nextendo] online status: refused: invalid text", None),
            (true, "1", denied, None),
            (false, "2", denied, None),
        ] {
            fs::write(&path, log).unwrap();
            app.nextendo_log = nextendo.then(|| fs::File::open(&path).unwrap());
            if nextendo {
                // Another launcher can rotate the file after this game releases its lock.
                mk8_installer::rotate_game_log(&root).unwrap();
                fs::write(&path, "[nextendo] online status: refused: unrelated\n").unwrap();
            }
            let mut child = std::process::Command::new("cmd")
                .args(["/d", "/c", "exit", exit]).spawn().unwrap();
            child.wait().unwrap();
            app.game = Some(child);
            app.poll_game();
            assert_eq!(app.nextendo_refusal.as_deref(), expected);
            assert!(app.nextendo_log.is_none());
        }
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn dlc_changes_refuse_resources_owned_by_another_session() {
        let root = std::env::temp_dir().join(format!("mk8-dlc-lock-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        let guards = mk8_build_manager::acquire_game(&root, &root).unwrap();
        let error = app
            .handle(&Method::Post, "/api/dlc/toggle", r#"{"enabled":true}"#)
            .unwrap_err();
        assert!(error.contains("Outra instância"), "{error}");
        let error = app
            .handle(&Method::Post, "/api/fonts", r#"{"enabled":false}"#)
            .unwrap_err();
        assert!(error.contains("Outra instância"), "{error}");
        assert!(!root.join("font-library.json").exists());
        let error = app
            .handle(&Method::Post, "/api/settings", r#"{"volume":"0.5"}"#)
            .unwrap_err();
        assert!(error.contains("Outra instância"), "{error}");
        assert!(!root.join("user-no-mii/mk8d.ini").exists());
        let error = app
            .handle(
                &Method::Post,
                "/api/dlc/import",
                r#"{"package":"fixture.nsp"}"#,
            )
            .unwrap_err();
        assert!(error.contains("Outra instância"), "{error}");
        assert!(!root.join("dlc.json").exists());
        drop(guards);
        let pipeline = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(root.join(".build.lock"))
            .unwrap();
        pipeline.try_lock().unwrap();
        let error = app
            .handle(&Method::Post, "/api/dlc/toggle", r#"{"enabled":true}"#)
            .unwrap_err();
        assert!(error.contains("Já existe uma preparação"), "{error}");
        drop(pipeline);
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn fsr_settings_route_persists_and_rejects_the_unavailable_mlfi_mode() {
        let root = std::env::temp_dir().join(format!("mk8-fsr-route-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        let provider = if cfg!(windows) {
            r"C:\\fsr\\provider.dll"
        } else {
            "/opt/fsr.so"
        };
        let body = format!(
            r#"{{"upscaling":"3.1.6","provider_lib":"{provider}","frame_generation":"fsr3","dxc":"/opt/dxc","legacy_abi":true}}"#
        );
        let value = app
            .handle(&Method::Post, "/api/fsr/settings", &body)
            .unwrap();
        assert_eq!(value["settings"]["upscaling"], "3.1.6");
        assert_eq!(value["settings"]["frame_generation"], "fsr3");
        let settings = crate::fsr::read_settings(&root).unwrap();
        assert_eq!(settings.upscaling, "3.1.6");
        assert!(settings.legacy_abi);

        let error = app
            .handle(
                &Method::Post,
                "/api/fsr/settings",
                r#"{"upscaling":"off","provider_lib":"","frame_generation":"fsr4","dxc":"","legacy_abi":false}"#,
            )
            .unwrap_err();
        assert!(error.contains("FSR 4 MLFI"), "{error}");
        assert_eq!(crate::fsr::read_settings(&root).unwrap(), settings);
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    struct LocalSteamFixture;
    impl steam::P2PBackend for LocalSteamFixture {
        fn local_user(&self) -> u64 {
            42
        }
        fn tick(&self) {}
        fn send(&self, _: u64, _: i32, _: &[u8], _: bool) -> bool {
            false
        }
        fn recv(&self, _: i32, _: &mut [u8]) -> Option<(usize, u64)> {
            None
        }
    }

    fn test_shell(windows: &str, unix: &str) -> std::process::Command {
        let mut command = if cfg!(windows) {
            std::process::Command::new("cmd.exe")
        } else {
            std::process::Command::new("sh")
        };
        command.args(if cfg!(windows) {
            ["/C", windows]
        } else {
            ["-c", unix]
        });
        command
    }

    #[test]
    fn closed_game_and_idle_guest_release_steam_workers_before_next_join() {
        let root = std::env::temp_dir().join(format!("mk8-rejoin-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        for host_role in [false, true] {
            let (room, host, invite) = SteamRoom::host_with_backend(
                Arc::new(LocalSteamFixture),
                17,
                42,
                "fixture",
                "Test",
            )
            .unwrap();
            let gateway = Gateway::start(invite).unwrap();
            let address = gateway.local_addr;
            app.gateway = Some(gateway);
            app.steam_room = Some(room);
            if host_role {
                app.host = Some(host);
            } else {
                drop(host);
            }
            let mut child = test_shell("exit 0", "exit 0").spawn().unwrap();
            child.wait().unwrap();
            app.game = Some(child);
            app.prepare_steam_join().unwrap();
            assert!(!app.session_busy());
            assert!(app.pending_play.is_none());
            // Joining Drop waits for both socket workers. On Windows, release of
            // the port can complete just after the worker handles are closed.
            let deadline = Instant::now() + Duration::from_secs(2);
            loop {
                match std::net::UdpSocket::bind(address) {
                    Ok(_) => break,
                    Err(error) if Instant::now() >= deadline => {
                        panic!("gateway port stayed open: {error}")
                    }
                    Err(_) => std::thread::sleep(Duration::from_millis(10)),
                }
            }
        }
        // Idle guest without a tracked game (e.g. failed startup) is also replaceable.
        let (room, host, invite) =
            SteamRoom::host_with_backend(Arc::new(LocalSteamFixture), 18, 42, "fixture", "Test")
                .unwrap();
        app.gateway = Some(Gateway::start(invite).unwrap());
        app.steam_room = Some(room);
        app.prepare_steam_join().unwrap();
        assert!(!app.session_busy());
        drop(host);
        // A running game must never be stopped or detached by a new invitation.
        app.game = Some(
            test_shell("set /p value=", "read value")
                .stdin(std::process::Stdio::piped())
                .stdout(std::process::Stdio::null())
                .spawn()
                .unwrap(),
        );
        let rejected = app.prepare_steam_join().is_err();
        let still_running = app.game.as_mut().unwrap().try_wait().unwrap().is_none();
        app.game.as_mut().unwrap().kill().unwrap();
        app.game.as_mut().unwrap().wait().unwrap();
        app.game = None;
        assert!(rejected && still_running);
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn manual_launch_keeps_a_pending_role_without_auto_lan() {
        for role in ["host", "guest"] {
            assert_eq!(
                PendingPlay {
                    role,
                    automatic: true,
                    two_player_guest: false,
                }
                .auto_lan(),
                Some(role)
            );
            let manual = PendingPlay {
                role,
                automatic: false,
                two_player_guest: false,
            };
            assert_eq!(manual.role, role);
            assert_eq!(manual.auto_lan(), None);
        }
    }

    #[test]
    fn invite_navigation_is_persisted_and_invalid_choice_is_rejected() {
        let root = std::env::temp_dir().join(format!("mk8-navigation-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        assert!(app.steam_auto_join);
        for players in ["0", "3", "null", "true", "\"2\""] {
            let body = format!(r#"{{"lobby":"123","players":{players}}}"#);
            assert_eq!(
                app.handle(&Method::Post, "/api/steam/join", &body)
                    .unwrap_err(),
                "Escolha 1 ou 2 jogadores neste PC."
            );
        }
        app.handle(
            &Method::Post,
            "/api/steam/navigation",
            r#"{"automatic":false}"#,
        )
        .unwrap();
        assert!(!app.steam_auto_join);
        assert!(app
            .handle(
                &Method::Post,
                "/api/steam/navigation",
                r#"{"automatic":"false"}"#
            )
            .is_err());
        drop(app);
        let app = App::new(root.clone(), root.clone(), Config::default(), None);
        assert!(!app.steam_auto_join);
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn failed_or_cancelled_build_clears_optional_dlc_without_changing_preference() {
        let root = std::env::temp_dir().join(format!("mk8-prepare-dlc-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("dlc.json"), b"previous preference").unwrap();
        let config = Config {
            prod_keys: root
                .join("missing-keys/prod.keys")
                .to_string_lossy()
                .into_owned(),
            ..Config::default()
        };
        assert!(!Path::new(&config.prod_keys).exists());
        let mut app = App::new(root.clone(), root.clone(), config, None);
        // A previous candidate must not start the new import while tools install.
        app.prepare_after_setup = true;
        app.prepare_dlc = Some(root.join("optional.nsp"));
        app.finish_optional_dlc("candidate");
        assert!(app.prepare_dlc.is_some());
        app.prepare_after_setup = false;
        // Missing keys fail visibly after build completion and consume the request once.
        app.finish_optional_dlc("candidate");
        assert!(app.prepare_dlc.is_none());
        assert_eq!(app.build.poll().state, "failed");
        assert!(!app.imports_running());
        for state in ["failed", "cancelled"] {
            app.prepare_dlc = Some(root.join("optional.nsp"));
            app.finish_optional_dlc("running");
            assert!(app.prepare_dlc.is_some());
            app.finish_optional_dlc(state);
            assert!(app.prepare_dlc.is_none());
            assert!(!app.imports_running());
        }
        assert_eq!(
            fs::read(root.join("dlc.json")).unwrap(),
            b"previous preference"
        );
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn matching_prebuilt_host_does_not_vouch_for_an_old_guest() {
        let root = std::env::temp_dir().join(format!("mk8-guest-upgrade-{}", std::process::id()));
        fs::create_dir_all(root.join("prebuilt/host")).unwrap();
        let hash = "a".repeat(64);
        save(
            &root.join("prebuilt/host/manifest.json"),
            &json!({"files":{"mk8drecomp.exe":hash}}),
        )
        .unwrap();
        save(
            &root.join("built-with.json"),
            &json!({"version":update::CURRENT}),
        )
        .unwrap();
        let mut candidate = json!({"schema":2, "identity":"fixture", "profile":"mk8d-smm2-experimental",
            "executable":{"path":"fixture.exe","sha256":hash,"bytes":1}, "files":{},
            "data_root":".","shader_root":".","gameplay_verified":false,"nextendo_supported":true,
            "menu_themes_supported":true,"menu_light_themes_supported":true,"bloom_supported":true,
            "controller_vibration_supported":true,
            "direct_lan_guest_supported":true});
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        save(&root.join("candidate.json"), &candidate).unwrap();
        // Every other capability is current: only the missing guest cadence hook makes it stale.
        assert_eq!(app.status()["candidate_stale"], true);
        candidate["split_screen_60fps_supported"] = json!(true);
        save(&root.join("candidate.json"), &candidate).unwrap();
        assert_eq!(app.status()["candidate_stale"], false);
        candidate
            .as_object_mut()
            .unwrap()
            .remove("controller_vibration_supported");
        save(&root.join("candidate.json"), &candidate).unwrap();
        assert!(app.status()["candidate_stale"].as_bool().unwrap());
        assert!(!app.controller_vibration_supported());
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn candidate_network_identity_uses_content_and_launch_still_checks_hashes() {
        let root =
            std::env::temp_dir().join(format!("mk8-network-manifest-{}", std::process::id()));
        fs::create_dir_all(root.join("extracted/romfs")).unwrap();
        let path = root.join("fixture");
        fs::write(&path, b"authored fixture").unwrap();
        let file = mk8_manifest::hash_file(&path).unwrap();
        let mut install = Installation {
            schema: 2,
            identity: "machine-specific-build".into(),
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
            controller_vibration_supported: false,
            direct_lan_guest_supported: false,
        };
        save(&root.join("candidate.json"), &install).unwrap();
        let mut app = App::new(root.clone(), root.clone(), Config::default(), None);
        let identity = app.identity();
        assert!(identity.starts_with("mk8-lan-v1:"));
        assert!(app.installation().is_none());
        assert_eq!(app.status()["session"]["diagnostic_only"], false);
        install.identity = "other-machine".into();
        install.files.get_mut("main").unwrap().path = root.join("different-path");
        save(&root.join("candidate.json"), &install).unwrap();
        assert_eq!(app.identity(), identity);
        install.files.get_mut("main").unwrap().sha256 = "changed".into();
        save(&root.join("candidate.json"), &install).unwrap();
        assert_ne!(app.identity(), identity);
        assert!(mk8_installer::launch_candidate(
            &install,
            &root,
            Some(("127.0.0.1:47820".into(), 0)),
            "Test"
        )
        .is_err());
        drop(app);
        fs::remove_dir_all(root).unwrap();
    }
}
