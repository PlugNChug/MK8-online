//! Explicit, debug-only live text assets. The production server stays embedded.
use std::{fs, io, path::Path};

pub(crate) fn asset(
    root: &Path,
    url: &str,
    token: &str,
    language: &str,
) -> Option<io::Result<(String, &'static str)>> {
    let path = url.split('?').next()?;
    let (file, mime) = match path {
        "/" => ("index.html", "text/html; charset=utf-8"),
        "/style.css" | "/preview.css" | "/workspace.css" | "/installation.css"
        | "/mii-model.css" | "/fsr.css" => (&path[1..], "text/css; charset=utf-8"),
        "/app.js" | "/workspace.js" | "/installation.js" | "/storyboard.js" | "/preview.js"
        | "/i18n.js" | "/mii-composer.js" | "/mii-model.js" | "/mii-glb.js" | "/fsr.js" => {
            (&path[1..], "text/javascript; charset=utf-8")
        }
        "/en.json" | "/es.json" => (&path[1..], "application/json; charset=utf-8"),
        "/mii-model.html" => (&path[1..], "text/html; charset=utf-8"),
        _ => return None,
    };
    Some(fs::read_to_string(root.join(file)).map(|source| {
        let mut source = source
            .replace("__TOKEN__", token)
            .replace("__LANGUAGE__", language)
            .replace("from 'three'", "from '/vendor/three/three.module.js'");
        if path == "/" {
            source = source.replace("</head>", "<link rel=\"stylesheet\" href=\"/preview.css\"><script src=\"/preview.js\" defer></script></head>");
        }
        if path == "/" && url.split('?').nth(1).is_some_and(|q| q.split('&').any(|p| p == "storyboard=1")) {
            source = source.replace("<script src=\"/installation.js\" defer></script>", "<script src=\"/storyboard.js\" defer></script>");
        }
        (source, mime)
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reloads_only_known_assets_and_reports_missing_files() {
        let root = std::env::temp_dir().join(format!("mk8-live-web-{}", std::process::id()));
        fs::create_dir_all(&root).unwrap();
        fs::write(
            root.join("index.html"),
            "<head>__TOKEN__ __LANGUAGE__</head>",
        )
        .unwrap();
        let (html, mime) = asset(&root, "/", "test-token", "en").unwrap().unwrap();
        assert_eq!(mime, "text/html; charset=utf-8");
        assert!(html.contains("test-token en"));
        assert!(html.contains("/preview.js"));
        fs::write(root.join("style.css"), "first").unwrap();
        assert_eq!(
            asset(&root, "/style.css?reload=1", "", "")
                .unwrap()
                .unwrap()
                .0,
            "first"
        );
        fs::write(root.join("style.css"), "second").unwrap();
        assert_eq!(
            asset(&root, "/style.css", "", "").unwrap().unwrap().0,
            "second"
        );
        for path in [
            "/../config.json",
            "/%2e%2e/config.json",
            "/api/status",
            "/config.json",
            "/style.css/../config.json",
        ] {
            assert!(asset(&root, path, "", "").is_none());
        }
        assert!(asset(&root, "/preview.js", "", "").unwrap().is_err());
        fs::remove_file(root.join("index.html")).unwrap();
        fs::remove_file(root.join("style.css")).unwrap();
        fs::remove_dir(root).unwrap();
    }
}
