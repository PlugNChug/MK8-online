use crate::{python_setup, Error};
use std::{
    fs,
    path::{Path, PathBuf},
};

fn hex_id(name: &str) -> bool {
    name.len() == 16 && name.bytes().all(|c| c.is_ascii_hexdigit())
}

pub(crate) fn direct(path: &Path) -> Result<(), Error> {
    if !path.is_absolute()
        || path
            .components()
            .any(|p| p == std::path::Component::ParentDir)
    {
        return Err("Caminho inválido".into());
    }
    for part in path.ancestors() {
        python_setup::no_link(part)?;
    }
    Ok(())
}

pub(crate) fn payload(path: &Path) -> bool {
    path.file_name()
        .and_then(|s| s.to_str())
        .is_some_and(hex_id)
        && path.join("upstream/manifests/smm2recomp.json").is_file()
        && path.join("scripts/setup-native.py").is_file()
}

// Only direct children of a verified directory may be selected. Inspect the
// entire selection before deleting anything so a junction fails closed.
fn validate_selection(parent: &Path, paths: &[PathBuf]) -> Result<(), Error> {
    direct(parent)?;
    for path in paths {
        if !parent.is_absolute() || path.parent() != Some(parent) {
            return Err("Destino inválido".into());
        }
        python_setup::no_link(path)?;
        if path.is_dir() {
            python_setup::checked_tree(path, parent)?;
        }
    }
    Ok(())
}

fn remove_validated(paths: &[PathBuf]) -> Result<(), Error> {
    for path in paths {
        if path.is_dir() {
            fs::remove_dir_all(path)?;
        } else if path.is_file() {
            fs::remove_file(path)?;
        }
    }
    Ok(())
}

fn version(s: &str) -> bool {
    s.split('.').count() == 3
        && s.split('.')
            .all(|p| !p.is_empty() && p.bytes().all(|c| c.is_ascii_digit()))
}

fn tool_name(name: &str, older: bool) -> bool {
    let (name, backup) = match name.split_once("-backup-") {
        Some((original, id)) if hex_id(id) => (original, true),
        _ => (name, false),
    };
    (older || backup)
        && (name == "python"
            || name.strip_prefix("cpython-").is_some_and(version)
            || name
                .strip_prefix("python-")
                .and_then(|s| s.strip_suffix(".zip"))
                .is_some_and(version))
}

fn lock_existing(dir: &Path, name: &str) -> Result<Option<fs::File>, Error> {
    let path = dir.join(name);
    direct(&path)?;
    if !path.exists() {
        return Ok(None);
    }
    let file = fs::OpenOptions::new().read(true).write(true).open(path)?;
    file.try_lock()
        .map_err(|e| format!("Feche o jogo ou a preparação em andamento: {e}"))?;
    Ok(Some(file))
}

fn obsolete_compiler(root: &Path, old: &Path, shared: &Path) -> Result<(), Error> {
    let script = |dir: &Path| fs::read_to_string(dir.join("scripts/setup-native.py"));
    let current_script = script(root)?;
    let old_script = script(old)?;
    let pin = |s: &str, key| crate::setup::script_constant(s, key);
    let (Some(current), Some(previous), Some(hash)) = (
        pin(&current_script, "VERSION"),
        pin(&old_script, "VERSION"),
        pin(&old_script, "SHA256"),
    ) else {
        return Ok(());
    };
    if !version(&previous)
        || !version(&current)
        || previous == current
        || hash.len() != 64
        || !shared
            .join(format!("zig-x86_64-windows-{current}/zig.exe"))
            .is_file()
    {
        return Ok(());
    }
    let name = format!("zig-x86_64-windows-{previous}");
    let receipt = shared.join(format!("{name}.done"));
    let Ok(raw) = fs::read_to_string(&receipt) else {
        return Ok(());
    };
    let fields: Vec<_> = raw.split_whitespace().collect();
    let dir = shared.join(&name);
    if fields.len() == 2
        && fields[0] == hash
        && mk8_manifest::hash_file(&dir.join("zig.exe")).is_ok_and(|f| f.sha256 == fields[1])
    {
        let targets = [dir, receipt];
        validate_selection(shared, &targets)?;
        remove_validated(&targets)?;
    }
    Ok(())
}

pub fn clean_old_tools(root: &Path) -> Result<(), Error> {
    if !payload(root) {
        return Ok(());
    }
    let base = root.parent().ok_or("Pasta do launcher ausente")?;
    direct(root)?;
    let created = fs::metadata(root)?.created()?;
    let shared = base.join("native-tools");
    for entry in fs::read_dir(base)? {
        let entry = entry?;
        let folder = entry.path();
        if !entry.file_type()?.is_dir() || !payload(&folder) {
            continue;
        }
        direct(&folder)?;
        let older = folder != root;
        if older && entry.metadata()?.created()? >= created {
            continue;
        }
        let local = folder.join("local");
        direct(&local)?;
        if !local.is_dir() {
            continue;
        }
        let _guard = if older {
            lock_existing(&local, ".launcher-operation.lock")?
        } else {
            None
        };
        let mut targets = Vec::new();
        for item in fs::read_dir(&local)? {
            let item = item?;
            let name = item.file_name();
            let name = name.to_string_lossy();
            if tool_name(&name, older)
                || (older
                    && name == "native-tools"
                    && item.path().join("deps/runtime-ready.json").is_file())
            {
                // Legacy versions sometimes linked local/native-tools to the shared tools.
                if python_setup::no_link(&item.path()).is_err() {
                    continue;
                }
                let path = item.path();
                let original = name.split("-backup-").next().unwrap_or(&name);
                if (original == "python" && path.join("pyvenv.cfg").is_file())
                    || (original.starts_with("cpython-") && path.join("tools/python.exe").is_file())
                    || (original.ends_with(".zip") && path.is_file())
                    || original == "native-tools"
                {
                    targets.push(path);
                }
            }
        }
        if !targets.is_empty() {
            python_setup::idle(&local)?;
            validate_selection(&local, &targets)?;
            remove_validated(&targets)?;
        }
        if older && shared.is_dir() {
            obsolete_compiler(root, &folder, &shared)?;
        }
    }
    Ok(())
}

pub fn remove_data(base: &Path) -> Result<(), Error> {
    direct(base)?;
    if !base.is_absolute() || base.parent().is_none() {
        return Err("Pasta de dados inválida".into());
    }
    let data = base.join("data");
    let shared = base.join("native-tools");
    if !base.exists() {
        return Ok(());
    }
    let mut guards = Vec::new();
    for (dir, name) in [
        (&data, ".launcher-operation.lock"),
        (&data, ".build.lock"),
        (&shared, ".launcher-operation.lock"),
    ] {
        guards.push(lock_existing(dir, name)?);
    }
    let mut targets = Vec::new();
    for entry in fs::read_dir(base)? {
        let entry = entry?;
        if entry.file_type()?.is_dir() && payload(&entry.path()) {
            guards.push(lock_existing(
                &entry.path().join("local"),
                ".launcher-operation.lock",
            )?);
            targets.push(entry.path());
        }
    }
    let has_payload = !targets.is_empty();
    if fs::read(shared.join(".mk8-managed-tools")).is_ok_and(|s| s == b"MK8-online tools v1")
        || (shared.join("deps/runtime-ready.json").is_file()
            && shared.join(".launcher-operation.lock").is_file())
    {
        targets.push(shared.clone());
    }
    direct(&data)?;
    let mut legacy = Vec::new();
    if data.is_dir() {
        // The installer accepts custom base folders. Never remove the chosen base
        // recursively or follow source paths from the user's configuration.
        let owned = data.join(".mk8-managed-data");
        if fs::read(&owned).is_ok_and(|s| s == b"MK8-online data v1") {
            targets.push(data.clone());
        } else {
            let names = [
                "builds",
                "game-data",
                "imports",
                "logs",
                "dlc",
                "ctgp",
                "reports",
                "profiles",
                "user-no-mii",
                "user-ctgp",
                "mii-library",
                "mii-resources-generated",
                "font-library",
                "updates",
                "native-tools",
                "config.json",
                "candidate.json",
                "active.json",
                "native.json",
                "built-with.json",
                "generation.json",
                "ctgp.json",
                "dlc.json",
                "mii-profile.json",
                "mii-resources.json",
                "mii-composer.json",
                "mii-native-tools.json",
                "font-library.json",
                "debug-options.json",
                "username.json",
                "steam-enabled",
                "steam-navigation.json",
                "language.json",
                "launcher.json",
                "steam_appid.txt",
                "build.log",
                "setup.log",
                "game.log",
                "ctgp.log",
                "dlc.log",
                "vulkan-check.log",
                ".build.lock",
                ".launcher-operation.lock",
            ];
            if has_payload
                || data.join("launcher.json").is_file()
                || data.join("candidate.json").is_file()
            {
                legacy = names
                    .iter()
                    .map(|n| data.join(n))
                    .filter(|p| p.exists())
                    .collect();
            }
        }
    }
    validate_selection(&data, &legacy)?;
    validate_selection(base, &targets)?;
    remove_validated(&legacy)?;
    remove_validated(&targets)?;
    drop(guards);
    for dir in [&data, &shared] {
        if dir.is_dir() && fs::read_dir(dir)?.next().is_none() {
            fs::remove_dir(dir)?;
        }
    }
    Ok(())
}

pub fn create_data(base: &Path) -> Result<(), Error> {
    let dir = base.join("data");
    direct(&dir)?;
    fs::create_dir_all(base)?;
    match fs::create_dir(&dir) {
        Ok(()) => fs::write(dir.join(".mk8-managed-data"), b"MK8-online data v1")?,
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists && dir.is_dir() => (),
        Err(e) => return Err(e.into()),
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch() -> PathBuf {
        let base = std::env::temp_dir().join(format!(
            "mk8-owned-{:016x}",
            u64::from_le_bytes(mk8_transport::random())
        ));
        fs::create_dir(&base).unwrap();
        base
    }

    fn seed(base: &Path, file: &str) {
        let path = base.join(file);
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        fs::write(path, b"fixture").unwrap();
    }

    #[test]
    fn tools_cleanup_preserves_builds_active_tools_and_unrelated_folders() {
        let base = scratch();
        let old = base.join("1111111111111111");
        let current = base.join("2222222222222222");
        for root in [&old, &current] {
            for f in [
                "upstream/manifests/smm2recomp.json",
                "scripts/setup-native.py",
                "local/python/pyvenv.cfg",
                "local/cpython-3.12.1/tools/python.exe",
                "local/python-3.12.1.zip",
                "local/builds/game.exe",
                "local/user/save",
            ] {
                seed(root, f);
            }
        }
        let backup = "local/python-backup-abcdef0123456789/pyvenv.cfg";
        seed(&current, backup);
        seed(&base, "3333333333333333/local/python/pyvenv.cfg");
        seed(&old, "local/cpython-1.2.3/personal.txt");
        seed(&base, "native-tools/deps/asset");
        let shared = base.join("native-tools");
        let previous = "zig-x86_64-windows-0.15.0";
        let active = "zig-x86_64-windows-0.17.0/zig.exe";
        seed(&shared, active);
        seed(&shared, &format!("{previous}/zig.exe"));
        let digest = mk8_manifest::hash_file(&shared.join(previous).join("zig.exe"))
            .unwrap()
            .sha256;
        let archive = "a".repeat(64);
        fs::write(
            old.join("scripts/setup-native.py"),
            format!("VERSION = '0.15.0'\nSHA256 = '{archive}'"),
        )
        .unwrap();
        fs::write(
            current.join("scripts/setup-native.py"),
            "VERSION = '0.17.0'",
        )
        .unwrap();
        let receipt = shared.join(format!("{previous}.done"));
        fs::write(&receipt, format!("{archive} {digest}")).unwrap();
        clean_old_tools(&current).unwrap();
        assert!(shared.join(active).is_file());
        assert!(!shared.join(previous).exists() && !receipt.exists());
        assert!(!current.join(backup).exists());
        for f in [
            "local/python/pyvenv.cfg",
            "local/cpython-3.12.1/tools/python.exe",
            "local/python-3.12.1.zip",
        ] {
            assert!(current.join(f).is_file());
            assert!(!old.join(f).exists());
        }
        for f in [
            "local/builds/game.exe",
            "local/user/save",
            "local/cpython-1.2.3/personal.txt",
        ] {
            assert!(old.join(f).is_file());
        }
        assert!(base
            .join("3333333333333333/local/python/pyvenv.cfg")
            .exists());
        assert!(base.join("native-tools/deps/asset").exists());
        fs::remove_dir_all(base).unwrap();
    }

    #[test]
    fn full_removal_requires_idle_data_and_preserves_neighbors() {
        let base = scratch();
        create_data(&base).unwrap();
        seed(&base, "data/user-no-mii/save/progress");
        seed(&base, "personal/file");
        seed(&base, "data/.build.lock");
        let guard = lock_existing(&base.join("data"), ".build.lock").unwrap();
        assert!(remove_data(&base).is_err());
        assert!(base.join("data/user-no-mii/save/progress").is_file());
        drop(guard);
        remove_data(&base).unwrap();
        assert!(!base.join("data").exists());
        assert!(base.join("personal/file").is_file());
        seed(&base, "data/launcher.json");
        seed(&base, "data/other/document");
        seed(&base, "data/user-no-mii/save/progress");
        remove_data(&base).unwrap();
        assert!(base.join("data/other/document").is_file());
        assert!(!base.join("data/user-no-mii").exists());
        fs::remove_dir_all(base).unwrap();
    }

    #[cfg(windows)]
    #[test]
    fn full_removal_refuses_nested_junction_before_deleting_other_files() {
        let base = scratch();
        create_data(&base).unwrap();
        seed(&base, "data/mii-profile.json");
        seed(&base, "external/save");
        let link = base.join("data").join("linked");
        assert!(std::process::Command::new("cmd")
            .args(["/d", "/c", "mklink", "/J"])
            .arg(&link)
            .arg(base.join("external"))
            .output()
            .unwrap()
            .status
            .success());
        assert!(remove_data(&base).is_err());
        assert!(base.join("external/save").exists());
        assert!(base.join("data/mii-profile.json").exists());
        fs::remove_dir(link).unwrap();
        fs::remove_dir_all(base).unwrap();
    }
}
