//! Private CPython bootstrap. Only pinned HTTPS bytes are extracted; no shell or .NET loader.
use mk8_manifest::{hash_file, Result};
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

const VERSION: &str = "3.12.10";
const SHA256: &str = "0eb85c2dfccccf1b17352de4c397f69194035b7d37149eacc16f1147d93de3b8";
const BYTES: u64 = 14_515_433;
const URL: &str = "https://api.nuget.org/v3-flatcontainer/python/3.12.10/python.3.12.10.nupkg";

fn id() -> String {
    mk8_transport::random::<8>()
        .iter()
        .map(|b| format!("{b:02x}"))
        .collect()
}

fn command(program: &Path) -> Command {
    let mut cmd = Command::new(program);
    cmd.stdin(Stdio::null());
    for (key, _) in std::env::vars_os() {
        if key
            .to_string_lossy()
            .to_ascii_uppercase()
            .starts_with("PYTHON")
        {
            cmd.env_remove(key);
        }
    }
    cmd.env("PIP_CONFIG_FILE", "NUL")
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    cmd
}

fn run(cmd: &mut Command) -> Result<()> {
    let exit = cmd.status().map_err(|e| e.to_string())?;
    if exit.success() {
        Ok(())
    } else {
        Err(format!("Preparação do Python falhou ({exit}). Consulte o log; os arquivos anteriores foram preservados."))
    }
}

fn ready(python: &Path) -> bool {
    command(python).args(["-I", "-c", "import sys,pip; sys.exit(0 if sys.version_info[:2] == (3,12) and sys.maxsize > 2**32 else 1)"])
        .output().is_ok_and(|out| out.status.success())
}

fn no_link(path: &Path) -> Result<()> {
    match fs::symlink_metadata(path) {
        Ok(meta) => {
            #[cfg(windows)]
            {
                use std::os::windows::fs::MetadataExt;
                if meta.file_attributes() & 0x400 != 0 {
                    return Err(format!(
                        "Pasta Python contém junction ou link: {}",
                        path.display()
                    ));
                }
            }
            if meta.file_type().is_symlink() {
                return Err(format!("Pasta Python contém link: {}", path.display()));
            }
            Ok(())
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.to_string()),
    }
}

fn checked_tree(path: &Path, local: &Path) -> Result<()> {
    if path.parent() != Some(local) || !local.is_absolute() {
        return Err("Pasta Python fora do diretório local.".into());
    }
    for parent in path.ancestors() {
        no_link(parent)?;
    }
    if !path.exists() {
        return Ok(());
    }
    let mut pending = vec![path.to_path_buf()];
    while let Some(dir) = pending.pop() {
        for entry in fs::read_dir(dir).map_err(|e| e.to_string())? {
            let file = entry.map_err(|e| e.to_string())?.path();
            no_link(&file)?;
            if file.is_dir() {
                pending.push(file);
            }
        }
    }
    Ok(())
}

#[cfg(windows)]
fn idle(path: &Path) -> Result<()> {
    use windows_sys::Win32::{
        Foundation::*,
        System::{Diagnostics::ToolHelp::*, Threading::*},
    };
    let prefix = path
        .canonicalize()
        .map_err(|e| e.to_string())?
        .to_string_lossy()
        .trim_start_matches(r"\\?\")
        .to_lowercase()
        + "\\";
    unsafe {
        let snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snapshot == INVALID_HANDLE_VALUE {
            return Err(std::io::Error::last_os_error().to_string());
        }
        let result = (|| {
            let mut entry: PROCESSENTRY32W = std::mem::zeroed();
            entry.dwSize = std::mem::size_of_val(&entry) as u32;
            let mut more = Process32FirstW(snapshot, &mut entry);
            while more != 0 {
                let n = entry
                    .szExeFile
                    .iter()
                    .position(|&c| c == 0)
                    .unwrap_or(entry.szExeFile.len());
                let name = String::from_utf16_lossy(&entry.szExeFile[..n]).to_lowercase();
                if matches!(name.as_str(), "python.exe" | "pythonw.exe") {
                    let process =
                        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, entry.th32ProcessID);
                    if process.is_null() {
                        let error = GetLastError();
                        if error != ERROR_INVALID_PARAMETER {
                            return Err(
                                "Não foi possível verificar se o Python está em uso.".into()
                            );
                        }
                    } else {
                        let mut buffer = vec![0u16; 32768];
                        let mut size = buffer.len() as u32;
                        let ok =
                            QueryFullProcessImageNameW(process, 0, buffer.as_mut_ptr(), &mut size);
                        CloseHandle(process);
                        if ok == 0 {
                            return Err(
                                "Não foi possível verificar o caminho do Python em execução."
                                    .into(),
                            );
                        }
                        if String::from_utf16_lossy(&buffer[..size as usize])
                            .trim_start_matches(r"\\?\")
                            .to_lowercase()
                            .starts_with(&prefix)
                        {
                            return Err(
                                "Ambiente Python em uso; feche a tarefa antes de reparar.".into()
                            );
                        }
                    }
                }
                more = Process32NextW(snapshot, &mut entry);
            }
            if GetLastError() != ERROR_NO_MORE_FILES {
                return Err("Não foi possível concluir a verificação dos processos Python.".into());
            }
            Ok(())
        })();
        CloseHandle(snapshot);
        result
    }
}

fn backup(path: &Path, local: &Path) -> Result<()> {
    checked_tree(path, local)?;
    if !path.exists() {
        return Ok(());
    }
    #[cfg(windows)]
    idle(path)?;
    let saved = local.join(format!(
        "{}-backup-{}",
        path.file_name()
            .ok_or("Pasta Python inválida")?
            .to_string_lossy(),
        id()
    ));
    crate::publish_payload(path, &saved, std::time::Duration::from_secs(15))
        .map_err(|e| e.to_string())?;
    println!("Ambiente anterior preservado: {}", saved.display());
    Ok(())
}

fn verify(archive: &Path) -> Result<()> {
    let actual = hash_file(archive)?;
    if actual.bytes != BYTES || actual.sha256 != SHA256 {
        return Err("Python: tamanho ou SHA-256 divergente; arquivo recusado.".into());
    }
    Ok(())
}

pub fn install(root: &Path) -> Result<()> {
    if !root.is_absolute() {
        return Err("A raiz das ferramentas deve ser absoluta.".into());
    }
    let local = root.join("local");
    for parent in local.ancestors() {
        no_link(parent)?;
    }
    fs::create_dir_all(&local).map_err(|e| e.to_string())?;
    if crate::installer::free_gib(&local).is_none_or(|n| n < 4.25) {
        return Err("Python requer 250 MiB além da reserva de 4 GiB.".into());
    }
    let environment = local.join("python");
    checked_tree(&environment, &local)?;
    let python = environment.join("Scripts/python.exe");
    if !ready(&python) {
        let base = local.join(format!("cpython-{VERSION}"));
        checked_tree(&base, &local)?;
        let exe = base.join("tools/python.exe");
        if !ready(&exe) {
            let archive = local.join(format!("python-{VERSION}.zip"));
            no_link(&archive)?;
            if verify(&archive).is_err() {
                println!("Baixando CPython {VERSION} x64 do NuGet oficial (14 MB)…");
                let partial = local.join(format!("python-download-{}.zip", id()));
                run(crate::update::curl()
                    .args([
                        "--connect-timeout",
                        "15",
                        "--max-time",
                        "180",
                        "--max-filesize",
                        &BYTES.to_string(),
                        "--output",
                    ])
                    .arg(&partial)
                    .args(["--url", URL]))?;
                verify(&partial)?;
                fs::rename(&partial, &archive).map_err(|e| e.to_string())?;
            }
            verify(&archive)?;
            println!("Extraindo CPython com SHA-256 verificado…");
            let stage = local.join(format!("cpython-staging-{}", id()));
            fs::create_dir(&stage).map_err(|e| e.to_string())?;
            let tar = PathBuf::from(std::env::var_os("SystemRoot").ok_or("SystemRoot ausente")?)
                .join("System32/tar.exe");
            run(command(&tar).arg("-xf").arg(&archive).arg("-C").arg(&stage))?;
            checked_tree(&stage, &local)?;
            if !ready(&stage.join("tools/python.exe")) {
                return Err("CPython extraído não passou na verificação de versão/pip.".into());
            }
            backup(&base, &local)?;
            crate::publish_payload(&stage, &base, std::time::Duration::from_secs(15))
                .map_err(|e| e.to_string())?;
        }
        backup(&environment, &local)?;
        println!("Configurando ambiente Python 3.12 x64…");
        run(command(&exe).args(["-I", "-m", "venv"]).arg(&environment))?;
        if !ready(&python) {
            return Err("Ambiente Python criado não passou na verificação.".into());
        }
    }
    for requirements in [
        "upstream/smm2recomp/recomp/tools/requirements-gen.txt",
        "toolchain/requirements-host.txt",
    ] {
        println!("Instalando pacotes binários com hashes fixados: {requirements}");
        run(command(&python)
            .args([
                "-I",
                "-m",
                "pip",
                "--isolated",
                "--disable-pip-version-check",
                "install",
                "--only-binary=:all:",
                "--index-url",
                "https://pypi.org/simple",
                "--require-hashes",
                "-r",
            ])
            .arg(root.join(requirements)))?;
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn invalid_archive_and_external_backup_are_refused() {
        let local = std::env::temp_dir().join(format!("mk8-python-check-{}", id()));
        fs::create_dir(&local).unwrap();
        let archive = local.join("bad.zip");
        fs::write(&archive, b"untrusted").unwrap();
        assert!(verify(&archive).unwrap_err().contains("SHA-256"));
        assert!(backup(&local, &local).is_err());
        assert_eq!(fs::read(&archive).unwrap(), b"untrusted");
        fs::remove_file(archive).unwrap();
        fs::remove_dir(local).unwrap();
    }
}
