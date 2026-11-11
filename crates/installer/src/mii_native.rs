//! Verified, opt-in static heads selected at game launch. Does not enable the SDK Mii renderer.
use crate::{mii, mii_composer};
use mk8_manifest::{hash_file, load, save, verify, FileIdentity, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
    process::Command,
};

const ORIGINAL: &str = "d2d4f0992ab12c0773e0a89add358576c2ceccff0450deb5053740f73c57568a";
const ORIGINAL_FEMALE: &str = "8fa6b46908450bb05d24a2a4607fdd205e51ee00f956abd33ea80368b5a4dfc8";

/// Normal Suit body resource selected by CharInfo gender (byte 0x28).
/// Both verified bodies share the skeleton and the `Head` bone at index 10.
/// With `MK8_EXPERIMENTAL_NO_MII` the game loads `Driver/MiiM.szs` for every Mii, so the
/// selected body is always served under that name (a separate `MiiF.szs` is never read).
const OVERLAY_NAME: &str = "MiiM.szs";

fn body(charinfo: &[u8]) -> Result<(&'static str, &'static str)> {
    match charinfo.get(0x28) {
        Some(0) => Ok(("MiiM", ORIGINAL)),
        Some(1) => Ok(("MiiF", ORIGINAL_FEMALE)),
        _ => Err("Gênero Mii inválido para a conversão nativa.".into()),
    }
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Tools {
    pub python: PathBuf,
    pub libraries: PathBuf,
    pub original: PathBuf,
    /// Verified MiiF.bfres for female Miis. Optional so older configurations stay valid.
    #[serde(default)]
    pub original_female: Option<PathBuf>,
    pub builder: PathBuf,
}

impl Tools {
    fn original_for(&self, body: &str) -> Option<&Path> {
        match body {
            "MiiF" => self.original_female.as_deref(),
            _ => Some(&self.original),
        }
    }
}

pub fn tools(root: &Path, local: &Path) -> Result<Tools> {
    let config = local.join("mii-native-tools.json");
    if config.exists() {
        return load(&config);
    }
    Ok(Tools {
        python: root.join("local/python/Scripts/python.exe"),
        libraries: root.join("local/mii-library-research/python-libs"),
        original: root.join("local/mii-native-probe/MiiM.bfres"),
        original_female: Some(root.join("local/mii-native-probe/MiiF.bfres")),
        builder: root.join("local/mii-native-probe/MiiHeadBuilder.exe"),
    })
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Receipt {
    pub schema: u8,
    pub enabled: bool,
    pub charinfo: Vec<u8>,
    pub glb_sha256: String,
    pub overlay: FileIdentity,
}

fn profile_dir(local: &Path) -> Result<PathBuf> {
    let profile = mii::read(local)?.ok_or("Selecione um perfil Mii primeiro.")?;
    Ok(local.join("profiles").join(mii::id_text(&profile)))
}

fn read(local: &Path) -> Result<Option<Receipt>> {
    if mii::read(local)?.is_none() {
        return Ok(None);
    }
    let path = profile_dir(local)?.join("native-head.json");
    if path.exists() {
        load(&path).map(Some)
    } else {
        Ok(None)
    }
}

fn validate(local: &Path, receipt: &Receipt) -> Result<PathBuf> {
    let head = mii_composer::saved(local)?.ok_or(
        "Modelo Mii desatualizado. Salve e prepare novamente ou desative seu uso no jogo.",
    )?;
    if receipt.schema != 1 || receipt.charinfo != head.charinfo || receipt.glb_sha256 != head.sha256
    {
        return Err(
            "Modelo Mii desatualizado. Prepare novamente ou desative seu uso no jogo.".into(),
        );
    }
    // Resolve before checking containment, including junctions and symlinks.
    let base = profile_dir(local)?
        .join("native")
        .canonicalize()
        .map_err(|e| e.to_string())?;
    let file = receipt
        .overlay
        .path
        .canonicalize()
        .map_err(|e| e.to_string())?;
    body(&receipt.charinfo)?;
    if !file.starts_with(&base) || file.file_name().is_none_or(|n| n != OVERLAY_NAME) {
        return Err("Sobreposição Mii fora da pasta do perfil.".into());
    }
    if !(32..=5 * 1024 * 1024).contains(&receipt.overlay.bytes) {
        return Err("Tamanho inválido da sobreposição Mii.".into());
    }
    verify(&receipt.overlay)?;
    let bytes = fs::read(&file).map_err(|e| e.to_string())?;
    if bytes.len() < 16
        || &bytes[..4] != b"Yaz0"
        || !(16..=4 * 1024 * 1024).contains(&u32::from_be_bytes(bytes[4..8].try_into().unwrap()))
    {
        return Err("Sobreposição Mii inválida.".into());
    }
    let driver = file.parent().ok_or("Pasta Driver ausente")?;
    let romfs = driver.parent().ok_or("Pasta romfs ausente")?;
    if driver.file_name().is_none_or(|n| n != "Driver")
        || romfs.file_name().is_none_or(|n| n != "romfs")
    {
        return Err("Estrutura inválida da sobreposição Mii.".into());
    }
    for directory in [driver, romfs] {
        let entries = fs::read_dir(directory)
            .map_err(|e| e.to_string())?
            .collect::<std::io::Result<Vec<_>>>()
            .map_err(|e| e.to_string())?;
        if entries.len() != 1 {
            return Err("Arquivos adicionais na sobreposição Mii. Prepare novamente.".into());
        }
    }
    let directory = romfs.parent().ok_or("Pasta da sobreposição ausente")?;
    Ok(crate::mii_resources::runtime_path(directory))
}

pub fn status(root: &Path, local: &Path) -> Result<Value> {
    let t = tools(root, local)?;
    // Readiness follows the selected profile's body; a male-only setup keeps working.
    let selected = match mii::read(local)? {
        Some(profile) => body(&profile.data()?)?.0,
        None => "MiiM",
    };
    let available = t.python.is_file()
        && t.original_for(selected).is_some_and(Path::is_file)
        && t.builder.is_file()
        && t.libraries.join("PIL/__init__.py").is_file()
        && root.join("toolchain/mii_native_head.py").is_file();
    let receipt = read(local)?;
    let checked = receipt.as_ref().map(|r| validate(local, r));
    Ok(
        json!({"tools_ready":available, "body":selected, "enabled":receipt.as_ref().is_some_and(|r|r.enabled),
        "ready":checked.as_ref().is_some_and(|r|r.is_ok()), "error":checked.and_then(|r|r.err())}),
    )
}

pub fn set_enabled(local: &Path, enabled: bool) -> Result<()> {
    let file = profile_dir(local)?.join("native-head.json");
    if !file.is_file() {
        return Err("Prepare o modelo Mii primeiro.".into());
    }
    let mut receipt: Receipt = load(&file)?;
    if enabled {
        validate(local, &receipt)?;
    }
    receipt.enabled = enabled;
    save(&file, &receipt)
}

pub fn check_profile(local: &Path, expected: &[u8]) -> Result<()> {
    if mii::read(local)?
        .ok_or("Selecione um perfil Mii primeiro.")?
        .data()?
        != expected
    {
        return Err("O perfil mudou. Reabra o editor antes de salvar.".into());
    }
    Ok(())
}

pub fn launch_directory(local: &Path, online: bool) -> Result<Option<PathBuf>> {
    let Some(receipt) = read(local)? else {
        return Ok(None);
    };
    if !receipt.enabled {
        return Ok(None);
    }
    if online {
        return Err(
            "Desative a cabeça Mii experimental para jogar em uma sala multiplayer.".into(),
        );
    }
    validate(local, &receipt).map(Some)
}

pub fn configure_command(cmd: &mut Command, directory: Option<&Path>) -> Result<()> {
    if let Some(directory) = directory {
        if std::env::var_os("RECOMP_MOD_DIR").is_some_and(|p| !p.is_empty()) {
            return Err("RECOMP_MOD_DIR já define outra sobreposição. Remova essa configuração antes de usar o Mii do perfil.".into());
        }
        cmd.env("RECOMP_MOD_DIR", directory);
    }
    Ok(())
}

pub struct Pending {
    pub output: PathBuf,
    pub log: PathBuf,
    charinfo: Vec<u8>,
    sha256: String,
    inputs: Vec<FileIdentity>,
    profile_dir: PathBuf,
}

pub fn prepare(root: &Path, local: &Path) -> Result<(Command, Pending)> {
    let t = tools(root, local)?;
    if !status(root, local)?["tools_ready"]
        .as_bool()
        .unwrap_or(false)
    {
        return Err("Ferramentas locais de conversão indisponíveis. Configure mii-native-tools.json conforme STATUS.md.".into());
    }
    let head = mii_composer::saved(local)?
        .ok_or("Salve o perfil e o modelo 3D antes de preparar para o jogo.")?;
    let (name, expected) = body(&head.charinfo)?;
    let path = t
        .original_for(name)
        .ok_or_else(|| format!("Configure o {name}.bfres original em mii-native-tools.json."))?;
    let original = hash_file(path)?;
    if original.sha256 != expected {
        return Err(format!("{name}.bfres original incompatível com MK8 4.0.1."));
    }
    let directory = profile_dir(local)?;
    let source = directory.join(format!("assembled-{}.glb", head.sha256));
    let script = root.join("toolchain/mii_native_head.py");
    let inputs = vec![
        original,
        hash_file(&t.builder)?,
        hash_file(&source)?,
        hash_file(&script)?,
        hash_file(&root.join("toolchain/mii_head_bake.py"))?,
    ];
    let work = directory
        .join("native")
        .join(format!("{:016x}", rand::random::<u64>()));
    fs::create_dir_all(&work).map_err(|e| e.to_string())?;
    let output = work.join("model");
    let mut cmd = Command::new(&t.python);
    cmd.arg(script)
        .arg(source)
        .arg(&output)
        .arg("--original")
        .arg(path)
        .arg("--builder")
        .arg(&t.builder)
        .arg("--python-libs")
        .arg(&t.libraries)
        .current_dir(root);
    Ok((
        cmd,
        Pending {
            output,
            log: work.join("conversion.log"),
            charinfo: head.charinfo,
            sha256: head.sha256,
            inputs,
            profile_dir: directory,
        },
    ))
}

pub fn publish(local: &Path, pending: Pending) -> Result<()> {
    for file in &pending.inputs {
        verify(file)?;
    }
    let head = mii_composer::saved(local)?
        .ok_or("O perfil mudou durante a conversão. Prepare novamente.")?;
    if head.charinfo != pending.charinfo || head.sha256 != pending.sha256 {
        return Err("O perfil mudou durante a conversão. A sobreposição não foi ativada.".into());
    }
    let (name, original) = body(&head.charinfo)?;
    let manifest: Value = load(&pending.output.join("manifest.json"))?;
    let overlay = hash_file(&pending.output.join("romfs/Driver").join(OVERLAY_NAME))?;
    if manifest["schema"] != 1
        || manifest["body"] != name
        || manifest["overlay_file"] != OVERLAY_NAME
        || manifest["charinfo"] != json!(head.charinfo)
        || manifest["glb_sha256"] != head.sha256
        || manifest["overlay_sha256"] != overlay.sha256
        || manifest["original_bfres_sha256"] != original
        || manifest["builder_sha256"] != pending.inputs[1].sha256
        || manifest["gameplay_verified"] != false
        || manifest["multiplayer_verified"] != false
    {
        return Err("A conversão não produziu um manifesto válido para este perfil.".into());
    }
    let receipt = Receipt {
        schema: 1,
        enabled: true,
        charinfo: head.charinfo,
        glb_sha256: head.sha256,
        overlay,
    };
    validate(local, &receipt)?;
    save(&pending.profile_dir.join("native-head.json"), &receipt)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture(local: &Path) -> Receipt {
        fixture_body(local, 0, "MiiM.szs")
    }

    fn fixture_body(local: &Path, gender: u8, overlay_name: &str) -> Receipt {
        let profile = mii::create(
            local,
            mii::Appearance {
                name: "Native".into(),
                color: 0,
                gender,
                face: Default::default(),
            },
        )
        .unwrap();
        let data = profile.data().unwrap();
        let doc = json!({"asset":{"version":"2.0","extras":{"mii_charinfo":data}},"buffers":[{"byteLength":4}],"bufferViews":[{"buffer":0,"byteLength":4}],"images":[{"bufferView":0,"mimeType":"image/png"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}]});
        let mut json = serde_json::to_vec(&doc).unwrap();
        while !json.len().is_multiple_of(4) {
            json.push(b' ');
        }
        let mut bytes = b"glTF".to_vec();
        for n in [2, (json.len() + 32) as u32, json.len() as u32] {
            bytes.extend(n.to_le_bytes());
        }
        bytes.extend(b"JSON");
        bytes.extend(json);
        bytes.extend(4u32.to_le_bytes());
        bytes.extend(b"BIN\0");
        bytes.extend([0; 4]);
        let directory = profile_dir(local).unwrap();
        let temp = directory.join("fixture.glb");
        fs::write(&temp, &bytes).unwrap();
        let hash = hash_file(&temp).unwrap().sha256;
        fs::rename(temp, directory.join(format!("assembled-{hash}.glb"))).unwrap();
        save(
            &directory.join("assembled-head.json"),
            &mii_composer::Head {
                schema: 1,
                charinfo: data.clone(),
                sha256: hash.clone(),
                resource_sha256: mii_composer::RESOURCE_SHA256.into(),
                native_integration_verified: false,
            },
        )
        .unwrap();
        let path = directory.join(format!("native/test/model/romfs/Driver/{overlay_name}"));
        fs::create_dir_all(path.parent().unwrap()).unwrap();
        // Synthetic envelopes only exercise selection/integrity. The browser/native test uses actual assets.
        let mut overlay = b"Yaz0".to_vec();
        overlay.extend(16u32.to_be_bytes());
        overlay.extend([0; 24]);
        fs::write(&path, overlay).unwrap();
        let receipt = Receipt {
            schema: 1,
            enabled: true,
            charinfo: data,
            glb_sha256: hash,
            overlay: hash_file(&path).unwrap(),
        };
        save(&directory.join("native-head.json"), &receipt).unwrap();
        receipt
    }

    #[test]
    fn selects_only_current_profile_and_rejects_stale_tampered_or_online_heads() {
        let root = std::env::temp_dir().join(format!("mk8-native-head-{}", rand::random::<u64>()));
        fs::create_dir_all(&root).unwrap();
        let receipt = fixture(&root);
        check_profile(&root, &receipt.charinfo).unwrap();
        assert!(check_profile(&root, &[]).is_err());
        let selected = launch_directory(&root, false).unwrap().unwrap();
        assert!(selected.join("romfs/Driver/MiiM.szs").is_file());
        let extra = selected.join("romfs/Driver/extra.szs");
        fs::write(&extra, b"unexpected overlay").unwrap();
        assert!(launch_directory(&root, false).is_err());
        fs::remove_file(extra).unwrap();
        let mut command = Command::new("fixture");
        configure_command(&mut command, Some(&selected)).unwrap();
        assert_eq!(
            command
                .get_envs()
                .find(|(k, _)| *k == "RECOMP_MOD_DIR")
                .unwrap()
                .1,
            Some(selected.as_os_str())
        );
        assert!(launch_directory(&root, true).is_err());
        let mut data = receipt.charinfo.clone();
        data[39] = 1;
        mii::update_data(&root, &data).unwrap();
        assert!(launch_directory(&root, false).is_err());
        set_enabled(&root, false).unwrap();
        assert!(launch_directory(&root, false).unwrap().is_none());
        assert!(set_enabled(&root, true).is_err());
        mii::update_data(&root, &receipt.charinfo).unwrap();
        set_enabled(&root, true).unwrap();
        fs::write(&receipt.overlay.path, b"changed").unwrap();
        assert!(launch_directory(&root, false).is_err());
        mii::create(
            &root,
            mii::Appearance {
                name: "Other".into(),
                color: 0,
                gender: 0,
                face: Default::default(),
            },
        )
        .unwrap();
        assert!(launch_directory(&root, false).unwrap().is_none());
        fs::remove_dir_all(root).unwrap();
    }

    #[test]
    fn female_profiles_use_miif_body_served_as_the_loaded_miim_file() {
        let root =
            std::env::temp_dir().join(format!("mk8-native-head-f-{}", rand::random::<u64>()));
        fs::create_dir_all(&root).unwrap();
        let receipt = fixture_body(&root, 1, "MiiM.szs");
        assert_eq!(body(&receipt.charinfo).unwrap(), ("MiiF", ORIGINAL_FEMALE));
        let selected = launch_directory(&root, false).unwrap().unwrap();
        assert!(selected.join("romfs/Driver/MiiM.szs").is_file());
        assert_eq!(status(&root, &root).unwrap()["body"], "MiiF");
        fs::remove_dir_all(&root).unwrap();

        // The game never reads MiiF.szs, so that name is not accepted as an active overlay.
        fs::create_dir_all(&root).unwrap();
        fixture_body(&root, 1, "MiiF.szs");
        assert!(launch_directory(&root, false).is_err());
        fs::remove_dir_all(&root).unwrap();
        assert!(body(&[0; 0x28]).is_err());
        let mut other = vec![0; 88];
        other[0x28] = 2;
        assert!(body(&other).is_err());
    }
}
