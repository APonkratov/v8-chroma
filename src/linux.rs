//! Linux x64 Designer discovery, live attachment and optional audit-mode launch.
//! System preload settings and platform files are never modified.
use crate::{
    process::Identity,
    storage::{Lock, atomic_write},
    theme,
};
use anyhow::{Context, Result, bail, ensure};
use directories::ProjectDirs;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    ffi::OsString,
    fs::{self, File},
    io::Read,
    os::unix::fs::{DirBuilderExt, MetadataExt, PermissionsExt},
    os::unix::process::CommandExt,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
mod appearance;

#[derive(Debug, Serialize)]
pub struct Candidate {
    pub pid: u32,
    pub identity: Option<Identity>,
    pub eligible: bool,
    pub managed: bool,
    pub reason: String,
}
#[derive(Debug, Serialize, Deserialize)]
pub struct Status {
    pub schema: u32,
    pub pid: u32,
    pub theme: String,
    pub generation: u32,
    pub hook_mask: u32,
    pub observed_calls: u64,
    pub mapped_calls: u64,
    pub error: String,
    #[serde(default)]
    pub icon_mode: u32,
    #[serde(default)]
    pub icons_available: bool,
    #[serde(default)]
    pub icon_calls: u64,
    #[serde(default)]
    pub icon_matches: u64,
    #[serde(default)]
    pub fonts_available: u32,
    #[serde(default)]
    pub font_generation: u32,
    #[serde(default)]
    pub font_refresh: u32,
    #[serde(default)]
    pub font_error: String,
    #[serde(default)]
    pub editor_enabled: bool,
    #[serde(default)]
    pub ui_enabled: bool,
    #[serde(default)]
    pub ui_fallback: bool,
    #[serde(default)]
    pub editor_points10: u32,
    #[serde(default)]
    pub editor_family: String,
    #[serde(default)]
    pub ui_family: String,
}
#[derive(Debug, Serialize, Deserialize)]
pub struct Session {
    pub schema: u32,
    pub identity: Identity,
    pub engine: PathBuf,
    pub engine_sha256: String,
    pub config: PathBuf,
    pub console: PathBuf,
}
#[derive(Serialize)]
pub struct Applied {
    pub identity: Identity,
    pub requested_theme: String,
    pub confirmed: bool,
    pub status: Option<Status>,
}

fn uid() -> Result<u32> {
    Ok(fs::metadata("/proc/self")?.uid())
}
fn private_dir(path: &Path) -> Result<()> {
    fs::DirBuilder::new()
        .recursive(true)
        .mode(0o700)
        .create(path)?;
    let metadata = fs::symlink_metadata(path)?;
    ensure!(
        metadata.is_dir() && metadata.uid() == uid()?,
        "Каталог должен принадлежать текущему пользователю"
    );
    fs::set_permissions(path, fs::Permissions::from_mode(0o700))?;
    Ok(())
}
fn elf(path: &Path) -> Result<()> {
    let mut header = [0u8; 64];
    File::open(path)?.read_exact(&mut header)?;
    ensure!(
        header[..7] == *b"\x7fELF\x02\x01\x01"
            && header[18..20] == [62, 0]
            && matches!(u16::from_le_bytes([header[16], header[17]]), 2 | 3),
        "Ожидался ELF Linux x64: {}",
        path.display()
    );
    Ok(())
}
pub fn validate_platform(path: &Path) -> Result<PathBuf> {
    let path = fs::canonicalize(path)?;
    ensure!(
        path.file_name().is_some_and(|name| name == "1cv8"),
        "Ожидался Linux 1cv8"
    );
    elf(&path)?;
    let parent = path.parent().context("Нет каталога платформы")?;
    for library in ["core83.so", "frame.so", "grphcs.so", "libcairo-v8.so"] {
        elf(&parent.join(library)).with_context(|| format!("Неполная платформа: {library}"))?;
    }
    Ok(path)
}
fn start_time(stat: &str) -> Result<u64> {
    // comm is parenthesized and may contain spaces and ')' characters.
    stat.rsplit_once(')')
        .context("Некорректный /proc/stat")?
        .1
        .split_whitespace()
        .nth(19)
        .context("Нет starttime в /proc/stat")?
        .parse()
        .context("Некорректный starttime")
}
fn identity(pid: u32) -> Result<Identity> {
    let root = PathBuf::from(format!("/proc/{pid}"));
    ensure!(
        fs::metadata(&root)?.uid() == uid()?,
        "Процесс другого пользователя"
    );
    let before = start_time(&fs::read_to_string(root.join("stat"))?)?;
    let command = fs::read(root.join("cmdline"))?;
    let mode = command.split(|byte| *byte == 0).nth(1).unwrap_or_default();
    ensure!(mode.eq_ignore_ascii_case(b"DESIGNER"), "Режим не DESIGNER");
    let exe = validate_platform(&fs::read_link(root.join("exe"))?)?;
    ensure!(
        start_time(&fs::read_to_string(root.join("stat"))?)? == before,
        "Процесс изменился во время проверки"
    );
    Ok(Identity {
        pid,
        created_time: before,
        exe,
    })
}
fn digest(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

pub struct Manager {
    root: PathBuf,
}
impl Manager {
    pub fn new(directory: Option<PathBuf>) -> Result<Self> {
        let root = match directory {
            Some(path) => path,
            None => ProjectDirs::from("", "", crate::APP_NAME)
                .context("Не найден пользовательский каталог")?
                .data_local_dir()
                .join("linux"),
        };
        private_dir(&root)?;
        let root = fs::canonicalize(root)?;
        for name in ["sessions", "runs", "runtime"] {
            private_dir(&root.join(name))?;
        }
        Ok(Self { root })
    }
    fn session_path(&self, pid: u32) -> PathBuf {
        self.root.join("sessions").join(format!("{pid}.json"))
    }
    pub fn has_session(&self, pid: u32) -> bool {
        self.session_path(pid).exists()
    }
    fn session(&self, pid: u32) -> Result<Session> {
        let session: Session = serde_json::from_slice(&fs::read(self.session_path(pid))?)?;
        ensure!(
            session.schema == 1 && session.identity == identity(pid)?,
            "Сессия относится к другому или завершённому процессу"
        );
        let config = fs::canonicalize(&session.config)?;
        ensure!(
            config.starts_with(self.root.join("runs")) && config == session.config,
            "Конфигурация вне каталога сессий"
        );
        ensure!(
            digest(&fs::read(&session.engine)?) == session.engine_sha256,
            "Изменилась библиотека движка; перезапустите Конфигуратор"
        );
        let maps = fs::read_to_string(format!("/proc/{pid}/maps"))?;
        let inode = fs::metadata(&session.engine)?.ino();
        // /proc/maps leaves spaces literal. Do not split the pathname column;
        // runtime paths with newlines are rejected before invoking the loader.
        let mapped_path = session
            .engine
            .to_str()
            .context("Путь движка должен быть Unicode")?;
        ensure!(
            maps.lines().any(|line| line
                .split_whitespace()
                .nth(4)
                .and_then(|value| value.parse::<u64>().ok())
                == Some(inode)
                && line.ends_with(mapped_path)),
            "Движок не загружен в процесс"
        );
        Ok(session)
    }
    pub fn discover(&self) -> Result<Vec<Candidate>> {
        let mut candidates = Vec::new();
        for entry in fs::read_dir("/proc")? {
            let entry = entry?;
            let Some(pid) = entry
                .file_name()
                .to_str()
                .and_then(|name| name.parse::<u32>().ok())
            else {
                continue;
            };
            let Ok(exe) = fs::read_link(entry.path().join("exe")) else {
                continue;
            };
            if !exe.file_name().is_some_and(|name| name == "1cv8") {
                continue;
            }
            let inspected = identity(pid);
            let managed = inspected.is_ok() && self.session(pid).is_ok();
            let (identity, reason) = match inspected {
                Ok(value) => (
                    Some(value),
                    if managed {
                        "Движок загружен"
                    } else {
                        "Готов к подключению"
                    }
                    .into(),
                ),
                Err(error) => (None, format!("{error:#}")),
            };
            candidates.push(Candidate {
                pid,
                eligible: identity.is_some(),
                identity,
                managed,
                reason,
            });
        }
        candidates.sort_by_key(|candidate| candidate.pid);
        Ok(candidates)
    }
    /// Connect to an ordinary, already-running Designer of the same user.
    pub fn attach_to(
        &self,
        expected: &Identity,
        selected: &str,
        engine: &Path,
        injector: &Path,
    ) -> Result<Applied> {
        self.attach_selection(expected, selected, None, engine, injector)
    }
    pub fn attach_to_with_icons(
        &self,
        expected: &Identity,
        selected: &str,
        custom_icons: bool,
        engine: &Path,
        injector: &Path,
    ) -> Result<Applied> {
        self.attach_selection(expected, selected, Some(custom_icons), engine, injector)
    }
    fn attach_selection(
        &self,
        expected: &Identity,
        selected: &str,
        custom_icons: Option<bool>,
        engine: &Path,
        injector: &Path,
    ) -> Result<Applied> {
        ensure!(
            identity(expected.pid)? == *expected,
            "Процесс изменился; обновите список Конфигураторов"
        );
        if self.session(expected.pid).is_ok() {
            let supported = self
                .status(expected.pid)
                .is_ok_and(|status| status.icons_available);
            return self.apply_selection(
                expected.pid,
                selected,
                custom_icons.map(|icons| icons && supported),
            );
        }
        let config_bytes = theme::config_with_icons(
            selected,
            Path::new("x"),
            Path::new("y"),
            custom_icons.unwrap_or(false),
        )?;
        let engine = fs::canonicalize(engine)?;
        let injector = fs::canonicalize(injector)?;
        elf(&engine)?;
        elf(&injector)?;
        let _lock = Lock::acquire(&self.root.join("manager.lock"))?;
        // Another command may have completed an attachment while we waited.
        if self.session(expected.pid).is_ok() {
            drop(_lock);
            let supported = self
                .status(expected.pid)
                .is_ok_and(|status| status.icons_available);
            return self.apply_selection(
                expected.pid,
                selected,
                custom_icons.map(|icons| icons && supported),
            );
        }
        let bytes = fs::read(&engine)?;
        let hash = digest(&bytes);
        let runtime = self.root.join("runtime").join(format!("{hash}.so"));
        if runtime.exists() {
            ensure!(
                digest(&fs::read(&runtime)?) == hash,
                "Повреждён runtime движка"
            );
        } else {
            atomic_write(&runtime, &bytes)?;
        }
        let helper_bytes = fs::read(&injector)?;
        let helper = self
            .root
            .join("runtime")
            .join(format!("{}.injector", digest(&helper_bytes)));
        if helper.exists() {
            ensure!(
                fs::read(&helper)? == helper_bytes,
                "Повреждён загрузчик движка"
            );
        } else {
            atomic_write(&helper, &helper_bytes)?;
        }
        fs::set_permissions(&helper, fs::Permissions::from_mode(0o700))?;
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let directory = self
            .root
            .join("runs")
            .join(format!("{}-{nonce}", std::process::id()));
        private_dir(&directory)?;
        let config = directory.join("config.bin");
        atomic_write(&config, config_bytes.bytes())?;
        let console = directory.join("console.log");
        ensure!(
            identity(expected.pid)? == *expected,
            "Процесс изменился перед подключением"
        );
        let output = Command::new(&helper)
            .arg(expected.pid.to_string())
            .arg(expected.created_time.to_string())
            .arg(&runtime)
            .arg(&config)
            .stdin(Stdio::null())
            .output()
            .context("Не удалось запустить загрузчик Linux")?;
        fs::write(&console, &output.stderr)?;
        ensure!(
            output.status.success(),
            "Не удалось подключить движок: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        );
        ensure!(
            identity(expected.pid)? == *expected,
            "Процесс изменился во время подключения"
        );
        let status = read_status(&config)?;
        ensure!(
            status.pid == expected.pid && status.error.is_empty() && status.hook_mask & 11 == 11,
            "Движок не подтвердил перехваты отрисовки: {}",
            status.error
        );
        let session = Session {
            schema: 1,
            identity: expected.clone(),
            engine: runtime,
            engine_sha256: hash,
            config,
            console,
        };
        atomic_write(
            &self.session_path(expected.pid),
            &serde_json::to_vec_pretty(&session)?,
        )?;
        Ok(Applied {
            identity: expected.clone(),
            requested_theme: selected.into(),
            confirmed: status.theme == selected,
            status: Some(status),
        })
    }
    pub fn attach(
        &self,
        pid: u32,
        selected: &str,
        engine: &Path,
        injector: &Path,
    ) -> Result<Applied> {
        self.attach_to(&identity(pid)?, selected, engine, injector)
    }
    pub fn launch(
        &self,
        platform: &Path,
        engine: &Path,
        selected: &str,
        arguments: &[OsString],
    ) -> Result<Session> {
        let platform = validate_platform(platform)?;
        let engine = fs::canonicalize(engine)?;
        elf(&engine)?;
        let bytes = fs::read(&engine)?;
        let hash = digest(&bytes);
        let _lock = Lock::acquire(&self.root.join("manager.lock"))?;
        let runtime = self.root.join("runtime").join(format!("{hash}.so"));
        if runtime.exists() {
            ensure!(
                digest(&fs::read(&runtime)?) == hash,
                "Повреждён runtime движка"
            );
        } else {
            atomic_write(&runtime, &bytes)?;
        }
        let runtime_text = runtime
            .to_str()
            .context("Путь движка должен быть Unicode")?;
        ensure!(
            !runtime_text.contains([':', '$', '\n']),
            "Путь движка содержит специальные символы загрузчика"
        );
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let directory = self
            .root
            .join("runs")
            .join(format!("{}-{nonce}", std::process::id()));
        private_dir(&directory)?;
        let config = directory.join("config.bin");
        atomic_write(
            &config,
            theme::config(selected, Path::new("x"), Path::new("y"))?.bytes(),
        )?;
        let console = directory.join("console.log");
        let output = File::create(&console)?;
        let mut command = Command::new(&platform);
        // WSL tears down the invoking login session when its CLI exits. Only
        // async-signal-safe libc calls are made between fork and exec.
        unsafe {
            command.pre_exec(|| {
                if libc::setsid() == -1
                    || libc::signal(libc::SIGHUP, libc::SIG_IGN) == libc::SIG_ERR
                {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
        let mut child = command
            .arg("DESIGNER")
            .args(arguments)
            .env("LD_AUDIT", &runtime)
            .env("V8_CHROMA_CONFIG", &config)
            .env(
                "V8_CHROMA_CAIRO",
                platform
                    .parent()
                    .context("Нет каталога платформы")?
                    .join("libcairo-v8.so"),
            )
            .stdin(Stdio::null())
            .stdout(output.try_clone()?)
            .stderr(output)
            .spawn()?;
        let deadline = Instant::now() + Duration::from_secs(10);
        let inspected = loop {
            if let Some(code) = child.try_wait()? {
                bail!("1С завершилась ({code}); журнал: {}", console.display());
            }
            let current = identity(child.id());
            if let Ok(value) = current
                && let Ok(receipt) = read_status(&config)
                && receipt.pid == child.id()
                && receipt.error.is_empty()
            {
                break Ok(value);
            }
            if Instant::now() >= deadline {
                break Err(anyhow::anyhow!(
                    "Загрузчик не подтвердил запуск; журнал: {}",
                    console.display()
                ));
            }
            std::thread::sleep(Duration::from_millis(50));
        };
        let identity = match inspected {
            Ok(value) => value,
            Err(error) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(error);
            }
        };
        let session = Session {
            schema: 1,
            identity,
            engine: runtime,
            engine_sha256: hash,
            config,
            console,
        };
        atomic_write(
            &self.session_path(session.identity.pid),
            &serde_json::to_vec_pretty(&session)?,
        )?;
        Ok(session)
    }
    pub fn status(&self, pid: u32) -> Result<Status> {
        let session = self.session(pid)?;
        let status = read_status(&session.config)?;
        ensure!(
            status.schema == 1 && status.pid == pid,
            "Некорректный ответ движка"
        );
        Ok(status)
    }
    pub fn status_to(&self, expected: &Identity) -> Result<Status> {
        ensure!(
            identity(expected.pid)? == *expected,
            "Идентичность процесса изменилась"
        );
        self.status(expected.pid)
    }
    pub fn requested_theme(&self, pid: u32) -> Result<String> {
        let bytes = fs::read(self.session(pid)?.config)?;
        ensure!(bytes.len() == 4760, "Некорректная конфигурация движка");
        let text = &bytes[16..80];
        let end = text
            .iter()
            .position(|byte| *byte == 0)
            .context("Некорректная тема движка")?;
        Ok(std::str::from_utf8(&text[..end])?.to_owned())
    }
    pub fn requested_icon_mode(&self, pid: u32) -> Result<u32> {
        let bytes = fs::read(self.session(pid)?.config)?;
        ensure!(bytes.len() == 4760, "Некорректная конфигурация движка");
        Ok(u32::from_le_bytes(bytes[4752..4756].try_into()?))
    }
    pub fn apply(&self, pid: u32, selected: &str) -> Result<Applied> {
        self.apply_selection(pid, selected, None)
    }
    pub fn apply_with_icons(
        &self,
        pid: u32,
        selected: &str,
        custom_icons: bool,
    ) -> Result<Applied> {
        self.apply_selection(pid, selected, Some(custom_icons))
    }
    fn apply_selection(
        &self,
        pid: u32,
        selected: &str,
        custom_icons: Option<bool>,
    ) -> Result<Applied> {
        let _lock = Lock::acquire(&self.root.join("manager.lock"))?;
        let session = self
            .session(pid)
            .context("Нет управляемой сессии; подключите Конфигуратор через attach")?;
        let custom_icons = match custom_icons {
            Some(icons) => icons,
            None => self.requested_icon_mode(pid)? != 0,
        };
        let request =
            theme::config_with_icons(selected, Path::new("x"), Path::new("y"), custom_icons)?;
        let expected_icon_mode = request.icon_mode;
        atomic_write(&session.config, request.bytes())?;
        let deadline = Instant::now() + Duration::from_millis(500);
        let mut status = self.status(pid).ok();
        loop {
            if status.as_ref().is_some_and(|value| {
                value.theme == selected
                    && value.error.is_empty()
                    && value.icon_mode == expected_icon_mode
            }) || Instant::now() >= deadline
            {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
            status = self.status(pid).ok();
        }
        Ok(Applied {
            identity: session.identity,
            requested_theme: selected.into(),
            confirmed: status.as_ref().is_some_and(|value| {
                value.theme == selected
                    && value.error.is_empty()
                    && value.icon_mode == expected_icon_mode
            }),
            status,
        })
    }
}
fn read_status(config: &Path) -> Result<Status> {
    let mut path = config.as_os_str().to_os_string();
    path.push(".status");
    Ok(serde_json::from_slice(&fs::read(PathBuf::from(path))?)?)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parses_starttime_with_spaces_and_parentheses_in_comm() {
        let tail = (3..=21).map(|_| "0").collect::<Vec<_>>().join(" ");
        assert_eq!(
            start_time(&format!("42 (a ) b) {tail} 123456 0")).unwrap(),
            123456
        );
        assert!(start_time("42 (bad)").is_err());
    }
    #[test]
    fn rejects_non_elf_and_wrong_architecture() {
        let temporary = tempfile::tempdir().unwrap();
        let binary = temporary.path().join("1cv8");
        fs::write(&binary, [0; 64]).unwrap();
        assert!(elf(&binary).is_err());
        let mut header = [0; 64];
        header[..7].copy_from_slice(b"\x7fELF\x02\x01\x01");
        header[16] = 3;
        header[18] = 62;
        fs::write(&binary, header).unwrap();
        assert!(elf(&binary).is_ok());
        assert!(validate_platform(&binary).is_err());
        header[18] = 183;
        fs::write(&binary, header).unwrap();
        assert!(elf(&binary).is_err());
    }
    #[test]
    fn runtime_directory_is_private() {
        let temporary = tempfile::tempdir().unwrap();
        let root = temporary.path().join("profile with spaces");
        let manager = Manager::new(Some(root.clone())).unwrap();
        assert_eq!(fs::metadata(&root).unwrap().mode() & 0o777, 0o700);
        assert!(manager.session(u32::MAX).is_err());
    }
}
