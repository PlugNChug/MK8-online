//! Local editor resources and derived heads. These files are not mounted by the game.
use crate::mii;
use mk8_manifest::{load, save, Result};
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{fs, path::Path};

pub const RESOURCE_SHA256: &str =
    "4a4be71d75162c20b48720ef89cd3d4e6cd4e8e21bcbc2aed63ee08d96de7722";
pub const MAX_GLB: usize = 2 * 1024 * 1024;

fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Resource {
    pub path: std::path::PathBuf,
}

pub fn resource_config(local: &Path) -> Result<Option<Resource>> {
    let file = local.join("mii-composer.json");
    if file.exists() {
        load(&file).map(Some)
    } else {
        Ok(None)
    }
}

fn checked_resource(path: &Path) -> Result<Vec<u8>> {
    if fs::metadata(path).map_err(|e| e.to_string())?.len() != 4_579_008 {
        return Err(
            "AFL incompatível: selecione AFLResHigh_2_3.dat da biblioteca verificada.".into(),
        );
    }
    let bytes = fs::read(path).map_err(|e| e.to_string())?;
    validate_resource(&bytes)?;
    Ok(bytes)
}

pub fn validate_resource(bytes: &[u8]) -> Result<()> {
    if bytes.len() != 4_579_008 {
        return Err("Tamanho inesperado da biblioteca Mii.".into());
    }
    if digest(bytes) != RESOURCE_SHA256 {
        return Err(
            "AFL alterado ou incompatível; o hash não corresponde à biblioteca verificada.".into(),
        );
    }
    Ok(())
}

pub fn configure(local: &Path, path: &Path) -> Result<Resource> {
    checked_resource(path)?;
    let config = Resource {
        path: path.canonicalize().map_err(|e| e.to_string())?,
    };
    save(&local.join("mii-composer.json"), &config)?;
    Ok(config)
}

pub fn resource(local: &Path) -> Result<Vec<u8>> {
    let config =
        resource_config(local)?.ok_or("Selecione o arquivo AFL local para montar o Mii.")?;
    checked_resource(&config.path)
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Head {
    pub schema: u8,
    pub charinfo: Vec<u8>,
    pub sha256: String,
    pub resource_sha256: String,
    pub native_integration_verified: bool,
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SaveHead {
    pub expected: Vec<u8>,
    pub data: Vec<u8>,
    pub glb: Vec<u8>,
}

fn validate_glb(bytes: &[u8], data: &[u8]) -> Result<()> {
    let fail = || {
        "GLB inválido: use a montagem do editor com texturas e normais incorporadas.".to_string()
    };
    if bytes.len() < 28 || bytes.len() > MAX_GLB || &bytes[..4] != b"glTF" {
        return Err(fail());
    }
    let word = |at| u32::from_le_bytes(bytes[at..at + 4].try_into().unwrap()) as usize;
    if word(4) != 2 || word(8) != bytes.len() || &bytes[16..20] != b"JSON" {
        return Err(fail());
    }
    let end = 20usize
        .checked_add(word(12))
        .filter(|&n| n <= bytes.len() - 8 && n % 4 == 0)
        .ok_or_else(fail)?;
    if &bytes[end + 4..end + 8] != b"BIN\0" || word(end) != bytes.len() - end - 8 {
        return Err(fail());
    }
    let doc: serde_json::Value = serde_json::from_slice(&bytes[20..end]).map_err(|_| fail())?;
    if doc["asset"]["version"] != "2.0"
        || doc["asset"]["extras"]["mii_charinfo"] != serde_json::json!(data)
    {
        return Err(fail());
    }
    let buffers = doc["buffers"].as_array().ok_or_else(fail)?;
    if buffers.len() != 1
        || buffers[0].get("uri").is_some()
        || buffers[0]["byteLength"]
            .as_u64()
            .is_none_or(|n| n == 0 || n > word(end) as u64)
    {
        return Err(fail());
    }
    let views = doc["bufferViews"].as_array().ok_or_else(fail)?;
    for view in views {
        let start = view["byteOffset"].as_u64().unwrap_or(0);
        let length = view["byteLength"].as_u64().ok_or_else(fail)?;
        if view["buffer"] != 0
            || start
                .checked_add(length)
                .is_none_or(|n| n > word(end) as u64)
        {
            return Err(fail());
        }
    }
    let images = doc["images"].as_array().ok_or_else(fail)?;
    if images.is_empty()
        || images.iter().any(|i| {
            i.get("uri").is_some()
                || i["mimeType"] != "image/png"
                || i["bufferView"]
                    .as_u64()
                    .is_none_or(|n| n >= views.len() as u64)
        })
    {
        return Err(fail());
    }
    let meshes = doc["meshes"].as_array().ok_or_else(fail)?;
    if meshes.is_empty() {
        return Err(fail());
    }
    for mesh in meshes {
        let primitives = mesh["primitives"].as_array().ok_or_else(fail)?;
        if primitives.is_empty()
            || primitives.iter().any(|p| {
                p["attributes"]["POSITION"].as_u64().is_none()
                    || p["attributes"]["NORMAL"].as_u64().is_none()
            })
        {
            return Err(fail());
        }
    }
    Ok(())
}

pub fn store(local: &Path, request: SaveHead) -> Result<Head> {
    crate::mii_data::validate(&request.data)?;
    let profile = mii::read(local)?.ok_or("Selecione um perfil primeiro.")?;
    if profile.data()? != request.expected || request.data[..16] != profile.id {
        return Err("O perfil mudou. Reabra o editor antes de salvar.".into());
    }
    resource(local)?;
    validate_glb(&request.glb, &request.data)?;
    let head = Head {
        schema: 1,
        charinfo: request.data.clone(),
        sha256: digest(&request.glb),
        resource_sha256: RESOURCE_SHA256.into(),
        native_integration_verified: false,
    };
    let directory = local.join("profiles").join(mii::id_text(&profile));
    fs::create_dir_all(&directory).map_err(|e| e.to_string())?;
    // Content addressing keeps the previous head intact if saving the profile/receipt fails.
    let file = directory.join(format!("assembled-{}.glb", head.sha256));
    fs::write(file, &request.glb).map_err(|e| e.to_string())?;
    mii::update_data(local, &request.data)?;
    save(&directory.join("assembled-head.json"), &head)?;
    Ok(head)
}

pub fn saved(local: &Path) -> Result<Option<Head>> {
    let Some(profile) = mii::read(local)? else {
        return Ok(None);
    };
    let directory = local.join("profiles").join(mii::id_text(&profile));
    let receipt = directory.join("assembled-head.json");
    if !receipt.exists() {
        return Ok(None);
    }
    let head: Head = load(&receipt)?;
    if head.schema != 1
        || head.charinfo != profile.data()?
        || head.resource_sha256 != RESOURCE_SHA256
        || head.native_integration_verified
        || head.sha256.len() != 64
        || !head.sha256.bytes().all(|b| b.is_ascii_hexdigit())
    {
        return Ok(None);
    }
    let file = directory.join(format!("assembled-{}.glb", head.sha256));
    if fs::metadata(&file).map_err(|e| e.to_string())?.len() > MAX_GLB as u64 {
        return Err("Modelo salvo excede o limite.".into());
    }
    let bytes = fs::read(file).map_err(|e| e.to_string())?;
    if digest(&bytes) != head.sha256 {
        return Err("Modelo salvo foi alterado; gere novamente.".into());
    }
    validate_glb(&bytes, &head.charinfo)?;
    Ok(Some(head))
}

pub fn saved_bytes(local: &Path) -> Result<Vec<u8>> {
    let head = saved(local)?.ok_or("Não há modelo atualizado salvo para este perfil.")?;
    let profile = mii::read(local)?.ok_or("Selecione um perfil primeiro.")?;
    fs::read(
        local
            .join("profiles")
            .join(mii::id_text(&profile))
            .join(format!("assembled-{}.glb", head.sha256)),
    )
    .map_err(|e| e.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn rejects_missing_resources_and_invalid_or_unbound_models() {
        assert!(checked_resource(Path::new("missing-afl.dat")).is_err());
        for data in [vec![], b"glTF".to_vec(), vec![0; 128], vec![0; MAX_GLB + 1]] {
            assert!(validate_glb(&data, &[0; 88]).is_err());
        }
        let doc = serde_json::json!({"asset":{"version":"2.0","extras":{"mii_charinfo":vec![0;88]}},"buffers":[{"byteLength":4}],"bufferViews":[{"buffer":0,"byteLength":4}],"images":[{"bufferView":0,"mimeType":"image/png"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1}}]}]});
        let mut json = serde_json::to_vec(&doc).unwrap();
        while !json.len().is_multiple_of(4) {
            json.push(b' ');
        }
        let mut glb = b"glTF".to_vec();
        for n in [2, (json.len() + 32) as u32, json.len() as u32] {
            glb.extend(n.to_le_bytes());
        }
        glb.extend(b"JSON");
        glb.extend(json);
        glb.extend(4u32.to_le_bytes());
        glb.extend(b"BIN\0");
        glb.extend([0; 4]);
        // Envelope only: browser integration tests exercise real geometry and PNG textures.
        validate_glb(&glb, &[0; 88]).unwrap();
        assert!(validate_glb(&glb, &[1; 88]).is_err());
        glb.pop();
        assert!(validate_glb(&glb, &[0; 88]).is_err());
    }
}
