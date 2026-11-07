use base64::{engine::general_purpose::URL_SAFE_NO_PAD, Engine};
use mk8_session::Host;
use mk8_transport::random;
use serde_json::{json, Value};
use std::{
    collections::HashMap,
    io::Read,
    net::{IpAddr, SocketAddr},
    time::{Duration, Instant},
};
use tiny_http::{Header, Method, Request, Response, Server};

const ROOM_LIMIT: u16 = 16;
const ROOM_TTL: Duration = Duration::from_secs(4 * 3600);
const IDLE_TTL: Duration = Duration::from_secs(60);

struct Room {
    host: Host,
    owner: String,
    identity: String,
    created: Instant,
    last_active: Instant,
}

struct Relay {
    rooms: HashMap<String, Room>,
    bind: IpAddr,
    public: IpAddr,
    first_port: u16,
}

impl Relay {
    fn expire(&mut self) {
        self.rooms.retain(|_, room| {
            if room.host.owner_connected() {
                room.last_active = Instant::now();
            }
            room.created.elapsed() < ROOM_TTL && room.last_active.elapsed() < IDLE_TTL
        });
    }

    fn handle(&mut self, req: &mut Request) -> Result<Value, &'static str> {
        if req.method() != &Method::Post || !req.body_length().is_some_and(|n| n <= 2048) {
            return Err("Requisição inválida.");
        }
        let mut body = Vec::new();
        req.as_reader()
            .take(2049)
            .read_to_end(&mut body)
            .map_err(|_| "Leitura falhou.")?;
        let body: Value = serde_json::from_slice(&body).map_err(|_| "JSON inválido.")?;
        match req.url() {
            "/v1/rooms" => {
                let identity = body["identity"]
                    .as_str()
                    .filter(|id| {
                        !id.is_empty()
                            && id.len() <= 320
                            && id
                                .bytes()
                                .all(|b| b.is_ascii_alphanumeric() || b":-".contains(&b))
                    })
                    .ok_or("Identidade inválida.")?;
                if self.rooms.len() >= ROOM_LIMIT as usize {
                    return Err("Relay cheio. Tente mais tarde.");
                }
                let host = (self.first_port..self.first_port + ROOM_LIMIT)
                    .find_map(|port| Host::start(SocketAddr::new(self.bind, port), identity).ok())
                    .ok_or("Relay sem porta UDP disponível.")?;
                let endpoint = SocketAddr::new(self.public, host.address.port());
                let invite = host.invite(endpoint).map_err(|_| "Convite indisponível.")?;
                let code = format!("MK8-{}", URL_SAFE_NO_PAD.encode(random::<16>()));
                let owner = URL_SAFE_NO_PAD.encode(random::<32>());
                self.rooms.insert(
                    code.clone(),
                    Room {
                        host,
                        owner: owner.clone(),
                        identity: identity.into(),
                        created: Instant::now(),
                        last_active: Instant::now(),
                    },
                );
                Ok(json!({"code":code,"owner":owner,"invite":invite.encode()}))
            }
            "/v1/join" => {
                let room = self
                    .rooms
                    .get(body["code"].as_str().unwrap_or(""))
                    .ok_or("Código inválido ou sala encerrada.")?;
                if body["identity"].as_str() != Some(room.identity.as_str()) {
                    return Err("Instalação incompatível com a sala.");
                }
                let invite = room
                    .host
                    .invite(SocketAddr::new(self.public, room.host.address.port()))
                    .map_err(|_| "Sala sem vagas. Peça ao anfitrião para criar outra.")?;
                Ok(json!({"invite":invite.encode()}))
            }
            "/v1/close" => {
                let code = body["code"].as_str().unwrap_or("");
                if !self
                    .rooms
                    .get(code)
                    .is_some_and(|room| body["owner"].as_str() == Some(room.owner.as_str()))
                {
                    return Err("Sala encerrada ou credencial inválida.");
                }
                self.rooms.remove(code);
                Ok(json!({}))
            }
            _ => Err("Rota inexistente."),
        }
    }
}

fn main() -> Result<(), Box<dyn std::error::Error + Send + Sync>> {
    let args: Vec<_> = std::env::args().collect();
    if args.len() != 5 {
        return Err("Uso: mk8-relay HTTP_LOOPBACK UDP_BIND_IP PUBLIC_IP PRIMEIRA_PORTA_UDP".into());
    }
    let http: SocketAddr = args[1].parse()?;
    let mut relay = Relay {
        rooms: HashMap::new(),
        bind: args[2].parse()?,
        public: args[3].parse()?,
        first_port: args[4].parse()?,
    };
    if !http.ip().is_loopback()
        || relay.first_port < 1024
        || relay.first_port > u16::MAX - ROOM_LIMIT
        || relay.bind.is_ipv4() != relay.public.is_ipv4()
        || relay.public.is_unspecified()
        || relay.public.is_multicast()
    {
        return Err(
            "Configuração inválida. Controle HTTP deve escutar no loopback, atrás de HTTPS.".into(),
        );
    }
    let server = Server::http(http)?;
    println!("MK8 relay {}", server.server_addr());
    loop {
        let request = server.recv_timeout(Duration::from_secs(1))?;
        relay.expire();
        if let Some(mut request) = request {
            let payload = match relay.handle(&mut request) {
                Ok(data) => json!({"ok":true,"data":data}),
                Err(error) => json!({"ok":false,"error":error}),
            };
            let _ = request.respond(
                Response::from_string(payload.to_string())
                    .with_header(Header::from_bytes("Content-Type", "application/json").unwrap())
                    .with_header(Header::from_bytes("Cache-Control", "no-store").unwrap()),
            );
        }
    }
}
