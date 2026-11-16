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

fn valid(schema: &Value, field: &Value, value: &str) -> bool {
    match field["kind"].as_str() {
        Some("bool") => matches!(value, "0" | "1"),
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

pub fn read(local: &Path) -> Result<Value> {
    let mut schema = schema();
    let mut values: Map<String, Value> = fields(&schema)
        .map(|field| {
            (
                field["key"].as_str().unwrap().into(),
                field["default"].clone(),
            )
        })
        .collect();
    for line in contents(&path(local)?)?.lines() {
        if let Some((key, value)) = entry(line) {
            if let Some(slot) = values.get_mut(key) {
                *slot = Value::String(value.into());
            }
        }
    }
    if values["resolution"] == "67%" {
        values["resolution"] = json!("75%");
    }
    schema["values"] = Value::Object(values);
    Ok(schema)
}

pub fn write(local: &Path, patch: &Value) -> Result<()> {
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
}
