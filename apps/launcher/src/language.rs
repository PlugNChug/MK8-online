use mk8_manifest::{load, save, Result};
use serde_json::Value;
use std::path::Path;

pub fn valid(value: &str) -> bool {
    matches!(value, "en" | "pt-BR")
}

pub fn read(local: &Path) -> String {
    load::<String>(&local.join("language.json"))
        .ok()
        .filter(|value| valid(value))
        .unwrap_or_default()
}

pub fn write(local: &Path, body: &Value) -> Result<String> {
    let value = body["language"]
        .as_str()
        .filter(|value| valid(value))
        .ok_or("Unsupported language / Idioma não suportado")?;
    save(&local.join("language.json"), &value)?;
    Ok(value.into())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn persists_only_supported_languages_without_touching_game_config() {
        let root = std::env::temp_dir().join(format!("mk8-language-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        let config = root.join("config.json");
        std::fs::write(&config, b"preserve game configuration").unwrap();
        write(&root, &json!({"language": "en"})).unwrap();
        assert_eq!(read(&root), "en");
        assert!(write(&root, &json!({"language": "../other"})).is_err());
        assert!(write(&root, &json!({"language": 1})).is_err());
        assert_eq!(read(&root), "en");
        write(&root, &json!({"language": "pt-BR"})).unwrap();
        assert_eq!(read(&root), "pt-BR");
        assert_eq!(
            std::fs::read(&config).unwrap(),
            b"preserve game configuration"
        );
        std::fs::remove_file(root.join("language.json")).unwrap();
        assert_eq!(read(&root), "");
        std::fs::remove_file(config).unwrap();
        std::fs::remove_dir(root).unwrap();
    }
}
