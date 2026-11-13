use mk8_manifest::{save, verify, Installation, Result};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
};

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Preference {
    enabled: bool,
}

fn enabled(local: &Path) -> Result<bool> {
    match fs::read(local.join("dlc.json")) {
        Ok(bytes) => serde_json::from_slice::<Preference>(&bytes)
            .map(|value| value.enabled)
            .map_err(|_| "Preferência de DLC inválida.".into()),
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(true),
        Err(error) => Err(error.to_string()),
    }
}

#[derive(Serialize)]
pub struct Status {
    pub state: &'static str,
    pub installed: bool,
    pub enabled: bool,
    pub detail: String,
}

fn verified_root(local: &Path, install: &Installation) -> Result<Option<PathBuf>> {
    let stable = local.join("dlc");
    let root = if stable.try_exists().map_err(|e| e.to_string())? {
        stable
    } else {
        install.data_root.join("extracted/dlc")
    };
    let has_records = install.files.keys().any(|name| name.starts_with("dlc-"));
    if !root.try_exists().map_err(|e| e.to_string())? && !has_records {
        return Ok(None);
    }
    let romfs = root.join("0100152000023001/romfs");
    for (name, path) in [
        ("dlc-manifest", root.join("installed.bin")),
        ("dlc-contents", romfs.join("contents.dat")),
        ("dlc-version", romfs.join("version.txt")),
    ] {
        let file = install
            .files
            .get(name)
            .ok_or("DLC: registros incompletos; importe novamente.")?;
        let expected = path.canonicalize().map_err(|e| e.to_string())?;
        if file.path.canonicalize().map_err(|e| e.to_string())? != expected {
            return Err("DLC: arquivos fora da instalação selecionada.".into());
        }
        verify(file)?;
    }
    let marker = fs::read(root.join("installed.bin")).map_err(|e| e.to_string())?;
    let mut header = b"MK8DLC1\0".to_vec();
    header.extend(0x0100152000022000u64.to_le_bytes());
    header.extend(0x0100152000023001u64.to_le_bytes());
    header.extend(65536u32.to_le_bytes());
    header.extend(1u32.to_le_bytes());
    header.extend(4096u64.to_le_bytes());
    header.extend(5u64.to_le_bytes());
    if marker.len() != 112
        || marker[..48] != header
        || install.files["dlc-contents"].bytes != 4096
        || install.files["dlc-version"].bytes != 5
    {
        return Err("DLC: manifesto incompatível.".into());
    }
    for (offset, name) in [(48, "dlc-contents"), (80, "dlc-version")] {
        let hash: String = marker[offset..offset + 32]
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect();
        if hash != install.files[name].sha256 {
            return Err("DLC: hash do manifesto divergente.".into());
        }
    }
    Ok(Some(root))
}

pub fn status(local: &Path, install: Option<&Installation>) -> Status {
    let mut value = Status {
        state: "unprepared",
        installed: false,
        enabled: false,
        detail: String::new(),
    };
    if let Some(install) = install {
        if !install.dlc_supported {
            value.state = "unsupported";
            value.detail = "Atualize a build para usar DLC.".into();
            return value;
        }
        match verified_root(local, install).and_then(|root| Ok((root, enabled(local)?))) {
            Ok((root, preference)) => {
                value.installed = root.is_some();
                value.enabled = value.installed && preference;
                value.state = if value.installed { "ready" } else { "missing" };
            }
            Err(error) => {
                value.state = "invalid";
                value.detail = error;
            }
        }
    }
    value
}

pub fn set_enabled(local: &Path, install: &Installation, value: bool) -> Result<()> {
    if !install.dlc_supported {
        return Err("Atualize a build para usar DLC.".into());
    }
    if verified_root(local, install)?.is_none() {
        return Err("Importe o DLC antes de ativar ou desativar.".into());
    }
    save(&local.join("dlc.json"), &Preference { enabled: value })
}

pub fn launch_root(local: &Path, install: &Installation) -> Result<Option<PathBuf>> {
    if !install.dlc_supported {
        return Ok(None);
    }
    if !enabled(local)? {
        return Ok(None);
    }
    verified_root(local, install)
}

#[cfg(test)]
mod tests {
    use super::*;
    use mk8_manifest::{hash_file, save, Installation};
    use std::{collections::BTreeMap, fs};

    #[test]
    fn preference_persists_without_changing_installed_files() {
        let local = std::env::temp_dir().join(format!("mk8-dlc-state-{}", std::process::id()));
        fs::create_dir_all(&local).unwrap();
        let root = local.join("dlc");
        let romfs = root.join("0100152000023001/romfs");
        fs::create_dir_all(&romfs).unwrap();
        fs::write(romfs.join("contents.dat"), [b'!'; 4096]).unwrap();
        fs::write(romfs.join("version.txt"), b"teste").unwrap();
        let content = hash_file(&romfs.join("contents.dat")).unwrap();
        let version = hash_file(&romfs.join("version.txt")).unwrap();
        let mut marker = b"MK8DLC1\0".to_vec();
        marker.extend(0x0100152000022000u64.to_le_bytes());
        marker.extend(0x0100152000023001u64.to_le_bytes());
        marker.extend(65536u32.to_le_bytes());
        marker.extend(1u32.to_le_bytes());
        marker.extend(4096u64.to_le_bytes());
        marker.extend(5u64.to_le_bytes());
        for hash in [&content.sha256, &version.sha256] {
            for i in (0..64).step_by(2) {
                marker.push(u8::from_str_radix(&hash[i..i + 2], 16).unwrap());
            }
        }
        fs::write(root.join("installed.bin"), &marker).unwrap();
        let mut install = Installation {
            schema: 2,
            identity: "autoral".into(),
            profile: "mk8d-smm2-experimental".into(),
            executable: content.clone(),
            data_root: local.join("game-data/test"),
            shader_root: local.join("shaders"),
            gameplay_verified: false,
            dlc_supported: true,
            files: BTreeMap::from([
                (
                    "dlc-manifest".into(),
                    hash_file(&root.join("installed.bin")).unwrap(),
                ),
                ("dlc-contents".into(), content.clone()),
                ("dlc-version".into(), version),
            ]),
        };
        assert_eq!(launch_root(&local, &install).unwrap(), Some(root.clone()));
        set_enabled(&local, &install, false).unwrap();
        assert_eq!(launch_root(&local, &install).unwrap(), None);
        assert!(!status(&local, Some(&install)).enabled);
        assert_eq!(fs::read(root.join("installed.bin")).unwrap(), marker);
        set_enabled(&local, &install, true).unwrap();
        assert_eq!(launch_root(&local, &install).unwrap(), Some(root));
        fs::write(&content.path, b"alterado").unwrap();
        assert_eq!(status(&local, Some(&install)).state, "invalid");
        assert!(launch_root(&local, &install).is_err());
        assert!(set_enabled(&local, &install, true).is_err());
        install.dlc_supported = false;
        assert_eq!(status(&local, Some(&install)).state, "unsupported");
        assert!(set_enabled(&local, &install, true).is_err());
        assert_eq!(launch_root(&local, &install).unwrap(), None);
        install.dlc_supported = true;
        save(
            &local.join("dlc.json"),
            &serde_json::json!({"enabled": "errado"}),
        )
        .unwrap();
        assert!(launch_root(&local, &install).is_err());
        fs::remove_dir_all(&local).unwrap();
    }

    #[test]
    fn absent_content_cannot_be_enabled() {
        let local = std::env::temp_dir().join(format!("mk8-dlc-missing-{}", std::process::id()));
        fs::create_dir_all(&local).unwrap();
        assert_eq!(status(&local, None).state, "unprepared");
        assert!(!status(&local, None).installed);
        fs::remove_dir_all(local).unwrap();
    }
}
