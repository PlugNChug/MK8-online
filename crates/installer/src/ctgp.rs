//! CTGP Deluxe as an optional course pack (toolchain/ctgp_pack.py, import_ctgp.py).
//!
//! The pack is an overlay in `<data>/ctgp/pack` built from the player's own copy. The launcher switches it on or
//! off (`ctgp.json`); with it on, the game gets `MK8_ROMFS_OVERLAY` and its own saves (`save-ctgp`, seeded once
//! from the regular saves), so records and ghosts made on CTGP courses never mix with the base game's. The base
//! game and the DLC are never changed: base only, base + DLC, base + CTGP and base + DLC + CTGP all work.
use mk8_manifest::{save, Installation, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    fs,
    path::{Path, PathBuf},
};

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Preference {
    enabled: bool,
}

#[derive(Serialize)]
pub struct Status {
    pub state: &'static str,
    pub installed: bool,
    pub enabled: bool,
    pub detail: String,
    pub courses: u64,
    pub base_courses: u64,
    pub names_verified: bool,
    /// "base": in place of the base game's cups (+4 DLC cups); "dlc" / "dlc-late": CTGP cups 1-12 / 5-16 in place
    /// of the DLC's cups only.
    pub layout: String,
}

fn pack(local: &Path) -> PathBuf {
    local.join("ctgp/pack")
}

fn enabled(local: &Path) -> Result<bool> {
    match fs::read(local.join("ctgp.json")) {
        Ok(bytes) => serde_json::from_slice::<Preference>(&bytes)
            .map(|value| value.enabled)
            .map_err(|_| "Preferência do CTGP inválida.".into()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(false),
        Err(error) => Err(error.to_string()),
    }
}

/// The manifest, or None without a pack. `full` also checks every recorded file (size) and that nothing extra
/// was added: done before each launch, while status only reads the manifest.
fn manifest(local: &Path, full: bool) -> Result<Option<Value>> {
    let root = pack(local);
    let path = root.join("manifest.json");
    let bytes = match fs::read(&path) {
        Ok(bytes) => bytes,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            return if root.try_exists().map_err(|e| e.to_string())? {
                Err("CTGP: importação incompleta; importe novamente.".into())
            } else {
                Ok(None)
            };
        }
        Err(error) => return Err(error.to_string()),
    };
    let value: Value =
        serde_json::from_slice(&bytes).map_err(|_| "CTGP: manifesto inválido.".to_string())?;
    let files = value["files"]
        .as_object()
        .filter(|_| value["kind"] == "ctgp-deluxe" && value["schema"] == 1)
        .ok_or("CTGP: manifesto inválido.")?;
    if files.is_empty() {
        return Err("CTGP: manifesto sem arquivos.".into());
    }
    if full {
        let romfs = root.join("romfs");
        for (rel, entry) in files {
            if rel.contains("..") || rel.starts_with('/') || rel.contains(':') {
                return Err("CTGP: caminho inválido no manifesto.".into());
            }
            let size = fs::metadata(romfs.join(rel))
                .ok()
                .filter(|m| m.is_file())
                .map(|m| m.len());
            if size != entry["bytes"].as_u64() {
                return Err(format!(
                    "CTGP: arquivo ausente ou alterado: {rel}. Importe novamente."
                ));
            }
        }
        if count_files(&romfs)? != files.len() {
            return Err("CTGP: arquivos inesperados na sobreposição. Importe novamente.".into());
        }
    }
    Ok(Some(value))
}

fn count_files(dir: &Path) -> Result<usize> {
    let mut total = 0;
    for entry in fs::read_dir(dir).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let kind = entry.file_type().map_err(|e| e.to_string())?;
        if kind.is_symlink() {
            return Err("CTGP: link na sobreposição recusado.".into());
        }
        total += if kind.is_dir() {
            count_files(&entry.path())?
        } else {
            1
        };
    }
    Ok(total)
}

pub fn status(local: &Path, install: Option<&Installation>) -> Status {
    let mut value = Status {
        state: "unprepared",
        installed: false,
        enabled: false,
        detail: String::new(),
        courses: 0,
        base_courses: 0,
        names_verified: false,
        layout: String::new(),
    };
    let Some(install) = install else {
        return value;
    };
    if !install.pack_supported {
        value.state = "unsupported";
        value.detail = "Atualize a build para usar o CTGP.".into();
        return value;
    }
    match manifest(local, false).and_then(|m| Ok((m, enabled(local)?))) {
        Ok((Some(m), preference)) => {
            value.installed = true;
            value.enabled = preference;
            value.state = "ready";
            value.courses = m["courses"].as_u64().unwrap_or(0);
            value.base_courses = m["base_courses"].as_u64().unwrap_or(0);
            value.names_verified = m["names_verified"].as_bool().unwrap_or(false);
            value.layout = m["layout"].as_str().unwrap_or("base").to_string();
        }
        Ok((None, _)) => value.state = "missing",
        Err(error) => {
            value.state = "invalid";
            value.detail = error;
        }
    }
    value
}

pub fn set_enabled(local: &Path, install: &Installation, value: bool) -> Result<()> {
    if !install.pack_supported {
        return Err("Atualize a build para usar o CTGP.".into());
    }
    if manifest(local, false)?.is_none() {
        return Err("Importe o CTGP antes de ativar ou desativar.".into());
    }
    save(&local.join("ctgp.json"), &Preference { enabled: value })
}

/// The overlay folder to serve, verified, or None with the pack off (or absent).
pub fn launch_overlay(local: &Path, install: &Installation) -> Result<Option<PathBuf>> {
    if !enabled(local)? {
        return Ok(None);
    }
    if !install.pack_supported {
        return Err("Atualize a build para usar o CTGP ou desative-o em Configurações.".into());
    }
    match manifest(local, true)? {
        Some(_) => Ok(Some(pack(local).join("romfs"))),
        None => Err("CTGP ativado sem pacote importado. Importe-o ou desative-o.".into()),
    }
}

/// The identity of the pack the game would get at launch (for the Steam lobby rule), None with it off.
pub fn launch_identity(local: &Path, install: &Installation) -> Result<Option<String>> {
    if launch_overlay(local, install)?.is_none() {
        return Ok(None);
    }
    let value = manifest(local, false)?.ok_or("CTGP: pacote ausente.")?;
    value["identity"]
        .as_str()
        .filter(|id| id.len() == 64 && id.bytes().all(|b| b.is_ascii_hexdigit()))
        .map(|id| Some(id.to_string()))
        .ok_or_else(|| "CTGP: identidade do pacote inválida. Importe novamente.".into())
}

/// The save folder for a session with the pack on: `save-ctgp`, created once as a copy of `save` (the player's
/// progress carries over; what is earned on CTGP courses stays there).
pub fn save_folder(saves: &Path) -> Result<PathBuf> {
    let target = saves.join("save-ctgp");
    if !target.try_exists().map_err(|e| e.to_string())? {
        let source = saves.join("save");
        let pending = saves.join("save-ctgp.pending");
        if pending.try_exists().map_err(|e| e.to_string())? {
            fs::remove_dir_all(&pending).map_err(|e| e.to_string())?;
        }
        if source.is_dir() {
            copy_tree(&source, &pending)?;
        } else {
            fs::create_dir_all(&pending).map_err(|e| e.to_string())?;
        }
        fs::rename(&pending, &target).map_err(|e| e.to_string())?;
    }
    Ok(target)
}

fn copy_tree(from: &Path, to: &Path) -> Result<()> {
    fs::create_dir_all(to).map_err(|e| e.to_string())?;
    for entry in fs::read_dir(from).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        let kind = entry.file_type().map_err(|e| e.to_string())?;
        if kind.is_symlink() {
            return Err("Save com link recusado.".into());
        }
        let target = to.join(entry.file_name());
        if kind.is_dir() {
            copy_tree(&entry.path(), &target)?;
        } else {
            fs::copy(entry.path(), target).map_err(|e| e.to_string())?;
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use mk8_manifest::{hash_file, Installation};
    use std::collections::BTreeMap;

    fn install(local: &Path, supported: bool) -> Installation {
        let exe = local.join("exe.bin");
        fs::write(&exe, b"exe").unwrap();
        Installation {
            schema: 2,
            identity: "teste".into(),
            profile: "mk8d-smm2-experimental".into(),
            executable: hash_file(&exe).unwrap(),
            data_root: local.join("game-data/test"),
            shader_root: local.join("shaders"),
            gameplay_verified: false,
            dlc_supported: true,
            pack_supported: supported,
            files: BTreeMap::new(),
        }
    }

    fn fixture_pack(local: &Path) {
        let romfs = pack(local).join("romfs/Course/Gu_FirstCircuit");
        fs::create_dir_all(&romfs).unwrap();
        fs::write(romfs.join("course_model.szs"), b"modelo").unwrap();
        let manifest = serde_json::json!({"schema":1,"kind":"ctgp-deluxe","courses":64,"base_courses":48,
            "names_verified":true,"files":{"Course/Gu_FirstCircuit/course_model.szs":{"bytes":6}}});
        fs::write(
            pack(local).join("manifest.json"),
            serde_json::to_vec(&manifest).unwrap(),
        )
        .unwrap();
    }

    fn temp(name: &str) -> PathBuf {
        let local = std::env::temp_dir().join(format!("mk8-ctgp-{name}-{}", std::process::id()));
        let _ = fs::remove_dir_all(&local);
        fs::create_dir_all(&local).unwrap();
        local
    }

    #[test]
    fn the_pack_is_optional_and_switches_without_touching_its_files() {
        let local = temp("toggle");
        let install = install(&local, true);
        assert_eq!(status(&local, Some(&install)).state, "missing");
        assert!(set_enabled(&local, &install, true).is_err());
        assert_eq!(launch_overlay(&local, &install).unwrap(), None);
        fixture_pack(&local);
        let ready = status(&local, Some(&install));
        assert_eq!(
            (ready.state, ready.installed, ready.enabled),
            ("ready", true, false)
        );
        assert_eq!((ready.courses, ready.base_courses), (64, 48));
        assert_eq!(ready.layout, "base"); // (a pack from before the layouts)
        assert_eq!(launch_overlay(&local, &install).unwrap(), None); // imported but off by default
        set_enabled(&local, &install, true).unwrap();
        assert_eq!(
            launch_overlay(&local, &install).unwrap(),
            Some(pack(&local).join("romfs"))
        );
        set_enabled(&local, &install, false).unwrap();
        assert_eq!(launch_overlay(&local, &install).unwrap(), None);
        assert!(pack(&local)
            .join("romfs/Course/Gu_FirstCircuit/course_model.szs")
            .is_file());
        fs::remove_dir_all(&local).unwrap();
    }

    #[test]
    fn a_changed_pack_or_an_old_runtime_never_launches_as_if_it_were_on() {
        let local = temp("invalid");
        let mut install = install(&local, true);
        fixture_pack(&local);
        set_enabled(&local, &install, true).unwrap();
        let model = pack(&local).join("romfs/Course/Gu_FirstCircuit/course_model.szs");
        fs::write(&model, b"outro tamanho").unwrap();
        assert!(launch_overlay(&local, &install).is_err());
        fs::write(&model, b"modelo").unwrap();
        fs::write(pack(&local).join("romfs/extra.bin"), b"x").unwrap();
        assert!(launch_overlay(&local, &install).is_err());
        fs::remove_file(pack(&local).join("romfs/extra.bin")).unwrap();
        assert!(launch_overlay(&local, &install).unwrap().is_some());
        install.pack_supported = false;
        assert_eq!(status(&local, Some(&install)).state, "unsupported");
        assert!(launch_overlay(&local, &install).is_err());
        install.pack_supported = true;
        fs::write(pack(&local).join("manifest.json"), b"{}").unwrap();
        assert_eq!(status(&local, Some(&install)).state, "invalid");
        assert!(launch_overlay(&local, &install).is_err());
        fs::write(local.join("ctgp.json"), br#"{"enabled":"sim"}"#).unwrap();
        assert!(launch_overlay(&local, &install).is_err());
        fs::remove_dir_all(&local).unwrap();
    }

    #[test]
    fn ctgp_saves_start_as_a_copy_and_stay_apart() {
        let local = temp("saves");
        let saves = local.join("user-no-mii");
        fs::create_dir_all(saves.join("save/user")).unwrap();
        fs::write(saves.join("save/user/progress.bin"), b"base").unwrap();
        let target = save_folder(&saves).unwrap();
        assert_eq!(fs::read(target.join("user/progress.bin")).unwrap(), b"base");
        fs::write(target.join("user/progress.bin"), b"ctgp").unwrap();
        assert_eq!(save_folder(&saves).unwrap(), target); // seeded only once
        assert_eq!(fs::read(target.join("user/progress.bin")).unwrap(), b"ctgp");
        assert_eq!(
            fs::read(saves.join("save/user/progress.bin")).unwrap(),
            b"base"
        );
        fs::remove_dir_all(&local).unwrap();
    }
}
