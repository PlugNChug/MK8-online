//! Download the pinned archived Miitomo resource into private data, without running downloaded code.
use mk8_installer::mii_composer;
use mk8_manifest::{hash_file, Result};
use serde_json::{json, Value};
use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    process::Command,
    thread::JoinHandle,
};

pub const SOURCE: &str = "https://web.archive.org/web/20180502054513id_/http://download-cdn.miitomo.com/native/20180125111639/android/v2/asset_model_character_mii_AFLResHigh_2_3_dat.zip";
const ARCHIVE_SHA256: &str = "42c106511e8d520583dd4f43360386aa77a5080e4ca8574e7e3d2288e2786d43";
const ARCHIVE_BYTES: u64 = 4_393_464;

#[derive(Default)]
pub struct Download {
    worker: Option<JoinHandle<Result<PathBuf>>>,
    expected: Option<PathBuf>,
    state: &'static str,
    detail: String,
}

fn configured(local: &Path) -> Result<Option<PathBuf>> {
    Ok(mii_composer::resource_config(local)?.map(|r| r.path))
}

impl Download {
    pub fn start(&mut self, local: &Path) -> Result<Value> {
        self.poll(local);
        if self.worker.is_none() {
            if mii_composer::resource(local).is_ok() {
                self.state = "done";
                self.detail.clear();
            } else {
                self.expected = configured(local)?;
                let local = local.to_path_buf();
                self.worker = Some(
                    std::thread::Builder::new()
                        .name("mii-library-download".into())
                        .spawn(move || download(&local))
                        .map_err(|e| e.to_string())?,
                );
                self.state = "running";
                self.detail.clear();
            }
        }
        Ok(self.status(local))
    }

    fn poll(&mut self, local: &Path) {
        if !self.worker.as_ref().is_some_and(|w| w.is_finished()) {
            return;
        }
        let result = self
            .worker
            .take()
            .unwrap()
            .join()
            .map_err(|_| "O download da biblioteca foi interrompido.".to_string())
            .and_then(|r| r)
            .and_then(|path| {
                // A file selected manually while downloading takes precedence.
                if configured(local)? == self.expected {
                    mii_composer::configure(local, &path)?;
                }
                Ok(())
            });
        match result {
            Ok(()) => {
                self.state = "done";
                self.detail.clear();
            }
            Err(e) => {
                self.state = "failed";
                self.detail = e;
            }
        }
    }

    pub fn status(&mut self, local: &Path) -> Value {
        self.poll(local);
        json!({"state":if self.state.is_empty(){"idle"}else{self.state},"detail":self.detail,"source":SOURCE})
    }
}

fn extract(archive: &Path) -> Result<Vec<u8>> {
    let identity = hash_file(archive)?;
    if identity.bytes != ARCHIVE_BYTES || identity.sha256 != ARCHIVE_SHA256 {
        return Err("O download não corresponde à biblioteca Mii verificada.".into());
    }
    // Windows ships bsdtar. Extract just this member to memory, after verifying the entire ZIP.
    let system = std::env::var_os("SystemRoot").ok_or("Pasta do Windows indisponível.")?;
    let mut cmd = Command::new(PathBuf::from(system).join("System32/tar.exe"));
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    let out = cmd
        .args(["-xOf"])
        .arg(archive)
        .arg("asset/model/character/mii/AFLResHigh_2_3.dat")
        .output()
        .map_err(|e| format!("Não foi possível abrir o ZIP da biblioteca Mii: {e}"))?;
    if !out.status.success() {
        return Err("Não foi possível extrair a biblioteca Mii.".into());
    }
    mii_composer::validate_resource(&out.stdout)?;
    Ok(out.stdout)
}

fn download(local: &Path) -> Result<PathBuf> {
    let folder = local.join("mii-library");
    fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
    let target = folder.join("AFLResHigh_2_3.dat");
    if target.is_file() && fs::metadata(&target).map_err(|e| e.to_string())?.len() == 4_579_008 {
        let bytes = fs::read(&target).map_err(|e| e.to_string())?;
        if mii_composer::validate_resource(&bytes).is_ok() {
            return Ok(target);
        }
    }
    if crate::installer::free_gib(&folder).is_none_or(|free| free < 4.0) {
        return Err("Mantenha pelo menos 4 GiB livres para baixar a biblioteca Mii.".into());
    }
    let id = mk8_transport::random::<8>()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect::<String>();
    let archive = folder.join(format!("{id}.zip.partial"));
    let pending = folder.join(format!("{id}.dat.partial"));
    let result = (|| {
        let out = crate::update::curl()
            .args([
                "--connect-timeout",
                "15",
                "--max-time",
                "90",
                "--max-filesize",
                &ARCHIVE_BYTES.to_string(),
                "--max-redirs",
                "3",
                "--output",
            ])
            .arg(&archive)
            .args(["--url", SOURCE])
            .output()
            .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).")?;
        if !out.status.success() {
            return Err("Não foi possível baixar do Internet Archive. Tente novamente ou selecione um arquivo local.".into());
        }
        let bytes = extract(&archive)?;
        let mut file = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&pending)
            .map_err(|e| e.to_string())?;
        file.write_all(&bytes)
            .and_then(|_| file.sync_all())
            .map_err(|e| e.to_string())?;
        drop(file);
        fs::rename(&pending, &target).map_err(|e| e.to_string())?;
        Ok(target)
    })();
    // Only this attempt's two files, never the user's resource, profiles or directories.
    for file in [&archive, &pending] {
        if let Err(e) = fs::remove_file(file) {
            if e.kind() != std::io::ErrorKind::NotFound {
                eprintln!("Temporary Mii download retained: {e}");
            }
        }
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn altered_archives_and_resources_are_rejected_before_extraction() {
        let file = std::env::temp_dir().join(format!("mk8-mii-archive-{}.zip", std::process::id()));
        fs::write(&file, vec![0; ARCHIVE_BYTES as usize]).unwrap();
        assert!(extract(&file).unwrap_err().contains("não corresponde"));
        assert!(mii_composer::validate_resource(&vec![0; 4_579_008]).is_err());
        assert!(mii_composer::validate_resource(b"truncated").is_err());
        fs::remove_file(file).unwrap();
    }
}
