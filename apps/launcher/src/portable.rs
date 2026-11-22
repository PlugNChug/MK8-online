//! Rebase only launcher-owned metadata when a portable tree changes location.
use crate::Error;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    fs,
    path::{Path, PathBuf},
};

#[derive(Serialize, Deserialize)]
struct Location {
    schema: u8,
    home: PathBuf,
}

fn mapped(value: &str, old: &Path, new: &Path) -> Option<String> {
    let clean = |v: &str| v.trim_start_matches(r"\\?\").replace('\\', "/");
    let value = clean(value);
    let prefix = clean(&old.to_string_lossy());
    let prefix = prefix.trim_end_matches('/');
    if !value.get(..prefix.len())?.eq_ignore_ascii_case(prefix) {
        return None;
    }
    let rest = &value[prefix.len()..];
    if !rest.is_empty() && !rest.starts_with('/') {
        return None;
    }
    if rest.split('/').any(|part| part == "..") {
        return None;
    }
    Some(
        new.join(rest.trim_start_matches('/'))
            .to_string_lossy()
            .into_owned(),
    )
}

fn rebase(value: &mut Value, old: &Path, new: &Path) {
    match value {
        Value::String(s) => {
            if let Some(next) = mapped(s, old, new) {
                *s = next;
            }
        }
        Value::Array(items) => {
            for item in items {
                rebase(item, old, new);
            }
        }
        Value::Object(items) => {
            for item in items.values_mut() {
                rebase(item, old, new);
            }
        }
        _ => {}
    }
}

fn metadata(name: &str) -> bool {
    matches!(
        name,
        "config.json"
            | "candidate.json"
            | "active.json"
            | "generation.json"
            | "generation-receipt.json"
            | "native.json"
            | "native-receipt.json"
            | "mii-native-tools.json"
            | "mii-resources.json"
            | "mii-composer.json"
            | "native-head.json"
            | "mii-library.json"
            | "fonts.json"
            | "font-library.json"
            | "import.json"
            | "import-receipt.json"
    )
}

fn regular(path: &Path) -> Result<(), Error> {
    for part in path.ancestors() {
        if let Ok(info) = fs::symlink_metadata(part) {
            #[cfg(windows)]
            {
                use std::os::windows::fs::MetadataExt;
                if info.file_attributes() & 0x400 != 0 {
                    return Err("Link recusado na realocação portátil".into());
                }
            }
            if info.file_type().is_symlink() {
                return Err("Link recusado na realocação portátil".into());
            }
        }
    }
    Ok(())
}

fn collect(dir: &Path, out: &mut Vec<PathBuf>) -> Result<(), Error> {
    if !dir.exists() {
        return Ok(());
    }
    regular(dir)?;
    for entry in fs::read_dir(dir)? {
        let entry = entry?;
        let path = entry.path();
        let info = fs::symlink_metadata(&path)?;
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if info.file_attributes() & 0x400 != 0 {
                continue;
            }
        }
        if info.file_type().is_symlink() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().into_owned();
        if info.is_dir() {
            if name.starts_with("user")
                || name.starts_with("cpython")
                || name.starts_with('.')
                || matches!(
                    name.as_str(),
                    "python"
                        | "runtime"
                        | "romfs"
                        | "exefs"
                        | "extracted"
                        | "cache"
                        | "objects"
                        | "reports"
                        | "logs"
                        | "node_modules"
                        | "native-tools"
                )
            {
                continue;
            }
            collect(&path, out)?;
        } else if metadata(&name) {
            out.push(path);
        }
    }
    Ok(())
}

pub fn relocate(home: &Path, root: &Path, local: &Path) -> Result<(), Error> {
    regular(home)?;
    fs::create_dir_all(home)?;
    let home = crate::canonical(home)?;
    // Explicit external --data-dir belongs to that installation, not to this portable tree.
    let state = home.join("portable-location.json");
    regular(&state)?;
    let previous: Option<Location> = if state.exists() {
        Some(mk8_manifest::load(&state)?)
    } else {
        None
    };
    if let Some(previous) = &previous {
        if previous.schema != 1 || !previous.home.is_absolute() {
            return Err("Registro portátil inválido".into());
        }
        if previous.home == home {
            return Ok(());
        }
    }
    let _guards = mk8_build_manager::acquire_operation(root, local)?;
    let build_lock = fs::OpenOptions::new()
        .create(true)
        .truncate(false)
        .read(true)
        .write(true)
        .open(local.join(".build.lock"))?;
    build_lock.try_lock()?;
    if let Some(previous) = previous {
        let mut paths = Vec::new();
        // Only mutable metadata, never embedded sources, saves or game data.
        if local.starts_with(&home) {
            collect(local, &mut paths)?;
        }
        for entry in fs::read_dir(&home)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.len() == 16 && name.bytes().all(|b| b.is_ascii_hexdigit()) {
                let version = entry.path();
                collect(&version.join("local"), &mut paths)?;
                let cfg = version.join("local/python/pyvenv.cfg");
                if cfg.is_file() {
                    regular(&cfg)?;
                    paths.push(cfg);
                }
            }
        }
        paths.sort();
        paths.dedup();
        let mut changes = Vec::new();
        for path in paths {
            let original = fs::read(&path)?;
            let next = if path.file_name().is_some_and(|n| n == "pyvenv.cfg") {
                let text = std::str::from_utf8(&original)?;
                text.lines()
                    .map(|line| {
                        if let Some((key, value)) = line.split_once(" = ") {
                            if matches!(key, "home" | "executable") {
                                if let Some(next) = mapped(value, &previous.home, &home) {
                                    return format!("{key} = {next}");
                                }
                            }
                        }
                        line.to_owned()
                    })
                    .collect::<Vec<_>>()
                    .join("\n")
                    .into_bytes()
            } else {
                let mut value: Value = serde_json::from_slice(&original)?;
                let before = value.clone();
                rebase(&mut value, &previous.home, &home);
                if value == before {
                    continue;
                }
                serde_json::to_vec_pretty(&value)?
            };
            if next != original {
                changes.push((path, original, next));
            }
        }
        let stamp = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)?
            .as_nanos();
        let backup = home.join(".relocation-backups").join(stamp.to_string());
        regular(&backup)?;
        for (path, original, next) in changes {
            let saved = backup.join(path.strip_prefix(&home)?);
            fs::create_dir_all(saved.parent().ok_or("Backup sem pasta")?)?;
            fs::write(saved, original)?;
            let temp = path.with_extension("relocating");
            regular(&temp)?;
            fs::write(&temp, next)?;
            fs::rename(temp, path)?;
        }
    }
    mk8_manifest::save(&state, &Location { schema: 1, home })?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(windows)]
    #[test]
    fn renamed_portable_mii_resources_launch_but_redirected_resources_do_not() {
        use mk8_installer::mii_resources::{configure, launch_directory};
        let base = crate::canonical(&std::env::temp_dir())
            .unwrap()
            .join(format!("mk8-relocate-mii-{}", std::process::id()));
        let old = base.join("old");
        let new = base.join("new");
        let data = old.join("data");
        let assets = data.join("MiiModel");
        fs::create_dir_all(&assets).unwrap();
        let mut bytes = vec![0; 0x15000];
        bytes[12] = 1;
        for name in [
            "ShapeMid.dat",
            "ShapeHigh.dat",
            "NXTextureLowLinear.dat",
            "NXTextureLowSRGB.dat",
            "NXTextureMidLinear.dat",
            "NXTextureMidSRGB.dat",
        ] {
            fs::write(assets.join(name), &bytes).unwrap();
        }
        configure(&data, &assets, true).unwrap();
        assert!(launch_directory(&data).unwrap().is_some());
        relocate(&old, &old.join("0123456789abcdef"), &data).unwrap();
        fs::rename(&old, &new).unwrap();
        let data = new.join("data");
        relocate(&new, &new.join("0123456789abcdef"), &data).unwrap();
        let assets = data.join("MiiModel");
        assert_eq!(launch_directory(&data).unwrap(), Some(assets.clone()));

        // The same bytes at a redirected destination must still fail the path check.
        let outside = base.join("outside");
        fs::rename(&assets, &outside).unwrap();
        let made = std::process::Command::new("cmd")
            .args(["/d", "/c", "mklink", "/J"])
            .arg(&assets)
            .arg(&outside)
            .output()
            .unwrap();
        assert!(made.status.success(), "{made:?}");
        assert_eq!(
            launch_directory(&data).unwrap_err().to_string(),
            "Caminho de recurso Mii alterado."
        );
        fs::remove_dir(&assets).unwrap();
        fs::remove_dir_all(base).unwrap();
    }
    #[test]
    fn move_preserves_saves_hashes_and_external_paths() {
        // Match run_at's canonical paths even when TEMP uses a different Windows spelling.
        let base = crate::canonical(&std::env::temp_dir())
            .unwrap()
            .join(format!("mk8-relocate-{}", std::process::id()));
        let old = base.join("before/portable");
        let new = base.join("after/portable");
        let version = "0123456789abcdef";
        fs::create_dir_all(old.join("data/user-no-mii/save")).unwrap();
        fs::create_dir_all(old.join(version).join("local/python")).unwrap();
        let game = old.join("data/game.exe");
        fs::write(&game, b"fixture").unwrap();
        let candidate = serde_json::json!({"executable":{"path":game,"sha256":"unchanged"},"outside":"Z:/outside/game.nsp"});
        mk8_manifest::save(&old.join("data/candidate.json"), &candidate).unwrap();
        let save = old.join("data/user-no-mii/save/data.json");
        fs::write(&save, serde_json::to_vec(&candidate).unwrap()).unwrap();
        let saved = fs::read(&save).unwrap();
        fs::write(
            old.join(version).join("local/python/pyvenv.cfg"),
            format!("home = {}/tools\n", old.display()),
        )
        .unwrap();
        relocate(&old, &old.join(version), &old.join("data")).unwrap();
        fs::create_dir_all(new.parent().unwrap()).unwrap();
        assert!(old.starts_with(&base) && new.starts_with(&base));
        fs::rename(&old, &new).unwrap();
        relocate(&new, &new.join(version), &new.join("data")).unwrap();
        let changed: Value = mk8_manifest::load(&new.join("data/candidate.json")).unwrap();
        assert_eq!(
            Path::new(changed["executable"]["path"].as_str().unwrap())
                .canonicalize()
                .unwrap(),
            new.join("data/game.exe").canonicalize().unwrap()
        );
        assert_eq!(changed["executable"]["sha256"], "unchanged");
        assert_eq!(changed["outside"], "Z:/outside/game.nsp");
        assert_eq!(
            fs::read(new.join("data/user-no-mii/save/data.json")).unwrap(),
            saved
        );
        assert!(
            fs::read_to_string(new.join(version).join("local/python/pyvenv.cfg"))
                .unwrap()
                .contains(
                    &crate::canonical(&new)
                        .unwrap()
                        .to_string_lossy()
                        .to_string()
                )
        );
        assert!(new.join(".relocation-backups").is_dir());
        let bytes = fs::read(new.join("data/candidate.json")).unwrap();
        relocate(&new, &new.join(version), &new.join("data")).unwrap();
        assert_eq!(fs::read(new.join("data/candidate.json")).unwrap(), bytes);
        assert!(base
            .canonicalize()
            .unwrap()
            .starts_with(std::env::temp_dir().canonicalize().unwrap()));
        fs::remove_dir_all(base).unwrap();
    }
    #[test]
    fn mapping_requires_component_boundary_and_rejects_traversal() {
        let old = Path::new("C:/portable");
        let new = Path::new("D:/moved");
        assert!(mapped("C:/portable-other/file", old, new).is_none());
        assert!(mapped("C:/portable/../outside", old, new).is_none());
        assert_eq!(
            mapped(r"\\?\C:\portable\data\file", old, new),
            Some(new.join("data/file").to_string_lossy().into_owned())
        );
    }

    #[test]
    fn invalid_metadata_leaves_location_and_other_files_unchanged() {
        let base = crate::canonical(&std::env::temp_dir())
            .unwrap()
            .join(format!("mk8-invalid-relocate-{}", std::process::id()));
        fs::create_dir_all(base.join("data")).unwrap();
        let old = base.join("previous");
        mk8_manifest::save(
            &base.join("portable-location.json"),
            &Location {
                schema: 1,
                home: old.clone(),
            },
        )
        .unwrap();
        let bytes = serde_json::to_vec(&serde_json::json!({"romfs":old.join("romfs")})).unwrap();
        fs::write(base.join("data/config.json"), &bytes).unwrap();
        fs::write(base.join("data/candidate.json"), b"broken").unwrap();
        assert!(relocate(&base, &base.join("0123456789abcdef"), &base.join("data")).is_err());
        assert_eq!(fs::read(base.join("data/config.json")).unwrap(), bytes);
        let state: Location = mk8_manifest::load(&base.join("portable-location.json")).unwrap();
        assert_eq!(state.home, old);
        assert!(base
            .canonicalize()
            .unwrap()
            .starts_with(std::env::temp_dir().canonicalize().unwrap()));
        fs::remove_dir_all(base).unwrap();
    }
}
