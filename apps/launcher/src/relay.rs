use mk8_manifest::Result;
use mk8_session::Invite;
use serde_json::{json, Value};
use std::{
    io::Write,
    process::{Command, Stdio},
};

pub struct InternetRoom {
    pub code: String,
    owner: String,
}

pub fn url() -> Option<String> {
    let base = std::env::var("MK8_RELAY_URL")
        .ok()
        .or_else(|| option_env!("MK8_RELAY_URL").map(str::to_owned))?;
    let base = base.trim_end_matches('/');
    (base.starts_with("https://")
        || base
            .strip_prefix("http://127.0.0.1:")
            .is_some_and(|port| port.parse::<u16>().is_ok()))
    .then(|| base.to_owned())
}

fn request(route: &str, body: Value) -> Result<Value> {
    let base = url().ok_or("Relay pela internet ainda não configurado nesta edição.")?;
    let mut cmd = Command::new("curl");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    let mut child = cmd
        .args([
            "--disable",
            "--silent",
            "--show-error",
            "--max-time",
            "5",
            "--max-filesize",
            "8192",
            "--header",
            "Content-Type: application/json",
            "--data-binary",
            "@-",
            "--url",
            &format!("{base}/v1/{route}"),
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).")?;
    let sent = child
        .stdin
        .take()
        .unwrap()
        .write_all(body.to_string().as_bytes());
    let output = child
        .wait_with_output()
        .map_err(|_| "Falha consultando o relay.")?;
    if sent.is_err() || !output.status.success() {
        return Err("Relay indisponível. Verifique a conexão.".into());
    }
    let value: Value =
        serde_json::from_slice(&output.stdout).map_err(|_| "Resposta inválida do relay.")?;
    if value["ok"] != true {
        return Err(value["error"]
            .as_str()
            .unwrap_or("Relay recusou a operação.")
            .to_owned());
    }
    Ok(value["data"].clone())
}

fn read_invite(value: &Value, identity: &str) -> Result<Invite> {
    Invite::decode(
        value["invite"]
            .as_str()
            .ok_or("Relay não entregou o convite.")?,
        identity,
    )
}

impl InternetRoom {
    pub fn create(identity: &str) -> Result<(Self, Invite)> {
        let value = request("rooms", json!({"identity":identity}))?;
        let room = Self {
            code: value["code"]
                .as_str()
                .ok_or("Relay não entregou o código.")?
                .into(),
            owner: value["owner"]
                .as_str()
                .ok_or("Relay não entregou a credencial.")?
                .into(),
        };
        let invite = read_invite(&value, identity)?;
        Ok((room, invite))
    }

    pub fn join(code: &str, identity: &str) -> Result<Invite> {
        read_invite(
            &request("join", json!({"code":code,"identity":identity}))?,
            identity,
        )
    }

    pub fn close(&self) -> Result<()> {
        request("close", json!({"code":self.code,"owner":self.owner})).map(|_| ())
    }
}
