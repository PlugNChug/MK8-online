use mk8_manifest::Result;
use serde::Serialize;
use std::{
    fs,
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
    time::{Instant, SystemTime, UNIX_EPOCH},
};

#[derive(Clone, Serialize)]
pub struct Status {
    pub state: String,
    pub detail: String,
    pub pid: Option<u32>,
    /// 0-100, estimated from the build log's stage lines (`progress`).
    pub percent: f64,
    /// The stage in plain words, for the progress screen.
    pub phase: String,
    /// When the running or last build started (Unix milliseconds): the page counts the elapsed time from it.
    pub started_ms: Option<u64>,
    /// Seconds the last build took, once it ended.
    pub duration_s: Option<f64>,
}
pub struct Manager {
    child: Option<Child>,
    status: Status,
    log: Option<PathBuf>,
    started: Option<Instant>,
}
impl Default for Manager {
    fn default() -> Self {
        Self {
            child: None,
            status: Status {
                state: "idle".into(),
                detail: "Nenhuma preparação em andamento.".into(),
                pid: None,
                percent: 0.0,
                phase: String::new(),
                started_ms: None,
                duration_s: None,
            },
            log: None,
            started: None,
        }
    }
}

/// Where a build is, from its log: (percent, phase). Shares measured on a first build from packages (1218 s:
/// import and translation 261 s, native compilation 660 s, linking and game data 297 s): import 0-8 %
/// (skipped when reused), runtime adaptation 8-9 %, translation of the game modules to C 9-21 %
/// (`<module>: progress done/total`), native compilation 21-75 % (`Compilação nativa: i/n unidades`),
/// linking 75-90 % (a line per module, then the runtime libraries), game data 90-100 %.
pub fn progress(log: &str) -> (f64, String) {
    const TRANSLATE: &str = "Traduzindo o código do jogo";
    const COMPILE: &str = "Compilando para o seu PC";
    const LINK: &str = "Ligando o jogo";
    let mut percent: f64 = 0.0;
    let mut phase = "Iniciando".to_string();
    let mut modules: Vec<(String, f64)> = Vec::new();
    let fraction = |text: &str| -> Option<(f64, f64)> {
        let (a, b) = text.split_whitespace().next()?.split_once('/')?;
        Some((a.parse().ok()?, b.parse::<f64>().ok()?.max(1.0)))
    };
    for line in log.lines() {
        let line = line.trim();
        let mut set = |p: f64, name: &str| {
            percent = percent.max(p);
            phase = name.into();
        };
        if line.starts_with("Importação: reconstruindo") {
            set(1.0, "Lendo os pacotes do jogo");
        } else if line.starts_with("Importação: aplicando") {
            set(3.0, "Extraindo e verificando os arquivos do jogo");
        } else if line.starts_with("Importação 4.0.1 verificada")
            || line.starts_with("Importação: reaproveitando")
        {
            set(8.0, "Arquivos do jogo verificados");
        } else if line.starts_with("Etapa: validar e adaptar runtime") {
            set(8.5, "Preparando o motor do jogo");
        } else if line.starts_with("Etapa: traduzir") {
            set(9.0, TRANSLATE);
        } else if let Some((module, rest)) = line.split_once(": progress ") {
            if let Some((done, total)) = fraction(rest) {
                let value = (done / total).min(1.0);
                match modules.iter_mut().find(|(m, _)| m == module) {
                    Some(entry) => entry.1 = entry.1.max(value),
                    None => modules.push((module.to_string(), value)),
                }
                let all = modules.iter().map(|(_, f)| f).sum::<f64>() / modules.len().max(3) as f64;
                set(9.0 + 12.0 * all, TRANSLATE);
            }
        } else if let Some((module, _)) = line.split_once(": wrote ") {
            match modules.iter_mut().find(|(m, _)| m == module) {
                Some(entry) => entry.1 = 1.0,
                None => modules.push((module.to_string(), 1.0)),
            }
        } else if line.starts_with("Geração C concluída") {
            set(21.0, COMPILE);
        } else if let Some(rest) = line.strip_prefix("Compilação nativa: ") {
            let share = fraction(rest).map_or(0.0, |(i, n)| (i / n).min(1.0));
            set(21.0 + 54.0 * share, COMPILE);
        } else if line.starts_with("Compilação nativa concluída") {
            set(75.0, LINK);
        } else if let Some(module) = line.strip_prefix("Ligando módulo ") {
            let step = match module.trim_end_matches('.') {
                "sdk" => 77.0,
                "subsdk0" => 80.0,
                _ => 83.0,
            };
            set(step, LINK);
        } else if line.starts_with("Dependência:") {
            set(88.0, "Copiando as bibliotecas do motor");
        } else if line.starts_with("Etapa: preparar os dados do jogo") {
            set(90.0, "Preparando os dados do jogo");
        } else if line.starts_with("Build experimental integrada")
            || line.starts_with("Runtime e ligação:")
        {
            set(100.0, "Concluído");
        }
    }
    (percent.min(100.0), phase)
}

fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}

impl Manager {
    pub fn poll(&mut self) -> Status {
        if self.child.is_some() {
            if let Some(log) = self.log.as_ref().and_then(|p| fs::read(p).ok()) {
                let (percent, phase) = progress(&String::from_utf8_lossy(&log));
                self.status.percent = percent.min(99.0);
                self.status.phase = phase;
            }
        }
        if let Some(child) = self.child.as_mut() {
            match child.try_wait() {
                Ok(Some(exit)) => {
                    let ok = exit.success();
                    self.status.state = if ok { "candidate" } else { "failed" }.into();
                    self.status.detail = if ok {
                        "Build experimental ligada. Teste solo disponível; gameplay ainda não validado."
                    } else {
                        "Preparação interrompida. Consulte o log; ao tentar de novo, as etapas concluídas são reaproveitadas."
                    }
                    .into();
                    self.status.pid = None;
                    if ok {
                        self.status.percent = 100.0;
                        self.status.phase = "Concluído".into();
                    }
                    self.status.duration_s = self.started.map(|t| t.elapsed().as_secs_f64());
                    self.child = None;
                }
                Err(e) => {
                    self.status.detail = e.to_string();
                }
                _ => {}
            }
        }
        self.status.clone()
    }
    pub fn start(&mut self, root: &Path, config: &Path, local: &Path) -> Result<()> {
        self.poll();
        if self.child.is_some() {
            return Err("Já existe uma preparação em andamento.".into());
        }
        fs::create_dir_all(local).map_err(|e| e.to_string())?;
        let log_path = local.join("build.log");
        let log = fs::File::create(&log_path).map_err(|e| e.to_string())?;
        let python = root.join("local/python/Scripts/python.exe");
        let mut cmd = if python.is_file() {
            Command::new(python)
        } else {
            Command::new("python")
        };
        cmd.env("PYTHONUTF8", "1").env("PYTHONIOENCODING", "utf-8");
        cmd.arg("-u")
            .arg(root.join("toolchain/build.py"))
            .arg("--config")
            .arg(config)
            .arg("--local")
            .arg(local)
            .current_dir(root)
            .stdin(Stdio::null())
            .stdout(Stdio::from(log.try_clone().map_err(|e| e.to_string())?))
            .stderr(Stdio::from(log));
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x08000000);
        }
        let child = cmd
            .spawn()
            .map_err(|e| format!("Python não iniciou: {e}"))?;
        self.status = Status {
            state: "running".into(),
            detail: "Preparando o jogo.".into(),
            pid: Some(child.id()),
            percent: 0.0,
            phase: "Iniciando".into(),
            started_ms: Some(now_ms()),
            duration_s: None,
        };
        self.child = Some(child);
        self.log = Some(log_path);
        self.started = Some(Instant::now());
        Ok(())
    }
    pub fn record_start(&mut self, result: Result<()>) {
        if let Err(detail) = result {
            self.status = Status {
                state: "failed".into(),
                detail,
                ..Status {
                    pid: None,
                    ..self.status.clone()
                }
            };
        }
    }

    pub fn cancel(&mut self) -> Result<()> {
        if let Some(mut child) = self.child.take() {
            #[cfg(windows)]
            {
                let status = Command::new("taskkill")
                    .args(["/PID", &child.id().to_string(), "/T", "/F"])
                    .output()
                    .map_err(|e| e.to_string())?;
                if !status.status.success()
                    && child.try_wait().map_err(|e| e.to_string())?.is_none()
                {
                    self.child = Some(child);
                    return Err("Não foi possível cancelar a árvore de build".into());
                }
            }
            #[cfg(not(windows))]
            child.kill().map_err(|e| e.to_string())?;
            let _ = child.wait();
            self.status.state = "cancelled".into();
            self.status.detail = "Cancelado. Etapas concluídas serão reutilizadas.".into();
            self.status.pid = None;
            self.status.duration_s = self.started.map(|t| t.elapsed().as_secs_f64());
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::progress;

    #[test]
    fn progress_follows_the_build_stages() {
        assert_eq!(progress("").0, 0.0);
        let import = "Importação: reconstruindo os arquivos NCA dos pacotes.\nImportação: aplicando atualização\n";
        let (p, phase) = progress(import);
        assert_eq!(p, 3.0);
        assert!(phase.starts_with("Extraindo"));
        let mut log = String::from(import);
        log += "Importação 4.0.1 verificada.\nMK8: verificando entradas e runtime.\nEtapa: validar e adaptar runtime\n";
        log += "Etapa: traduzir AArch64 para C e preparar runtime Windows em paralelo.\nRuntime Windows: 13/210 unidades\n";
        assert_eq!(progress(&log).0, 9.0);
        // One of three modules half done: a sixth of the translation share.
        log += "subsdk0: progress 500/1000 files 3\n";
        let (p, phase) = progress(&log);
        assert!((p - 11.0).abs() < 1e-9, "{p}");
        assert_eq!(phase, "Traduzindo o código do jogo");
        log += "subsdk0: wrote 115 chunks, 11s total\nsdk: progress 10/20 files 1\nGeração C concluída em 261.3 s.\n";
        assert_eq!(progress(&log).0, 21.0);
        log += "Compilação nativa: 8 compiladores, 484 unidades.\nCompilação nativa: 242/484 unidades\n";
        let (p, phase) = progress(&log);
        assert!((p - 48.0).abs() < 1e-9, "{p}");
        assert_eq!(phase, "Compilando para o seu PC");
        log += "Compilação nativa concluída em 660.1 s.\n";
        assert_eq!(progress(&log).0, 75.0);
        log += "Ligando módulo sdk...\nLigando módulo main...\nDependência: libgcc\n";
        assert_eq!(progress(&log).0, 88.0);
        log += "Etapa: preparar os dados do jogo.\n";
        assert_eq!(progress(&log), (90.0, "Preparando os dados do jogo".into()));
        log += "Build experimental integrada. Use Testar solo experimental.\n";
        assert_eq!(progress(&log), (100.0, "Concluído".into()));
    }

    #[test]
    fn a_reused_import_and_out_of_order_lines_never_go_back() {
        let log = "Importação: reaproveitando arquivos extraídos.\nCompilação nativa: 400/484 unidades\nsubsdk0: progress 1/10 files 1\n";
        assert!(progress(log).0 > 65.0);
        assert_eq!(
            progress("Importação: reaproveitando arquivos extraídos.\n").0,
            8.0
        );
    }

    #[test]
    fn the_real_log_of_a_first_build_reaches_every_stage_in_order() {
        // Stage lines of the 1218 s build this calibration comes from (progress lines thinned).
        let log = include_str!("../tests/first-build.log");
        let mut previous = 0.0;
        let mut text = String::new();
        for line in log.lines() {
            text += line;
            text += "\n";
            let (p, _) = progress(&text);
            assert!(p >= previous, "{line}: {p} < {previous}");
            previous = p;
        }
        assert_eq!(previous, 100.0);
    }
}
