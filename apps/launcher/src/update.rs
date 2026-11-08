//! Updates from the project's GitHub Releases.
//!
//! The repository comes from `MK8_UPDATE_REPO` (`owner/name`), set when the release is packaged or at launch.
//! The newest non-draft release is used, pre-releases included, because the project is experimental.
//! Its `*-setup.exe` asset is downloaded over HTTPS. The asset must match the SHA-256 that GitHub reports
//! for it, or the `<asset>.sha256` file published next to it. The installer then runs silently and
//! restarts the launcher.
use mk8_manifest::{hash_file, Result};
use serde_json::{json, Value};
use std::{
    path::{Path, PathBuf},
    process::Command,
};

pub const CURRENT: &str = env!("CARGO_PKG_VERSION");
const AGENT: &str = "MK8-online-launcher";

pub fn repository() -> Option<String> {
    let repo = std::env::var("MK8_UPDATE_REPO")
        .ok()
        .or_else(|| option_env!("MK8_UPDATE_REPO").map(str::to_owned))?;
    let valid = |part: &str| {
        !part.is_empty()
            && part.len() <= 100
            && part
                .chars()
                .all(|c| c.is_ascii_alphanumeric() || "-_.".contains(c))
    };
    let (owner, name) = repo.trim().split_once('/')?;
    (valid(owner) && valid(name)).then(|| format!("{owner}/{name}"))
}

/// Whether this executable can replace itself: only the packaged launcher; a source checkout updates with git.
pub fn installable() -> bool {
    cfg!(mk8_portable)
}

/// (major, minor, patch, release-after-prerelease, prerelease text) of "v1.2.3-alpha.1".
fn version_key(text: &str) -> Option<(u64, u64, u64, bool, String)> {
    let text = text.trim().trim_start_matches(['v', 'V']);
    let (core, pre) = text.split_once('-').unwrap_or((text, ""));
    let mut parts = core.split('.').map(|p| p.parse::<u64>().ok());
    let key = (
        parts.next()??,
        parts.next().unwrap_or(Some(0))?,
        parts.next().unwrap_or(Some(0))?,
    );
    Some((key.0, key.1, key.2, pre.is_empty(), pre.to_owned()))
}

pub fn newer(candidate: &str, current: &str) -> bool {
    match (version_key(candidate), version_key(current)) {
        (Some(a), Some(b)) => a > b,
        _ => false,
    }
}

fn curl() -> Command {
    let mut cmd = Command::new("curl");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    cmd.args([
        "--disable",
        "--silent",
        "--show-error",
        "--fail",
        "--location",
        "--proto",
        "=https",
        "--proto-redir",
        "=https",
    ])
    .args(["--user-agent", AGENT]);
    cmd
}

fn get_text(url: &str, max_bytes: u64) -> Result<String> {
    let output = curl()
        .args(["--max-time", "20", "--max-filesize", &max_bytes.to_string()])
        .args([
            "--header",
            "Accept: application/vnd.github+json",
            "--url",
            url,
        ])
        .output()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).")?;
    if !output.status.success() {
        return Err(format!(
            "Falha ao consultar atualizações: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    String::from_utf8(output.stdout).map_err(|_| "Resposta inválida do GitHub.".into())
}

#[derive(Clone, Debug, PartialEq)]
pub struct Release {
    pub tag: String,
    pub name: String,
    pub page: String,
    pub prerelease: bool,
    pub setup_name: String,
    pub setup_url: String,
    pub sha256: Option<String>,
    pub sha256_url: Option<String>,
}

impl Release {
    pub fn summary(&self) -> Value {
        json!({"tag":self.tag,"name":self.name,"page":self.page,"prerelease":self.prerelease,"asset":self.setup_name})
    }
}

/// The newest published release with a setup asset, from the API's release list.
pub fn pick(releases: &Value) -> Option<Release> {
    releases.as_array()?.iter().find_map(|release| {
        if release["draft"] == true {
            return None;
        }
        let assets = release["assets"].as_array()?;
        let setup = assets.iter().find(|a| {
            a["name"]
                .as_str()
                .is_some_and(|n| n.to_ascii_lowercase().ends_with("-setup.exe"))
        })?;
        let setup_name = setup["name"].as_str()?.to_owned();
        let https = |v: &Value| {
            v.as_str()
                .filter(|u| u.starts_with("https://"))
                .map(str::to_owned)
        };
        let sha256 = setup["digest"]
            .as_str()
            .and_then(|d| d.strip_prefix("sha256:"))
            .filter(|d| d.len() == 64 && d.chars().all(|c| c.is_ascii_hexdigit()))
            .map(str::to_ascii_lowercase);
        let sidecar = format!("{setup_name}.sha256").to_ascii_lowercase();
        let sha256_url = assets
            .iter()
            .find(|a| {
                a["name"]
                    .as_str()
                    .is_some_and(|n| n.to_ascii_lowercase() == sidecar)
            })
            .and_then(|a| https(&a["browser_download_url"]));
        Some(Release {
            tag: release["tag_name"].as_str()?.to_owned(),
            name: release["name"].as_str().unwrap_or_default().to_owned(),
            page: https(&release["html_url"]).unwrap_or_default(),
            prerelease: release["prerelease"] == true,
            setup_url: https(&setup["browser_download_url"])?,
            setup_name,
            sha256,
            sha256_url,
        })
    })
}

pub fn latest() -> Result<Release> {
    let repo =
        repository().ok_or("Atualizações não configuradas nesta edição (MK8_UPDATE_REPO).")?;
    let text = get_text(
        &format!("https://api.github.com/repos/{repo}/releases?per_page=10"),
        4 << 20,
    )?;
    let releases: Value =
        serde_json::from_str(&text).map_err(|_| "Resposta inválida do GitHub.")?;
    pick(&releases).ok_or_else(|| "Nenhuma versão com instalador publicada ainda.".into())
}

pub fn check() -> Result<Value> {
    let release = latest()?;
    let available = newer(&release.tag, CURRENT);
    Ok(json!({
        "current": CURRENT,
        "latest": release.summary(),
        "available": available,
        "installable": installable(),
        "message": if available {
            format!("Versão {} disponível.", release.tag)
        } else {
            format!("Você já está na versão mais recente ({CURRENT}).")
        },
    }))
}

/// Downloads and verifies the newest setup into `updates/`, then starts it; the caller exits the launcher.
pub fn download_and_start(local: &Path) -> Result<PathBuf> {
    if !installable() {
        return Err("Edição de desenvolvimento: atualize o código com git e recompile.".into());
    }
    let release = latest()?;
    if !newer(&release.tag, CURRENT) {
        return Err(format!("Você já está na versão mais recente ({CURRENT})."));
    }
    let expected = match (&release.sha256, &release.sha256_url) {
        (Some(hash), _) => hash.clone(),
        (None, Some(url)) => get_text(url, 4096)?
            .split_whitespace()
            .next()
            .filter(|h| h.len() == 64 && h.chars().all(|c| c.is_ascii_hexdigit()))
            .ok_or("Arquivo .sha256 inválido na versão publicada.")?
            .to_ascii_lowercase(),
        (None, None) => {
            return Err("A versão publicada não informa o SHA-256 do instalador.".into())
        }
    };
    let safe = |c: char| c.is_ascii_alphanumeric() || "-_.".contains(c);
    if !release.setup_name.chars().all(safe) {
        return Err("Nome de instalador inesperado.".into());
    }
    let folder = local.join("updates");
    std::fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
    let target = folder.join(&release.setup_name);
    let partial = folder.join(format!("{}.partial", release.setup_name));
    let output = curl()
        .args([
            "--max-time",
            "1800",
            "--max-filesize",
            &(2u64 << 30).to_string(),
            "--output",
        ])
        .arg(&partial)
        .args(["--url", &release.setup_url])
        .output()
        .map_err(|_| "Não foi possível iniciar o cliente HTTPS (curl).")?;
    if !output.status.success() {
        let _ = std::fs::remove_file(&partial);
        return Err(format!(
            "Download falhou: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let actual = hash_file(&partial)?.sha256;
    if actual != expected {
        let _ = std::fs::remove_file(&partial);
        return Err(
            "O instalador baixado não confere com o SHA-256 publicado; nada foi instalado.".into(),
        );
    }
    std::fs::rename(&partial, &target).map_err(|e| e.to_string())?;
    // The setup (installer.rs) installs silently over this copy, with its folders, and starts the new launcher.
    Command::new(&target)
        .args([
            "/SILENT",
            "/SUPPRESSMSGBOXES",
            "/NORESTART",
            "/CLOSEAPPLICATIONS",
        ])
        .spawn()
        .map_err(|e| format!("Falha ao iniciar o instalador: {e}"))?;
    Ok(target)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn versions_compare_numerically_and_releases_win_over_prereleases() {
        assert!(newer("v0.2.0", "0.1.9"));
        assert!(newer("v0.10.0", "0.9.0"));
        assert!(newer("v0.2.0", "0.2.0-alpha.1"));
        assert!(newer("v0.2.0-alpha.2", "0.2.0-alpha.1"));
        assert!(!newer("v0.1.0", "0.1.0"));
        assert!(!newer("v0.1.0-alpha.1", "0.1.0"));
        assert!(!newer("nightly", "0.1.0"));
    }

    #[test]
    fn picks_newest_release_with_setup_and_digest() {
        let releases = json!([
            {"draft": true, "tag_name": "v9.0.0", "assets": [{"name": "MK8-online-9.0.0-setup.exe", "browser_download_url": "https://x/a"}]},
            {"draft": false, "tag_name": "v0.3.0", "assets": [{"name": "notes.txt", "browser_download_url": "https://x/n"}]},
            {"draft": false, "prerelease": true, "tag_name": "v0.2.0-alpha.1", "html_url": "https://github.com/o/r/releases/v0.2.0-alpha.1",
             "assets": [
                {"name": "MK8-online-0.2.0-alpha.1-setup.exe", "browser_download_url": "https://github.com/o/r/s.exe",
                 "digest": "sha256:ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789"},
                {"name": "MK8-online-0.2.0-alpha.1-setup.exe.sha256", "browser_download_url": "https://github.com/o/r/s.sha256"}]}
        ]);
        let release = pick(&releases).unwrap();
        assert_eq!(release.tag, "v0.2.0-alpha.1");
        assert!(release.prerelease);
        assert_eq!(
            release.sha256.as_deref(),
            Some("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789")
        );
        assert_eq!(
            release.sha256_url.as_deref(),
            Some("https://github.com/o/r/s.sha256")
        );
        let insecure = json!([{"tag_name": "v1.0.0", "assets": [{"name": "a-setup.exe", "browser_download_url": "http://x/a"}]}]);
        assert!(pick(&insecure).is_none());
    }
}
