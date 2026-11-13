//! Diagnostic reports: one zip with the logs a developer needs to look into a problem.
//!
//! The report holds the tail of each log, the build manifests and a summary of the launcher state.
//! It never includes saves, game files or keys. The user profile path and the account name are replaced
//! before anything is written, and the report stays on the machine until the user sends it.
use mk8_manifest::Result;
use serde_json::{json, Value};
use std::{
    fs,
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

/// Largest tail of a single file copied into a report.
const TAIL: u64 = 4 << 20;
/// Reports kept in `reports/`; older ones are removed when a new one is written.
const KEEP: usize = 20;

pub fn folder(local: &Path) -> PathBuf {
    local.join("reports")
}

/// Removes the user's profile path and account name from text that leaves the machine.
pub fn redact(text: &str) -> String {
    let mut out = text.to_owned();
    if let Some(profile) = std::env::var_os("USERPROFILE").and_then(|p| p.into_string().ok()) {
        for form in [
            profile.clone(),
            profile.replace('\\', "/"),
            profile.replace('\\', "\\\\"),
        ] {
            if form.len() > 3 {
                out = replace_ignore_case(&out, &form, "%USERPROFILE%");
            }
        }
    }
    if let Ok(user) = std::env::var("USERNAME") {
        if user.chars().count() >= 3 {
            out = replace_ignore_case(&out, &user, "<user>");
        }
    }
    out
}

fn replace_ignore_case(text: &str, needle: &str, with: &str) -> String {
    let lower = text.to_lowercase();
    let pattern = needle.to_lowercase();
    if lower.len() != text.len() || pattern.is_empty() {
        return text.replace(needle, with); // (case folding changed byte offsets: exact match only)
    }
    let mut out = String::with_capacity(text.len());
    let mut last = 0;
    for (at, _) in lower.match_indices(&pattern) {
        out.push_str(&text[last..at]);
        out.push_str(with);
        last = at + pattern.len();
    }
    out.push_str(&text[last..]);
    out
}

fn tail(path: &Path) -> Option<String> {
    let mut file = fs::File::open(path).ok()?;
    let size = file.metadata().ok()?.len();
    file.seek(SeekFrom::Start(size.saturating_sub(TAIL))).ok()?;
    let mut bytes = Vec::new();
    file.take(TAIL).read_to_end(&mut bytes).ok()?;
    let mut text = String::from_utf8_lossy(&bytes).into_owned();
    if size > TAIL {
        text.insert_str(0, &format!("[report: last {TAIL} of {size} bytes]\n"));
    }
    Some(text)
}

fn stamp() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_or(0, |d| d.as_secs())
}

/// Previous game logs kept by `mk8_installer::rotate_game_log`, newest first.
fn previous_game_logs(local: &Path, count: usize) -> Vec<PathBuf> {
    let mut logs: Vec<PathBuf> = fs::read_dir(local.join("logs"))
        .map(|dir| dir.filter_map(|e| e.ok().map(|e| e.path())).collect())
        .unwrap_or_default();
    logs.sort_by_key(|p| std::cmp::Reverse(fs::metadata(p).and_then(|m| m.modified()).ok()));
    logs.truncate(count);
    logs
}

/// Writes `reports/mk8-report-<time>.zip` and returns its path.
pub fn create(
    local: &Path,
    runtime: Option<&Path>,
    reason: &str,
    summary: Value,
) -> Result<PathBuf> {
    let reports = folder(local);
    fs::create_dir_all(&reports).map_err(|e| e.to_string())?;
    let name = format!("mk8-report-{}", stamp());
    let stage = reports.join(format!("{name}.partial"));
    fs::create_dir(&stage).map_err(|e| format!("{}: {e}", stage.display()))?;
    let result = fill(local, runtime, &stage, reason, summary).and_then(|files| {
        let zip = reports.join(format!("{name}.zip"));
        let tar = std::env::var_os("SystemRoot")
            .map(|root| Path::new(&root).join("System32/tar.exe"))
            .filter(|p| p.is_file())
            .ok_or("tar.exe do Windows não encontrado.")?;
        let mut cmd = Command::new(tar);
        cmd.arg("-a")
            .arg("-c")
            .arg("-f")
            .arg(&zip)
            .arg("-C")
            .arg(&stage)
            .args(&files);
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x08000000);
        }
        let output = cmd.output().map_err(|e| e.to_string())?;
        if !output.status.success() {
            return Err(format!(
                "Falha ao compactar o relatório: {}",
                String::from_utf8_lossy(&output.stderr)
            ));
        }
        Ok(zip)
    });
    // (the staging folder was created above by this call: nothing else lives in it)
    let _ = fs::remove_dir_all(&stage);
    prune(&reports);
    result
}

fn fill(
    local: &Path,
    runtime: Option<&Path>,
    stage: &Path,
    reason: &str,
    summary: Value,
) -> Result<Vec<String>> {
    let mut files = Vec::new();
    let mut sources: Vec<(String, PathBuf)> = vec![
        ("game.log".into(), local.join("game.log")),
        ("build.log".into(), local.join("build.log")),
        ("setup.log".into(), local.join("setup.log")),
        ("candidate.json".into(), local.join("candidate.json")),
        ("config.json".into(), local.join("config.json")),
        ("vulkan-check.log".into(), local.join("vulkan-check.log")),
        ("mk8d.ini".into(), local.join("user-no-mii/mk8d.ini")),
    ];
    for (i, path) in previous_game_logs(local, 3).into_iter().enumerate() {
        sources.push((format!("previous-game-{}.log", i + 1), path));
    }
    // Windows runtime redirects output beside its executable; launcher stdout can be empty.
    if let Some(runtime) = runtime {
        for name in [
            "mk8drecomp.log",
            "mk8drecomp.previous.log",
            "mk8drecomp.previous2.log",
            "mk8drecomp.previous3.log",
        ] {
            sources.push((name.into(), runtime.join(name)));
        }
    }
    let mut included = Vec::new();
    for (name, path) in sources {
        if let Some(text) = tail(&path) {
            fs::write(stage.join(&name), redact(&text)).map_err(|e| e.to_string())?;
            included.push(name.clone());
            files.push(name);
        }
    }
    let info = json!({
        "reason": reason,
        "created_unix": stamp(),
        "launcher_version": env!("CARGO_PKG_VERSION"),
        "os": std::env::consts::OS,
        "arch": std::env::consts::ARCH,
        "windows": windows_version(),
        "files": included,
        "launcher": summary,
    });
    let text = serde_json::to_string_pretty(&info).map_err(|e| e.to_string())?;
    fs::write(stage.join("report.json"), redact(&text)).map_err(|e| e.to_string())?;
    files.insert(0, "report.json".into());
    Ok(files)
}

fn windows_version() -> String {
    let mut cmd = Command::new("cmd");
    cmd.args(["/C", "ver"]);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    cmd.output()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn prune(reports: &Path) {
    let mut zips: Vec<PathBuf> = fs::read_dir(reports)
        .map(|dir| dir.filter_map(|e| e.ok().map(|e| e.path())).collect())
        .unwrap_or_default();
    zips.retain(|p| p.extension().is_some_and(|e| e == "zip"));
    zips.sort_by_key(|p| fs::metadata(p).and_then(|m| m.modified()).ok());
    let excess = zips.len().saturating_sub(KEEP);
    for path in zips.into_iter().take(excess) {
        let _ = fs::remove_file(path);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn redaction_hides_profile_path_and_account() {
        let profile = std::env::var("USERPROFILE").unwrap_or_default();
        if profile.len() <= 3 {
            return;
        }
        let text = format!(
            "loaded {profile}\\game and {}",
            profile.replace('\\', "/").to_uppercase()
        );
        let clean = redact(&text);
        assert!(!clean.to_lowercase().contains(&profile.to_lowercase()));
        assert!(clean.contains("%USERPROFILE%\\game"));
    }

    #[test]
    fn report_contains_logs_and_summary_but_no_saves() {
        let local = std::env::temp_dir().join(format!("mk8-report-{}", std::process::id()));
        fs::create_dir_all(local.join("user-no-mii/save")).unwrap();
        fs::write(local.join("game.log"), "FATAL: test crash\n").unwrap();
        fs::write(local.join("vulkan-check.log"), "DLL dependency failed\n").unwrap();
        fs::write(local.join("user-no-mii/save/save_0"), "private save").unwrap();
        let runtime = local.join("builds/test/host-optimized");
        fs::create_dir_all(&runtime).unwrap();
        fs::write(local.join("game.log"), "").unwrap();
        fs::write(runtime.join("mk8drecomp.log"), "FATAL: runtime crash\n").unwrap();
        fs::write(
            runtime.join("mk8drecomp.previous.log"),
            "previous session\n",
        )
        .unwrap();
        fs::write(runtime.join("private-save.bin"), "never include").unwrap();
        let zip = create(&local, Some(&runtime), "test", json!({"build":"idle"})).unwrap();
        assert!(zip.is_file());
        let listing =
            Command::new(Path::new(&std::env::var("SystemRoot").unwrap()).join("System32/tar.exe"))
                .arg("-tf")
                .arg(&zip)
                .output()
                .unwrap();
        let names = String::from_utf8_lossy(&listing.stdout);
        assert!(names.contains("report.json") && names.contains("game.log"));
        assert!(names.contains("vulkan-check.log"));
        assert!(names.contains("mk8drecomp.log"));
        assert!(names.contains("mk8drecomp.previous.log"));
        assert!(!names.contains("private-save.bin"));
        assert!(!names.contains("save_0"));
        let content =
            Command::new(Path::new(&std::env::var("SystemRoot").unwrap()).join("System32/tar.exe"))
                .args(["-xOf"])
                .arg(&zip)
                .arg("mk8drecomp.log")
                .output()
                .unwrap();
        assert!(content.status.success());
        assert_eq!(
            String::from_utf8_lossy(&content.stdout),
            "FATAL: runtime crash\n"
        );
        assert!(!folder(&local)
            .join(format!(
                "{}.partial",
                zip.file_stem().unwrap().to_string_lossy()
            ))
            .exists());
        fs::remove_dir_all(local).unwrap();
    }
}
