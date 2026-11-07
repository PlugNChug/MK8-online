use chacha20poly1305::{
    aead::{Aead, Payload},
    KeyInit, XChaCha20Poly1305, XNonce,
};
use rand::{rngs::OsRng, RngCore};

pub const MAX_PAYLOAD: usize = 1419;
pub const MAX_PACKET: usize = MAX_PAYLOAD + 70;
pub fn random<const N: usize>() -> [u8; N] {
    let mut bytes = [0; N];
    OsRng.fill_bytes(&mut bytes);
    bytes
}

#[derive(Default)]
struct Replay {
    high: u64,
    bits: u64,
}
impl Replay {
    fn accept(&mut self, sequence: u64) -> bool {
        if sequence == 0 {
            return false;
        }
        if sequence > self.high {
            let delta = sequence - self.high;
            self.bits = if delta >= 64 {
                1
            } else {
                (self.bits << delta) | 1
            };
            self.high = sequence;
            return true;
        }
        let delta = self.high - sequence;
        if delta >= 64 || self.bits & (1 << delta) != 0 {
            return false;
        }
        self.bits |= 1 << delta;
        true
    }
}

// One channel per fresh invitation. Keys are never persisted or reused on reconnect.
pub struct Channel {
    cipher: XChaCha20Poly1305,
    session: [u8; 16],
    slot: u8,
    direction: u8,
    outgoing: u64,
    incoming: Replay,
}
impl Channel {
    pub fn new(key: [u8; 32], session: [u8; 16], slot: u8, server: bool) -> Self {
        Self {
            cipher: XChaCha20Poly1305::new((&key).into()),
            session,
            slot,
            direction: u8::from(server),
            outgoing: 0,
            incoming: Replay::default(),
        }
    }
    pub fn seal(&mut self, payload: &[u8]) -> Option<Vec<u8>> {
        if payload.len() > MAX_PAYLOAD {
            return None;
        }
        self.outgoing = self.outgoing.checked_add(1)?;
        let mut header = b"MK82".to_vec();
        header.extend(self.session);
        header.push(self.slot);
        header.push(self.direction);
        header.extend(self.outgoing.to_be_bytes());
        // Random extended nonce avoids nonce reuse even if a copied invite restarts a client.
        let nonce = random::<24>();
        header.extend(nonce);
        let encrypted = self
            .cipher
            .encrypt(
                XNonce::from_slice(&nonce),
                Payload {
                    msg: payload,
                    aad: &header,
                },
            )
            .ok()?;
        header.extend(encrypted);
        Some(header)
    }
    pub fn open(&mut self, packet: &[u8]) -> Option<Vec<u8>> {
        if packet.len() < 70
            || packet.len() > MAX_PACKET
            || &packet[..4] != b"MK82"
            || packet[4..20] != self.session
            || packet[20] != self.slot
            || packet[21] != (self.direction ^ 1)
        {
            return None;
        }
        let sequence = u64::from_be_bytes(packet[22..30].try_into().ok()?);
        let payload = self
            .cipher
            .decrypt(
                XNonce::from_slice(&packet[30..54]),
                Payload {
                    msg: &packet[54..],
                    aad: &packet[..54],
                },
            )
            .ok()?;
        if !self.incoming.accept(sequence) {
            return None;
        }
        Some(payload)
    }
}

pub fn valid_datagram(packet: &[u8], slot: u8) -> bool {
    slot < 12
        && packet.len() >= 19
        && packet.len() <= MAX_PAYLOAD
        && &packet[..7] == b"MMKDVL1"
        && packet[7..11] == [10, 13, 37, slot + 1]
        && (packet[13..17] == [255, 255, 255, 255]
            || (packet[13..16] == [10, 13, 37]
                && (packet[16] == 255 || (1..=12).contains(&packet[16]))))
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn authentication_replay_reordering_and_direction() {
        let key = random();
        let id = random();
        let mut a = Channel::new(key, id, 1, false);
        let mut b = Channel::new(key, id, 1, true);
        let first = a.seal(b"first").unwrap();
        let second = a.seal(b"second").unwrap();
        let mut bad = second.clone();
        bad[35] ^= 1;
        assert!(b.open(&bad).is_none());
        assert_eq!(b.open(&second).unwrap(), b"second");
        assert_eq!(b.open(&first).unwrap(), b"first");
        assert!(b.open(&first).is_none());
        assert!(a.open(&second).is_none());
        let reply = b.seal(b"reply").unwrap();
        assert_eq!(a.open(&reply).unwrap(), b"reply");
        assert!(Channel::new(random(), id, 1, true).open(&second).is_none());
    }
    #[test]
    fn rejects_spoofed_source() {
        let mut p = b"MMKDVL1".to_vec();
        p.extend([10, 13, 37, 1, 0, 1, 255, 255, 255, 255, 0, 2]);
        assert!(valid_datagram(&p, 0));
        assert!(!valid_datagram(&p, 1));
        assert!(!valid_datagram(&p, 255));
    }

    #[test]
    fn replay_window_handles_gaps_and_old_packets() {
        let mut replay = Replay::default();
        assert!(!replay.accept(0));
        assert!(replay.accept(100));
        assert!(replay.accept(98));
        assert!(!replay.accept(98));
        assert!(!replay.accept(36));
        assert!(replay.accept(200));
        assert!(!replay.accept(100));
    }

    #[test]
    fn malformed_and_wrong_session_packets_cannot_advance_window() {
        let key = random();
        let id = random();
        let mut a = Channel::new(key, id, 0, false);
        let mut b = Channel::new(key, id, 0, true);
        for length in 0..70 {
            assert!(b.open(&vec![0; length]).is_none());
        }
        let valid = a.seal(b"hello").unwrap();
        let mut forged = valid.clone();
        forged[22..30].copy_from_slice(&u64::MAX.to_be_bytes());
        assert!(b.open(&forged).is_none());
        let mut other = Channel::new(key, random(), 0, true);
        assert!(other.open(&valid).is_none());
        assert_eq!(b.open(&valid).unwrap(), b"hello");
        assert!(a.seal(&vec![0; MAX_PAYLOAD + 1]).is_none());
    }

    #[test]
    fn restarting_an_invite_does_not_reuse_crypto_nonce() {
        let key = random();
        let id = random();
        let first = Channel::new(key, id, 0, false).seal(b"one").unwrap();
        let second = Channel::new(key, id, 0, false).seal(b"two").unwrap();
        assert_ne!(&first[30..54], &second[30..54]);
    }
}
