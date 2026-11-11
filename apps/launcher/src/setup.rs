//! Guided tool setup: runs the existing setup scripts one after another and
//! reports which local tools are present. Presence is not a hash check: the
//! scripts verify hashes when they run.
use serde_json::{json, Value};
use std::{
    ffi::OsString,
    fs::{self, File, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
};

pub struct Step {
    pub name: String,
    pub program: OsString,
    pub args: Vec<OsString>,
}

impl Step {
    fn new(name: &str, program: impl Into<OsString>, args: &[&dyn AsRef<std::ffi::OsStr>]) -> Self {
        Self {
            name: name.into(),
            program: program.into(),
            args: args.iter().map(|a| a.as_ref().to_os_string()).collect(),
        }
    }
}

fn local_python(root: &Path) -> PathBuf {
    root.join("local/python/Scripts/python.exe")
}

/// The same three commands documented in the README, in order.
pub fn steps(root: &Path) -> Vec<Step> {
    let scripts = root.join("scripts");
    vec![
        Step::new(
            "Ambiente Python local e pacotes verificados",
            "powershell.exe",
            &[
                &"-NoProfile",
                &"-NonInteractive",
                &"-ExecutionPolicy",
                &"Bypass",
                &"-File",
                &scripts.join("setup-tools.ps1"),
            ],
        ),
        Step::new(
            "Compilador Zig fixado",
            local_python(root),
            &[&"-u", &scripts.join("setup-native.py")],
        ),
        Step::new(
            "Bibliotecas gráficas e FFmpeg",
            local_python(root),
            &[&"-u", &scripts.join("setup-runtime-deps.py")],
        ),
    ]
}

#[derive(Clone, Copy, PartialEq, Debug)]
enum State {
    Idle,
    Running,
    Done,
    Failed,
    Cancelled,
}

impl State {
    fn name(self) -> &'static str {
        match self {
            State::Idle => "idle",
            State::Running => "running",
            State::Done => "done",
            State::Failed => "failed",
            State::Cancelled => "cancelled",
        }
    }
}

pub struct Setup {
    steps: Vec<Step>,
    current: usize,
    child: Option<Child>,
    guards: Vec<File>,
    supervised: Option<(PathBuf, PathBuf)>,
    state: State,
    detail: String,
    log: PathBuf,
}

impl Default for Setup {
    fn default() -> Self {
        Self {
            steps: Vec::new(),
            current: 0,
            child: None,
            guards: Vec::new(),
            supervised: None,
            state: State::Idle,
            detail: "Nenhuma instalação de ferramentas iniciada nesta sessão.".into(),
            log: PathBuf::new(),
        }
    }
}

impl Setup {
    pub fn start_guarded(&mut self, root: &Path, local: &Path) -> Result<(), String> {
        self.start_steps_guarded(root, local, steps(root), &local.join("setup.log"))
    }

    pub fn start_dlc(
        &mut self,
        root: &Path,
        local: &Path,
        package: &Path,
        keys: &Path,
        reinstall: bool,
    ) -> Result<(), String> {
        let mut step = Step::new(
            "Importação do Booster Course Pass",
            local_python(root),
            &[
                &"-u",
                &root.join("toolchain/import_dlc.py"),
                &"--package",
                &package,
                &"--keys",
                &keys,
                &"--local",
                &local,
            ],
        );
        if reinstall {
            step.args.push("--reinstall".into());
        }
        self.start_steps_guarded(root, local, vec![step], &local.join("dlc.log"))
    }

    fn start_steps_guarded(
        &mut self,
        root: &Path,
        local: &Path,
        steps: Vec<Step>,
        log: &Path,
    ) -> Result<(), String> {
        if self.running() {
            return Err("A instalação de ferramentas já está em andamento.".into());
        }
        let guards = mk8_build_manager::acquire_operation(root, local)?;
        self.supervised = Some((root.into(), local.into()));
        self.guards = guards;
        let result = self.start(steps, log);
        if result.is_err() {
            self.guards.clear();
        }
        result?;
        Ok(())
    }
    pub fn running(&self) -> bool {
        self.state == State::Running
    }

    pub fn start(&mut self, steps: Vec<Step>, log: &Path) -> Result<(), String> {
        if self.running() {
            return Err("A instalação de ferramentas já está em andamento.".into());
        }
        if steps.is_empty() {
            return Err("Nenhuma etapa de instalação definida.".into());
        }
        if let Some(parent) = log.parent() {
            fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        File::create(log).map_err(|e| e.to_string())?;
        self.steps = steps;
        self.log = log.to_path_buf();
        self.current = 0;
        self.state = State::Running;
        self.spawn_current();
        Ok(())
    }

    fn append(&self, text: &str) {
        if let Ok(mut file) = OpenOptions::new().append(true).open(&self.log) {
            let _ = writeln!(file, "{text}");
        }
    }

    fn fail(&mut self, detail: String) {
        self.append(&detail);
        self.state = State::Failed;
        self.detail = detail;
        self.child = None;
        self.guards.clear();
    }

    fn spawn_current(&mut self) {
        let total = self.steps.len();
        let step = &self.steps[self.current];
        let name = step.name.clone();
        self.append(&format!("== Etapa {}/{total}: {name} ==", self.current + 1));
        let log = match OpenOptions::new().append(true).open(&self.log) {
            Ok(file) => file,
            Err(e) => return self.fail(format!("Não foi possível abrir o log: {e}")),
        };
        let mut cmd = Command::new(&step.program);
        cmd.args(&step.args)
            .env("PYTHONUTF8", "1")
            .env("PYTHONIOENCODING", "utf-8")
            .stdin(Stdio::null());
        match log.try_clone() {
            Ok(out) => {
                cmd.stdout(Stdio::from(out)).stderr(Stdio::from(log));
            }
            Err(e) => return self.fail(format!("Não foi possível abrir o log: {e}")),
        }
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
        }
        let spawned = if let Some((root, local)) = &self.supervised {
            self.guards.clear();
            mk8_build_manager::supervisor::spawn(&cmd, root, local, &self.log)
        } else {
            cmd.spawn().map_err(|e| e.to_string())
        };
        match spawned {
            Ok(child) => {
                self.child = Some(child);
                self.detail = format!("Etapa {}/{total}: {name}", self.current + 1);
            }
            Err(e) => self.fail(format!(
                "Etapa {}/{total} ({name}) não iniciou: {e}",
                self.current + 1
            )),
        }
    }

    /// Advances to the next step when the current one exits. Returns true when
    /// the run just finished (successfully or not), so callers can re-check tools.
    pub fn poll(&mut self) -> bool {
        let Some(child) = self.child.as_mut() else {
            return false;
        };
        match child.try_wait() {
            Ok(None) => false,
            Ok(Some(exit)) => {
                self.child = None;
                let total = self.steps.len();
                if !exit.success() {
                    let name = self.steps[self.current].name.clone();
                    self.fail(format!(
                        "Etapa {}/{total} ({name}) falhou ({exit}). Consulte o log, corrija e tente de novo.",
                        self.current + 1
                    ));
                    return true;
                }
                self.current += 1;
                if self.current == total {
                    self.state = State::Done;
                    self.guards.clear();
                    self.detail =
                        "Scripts concluídos sem erro; downloads conferidos por hash.".into();
                    self.append("== Concluído ==");
                    return true;
                }
                self.spawn_current();
                !self.running()
            }
            Err(e) => {
                self.detail = format!("Não foi possível consultar o processo: {e}");
                false
            }
        }
    }

    pub fn cancel(&mut self) -> Result<(), String> {
        let Some(child) = self.child.as_mut() else {
            return Ok(());
        };
        mk8_build_manager::terminate_tree(child)?;
        self.child = None;
        self.append("== Cancelado ==");
        self.state = State::Cancelled;
        self.guards.clear();
        self.detail = "Cancelado. Arquivos já baixados e conferidos serão reaproveitados.".into();
        Ok(())
    }

    pub fn status(&self) -> Value {
        let steps: Vec<Value> = self
            .steps
            .iter()
            .enumerate()
            .map(|(index, step)| {
                let state = match (index.cmp(&self.current), self.state) {
                    (std::cmp::Ordering::Less, _) => "done",
                    (std::cmp::Ordering::Equal, State::Running) => "running",
                    (std::cmp::Ordering::Equal, State::Failed) => "failed",
                    (std::cmp::Ordering::Equal, State::Cancelled) => "cancelled",
                    _ => "pending",
                };
                json!({"name": step.name, "state": state})
            })
            .collect();
        json!({
            "state": self.state.name(),
            "detail": self.detail,
            "completed": self.current.min(self.steps.len()),
            "total": self.steps.len(),
            "steps": steps,
        })
    }
}

fn check(name: String, ok: bool, detail: String) -> Value {
    json!({"name": name, "ok": ok, "detail": detail})
}

fn python_check(root: &Path) -> Value {
    let name = "Ambiente Python local".to_string();
    let mut cmd = Command::new(local_python(root));
    cmd.args([
        "-I", "-c",
        "import sys,struct,pip,capstone,lz4.block,unicorn,pefile,zstandard,Crypto; print(sys.version.split()[0],struct.calcsize('P')*8)",
    ])
    .stdin(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    match cmd.output() {
        Ok(out) if out.status.success() => {
            let text = String::from_utf8_lossy(&out.stdout).trim().to_string();
            let ok = text.starts_with("3.12.") && text.ends_with(" 64");
            check(name, ok, if ok {
                format!("Python {text} bits e dependências encontrados no ambiente local.")
            } else {
                "Ambiente Python incompatível. Execute Ferramentas para reparar automaticamente.".into()
            })
        }
        _ => check(name, false,
            "Ambiente Python ausente ou incompleto. Execute Ferramentas para instalar ou reparar automaticamente.".into()),
    }
}

/// Reads `NAME = 'value'` from a setup script so the expected paths follow it.
fn script_constant(text: &str, name: &str) -> Option<String> {
    text.lines().find_map(|line| {
        let rest = line
            .trim()
            .strip_prefix(name)?
            .trim_start()
            .strip_prefix('=')?;
        let rest = rest.trim();
        let quote = rest.chars().next().filter(|c| *c == '\'' || *c == '"')?;
        let inner = &rest[1..];
        Some(inner[..inner.find(quote)?].to_string())
    })
}

/// Pinned packages as `setup-runtime-deps.py` computes them: the pins listed in
/// the upstream fetch script plus the extra appended in the Python script.
pub fn pinned_packages(fetch_script: &str, deps_script: &str) -> Vec<(String, String)> {
    let is_hash = |s: &str| {
        s.len() == 64
            && s.bytes()
                .all(|b| b.is_ascii_hexdigit() && !b.is_ascii_uppercase())
    };
    let mut pins: Vec<(String, String)> = fetch_script
        .lines()
        .filter_map(|line| {
            let (hash, name) = line.split_once(' ')?;
            (is_hash(hash) && !name.is_empty() && !name.contains(char::is_whitespace))
                .then(|| (hash.to_string(), name.to_string()))
        })
        .collect();
    for line in deps_script.lines() {
        if let Some(rest) = line.trim().strip_prefix("pins.append((") {
            let parts: Vec<&str> = rest.split('\'').collect();
            if parts.len() >= 4 && is_hash(parts[1]) {
                pins.push((parts[1].to_string(), parts[3].to_string()));
            }
        }
    }
    pins
}

/// Zig, graphics libraries and FFmpeg: the folder shared by the launcher versions
/// (`MK8_NATIVE_TOOLS`, see `main::share_tools`), or `local/native-tools` when running from source.
fn tools_dir(root: &Path) -> std::path::PathBuf {
    std::env::var_os("MK8_NATIVE_TOOLS")
        .map(std::path::PathBuf::from)
        .unwrap_or_else(|| root.join("local/native-tools"))
}

pub fn tool_checks(root: &Path) -> Vec<Value> {
    let mut checks = vec![python_check(root)];
    let native = fs::read_to_string(root.join("scripts/setup-native.py")).unwrap_or_default();
    match script_constant(&native, "VERSION") {
        Some(version) => {
            let zig = tools_dir(root).join(format!("zig-x86_64-windows-{version}/zig.exe"));
            checks.push(check(
                format!("Compilador Zig {version}"),
                zig.is_file(),
                if zig.is_file() {
                    "Encontrado; o arquivo baixado é conferido por SHA-256 na instalação.".into()
                } else {
                    "Ainda não instalado.".into()
                },
            ));
        }
        None => checks.push(check(
            "Compilador Zig".into(),
            false,
            "Versão fixada não encontrada em scripts/setup-native.py.".into(),
        )),
    }
    let fetch = fs::read_to_string(root.join("upstream/smm2recomp/recomp/tools/fetch_deps.sh"))
        .unwrap_or_default();
    let deps = fs::read_to_string(root.join("scripts/setup-runtime-deps.py")).unwrap_or_default();
    let pins = pinned_packages(&fetch, &deps);
    let folder = tools_dir(root).join("deps");
    let extracted = pins
        .iter()
        .filter(|(hash, name)| {
            fs::read_to_string(folder.join(format!("{name}.done")))
                .is_ok_and(|marker| marker.trim() == hash)
        })
        .count();
    // Written by `runtime-dlls.py --prefetch` at the end of the step: the DLLs the build
    // copies are already downloaded and checked (older tool folders run the step once more).
    let prefetched = folder.join("dlls/index.json").is_file();
    checks.push(check(
        "Bibliotecas gráficas e FFmpeg".into(),
        !pins.is_empty() && extracted == pins.len() && prefetched,
        if pins.is_empty() {
            "Lista de pacotes fixados não encontrada.".into()
        } else if extracted == pins.len() && !prefetched {
            "Pacotes extraídos; falta baixar as DLLs usadas pelo jogo.".into()
        } else {
            format!(
                "{extracted} de {} pacotes extraídos com o hash esperado.",
                pins.len()
            )
        },
    ));
    checks
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cmd(name: &str, code: u8) -> Step {
        Step::new(
            name,
            "cmd.exe",
            &[&"/C", &format!("echo {name}& exit {code}")],
        )
    }

    fn wait(setup: &mut Setup) {
        for _ in 0..400 {
            if setup.poll() {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(25));
        }
        panic!("setup did not finish");
    }

    fn temp(name: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!("mk8-setup-{name}-{}", std::process::id()));
        let _ = fs::create_dir_all(&dir);
        dir.join("setup.log")
    }

    #[cfg(windows)]
    #[test]
    fn runs_all_steps_in_order_and_reports_done() {
        let log = temp("ok");
        let mut setup = Setup::default();
        setup
            .start(vec![cmd("primeira", 0), cmd("segunda", 0)], &log)
            .unwrap();
        assert_eq!(setup.status()["state"], "running");
        wait(&mut setup);
        let status = setup.status();
        assert_eq!(status["state"], "done");
        assert_eq!(status["completed"], 2);
        let text = fs::read_to_string(&log).unwrap();
        let first = text.find("primeira").unwrap();
        assert!(text[first..].contains("segunda"));
    }

    #[cfg(windows)]
    #[test]
    fn cancellation_stops_sequence_and_releases_operation_locks() {
        let log = temp("cancel");
        let local = log.parent().unwrap();
        let root = local.join("app");
        let mut setup = Setup::default();
        let guards = mk8_build_manager::acquire_operation(&root, local).unwrap();
        setup
            .start(
                vec![
                    Step::new(
                        "espera",
                        "powershell.exe",
                        &[
                            &"-NoProfile",
                            &"-NonInteractive",
                            &"-Command",
                            &"Start-Sleep -Seconds 60",
                        ],
                    ),
                    cmd("nao-executar", 0),
                ],
                &log,
            )
            .unwrap();
        setup.guards = guards;
        let locked = mk8_build_manager::acquire_operation(&root, local).is_err();
        setup.cancel().unwrap();
        assert!(locked);
        assert_eq!(setup.status()["state"], "cancelled");
        assert!(!setup.poll());
        assert!(!fs::read_to_string(&log).unwrap().contains("nao-executar"));
        assert!(mk8_build_manager::acquire_operation(&root, local).is_ok());
        setup.start(vec![cmd("retomada", 0)], &log).unwrap();
        wait(&mut setup);
        assert_eq!(setup.status()["state"], "done");
    }

    #[cfg(windows)]
    #[test]
    fn stops_at_failed_step_without_running_the_rest() {
        let log = temp("fail");
        let mut setup = Setup::default();
        setup
            .start(vec![cmd("um", 0), cmd("dois", 3), cmd("tres", 0)], &log)
            .unwrap();
        wait(&mut setup);
        let status = setup.status();
        assert_eq!(status["state"], "failed");
        assert_eq!(status["steps"][0]["state"], "done");
        assert_eq!(status["steps"][1]["state"], "failed");
        assert_eq!(status["steps"][2]["state"], "pending");
        assert!(status["detail"].as_str().unwrap().contains("2/3"));
        assert!(!fs::read_to_string(&log).unwrap().contains("tres"));
    }

    #[test]
    fn missing_program_is_a_failure_not_success() {
        let log = temp("missing");
        let mut setup = Setup::default();
        setup
            .start(
                vec![Step::new("ausente", "mk8-no-such-program.exe", &[])],
                &log,
            )
            .unwrap();
        assert_eq!(setup.status()["state"], "failed");
        assert!(!setup.poll());
    }

    #[test]
    fn python_requires_a_working_local_interpreter_not_just_a_file() {
        let root = temp("python-check").with_file_name("python-root");
        let python = local_python(&root);
        fs::create_dir_all(python.parent().unwrap()).unwrap();
        fs::write(&python, b"interrupted download").unwrap();
        assert_eq!(python_check(&root)["ok"], false);
        fs::remove_file(&python).unwrap();
        assert_eq!(python_check(&root)["ok"], false);
    }

    #[test]
    fn parses_pins_and_script_version() {
        let fetch = format!(
            "#!/bin/sh\n{} vulkan-loader-1.4\nnot a pin\n",
            "a".repeat(64)
        );
        let deps = format!(
            "    pins.append(('{}', 'ffmpeg-9.0.2-2'))\n",
            "b".repeat(64)
        );
        let pins = pinned_packages(&fetch, &deps);
        assert_eq!(pins.len(), 2);
        assert_eq!(pins[1].1, "ffmpeg-9.0.2-2");
        assert_eq!(
            script_constant("VERSION = '0.17.0'\nSHA256 = 'x'", "VERSION").as_deref(),
            Some("0.17.0")
        );
    }

    #[test]
    fn libraries_are_ready_only_after_the_dlls_were_prefetched() {
        if std::env::var_os("MK8_NATIVE_TOOLS").is_some() {
            return; // (checks the folder a source checkout uses)
        }
        let root = temp("checks").with_file_name("root");
        let _ = fs::remove_dir_all(&root);
        let deps = root.join("local/native-tools/deps");
        fs::create_dir_all(root.join("upstream/smm2recomp/recomp/tools")).unwrap();
        fs::create_dir_all(root.join("scripts")).unwrap();
        fs::create_dir_all(&deps).unwrap();
        let pin = "c".repeat(64);
        fs::write(
            root.join("upstream/smm2recomp/recomp/tools/fetch_deps.sh"),
            format!("{pin} sdl3-3.4\n"),
        )
        .unwrap();
        fs::write(root.join("scripts/setup-runtime-deps.py"), "").unwrap();
        fs::write(deps.join("sdl3-3.4.done"), &pin).unwrap();
        let libraries = |root: &Path| {
            tool_checks(root)
                .into_iter()
                .find(|c| c["name"] == "Bibliotecas gráficas e FFmpeg")
                .unwrap()
        };
        let before = libraries(&root);
        assert_eq!(before["ok"], false);
        assert!(before["detail"].as_str().unwrap().contains("DLLs"));
        fs::create_dir_all(deps.join("dlls")).unwrap();
        fs::write(deps.join("dlls/index.json"), "{}").unwrap();
        assert_eq!(libraries(&root)["ok"], true);
        let _ = fs::remove_dir_all(&root);
    }
}
