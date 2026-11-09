//! A separate process owns task lifetime and locks, independently of the launcher window.
use crate::acquire_operation;
use mk8_manifest::Result;
use serde::{Deserialize, Serialize};
use std::{
    ffi::OsString,
    fs,
    io::{BufRead, Write},
    path::{Path, PathBuf},
    process::{Child, Command, Stdio},
};

#[derive(Serialize, Deserialize)]
pub struct Task {
    pub root: PathBuf,
    pub local: PathBuf,
    pub log: PathBuf,
    pub parent: u32,
    pub program: OsString,
    pub args: Vec<OsString>,
    pub cwd: Option<PathBuf>,
    pub env: Vec<(OsString, Option<OsString>)>,
}

pub fn spawn(command: &Command, root: &Path, local: &Path, log: &Path) -> Result<Child> {
    let task = Task {
        root: root.into(),
        local: local.into(),
        log: log.into(),
        parent: std::process::id(),
        program: command.get_program().into(),
        args: command.get_args().map(Into::into).collect(),
        cwd: command.get_current_dir().map(Into::into),
        env: command
            .get_envs()
            .map(|(k, v)| (k.into(), v.map(Into::into)))
            .collect(),
    };
    spawn_request("--task-supervisor", &task)
}

pub fn spawn_request(mode: &str, request: &impl Serialize) -> Result<Child> {
    let mut cmd = Command::new(std::env::current_exe().map_err(|e| e.to_string())?);
    cmd.arg(mode)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
    }
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    let result = (|| {
        let mut input = child.stdin.take().ok_or("Supervisor sem entrada")?;
        serde_json::to_writer(&mut input, request).map_err(|e| e.to_string())?;
        input.write_all(b"\n").map_err(|e| e.to_string())?;
        drop(input);
        let mut reply = String::new();
        std::io::BufReader::new(child.stdout.take().ok_or("Supervisor sem resposta")?)
            .read_line(&mut reply)
            .map_err(|e| e.to_string())?;
        if reply.trim() != "READY" {
            return Err(reply.trim().to_owned());
        }
        Ok(())
    })();
    if let Err(error) = result {
        let _ = child.wait();
        return Err(format!("Não foi possível iniciar a operação: {error}"));
    }
    Ok(child)
}

pub fn read_request<T: serde::de::DeserializeOwned>() -> Result<T> {
    let mut line = String::new();
    std::io::stdin()
        .lock()
        .read_line(&mut line)
        .map_err(|e| e.to_string())?;
    serde_json::from_str(&line).map_err(|e| e.to_string())
}

pub fn ready() -> Result<()> {
    writeln!(std::io::stdout(), "READY").map_err(|e| e.to_string())?;
    std::io::stdout().flush().map_err(|e| e.to_string())
}

pub fn run_task() -> Result<i32> {
    let task: Task = read_request()?;
    #[cfg(windows)]
    let lifetime = windows::Lifetime::new(task.parent)?;
    let _guards = acquire_operation(&task.root, &task.local)?;
    let log = fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&task.log)
        .map_err(|e| e.to_string())?;
    let mut cmd = Command::new(task.program);
    cmd.args(task.args)
        .stdin(Stdio::null())
        .stdout(log.try_clone().map_err(|e| e.to_string())?)
        .stderr(log);
    if let Some(cwd) = task.cwd {
        cmd.current_dir(cwd);
    }
    for (key, value) in task.env {
        if let Some(value) = value {
            cmd.env(key, value);
        } else {
            cmd.env_remove(key);
        }
    }
    // Tool children must not inherit another Python installation or user site packages.
    // Keep script-directory imports available (unlike Python -I).
    let python_vars: Vec<_> = std::env::vars_os()
        .map(|(name, _)| name)
        .chain(cmd.get_envs().map(|(name, _)| name.to_os_string()))
        .filter(|name| {
            name.to_string_lossy()
                .to_ascii_uppercase()
                .starts_with("PYTHON")
        })
        .collect();
    for name in python_vars {
        cmd.env_remove(name);
    }
    cmd.env("PYTHONNOUSERSITE", "1")
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8");
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000);
        lifetime.check_parent()?;
    }
    let mut child = cmd.spawn().map_err(|e| e.to_string())?;
    // A broken handshake means the launcher disappeared; the job closes on process exit.
    ready()?;
    loop {
        if let Some(exit) = child.try_wait().map_err(|e| e.to_string())? {
            return Ok(exit.code().unwrap_or(1));
        }
        #[cfg(windows)]
        lifetime.check_parent()?;
        std::thread::sleep(std::time::Duration::from_millis(50));
    }
}

#[cfg(windows)]
mod windows {
    use super::Result;
    use windows_sys::Win32::{
        Foundation::*,
        System::{JobObjects::*, Threading::*},
    };
    pub struct Lifetime {
        parent: HANDLE,
    }
    impl Lifetime {
        pub fn new(pid: u32) -> Result<Self> {
            unsafe {
                let parent = OpenProcess(PROCESS_SYNCHRONIZE, 0, pid);
                if parent.is_null() {
                    return Err(std::io::Error::last_os_error().to_string());
                }
                let job = CreateJobObjectW(std::ptr::null(), std::ptr::null());
                if job.is_null() {
                    CloseHandle(parent);
                    return Err(std::io::Error::last_os_error().to_string());
                }
                let mut limits: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    (&limits as *const _) as _,
                    std::mem::size_of_val(&limits) as u32,
                ) == 0
                    || AssignProcessToJobObject(job, GetCurrentProcess()) == 0
                {
                    let error = std::io::Error::last_os_error().to_string();
                    CloseHandle(job);
                    CloseHandle(parent);
                    return Err(error);
                }
                // Intentionally retained until process exit: closing it here would kill this supervisor too.
                Ok(Self { parent })
            }
        }
        pub fn check_parent(&self) -> Result<()> {
            if unsafe { WaitForSingleObject(self.parent, 0) } != WAIT_TIMEOUT {
                return Err("Launcher encerrado; interrompendo ferramentas.".into());
            }
            Ok(())
        }
    }
    impl Drop for Lifetime {
        fn drop(&mut self) {
            unsafe {
                CloseHandle(self.parent);
            }
        }
    }
}
