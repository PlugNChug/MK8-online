//! Private shared-font imports. Structural checks do not establish Nintendo provenance.
use mk8_manifest::{hash_file, load, save, verify, FileIdentity, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

const NAMES: [(&str, &str); 6] = [
    ("nintendo_udsg-r_std_003", "FontStandard"),
    ("nintendo_udsg-r_org_zh-cn_003", "FontChineseSimplified"),
    (
        "nintendo_udsg-r_ext_zh-cn_003",
        "FontExtendedChineseSimplified",
    ),
    ("nintendo_udjxh-db_zh-tw_003", "FontChineseTraditional"),
    ("nintendo_udsg-r_ko_003", "FontKorean"),
    ("nintendo_ext_003", "FontNintendoExtended"),
];
#[derive(Default, Serialize, Deserialize)]
struct Selection {
    enabled: bool,
    files: Vec<FileIdentity>,
}

fn plain(path: &Path) -> Result<()> {
    for parent in path.ancestors() {
        let meta = match fs::symlink_metadata(parent) {
            Ok(meta) => meta,
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => continue,
            Err(error) => return Err(error.to_string()),
        };
        {
            #[cfg(windows)]
            let linked = {
                use std::os::windows::fs::MetadataExt;
                meta.file_attributes() & 0x400 != 0
            };
            #[cfg(not(windows))]
            let linked = meta.file_type().is_symlink();
            if linked {
                return Err("Fontes: caminho com link recusado.".into());
            }
        }
    }
    Ok(())
}
fn read(local: &Path) -> Result<Selection> {
    let path = local.join("font-library.json");
    plain(&path)?;
    if !path.exists() {
        return Ok(Selection::default());
    }
    if fs::metadata(&path).map_err(|e| e.to_string())?.len() > 65536 {
        return Err("Manifesto de fontes grande demais.".into());
    }
    load(&path)
}
fn validate_selection(local: &Path, selection: &Selection) -> Result<PathBuf> {
    if selection.files.len() > NAMES.len() {
        return Err("Manifesto de fontes inválido.".into());
    }
    let root = local.join("font-library");
    plain(&root)?;
    let root = fs::canonicalize(root).map_err(|e| e.to_string())?;
    let mut directory = None;
    for file in &selection.files {
        plain(&file.path)?;
        let path = fs::canonicalize(&file.path).map_err(|e| e.to_string())?;
        let parent = path.parent().ok_or("Pasta de fontes inválida.")?;
        if parent.parent() != Some(root.as_path())
            || directory.as_deref().is_some_and(|d| d != parent)
        {
            return Err("Pasta de fontes inválida.".into());
        }
        verify(file)?;
        directory = Some(parent.to_path_buf());
    }
    let directory = directory.ok_or("Importe as fontes primeiro.")?;
    if directory.to_string_lossy().len() > 3900 {
        return Err("Caminho de fontes longo demais.".into());
    }
    if !selection.files.iter().any(|f| {
        f.path
            .file_name()
            .is_some_and(|n| n == "nintendo_udsg-r_std_003.ttf")
    }) {
        return Err("Fonte padrão do console ausente.".into());
    }
    Ok(directory)
}
pub fn status(local: &Path) -> Result<Value> {
    let selection = read(local)?;
    Ok(
        json!({"enabled":selection.enabled,"imported":!selection.files.is_empty(),"count":selection.files.len()}),
    )
}
pub fn select(local: &Path, enabled: bool) -> Result<Value> {
    let mut selection = read(local)?;
    if enabled {
        validate_selection(local, &selection)?;
    }
    selection.enabled = enabled;
    save(&local.join("font-library.json"), &selection)?;
    status(local)
}
fn decode(mut bytes: Vec<u8>, encoded: bool) -> Result<Vec<u8>> {
    if encoded {
        let mut decoded = None;
        for key in [0x49621806u32, 0x06186249] {
            if bytes.len() < 12 {
                break;
            }
            let size = u32::from_le_bytes(bytes[4..8].try_into().unwrap()) ^ key;
            let size = size as usize;
            if size < 12 || size > bytes.len() - 8 || !size.is_multiple_of(4) {
                continue;
            }
            let mut out = Vec::with_capacity(size);
            for word in bytes[8..8 + size].as_chunks::<4>().0 {
                out.extend_from_slice(&(u32::from_le_bytes(*word) ^ key).to_le_bytes());
            }
            if out.starts_with(&[0, 1, 0, 0]) {
                decoded = Some(out);
                break;
            }
        }
        bytes = decoded.ok_or("Arquivo BFTTF inválido.")?;
    }
    if bytes.len() < 12 || !bytes.starts_with(&[0, 1, 0, 0]) {
        return Err("Selecione fontes TrueType do console (.ttf ou .bfttf).".into());
    }
    let count = u16::from_be_bytes([bytes[4], bytes[5]]) as usize;
    if count == 0 || count > 128 || 12 + count * 16 > bytes.len() {
        return Err("Diretório de fonte inválido.".into());
    }
    let mut tags = std::collections::BTreeSet::new();
    for table in bytes[12..12 + count * 16].as_chunks::<16>().0 {
        let offset = u32::from_be_bytes(table[8..12].try_into().unwrap()) as usize;
        let size = u32::from_be_bytes(table[12..16].try_into().unwrap()) as usize;
        if !tags.insert(table[..4].to_vec())
            || offset < 12 + count * 16
            || size > bytes.len()
            || offset > bytes.len() - size
        {
            return Err("Tabela de fonte inválida.".into());
        }
    }
    for tag in [
        b"cmap", b"glyf", b"head", b"hhea", b"hmtx", b"loca", b"maxp",
    ] {
        if !tags.contains(tag.as_slice()) {
            return Err("Tabelas TrueType obrigatórias ausentes.".into());
        }
    }
    Ok(bytes)
}
pub fn import(local: &Path, source: &Path) -> Result<Value> {
    plain(source)?;
    if !source.is_dir() {
        return Err("Escolha a pasta das fontes do console.".into());
    }
    let mut validated = Vec::new();
    for (name, alias) in NAMES {
        let mut found = None;
        for base in [name, alias] {
            for ext in ["ttf", "bfttf"] {
                let path = source.join(format!("{base}.{ext}"));
                if path.exists() {
                    found = Some((path, ext == "bfttf"));
                    break;
                }
            }
            if found.is_some() {
                break;
            }
        }
        if let Some((path, encoded)) = found {
            plain(&path)?;
            let meta = fs::metadata(&path).map_err(|e| e.to_string())?;
            if !meta.is_file() || meta.len() > 32 * 1024 * 1024 {
                return Err("Arquivo de fonte inválido ou grande demais.".into());
            }
            let mut bytes = Vec::new();
            fs::File::open(path)
                .map_err(|e| e.to_string())?
                .take(32 * 1024 * 1024 + 1)
                .read_to_end(&mut bytes)
                .map_err(|e| e.to_string())?;
            if bytes.len() > 32 * 1024 * 1024 {
                return Err("Arquivo de fonte grande demais.".into());
            }
            validated.push((name, decode(bytes, encoded)?));
        }
    }
    if !validated.iter().any(|(name, _)| *name == NAMES[0].0) {
        return Err(
            "Fonte padrão ausente: FontStandard.ttf ou nintendo_udsg-r_std_003.ttf/.bfttf.".into(),
        );
    }
    let library = local.join("font-library");
    plain(&library)?;
    fs::create_dir_all(&library).map_err(|e| e.to_string())?;
    let id = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let directory = library.join(format!("{}-{id}", std::process::id()));
    fs::create_dir(&directory).map_err(|e| e.to_string())?;
    let mut selection = Selection {
        enabled: true,
        files: Vec::new(),
    };
    for (name, bytes) in validated {
        let path = directory.join(format!("{name}.ttf"));
        fs::write(&path, bytes).map_err(|e| e.to_string())?;
        selection.files.push(hash_file(
            &fs::canonicalize(path).map_err(|e| e.to_string())?,
        )?);
    }
    save(&local.join("font-library.json"), &selection)?;
    status(local)
}
pub fn configure(local: &Path) -> Result<()> {
    std::env::remove_var("MK8_FONT_MODE");
    std::env::remove_var("MK8_FONT_DIR");
    let selection = read(local)?;
    // An untouched installation retains the existing font/environment behavior.
    if !local.join("font-library.json").exists() {
        return Ok(());
    }
    std::env::set_var(
        "MK8_FONT_MODE",
        if selection.enabled {
            "console"
        } else {
            "system"
        },
    );
    if selection.enabled {
        std::env::set_var("MK8_FONT_DIR", validate_selection(local, &selection)?);
    } else {
        std::env::remove_var("MK8_FONT_DIR");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> Vec<u8> {
        let mut data = vec![0; 12 + 7 * 16 + 7 * 4];
        data[..4].copy_from_slice(&[0, 1, 0, 0]);
        data[4..6].copy_from_slice(&7u16.to_be_bytes());
        for (i, tag) in [
            b"cmap", b"glyf", b"head", b"hhea", b"hmtx", b"loca", b"maxp",
        ]
        .iter()
        .enumerate()
        {
            let at = 12 + i * 16;
            data[at..at + 4].copy_from_slice(*tag);
            data[at + 8..at + 12].copy_from_slice(&((12 + 7 * 16 + i * 4) as u32).to_be_bytes());
            data[at + 12..at + 16].copy_from_slice(&4u32.to_be_bytes());
        }
        data
    }
    #[test]
    fn imports_aliases_retains_files_and_rejects_bad_replacements_and_tampering() {
        let root = std::env::temp_dir().join(format!("mk8-font-import-{}", std::process::id()));
        fs::create_dir_all(root.join("source")).unwrap();
        let source = root.join("source");
        let local = root.join("data");
        fs::write(source.join("FontStandard.ttf"), fixture()).unwrap();
        fs::write(source.join("FontNintendoExtended.ttf"), fixture()).unwrap();
        assert_eq!(import(&local, &source).unwrap()["count"], 2);
        let before = fs::read(local.join("font-library.json")).unwrap();
        let state = read(&local).unwrap();
        let directory = validate_selection(&local, &state).unwrap();
        assert!(directory.join("nintendo_ext_003.ttf").is_file());
        fs::write(source.join("FontNintendoExtended.ttf"), b"bad").unwrap();
        assert!(import(&local, &source).is_err());
        assert_eq!(fs::read(local.join("font-library.json")).unwrap(), before);
        select(&local, false).unwrap();
        assert!(!read(&local).unwrap().enabled);
        assert!(directory.join("nintendo_ext_003.ttf").is_file());
        select(&local, true).unwrap();
        fs::write(directory.join("nintendo_ext_003.ttf"), b"tampered").unwrap();
        assert!(select(&local, true).is_err());
        select(&local, false).unwrap(); // recovery remains possible
        fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn bfttf_decoding_and_table_bounds_are_checked() {
        let raw = fixture();
        for key in [0x49621806u32, 0x06186249] {
            let mut encoded = vec![0; 4];
            encoded.extend_from_slice(&((raw.len() as u32) ^ key).to_le_bytes());
            for word in raw.as_chunks::<4>().0 {
                encoded.extend_from_slice(&(u32::from_le_bytes(*word) ^ key).to_le_bytes());
            }
            assert_eq!(decode(encoded, true).unwrap(), raw);
        }
        assert!(decode(vec![0; 8], true).is_err());
        assert!(decode(b"ttcf".to_vec(), false).is_err());
        let mut bad = raw.clone();
        bad[20..24].copy_from_slice(&u32::MAX.to_be_bytes());
        assert!(decode(bad, false).is_err());
        assert!(decode(raw[..20].to_vec(), false).is_err());
    }
}
