use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use mk8_manifest::Result;
use mk8_transport::{random, valid_datagram, Channel, MAX_PACKET};
use serde::{Deserialize, Serialize};
use std::{
    net::{SocketAddr, UdpSocket},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex,
    },
    thread::{self, JoinHandle},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

pub const DISCOVERY_PORT: u16 = 47820;
const PROBE: &[u8] = b"MK8-DISCOVER-1 ";

#[derive(Serialize)]
pub struct Room {
    name: String,
    endpoint: SocketAddr,
}

pub fn discover(identity: &str) -> Result<Vec<Room>> {
    let socket = UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
    socket.set_broadcast(true).map_err(|e| e.to_string())?;
    socket
        .set_read_timeout(Some(Duration::from_millis(100)))
        .map_err(|e| e.to_string())?;
    let nonce = random::<16>();
    let request = [PROBE, &nonce, identity.as_bytes()].concat();
    let broadcast = socket.send_to(&request, ("255.255.255.255", DISCOVERY_PORT));
    let loopback = socket.send_to(&request, ("127.0.0.1", DISCOVERY_PORT));
    broadcast.or(loopback).map_err(|e| e.to_string())?;
    let deadline = Instant::now() + Duration::from_millis(350);
    let mut rooms: Vec<Room> = Vec::new();
    let mut buf = [0; 128];
    while Instant::now() < deadline && rooms.len() < 32 {
        let Ok((n, endpoint)) = socket.recv_from(&mut buf) else {
            continue;
        };
        let Some(data) = buf[..n].strip_prefix(nonce.as_slice()) else {
            continue;
        };
        let Ok(name) = std::str::from_utf8(data) else {
            continue;
        };
        if endpoint.port() == DISCOVERY_PORT
            && !name.is_empty()
            && name.chars().count() <= 24
            && !name.chars().any(char::is_control)
            && !rooms.iter().any(|room| room.endpoint == endpoint)
        {
            rooms.push(Room {
                name: name.into(),
                endpoint,
            });
        }
    }
    Ok(rooms)
}

fn now() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}
#[derive(Clone, Serialize, Deserialize)]
pub struct Invite {
    pub version: u8,
    pub endpoint: SocketAddr,
    pub identity: String,
    pub session: [u8; 16],
    pub slot: u8,
    pub key: [u8; 32],
    pub expires: u64,
}
impl Invite {
    pub fn encode(&self) -> String {
        format!(
            "mk8://{}",
            URL_SAFE_NO_PAD.encode(serde_json::to_vec(self).expect("serializable invite"))
        )
    }
    pub fn decode(text: &str, identity: &str) -> Result<Self> {
        if text.len() > 4096 {
            return Err("Convite muito longo".into());
        }
        let bytes = URL_SAFE_NO_PAD
            .decode(
                text.trim()
                    .strip_prefix("mk8://")
                    .ok_or("Convite inválido")?,
            )
            .map_err(|_| "Convite inválido")?;
        let invite: Self = serde_json::from_slice(&bytes).map_err(|_| "Convite inválido")?;
        if invite.version != 2
            || invite.slot >= 12
            || invite.identity != identity
            || invite.expires <= now()
            || invite.endpoint.port() == 0
            || invite.endpoint.ip().is_unspecified()
            || invite.endpoint.ip().is_multicast()
        {
            return Err("Convite expirado, endereço inválido ou instalação incompatível.".into());
        }
        Ok(invite)
    }
}
struct Peer {
    invite: Invite,
    channel: Channel,
    endpoint: Option<SocketAddr>,
    seen: Instant,
    window: Instant,
    packets: u32,
}
pub struct Host {
    pub address: SocketAddr,
    session: [u8; 16],
    identity: String,
    peers: Arc<Mutex<Vec<Peer>>>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}
impl Host {
    pub fn start(bind: SocketAddr, identity: &str) -> Result<Self> {
        Self::start_named(bind, identity, "", false)
    }

    pub fn start_named(
        bind: SocketAddr,
        identity: &str,
        name: &str,
        advertise: bool,
    ) -> Result<Self> {
        if identity.is_empty() {
            return Err("Instalação sem identidade".into());
        }
        let socket = UdpSocket::bind(bind).map_err(|e| e.to_string())?;
        socket
            .set_read_timeout(Some(Duration::from_millis(50)))
            .map_err(|e| e.to_string())?;
        let address = socket.local_addr().map_err(|e| e.to_string())?;
        let peers: Arc<Mutex<Vec<Peer>>> = Arc::new(Mutex::new(Vec::new()));
        let stop = Arc::new(AtomicBool::new(false));
        let p = peers.clone();
        let s = stop.clone();
        let name = name.as_bytes().to_vec();
        let identity_bytes = identity.as_bytes().to_vec();
        let worker = thread::spawn(move || {
            let mut buf = [0; MAX_PACKET + 1];
            while !s.load(Ordering::Relaxed) {
                let Ok((n, from)) = socket.recv_from(&mut buf) else {
                    continue;
                };
                if let Some(query) = buf[..n].strip_prefix(PROBE).filter(|_| advertise) {
                    if query.len() == 16 + identity_bytes.len() && query[16..] == identity_bytes {
                        let _ = socket.send_to(&[&query[..16], name.as_slice()].concat(), from);
                    }
                    continue;
                }
                if !(70..=MAX_PACKET).contains(&n) {
                    continue;
                }
                let slot = buf[20] as usize;
                let mut peers = p.lock().unwrap();
                let Some(peer) = peers.get_mut(slot) else {
                    continue;
                };
                if peer.invite.expires <= now() || peer.endpoint.is_some_and(|addr| addr != from) {
                    continue;
                }
                if peer.window.elapsed() >= Duration::from_secs(1) {
                    peer.window = Instant::now();
                    peer.packets = 0;
                }
                if peer.packets >= 2000 {
                    continue;
                }
                peer.packets += 1;
                let Some(payload) = peer.channel.open(&buf[..n]) else {
                    continue;
                };
                if payload == b"HELLO" || payload == b"PING" {
                    peer.endpoint = Some(from);
                    peer.seen = Instant::now();
                    if let Some(reply) = peer.channel.seal(b"READY") {
                        let _ = socket.send_to(&reply, from);
                    }
                    continue;
                }
                if peer.endpoint.is_none() || !valid_datagram(&payload, slot as u8) {
                    continue;
                }
                peer.seen = Instant::now();
                let destination = payload[16];
                let broadcast = destination == 255;
                for (i, target) in peers.iter_mut().enumerate() {
                    if i == slot
                        || (!broadcast && destination as usize != i + 1)
                        || target.invite.expires <= now()
                        || target.seen.elapsed() > Duration::from_secs(15)
                    {
                        continue;
                    }
                    if let Some(to) = target.endpoint {
                        if let Some(wire) = target.channel.seal(&payload) {
                            let _ = socket.send_to(&wire, to);
                        }
                    }
                }
            }
        });
        Ok(Self {
            address,
            session: random(),
            identity: identity.into(),
            peers,
            stop,
            worker: Some(worker),
        })
    }
    pub fn invite_endpoint(&self) -> Option<SocketAddr> {
        if !self.address.ip().is_unspecified() {
            return Some(self.address);
        }
        let route = UdpSocket::bind("0.0.0.0:0").ok()?;
        route.connect("192.0.2.1:9").ok()?;
        let ip = route.local_addr().ok()?.ip();
        Some(SocketAddr::new(ip, self.address.port()))
    }

    pub fn invite(&self, advertised: SocketAddr) -> Result<Invite> {
        if advertised.port() == 0
            || advertised.ip().is_unspecified()
            || advertised.ip().is_multicast()
        {
            return Err("Informe IP acessível e porta do anfitrião.".into());
        }
        let mut peers = self.peers.lock().unwrap();
        if peers.len() >= 12 {
            return Err("Limite de 12 convites atingido. Crie nova sala para renovar.".into());
        }
        let invite = Invite {
            version: 2,
            endpoint: advertised,
            identity: self.identity.clone(),
            session: self.session,
            slot: peers.len() as u8,
            key: random(),
            expires: now() + 4 * 3600,
        };
        peers.push(Peer {
            channel: Channel::new(invite.key, invite.session, invite.slot, true),
            invite: invite.clone(),
            endpoint: None,
            seen: Instant::now(),
            window: Instant::now(),
            packets: 0,
        });
        Ok(invite)
    }
    pub fn connected(&self) -> usize {
        self.peers
            .lock()
            .unwrap()
            .iter()
            .filter(|p| {
                p.endpoint.is_some()
                    && p.seen.elapsed() < Duration::from_secs(15)
                    && p.invite.expires > now()
            })
            .count()
    }

    pub fn owner_connected(&self) -> bool {
        self.peers.lock().unwrap().first().is_some_and(|peer| {
            peer.endpoint.is_some()
                && peer.seen.elapsed() < Duration::from_secs(15)
                && peer.invite.expires > now()
        })
    }
}
impl Drop for Host {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

pub struct Gateway {
    pub local_addr: SocketAddr,
    pub slot: u8,
    ready: Arc<Mutex<Option<Instant>>>,
    game: Arc<Mutex<Option<SocketAddr>>>,
    stop: Arc<AtomicBool>,
    worker: Option<JoinHandle<()>>,
}
impl Gateway {
    pub fn start(invite: Invite) -> Result<Self> {
        let local = UdpSocket::bind("127.0.0.1:0").map_err(|e| e.to_string())?;
        let remote = UdpSocket::bind(if invite.endpoint.is_ipv4() {
            "0.0.0.0:0"
        } else {
            "[::]:0"
        })
        .map_err(|e| e.to_string())?;
        remote.connect(invite.endpoint).map_err(|e| e.to_string())?;
        local
            .set_read_timeout(Some(Duration::from_millis(50)))
            .map_err(|e| e.to_string())?;
        remote
            .set_read_timeout(Some(Duration::from_millis(50)))
            .map_err(|e| e.to_string())?;
        let local_rx = local.try_clone().map_err(|e| e.to_string())?;
        let remote_tx = remote.try_clone().map_err(|e| e.to_string())?;
        let local_addr = local.local_addr().map_err(|e| e.to_string())?;
        let slot = invite.slot;
        let stop = Arc::new(AtomicBool::new(false));
        let ready = Arc::new(Mutex::new(None));
        let game = Arc::new(Mutex::new(None));
        let g = game.clone();
        let s = stop.clone();
        let r = ready.clone();
        let worker = thread::spawn(move || {
            let channel = Arc::new(Mutex::new(Channel::new(
                invite.key,
                invite.session,
                slot,
                false,
            )));
            let tx_channel = channel.clone();
            let tx_stop = s.clone();
            let tx_game = g.clone();
            // Block on each socket independently: a fixed polling sleep adds latency to every hop.
            let sender = thread::spawn(move || {
                let mut buf = [0; MAX_PACKET + 1];
                while !tx_stop.load(Ordering::Relaxed) && invite.expires > now() {
                    let Ok((n, from)) = local_rx.recv_from(&mut buf) else {
                        continue;
                    };
                    let mut game = tx_game.lock().unwrap();
                    if !from.ip().is_loopback() || game.is_some_and(|addr| addr != from) {
                        continue;
                    }
                    if n == 15
                        && &buf[..4] == b"MMKJ"
                        && buf[4] == slot
                        && buf[5] == 9
                        && &buf[6..15] == b"MK8ONLINE"
                    {
                        *game = Some(from);
                        // This acknowledges only the local bridge, never a game lobby.
                        let _ = local_rx.send_to(b"MMKW\x00", from);
                    } else if *game == Some(from) && valid_datagram(&buf[..n], slot) {
                        if let Some(wire) = tx_channel.lock().unwrap().seal(&buf[..n]) {
                            let _ = remote_tx.send(&wire);
                        }
                    }
                }
            });
            let mut heartbeat = Instant::now() - Duration::from_secs(3);
            let mut buf = [0; MAX_PACKET + 1];
            while !s.load(Ordering::Relaxed) && invite.expires > now() {
                if heartbeat.elapsed() > Duration::from_secs(2) {
                    if let Some(wire) = channel.lock().unwrap().seal(b"HELLO") {
                        let _ = remote.send(&wire);
                    }
                    heartbeat = Instant::now();
                }
                if let Ok(n) = remote.recv(&mut buf) {
                    let payload = channel.lock().unwrap().open(&buf[..n]);
                    if let Some(payload) = payload {
                        if payload == b"READY" {
                            *r.lock().unwrap() = Some(Instant::now());
                        } else if payload.starts_with(b"MMKDVL1") {
                            if let Some(to) = *g.lock().unwrap() {
                                let _ = local.send_to(&payload, to);
                            }
                        }
                    }
                }
            }
            let _ = sender.join();
        });
        Ok(Self {
            local_addr,
            slot,
            ready,
            game,
            stop,
            worker: Some(worker),
        })
    }
    pub fn connected(&self) -> bool {
        self.ready
            .lock()
            .unwrap()
            .is_some_and(|t| t.elapsed() < Duration::from_secs(10))
    }
    pub fn reset_game(&self) {
        *self.game.lock().unwrap() = None;
    }
}
impl Drop for Gateway {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Relaxed);
        if let Some(worker) = self.worker.take() {
            let _ = worker.join();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn game(gateway: &Gateway) -> UdpSocket {
        let socket = UdpSocket::bind("127.0.0.1:0").unwrap();
        socket
            .set_read_timeout(Some(Duration::from_secs(2)))
            .unwrap();
        let join = [b"MMKJ".as_slice(), &[gateway.slot, 9], b"MK8ONLINE"].concat();
        socket.send_to(&join, gateway.local_addr).unwrap();
        let mut buf = [0; 64];
        let (n, _) = socket.recv_from(&mut buf).unwrap();
        assert_eq!(&buf[..n], b"MMKW\x00");
        socket
    }
    fn datagram(slot: u8, destination: u8) -> Vec<u8> {
        [
            b"MMKDVL1".as_slice(),
            &[10, 13, 37, slot + 1, 0, 1, 10, 13, 37, destination, 0, 2],
            b"fixture",
        ]
        .concat()
    }
    #[test]
    fn twelve_players_broadcast_unicast_and_latency() {
        let host = Host::start("127.0.0.1:0".parse().unwrap(), "fixture").unwrap();
        let gateways: Vec<_> = (0..12)
            .map(|_| Gateway::start(host.invite(host.address).unwrap()).unwrap())
            .collect();
        let deadline = Instant::now() + Duration::from_secs(3);
        while !gateways.iter().all(Gateway::connected) && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(host.connected(), 12);
        assert!(host.invite(host.address).is_err());
        let games: Vec<_> = gateways.iter().map(game).collect();
        let mut buf = [0; 256];
        for source in 0..12 {
            let packet = datagram(source as u8, 255);
            games[source]
                .send_to(&packet, gateways[source].local_addr)
                .unwrap();
            for (target, socket) in games.iter().enumerate() {
                if source == target {
                    continue;
                }
                let (n, _) = socket.recv_from(&mut buf).unwrap();
                assert_eq!(&buf[..n], packet.as_slice());
            }
        }
        let mut times = Vec::new();
        for _ in 0..100 {
            let start = Instant::now();
            for (source, target) in [(0, 11), (11, 0)] {
                let packet = datagram(source as u8, target as u8 + 1);
                games[source]
                    .send_to(&packet, gateways[source].local_addr)
                    .unwrap();
                let (n, _) = games[target].recv_from(&mut buf).unwrap();
                assert_eq!(&buf[..n], packet.as_slice());
            }
            times.push(start.elapsed().as_secs_f64() * 1000.0);
        }
        times.sort_by(f64::total_cmp);
        eprintln!(
            "12-player transport RTT: p50={:.3}ms p95={:.3}ms max={:.3}ms",
            times[49], times[94], times[99]
        );
        // A new runtime process gets a different UDP endpoint after closing/reopening the game.
        gateways[11].reset_game();
        let restarted = game(&gateways[11]);
        let packet = datagram(0, 12);
        games[0].send_to(&packet, gateways[0].local_addr).unwrap();
        let (n, _) = restarted.recv_from(&mut buf).unwrap();
        assert_eq!(&buf[..n], packet.as_slice());
    }
    #[test]
    fn delayed_lossy_network_keeps_authenticated_datagrams_intact() {
        let host = Host::start("127.0.0.1:0".parse().unwrap(), "fixture").unwrap();
        let proxy = UdpSocket::bind("127.0.0.1:0").unwrap();
        proxy.set_nonblocking(true).unwrap();
        let mut invite = host.invite(host.address).unwrap();
        invite.endpoint = proxy.local_addr().unwrap();
        let stop = Arc::new(AtomicBool::new(false));
        let s = stop.clone();
        let endpoint = host.address;
        let worker = thread::spawn(move || {
            let mut client = None;
            let mut queue = Vec::new();
            let mut buf = [0; MAX_PACKET + 1];
            let mut packets = 0;
            while !s.load(Ordering::Relaxed) {
                while let Ok((n, from)) = proxy.recv_from(&mut buf) {
                    let target = if from == endpoint {
                        client
                    } else {
                        client = Some(from);
                        Some(endpoint)
                    };
                    packets += 1;
                    // Deterministic 10% wire loss, 10-12 ms delay on each leg. No payload is modified.
                    if packets % 10 != 0 {
                        if let Some(target) = target {
                            queue.push((
                                Instant::now() + Duration::from_millis(10 + packets % 3),
                                buf[..n].to_vec(),
                                target,
                            ));
                        }
                    }
                }
                queue.retain(|(when, data, to)| {
                    if Instant::now() < *when {
                        true
                    } else {
                        proxy.send_to(data, to).unwrap();
                        false
                    }
                });
                thread::sleep(Duration::from_millis(1));
            }
        });
        // Always stop the proxy, including if an assertion fails.
        struct Proxy(Arc<AtomicBool>, Option<JoinHandle<()>>);
        impl Drop for Proxy {
            fn drop(&mut self) {
                self.0.store(true, Ordering::Relaxed);
                self.1.take().unwrap().join().unwrap();
            }
        }
        let _proxy = Proxy(stop, Some(worker));
        let a = Gateway::start(invite).unwrap();
        let b = Gateway::start(host.invite(host.address).unwrap()).unwrap();
        let deadline = Instant::now() + Duration::from_secs(5);
        while !(a.connected() && b.connected()) && Instant::now() < deadline {
            thread::sleep(Duration::from_millis(10));
        }
        assert!(a.connected() && b.connected());
        let x = game(&a);
        let y = game(&b);
        y.set_read_timeout(Some(Duration::from_millis(150)))
            .unwrap();
        let mut delivered = 0;
        let mut lost = 0;
        let mut times = Vec::new();
        let mut buf = [0; 256];
        for sequence in 0u8..40 {
            let mut packet = datagram(0, 2);
            packet.push(sequence);
            let start = Instant::now();
            x.send_to(&packet, a.local_addr).unwrap();
            match y.recv_from(&mut buf) {
                Ok((n, _)) => {
                    assert_eq!(&buf[..n], packet.as_slice());
                    delivered += 1;
                    times.push(start.elapsed().as_secs_f64() * 1000.0);
                }
                Err(error) => {
                    assert!(matches!(
                        error.kind(),
                        std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut
                    ));
                    lost += 1;
                }
            }
        }
        assert!(delivered >= 30 && lost > 0);
        times.sort_by(f64::total_cmp);
        eprintln!(
            "Simulated network: delivered={delivered}/40 lost={lost}, one-way p50={:.2}ms",
            times[times.len() / 2]
        );
    }
    #[test]
    fn two_gateways_forward_and_probe_does_not_take_slot() {
        let host = Host::start("127.0.0.1:0".parse().unwrap(), "fixture").unwrap();
        let i0 = host.invite(host.address).unwrap();
        let i1 = host.invite(host.address).unwrap();
        assert!(Invite::decode(&i1.encode(), "wrong").is_err());
        let a = Gateway::start(i0).unwrap();
        let b = Gateway::start(Invite::decode(&i1.encode(), "fixture").unwrap()).unwrap();
        let start = Instant::now();
        while !(a.connected() && b.connected()) && start.elapsed() < Duration::from_secs(3) {
            thread::sleep(Duration::from_millis(10));
        }
        assert!(a.connected() && b.connected());
        assert_eq!(host.connected(), 2);
        let x = UdpSocket::bind("127.0.0.1:0").unwrap();
        let y = UdpSocket::bind("127.0.0.1:0").unwrap();
        for (sock, gateway) in [(&x, &a), (&y, &b)] {
            sock.set_read_timeout(Some(Duration::from_secs(2))).unwrap();
            let mut join = b"MMKJ".to_vec();
            join.extend([gateway.slot, 9]);
            join.extend(b"MK8ONLINE");
            sock.send_to(&join, gateway.local_addr).unwrap();
            let mut buf = [0; 64];
            let (n, _) = sock.recv_from(&mut buf).unwrap();
            assert_eq!(&buf[..n], b"MMKW\x00");
        }
        let mut data = b"MMKDVL1".to_vec();
        data.extend([10, 13, 37, 1, 0, 1, 255, 255, 255, 255, 0, 2]);
        data.extend(b"race-fixture");
        x.send_to(&data, a.local_addr).unwrap();
        let mut buf = [0; 256];
        let (n, _) = y.recv_from(&mut buf).unwrap();
        assert_eq!(&buf[..n], &data);
        let intruder = UdpSocket::bind("127.0.0.1:0").unwrap();
        intruder
            .send_to(b"MMKJ\x00\x09MK8ONLINE", host.address)
            .unwrap();
        assert_eq!(host.connected(), 2);
    }
}
