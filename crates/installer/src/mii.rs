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
    Ok(Some(profile))
}

pub fn store(local: &Path, mut appearance: Appearance) -> Result<Profile> {
    appearance.name = appearance.name.trim().to_owned();
    appearance.validate()?;
    let id = if let Some(previous) = read(local)? {
        previous.id
    } else {
        let mut id = [0; 16];
        rand::thread_rng().fill_bytes(&mut id);
        id[8] = (id[8] & 0x3f) | 0x80;
        id
    };
    let profile = Profile {
        schema: 1,
        id,
        appearance,
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
    id[8] = (id[8] & 0x3f) | 0x80;
    let profile = Profile {
        schema: 1,
        id,
        appearance,
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
    ] {
        command.env_remove(key);
    }
    if let Some(profile) = read(local)? {
        archive(local, &profile)?;
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

#[cfg(test)]
mod tests {
    use super::*;
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
