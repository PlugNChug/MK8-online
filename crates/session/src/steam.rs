//! Dynamic Steamworks Lobby + P2P Relay adapter (`steam_api64.dll`).
//! Uses a local UDP socket bridged to `ISteamNetworking` P2P channels and `ISteamMatchmaking`
//! lobbies so the existing authenticated `Host` and `Gateway` work over Steam Datagram Relay
//! without VPN or firewall port forwarding. No proprietary SDK binary is bundled in the repository.

use crate::{Gateway, Host, Invite};
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use mk8_manifest::Result;
use mk8_transport::{random, MAX_PACKET};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    ffi::{c_char, c_int, c_void, CStr, CString},
    fs,
    net::{SocketAddr, UdpSocket},
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant},
};

const LOBBY_CREATED_CALLBACK: c_int = 513;
const LOBBY_ENTER_CALLBACK: c_int = 504;
const ELOBBY_TYPE_FRIENDS_ONLY: c_int = 1;
const EP2P_SEND_UNRELIABLE_NO_DELAY: c_int = 1;
const EP2P_SEND_RELIABLE: c_int = 2;
const P2P_CHANNEL_CONTROL: c_int = 0;
const P2P_CHANNEL_DATA: c_int = 1;
const DEFAULT_APP_ID: &str = "480";

#[repr(C, packed(8))]
#[derive(Clone, Copy, Default)]
struct LobbyCreated {
    result: u32,
    lobby_id: u64,
}

#[repr(C, packed(8))]
#[derive(Clone, Copy, Default)]
struct LobbyEnter {
    lobby_id: u64,
    chat_permissions: u32,
    locked: bool,
    response: u32,
}

struct Api {
    _handle: isize,
    is_steam_running: unsafe extern "C" fn() -> bool,
    init: unsafe extern "C" fn() -> bool,
    run_callbacks: unsafe extern "C" fn(),
    steam_user: unsafe extern "C" fn() -> *mut c_void,
    steam_friends: unsafe extern "C" fn() -> *mut c_void,
    steam_matchmaking: unsafe extern "C" fn() -> *mut c_void,
    steam_networking: unsafe extern "C" fn() -> *mut c_void,
    steam_utils: unsafe extern "C" fn() -> *mut c_void,
    get_steam_id: unsafe extern "C" fn(*mut c_void) -> u64,
    get_persona_name: unsafe extern "C" fn(*mut c_void) -> *const c_char,
    activate_invite_dialog: unsafe extern "C" fn(*mut c_void, u64),
    set_rich_presence: unsafe extern "C" fn(*mut c_void, *const c_char, *const c_char) -> bool,
    clear_rich_presence: unsafe extern "C" fn(*mut c_void),
    create_lobby: unsafe extern "C" fn(*mut c_void, c_int, c_int) -> u64,
    join_lobby: unsafe extern "C" fn(*mut c_void, u64) -> u64,
    leave_lobby: unsafe extern "C" fn(*mut c_void, u64),
    set_lobby_data: unsafe extern "C" fn(*mut c_void, u64, *const c_char, *const c_char) -> bool,
    get_lobby_data: unsafe extern "C" fn(*mut c_void, u64, *const c_char) -> *const c_char,
    accept_p2p: unsafe extern "C" fn(*mut c_void, u64) -> bool,
    allow_p2p_relay: unsafe extern "C" fn(*mut c_void, bool) -> bool,
    send_p2p: unsafe extern "C" fn(*mut c_void, u64, *const c_void, u32, c_int, c_int) -> bool,
    is_p2p_available: unsafe extern "C" fn(*mut c_void, *mut u32, c_int) -> bool,
    read_p2p:
        unsafe extern "C" fn(*mut c_void, *mut c_void, u32, *mut u32, *mut u64, c_int) -> bool,
    is_call_completed: unsafe extern "C" fn(*mut c_void, u64, *mut bool) -> bool,
    get_call_result:
        unsafe extern "C" fn(*mut c_void, u64, *mut c_void, c_int, c_int, *mut bool) -> bool,
}

#[cfg(windows)]
fn load_symbol<T>(handle: isize, name: &CStr) -> Result<T> {
    use windows_sys::Win32::System::LibraryLoader::GetProcAddress;
    let sym = unsafe { GetProcAddress(handle as _, name.as_ptr().cast()) }.ok_or_else(|| {
        format!(
            "steam_api64.dll sem função exigida: {}",
            name.to_string_lossy()
        )
    })?;
    Ok(unsafe { std::mem::transmute_copy(&sym) })
}

#[cfg(not(windows))]
fn load_symbol<T>(_handle: isize, name: &CStr) -> Result<T> {
    Err(format!(
        "Steam DLL suportada apenas no Windows ({})",
        name.to_string_lossy()
    ))
}

pub fn candidate_dll_paths(local: &Path) -> Vec<PathBuf> {
    let mut paths = Vec::new();
    if let Ok(env_path) = std::env::var("MK8_STEAM_DLL") {
        let p = PathBuf::from(env_path.trim());
        if !p.as_os_str().is_empty() {
            paths.push(p);
        }
    }
    paths.push(local.join("steam_api64.dll"));
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            paths.push(parent.join("steam_api64.dll"));
        }
    }
    // Check common Steam install folder for any user-installed game's redistributable steam_api64.dll
    for base in [
        r"C:\Program Files (x86)\Steam\steamapps\common",
        r"C:\Program Files\Steam\steamapps\common",
    ] {
        let dir = Path::new(base);
        if let Ok(entries) = fs::read_dir(dir) {
            for entry in entries.flatten().take(40) {
                let candidate = entry.path().join("steam_api64.dll");
                if candidate.is_file() {
                    paths.push(candidate);
                    break;
                }
            }
        }
    }
    paths
}

pub fn find_dll(local: &Path) -> Option<PathBuf> {
    candidate_dll_paths(local)
        .into_iter()
        .find(|path| path.is_file())
}

#[derive(Clone, Serialize, Deserialize)]
pub struct SteamCode {
    pub v: u8,
    pub lobby: u64,
    pub host: u64,
    pub token: [u8; 16],
}

impl SteamCode {
    pub fn encode(&self) -> String {
        let mut raw = Vec::with_capacity(33);
        raw.push(self.v);
        raw.extend_from_slice(&self.lobby.to_le_bytes());
        raw.extend_from_slice(&self.host.to_le_bytes());
        raw.extend_from_slice(&self.token);
        format!("MK8S-{}", URL_SAFE_NO_PAD.encode(raw))
    }

    pub fn decode(text: &str) -> Result<Self> {
        let stripped = text
            .trim()
            .strip_prefix("MK8S-")
            .ok_or("Código Steam inválido.")?;
        let bytes = URL_SAFE_NO_PAD
            .decode(stripped)
            .map_err(|_| "Código Steam inválido.")?;
        if bytes.len() != 33 || bytes[0] != 1 {
            return Err("Código Steam incompatível.".into());
        }
        let lobby = u64::from_le_bytes(bytes[1..9].try_into().unwrap());
        let host = u64::from_le_bytes(bytes[9..17].try_into().unwrap());
        let token: [u8; 16] = bytes[17..33].try_into().unwrap();
        if lobby == 0 || host == 0 {
            return Err("Código Steam inválido.".into());
        }
        Ok(Self {
            v: 1,
            lobby,
            host,
            token,
        })
    }
}

impl Api {
    fn load(path: &Path, local: &Path) -> Result<Self> {
        #[cfg(windows)]
        {
            use std::os::windows::ffi::OsStrExt;
            use windows_sys::Win32::System::LibraryLoader::{
                LoadLibraryExW, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
            };
            if !path.is_file() {
                return Err(format!(
                    "steam_api64.dll não encontrada em {}",
                    path.display()
                ));
            }
            let app_id = std::env::var("MK8_STEAM_APPID")
                .ok()
                .filter(|s| !s.trim().is_empty() && s.trim().chars().all(|c| c.is_ascii_digit()))
                .unwrap_or_else(|| DEFAULT_APP_ID.to_owned());
            std::env::set_var("SteamAppId", &app_id);
            std::env::set_var("SteamGameId", &app_id);
            let _ = fs::write(local.join("steam_appid.txt"), &app_id);
            if let Ok(exe) = std::env::current_exe() {
                if let Some(dir) = exe.parent() {
                    let _ = fs::write(dir.join("steam_appid.txt"), &app_id);
                }
            }
            let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
            let handle = unsafe {
                LoadLibraryExW(
                    wide.as_ptr(),
                    std::ptr::null_mut(),
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
                )
            } as isize;
            if handle == 0 {
                return Err(format!(
                    "Falha ao carregar {}: {}",
                    path.display(),
                    std::io::Error::last_os_error()
                ));
            }
            let init = load_symbol::<unsafe extern "C" fn() -> bool>(handle, c"SteamAPI_Init")
                .or_else(|_| load_symbol(handle, c"SteamAPI_InitSafe"))?;
            Ok(Self {
                _handle: handle,
                is_steam_running: load_symbol(handle, c"SteamAPI_IsSteamRunning")?,
                init,
                run_callbacks: load_symbol(handle, c"SteamAPI_RunCallbacks")?,
                steam_user: load_symbol(handle, c"SteamUser")?,
                steam_friends: load_symbol(handle, c"SteamFriends")?,
                steam_matchmaking: load_symbol(handle, c"SteamMatchmaking")?,
                steam_networking: load_symbol(handle, c"SteamNetworking")?,
                steam_utils: load_symbol(handle, c"SteamUtils")?,
                get_steam_id: load_symbol(handle, c"SteamAPI_ISteamUser_GetSteamID")?,
                get_persona_name: load_symbol(handle, c"SteamAPI_ISteamFriends_GetPersonaName")?,
                activate_invite_dialog: load_symbol(
                    handle,
                    c"SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog",
                )?,
                set_rich_presence: load_symbol(handle, c"SteamAPI_ISteamFriends_SetRichPresence")?,
                clear_rich_presence: load_symbol(
                    handle,
                    c"SteamAPI_ISteamFriends_ClearRichPresence",
                )?,
                create_lobby: load_symbol(handle, c"SteamAPI_ISteamMatchmaking_CreateLobby")?,
                join_lobby: load_symbol(handle, c"SteamAPI_ISteamMatchmaking_JoinLobby")?,
                leave_lobby: load_symbol(handle, c"SteamAPI_ISteamMatchmaking_LeaveLobby")?,
                set_lobby_data: load_symbol(handle, c"SteamAPI_ISteamMatchmaking_SetLobbyData")?,
                get_lobby_data: load_symbol(handle, c"SteamAPI_ISteamMatchmaking_GetLobbyData")?,
                accept_p2p: load_symbol(
                    handle,
                    c"SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser",
                )?,
                allow_p2p_relay: load_symbol(
                    handle,
                    c"SteamAPI_ISteamNetworking_AllowP2PPacketRelay",
                )?,
                send_p2p: load_symbol(handle, c"SteamAPI_ISteamNetworking_SendP2PPacket")?,
                is_p2p_available: load_symbol(
                    handle,
                    c"SteamAPI_ISteamNetworking_IsP2PPacketAvailable",
                )?,
                read_p2p: load_symbol(handle, c"SteamAPI_ISteamNetworking_ReadP2PPacket")?,
                is_call_completed: load_symbol(handle, c"SteamAPI_ISteamUtils_IsAPICallCompleted")?,
                get_call_result: load_symbol(handle, c"SteamAPI_ISteamUtils_GetAPICallResult")?,
            })
        }
        #[cfg(not(windows))]
        {
            let _ = (path, local);
            Err("Steam API disponível apenas no Windows.".into())
        }
    }

    fn wait_call<T: Default + Copy>(&self, call: u64, callback_id: c_int) -> Result<T> {
        if call == 0 {
            return Err("Steam recusou a chamada assíncrona.".into());
        }
        let utils = unsafe { (self.steam_utils)() };
        if utils.is_null() {
            return Err("ISteamUtils indisponível.".into());
        }
        let deadline = Instant::now() + Duration::from_secs(10);
        while Instant::now() < deadline {
            unsafe { (self.run_callbacks)() };
            let mut failed = false;
            if unsafe { (self.is_call_completed)(utils, call, &mut failed) } {
                if failed {
                    return Err("A operação na Steam falhou.".into());
                }
                let mut result = T::default();
                let mut call_failed = false;
                let ok = unsafe {
                    (self.get_call_result)(
                        utils,
                        call,
                        (&mut result as *mut T).cast(),
                        std::mem::size_of::<T>() as c_int,
                        callback_id,
                        &mut call_failed,
                    )
                };
                if !ok || call_failed {
                    return Err("Não foi possível ler a resposta da Steam.".into());
                }
                return Ok(result);
            }
            thread::sleep(Duration::from_millis(20));
        }
        Err("Tempo esgotado aguardando resposta da Steam.".into())
    }
}

static STEAM_API: Mutex<Option<Arc<Api>>> = Mutex::new(None);

fn ensure_api(local: &Path) -> Result<Arc<Api>> {
    let mut guard = STEAM_API.lock().unwrap();
    if let Some(api) = guard.as_ref() {
        return Ok(api.clone());
    }
    let dll = find_dll(local).ok_or(
        "steam_api64.dll não encontrada. Abra um jogo na Steam ou copie steam_api64.dll para a pasta de dados (local/).",
    )?;
    let api = Api::load(&dll, local)?;
    if !unsafe { (api.is_steam_running)() } {
        return Err("O cliente Steam não está aberto. Abra a Steam e faça login antes de criar ou entrar na sala.".into());
    }
    if !unsafe { (api.init)() } {
        return Err("Não foi possível conectar ao cliente Steam. Verifique se você está logado na Steam (AppID 480) e tente novamente.".into());
    }
    let net = unsafe { (api.steam_networking)() };
    if !net.is_null() {
        unsafe { (api.allow_p2p_relay)(net, true) };
    }
    let arc = Arc::new(api);
    *guard = Some(arc.clone());
    Ok(arc)
}

#[derive(Serialize)]
pub struct SteamStatus {
    pub dll_found: bool,
    pub ready: bool,
    pub persona: Option<String>,
    pub steam_id: Option<String>,
}

pub fn probe_status(local: &Path) -> SteamStatus {
    let dll_found = find_dll(local).is_some();
    let guard = STEAM_API.lock().unwrap();
    if let Some(api) = guard.as_ref() {
        let user = unsafe { (api.steam_user)() };
        let friends = unsafe { (api.steam_friends)() };
        let steam_id = (!user.is_null()).then(|| unsafe { (api.get_steam_id)(user).to_string() });
        let persona = (!friends.is_null())
            .then(|| {
                let ptr = unsafe { (api.get_persona_name)(friends) };
                (!ptr.is_null())
                    .then(|| unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() })
            })
            .flatten();
        return SteamStatus {
            dll_found: true,
            ready: true,
            persona,
            steam_id,
        };
    }
    SteamStatus {
        dll_found,
        ready: false,
        persona: None,
        steam_id: None,
    }
}

/// Abstract P2P transport so we can unit-test the full Steam session handshake and relay
/// deterministically in memory without needing a live Steam client during automated tests.
pub trait P2PBackend: Send + Sync + 'static {
    fn local_user(&self) -> u64;
    fn tick(&self);
    fn send(&self, remote: u64, channel: i32, data: &[u8], reliable: bool) -> bool;
    fn recv(&self, channel: i32, buf: &mut [u8]) -> Option<(usize, u64)>;
    fn open_invite_dialog(&self, _lobby_id: u64) -> Result<()> {
        Err("Overlay da Steam indisponível no modo de simulação.".into())
    }
    fn leave_lobby(&self, _lobby_id: u64) {}
}

struct LiveSteamBackend {
    api: Arc<Api>,
}

impl P2PBackend for LiveSteamBackend {
    fn local_user(&self) -> u64 {
        let user = unsafe { (self.api.steam_user)() };
        if user.is_null() {
            0
        } else {
            unsafe { (self.api.get_steam_id)(user) }
        }
    }

    fn tick(&self) {
        unsafe { (self.api.run_callbacks)() };
    }

    fn send(&self, remote: u64, channel: i32, data: &[u8], reliable: bool) -> bool {
        let net = unsafe { (self.api.steam_networking)() };
        if net.is_null() {
            return false;
        }
        unsafe { (self.api.accept_p2p)(net, remote) };
        let flags = if reliable {
            EP2P_SEND_RELIABLE
        } else {
            EP2P_SEND_UNRELIABLE_NO_DELAY
        };
        unsafe {
            (self.api.send_p2p)(
                net,
                remote,
                data.as_ptr().cast(),
                data.len() as u32,
                flags,
                channel,
            )
        }
    }

    fn recv(&self, channel: i32, buf: &mut [u8]) -> Option<(usize, u64)> {
        let net = unsafe { (self.api.steam_networking)() };
        if net.is_null() {
            return None;
        }
        let mut size = 0u32;
        if !unsafe { (self.api.is_p2p_available)(net, &mut size, channel) } || size == 0 {
            return None;
        }
        let mut remote = 0u64;
        let mut read = 0u32;
        let ok = unsafe {
            (self.api.read_p2p)(
                net,
                buf.as_mut_ptr().cast(),
                buf.len() as u32,
                &mut read,
                &mut remote,
                channel,
            )
        };
        if ok && remote != 0 && (read as usize) <= buf.len() {
            unsafe { (self.api.accept_p2p)(net, remote) };
            Some((read as usize, remote))
        } else {
            None
        }
    }

    fn open_invite_dialog(&self, lobby_id: u64) -> Result<()> {
        let friends = unsafe { (self.api.steam_friends)() };
        if friends.is_null() {
            return Err("ISteamFriends indisponível.".into());
        }
        unsafe { (self.api.activate_invite_dialog)(friends, lobby_id) };
        Ok(())
    }

    fn leave_lobby(&self, lobby_id: u64) {
        let mm = unsafe { (self.api.steam_matchmaking)() };
        if !mm.is_null() && lobby_id != 0 {
            unsafe { (self.api.leave_lobby)(mm, lobby_id) };
        }
        let friends = unsafe { (self.api.steam_friends)() };
        if !friends.is_null() {
            unsafe { (self.api.clear_rich_presence)(friends) };
        }
    }
}

pub struct SteamRoom {
    pub code: String,
    pub lobby_id: u64,
    backend: Arc<dyn P2PBackend>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}

impl SteamRoom {
    pub fn host_live(local: &Path, identity: &str, name: &str) -> Result<(Self, Host, Invite)> {
        let api = ensure_api(local)?;
        let mm = unsafe { (api.steam_matchmaking)() };
        if mm.is_null() {
            return Err("ISteamMatchmaking indisponível.".into());
        }
        let call = unsafe { (api.create_lobby)(mm, ELOBBY_TYPE_FRIENDS_ONLY, 12) };
        let created: LobbyCreated = api.wait_call(call, LOBBY_CREATED_CALLBACK)?;
        if created.result != 1 || created.lobby_id == 0 {
            return Err(format!(
                "Steam recusou a criação do lobby (código {}).",
                created.result
            ));
        }
        let backend = Arc::new(LiveSteamBackend { api: api.clone() });
        let host_steam_id = backend.local_user();
        let (room, host, invite) =
            Self::host_with_backend(backend, created.lobby_id, host_steam_id, identity, name)?;
        let key_code = CString::new("mk8_code").unwrap();
        let val_code = CString::new(room.code.as_str()).unwrap();
        let key_id = CString::new("mk8_identity").unwrap();
        let val_id = CString::new(identity).unwrap();
        unsafe {
            (api.set_lobby_data)(mm, created.lobby_id, key_code.as_ptr(), val_code.as_ptr());
            (api.set_lobby_data)(mm, created.lobby_id, key_id.as_ptr(), val_id.as_ptr());
        }
        let friends = unsafe { (api.steam_friends)() };
        if !friends.is_null() {
            let k_connect = CString::new("connect").unwrap();
            let v_connect = CString::new(format!("+connect_lobby {}", created.lobby_id)).unwrap();
            let k_status = CString::new("status").unwrap();
            let v_status = CString::new("Jogando Mario Kart 8 Deluxe (Sala Steam)").unwrap();
            unsafe {
                (api.set_rich_presence)(friends, k_connect.as_ptr(), v_connect.as_ptr());
                (api.set_rich_presence)(friends, k_status.as_ptr(), v_status.as_ptr());
            }
        }
        Ok((room, host, invite))
    }

    pub fn host_with_backend(
        backend: Arc<dyn P2PBackend>,
        lobby_id: u64,
        host_steam_id: u64,
        identity: &str,
        name: &str,
    ) -> Result<(Self, Host, Invite)> {
        let host = Host::start_named("127.0.0.1:0".parse().unwrap(), identity, name, false)?;
        let host_addr = host.address;
        let owner_invite = host.invite(host_addr)?;
        // Pre-allocate the 11 guest invites in Host so each Steam guest gets a dedicated authenticated Channel
        let mut guest_invites = Vec::with_capacity(11);
        for _ in 0..11 {
            guest_invites.push(host.invite(host_addr)?);
        }
        let token = random::<16>();
        let code = SteamCode {
            v: 1,
            lobby: lobby_id,
            host: host_steam_id,
            token,
        }
        .encode();
        let stop = Arc::new(AtomicBool::new(false));
        let s = stop.clone();
        let b = backend.clone();
        let identity_owned = identity.to_owned();
        let worker = thread::spawn(move || {
            struct GuestProxy {
                socket: UdpSocket,
                invite_encoded: String,
            }
            let mut assigned: HashMap<u64, GuestProxy> = HashMap::new();
            let mut available = guest_invites.into_iter();
            let mut buf = [0u8; MAX_PACKET + 64];
            while !s.load(Ordering::Relaxed) {
                b.tick();
                // 1. Control channel: handle guest JOIN requests (`MK8J` + token + identity)
                while let Some((n, remote_steam_id)) = b.recv(P2P_CHANNEL_CONTROL, &mut buf) {
                    if n < 20 || &buf[..4] != b"MK8J" || buf[4..20] != token {
                        continue;
                    }
                    let req_id = std::str::from_utf8(&buf[20..n]).unwrap_or("");
                    if req_id != identity_owned {
                        let _ = b.send(
                            remote_steam_id,
                            P2P_CHANNEL_CONTROL,
                            b"MK8EInstala\xc3\xa7\xc3\xa3o incompat\xc3\xadvel com a sala Steam.",
                            true,
                        );
                        continue;
                    }
                    if let std::collections::hash_map::Entry::Vacant(e) =
                        assigned.entry(remote_steam_id)
                    {
                        let Some(inv) = available.next() else {
                            let _ = b.send(
                                remote_steam_id,
                                P2P_CHANNEL_CONTROL,
                                b"MK8ESala cheia (limite de 12 jogadores).",
                                true,
                            );
                            continue;
                        };
                        let Ok(sock) = UdpSocket::bind("127.0.0.1:0") else {
                            continue;
                        };
                        let _ = sock.set_nonblocking(true);
                        let _ = sock.connect(host_addr);
                        e.insert(GuestProxy {
                            socket: sock,
                            invite_encoded: inv.encode(),
                        });
                    }
                    if let Some(proxy) = assigned.get(&remote_steam_id) {
                        let mut reply = b"MK8I".to_vec();
                        reply.extend_from_slice(proxy.invite_encoded.as_bytes());
                        let _ = b.send(remote_steam_id, P2P_CHANNEL_CONTROL, &reply, true);
                    }
                }
                // 2. Data channel: Steam P2P -> local Host UDP
                while let Some((n, remote_steam_id)) = b.recv(P2P_CHANNEL_DATA, &mut buf) {
                    if let Some(proxy) = assigned.get(&remote_steam_id) {
                        let _ = proxy.socket.send(&buf[..n]);
                    }
                }
                // 3. Local Host UDP -> Steam P2P
                for (remote_steam_id, proxy) in &assigned {
                    while let Ok(n) = proxy.socket.recv(&mut buf) {
                        let _ = b.send(*remote_steam_id, P2P_CHANNEL_DATA, &buf[..n], false);
                    }
                }
                thread::sleep(Duration::from_millis(2));
            }
        });
        Ok((
            Self {
                code,
                lobby_id,
                backend,
                stop,
                worker: Some(worker),
            },
            host,
            owner_invite,
        ))
    }

    pub fn join_live(local: &Path, input: &str, identity: &str) -> Result<(Self, Gateway)> {
        let api = ensure_api(local)?;
        let mm = unsafe { (api.steam_matchmaking)() };
        if mm.is_null() {
            return Err("ISteamMatchmaking indisponível.".into());
        }
        let code = if input.trim().starts_with("MK8S-") {
            let parsed = SteamCode::decode(input)?;
            let call = unsafe { (api.join_lobby)(mm, parsed.lobby) };
            let _ = api.wait_call::<LobbyEnter>(call, LOBBY_ENTER_CALLBACK);
            parsed
        } else if let Ok(lobby_id) = input
            .trim()
            .strip_prefix("steam://")
            .unwrap_or(input.trim())
            .parse::<u64>()
        {
            let call = unsafe { (api.join_lobby)(mm, lobby_id) };
            let entered: LobbyEnter = api.wait_call(call, LOBBY_ENTER_CALLBACK)?;
            if entered.lobby_id == 0 {
                return Err("Não foi possível entrar no lobby da Steam.".into());
            }
            let key = CString::new("mk8_code").unwrap();
            let ptr = unsafe { (api.get_lobby_data)(mm, lobby_id, key.as_ptr()) };
            if ptr.is_null() {
                return Err("Lobby da Steam não contém código de sala do MK8-online.".into());
            }
            let code_str = unsafe { CStr::from_ptr(ptr).to_string_lossy() };
            SteamCode::decode(&code_str)?
        } else {
            return Err("Informe um código MK8S-… ou ID de lobby da Steam.".into());
        };
        let backend = Arc::new(LiveSteamBackend { api });
        Self::join_with_backend(backend, code, identity)
    }

    pub fn join_with_backend(
        backend: Arc<dyn P2PBackend>,
        code: SteamCode,
        identity: &str,
    ) -> Result<(Self, Gateway)> {
        let proxy = UdpSocket::bind("127.0.0.1:0").map_err(|e| e.to_string())?;
        proxy.set_nonblocking(true).map_err(|e| e.to_string())?;
        let proxy_addr = proxy.local_addr().map_err(|e| e.to_string())?;

        // Perform handshake with Host over P2P_CHANNEL_CONTROL
        let mut join_req = b"MK8J".to_vec();
        join_req.extend_from_slice(&code.token);
        join_req.extend_from_slice(identity.as_bytes());

        let deadline = Instant::now() + Duration::from_secs(10);
        let mut last_send = Instant::now() - Duration::from_secs(2);
        let mut buf = [0u8; MAX_PACKET + 64];
        let mut received_invite = None;

        while Instant::now() < deadline {
            backend.tick();
            if last_send.elapsed() >= Duration::from_millis(500) {
                let _ = backend.send(code.host, P2P_CHANNEL_CONTROL, &join_req, true);
                last_send = Instant::now();
            }
            if let Some((n, from)) = backend.recv(P2P_CHANNEL_CONTROL, &mut buf) {
                if from == code.host && n > 4 {
                    if &buf[..4] == b"MK8I" {
                        let text = std::str::from_utf8(&buf[4..n])
                            .map_err(|_| "Convite corrompido na Steam.")?;
                        let mut invite = Invite::decode(text, identity)?;
                        invite.endpoint = proxy_addr;
                        received_invite = Some(invite);
                        break;
                    } else if &buf[..4] == b"MK8E" {
                        let err = std::str::from_utf8(&buf[4..n]).unwrap_or("Recusado pelo host.");
                        return Err(err.to_owned());
                    }
                }
            }
            thread::sleep(Duration::from_millis(20));
        }

        let invite = received_invite.ok_or(
            "Sem resposta do anfitrião via Steam Relay. Verifique se o anfitrião está com a sala aberta na Steam.",
        )?;
        let gateway = Gateway::start(invite)?;
        let stop = Arc::new(AtomicBool::new(false));
        let s = stop.clone();
        let b = backend.clone();
        let host_steam_id = code.host;
        let worker = thread::spawn(move || {
            let mut gateway_addr: Option<SocketAddr> = None;
            let mut pkt = [0u8; MAX_PACKET + 64];
            while !s.load(Ordering::Relaxed) {
                b.tick();
                while let Ok((n, from)) = proxy.recv_from(&mut pkt) {
                    gateway_addr = Some(from);
                    let _ = b.send(host_steam_id, P2P_CHANNEL_DATA, &pkt[..n], false);
                }
                while let Some((n, from)) = b.recv(P2P_CHANNEL_DATA, &mut pkt) {
                    if from == host_steam_id {
                        if let Some(to) = gateway_addr {
                            let _ = proxy.send_to(&pkt[..n], to);
                        }
                    }
                }
                thread::sleep(Duration::from_millis(2));
            }
        });

        Ok((
            Self {
                code: code.encode(),
                lobby_id: code.lobby,
                backend,
                stop,
                worker: Some(worker),
            },
            gateway,
        ))
    }

    pub fn open_invite_overlay(&self) -> Result<()> {
        self.backend.open_invite_dialog(self.lobby_id)
    }
}

impl Drop for SteamRoom {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
        self.backend.leave_lobby(self.lobby_id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::VecDeque;

    type PacketQueue = HashMap<(u64, i32), VecDeque<(u64, Vec<u8>)>>;

    struct MockSteamBus {
        queues: Mutex<PacketQueue>,
    }

    impl MockSteamBus {
        fn new() -> Arc<Self> {
            Arc::new(Self {
                queues: Mutex::new(HashMap::new()),
            })
        }
    }

    struct MockSteamNode {
        steam_id: u64,
        bus: Arc<MockSteamBus>,
    }

    impl P2PBackend for MockSteamNode {
        fn local_user(&self) -> u64 {
            self.steam_id
        }
        fn tick(&self) {}
        fn send(&self, remote: u64, channel: i32, data: &[u8], _reliable: bool) -> bool {
            let mut queues = self.bus.queues.lock().unwrap();
            queues
                .entry((remote, channel))
                .or_default()
                .push_back((self.steam_id, data.to_vec()));
            true
        }
        fn recv(&self, channel: i32, buf: &mut [u8]) -> Option<(usize, u64)> {
            let mut queues = self.bus.queues.lock().unwrap();
            let queue = queues.get_mut(&(self.steam_id, channel))?;
            let (from, data) = queue.pop_front()?;
            if data.len() > buf.len() {
                return None;
            }
            buf[..data.len()].copy_from_slice(&data);
            Some((data.len(), from))
        }
    }

    #[test]
    fn steam_code_roundtrip_and_validation() {
        let code = SteamCode {
            v: 1,
            lobby: 109775241012345678,
            host: 76561198000000001,
            token: [7u8; 16],
        };
        let encoded = code.encode();
        assert!(encoded.starts_with("MK8S-"));
        let decoded = SteamCode::decode(&encoded).unwrap();
        assert_eq!(decoded.lobby, code.lobby);
        assert_eq!(decoded.host, code.host);
        assert_eq!(decoded.token, code.token);
        assert!(SteamCode::decode("MK8S-invalid").is_err());
    }

    #[test]
    fn steam_p2p_relay_connects_and_forwards_bidirectional_game_packets() {
        let bus = MockSteamBus::new();
        let host_node = Arc::new(MockSteamNode {
            steam_id: 76561198000000001,
            bus: bus.clone(),
        });
        let guest_node = Arc::new(MockSteamNode {
            steam_id: 76561198000000002,
            bus: bus.clone(),
        });

        let (steam_host, host, owner_invite) = SteamRoom::host_with_backend(
            host_node,
            109775241000000001,
            76561198000000001,
            "fixture-v1",
            "HostPlayer",
        )
        .unwrap();
        let host_gateway = Gateway::start(owner_invite).unwrap();

        let parsed = SteamCode::decode(&steam_host.code).unwrap();
        let (_steam_guest, guest_gateway) =
            SteamRoom::join_with_backend(guest_node, parsed, "fixture-v1").unwrap();

        let deadline = Instant::now() + Duration::from_secs(3);
        while !(host_gateway.connected() && guest_gateway.connected()) && Instant::now() < deadline
        {
            thread::sleep(Duration::from_millis(10));
        }
        assert!(host_gateway.connected());
        assert!(guest_gateway.connected());
        assert_eq!(host.connected(), 2);

        // Bind synthetic game sockets to both local gateways and exchange MMKDVL1 packets
        let bind_game = |gw: &Gateway| {
            let s = UdpSocket::bind("127.0.0.1:0").unwrap();
            s.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let join = [b"MMKJ".as_slice(), &[gw.slot, 9], b"MK8ONLINE"].concat();
            s.send_to(&join, gw.local_addr).unwrap();
            let mut b = [0u8; 32];
            let (n, _) = s.recv_from(&mut b).unwrap();
            assert_eq!(&b[..n], b"MMKW\x00");
            s
        };

        let g_host = bind_game(&host_gateway);
        let g_guest = bind_game(&guest_gateway);

        let pkt_from_host = [
            b"MMKDVL1".as_slice(),
            &[10, 13, 37, 1, 0, 1, 255, 255, 255, 255, 0, 2],
            b"steam-race-state",
        ]
        .concat();
        g_host
            .send_to(&pkt_from_host, host_gateway.local_addr)
            .unwrap();
        let mut recv_buf = [0u8; 256];
        let (n, _) = g_guest.recv_from(&mut recv_buf).unwrap();
        assert_eq!(&recv_buf[..n], pkt_from_host.as_slice());

        let pkt_from_guest = [
            b"MMKDVL1".as_slice(),
            &[10, 13, 37, 2, 0, 1, 10, 13, 37, 1, 0, 2],
            b"guest-input",
        ]
        .concat();
        g_guest
            .send_to(&pkt_from_guest, guest_gateway.local_addr)
            .unwrap();
        let (n, _) = g_host.recv_from(&mut recv_buf).unwrap();
        assert_eq!(&recv_buf[..n], pkt_from_guest.as_slice());
    }
}
