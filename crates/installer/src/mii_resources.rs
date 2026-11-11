//! Opt-in mounting of a MiiModel-layout folder: the player's extracted system archive, or files
//! derived by `toolchain/mii_resource_nx.py` from the verified local AFL. Never manufactures assets.
use crate::{mii_composer, mii_native};
use mk8_manifest::{hash_file, load, save, verify, FileIdentity, Result};
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

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

/// A derivation job: the converter writes `output`; `finish` validates it before enabling.
pub struct Generation {
    pub output: PathBuf,
    pub log: PathBuf,
}

/// Command deriving the six files from the editor's verified AFL into a new private folder.
pub fn generate(root: &Path, local: &Path) -> Result<(Command, Generation)> {
    // Re-checks size and SHA-256 of the configured AFL before any output exists.
    mii_composer::resource(local)?;
    let afl = mii_composer::resource_config(local)?
        .ok_or("Selecione o arquivo AFL local no editor Mii.")?
        .path;
    let python = mii_native::tools(root, local)?.python;
    let script = root.join("toolchain/mii_resource_nx.py");
    if !python.is_file() || !script.is_file() {
        return Err("Python local ou conversor de recursos Mii indisponível.".into());
    }
    let work = local
        .join("mii-resources-generated")
        .join(format!("{:016x}", rand::random::<u64>()));
    fs::create_dir_all(&work).map_err(|e| e.to_string())?;
    let output = work.join("files");
    let mut cmd = Command::new(python);
    cmd.arg(script).arg(afl).arg(&output).current_dir(root);
    Ok((
        cmd,
        Generation {
            output,
            log: work.join("generation.log"),
        },
    ))
}

/// Accepts only a complete converter output for the verified AFL, then enables it.
pub fn finish(local: &Path, generation: &Generation) -> Result<Resources> {
    let manifest: Value = load(&generation.output.join("manifest.json"))?;
    if manifest["schema"] != 1
        || manifest["afl_sha256"] != mii_composer::RESOURCE_SHA256
        || manifest["rendering_verified"] != false
        || FILES
            .iter()
            .any(|name| manifest["files"][*name].as_str().is_none())
    {
        return Err("A geração não produziu um manifesto válido de recursos Mii.".into());
    }
    for name in FILES {
        if hash_file(&generation.output.join(name))?.sha256 != manifest["files"][name] {
            return Err(format!("Recurso Mii gerado diverge do manifesto: {name}"));
        }
    }
    configure(local, &generation.output, true)
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
    fn generated_output_is_enabled_only_with_a_consistent_manifest() {
        let root =
            std::env::temp_dir().join(format!("mk8-mii-generated-{}", rand::random::<u64>()));
        let output = root.join("generated/files");
        std::fs::create_dir_all(&output).unwrap();
        // Without a configured AFL nothing is derived.
        assert!(generate(&root, &root).is_err());
        let mut files = serde_json::Map::new();
        for name in FILES {
            // Synthetic envelopes exercise manifest/hash handling only, never rendering validity.
            let mut data = vec![0u8; 0x15000];
            data[12] = 1;
            std::fs::write(output.join(name), &data).unwrap();
            files.insert(
                name.into(),
                hash_file(&output.join(name)).unwrap().sha256.into(),
            );
        }
        let generation = Generation {
            output: output.clone(),
            log: root.join("generation.log"),
        };
        let manifest = |afl: &str, files: &serde_json::Map<String, Value>| serde_json::json!({"schema":1,"afl_sha256":afl,"files":files,"rendering_verified":false});
        save(&output.join("manifest.json"), &manifest("other", &files)).unwrap();
        assert!(finish(&root, &generation).is_err());
        let mut incomplete = files.clone();
        incomplete.remove(FILES[0]);
        save(
            &output.join("manifest.json"),
            &manifest(mii_composer::RESOURCE_SHA256, &incomplete),
        )
        .unwrap();
        assert!(finish(&root, &generation).is_err());
        save(
            &output.join("manifest.json"),
            &manifest(mii_composer::RESOURCE_SHA256, &files),
        )
        .unwrap();
        std::fs::write(output.join(FILES[1]), vec![1u8; 0x15000]).unwrap();
        assert!(finish(&root, &generation).is_err());
        assert!(read(&root).unwrap().is_none());
        let mut data = vec![0u8; 0x15000];
        data[12] = 1;
        std::fs::write(output.join(FILES[1]), data).unwrap();
        let enabled = finish(&root, &generation).unwrap();
        assert!(enabled.enabled);
        assert!(launch_directory(&root).unwrap().is_some());
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
