//! Opt-in mounting of the player's extracted MiiModel system archive. Never manufactures assets.
use mk8_manifest::{hash_file, load, save, verify, FileIdentity, Result};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

const FILES: [&str; 6] = [
    "ShapeMid.dat",
    "ShapeHigh.dat",
    "NXTextureLowLinear.dat",
    "NXTextureLowSRGB.dat",
    "NXTextureMidLinear.dat",
    "NXTextureMidSRGB.dat",
];

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Resources {
    pub directory: PathBuf,
    pub enabled: bool,
    pub files: Vec<FileIdentity>,
}

pub fn read(local: &Path) -> Result<Option<Resources>> {
    let path = local.join("mii-resources.json");
    if path.exists() {
        load(&path).map(Some)
    } else {
        Ok(None)
    }
}

pub fn configure(local: &Path, directory: &Path, enabled: bool) -> Result<Resources> {
    if !enabled {
        let value = Resources {
            directory: directory.to_path_buf(),
            enabled: false,
            files: vec![],
        };
        save(&local.join("mii-resources.json"), &value)?;
        return Ok(value);
    }
    let directory = directory
        .canonicalize()
        .map_err(|e| format!("Pasta MiiModel indisponível: {e}"))?;
    let mut files = Vec::new();
    for name in FILES {
        let path = directory.join(name);
        let identity = hash_file(&path)?;
        // Presence checks are not SDK/GPU qualification. Reject empty/truncated resources and zero-filled stand-ins.
        let shape = name.starts_with("Shape");
        let minimum = if shape { 0x14dd4 } else { 0x64 };
        if identity.bytes <= minimum || identity.bytes > 256 * 1024 * 1024 {
            return Err(format!(
                "Recurso Mii ausente, truncado ou grande demais: {name}"
            ));
        }
        let bytes = std::fs::read(&path).map_err(|e| e.to_string())?;
        let kinds = if shape { 12 } else { 11 };
        if bytes[12..12 + kinds * 4]
            .as_chunks::<4>()
            .0
            .iter()
            .all(|b| *b == [0, 0, 0, 0])
        {
            return Err(format!("Recurso Mii sem dados de partes: {name}"));
        }
        files.push(identity);
    }
    let value = Resources {
        directory,
        enabled,
        files,
    };
    save(&local.join("mii-resources.json"), &value)?;
    Ok(value)
}

pub fn launch_directory(local: &Path) -> Result<Option<PathBuf>> {
    let Some(value) = read(local)? else {
        return Ok(None);
    };
    if !value.enabled {
        return Ok(None);
    }
    if value.files.len() != FILES.len() {
        return Err("Manifesto Mii incompleto; configure os recursos novamente.".into());
    }
    for (name, file) in FILES.into_iter().zip(value.files.iter()) {
        if value.directory.join(name).canonicalize().ok().as_ref() != Some(&file.path) {
            return Err("Caminho de recurso Mii alterado.".into());
        }
        verify(file)?;
    }
    Ok(Some(runtime_path(&value.directory)))
}

/// The C runtime appends slash-separated names to these folders. Windows verbatim prefixes
/// produced by `canonicalize` reject forward slashes, so pass the plain equivalent.
pub(crate) fn runtime_path(path: &Path) -> PathBuf {
    #[cfg(windows)]
    if let Some(text) = path.to_str() {
        if let Some(unc) = text.strip_prefix(r"\\?\UNC\") {
            return PathBuf::from(format!(r"\\{unc}"));
        }
        if let Some(disk) = text.strip_prefix(r"\\?\") {
            return PathBuf::from(disk);
        }
    }
    path.to_path_buf()
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn missing_and_empty_assets_do_not_enable_renderer() {
        let root = std::env::temp_dir().join(format!("mk8-mii-assets-{}", rand::random::<u64>()));
        std::fs::create_dir_all(&root).unwrap();
        assert!(configure(&root, &root, true).is_err());
        std::fs::write(root.join(FILES[0]), vec![0; 0x15000]).unwrap();
        assert!(configure(&root, &root, true).is_err());
        assert!(read(&root).unwrap().is_none());
        configure(&root, &root, false).unwrap();
        assert!(launch_directory(&root).unwrap().is_none());
        std::fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn resource_manifest_detects_changes_before_launch() {
        let root = std::env::temp_dir().join(format!(
            "mk8-mii-resource-manifest-{}",
            rand::random::<u64>()
        ));
        std::fs::create_dir_all(&root).unwrap();
        // Synthetic envelopes test only preflight/hash handling, never rendering or firmware validity.
        for name in FILES {
            let mut data = vec![0u8; 0x15000];
            data[12] = 1;
            std::fs::write(root.join(name), data).unwrap();
        }
        configure(&root, &root, true).unwrap();
        // The runtime appends "/ShapeMid.dat": the folder must not keep a verbatim prefix.
        let selected = launch_directory(&root).unwrap().unwrap();
        assert_eq!(selected, runtime_path(&root.canonicalize().unwrap()));
        assert!(!selected.to_string_lossy().starts_with(r"\\?\"));
        assert!(std::path::Path::new(&format!("{}/ShapeMid.dat", selected.display())).is_file());
        std::fs::write(root.join(FILES[2]), b"changed").unwrap();
        assert!(launch_directory(&root).is_err());
        configure(&root, &root, false).unwrap();
        assert!(launch_directory(&root).unwrap().is_none());
        std::fs::remove_dir_all(root).unwrap();
    }
}
