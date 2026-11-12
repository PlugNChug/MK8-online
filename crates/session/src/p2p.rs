//! Single-code room tickets (`MK8P-…`) + STUN public endpoint discovery + UDP NAT hole punching.
//! Allows all guests to join a room using one reusable room code instead of per-player invitations,
//! automatically negotiating their dedicated `XChaCha20-Poly1305` slot (`Invite`) on the Host socket.

use crate::Invite;
use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use mk8_manifest::Result;
use mk8_transport::{random, MAX_PACKET};
use serde::{Deserialize, Serialize};
use std::{
    net::{IpAddr, Ipv4Addr, SocketAddr, ToSocketAddrs, UdpSocket},
    time::{Duration, Instant},
};

const STUN_MAGIC_COOKIE: u32 = 0x2112_A442;
const STUN_BINDING_REQUEST: u16 = 0x0001;
const STUN_BINDING_RESPONSE: u16 = 0x0101;
const ATTR_MAPPED_ADDRESS: u16 = 0x0001;
const ATTR_XOR_MAPPED_ADDRESS: u16 = 0x0020;

const DEFAULT_STUN_SERVERS: &[&str] = &[
    "stun.cloudflare.com:3478",
    "stun.l.google.com:19302",
    "stun1.l.google.com:19302",
];

#[derive(Clone, Serialize, Deserialize)]
pub struct RoomTicket {
    pub v: u8,
    pub endpoints: Vec<SocketAddr>,
    pub token: [u8; 16],
}

impl RoomTicket {
    pub fn new(endpoints: Vec<SocketAddr>, token: [u8; 16]) -> Result<Self> {
        let mut clean = Vec::new();
        for addr in endpoints {
            if addr.port() != 0
                && !addr.ip().is_unspecified()
                && !addr.ip().is_multicast()
                && !clean.contains(&addr)
            {
                clean.push(addr);
            }
        }
        if clean.is_empty() {
            return Err("Informe IP acessível e porta do anfitrião.".into());
        }
        Ok(Self {
            v: 1,
            endpoints: clean,
            token,
        })
    }

    pub fn encode(&self) -> String {
        format!(
            "MK8P-{}",
            URL_SAFE_NO_PAD.encode(serde_json::to_vec(self).expect("serializable ticket"))
        )
    }

    pub fn decode(text: &str) -> Result<Self> {
        if text.len() > 2048 {
            return Err("Código de sala muito longo.".into());
        }
        let stripped = text
            .trim()
            .strip_prefix("MK8P-")
            .ok_or("Código de sala inválido.")?;
        let bytes = URL_SAFE_NO_PAD
            .decode(stripped)
            .map_err(|_| "Código de sala inválido.")?;
        let ticket: Self =
            serde_json::from_slice(&bytes).map_err(|_| "Código de sala inválido.")?;
        if ticket.v != 1
            || ticket.endpoints.is_empty()
            || ticket.endpoints.len() > 8
            || ticket
                .endpoints
                .iter()
                .any(|e| e.port() == 0 || e.ip().is_unspecified() || e.ip().is_multicast())
        {
            return Err("Código de sala inválido ou incompatível.".into());
        }
        Ok(ticket)
    }
}

/// Builds a 20-byte RFC 5389 STUN Binding Request.
pub fn build_stun_binding_request(tx_id: &[u8; 12]) -> [u8; 20] {
    let mut pkt = [0u8; 20];
    pkt[0..2].copy_from_slice(&STUN_BINDING_REQUEST.to_be_bytes());
    pkt[2..4].copy_from_slice(&0u16.to_be_bytes());
    pkt[4..8].copy_from_slice(&STUN_MAGIC_COOKIE.to_be_bytes());
    pkt[8..20].copy_from_slice(tx_id);
    pkt
}

/// Parses an RFC 5389 STUN Binding Response and extracts `XOR-MAPPED-ADDRESS` (or `MAPPED-ADDRESS`).
pub fn parse_stun_binding_response(pkt: &[u8], expected_tx_id: &[u8; 12]) -> Option<SocketAddr> {
    if pkt.len() < 20 {
        return None;
    }
    let msg_type = u16::from_be_bytes([pkt[0], pkt[1]]);
    let msg_len = u16::from_be_bytes([pkt[2], pkt[3]]) as usize;
    let cookie = u32::from_be_bytes([pkt[4], pkt[5], pkt[6], pkt[7]]);
    if msg_type != STUN_BINDING_RESPONSE
        || cookie != STUN_MAGIC_COOKIE
        || &pkt[8..20] != expected_tx_id
        || pkt.len() < 20 + msg_len
    {
        return None;
    }
    let mut offset = 20;
    let end = 20 + msg_len;
    let mut fallback = None;
    while offset + 4 <= end {
        let attr_type = u16::from_be_bytes([pkt[offset], pkt[offset + 1]]);
        let attr_len = u16::from_be_bytes([pkt[offset + 2], pkt[offset + 3]]) as usize;
        offset += 4;
        if offset + attr_len > end {
            break;
        }
        let val = &pkt[offset..offset + attr_len];
        if attr_type == ATTR_XOR_MAPPED_ADDRESS && val.len() >= 8 && val[1] == 0x01 {
            let port = u16::from_be_bytes([val[2], val[3]]) ^ ((STUN_MAGIC_COOKIE >> 16) as u16);
            let ip_u32 = u32::from_be_bytes([val[4], val[5], val[6], val[7]]) ^ STUN_MAGIC_COOKIE;
            return Some(SocketAddr::new(IpAddr::V4(Ipv4Addr::from(ip_u32)), port));
        } else if attr_type == ATTR_MAPPED_ADDRESS && val.len() >= 8 && val[1] == 0x01 {
            let port = u16::from_be_bytes([val[2], val[3]]);
            let ip = Ipv4Addr::new(val[4], val[5], val[6], val[7]);
            fallback = Some(SocketAddr::new(IpAddr::V4(ip), port));
        }
        let padded = (attr_len + 3) & !3;
        offset += padded;
    }
    fallback
}

/// Queries a STUN server using the given UDP socket to discover its external NAT mapping
/// and keep the outbound UDP state open on the router.
pub fn stun_discover_on_socket(socket: &UdpSocket) -> Option<SocketAddr> {
    if std::env::var("MK8_DISABLE_STUN").is_ok_and(|v| v == "1") {
        return None;
    }
    let custom = std::env::var("MK8_STUN_SERVER").ok();
    let servers: Vec<&str> = match custom.as_deref().filter(|s| !s.is_empty()) {
        Some(s) => vec![s],
        None => DEFAULT_STUN_SERVERS.to_vec(),
    };
    let tx_id = random::<12>();
    let req = build_stun_binding_request(&tx_id);
    for server in servers {
        let Ok(mut addrs) = server.to_socket_addrs() else {
            continue;
        };
        let Some(target) = addrs.find(|a| a.is_ipv4()) else {
            continue;
        };
        if socket.send_to(&req, target).is_err() {
            continue;
        }
        let deadline = Instant::now() + Duration::from_millis(250);
        let mut buf = [0u8; 512];
        while Instant::now() < deadline {
            if let Ok((n, _from)) = socket.recv_from(&mut buf) {
                if let Some(mapped) = parse_stun_binding_response(&buf[..n], &tx_id) {
                    return Some(mapped);
                }
            }
        }
    }
    None
}

/// Sends simultaneous UDP join probes (`MK8-JOIN-1`) across all candidate endpoints in `ticket`
/// (performing outbound UDP hole punching) and returns the connected `Gateway` reusing the punched socket.
pub fn join_with_ticket(ticket: &RoomTicket, identity: &str) -> Result<crate::Gateway> {
    let socket = UdpSocket::bind("0.0.0.0:0").map_err(|e| e.to_string())?;
    socket
        .set_read_timeout(Some(Duration::from_millis(50)))
        .map_err(|e| e.to_string())?;
    let mut probe = b"MK8-JOIN-1".to_vec();
    probe.extend_from_slice(&ticket.token);
    probe.extend_from_slice(identity.as_bytes());

    let deadline = Instant::now() + Duration::from_secs(5);
    let mut last_send = Instant::now() - Duration::from_secs(1);
    let mut buf = [0u8; MAX_PACKET + 64];

    while Instant::now() < deadline {
        if last_send.elapsed() >= Duration::from_millis(250) {
            for endpoint in &ticket.endpoints {
                let _ = socket.send_to(&probe, endpoint);
            }
            last_send = Instant::now();
        }
        if let Ok((n, from)) = socket.recv_from(&mut buf) {
            if !ticket.endpoints.contains(&from) || n <= 9 {
                continue;
            }
            if let Some(body) = buf[..n].strip_prefix(b"MK8-INV-1") {
                let text =
                    std::str::from_utf8(body).map_err(|_| "Resposta de convite inválida.")?;
                let mut invite = Invite::decode(text, identity)?;
                invite.endpoint = from;
                return crate::Gateway::start_with_remote(invite, socket);
            } else if let Some(err_bytes) = buf[..n].strip_prefix(b"MK8-ERR-1") {
                let msg = std::str::from_utf8(err_bytes)
                    .unwrap_or("O anfitrião recusou a conexão na sala.");
                return Err(msg.to_owned());
            }
        }
    }
    Err("Sem resposta do anfitrião pelo código da sala. Confira a conexão ou tente via Steam / Relay.".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn stun_binding_request_and_xor_mapped_response_roundtrip() {
        let tx_id = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12];
        let req = build_stun_binding_request(&tx_id);
        assert_eq!(&req[0..2], &[0x00, 0x01]);
        assert_eq!(&req[8..20], &tx_id);

        // Construct a synthetic STUN Binding Response for 203.0.113.42:47820
        let ip = Ipv4Addr::new(203, 0, 113, 42);
        let port: u16 = 47820;
        let xor_port = port ^ ((STUN_MAGIC_COOKIE >> 16) as u16);
        let xor_ip = u32::from(ip) ^ STUN_MAGIC_COOKIE;

        let mut resp = Vec::new();
        resp.extend_from_slice(&STUN_BINDING_RESPONSE.to_be_bytes());
        resp.extend_from_slice(&12u16.to_be_bytes()); // 1 attribute (4 header + 8 value)
        resp.extend_from_slice(&STUN_MAGIC_COOKIE.to_be_bytes());
        resp.extend_from_slice(&tx_id);
        resp.extend_from_slice(&ATTR_XOR_MAPPED_ADDRESS.to_be_bytes());
        resp.extend_from_slice(&8u16.to_be_bytes());
        resp.push(0);
        resp.push(0x01); // IPv4
        resp.extend_from_slice(&xor_port.to_be_bytes());
        resp.extend_from_slice(&xor_ip.to_be_bytes());

        let parsed = parse_stun_binding_response(&resp, &tx_id).unwrap();
        assert_eq!(parsed, SocketAddr::new(IpAddr::V4(ip), port));
        assert!(parse_stun_binding_response(&resp, &[0u8; 12]).is_none());
    }

    #[test]
    fn room_ticket_roundtrip_and_validation() {
        let a: SocketAddr = "192.168.1.15:47820".parse().unwrap();
        let b: SocketAddr = "203.0.113.9:47820".parse().unwrap();
        let ticket = RoomTicket::new(vec![a, b, a], [9u8; 16]).unwrap();
        assert_eq!(ticket.endpoints.len(), 2);
        let encoded = ticket.encode();
        assert!(encoded.starts_with("MK8P-"));
        let decoded = RoomTicket::decode(&encoded).unwrap();
        assert_eq!(decoded.endpoints, vec![a, b]);
        assert_eq!(decoded.token, [9u8; 16]);
        assert!(RoomTicket::decode("MK8P-invalid").is_err());
    }

    #[test]
    fn multiple_guests_join_with_one_reusable_room_ticket_and_exchange_packets() {
        std::env::set_var("MK8_DISABLE_STUN", "1");
        let host = crate::Host::start_named(
            "127.0.0.1:0".parse().unwrap(),
            "fixture-p2p",
            "HostRoom",
            true,
        )
        .unwrap();
        let owner_gw = crate::Gateway::start(host.invite(host.address).unwrap()).unwrap();
        let ticket = host.room_ticket(Some(host.address)).unwrap();
        let code = ticket.encode();

        let parsed = RoomTicket::decode(&code).unwrap();
        let gw1 = join_with_ticket(&parsed, "fixture-p2p").unwrap();
        let gw2 = join_with_ticket(&parsed, "fixture-p2p").unwrap();

        let deadline = Instant::now() + Duration::from_secs(3);
        while !(owner_gw.connected() && gw1.connected() && gw2.connected())
            && Instant::now() < deadline
        {
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(owner_gw.connected());
        assert!(gw1.connected());
        assert!(gw2.connected());
        assert_eq!(host.connected(), 3);
        assert_eq!(gw1.slot, 1);
        assert_eq!(gw2.slot, 2);

        // Wrong identity is rejected immediately
        assert!(join_with_ticket(&parsed, "wrong-identity").is_err());
    }
}
