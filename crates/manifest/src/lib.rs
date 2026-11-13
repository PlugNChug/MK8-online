use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    fs,
    io::Read,
    path::{Path, PathBuf},
};

pub type Result<T> = std::result::Result<T, String>;

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Config {
    pub upstream: String,
    pub exefs: String,
    pub romfs: String,
    pub nacp: String,
    pub base_package: String,
    pub update_package: String,
    pub prod_keys: String,
    pub fonts: String,
    pub mii: String,
    pub shaders: String,
    pub vulkan_sdk: String,
    pub cmake: String,
    pub overlay: bool,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct FileIdentity {
    pub path: PathBuf,
    pub sha256: String,
    pub bytes: u64,
}

#[derive(Clone, Debug, Serialize, Deserialize)]
pub struct Installation {
    pub schema: u32,
    pub identity: String,
    pub profile: String,
    pub executable: FileIdentity,
    pub files: BTreeMap<String, FileIdentity>,
    pub data_root: PathBuf,
    pub shader_root: PathBuf,
    pub gameplay_verified: bool,
    #[serde(default)]
    pub dlc_supported: bool,
}

pub fn hash_file(path: &Path) -> Result<FileIdentity> {
    let path = path
        .canonicalize()
        .map_err(|e| format!("{}: {e}", path.display()))?;
    if !path.is_file() {
        return Err(format!("Não é arquivo: {}", path.display()));
    }
    let mut file = fs::File::open(&path).map_err(|e| e.to_string())?;
    let mut hash = Sha256::new();
    let mut buf = [0u8; 65536];
    let mut bytes = 0;
    loop {
        let n = file.read(&mut buf).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        hash.update(&buf[..n]);
        bytes += n as u64;
    }
    Ok(FileIdentity {
        path,
        sha256: format!("{:x}", hash.finalize()),
        bytes,
    })
}

pub fn verify(file: &FileIdentity) -> Result<()> {
    let actual = hash_file(&file.path)?;
    if actual.sha256 != file.sha256 || actual.bytes != file.bytes {
        return Err(format!(
            "Arquivo alterado: {}. Prepare novamente.",
            file.path.display()
        ));
    }
    Ok(())
}

pub fn load<T: for<'a> Deserialize<'a>>(path: &Path) -> Result<T> {
    serde_json::from_slice(&fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?)
        .map_err(|e| e.to_string())
}

pub fn save<T: Serialize>(path: &Path, value: &T) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| e.to_string())?;
    }
    let bytes = serde_json::to_vec_pretty(value).map_err(|e| e.to_string())?;
    // Preserve the previous bytes if replacement fails on Windows.
    let temp = path.with_extension("json.new");
    fs::write(&temp, bytes).map_err(|e| e.to_string())?;
    fs::rename(&temp, path).map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn detects_mutation_and_replaces_config() {
        let p = std::env::temp_dir().join(format!("mk8-manifest-{}", std::process::id()));
        fs::create_dir_all(&p).unwrap();
        let f = p.join("input");
        fs::write(&f, b"one").unwrap();
        let id = hash_file(&f).unwrap();
        verify(&id).unwrap();
        fs::write(&f, b"two").unwrap();
        assert!(verify(&id).is_err());
        let c = p.join("config.json");
        save(&c, &Config::default()).unwrap();
        save(
            &c,
            &Config {
                overlay: true,
                ..Config::default()
            },
        )
        .unwrap();
        assert!(load::<Config>(&c).unwrap().overlay);
        fs::remove_file(f).unwrap();
        fs::remove_file(c).unwrap();
        fs::remove_dir(p).unwrap();
    }
}
