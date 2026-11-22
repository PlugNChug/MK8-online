use mk8_manifest::{load, save, Result};
use serde_json::Value;
use std::path::Path;

pub fn valid(value: &str) -> bool {
    matches!(value, "es" | "en" | "pt-BR")
}

/// Picks the text for a native dialog; anything that is not Portuguese or
/// Spanish (including an unset language) uses English.
pub fn pick<T>(language: &str, pt: T, en: T, es: T) -> T {
    match language {
        "pt-BR" => pt,
        "es" => es,
        _ => en,
    }
}

pub fn read(local: &Path) -> String {
    load::<String>(&local.join("language.json"))
        .ok()
        .filter(|value| valid(value))
        .unwrap_or_else(|| "en".into())
}

/// The language the launcher pages show: the saved choice, otherwise the system
/// language with the same fallback as `web/i18n.js` (Portuguese, English, else Spanish).
pub fn effective(local: &Path) -> String {
    let saved = read(local);
    if !saved.is_empty() {
        return saved;
    }
    #[cfg(windows)]
    let primary = unsafe { windows_sys::Win32::Globalization::GetUserDefaultUILanguage() } & 0x3ff;
    #[cfg(not(windows))]
    let primary = 0x09;
    from_primary_language(primary).into()
}

fn from_primary_language(primary: u16) -> &'static str {
    match primary {
        0x16 => "pt-BR",
        0x09 => "en",
        _ => "es",
    }
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
        write(&root, &json!({"language": "es"})).unwrap();
        assert_eq!(read(&root), "es");
        write(&root, &json!({"language": "pt-BR"})).unwrap();
        assert_eq!(read(&root), "pt-BR");
        assert_eq!(
            std::fs::read(&config).unwrap(),
            b"preserve game configuration"
        );
        std::fs::remove_file(root.join("language.json")).unwrap();
        assert_eq!(read(&root), "en");
        std::fs::remove_file(config).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn effective_language_prefers_the_saved_choice() {
        assert_eq!(from_primary_language(0x16), "pt-BR");
        assert_eq!(from_primary_language(0x09), "en");
        assert_eq!(from_primary_language(0x0a), "es");
        assert_eq!(from_primary_language(0x07), "es");
        let root =
            std::env::temp_dir().join(format!("mk8-language-effective-{}", std::process::id()));
        std::fs::create_dir_all(&root).unwrap();
        assert!(valid(&effective(&root)));
        write(&root, &json!({"language": "es"})).unwrap();
        assert_eq!(effective(&root), "es");
        std::fs::remove_file(root.join("language.json")).unwrap();
        std::fs::remove_dir(root).unwrap();
    }

    #[test]
    fn picks_dialog_text_by_language() {
        assert_eq!(pick("pt-BR", "pt", "en", "es"), "pt");
        assert_eq!(pick("es", "pt", "en", "es"), "es");
        assert_eq!(pick("en", "pt", "en", "es"), "en");
        assert_eq!(pick("", "pt", "en", "es"), "en");
    }
}
