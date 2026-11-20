//! Shared interactive setup and handoff to the launcher.
use std::{
    io::Read,
    path::PathBuf,
    sync::{Arc, Mutex, OnceLock},
};

use serde_json::{json, Value};
use tauri::{WebviewUrl, WebviewWindowBuilder};
use tiny_http::{Method, Server};

#[cfg(windows)]
use crate::installer::uninstall;
use crate::{
    installer::{self, free_gib, hard_disk, install, Options, Previous, Progress, NEEDED_GIB},
    server::response,
    update, Error,
};

type Parent = Arc<OnceLock<tauri::WebviewWindow>>;

struct Setup {
    uninstall_app: Option<PathBuf>,
    previous: Previous,
    progress: Arc<Mutex<Progress>>,
    data: Option<PathBuf>,
    started: bool,
    language: String,
}

pub fn window(uninstall_app: Option<PathBuf>) -> Result<(), Error> {
    let removing = uninstall_app.is_some();
    let server = Server::http(("127.0.0.1", 0))?;
    let authority = server.server_addr().to_string();
    let url = format!("http://{authority}");
    let token = mk8_transport::random::<32>()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let parent: Parent = Arc::new(OnceLock::new());
    let previous = Previous::find();
    let setup = Setup {
        uninstall_app,
        language: crate::language::read(&installer::launcher_data(&previous.data)),
        previous,
        progress: Arc::new(Mutex::new(Progress::default())),
        data: None,
        started: false,
    };
    let launcher = Arc::new(OnceLock::<tauri::Url>::new());
    {
        let parent = parent.clone();
        let url = url.clone();
        let launcher = launcher.clone();
        std::thread::spawn(move || serve(setup, server, authority, url, token, parent, launcher));
    }
    let target: tauri::Url = url.parse()?;
    let origin = target.origin();
    tauri::Builder::default()
        .setup(move |tauri_app| {
            let window = WebviewWindowBuilder::new(tauri_app, "main", WebviewUrl::External(target))
                .title(if removing {
                    "MK8-online Uninstall"
                } else {
                    "MK8-online Setup"
                })
                .inner_size(880.0, 620.0)
                .resizable(false)
                .maximizable(false)
                .theme(Some(tauri::Theme::Dark))
                .center()
                .disable_drag_drop_handler()
                .on_navigation(move |url| {
                    let target = url.origin();
                    target == origin || launcher.get().is_some_and(|page| page.origin() == target)
                })
                .on_new_window(|_, _| tauri::webview::NewWindowResponse::Deny)
                .build()?;
            let _ = parent.set(window);
            Ok(())
        })
        .run(tauri::generate_context!())?;
    Ok(())
}

fn state(setup: &Setup) -> Value {
    let previous = &setup.previous;
    json!({
        "version": update::CURRENT,
        "app": previous.app,
        "data": previous.data,
        "desktop": previous.desktop,
        "update": previous.found,
        "free_gib": free_gib(&previous.data),
        "hard_disk": hard_disk(&previous.data),
        "needed_gib": NEEDED_GIB,
        "platform": std::env::consts::OS,
    })
}

fn handle(setup: &mut Setup, method: &Method, path: &str, body: &str) -> Result<Value, String> {
    let body: Value = serde_json::from_str(body).unwrap_or_default();
    if let Some(app) = &setup.uninstall_app {
        return match (method.as_str(), path) {
            ("GET", "/api/state") => Ok(state(setup)),
            ("POST", "/api/uninstall") => {
                let purge = body["purge"].as_bool().ok_or("Opção inválida")?;
                if purge && body["confirmed"] != true {
                    return Err("Confirme a exclusão dos dados.".into());
                }
                #[cfg(windows)]
                {
                    uninstall(app, purge).map_err(|e| e.to_string())?;
                    Ok(json!({}))
                }
                #[cfg(not(windows))]
                {
                    let _ = (app, purge);
                    Err("Rota indisponível".into())
                }
            }
            _ => Err("Rota indisponível".into()),
        };
    }
    match (method.as_str(), path) {
        ("GET", "/api/state") => Ok(state(setup)),
        ("POST", "/api/language") => {
            let language = body["language"]
                .as_str()
                .filter(|value| crate::language::valid(value))
                .ok_or("Unsupported language / Idioma não suportado")?;
            setup.language = language.into();
            Ok(json!(language))
        }
        ("POST", "/api/space") => {
            let path = PathBuf::from(body["path"].as_str().unwrap_or(""));
            let absolute = path.is_absolute();
            Ok(json!({
                "free_gib": absolute.then(|| free_gib(&path)).flatten(),
                "hard_disk": absolute.then(|| hard_disk(&path)).flatten(),
            }))
        }
        ("POST", "/api/install") => {
            if setup.started && setup.progress.lock().unwrap().state == "running" {
                return Err("A instalação já está em andamento.".into());
            }
            let folder = |key: &str| {
                body[key]
                    .as_str()
                    .map(|p| PathBuf::from(p.trim()))
                    .filter(|p| p.is_absolute())
                    .ok_or_else(|| {
                        if cfg!(windows) {
                            "Escolha pastas completas (por exemplo C:\\Jogos\\MK8-online)."
                                .to_string()
                        } else {
                            "Escolha pastas com caminhos absolutos.".to_string()
                        }
                    })
            };
            let options = Options {
                app: folder("app")?,
                data: folder("data")?,
                desktop: body["desktop"].as_bool().unwrap_or(true),
            };
            let language = body["language"].as_str().unwrap_or(&setup.language);
            if !crate::language::valid(language) {
                return Err("Unsupported language / Idioma não suportado".into());
            }
            #[cfg(windows)]
            crate::managed::create_data(&options.data).map_err(|e| e.to_string())?;
            crate::language::write(
                &installer::launcher_data(&options.data),
                &json!({"language": language}),
            )?;
            setup.language = language.into();
            *setup.progress.lock().unwrap() = Progress {
                state: "running".into(),
                ..Progress::default()
            };
            setup.started = true;
            setup.data = Some(options.data.clone());
            let progress = setup.progress.clone();
            std::thread::spawn(move || {
                let _ = install(&options, &progress);
            });
            Ok(json!({}))
        }
        ("GET", "/api/progress") => {
            let p = setup.progress.lock().unwrap().clone();
            Ok(json!({"percent": p.percent, "step": p.step, "state": p.state, "error": p.error}))
        }
        _ => Err("Rota não encontrada".into()),
    }
}

fn serve(
    mut setup: Setup,
    server: Server,
    authority: String,
    url: String,
    token: String,
    parent: Parent,
    launcher: Arc<OnceLock<tauri::Url>>,
) {
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
        let page = match (method.as_str(), path.as_str()) {
            ("GET", "/") => Some((
                (if setup.uninstall_app.is_some() {
                    include_str!("../web/uninstall.html")
                } else {
                    include_str!("../web/setup.html")
                })
                .replace("__TOKEN__", &token)
                .replace("__LANGUAGE__", &setup.language),
                "text/html; charset=utf-8",
            )),
            ("GET", "/i18n.js") => Some((
                include_str!("../web/i18n.js").into(),
                "text/javascript; charset=utf-8",
            )),
            ("GET", "/en.json") => Some((
                include_str!("../web/en.json").into(),
                "application/json; charset=utf-8",
            )),
            ("GET", "/es.json") => Some((
                include_str!("../web/es.json").into(),
                "application/json; charset=utf-8",
            )),
            ("GET", "/uninstall.js") => Some((
                include_str!("../web/uninstall.js").into(),
                "text/javascript; charset=utf-8",
            )),
            ("GET", "/setup.js") => Some((
                include_str!("../web/setup.js").to_string(),
                "text/javascript; charset=utf-8",
            )),
            ("GET", "/style.css") => Some((
                include_str!("../web/style.css").to_string(),
                "text/css; charset=utf-8",
            )),
            ("GET", "/setup.css") => Some((
                include_str!("../web/setup.css").to_string(),
                "text/css; charset=utf-8",
            )),
            _ => None,
        };
        if let Some((text, mime)) = page {
            response(req, 200, text, mime);
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
        let mut body = String::new();
        if req
            .as_reader()
            .take(8193)
            .read_to_string(&mut body)
            .is_err()
            || body.len() > 8192
        {
            response(req, 413, "{}".into(), "application/json");
            continue;
        }
        if setup.uninstall_app.is_none() && method == Method::Post && path == "/api/browse" {
            let value: Value = serde_json::from_str(&body).unwrap_or_default();
            let current = PathBuf::from(value["current"].as_str().unwrap_or(""));
            let title = value["title"]
                .as_str()
                .unwrap_or("Escolher pasta")
                .chars()
                .take(80)
                .collect::<String>();
            let parent = parent.clone();
            std::thread::spawn(move || {
                let mut dialog = rfd::FileDialog::new().set_title(title);
                if let Some(window) = parent.get() {
                    dialog = dialog.set_parent(window);
                }
                if let Some(dir) = current.ancestors().find(|p| p.is_dir()) {
                    dialog = dialog.set_directory(dir);
                }
                let picked = dialog.pick_folder().map(|p| p.display().to_string());
                response(
                    req,
                    200,
                    json!({"ok": true, "data": {"path": picked}}).to_string(),
                    "application/json; charset=utf-8",
                );
            });
            continue;
        }
        if setup.uninstall_app.is_some() && method == Method::Post && path == "/api/close" {
            response(req, 200, "{}".into(), "application/json");
            if let Some(window) = parent.get() {
                let _ = window.close();
            }
            break;
        }
        let finish =
            setup.uninstall_app.is_none() && method == Method::Post && path == "/api/finish";
        let result = if finish {
            finish_setup(&setup, &body, &parent, &launcher).map_err(|e| e.to_string())
        } else {
            handle(&mut setup, &method, &path, &body)
        };
        let (code, data, finished) = match result {
            Ok(value) => (200, json!({"ok": true, "data": value}), finish),
            Err(error) => (400, json!({"ok": false, "error": error}), false),
        };
        response(
            req,
            code,
            data.to_string(),
            "application/json; charset=utf-8",
        );
        if finished {
            break;
        }
    }
}

fn finish_setup(
    setup: &Setup,
    body: &str,
    parent: &Parent,
    launcher: &OnceLock<tauri::Url>,
) -> Result<Value, Error> {
    let body: Value = serde_json::from_str(body)?;
    let launch = body["launch"]
        .as_bool()
        .ok_or("Opção de abertura inválida")?;
    let window = parent.get().ok_or("Janela do MK8-online indisponível")?;
    if setup.progress.lock().unwrap().state != "done" {
        return Err("A instalação precisa terminar primeiro.".into());
    }
    if !launch {
        window.close()?;
        return Ok(json!({}));
    }
    let base = setup.data.as_deref().ok_or("Pasta de dados ausente")?;
    let target = if let Some(target) = launcher.get() {
        target.clone()
    } else {
        let target = crate::launcher_after_setup(base, parent.clone())?;
        launcher
            .set(target.clone())
            .map_err(|_| "O MK8-online já foi aberto")?;
        target
    };
    window.set_resizable(true)?;
    window.set_maximizable(true)?;
    window.set_min_size(Some(tauri::LogicalSize::new(400.0, 560.0)))?;
    window.set_size(tauri::LogicalSize::new(1280.0, 880.0))?;
    window.center()?;
    window.set_title("MK8-online")?;
    window.navigate(target)?;
    Ok(json!({}))
}
