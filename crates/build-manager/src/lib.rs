use mk8_manifest::Result;
use serde::Serialize;
use std::{
    fs,
    path::Path,
    process::{Child, Command, Stdio},
};

#[derive(Clone, Serialize)]
pub struct Status {
    pub state: String,
    pub detail: String,
    pub pid: Option<u32>,
}
pub struct Manager {
    child: Option<Child>,
    status: Status,
}
impl Default for Manager {
    fn default() -> Self {
        Self {
            child: None,
            status: Status {
                state: "idle".into(),
                detail: "Nenhuma preparação em andamento.".into(),
                pid: None,
            },
        }
    }
}
impl Manager {
    pub fn poll(&mut self) -> Status {
        if let Some(child) = self.child.as_mut() {
            match child.try_wait() {
                Ok(Some(exit)) => {
                    self.status = Status { state: if exit.success() { "candidate" } else { "failed" }.into(), detail: if exit.success() { "Build experimental ligada. Teste solo disponível; gameplay ainda não validado." } else { "Preparação interrompida. Consulte o diagnóstico e retome após corrigir." }.into(), pid: None };
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
        let log = fs::File::create(local.join("build.log")).map_err(|e| e.to_string())?;
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
            detail: "Preparando instalação; progresso no log.".into(),
            pid: Some(child.id()),
        };
        self.child = Some(child);
        Ok(())
    }
    pub fn record_start(&mut self, result: Result<()>) {
        if let Err(detail) = result {
            self.status = Status {
                state: "failed".into(),
                detail,
                pid: None,
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
            self.status = Status {
                state: "cancelled".into(),
                detail: "Cancelado. Etapas concluídas serão reutilizadas.".into(),
                pid: None,
            };
        }
        Ok(())
    }
}
