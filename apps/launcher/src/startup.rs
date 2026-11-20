//! Explicit startup modes; the portable marker never migrates an existing installation.
use std::path::{Path, PathBuf};

pub fn portable_home(exe: &Path) -> Result<Option<PathBuf>, crate::Error> {
    let marker = exe.with_file_name("portable.txt");
    if !marker.try_exists()? {
        return Ok(None);
    }
    if !marker.is_file() {
        return Err("portable.txt deve ser um arquivo ao lado do Launcher.exe".into());
    }
    Ok(Some(
        exe.parent()
            .ok_or("Pasta do launcher ausente")?
            .join("portable"),
    ))
}

pub fn validate_args(args: &[String]) -> Result<(), crate::Error> {
    if let Some(i) = args.iter().position(|arg| arg == "--data-dir") {
        if args
            .get(i + 1)
            .is_none_or(|value| value.is_empty() || value.starts_with('-'))
        {
            return Err("--data-dir exige o caminho da pasta de dados".into());
        }
    }
    if args.iter().any(|arg| arg == "--play")
        && args.iter().any(|arg| {
            matches!(
                arg.as_str(),
                "+connect_lobby" | "--port" | "--headless" | "--no-browser"
            )
        })
    {
        return Err(
            "--play inicia uma sessão solo; não combine com sala Steam ou modo de servidor".into(),
        );
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn direct_play_requires_valid_data_arguments() {
        for args in [
            vec!["--play"],
            vec!["--play", "--data-dir", "D:/Games/MK8 data"],
        ] {
            assert!(
                validate_args(&args.into_iter().map(str::to_owned).collect::<Vec<_>>()).is_ok()
            );
        }
        for args in [
            vec!["--data-dir"],
            vec!["--data-dir", "--play"],
            vec!["--play", "+connect_lobby", "123"],
            vec!["--play", "--headless"],
        ] {
            assert!(
                validate_args(&args.into_iter().map(str::to_owned).collect::<Vec<_>>()).is_err()
            );
        }
    }
    #[test]
    fn portable_marker_selects_sibling_storage() {
        let dir = std::env::temp_dir().join(format!("mk8-portable-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let exe = dir.join("Launcher.exe");
        assert_eq!(portable_home(&exe).unwrap(), None);
        std::fs::write(dir.join("portable.txt"), b"").unwrap();
        assert_eq!(portable_home(&exe).unwrap(), Some(dir.join("portable")));
        std::fs::remove_file(dir.join("portable.txt")).unwrap();
        std::fs::remove_dir(dir).unwrap();
    }
}
