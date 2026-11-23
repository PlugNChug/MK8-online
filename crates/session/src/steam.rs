//! Dynamic Steamworks lobby and P2P relay adapter.
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
// Steam rejects unreliable packets above this size. Larger encrypted datagrams
// need its reliable fragmentation/reassembly; small game packets keep low latency.
const STEAM_UNRELIABLE_MAX: usize = 1200;
const P2P_CHANNEL_CONTROL: c_int = 0;
const P2P_CHANNEL_DATA: c_int = 1;
const DEFAULT_APP_ID: &str = "480";
#[cfg(windows)]
const STEAM_LIBRARY: &str = "steam_api64.dll";
#[cfg(not(windows))]
const STEAM_LIBRARY: &str = "libsteam_api.so";
// Callbacks delivered through SteamAPI_RegisterCallback (steam_api.h k_i* values).
const GAME_LOBBY_JOIN_REQUESTED: c_int = 333; // invite accepted in the Steam client
const GAME_RICH_PRESENCE_JOIN_REQUESTED: c_int = 337; // "Join game" on a friend
const LOBBY_INVITE: c_int = 503; // a friend invited us
const P2P_SESSION_REQUEST: c_int = 1202;
const FRIEND_FLAG_IMMEDIATE: c_int = 4;
/// Lobby data key with the host's effective Booster Course Pass state ("1" installed and enabled).
pub const DLC_KEY: &str = "mk8_dlc";

/// Players must share the same DLC state: otherwise the vote can offer tracks one game lacks.
/// A lobby without the key comes from a launcher without DLC support, so its host has none.
pub fn check_dlc(host: Option<&str>, local: bool) -> Result<()> {
    let host = match host {
        None | Some("") | Some("0") => false,
        Some("1") => true,
        Some(_) => return Err("Sala da Steam com estado de DLC inválido.".into()),
    };
    match (host, local) {
        (true, false) => Err(
            "A sala usa o Booster Course Pass. Importe e ative o DLC em Configurações para entrar."
                .into(),
        ),
        (false, true) => Err(
            "A sala não usa o Booster Course Pass. Desative o DLC em Configurações para entrar."
                .into(),
        ),
        _ => Ok(()),
    }
}

/// Lobby data key with the host's CTGP pack: its identity (64 hex digits), "0" or absent without one.
pub const PACK_KEY: &str = "mk8_ctgp";

/// Players must share the same course pack: its courses take the place of others, so a different pack (or none)
/// would put players on different tracks. `local` is the local pack's identity, empty without one.
pub fn check_pack(host: Option<&str>, local: &str) -> Result<()> {
    let host = match host {
        None | Some("") | Some("0") => "",
        Some(id) if id.len() == 64 && id.bytes().all(|b| b.is_ascii_hexdigit()) => id,
        Some(_) => return Err("Sala da Steam com pacote CTGP inválido.".into()),
    };
    match (host.is_empty(), local.is_empty()) {
        (false, true) => Err(
            "A sala usa o CTGP Deluxe. Importe e ative o CTGP em Configurações para entrar.".into(),
        ),
        (true, false) => Err(
            "A sala não usa o CTGP Deluxe. Desative o CTGP em Configurações para entrar.".into(),
        ),
        (false, false) if !host.eq_ignore_ascii_case(local) => Err(
            "A sala usa outra versão do CTGP Deluxe. Importe a mesma versão para entrar.".into(),
        ),
        _ => Ok(()),
    }
}

// Valve uses 4-byte packing for Linux callback structures and 8 on Windows.
#[repr(C)]
#[cfg_attr(target_os = "linux", repr(packed(4)))]
#[cfg_attr(windows, repr(packed(8)))]
#[derive(Clone, Copy, Default)]
struct LobbyCreated {
    result: u32,
    lobby_id: u64,
}

// Valve uses 4-byte packing for Linux callback structures and 8 on Windows.
#[repr(C)]
#[cfg_attr(target_os = "linux", repr(packed(4)))]
#[cfg_attr(windows, repr(packed(8)))]
#[derive(Clone, Copy, Default)]
struct LobbyEnter {
    lobby_id: u64,
    chat_permissions: u32,
    locked: u8,
    response: u32,
}

#[cfg(windows)]
struct Library(windows_sys::Win32::Foundation::HMODULE);

#[cfg(windows)]
unsafe impl Send for Library {}
#[cfg(windows)]
unsafe impl Sync for Library {}

#[cfg(windows)]
impl Drop for Library {
    fn drop(&mut self) {
        unsafe { windows_sys::Win32::Foundation::FreeLibrary(self.0) };
    }
}

#[cfg(windows)]
impl Library {
    fn open(path: &Path) -> Result<Self> {
        use std::os::windows::ffi::OsStrExt;
        use windows_sys::Win32::System::LibraryLoader::{
            LoadLibraryExW, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR,
        };
        let wide: Vec<u16> = path.as_os_str().encode_wide().chain(Some(0)).collect();
        let handle = unsafe {
            LoadLibraryExW(
                wide.as_ptr(),
                std::ptr::null_mut(),
                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            )
        };
        if handle.is_null() {
            return Err(format!(
                "Falha ao carregar {}: {}",
                path.display(),
                std::io::Error::last_os_error()
            ));
        }
        Ok(Self(handle))
    }

    fn symbol<T: Copy>(&self, name: &CStr) -> Result<T> {
        use windows_sys::Win32::System::LibraryLoader::GetProcAddress;
        let address = unsafe { GetProcAddress(self.0, name.as_ptr().cast()) }.ok_or_else(|| {
            format!(
                "{STEAM_LIBRARY} sem função exigida: {}",
                name.to_string_lossy()
            )
        })?;
        Ok(unsafe { std::mem::transmute_copy(&address) })
    }
}

#[cfg(target_os = "linux")]
struct Library(libloading::os::unix::Library);

#[cfg(target_os = "linux")]
impl Library {
    fn open(path: &Path) -> Result<Self> {
        use libloading::os::unix::{Library, RTLD_LOCAL, RTLD_NOW};
        let path = path
            .canonicalize()
            .map_err(|e| format!("{}: {e}", path.display()))?;
        unsafe { Library::open(Some(&path), RTLD_NOW | RTLD_LOCAL) }
            .map(Self)
            .map_err(|e| format!("Falha ao carregar {}: {e}", path.display()))
    }

    fn symbol<T: Copy>(&self, name: &CStr) -> Result<T> {
        unsafe { self.0.get::<T>(name.to_bytes_with_nul()) }
            .map(|symbol| *symbol)
            .map_err(|e| {
                format!(
                    "{STEAM_LIBRARY} sem função exigida {}: {e}",
                    name.to_string_lossy()
                )
            })
    }
}

#[cfg(not(any(windows, target_os = "linux")))]
struct Library;

#[cfg(not(any(windows, target_os = "linux")))]
impl Library {
    fn open(_: &Path) -> Result<Self> {
        Err("Steam disponível no Windows e Linux.".into())
    }

    fn symbol<T: Copy>(&self, _: &CStr) -> Result<T> {
        Err("Steam disponível no Windows e Linux.".into())
    }
}

enum SteamInit {
    Legacy(unsafe extern "C" fn() -> bool),
    Flat(unsafe extern "C" fn(*mut [c_char; 1024]) -> c_int),
}

impl SteamInit {
    fn load(library: &Library) -> Result<Self> {
        library
            .symbol(c"SteamAPI_Init")
            .or_else(|_| library.symbol(c"SteamAPI_InitSafe"))
            .map(Self::Legacy)
            .or_else(|_| library.symbol(c"SteamAPI_InitFlat").map(Self::Flat))
    }

    fn run(&self) -> Result<()> {
        let message = "Não foi possível conectar ao cliente Steam. Verifique se você está logado na Steam e tente novamente.";
        match self {
            Self::Legacy(init) if unsafe { init() } => Ok(()),
            Self::Legacy(_) => Err(message.into()),
            Self::Flat(init) => {
                let mut error = [0; 1024];
                let status = unsafe { init(&mut error) };
                if status == 0 {
                    return Ok(());
                }
                // Leave a NUL terminator even if the library filled the entire buffer.
                error[1023] = 0;
                let detail = unsafe { CStr::from_ptr(error.as_ptr()) }.to_string_lossy();
                Err(format!("{message} ({status}: {detail})"))
            }
        }
    }
}

type InterfaceAccessor = unsafe extern "C" fn() -> *mut c_void;

struct Api {
    _library: Library,
    is_steam_running: unsafe extern "C" fn() -> bool,
    init: SteamInit,
    shutdown: unsafe extern "C" fn(),
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
    friend_count: unsafe extern "C" fn(*mut c_void, c_int) -> c_int,
    friend_by_index: unsafe extern "C" fn(*mut c_void, c_int, c_int) -> u64,
    friend_name: unsafe extern "C" fn(*mut c_void, u64) -> *const c_char,
    friend_state: unsafe extern "C" fn(*mut c_void, u64) -> c_int,
    friend_game: unsafe extern "C" fn(*mut c_void, u64, *mut FriendGame) -> bool,
    friend_rich_presence: unsafe extern "C" fn(*mut c_void, u64, *const c_char) -> *const c_char,
    invite_to_lobby: unsafe extern "C" fn(*mut c_void, u64, u64) -> bool,
    register_callback: unsafe extern "C" fn(*mut Callback, c_int),
}

/// FriendGameInfo_t, including the lobby a friend is playing in.
#[repr(C)]
#[cfg_attr(target_os = "linux", repr(packed(4)))]
#[derive(Default)]
struct FriendGame {
    game_id: u64,
    ip: u32,
    game_port: u16,
    query_port: u16,
    lobby: u64,
}

/// CCallbackBase's virtual slots differ between the MSVC and Itanium C++ ABIs.
#[repr(C)]
struct CallbackVTable {
    #[cfg(windows)]
    run_call_result: unsafe extern "C" fn(*mut Callback, *mut c_void, bool, u64),
    run: unsafe extern "C" fn(*mut Callback, *mut c_void),
    #[cfg(not(windows))]
    run_call_result: unsafe extern "C" fn(*mut Callback, *mut c_void, bool, u64),
    size: unsafe extern "C" fn(*mut Callback) -> c_int,
}

#[repr(C)]
struct Callback {
    vtable: *const CallbackVTable,
    flags: u8,
    id: c_int,
}

// Steam keeps these pointers; the table is immutable.
unsafe impl Sync for CallbackVTable {}

/// What the Steam client told this launcher, for the server to act on.
#[derive(Clone, Debug, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum SteamEvent {
    /// The player accepted an invite (Steam chat/overlay) or chose "Join game" on a friend: enter that lobby.
    Join { lobby: u64 },
    /// A friend invited the player; shown in the launcher until accepted.
    Invited { lobby: u64, friend: u64 },
}

static EVENTS: Mutex<Vec<SteamEvent>> = Mutex::new(Vec::new());
// The last lobby this launcher entered, as LobbyEnter_t reported it: proof that registered callbacks are delivered.
static ENTERED_LOBBY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);

/// True once Steam has delivered a registered callback to this process (creating or entering a room does).
pub fn callbacks_delivered() -> bool {
    ENTERED_LOBBY.load(Ordering::Relaxed) != 0
}
// SteamAPI_RunCallbacks dispatches on the calling thread; the rooms' workers and the idle pump take turns.
static PUMP: Mutex<()> = Mutex::new(());

fn callback_size(id: c_int) -> c_int {
    match id {
        GAME_LOBBY_JOIN_REQUESTED => 16,
        GAME_RICH_PRESENCE_JOIN_REQUESTED => 8 + 256,
        LOBBY_INVITE => 24,
        LOBBY_ENTER_CALLBACK => std::mem::size_of::<LobbyEnter>() as c_int,
        _ => 8,
    }
}

/// Reads one delivered callback (`param`: its `callback_size(id)` bytes).
fn parse_callback(id: c_int, param: &[u8]) -> Option<SteamEvent> {
    let u64_at = |at: usize| {
        param
            .get(at..at + 8)
            .map(|b| u64::from_le_bytes(b.try_into().unwrap()))
    };
    match id {
        GAME_LOBBY_JOIN_REQUESTED => Some(SteamEvent::Join {
            lobby: u64_at(0).filter(|&l| l != 0)?,
        }),
        GAME_RICH_PRESENCE_JOIN_REQUESTED => {
            let text = param.get(8..)?;
            let end = text.iter().position(|&b| b == 0).unwrap_or(text.len());
            let connect = std::str::from_utf8(&text[..end]).ok()?;
            let lobby = connect
                .trim()
                .strip_prefix("+connect_lobby")?
                .trim()
                .parse()
                .ok()
                .filter(|&l| l != 0)?;
            Some(SteamEvent::Join { lobby })
        }
        LOBBY_INVITE => Some(SteamEvent::Invited {
            friend: u64_at(0)?,
            lobby: u64_at(8).filter(|&l| l != 0)?,
        }),
        _ => None,
    }
}

unsafe extern "C" fn callback_run(this: *mut Callback, param: *mut c_void) {
    if this.is_null() || param.is_null() {
        return;
    }
    let id = (*this).id;
    let bytes = std::slice::from_raw_parts(param.cast::<u8>(), callback_size(id) as usize);
    if id == P2P_SESSION_REQUEST {
        // A lobby member opening a P2P session with us: accept it (the rooms read packets per channel).
        let remote = u64::from_le_bytes(bytes[..8].try_into().unwrap());
        // (try_lock: this runs inside SteamAPI_RunCallbacks, possibly while ensure_api holds the lock)
        let api = STEAM_API.try_lock().ok().and_then(|g| g.clone());
        if let Some(api) = api {
            let net = (api.steam_networking)();
            if !net.is_null() && remote != 0 {
                (api.accept_p2p)(net, remote);
            }
        }
    } else if id == LOBBY_ENTER_CALLBACK {
        let lobby = u64::from_le_bytes(bytes[..8].try_into().unwrap());
        ENTERED_LOBBY.store(lobby, Ordering::Relaxed);
    } else if let Some(event) = parse_callback(id, bytes) {
        if let Ok(mut events) = EVENTS.lock() {
            if events.len() < 32 && !events.contains(&event) {
                events.push(event);
            }
        }
    }
}

unsafe extern "C" fn callback_run_result(
    this: *mut Callback,
    param: *mut c_void,
    _failed: bool,
    _call: u64,
) {
    callback_run(this, param);
}

unsafe extern "C" fn callback_size_bytes(this: *mut Callback) -> c_int {
    if this.is_null() {
        0
    } else {
        callback_size((*this).id)
    }
}

static CALLBACK_VTABLE: CallbackVTable = CallbackVTable {
    run_call_result: callback_run_result,
    run: callback_run,
    size: callback_size_bytes,
};

/// Steam events received since the last call.
pub fn take_events() -> Vec<SteamEvent> {
    EVENTS
        .lock()
        .map(|mut e| std::mem::take(&mut *e))
        .unwrap_or_default()
}

fn pump(api: &Api) {
    let _turn = PUMP.lock();
    unsafe { (api.run_callbacks)() };
}

// Flat C accessors are versioned exports; SteamUser() and friends are normally
// C++ header helpers, not DLL exports. Keep legacy exports for existing adapters.
// Each list holds only versions whose flat functions used here, their callback IDs
// and structures are identical in the Steamworks SDK 1.52-1.65 steam_api.json
// (issues #69-#71, #73). Do not guess newer ABI versions or substitute an unrelated interface.
const STEAM_USER: &[&CStr] = &[
    c"SteamAPI_SteamUser_v023",
    c"SteamAPI_SteamUser_v022",
    c"SteamAPI_SteamUser_v021",
];
const STEAM_FRIENDS: &[&CStr] = &[c"SteamAPI_SteamFriends_v017", c"SteamAPI_SteamFriends_v018"];
const STEAM_MATCHMAKING: &[&CStr] = &[c"SteamAPI_SteamMatchmaking_v009"];
const STEAM_NETWORKING: &[&CStr] = &[c"SteamAPI_SteamNetworking_v006"];
const STEAM_UTILS: &[&CStr] = &[c"SteamAPI_SteamUtils_v010", c"SteamAPI_SteamUtils_v011"];

fn resolve_interface(
    versioned: &[&CStr],
    legacy: &CStr,
    mut lookup: impl FnMut(&CStr) -> Result<InterfaceAccessor>,
) -> Result<InterfaceAccessor> {
    versioned
        .iter()
        .chain([&legacy])
        .find_map(|name| lookup(name).ok())
        .ok_or_else(|| {
            let accepted: Vec<_> = versioned
                .iter()
                .map(|name| name.to_string_lossy())
                .collect();
            format!(
                "Biblioteca Steam incompatível: interface exigida {} (ou {}) ausente.",
                accepted.join(", "),
                legacy.to_string_lossy()
            )
        })
}

fn load_interface(
    library: &Library,
    versioned: &[&CStr],
    legacy: &CStr,
) -> Result<InterfaceAccessor> {
    resolve_interface(versioned, legacy, |name| library.symbol(name))
}

#[cfg(windows)]
fn usable_library(path: &Path) -> bool {
    path.is_file()
}

#[cfg(target_os = "linux")]
fn usable_library(path: &Path) -> bool {
    use std::io::Read;
    // Ignore incompatible binaries in Steam libraries (notably 32-bit Linux and Android).
    let mut header = [0; 20];
    path.is_file()
        && fs::File::open(path)
            .and_then(|mut file| file.read_exact(&mut header))
            .is_ok()
        && &header[..7] == b"\x7fELF\x02\x01\x01"
        && header[16..20] == [3, 0, 62, 0] // ET_DYN, EM_X86_64
}

#[cfg(not(any(windows, target_os = "linux")))]
fn usable_library(_: &Path) -> bool {
    false
}

pub fn candidate_dll_paths(local: &Path) -> Vec<PathBuf> {
    dll_candidates(local, cached_library_dll_paths).collect()
}

fn dll_candidates(
    local: &Path,
    library_paths: impl FnOnce() -> Vec<PathBuf>,
) -> impl Iterator<Item = PathBuf> {
    let mut paths = Vec::new();
    if let Ok(env_path) = std::env::var("MK8_STEAM_DLL") {
        let p = PathBuf::from(env_path.trim());
        if !p.as_os_str().is_empty() {
            paths.push(p);
        }
    }
    paths.push(local.join(STEAM_LIBRARY));
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            paths.push(parent.join(STEAM_LIBRARY));
        }
    }
    // An explicit/local library is tried first; installed games are scanned only if it cannot be used.
    paths
        .into_iter()
        .chain(std::iter::once_with(library_paths).flatten())
}

fn cached_library_dll_paths() -> Vec<PathBuf> {
    // Status polls every two seconds; do not rescan installed games on each poll.
    static CACHE: Mutex<Option<(Instant, Vec<PathBuf>)>> = Mutex::new(None);
    let mut cache = CACHE.lock().unwrap();
    if cache
        .as_ref()
        .is_none_or(|(at, _)| at.elapsed() > Duration::from_secs(30))
    {
        *cache = Some((Instant::now(), library_dll_paths(&steam_roots())));
    }
    cache.as_ref().unwrap().1.clone()
}

fn steam_roots() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    #[cfg(target_os = "linux")]
    {
        if let Some(data) = std::env::var_os("XDG_DATA_HOME")
            .map(PathBuf::from)
            .filter(|p| p.is_absolute())
        {
            roots.push(data.join("Steam"));
        }
        if let Some(home) = std::env::var_os("HOME").map(PathBuf::from) {
            roots.extend(
                [
                    ".local/share/Steam",
                    ".steam/steam",
                    ".steam/root",
                    ".var/app/com.valvesoftware.Steam/data/Steam",
                ]
                .map(|p| home.join(p)),
            );
        }
    }
    #[cfg(windows)]
    {
        roots.extend([
            PathBuf::from(r"C:\Program Files (x86)\Steam"),
            PathBuf::from(r"C:\Program Files\Steam"),
        ]);
        use windows_sys::Win32::System::Registry::{
            RegGetValueW, HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE, RRF_RT_REG_SZ,
        };
        for (key, subkey, name) in [
            (HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
            (
                HKEY_LOCAL_MACHINE,
                r"SOFTWARE\WOW6432Node\Valve\Steam",
                "InstallPath",
            ),
            (HKEY_LOCAL_MACHINE, r"SOFTWARE\Valve\Steam", "InstallPath"),
        ] {
            let subkey: Vec<u16> = subkey.encode_utf16().chain(Some(0)).collect();
            let name: Vec<u16> = name.encode_utf16().chain(Some(0)).collect();
            let mut buffer = [0u16; 32768];
            let mut bytes = std::mem::size_of_val(&buffer) as u32;
            let code = unsafe {
                RegGetValueW(
                    key,
                    subkey.as_ptr(),
                    name.as_ptr(),
                    RRF_RT_REG_SZ,
                    std::ptr::null_mut(),
                    buffer.as_mut_ptr().cast(),
                    &mut bytes,
                )
            };
            if code == 0 {
                let length = buffer.iter().position(|v| *v == 0).unwrap_or(buffer.len());
                if length > 0 {
                    roots.push(PathBuf::from(String::from_utf16_lossy(&buffer[..length])));
                }
            }
        }
    }
    roots.sort();
    roots.dedup();
    roots
}

fn library_dll_paths(roots: &[PathBuf]) -> Vec<PathBuf> {
    let mut libraries = roots.to_vec();
    for root in roots {
        for file in ["steamapps/libraryfolders.vdf", "config/libraryfolders.vdf"] {
            if let Ok(text) = fs::read_to_string(root.join(file)) {
                for line in text.lines() {
                    let parts: Vec<_> = line.split('"').collect();
                    if parts.len() >= 4
                        && (parts[1] == "path" || parts[1].chars().all(|c| c.is_ascii_digit()))
                    {
                        let path = PathBuf::from(parts[3].replace("\\\\", "\\"));
                        if path.is_absolute() && !libraries.contains(&path) {
                            libraries.push(path);
                        }
                    }
                }
            }
        }
    }
    let mut games = Vec::new();
    for library in libraries {
        if let Ok(entries) = fs::read_dir(library.join("steamapps/common")) {
            games.extend(
                entries
                    .flatten()
                    .filter(|entry| entry.file_type().is_ok_and(|kind| kind.is_dir()))
                    .map(|entry| entry.path()),
            );
        }
    }
    games.sort();
    games.dedup();
    #[cfg(windows)]
    const BINARIES: &[&str] = &[
        "bin/steam_api64.dll",
        "bin/win64/steam_api64.dll",
        "game/bin/win64/steam_api64.dll",
        "bin64/steam_api64.dll",
        "x64/steam_api64.dll",
        "Binaries/Win64/steam_api64.dll",
    ];
    #[cfg(not(windows))]
    const BINARIES: &[&str] = &[
        "bin/libsteam_api.so",
        "bin/linux64/libsteam_api.so",
        "bin/linuxsteamrt64/libsteam_api.so",
        "game/bin/linuxsteamrt64/libsteam_api.so",
        "lib64/libsteam_api.so",
        "linux64/libsteam_api.so",
        "Binaries/Linux/libsteam_api.so",
    ];
    #[cfg(windows)]
    const UNREAL: &str = "Win64/steam_api64.dll";
    #[cfg(not(windows))]
    const UNREAL: &str = "x86_64-unknown-linux-gnu/libsteam_api.so";
    #[cfg(windows)]
    const UNITY: &[&str] = &["Plugins/steam_api64.dll", "Plugins/x86_64/steam_api64.dll"];
    #[cfg(not(windows))]
    const UNITY: &[&str] = &["Plugins/libsteam_api.so", "Plugins/x86_64/libsteam_api.so"];

    let mut found = Vec::new();
    // Prefer the game-root layout used by previous releases, before nested layouts.
    for game in &games {
        let path = game.join(STEAM_LIBRARY);
        if usable_library(&path) {
            found.push(path);
        }
    }
    for game in games {
        for relative in BINARIES {
            let path = game.join(relative);
            if usable_library(&path) {
                found.push(path);
            }
        }
        // Unreal titles (including Dead by Daylight) keep the redistributable under
        // a Steamworks version directory. The version is not fixed by our launcher.
        if let Ok(versions) = fs::read_dir(game.join("Engine/Binaries/ThirdParty/Steamworks")) {
            let mut versions: Vec<_> = versions.flatten().map(|entry| entry.path()).collect();
            versions.sort();
            for version in versions {
                let path = version.join(UNREAL);
                if usable_library(&path) {
                    found.push(path);
                }
            }
        }
        // Unity keeps its native plugins beneath a game-specific *_Data directory.
        if let Ok(entries) = fs::read_dir(&game) {
            for entry in entries
                .flatten()
                .filter(|entry| entry.file_name().to_string_lossy().ends_with("_Data"))
            {
                for relative in UNITY {
                    let path = entry.path().join(relative);
                    if usable_library(&path) {
                        found.push(path);
                    }
                }
            }
        }
    }
    found
}

pub fn find_dll(local: &Path) -> Option<PathBuf> {
    dll_candidates(local, cached_library_dll_paths).find(|path| usable_library(path))
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

fn configured_app_id() -> String {
    std::env::var("MK8_STEAM_APPID")
        .ok()
        .map(|id| id.trim().to_owned())
        .filter(|id| !id.is_empty() && id.bytes().all(|c| c.is_ascii_digit()))
        .unwrap_or_else(|| DEFAULT_APP_ID.to_owned())
}

/// Set Steam's application ID before loading native libraries or starting worker threads.
///
/// # Safety
///
/// The caller must ensure that no other thread can access the process environment.
#[cfg(target_os = "linux")]
pub unsafe fn configure_process() {
    let app_id = configured_app_id();
    std::env::set_var("SteamAppId", &app_id);
    std::env::set_var("SteamGameId", &app_id);
}

impl Api {
    fn load(path: &Path, local: &Path) -> Result<Self> {
        let library = Library::open(path)?;
        let init = SteamInit::load(&library)?;
        let app_id = configured_app_id();
        let _ = fs::write(local.join("steam_appid.txt"), &app_id);
        #[cfg(windows)]
        {
            std::env::set_var("SteamAppId", &app_id);
            std::env::set_var("SteamGameId", &app_id);
            if let Ok(exe) = std::env::current_exe() {
                if let Some(dir) = exe.parent() {
                    let _ = fs::write(dir.join("steam_appid.txt"), &app_id);
                }
            }
        }
        Ok(Self {
            is_steam_running: library.symbol(c"SteamAPI_IsSteamRunning")?,
            init,
            shutdown: library.symbol(c"SteamAPI_Shutdown")?,
            run_callbacks: library.symbol(c"SteamAPI_RunCallbacks")?,
            steam_user: load_interface(&library, STEAM_USER, c"SteamUser")?,
            steam_friends: load_interface(&library, STEAM_FRIENDS, c"SteamFriends")?,
            steam_matchmaking: load_interface(&library, STEAM_MATCHMAKING, c"SteamMatchmaking")?,
            steam_networking: load_interface(&library, STEAM_NETWORKING, c"SteamNetworking")?,
            steam_utils: load_interface(&library, STEAM_UTILS, c"SteamUtils")?,
            get_steam_id: library.symbol(c"SteamAPI_ISteamUser_GetSteamID")?,
            get_persona_name: library.symbol(c"SteamAPI_ISteamFriends_GetPersonaName")?,
            activate_invite_dialog: library
                .symbol(c"SteamAPI_ISteamFriends_ActivateGameOverlayInviteDialog")?,
            set_rich_presence: library.symbol(c"SteamAPI_ISteamFriends_SetRichPresence")?,
            clear_rich_presence: library.symbol(c"SteamAPI_ISteamFriends_ClearRichPresence")?,
            create_lobby: library.symbol(c"SteamAPI_ISteamMatchmaking_CreateLobby")?,
            join_lobby: library.symbol(c"SteamAPI_ISteamMatchmaking_JoinLobby")?,
            leave_lobby: library.symbol(c"SteamAPI_ISteamMatchmaking_LeaveLobby")?,
            set_lobby_data: library.symbol(c"SteamAPI_ISteamMatchmaking_SetLobbyData")?,
            get_lobby_data: library.symbol(c"SteamAPI_ISteamMatchmaking_GetLobbyData")?,
            accept_p2p: library.symbol(c"SteamAPI_ISteamNetworking_AcceptP2PSessionWithUser")?,
            allow_p2p_relay: library.symbol(c"SteamAPI_ISteamNetworking_AllowP2PPacketRelay")?,
            send_p2p: library.symbol(c"SteamAPI_ISteamNetworking_SendP2PPacket")?,
            is_p2p_available: library.symbol(c"SteamAPI_ISteamNetworking_IsP2PPacketAvailable")?,
            read_p2p: library.symbol(c"SteamAPI_ISteamNetworking_ReadP2PPacket")?,
            is_call_completed: library.symbol(c"SteamAPI_ISteamUtils_IsAPICallCompleted")?,
            get_call_result: library.symbol(c"SteamAPI_ISteamUtils_GetAPICallResult")?,
            friend_count: library.symbol(c"SteamAPI_ISteamFriends_GetFriendCount")?,
            friend_by_index: library.symbol(c"SteamAPI_ISteamFriends_GetFriendByIndex")?,
            friend_name: library.symbol(c"SteamAPI_ISteamFriends_GetFriendPersonaName")?,
            friend_state: library.symbol(c"SteamAPI_ISteamFriends_GetFriendPersonaState")?,
            friend_game: library.symbol(c"SteamAPI_ISteamFriends_GetFriendGamePlayed")?,
            friend_rich_presence: library
                .symbol(c"SteamAPI_ISteamFriends_GetFriendRichPresence")?,
            invite_to_lobby: library.symbol(c"SteamAPI_ISteamMatchmaking_InviteUserToLobby")?,
            register_callback: library.symbol(c"SteamAPI_RegisterCallback")?,
            _library: library,
        })
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
            pump(self);
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

fn compatible_api<T>(
    paths: impl IntoIterator<Item = PathBuf>,
    mut initialize: impl FnMut(&Path) -> Result<Option<T>>,
) -> Result<T> {
    let mut failures = Vec::new();
    for path in paths {
        if !path.is_file() {
            continue;
        }
        match initialize(&path) {
            Ok(Some(api)) => return Ok(api),
            Ok(None) => return Err("O cliente Steam não está aberto. Abra a Steam e faça login antes de criar ou entrar na sala.".into()),
            Err(error) => failures.push(format!("{}: {error}", path.display())),
        }
    }
    if failures.is_empty() {
        return Err(if cfg!(windows) {
            "steam_api64.dll não encontrada. Instalar ou abrir apenas a Steam não fornece essa biblioteca. Coloque a DLL Steamworks de 64 bits ao lado do Launcher.exe e reabra o Launcher. Não é necessário refazer a build do jogo.".into()
        } else {
            format!("{STEAM_LIBRARY} não encontrada. Use a biblioteca Steamworks de 64 bits em um jogo instalado na Steam, na pasta de dados do launcher ou no caminho MK8_STEAM_DLL.")
        });
    }
    Err(format!(
        "Não foi possível inicializar a Steam com as bibliotecas encontradas ({} verificadas). {}",
        failures.len(),
        failures.into_iter().take(4).collect::<Vec<_>>().join("\n")
    ))
}

// Ok(None) means the Steam client itself is offline. Only retry candidate-specific failures.
fn initialize_candidate(path: &Path, local: &Path) -> Result<Option<Api>> {
    // 32-bit or Android binaries in Steam folders cannot be loaded by this process.
    if !usable_library(path) {
        return Err("biblioteca de outra arquitetura".into());
    }
    // Resolve exports before initialization; a rejected library is released by Api::load.
    let api = Api::load(path, local)?;
    if !unsafe { (api.is_steam_running)() } {
        return Ok(None);
    }
    api.init.run()?;
    for (name, accessor) in [
        ("ISteamUser", api.steam_user),
        ("ISteamFriends", api.steam_friends),
        ("ISteamMatchmaking", api.steam_matchmaking),
        ("ISteamNetworking", api.steam_networking),
        ("ISteamUtils", api.steam_utils),
    ] {
        if unsafe { accessor() }.is_null() {
            unsafe { (api.shutdown)() };
            return Err(format!(
                "Interface Steam indisponível após inicialização: {name}."
            ));
        }
    }
    Ok(Some(api))
}

fn ensure_api(local: &Path) -> Result<Arc<Api>> {
    let mut guard = STEAM_API.lock().unwrap();
    if let Some(api) = guard.as_ref() {
        return Ok(api.clone());
    }
    let api = compatible_api(dll_candidates(local, cached_library_dll_paths), |path| {
        initialize_candidate(path, local)
    })?;
    let net = unsafe { (api.steam_networking)() };
    if !net.is_null() {
        unsafe { (api.allow_p2p_relay)(net, true) };
    }
    for id in [
        GAME_LOBBY_JOIN_REQUESTED,
        GAME_RICH_PRESENCE_JOIN_REQUESTED,
        LOBBY_INVITE,
        P2P_SESSION_REQUEST,
        LOBBY_ENTER_CALLBACK,
    ] {
        // Registered once for the life of the process (Steam keeps the pointer).
        let callback = Box::leak(Box::new(Callback {
            vtable: &CALLBACK_VTABLE,
            flags: 0,
            id,
        }));
        unsafe { (api.register_callback)(callback, id) };
    }
    let arc = Arc::new(api);
    *guard = Some(arc.clone());
    // Invites and join requests also arrive while no room is open: keep dispatching.
    let pumped = arc.clone();
    thread::spawn(move || loop {
        pump(&pumped);
        thread::sleep(Duration::from_millis(100));
    });
    Ok(arc)
}

/// A Steam friend as the Multiplayer tab lists them.
#[derive(Serialize)]
pub struct Friend {
    pub id: String,
    pub name: String,
    pub online: bool,
    pub presence: &'static str,
    /// The friend's open MK8-online room (the Steam lobby they are in, marked by our rich presence), if any.
    pub room: Option<String>,
}

fn friend_presence(state: i32) -> &'static str {
    match state {
        1 | 5 | 6 => "online",
        2 => "busy",
        3 | 4 => "away",
        _ => "offline",
    }
}

fn sort_friends(list: &mut [Friend]) {
    list.sort_by_cached_key(|friend| {
        let group = match friend.presence {
            "online" | "busy" => 0,
            "away" => 1,
            _ => 2,
        };
        (group, friend.name.to_lowercase(), friend.id.clone())
    });
}

/// Lists all immediate friends: online, away, then offline; names break ties.
pub fn friends(local: &Path) -> Result<Vec<Friend>> {
    let api = ensure_api(local)?;
    let friends = unsafe { (api.steam_friends)() };
    if friends.is_null() {
        return Err("ISteamFriends indisponível.".into());
    }
    let app: u64 = std::env::var("SteamAppId")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(480);
    let count = unsafe { (api.friend_count)(friends, FRIEND_FLAG_IMMEDIATE) }.max(0);
    let key = CString::new("mk8_room").unwrap();
    let mut list = Vec::new();
    for i in 0..count {
        let id = unsafe { (api.friend_by_index)(friends, i, FRIEND_FLAG_IMMEDIATE) };
        if id == 0 {
            continue;
        }
        let name_ptr = unsafe { (api.friend_name)(friends, id) };
        let name = if name_ptr.is_null() {
            id.to_string()
        } else {
            unsafe { CStr::from_ptr(name_ptr) }
                .to_string_lossy()
                .into_owned()
        };
        let presence = friend_presence(unsafe { (api.friend_state)(friends, id) });
        let online = presence != "offline";
        let mut game = FriendGame::default();
        let playing = unsafe { (api.friend_game)(friends, id, &mut game) };
        let marked = {
            let value = unsafe { (api.friend_rich_presence)(friends, id, key.as_ptr()) };
            !value.is_null() && unsafe { CStr::from_ptr(value) }.to_bytes() == b"1"
        };
        // The game id's low 24 bits are the app id.
        let lobby = game.lobby;
        let room = (playing && game.game_id & 0xff_ffff == app && lobby != 0 && marked)
            .then(|| lobby.to_string());
        list.push(Friend {
            id: id.to_string(),
            name,
            online,
            presence,
            room,
        });
    }
    sort_friends(&mut list);
    Ok(list)
}

/// Sends a Steam lobby invite to a friend (it appears in their Steam chat, and in their launcher).
pub fn invite(local: &Path, lobby: u64, friend: u64) -> Result<()> {
    let api = ensure_api(local)?;
    let mm = unsafe { (api.steam_matchmaking)() };
    if mm.is_null() || lobby == 0 || friend == 0 {
        return Err("Sala Steam indisponível para convite.".into());
    }
    if !unsafe { (api.invite_to_lobby)(mm, lobby, friend) } {
        return Err("A Steam recusou o convite.".into());
    }
    Ok(())
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
        pump(&self.api);
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
    pub fn host_live(
        local: &Path,
        identity: &str,
        name: &str,
        dlc: bool,
        pack: &str,
    ) -> Result<(Self, Host, Invite)> {
        let api = ensure_api(local)?;
        let mm = unsafe { (api.steam_matchmaking)() };
        if mm.is_null() {
            return Err("ISteamMatchmaking indisponível.".into());
        }
        let call = unsafe { (api.create_lobby)(mm, ELOBBY_TYPE_FRIENDS_ONLY, 12) };
        let LobbyCreated { result, lobby_id } = api.wait_call(call, LOBBY_CREATED_CALLBACK)?;
        if result != 1 || lobby_id == 0 {
            return Err(format!(
                "Steam recusou a criação do lobby (código {result})."
            ));
        }
        let backend = Arc::new(LiveSteamBackend { api: api.clone() });
        let host_steam_id = backend.local_user();
        let (room, host, invite) =
            Self::host_with_backend(backend, lobby_id, host_steam_id, identity, name)?;
        let key_code = CString::new("mk8_code").unwrap();
        let val_code = CString::new(room.code.as_str()).unwrap();
        let key_id = CString::new("mk8_identity").unwrap();
        let val_id = CString::new(identity).unwrap();
        let key_dlc = CString::new(DLC_KEY).unwrap();
        let val_dlc = CString::new(if dlc { "1" } else { "0" }).unwrap();
        let key_pack = CString::new(PACK_KEY).unwrap();
        let val_pack = CString::new(if pack.is_empty() { "0" } else { pack })
            .map_err(|_| "Identidade do CTGP inválida.")?;
        unsafe {
            (api.set_lobby_data)(mm, lobby_id, key_code.as_ptr(), val_code.as_ptr());
            (api.set_lobby_data)(mm, lobby_id, key_id.as_ptr(), val_id.as_ptr());
            (api.set_lobby_data)(mm, lobby_id, key_dlc.as_ptr(), val_dlc.as_ptr());
            (api.set_lobby_data)(mm, lobby_id, key_pack.as_ptr(), val_pack.as_ptr());
        }
        let friends = unsafe { (api.steam_friends)() };
        if !friends.is_null() {
            let k_connect = CString::new("connect").unwrap();
            let v_connect = CString::new(format!("+connect_lobby {}", lobby_id)).unwrap();
            let k_status = CString::new("status").unwrap();
            let v_status = CString::new("Jogando Mario Kart 8 Deluxe (Sala Steam)").unwrap();
            let k_room = CString::new("mk8_room").unwrap();
            let v_room = CString::new("1").unwrap();
            unsafe {
                (api.set_rich_presence)(friends, k_connect.as_ptr(), v_connect.as_ptr());
                (api.set_rich_presence)(friends, k_status.as_ptr(), v_status.as_ptr());
                (api.set_rich_presence)(friends, k_room.as_ptr(), v_room.as_ptr());
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
                        let mut error = b"MK8E".to_vec();
                        error.extend_from_slice(
                            "Instalação incompatível com a sala Steam.".as_bytes(),
                        );
                        let _ = b.send(remote_steam_id, P2P_CHANNEL_CONTROL, &error, true);
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
                        let _ = b.send(
                            *remote_steam_id,
                            P2P_CHANNEL_DATA,
                            &buf[..n],
                            n > STEAM_UNRELIABLE_MAX,
                        );
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

    pub fn join_live(
        local: &Path,
        input: &str,
        identity: &str,
        dlc: bool,
        pack: &str,
    ) -> Result<(Self, Gateway)> {
        let api = ensure_api(local)?;
        let mm = unsafe { (api.steam_matchmaking)() };
        if mm.is_null() {
            return Err("ISteamMatchmaking indisponível.".into());
        }
        let supplied = if input.trim().starts_with("MK8S-") {
            Some(SteamCode::decode(input)?)
        } else {
            None
        };
        let lobby_id = match &supplied {
            Some(code) => code.lobby,
            None => input
                .trim()
                .strip_prefix("steam://")
                .unwrap_or(input.trim())
                .parse::<u64>()
                .map_err(|_| "Informe um código MK8S-… ou ID de lobby da Steam.")?,
        };
        let backend = Arc::new(LiveSteamBackend { api: api.clone() });
        let result = (|| {
            let call = unsafe { (api.join_lobby)(mm, lobby_id) };
            let entered: LobbyEnter = api.wait_call(call, LOBBY_ENTER_CALLBACK)?;
            if entered.lobby_id != lobby_id || entered.response != 1 || lobby_id == 0 {
                return Err("Não foi possível entrar no lobby da Steam.".into());
            }
            let metadata = |key: &CStr| -> Result<String> {
                let ptr = unsafe { (api.get_lobby_data)(mm, lobby_id, key.as_ptr()) };
                if ptr.is_null() {
                    return Err("Lobby da Steam não contém código de sala do MK8-online.".into());
                }
                Ok(unsafe { CStr::from_ptr(ptr) }
                    .to_string_lossy()
                    .into_owned())
            };
            let host_identity = metadata(c"mk8_identity")?;
            if host_identity != identity {
                return Err("Instalação incompatível com a sala Steam.".into());
            }
            check_dlc(Some(&metadata(c"mk8_dlc")?), dlc)?;
            check_pack(Some(&metadata(c"mk8_ctgp")?), pack)?;
            let code = match supplied {
                Some(code) => code,
                None => SteamCode::decode(&metadata(c"mk8_code")?)?,
            };
            if code.lobby != lobby_id {
                return Err("Código Steam incompatível.".into());
            }
            Self::join_with_backend(backend.clone(), code, identity)
        })();
        if result.is_err() {
            backend.leave_lobby(lobby_id);
        }
        result
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
                    let _ = b.send(
                        host_steam_id,
                        P2P_CHANNEL_DATA,
                        &pkt[..n],
                        n > STEAM_UNRELIABLE_MAX,
                    );
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

    #[test]
    fn dlc_state_must_match_the_host_lobby() {
        // Same state on both sides is allowed, with or without the Booster Course Pass.
        assert!(check_dlc(Some("1"), true).is_ok());
        assert!(check_dlc(Some("0"), false).is_ok());
        // A lobby without the key comes from a launcher without DLC support.
        assert!(check_dlc(None, false).is_ok());
        assert!(check_dlc(Some(""), false).is_ok());
        let missing = check_dlc(Some("1"), false).unwrap_err();
        assert!(missing.contains("Importe e ative"), "{missing}");
        let extra = check_dlc(Some("0"), true).unwrap_err();
        assert!(extra.contains("Desative"), "{extra}");
        assert!(check_dlc(None, true).is_err());
        assert!(check_dlc(Some("yes"), true).is_err());
        assert!(check_dlc(Some("yes"), false).is_err());
    }

    #[test]
    fn course_pack_must_match_the_host_lobby() {
        let id = "a".repeat(64);
        assert!(check_pack(None, "").is_ok());
        assert!(check_pack(Some("0"), "").is_ok());
        assert!(check_pack(Some(&id), &id).is_ok());
        assert!(check_pack(Some(&id.to_uppercase()), &id).is_ok());
        assert!(check_pack(Some(&id), "")
            .unwrap_err()
            .contains("Importe e ative"));
        assert!(check_pack(Some("0"), &id).unwrap_err().contains("Desative"));
        assert!(check_pack(None, &id).is_err()); // an older launcher hosts without a pack
        assert!(check_pack(Some(&"b".repeat(64)), &id)
            .unwrap_err()
            .contains("outra versão"));
        assert!(check_pack(Some("sim"), "").is_err());
        assert!(check_pack(Some(&"z".repeat(64)), &id).is_err());
    }

    #[cfg(any(windows, target_os = "linux"))]
    #[test]
    fn incompatible_candidate_does_not_hide_a_compatible_library() {
        let root = std::env::temp_dir().join(format!("mk8-compatible-dll-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        let old = root.join(STEAM_LIBRARY);
        let good = root.join("steamapps/common/Game").join(STEAM_LIBRARY);
        fs::create_dir_all(good.parent().unwrap()).unwrap();
        // (an x86-64 ELF header: Linux only accepts 64-bit shared objects from Steam libraries)
        let mut library = b"ELF".to_vec();
        library.resize(16, 0);
        library.extend([3, 0, 62, 0]);
        fs::write(&old, &library).unwrap();
        fs::write(&good, &library).unwrap();
        let paths = dll_candidates(&root, || library_dll_paths(std::slice::from_ref(&root)));
        let mut visited = Vec::new();
        let chosen = compatible_api(paths, |path| {
            if path == old || path == good {
                visited.push(path.to_owned());
            }
            if path == good {
                Ok(Some(path.to_owned()))
            } else {
                Err("required export absent".into())
            }
        })
        .unwrap();
        assert_eq!(chosen, good);
        assert_eq!(visited, [old.clone(), good.clone()]);
        let paths = dll_candidates(&root, || {
            panic!("compatible local DLL should skip the scan")
        });
        assert_eq!(
            compatible_api(paths, |path| {
                if path == old {
                    Ok(Some(path.to_owned()))
                } else {
                    Err("unrelated DLL".into())
                }
            })
            .unwrap(),
            old
        );
        let error = compatible_api::<()>([old.clone()], |_| Err("required export absent".into()))
            .unwrap_err();
        assert!(error.contains(STEAM_LIBRARY) && error.contains("required export absent"));
        assert!(
            compatible_api::<()>([root.join("missing.dll")], |_| panic!("missing DLL loaded"))
                .is_err()
        );
        let offline = compatible_api::<()>([old.clone(), good.clone()], |path| {
            assert_eq!(path, old);
            Ok(None)
        })
        .unwrap_err();
        assert!(offline.contains("cliente Steam não está aberto"));
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn dll_search_reads_extra_libraries_and_common_nested_layouts() {
        let root = std::env::temp_dir().join(format!("mk8-dll-discovery-{}", std::process::id()));
        let steam = root.join("Steam custom");
        let library = root.join("Games drive");
        fs::create_dir_all(steam.join("steamapps/common")).unwrap();
        fs::create_dir_all(library.join("steamapps/common")).unwrap();
        // More than forty games: the old scan stopped before later entries.
        for i in 0..60 {
            fs::create_dir_all(steam.join(format!("steamapps/common/Game {i:03}"))).unwrap();
        }
        fn fixture(path: &Path) {
            #[cfg(target_os = "linux")]
            let bytes = b"\x7fELF\x02\x01\x01\0\0\0\0\0\0\0\0\0\x03\0\x3e\0";
            #[cfg(windows)]
            let bytes = b"authored path fixture, never loaded";
            fs::create_dir_all(path.parent().unwrap()).unwrap();
            fs::write(path, bytes).unwrap();
        }
        let direct = steam.join("steamapps/common/Game 059").join(STEAM_LIBRARY);
        #[cfg(windows)]
        let nested_file = "Binaries/Win64/steam_api64.dll";
        #[cfg(target_os = "linux")]
        let nested_file = "bin/linux64/libsteam_api.so";
        #[cfg(windows)]
        let unity_file = "Plugins/x86_64/steam_api64.dll";
        #[cfg(target_os = "linux")]
        let unity_file = "Plugins/x86_64/libsteam_api.so";
        #[cfg(windows)]
        let unreal_file = "Win64/steam_api64.dll";
        #[cfg(target_os = "linux")]
        let unreal_file = "x86_64-unknown-linux-gnu/libsteam_api.so";
        let nested = library.join("steamapps/common/Test").join(nested_file);
        let unity = library
            .join("steamapps/common/Unity/Test_Data")
            .join(unity_file);
        let unreal = library
            .join("steamapps/common/Unreal/Engine/Binaries/ThirdParty/Steamworks/Steamv161")
            .join(unreal_file);
        for path in [&direct, &nested, &unity, &unreal] {
            fixture(path);
        }
        #[cfg(target_os = "linux")]
        {
            let other = steam.join("steamapps/common/Game 000").join(STEAM_LIBRARY);
            fixture(&other);
            let mut header = fs::read(&other).unwrap();
            header[18] = 183; // AArch64
            fs::write(&other, header).unwrap();
        }
        let escaped = library.to_string_lossy().replace('\\', "\\\\");
        for vdf in [
            format!("\"libraryfolders\"\n{{\n\"1\"\n{{\n\"path\" \"{escaped}\"\n}}\n}}"),
            format!("\"1\" \"{escaped}\""),
        ] {
            fs::write(steam.join("steamapps/libraryfolders.vdf"), vdf).unwrap();
            assert_eq!(
                library_dll_paths(std::slice::from_ref(&steam)),
                [
                    direct.clone(),
                    nested.clone(),
                    unity.clone(),
                    unreal.clone()
                ]
            );
        }
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn all_friends_are_sorted_by_availability_then_name() {
        let mut friends: Vec<Friend> = (0..720)
            .map(|i| Friend {
                id: i.to_string(),
                name: format!("Friend {i:04}"),
                online: false,
                presence: "offline",
                room: Some("old-room".into()),
            })
            .collect();
        for (name, state) in [("Z online", 1), ("B away", 3), ("A busy", 2), ("A away", 4)] {
            let presence = friend_presence(state);
            friends.push(Friend {
                id: state.to_string(),
                name: name.into(),
                online: true,
                presence,
                room: None,
            });
        }
        friends.reverse();
        sort_friends(&mut friends);
        assert_eq!(friends.len(), 724);
        assert_eq!(
            friends[..4]
                .iter()
                .map(|f| f.name.as_str())
                .collect::<Vec<_>>(),
            ["A busy", "Z online", "A away", "B away"]
        );
        assert_eq!(friends[4].name, "Friend 0000");
        assert_eq!(friends.last().unwrap().name, "Friend 0719");
        for state in [0, -1, 7, 99] {
            assert_eq!(friend_presence(state), "offline");
        }
        for state in [1, 5, 6] {
            assert_eq!(friend_presence(state), "online");
        }
    }

    #[test]
    fn versioned_steam_accessor_is_called_and_legacy_is_only_a_fallback() {
        unsafe extern "C" fn current() -> *mut c_void {
            std::ptr::dangling_mut::<u8>().cast()
        }
        unsafe extern "C" fn legacy() -> *mut c_void {
            std::ptr::null_mut()
        }
        let mut looked_up = Vec::new();
        let accessor = resolve_interface(STEAM_USER, c"SteamUser", |name| {
            looked_up.push(name.to_owned());
            Ok(if name == c"SteamAPI_SteamUser_v023" {
                current
            } else {
                legacy
            })
        })
        .unwrap();
        assert!(!unsafe { accessor() }.is_null());
        assert_eq!(looked_up, [c"SteamAPI_SteamUser_v023".to_owned()]);
        // An older verified version (SDK 1.52-1.56) is used before the legacy export.
        let mut looked_up = Vec::new();
        let accessor = resolve_interface(STEAM_USER, c"SteamUser", |name| {
            looked_up.push(name.to_owned());
            if name == c"SteamAPI_SteamUser_v021" {
                Ok(current)
            } else {
                Err("absent".into())
            }
        })
        .unwrap();
        assert!(!unsafe { accessor() }.is_null());
        assert_eq!(
            looked_up.last().unwrap().as_c_str(),
            c"SteamAPI_SteamUser_v021"
        );
        // SDK 1.62+ DLLs (Counter-Strike 2) export SteamFriends v018 only.
        let accessor = resolve_interface(STEAM_FRIENDS, c"SteamFriends", |name| {
            if name == c"SteamAPI_SteamFriends_v018" {
                Ok(current)
            } else {
                Err("absent".into())
            }
        })
        .unwrap();
        assert!(!unsafe { accessor() }.is_null());
        let accessor = resolve_interface(STEAM_USER, c"SteamUser", |name| {
            if name == c"SteamUser" {
                Ok(legacy)
            } else {
                Err("absent".into())
            }
        })
        .unwrap();
        assert!(unsafe { accessor() }.is_null());
        // Unverified versions stay rejected instead of being guessed.
        let error = resolve_interface(STEAM_FRIENDS, c"SteamFriends", |name| {
            if name == c"SteamAPI_SteamFriends_v019" {
                Ok(current)
            } else {
                Err("absent".into())
            }
        })
        .unwrap_err();
        assert!(error.contains("SteamAPI_SteamFriends_v018") && error.contains("SteamFriends"));
        let error =
            resolve_interface(STEAM_USER, c"SteamUser", |_| Err("absent".into())).unwrap_err();
        assert!(error.contains("SteamAPI_SteamUser_v023") && error.contains("SteamUser"));
    }

    #[cfg(any(windows, target_os = "linux"))]
    #[test]
    #[ignore = "requires an installed native Steam library and a signed-in Steam client; creates a temporary friends-only lobby"]
    fn live_steam_dll_creates_and_leaves_lobby() {
        let dll = std::env::var("MK8_TEST_STEAM_DLL").expect("Set MK8_TEST_STEAM_DLL explicitly");
        std::env::set_var("MK8_STEAM_DLL", dll);
        let local = std::env::temp_dir().join(format!("mk8-steam-live-{}", std::process::id()));
        fs::create_dir_all(&local).unwrap();
        let api = ensure_api(&local).unwrap();
        assert!(probe_status(&local).ready);
        assert!(unsafe { (api.get_steam_id)((api.steam_user)()) } != 0);
        let _friends = friends(&local).unwrap();
        let (room, _host, _invite) =
            SteamRoom::host_live(&local, "loader-regression", "Teste", false, "").unwrap();
        assert_ne!(room.lobby_id, 0);
        let deadline = Instant::now() + Duration::from_secs(5);
        while !callbacks_delivered() && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(20));
        }
        let delivered = callbacks_delivered();
        drop(room); // Leave the test lobby and clear its rich presence even if the assertion fails.
        assert!(delivered, "Steam must deliver its native lobby callback");
        fs::remove_file(local.join("steam_appid.txt")).unwrap();
        fs::remove_dir(local).unwrap();
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn linux_steam_layouts_match_valve_sdk() {
        use std::mem::{offset_of, size_of};

        assert_eq!(size_of::<LobbyCreated>(), 12);
        assert_eq!(offset_of!(LobbyCreated, lobby_id), 4);
        assert_eq!(callback_size(LOBBY_ENTER_CALLBACK), 20);
        assert_eq!(offset_of!(LobbyEnter, response), 16);
        assert_eq!(size_of::<FriendGame>(), 24);
        assert_eq!(offset_of!(FriendGame, lobby), 16);
        assert_eq!(size_of::<Callback>(), 16);
        assert_eq!(offset_of!(Callback, id), 12);
        assert_eq!(offset_of!(CallbackVTable, run), 0);
    }

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
        fn send(&self, remote: u64, channel: i32, data: &[u8], reliable: bool) -> bool {
            // Match Steam's real datagram limit; the previous mock hid dropped large packets.
            if !reliable && data.len() > 1200 {
                return false;
            }
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
    fn steam_callbacks_become_join_and_invite_events() {
        let mut join = vec![0u8; 16];
        join[..8].copy_from_slice(&109775241012345678u64.to_le_bytes());
        assert_eq!(
            parse_callback(GAME_LOBBY_JOIN_REQUESTED, &join),
            Some(SteamEvent::Join {
                lobby: 109775241012345678
            })
        );
        assert_eq!(parse_callback(GAME_LOBBY_JOIN_REQUESTED, &[0u8; 16]), None);
        let mut presence = vec![0u8; 8 + 256];
        let connect = b"+connect_lobby 10977524101234";
        presence[8..8 + connect.len()].copy_from_slice(connect);
        assert_eq!(
            parse_callback(GAME_RICH_PRESENCE_JOIN_REQUESTED, &presence),
            Some(SteamEvent::Join {
                lobby: 10977524101234
            })
        );
        presence[8..8 + 6].copy_from_slice(b"+other");
        assert_eq!(
            parse_callback(GAME_RICH_PRESENCE_JOIN_REQUESTED, &presence),
            None
        );
        let mut invite = vec![0u8; 24];
        invite[..8].copy_from_slice(&76561198000000001u64.to_le_bytes());
        invite[8..16].copy_from_slice(&42u64.to_le_bytes());
        assert_eq!(
            parse_callback(LOBBY_INVITE, &invite),
            Some(SteamEvent::Invited {
                lobby: 42,
                friend: 76561198000000001
            })
        );
        // ABI of the structures Steam reads or fills.
        assert_eq!(std::mem::size_of::<FriendGame>(), 24);
        assert_eq!(std::mem::offset_of!(FriendGame, lobby), 16);
        assert_eq!(std::mem::offset_of!(Callback, flags), 8);
        assert_eq!(std::mem::offset_of!(Callback, id), 12);
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
    fn steam_handshake_rejects_different_game_identities() {
        for (host_id, guest_id, allowed) in [
            ("fixture-a", "fixture-a", true),
            ("fixture-b", "fixture-b", true),
            ("fixture-a", "fixture-b", false),
            ("fixture-b", "fixture-a", false),
            ("fixture-a", "fixture", false),
            ("fixture", "fixture-a", false),
        ] {
            let bus = MockSteamBus::new();
            let host_node = Arc::new(MockSteamNode {
                steam_id: 1,
                bus: bus.clone(),
            });
            let guest_node = Arc::new(MockSteamNode { steam_id: 2, bus });
            let (room, host, _) =
                SteamRoom::host_with_backend(host_node, 10, 1, host_id, "Host").unwrap();
            let result = SteamRoom::join_with_backend(
                guest_node,
                SteamCode::decode(&room.code).unwrap(),
                guest_id,
            );
            if allowed {
                let (_room, gateway) = result.unwrap();
                let deadline = Instant::now() + Duration::from_secs(3);
                while !gateway.connected() && Instant::now() < deadline {
                    thread::sleep(Duration::from_millis(10));
                }
                assert!(gateway.connected());
            } else {
                let error = result
                    .err()
                    .expect("incompatible peer must not get a gateway");
                assert_eq!(error, "Instalação incompatível com a sala Steam.");
                assert_eq!(host.connected(), 0);
            }
        }
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

        // Encryption adds 70 bytes: exercise both sides of Steam's 1200-byte
        // unreliable limit and the largest datagram supported by our transport.
        let mut recv_buf = [0u8; MAX_PACKET];
        for length in [1130, 1131, mk8_transport::MAX_PAYLOAD] {
            for (packet, sender, gateway, receiver) in [
                (&pkt_from_host, &g_host, &host_gateway, &g_guest),
                (&pkt_from_guest, &g_guest, &guest_gateway, &g_host),
            ] {
                let mut packet = packet.clone();
                packet.resize(length, 0x5a);
                sender.send_to(&packet, gateway.local_addr).unwrap();
                let (n, _) = receiver.recv_from(&mut recv_buf).unwrap_or_else(|error| {
                    panic!("Steam datagram of {length} bytes lost: {error}")
                });
                assert_eq!(&recv_buf[..n], packet.as_slice());
            }
        }
    }
}
