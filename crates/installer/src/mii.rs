use mk8_manifest::{load, save, Result};
use rand::RngCore;
use serde::{Deserialize, Serialize};
use std::{path::Path, process::Command};

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Appearance {
    pub name: String,
    pub color: u8,
    pub gender: u8,
    #[serde(default)]
    pub face: crate::avatar::Face,
}

impl Appearance {
    pub fn validate(&self) -> Result<()> {
        if self.name.trim().is_empty()
            || self.name.chars().count() > 10
            || self
                .name
                .chars()
                .any(|c| c.is_control() || c as u32 > 0xffff)
            || self.color > 11
            || self.gender > 1
        {
            return Err("Use um nome de 1 a 10 caracteres, cor de 0 a 11 e tipo de 0 a 1.".into());
        }
        self.face.validate()
    }
}

#[derive(Clone, Debug, Serialize, Deserialize, PartialEq)]
#[serde(deny_unknown_fields)]
pub struct Profile {
    pub schema: u8,
    pub id: [u8; 16],
    pub appearance: Appearance,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub charinfo: Option<Vec<u8>>,
}

impl Profile {
    pub fn data(&self) -> Result<Vec<u8>> {
        self.appearance.validate()?;
        let mut data = self.charinfo.clone().unwrap_or_else(crate::mii_data::base);
        if self.charinfo.is_some() {
            crate::mii_data::validate(&data)?;
        }
        data[..16].copy_from_slice(&self.id);
        crate::mii_data::set_name(&mut data, &self.appearance.name);
        data[0x27] = self.appearance.color;
        data[0x28] = self.appearance.gender;
        if self.charinfo.is_none() {
            apply_face(&mut data, &self.appearance.face, None);
        }
        crate::mii_data::validate(&data)?;
        Ok(data)
    }
}

fn apply_face(data: &mut [u8], face: &crate::avatar::Face, old: Option<&crate::avatar::Face>) {
    let values = face.bytes();
    let previous = old.map(|f| f.bytes());
    for (i, (offset, value)) in [
        (0x2e, face.skin),
        (0x2d, face.shape),
        (0x31, [0, 33, 24, 12][face.hair as usize]),
        (0x32, face.hair_color),
        (0x34, [2, 4, 8][face.eyes as usize]),
        (0x35, [8, 10, 11, 12, 9, 0][face.eye_color as usize]),
        (0x4f, face.glasses as u8),
        (0x29, face.height),
        (0x2a, face.build),
    ]
    .into_iter()
    .enumerate()
    {
        if previous.is_none_or(|p| p[i] != values[i]) {
            data[offset] = value;
        }
    }
}

pub fn read(local: &Path) -> Result<Option<Profile>> {
    let path = local.join("mii-profile.json");
    if !path.exists() {
        return Ok(None);
    }
    let profile: Profile = load(&path)?;
    profile.appearance.validate()?;
    if profile.schema != 1 || profile.id[8] & 0xc0 != 0x80 {
        return Err("Perfil Mii local inválido; o arquivo foi preservado.".into());
    }
    profile.data()?;
    Ok(Some(profile))
}

pub fn store(local: &Path, mut appearance: Appearance) -> Result<Profile> {
    appearance.name = appearance.name.trim().to_owned();
    appearance.validate()?;
    let previous = read(local)?;
    let id = if let Some(previous) = &previous {
        previous.id
    } else {
        let mut id = [0; 16];
        rand::thread_rng().fill_bytes(&mut id);
        id[6] = (id[6] & 0x0f) | 0x40;
        id[8] = (id[8] & 0x3f) | 0x80;
        id
    };
    let mut raw = previous.as_ref().and_then(|p| p.charinfo.clone());
    if let (Some(data), Some(old)) = (&mut raw, &previous) {
        apply_face(data, &appearance.face, Some(&old.appearance.face));
    }
    let profile = Profile {
        schema: 1,
        id,
        appearance,
        charinfo: raw,
    };
    persist(local, &profile)?;
    Ok(profile)
}

pub fn id_text(profile: &Profile) -> String {
    profile.id.iter().map(|b| format!("{b:02x}")).collect()
}

fn archive(local: &Path, profile: &Profile) -> Result<()> {
    let dir = local.join("profiles").join(id_text(profile));
    let file = dir.join("profile.json");
    if load::<Profile>(&file).ok().as_ref() != Some(profile) {
        save(&file, profile)?;
    }
    // Derived geometry is regenerated from the authoritative profile, never trusted as profile input.
    let bytes = crate::avatar::glb(&crate::avatar::head(&profile.appearance.face)?)?;
    let model = dir.join("head.glb");
    if std::fs::read(&model).ok().as_ref() != Some(&bytes) {
        let temp = dir.join("head.glb.new");
        std::fs::write(&temp, bytes).map_err(|e| e.to_string())?;
        std::fs::rename(&temp, &model).map_err(|e| e.to_string())?;
    }
    Ok(())
}

fn persist(local: &Path, profile: &Profile) -> Result<()> {
    profile.data()?;
    archive(local, profile)?;
    save(&local.join("mii-profile.json"), profile)
}

pub fn list(local: &Path) -> Result<Vec<Profile>> {
    if let Some(current) = read(local)? {
        archive(local, &current)?;
    }
    let mut result = Vec::new();
    let dir = local.join("profiles");
    if !dir.exists() {
        return Ok(result);
    }
    for entry in std::fs::read_dir(dir).map_err(|e| e.to_string())? {
        let entry = entry.map_err(|e| e.to_string())?;
        if !entry.file_type().map_err(|e| e.to_string())?.is_dir() {
            continue;
        }
        let p: Profile = load(&entry.path().join("profile.json"))?;
        p.appearance.validate()?;
        p.data()?;
        if p.schema != 1
            || p.id[8] & 0xc0 != 0x80
            || entry.file_name().to_str() != Some(&id_text(&p))
        {
            return Err("Arquivo de perfil inválido; nenhum perfil foi removido.".into());
        }
        result.push(p);
    }
    result.sort_by(|a, b| {
        a.appearance
            .name
            .cmp(&b.appearance.name)
            .then(a.id.cmp(&b.id))
    });
    Ok(result)
}

pub fn create(local: &Path, mut appearance: Appearance) -> Result<Profile> {
    appearance.name = appearance.name.trim().to_owned();
    appearance.validate()?;
    if let Some(previous) = read(local)? {
        archive(local, &previous)?;
    }
    let mut id = [0; 16];
    rand::thread_rng().fill_bytes(&mut id);
    id[6] = (id[6] & 0x0f) | 0x40;
    id[8] = (id[8] & 0x3f) | 0x80;
    let profile = Profile {
        schema: 1,
        id,
        appearance,
        charinfo: None,
    };
    persist(local, &profile)?;
    Ok(profile)
}

pub fn select(local: &Path, id: &str) -> Result<Profile> {
    if id.len() != 32
        || !id
            .bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
    {
        return Err("Identidade de perfil inválida.".into());
    }
    let profile = list(local)?
        .into_iter()
        .find(|p| id_text(p) == id)
        .ok_or("Perfil não encontrado")?;
    persist(local, &profile)?;
    Ok(profile)
}

pub fn configure(command: &mut Command, local: &Path) -> Result<()> {
    command.env("MK8_MII_PROFILE_PATH", local.join("mii-profile.json"));
    for key in [
        "MK8_MII_ID",
        "MK8_MII_NAME",
        "MK8_MII_COLOR",
        "MK8_MII_GENDER",
        "MK8_MII_FACE",
        "MK8_MII_CHARINFO",
    ] {
        command.env_remove(key);
    }
    if let Some(profile) = read(local)? {
        archive(local, &profile)?;
        command.env(
            "MK8_MII_CHARINFO",
            profile
                .data()?
                .iter()
                .map(|v| format!("{v:02x}"))
                .collect::<String>(),
        );
        let id: String = profile.id.iter().map(|b| format!("{b:02x}")).collect();
        command
            .env("MK8_MII_ID", id)
            .env("MK8_MII_NAME", profile.appearance.name)
            .env("MK8_MII_COLOR", profile.appearance.color.to_string())
            .env("MK8_MII_GENDER", profile.appearance.gender.to_string())
            .env(
                "MK8_MII_FACE",
                profile
                    .appearance
                    .face
                    .bytes()
                    .iter()
                    .map(|v| format!("{v:02x}"))
                    .collect::<String>(),
            );
    }
    Ok(())
}

pub fn import(local: &Path, data: &[u8]) -> Result<Profile> {
    crate::mii_data::validate(data)?;
    let id: [u8; 16] = data[..16].try_into().unwrap();
    if list(local)?.iter().any(|p| p.id == id) {
        return Err("Este Mii já existe. Selecione o perfil para editar.".into());
    }
    persist_data(local, data)
}

pub fn update_data(local: &Path, data: &[u8]) -> Result<Profile> {
    crate::mii_data::validate(data)?;
    let current = read(local)?.ok_or("Crie ou importe um Mii primeiro.")?;
    if current.id != data[..16] {
        return Err("O perfil ativo mudou. Recarregue o editor antes de salvar.".into());
    }
    persist_data(local, data)
}

fn persist_data(local: &Path, data: &[u8]) -> Result<Profile> {
    let id = data[..16].try_into().unwrap();
    let profile = Profile {
        schema: 1,
        id,
        charinfo: Some(data.to_vec()),
        appearance: Appearance {
            name: crate::mii_data::name(data)?,
            color: data[0x27],
            gender: data[0x28],
            face: crate::avatar::Face {
                skin: data[0x2e].min(5),
                shape: data[0x2d].min(2),
                hair: [0, 33, 24, 12]
                    .iter()
                    .position(|&v| v == data[0x31])
                    .unwrap_or(1) as u8,
                hair_color: data[0x32].min(7),
                eyes: [2, 4, 8].iter().position(|&v| v == data[0x34]).unwrap_or(0) as u8,
                eye_color: [8, 10, 11, 12, 9, 0]
                    .iter()
                    .position(|&v| v == data[0x35])
                    .unwrap_or(0) as u8,
                glasses: data[0x4f] != 0,
                height: data[0x29],
                build: data[0x2a],
            },
        },
    };
    persist(local, &profile)?;
    Ok(profile)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn imported_details_survive_export_and_name_edits() {
        let root = std::env::temp_dir().join(format!("mk8-import-{}", rand::random::<u64>()));
        let mut data = crate::mii_data::base();
        data[8] = 0x80;
        crate::mii_data::set_name(&mut data, "Importado");
        data[0x31] = 131;
        data[0x32] = 99;
        data[0x4f] = 19;
        data[0x2e] = 9;
        let p = import(&root, &data).unwrap();
        assert_eq!(p.data().unwrap(), data);
        assert!(import(&root, &data).is_err());
        let mut appearance = p.appearance.clone();
        appearance.name = "Novo".into();
        let edited = store(&root, appearance).unwrap();
        crate::mii_data::set_name(&mut data, "Novo");
        assert_eq!(edited.data().unwrap(), data);
        assert_eq!(read(&root).unwrap().unwrap(), edited);
        data[0x42] = 17;
        assert_eq!(update_data(&root, &data).unwrap().data().unwrap(), data);
        let mut wrong = data.clone();
        wrong[15] ^= 1;
        assert!(update_data(&root, &wrong).is_err());
        let before = std::fs::read(root.join("mii-profile.json")).unwrap();
        assert!(import(&root, &[0; 88]).is_err());
        assert_eq!(
            std::fs::read(root.join("mii-profile.json")).unwrap(),
            before
        );
        std::fs::remove_dir_all(root).unwrap();
    }
    #[test]
    fn profile_survives_reload_and_edits_keep_identity() {
        let root = std::env::temp_dir().join(format!("mk8-mii-{}", rand::random::<u64>()));
        let initial = Appearance {
            name: "João".into(),
            color: 3,
            gender: 0,
            face: crate::avatar::Face::default(),
        };
        assert_eq!(read(&root).unwrap(), None);
        let saved = store(&root, initial.clone()).unwrap();
        assert_eq!(read(&root).unwrap().unwrap(), saved);
        let changed = store(
            &root,
            Appearance {
                name: "Luigi".into(),
                ..initial.clone()
            },
        )
        .unwrap();
        assert_eq!(saved.id, changed.id);
        let bytes = std::fs::read(root.join("mii-profile.json")).unwrap();
        for name in ["", "abcdefghijk", "A\nB", "😀"] {
            assert!(store(
                &root,
                Appearance {
                    name: name.into(),
                    ..initial.clone()
                }
            )
            .is_err());
            assert_eq!(std::fs::read(root.join("mii-profile.json")).unwrap(), bytes);
        }
        let mut command = Command::new("unused.exe");
        configure(&mut command, &root).unwrap();
        assert!(command
            .get_envs()
            .any(|(k, v)| k == "MK8_MII_NAME" && v == Some(std::ffi::OsStr::new("Luigi"))));
        let second = create(
            &root,
            Appearance {
                name: "Mario".into(),
                face: crate::avatar::Face {
                    skin: 4,
                    hair: 3,
                    ..Default::default()
                },
                ..initial
            },
        )
        .unwrap();
        assert_ne!(second.id, changed.id);
        assert_eq!(list(&root).unwrap().len(), 2);
        assert_eq!(select(&root, &id_text(&changed)).unwrap(), changed);
        assert!(select(&root, "../mii-profile").is_err());
        assert!(root
            .join("profiles")
            .join(id_text(&second))
            .join("head.glb")
            .is_file());
        // This directory was allocated uniquely above and contains only this test's fixtures.
        std::fs::remove_dir_all(root).unwrap();
    }
}
