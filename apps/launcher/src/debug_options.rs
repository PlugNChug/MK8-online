//! Launcher-only switches, applied in the game supervisor before spawning the runtime.
use mk8_manifest::{load, save, Result};
use serde::{Deserialize, Serialize};
use std::{fs, path::Path, time::SystemTime};

#[derive(Debug, Default, Deserialize, Serialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Options {
    pub graphics_capture: bool,
    pub audio_recording: bool,
    pub native_settings: bool,
}

fn plain(path: &Path) -> Result<()> {
    for parent in path.ancestors() {
        let meta = match fs::symlink_metadata(parent) {
            Ok(meta) => meta,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error.to_string()),
        };
        #[cfg(windows)]
        let linked = {
            use std::os::windows::fs::MetadataExt;
            meta.file_attributes() & 0x400 != 0
        };
        #[cfg(not(windows))]
        let linked = meta.file_type().is_symlink();
        if linked {
            return Err("Depuração: caminho com link recusado.".into());
        }
    }
    Ok(())
}

fn stored(local: &Path) -> Result<Option<Options>> {
    let path = local.join("debug-options.json");
    plain(&path)?;
    match fs::metadata(&path) {
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(None),
        Err(error) => Err(error.to_string()),
        Ok(meta) if meta.len() > 4096 => Err("Preferências de depuração grandes demais.".into()),
        Ok(_) => load(&path).map(Some),
    }
}

pub fn read(local: &Path) -> Result<Options> {
    Ok(stored(local)?.unwrap_or_else(|| Options {
        graphics_capture: std::env::var("MK8_GFX_DIAGNOSTICS").as_deref() == Ok("1"),
        audio_recording: std::env::var_os("LARECOMP_AUDIO_WAV").is_some(),
        native_settings: std::env::var("MK8_SETTINGS_EXPERIMENT").as_deref() == Ok("1"),
    }))
}

pub fn write(local: &Path, options: &Options) -> Result<()> {
    let path = local.join("debug-options.json");
    plain(&path)?;
    plain(&path.with_extension("json.new"))?;
    save(&path, options)
}

pub fn configure(local: &Path) -> Result<()> {
    // Keep existing environment-based launches until the user saves launcher preferences.
    let Some(options) = stored(local)? else {
        return Ok(());
    };
    // Preserve legacy graphics/settings preferences without enabling absent runtime hooks.
    std::env::remove_var("LARECOMP_AUDIO_WAV");
    if options.audio_recording {
        let folder = local.join("user-no-mii/audio-recordings");
        plain(&folder)?;
        fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
        let stamp = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .map_err(|e| e.to_string())?
            .as_nanos();
        let path = folder.join(format!("audio-{}-{stamp}.wav", std::process::id()));
        fs::OpenOptions::new()
            .create_new(true)
            .write(true)
            .open(&path)
            .map_err(|e| e.to_string())?;
        std::env::set_var("LARECOMP_AUDIO_WAV", path);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::process::Command;

    #[test]
    fn supervisor_probe() {
        let Some(local) = std::env::var_os("MK8_DEBUG_OPTIONS_TEST") else {
            return;
        };
        let local = Path::new(&local);
        let options = read(local).unwrap();
        configure(local).unwrap();
        println!(
            "PROBE:{}",
            serde_json::json!({
                "options":options,
                "graphics":std::env::var("MK8_GFX_DIAGNOSTICS").ok(),
                "settings":std::env::var("MK8_SETTINGS_EXPERIMENT").ok(),
                "audio":std::env::var("LARECOMP_AUDIO_WAV").ok()
            })
        );
    }

    #[test]
    fn audio_overrides_inherited_flag_and_preserves_legacy_options_and_user_files() {
        let local = std::env::temp_dir().join(format!("mk8-debug-options-{}", std::process::id()));
        fs::create_dir_all(local.join("user-no-mii/save")).unwrap();
        fs::write(local.join("user-no-mii/mk8d.ini"), "volume = 0.60\n").unwrap();
        fs::write(local.join("user-no-mii/save/marker"), "save").unwrap();
        let probe = || {
            let output = Command::new(std::env::current_exe().unwrap())
                .args([
                    "--exact",
                    "debug_options::tests::supervisor_probe",
                    "--nocapture",
                ])
                .env("MK8_DEBUG_OPTIONS_TEST", &local)
                .env("MK8_GFX_DIAGNOSTICS", "1")
                .env("MK8_SETTINGS_EXPERIMENT", "1")
                .env("LARECOMP_AUDIO_WAV", "existing-custom.wav")
                .output()
                .unwrap();
            assert!(
                output.status.success(),
                "{}",
                String::from_utf8_lossy(&output.stderr)
            );
            let out = String::from_utf8(output.stdout).unwrap();
            serde_json::from_str::<serde_json::Value>(
                out.lines().find_map(|s| s.strip_prefix("PROBE:")).unwrap(),
            )
            .unwrap()
        };
        let inherited = probe();
        assert_eq!(inherited["audio"], "existing-custom.wav");
        assert_eq!(inherited["options"]["graphics_capture"], true);
        assert!(!local.join("debug-options.json").exists());
        write(&local, &Options::default()).unwrap();
        let off = probe();
        assert_eq!(off["graphics"], "1");
        assert_eq!(off["settings"], "1");
        assert_eq!(off["audio"], serde_json::Value::Null);
        let on = Options {
            graphics_capture: true,
            audio_recording: true,
            native_settings: true,
        };
        write(&local, &on).unwrap();
        assert_eq!(read(&local).unwrap(), on);
        let mut audio_off = read(&local).unwrap();
        audio_off.audio_recording = false;
        write(&local, &audio_off).unwrap();
        assert!(read(&local).unwrap().graphics_capture);
        assert!(read(&local).unwrap().native_settings);
        assert_eq!(probe()["audio"], serde_json::Value::Null);
        write(&local, &on).unwrap();
        let first = probe();
        assert_eq!(first["graphics"], "1");
        assert_eq!(first["settings"], "1");
        let first_path = Path::new(first["audio"].as_str().unwrap());
        fs::write(first_path, "earlier recording").unwrap();
        let second = probe();
        assert_ne!(first["audio"], second["audio"]);
        assert!(Path::new(second["audio"].as_str().unwrap()).is_file());
        assert_eq!(fs::read_to_string(first_path).unwrap(), "earlier recording");
        assert_eq!(
            fs::read_to_string(local.join("user-no-mii/mk8d.ini")).unwrap(),
            "volume = 0.60\n"
        );
        assert_eq!(
            fs::read_to_string(local.join("user-no-mii/save/marker")).unwrap(),
            "save"
        );
        assert!(serde_json::from_str::<Options>(
            r#"{"graphics_capture":"1","audio_recording":false,"native_settings":false}"#
        )
        .is_err());
        assert!(serde_json::from_str::<Options>(r#"{"graphics_capture":true,"audio_recording":false,"native_settings":false,"unknown":true}"#).is_err());
        fs::write(local.join("debug-options.json"), "invalid").unwrap();
        assert!(read(&local).is_err());
        assert!(configure(&local).is_err());
        fs::remove_dir_all(local).unwrap();
    }
}
