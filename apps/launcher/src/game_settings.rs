use mk8_manifest::Result;
use serde_json::{json, Map, Value};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
};

fn schema() -> Value {
    serde_json::from_str(include_str!("../game-settings.json")).expect("esquema de configurações")
}

fn path(local: &Path) -> Result<PathBuf> {
    let path = local.join("user-no-mii/mk8d.ini");
    for parent in path.ancestors() {
        match fs::symlink_metadata(parent) {
            Ok(meta) => {
                #[cfg(windows)]
                let linked = {
                    use std::os::windows::fs::MetadataExt;
                    meta.file_attributes() & 0x400 != 0
                };
                #[cfg(not(windows))]
                let linked = meta.file_type().is_symlink();
                if linked {
                    return Err("Configurações: caminho com link recusado.".into());
                }
            }
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => (),
            Err(error) => return Err(error.to_string()),
        }
    }
    Ok(path)
}

fn contents(path: &Path) -> Result<String> {
    match fs::metadata(path) {
        Ok(meta) if meta.len() > 1024 * 1024 => {
            return Err("Arquivo de configurações excede o limite.".into())
        }
        Ok(_) => (),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(String::new()),
        Err(error) => return Err(error.to_string()),
    }
    fs::read_to_string(path).map_err(|e| e.to_string())
}

fn entry(line: &str) -> Option<(&str, &str)> {
    let line = line.trim();
    if line.starts_with(['#', ';']) {
        return None;
    }
    line.split_once('=')
        .map(|(key, value)| (key.trim(), value.trim()))
}

fn fields(schema: &Value) -> impl Iterator<Item = &Value> {
    schema["groups"]
        .as_array()
        .into_iter()
        .flatten()
        .flat_map(|group| group["fields"].as_array().into_iter().flatten())
}

fn valid_camera_distance(value: &str) -> bool {
    value.bytes().all(|c| c.is_ascii_digit())
        && value.parse::<u8>().is_ok_and(|n| (75..=150).contains(&n))
}

fn rumble_percent(value: &str) -> Option<u8> {
    if !value.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    value.parse::<u8>().ok().filter(|percent| *percent <= 100)
}

fn valid_stick_deadzone(value: &str) -> bool {
    value.bytes().all(|c| c.is_ascii_digit()) && value.parse::<u8>().is_ok_and(|n| n <= 50)
}

fn valid(schema: &Value, field: &Value, value: &str) -> bool {
    match field["kind"].as_str() {
        Some("bool") => matches!(value, "0" | "1"),
        Some("camera_distance") => valid_camera_distance(value),
        Some("percentage") => rumble_percent(value).is_some(),
        Some("stick_deadzone") => valid_stick_deadzone(value),
        Some("volume") => value
            .parse::<f64>()
            .is_ok_and(|n| n.is_finite() && (0.0..=2.0).contains(&n)),
        Some("select") => field
            .get("options")
            .unwrap_or(&schema["choices"][field["choices"].as_str().unwrap_or("")])
            .as_array()
            .is_some_and(|options| options.iter().any(|option| option[0] == value)),
        _ => false,
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum MenuBuild {
    Legacy { has_layout: bool },
    ThreeThemes,
    FiveThemes,
}

impl From<&mk8_manifest::Installation> for MenuBuild {
    fn from(build: &mk8_manifest::Installation) -> Self {
        if !build.menu_themes_supported {
            Self::Legacy {
                has_layout: build.files.contains_key("settings-layout"),
            }
        } else if build.menu_light_themes_supported {
            Self::FiveThemes
        } else {
            Self::ThreeThemes
        }
    }
}

fn light_theme(theme: &str) -> bool {
    matches!(theme, "SimpleLight" | "NESLight")
}

pub fn read(local: &Path, build: MenuBuild) -> Result<Value> {
    let mut schema = schema();
    let mut values: Map<String, Value> = fields(&schema)
        .map(|field| {
            (
                field["key"].as_str().unwrap().into(),
                field["default"].clone(),
            )
        })
        .collect();
    let legacy_native = crate::debug_options::read(local)?.legacy_native;
    let default_theme = if build == MenuBuild::FiveThemes && legacy_native != Some(false) {
        "Native"
    } else {
        "Default"
    };
    values.insert("theme".into(), json!(default_theme));
    if std::env::var("MK8_SPLITSCREEN_60FPS").as_deref() == Ok("1") {
        values.insert("split_screen_60fps".into(), json!("1"));
    }
    let mut legacy_vibration = true;
    let mut rumble = None;
    for line in contents(&path(local)?)?.lines() {
        if let Some((key, value)) = entry(line) {
            if key == "controller_vibration" {
                legacy_vibration = value != "0";
            } else if key == "rumble_intensity" {
                rumble = rumble_percent(value);
            } else if let Some(slot) = values.get_mut(key) {
                let value = if key == "split_screen_60fps" && value != "1" {
                    "0"
                } else if key == "bloom" && value != "0" {
                    "1"
                } else {
                    value
                };
                *slot = Value::String(value.into());
            }
        }
    }
    values.insert(
        "rumble_intensity".into(),
        json!(rumble
            .unwrap_or(if legacy_vibration { 100 } else { 0 })
            .to_string()),
    );
    if legacy_native == Some(true) {
        values.insert("theme".into(), json!("Native"));
    } else if let Some(theme) = values["theme"].as_str() {
        let field = fields(&schema)
            .find(|field| field["key"] == "theme")
            .expect("theme field");
        if !valid(&schema, field, theme) {
            let fallback = if theme.is_empty() {
                default_theme
            } else {
                "Default"
            };
            values.insert("theme".into(), json!(fallback));
        }
    }
    if values["resolution"] == "67%" {
        values["resolution"] = json!("75%");
    }
    if !values["camera_distance"]
        .as_str()
        .is_some_and(valid_camera_distance)
    {
        values.insert("camera_distance".into(), json!("100"));
    }
    if !values["stick_deadzone"]
        .as_str()
        .is_some_and(valid_stick_deadzone)
    {
        values.insert("stick_deadzone".into(), json!("0"));
    }
    schema["values"] = Value::Object(values);
    Ok(schema)
}

pub fn write(local: &Path, patch: &Value, build: MenuBuild) -> Result<()> {
    let patch = patch.as_object().ok_or("Configurações inválidas.")?;
    let schema = schema();
    for (key, value) in patch {
        let field = fields(&schema)
            .find(|field| field["key"] == *key)
            .ok_or("Opção de configuração desconhecida.")?;
        if !value
            .as_str()
            .is_some_and(|value| valid(&schema, field, value))
        {
            return Err(format!("Valor inválido para {key}."));
        }
    }
    if patch.is_empty() {
        return Ok(());
    }
    if build != MenuBuild::FiveThemes
        && patch
            .get("theme")
            .and_then(Value::as_str)
            .is_some_and(light_theme)
    {
        return Err(UPDATE_FOR_LIGHT.into());
    }
    if let MenuBuild::Legacy { has_layout } = build {
        if let Some(theme) = patch.get("theme") {
            let native = theme == "Native";
            if native && !has_layout {
                return Err(UPDATE_FOR_NATIVE.into());
            }
            let mut prefs = crate::debug_options::read(local)?;
            let mut old_patch = patch.clone();
            if native {
                let current = ini_theme(local)?;
                if current == "Native" || light_theme(&current) {
                    old_patch.insert("theme".into(), json!("Default"));
                } else {
                    old_patch.remove("theme");
                }
            }
            if !old_patch.is_empty() {
                write_ini(local, &old_patch)?;
            }
            if native != prefs.legacy_native.unwrap_or(false) {
                prefs.legacy_native = Some(native);
                crate::debug_options::write(local, &prefs)?;
            }
            return Ok(());
        }
        return write_ini(local, patch);
    }
    migrate_theme(local)?;
    write_ini(local, patch)
}

// Write the former choice first so an interrupted migration can safely repeat.
fn migrate_theme(local: &Path) -> Result<()> {
    let mut options = crate::debug_options::read(local)?;
    if let Some(enabled) = options.legacy_native.take() {
        if enabled || ini_theme(local)?.is_empty() {
            let theme = if enabled { "Native" } else { "Default" };
            write_ini(local, json!({"theme":theme}).as_object().unwrap())?;
        }
        crate::debug_options::write(local, &options)?;
    }
    Ok(())
}

const UPDATE_FOR_NATIVE: &str = "Atualize a build do jogo para usar o tema Nativo do menu.";
const UPDATE_FOR_LIGHT: &str = "Atualize a build do jogo para usar os temas claros do menu.";

fn ini_theme(local: &Path) -> Result<String> {
    Ok(contents(&path(local)?)?
        .lines()
        .rev()
        .filter_map(entry)
        .find(|(k, _)| *k == "theme")
        .map_or("", |(_, value)| value)
        .to_owned())
}

pub fn configure_menu(local: &Path, build: MenuBuild) -> Result<bool> {
    std::env::remove_var("MK8_SETTINGS_EXPERIMENT");
    if build != MenuBuild::FiveThemes {
        let theme = ini_theme(local)?;
        if light_theme(&theme) {
            return Err(UPDATE_FOR_LIGHT.into());
        }
        if let MenuBuild::Legacy { has_layout } = build {
            let legacy = crate::debug_options::read(local)?.legacy_native == Some(true);
            if theme == "Native" || (legacy && !has_layout) {
                return Err(UPDATE_FOR_NATIVE.into());
            }
            if legacy {
                std::env::set_var("MK8_SETTINGS_EXPERIMENT", "1");
            }
            return Ok(legacy);
        }
    }
    migrate_theme(local)?;
    Ok(true)
}

fn write_ini(local: &Path, patch: &Map<String, Value>) -> Result<()> {
    // A runtime with the old toggle can still honor a muted slider.
    let compatible = patch.get("rumble_intensity").map(|intensity| {
        let mut patch = patch.clone();
        let muted = intensity.as_str().and_then(rumble_percent) == Some(0);
        patch.insert(
            "controller_vibration".into(),
            json!(if muted { "0" } else { "1" }),
        );
        patch
    });
    let patch = compatible.as_ref().unwrap_or(patch);
    let path = path(local)?;
    let original = contents(&path)?;
    let newline = if original.contains("\r\n") {
        "\r\n"
    } else {
        "\n"
    };
    let mut pending = patch.clone();
    let mut output = String::new();
    for line in original.split_inclusive('\n') {
        if let Some((key, _)) = entry(line) {
            if let Some(value) = patch.get(key) {
                output.push_str(&format!("{key} = {}{newline}", value.as_str().unwrap()));
                pending.remove(key);
                continue;
            }
        }
        output.push_str(line);
    }
    if !pending.is_empty() && !output.is_empty() && !output.ends_with('\n') {
        output.push_str(newline);
    }
    for (key, value) in pending {
        output.push_str(&format!("{key} = {}{newline}", value.as_str().unwrap()));
    }
    fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let temp = path.with_extension(format!("ini.{}.{stamp}.tmp", std::process::id()));
    let mut file = fs::OpenOptions::new()
        .create_new(true)
        .write(true)
        .open(&temp)
        .map_err(|e| e.to_string())?;
    file.write_all(output.as_bytes())
        .and_then(|_| file.sync_all())
        .map_err(|e| e.to_string())?;
    drop(file);
    fs::rename(temp, path).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn write(local: &Path, patch: &Value) -> Result<()> {
        super::write(local, patch, MenuBuild::FiveThemes)
    }

    fn read(local: &Path) -> Result<Value> {
        super::read(local, MenuBuild::FiveThemes)
    }

    #[test]
    fn missing_theme_and_downgrades_match_the_prepared_game() {
        let local = std::env::temp_dir().join(format!("mk8-light-themes-{}", std::process::id()));
        fs::create_dir_all(local.join("user-no-mii")).unwrap();
        let ini = local.join("user-no-mii/mk8d.ini");
        let older = [
            MenuBuild::ThreeThemes,
            MenuBuild::Legacy { has_layout: true },
        ];
        assert_eq!(read(&local).unwrap()["values"]["theme"], "Native");
        for build in older {
            assert_eq!(
                super::read(&local, build).unwrap()["values"]["theme"],
                "Default"
            );
        }
        assert!(!ini.exists());
        for (stored, current) in [
            ("", "Native"),
            ("Default", "Default"),
            ("NES", "NES"),
            ("Native", "Native"),
            ("unknown", "Default"),
        ] {
            let original = format!("volume = 1.50\ntheme = {stored}\n");
            fs::write(&ini, &original).unwrap();
            assert_eq!(read(&local).unwrap()["values"]["theme"], current);
            assert_eq!(fs::read_to_string(&ini).unwrap(), original);
        }
        for stored in ["SimpleLight", "NESLight"] {
            write(&local, &json!({"theme":stored})).unwrap();
            assert_eq!(read(&local).unwrap()["values"]["theme"], stored);
            let saved = fs::read(&ini).unwrap();
            for build in older {
                assert_eq!(
                    super::write(&local, &json!({"theme":stored}), build).unwrap_err(),
                    UPDATE_FOR_LIGHT
                );
                assert_eq!(configure_menu(&local, build).unwrap_err(), UPDATE_FOR_LIGHT);
                assert_eq!(fs::read(&ini).unwrap(), saved);
            }
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn legacy_opt_out_with_no_theme_keeps_simple_dark_when_migrated() {
        let local = std::env::temp_dir().join(format!("mk8-theme-opt-out-{}", std::process::id()));
        fs::create_dir_all(local.join("user-no-mii")).unwrap();
        let ini = local.join("user-no-mii/mk8d.ini");
        for launch in [false, true] {
            let original = if launch {
                "theme = \nvolume = 1.50\n"
            } else {
                "volume = 1.50\n"
            };
            fs::write(&ini, original).unwrap();
            fs::write(
                local.join("debug-options.json"),
                r#"{"graphics_capture":true,"audio_recording":false,"native_settings":false}"#,
            )
            .unwrap();
            assert_eq!(read(&local).unwrap()["values"]["theme"], "Default");
            assert_eq!(fs::read_to_string(&ini).unwrap(), original);
            if launch {
                assert!(configure_menu(&local, MenuBuild::FiveThemes).unwrap());
            } else {
                write(&local, &json!({"show_fps":"1"})).unwrap();
            }
            let saved = fs::read_to_string(&ini).unwrap();
            assert!(saved.contains("theme = Default") && saved.contains("volume = 1.50"));
            assert!(crate::debug_options::read(&local)
                .unwrap()
                .legacy_native
                .is_none());
            assert_eq!(read(&local).unwrap()["values"]["theme"], "Default");
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn old_host_saves_keep_the_legacy_theme_playable() {
        let local = std::env::temp_dir().join(format!("mk8-old-menu-{}", std::process::id()));
        let user = local.join("user-no-mii");
        fs::create_dir_all(&user).unwrap();
        let ini = user.join("mk8d.ini");
        fs::write(&ini, "theme = NES\n").unwrap();
        let debug = local.join("debug-options.json");
        let old = r#"{"graphics_capture":true,"audio_recording":false,"native_settings":true}"#;
        fs::write(&debug, old).unwrap();
        for patch in [
            json!({"volume":"1.5"}),
            json!({"language":"fr"}),
            json!({"volume":"1.5","theme":"Native"}),
        ] {
            super::write(&local, &patch, MenuBuild::Legacy { has_layout: true }).unwrap();
            assert!(fs::read_to_string(&ini).unwrap().contains("theme = NES"));
            assert_eq!(fs::read_to_string(&debug).unwrap(), old);
            assert_eq!(read(&local).unwrap()["values"]["theme"], "Native");
        }
        super::write(
            &local,
            &json!({"theme":"Default"}),
            MenuBuild::Legacy { has_layout: true },
        )
        .unwrap();
        assert_eq!(read(&local).unwrap()["values"]["theme"], "Default");
        assert!(super::write(
            &local,
            &json!({"theme":"Native"}),
            MenuBuild::Legacy { has_layout: false }
        )
        .is_err());
        assert_eq!(read(&local).unwrap()["values"]["theme"], "Default");
        super::write(
            &local,
            &json!({"theme":"Native"}),
            MenuBuild::Legacy { has_layout: true },
        )
        .unwrap();
        assert!(fs::read_to_string(&ini)
            .unwrap()
            .contains("theme = Default"));
        let prefs = crate::debug_options::read(&local).unwrap();
        assert_eq!(prefs.legacy_native, Some(true));
        assert!(prefs.graphics_capture);
        fs::remove_dir_all(&local).unwrap();
    }

    #[test]
    fn native_theme_migrates_once_and_an_interrupted_write_can_be_retried() {
        let local =
            std::env::temp_dir().join(format!("mk8-theme-migration-{}", std::process::id()));
        let user = local.join("user-no-mii");
        fs::create_dir_all(user.join("save")).unwrap();
        let ini = user.join("mk8d.ini");
        let debug = local.join("debug-options.json");
        let progress = user.join("save/marker");
        fs::write(&progress, b"race").unwrap();
        fs::write(&ini, "# keep\r\ntheme = NES\r\nvolume = 1.50\r\n").unwrap();
        let original = fs::read(&ini).unwrap();
        for flag in [None, Some(false), Some(true)] {
            let mut prefs = json!({"graphics_capture":true,"audio_recording":false});
            if let Some(flag) = flag {
                prefs["native_settings"] = json!(flag);
            }
            fs::write(&debug, prefs.to_string()).unwrap();
            let expected = if flag == Some(true) { "Native" } else { "NES" };
            assert_eq!(read(&local).unwrap()["values"]["theme"], expected);
            assert_eq!(fs::read(&ini).unwrap(), original);
        }
        let blocked = local.join("debug-options.json.new");
        fs::create_dir(&blocked).unwrap();
        assert!(write(&local, &json!({"theme":"Default"})).is_err());
        assert_eq!(read(&local).unwrap()["values"]["theme"], "Native");
        assert!(crate::debug_options::read(&local)
            .unwrap()
            .legacy_native
            .unwrap());
        fs::remove_dir(blocked).unwrap();
        write(&local, &json!({"theme":"Default"})).unwrap();
        assert_eq!(
            fs::read_to_string(&ini).unwrap(),
            "# keep\r\ntheme = Default\r\nvolume = 1.50\r\n"
        );
        let prefs: Value = serde_json::from_slice(&fs::read(&debug).unwrap()).unwrap();
        assert_eq!(
            prefs,
            json!({"graphics_capture":true,"audio_recording":false})
        );
        fs::write(&ini, "theme = NES\n").unwrap();
        migrate_theme(&local).unwrap();
        assert_eq!(read(&local).unwrap()["values"]["theme"], "NES");
        assert_eq!(fs::read(progress).unwrap(), b"race");
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn camera_distance_accepts_custom_percentages_and_preserves_invalid_ini_on_read() {
        let local =
            std::env::temp_dir().join(format!("mk8-camera-distance-{}", std::process::id()));
        let ini = local.join("user-no-mii/mk8d.ini");
        let key = "camera_distance";
        assert_eq!(read(&local).unwrap()["values"][key], "100");
        for value in ["75", "117", "150"] {
            write(&local, &json!({"camera_distance": value})).unwrap();
            assert_eq!(read(&local).unwrap()["values"][key], value);
        }
        for value in ["74", "151", "117.5", "", "NaN", "100\nvolume = 1"] {
            assert!(write(&local, &json!({"camera_distance": value})).is_err());
            assert_eq!(fs::read_to_string(&ini).unwrap(), "camera_distance = 150\n");
        }
        let invalid = "camera_distance = 999\nvolume = 0.75\n";
        fs::write(&ini, invalid).unwrap();
        let values = read(&local).unwrap();
        assert_eq!(values["values"][key], "100");
        assert_eq!(values["values"]["volume"], "0.75");
        assert_eq!(fs::read_to_string(&ini).unwrap(), invalid);
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn stick_deadzone_accepts_whole_percentages_and_defaults_to_zero() {
        let local = std::env::temp_dir().join(format!("mk8-stick-deadzone-{}", std::process::id()));
        let ini = local.join("user-no-mii/mk8d.ini");
        let key = "stick_deadzone";
        assert_eq!(read(&local).unwrap()["values"][key], "0");
        for value in ["0", "17", "50"] {
            write(&local, &json!({"stick_deadzone": value})).unwrap();
            assert_eq!(read(&local).unwrap()["values"][key], value);
        }
        for value in ["-1", "51", "17.5", "+3", ""] {
            assert!(write(&local, &json!({"stick_deadzone": value})).is_err());
            assert_eq!(read(&local).unwrap()["values"][key], "50");
        }
        let invalid = "stick_deadzone = 999\n";
        fs::write(&ini, invalid).unwrap();
        assert_eq!(read(&local).unwrap()["values"][key], "0");
        assert_eq!(fs::read_to_string(&ini).unwrap(), invalid);
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn split_screen_rate_uses_an_explicit_ini_value_before_the_environment_default() {
        let local = std::env::temp_dir().join(format!("mk8-split-rate-{}", std::process::id()));
        let ini = local.join("user-no-mii/mk8d.ini");
        let env_default = if std::env::var("MK8_SPLITSCREEN_60FPS").as_deref() == Ok("1") {
            "1"
        } else {
            "0"
        };
        assert_eq!(
            read(&local).unwrap()["values"]["split_screen_60fps"],
            env_default
        );
        fs::create_dir_all(ini.parent().unwrap()).unwrap();
        let original = "other_setting = keep\nsplit_screen_60fps = invalid\n";
        fs::write(&ini, original).unwrap();
        assert_eq!(read(&local).unwrap()["values"]["split_screen_60fps"], "0");
        assert!(write(&local, &json!({"split_screen_60fps":"2"})).is_err());
        assert_eq!(fs::read_to_string(&ini).unwrap(), original);
        for value in ["1", "0"] {
            write(&local, &json!({"split_screen_60fps":value})).unwrap();
            assert_eq!(read(&local).unwrap()["values"]["split_screen_60fps"], value);
            assert!(fs::read_to_string(&ini)
                .unwrap()
                .contains("other_setting = keep"));
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn rumble_migrates_the_toggle_and_persists_a_valid_percentage() {
        let local = std::env::temp_dir().join(format!("mk8-rumble-{}", std::process::id()));
        let ini = local.join("user-no-mii/mk8d.ini");
        assert_eq!(read(&local).unwrap()["values"]["rumble_intensity"], "100");
        fs::create_dir_all(ini.parent().unwrap()).unwrap();
        fs::write(&ini, "controller_vibration = 1\n").unwrap();
        assert_eq!(read(&local).unwrap()["values"]["rumble_intensity"], "100");
        fs::write(
            &ini,
            "rumble_intensity = 101\ncontroller_vibration = 0\nother = keep\n",
        )
        .unwrap();
        assert_eq!(read(&local).unwrap()["values"]["rumble_intensity"], "0");
        for value in ["101", "-1", "+35", "35.5"] {
            assert!(write(&local, &json!({"rumble_intensity":value})).is_err());
        }
        for (value, legacy) in [("35", "1"), ("0", "0")] {
            write(&local, &json!({"rumble_intensity":value})).unwrap();
            assert_eq!(read(&local).unwrap()["values"]["rumble_intensity"], value);
            let text = fs::read_to_string(&ini).unwrap();
            assert!(text.contains(&format!("controller_vibration = {legacy}")));
            assert!(text.contains("other = keep"));
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn bloom_defaults_on_and_preserves_unrelated_ini_entries() {
        let local = std::env::temp_dir().join(format!("mk8-bloom-{}", std::process::id()));
        let ini = local.join("user-no-mii/mk8d.ini");
        assert_eq!(read(&local).unwrap()["values"]["bloom"], "1");
        fs::create_dir_all(ini.parent().unwrap()).unwrap();
        let original = "other_setting = keep\nbloom = invalid\n";
        fs::write(&ini, original).unwrap();
        assert_eq!(read(&local).unwrap()["values"]["bloom"], "1");
        assert!(write(&local, &json!({"bloom":"2"})).is_err());
        assert_eq!(fs::read_to_string(&ini).unwrap(), original);
        for value in ["0", "1"] {
            write(&local, &json!({"bloom":value})).unwrap();
            assert_eq!(read(&local).unwrap()["values"]["bloom"], value);
            assert!(fs::read_to_string(&ini)
                .unwrap()
                .contains("other_setting = keep"));
        }
        fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn patches_shared_ini_preserving_unknown_fields_and_rejecting_invalid_values() {
        let local = std::env::temp_dir().join(format!("mk8-settings-{}", std::process::id()));
        std::fs::create_dir_all(local.join("user-no-mii")).unwrap();
        let path = local.join("user-no-mii/mk8d.ini");
        let original = "# meu arquivo\r\nfuture_option = keep\r\nvolume = 1.00\r\nvolume = 1.50\r\nmenu_tip_seen = 1\r\n";
        std::fs::write(&path, original).unwrap();
        write(
            &local,
            &json!({"volume":"0.65", "resolution":"75%", "bind_key_a":"Space",
                              "bind_pad_a":"south", "language":"fr"}),
        )
        .unwrap();
        let saved = std::fs::read_to_string(&path).unwrap();
        assert!(saved.contains("# meu arquivo\r\nfuture_option = keep\r\n"));
        assert!(saved.contains("menu_tip_seen = 1\r\n"));
        assert_eq!(saved.matches("volume = 0.65").count(), 2);
        let settings = read(&local).unwrap();
        assert_eq!(settings["values"]["volume"], "0.65");
        assert_eq!(settings["values"]["language"], "fr");
        for invalid in [
            json!({"volume":"NaN"}),
            json!({"volume":"2.1"}),
            json!({"fullscreen":"2"}),
            json!({"resolution":"99x"}),
            json!({"unknown":"1"}),
            json!({"bind_key_a":"invalid-key"}),
            json!({"language":"fr\nfullscreen=1"}),
            json!({"bind_pad_a":"invalid"}),
            json!({"volume": 0.8}),
        ] {
            assert!(write(&local, &invalid).is_err(), "{invalid}");
            assert_eq!(std::fs::read_to_string(&path).unwrap(), saved);
        }
        std::fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn defaults_do_not_create_an_ini_and_external_edits_are_read_back() {
        let local =
            std::env::temp_dir().join(format!("mk8-settings-default-{}", std::process::id()));
        std::fs::create_dir_all(&local).unwrap();
        assert_eq!(read(&local).unwrap()["values"]["volume"], "1");
        assert!(!local.join("user-no-mii/mk8d.ini").exists());
        write(&local, &json!({"show_fps":"1"})).unwrap();
        assert_eq!(read(&local).unwrap()["values"]["show_fps"], "1");
        std::fs::write(local.join("user-no-mii/mk8d.ini"), "volume = 0.25\n").unwrap();
        assert_eq!(read(&local).unwrap()["values"]["volume"], "0.25");
        std::fs::remove_dir_all(local).unwrap();
    }

    #[test]
    fn aspect_modes_round_trip_without_changing_legacy_defaults_or_other_data() {
        let local =
            std::env::temp_dir().join(format!("mk8-settings-aspect-{}", std::process::id()));
        let user = local.join("user-no-mii");
        std::fs::create_dir_all(user.join("save")).unwrap();
        let path = user.join("mk8d.ini");
        assert_eq!(read(&local).unwrap()["values"]["aspect"], "unlocked");
        assert!(!path.exists());
        std::fs::write(user.join("save/marker"), b"existing progress").unwrap();
        std::fs::write(
            &path,
            "# existing settings\r\nfuture_option = keep\r\naspect = original\r\naspect = original\r\nframe_gen = 120\r\nvolume = 0.65\r\n",
        )
        .unwrap();
        for value in ["original", "unlocked", "ultrawide", "original"] {
            write(&local, &json!({"aspect": value})).unwrap();
            let saved = std::fs::read_to_string(&path).unwrap();
            assert_eq!(saved.matches(&format!("aspect = {value}\r\n")).count(), 2);
            assert!(saved.starts_with("# existing settings\r\nfuture_option = keep\r\n"));
            assert!(saved.ends_with("frame_gen = 120\r\nvolume = 0.65\r\n"));
            let loaded = read(&local).unwrap();
            assert_eq!(loaded["values"]["aspect"], value);
            assert_eq!(loaded["values"]["frame_gen"], "120");
            assert_eq!(
                std::fs::read(user.join("save/marker")).unwrap(),
                b"existing progress"
            );
        }
        let before = std::fs::read(&path).unwrap();
        for invalid in ["wide", "ultrawide\nframe_gen=off", ""] {
            assert!(write(&local, &json!({"aspect": invalid})).is_err());
            assert_eq!(std::fs::read(&path).unwrap(), before);
        }
        std::fs::remove_dir_all(local).unwrap();
    }
}
