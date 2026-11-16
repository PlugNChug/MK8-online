//! Standalone local guide: no game files, account, or running launcher server required.
use tauri::{webview::NewWindowResponse, WebviewUrl, WebviewWindowBuilder};
use tiny_http::{Header, Method, Request, Response, Server};

fn respond(request: Request, code: u16, body: String, mime: &str) {
    let mut response = Response::from_string(body).with_status_code(code);
    for (key,value) in [("Content-Type",mime),("Cache-Control","no-store"),("X-Content-Type-Options","nosniff"),
        ("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")] {
        response.add_header(Header::from_bytes(key,value).unwrap());
    }
    let _ = request.respond(response);
}

pub fn run() -> Result<(), crate::Error> {
    let server = Server::http(("127.0.0.1", 0))?;
    let authority = server.server_addr().to_string();
    let url = format!("http://{authority}");
    let close = format!(
        "/close/{}",
        mk8_transport::random::<16>()
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>()
    );
    let target: tauri::Url = url.parse()?;
    let origin = target.origin();
    tauri::Builder::default()
        .setup(move |app| {
            WebviewWindowBuilder::new(app, "manual", WebviewUrl::External(target))
                .title("MK8-online — How to Play")
                .inner_size(900.0, 780.0)
                .min_inner_size(390.0, 400.0)
                .center()
                .on_navigation(move |url| url.origin() == origin)
                .on_new_window(|_, _| NewWindowResponse::Deny)
                .build()?;
            let handle = app.handle().clone();
            std::thread::spawn(move || {
                for request in server.incoming_requests() {
                    let host = request
                        .headers()
                        .iter()
                        .find(|h| h.field.equiv("Host"))
                        .map(|h| h.value.as_str());
                    if host != Some(authority.as_str()) {
                        respond(request, 403, String::new(), "text/plain");
                        continue;
                    }
                    if request.method() == &Method::Post && request.url() == close {
                        if !request
                            .headers()
                            .iter()
                            .any(|h| h.field.equiv("Origin") && h.value.as_str() == url)
                        {
                            respond(request, 403, String::new(), "text/plain");
                            continue;
                        }
                        respond(request, 200, "Guide closed.".into(), "text/plain");
                        handle.exit(0);
                        break;
                    }
                    let (code, body, mime) = match (request.method(), request.url()) {
                        (&Method::Get, "/") => (
                            200,
                            include_str!("../web/how-to-play.html").replace("__CLOSE__", &close),
                            "text/html; charset=utf-8",
                        ),
                        (&Method::Get, "/style.css") => {
                            (200, include_str!("../web/style.css").into(), "text/css")
                        }
                        (&Method::Get, "/guide.css") => {
                            (200, include_str!("../web/guide.css").into(), "text/css")
                        }
                        (&Method::Get, "/guide.js") => (
                            200,
                            include_str!("../web/guide.js").into(),
                            "text/javascript",
                        ),
                        _ => (404, String::new(), "text/plain"),
                    };
                    respond(request, code, body, mime);
                }
            });
            Ok(())
        })
        .run(tauri::generate_context!())?;
    Ok(())
}
