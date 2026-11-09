use crate::{
    engine_dll,
    storage::{Lock, atomic_write},
    theme,
    win::{self, Identity, Process, ProcessExited},
};
use anyhow::{Context, Result, ensure};
use directories::ProjectDirs;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::{
    fs,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

const ENGINE_MISMATCH: &str = "Версия движка этого процесса отличается от менеджера. Штатно перезапустите этот конфигуратор для обновления DLL темы";
const DLL_NAME: &str = "ThemeProbe.dll";
const THEME_WAIT: Duration = Duration::from_secs(30);
const THEME_MODULES: [&str; 3] = ["frame.dll", "grphcs.dll", "cairo.dll"];

#[derive(Debug)]
pub struct ThemeNotReady;
impl std::fmt::Display for ThemeNotReady {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("Модули темы ещё загружаются. Повторите применение после запуска Конфигуратора")
    }
}
impl std::error::Error for ThemeNotReady {}

#[derive(Default)]
pub struct DiscoveryFailures {
    consecutive: usize,
}

impl DiscoveryFailures {
    pub fn recovered(&mut self) {
        self.consecutive = 0;
    }

    pub fn should_report(&mut self, error: &anyhow::Error) -> bool {
        const REPORT_AFTER: usize = 3;
        let transient = match error.downcast_ref::<wmi::WMIError>() {
            Some(wmi::WMIError::HResultError { hres }) => matches!(
                *hres as u32,
                0x80041015 // WBEM_E_TRANSPORT_FAILURE
                    | 0x80041069 // WBEM_E_TIMED_OUT
                    | 0x8001010A // RPC_E_SERVERCALL_RETRYLATER
                    | 0x800706BA // HRESULT_FROM_WIN32(RPC_S_SERVER_UNAVAILABLE)
            ),
            _ => false,
        };
        self.consecutive = if transient {
            (self.consecutive + 1).min(REPORT_AFTER)
        } else {
            REPORT_AFTER
        };
        self.consecutive == REPORT_AFTER
    }
}

#[derive(Deserialize)]
#[serde(rename_all = "PascalCase")]
struct WmiProcess {
    process_id: u32,
    command_line: Option<String>,
    creation_date: Option<String>,
}

fn wmi_created_micros(value: &str) -> Result<i64> {
    // Parse the raw DMTF string ourselves: its fraction is microseconds, not nanoseconds.
    ensure!(
        value.len() == 25 && value.is_ascii() && matches!(&value[21..22], "+" | "-"),
        "Некорректная дата WMI"
    );
    let date = chrono::NaiveDateTime::parse_from_str(&value[..21], "%Y%m%d%H%M%S%.f")?;
    let offset: i64 = value[21..].parse()?;
    ensure!(offset.abs() <= 840, "Некорректное смещение даты WMI");
    Ok(date.and_utc().timestamp_micros() - offset * 60_000_000)
}

#[derive(Debug, Serialize)]
pub struct Candidate {
    pub pid: u32,
    pub identity: Option<Identity>,
    pub command_line: Option<String>,
    pub eligible: bool,
    pub reason: String,
}

fn rows(pid: Option<u32>) -> Result<Vec<WmiProcess>> {
    let connection = wmi::WMIConnection::new().context("WMI недоступен")?;
    let filter = pid
        .map(|pid| format!(" AND ProcessId={pid}"))
        .unwrap_or_default();
    connection.raw_query(format!("SELECT ProcessId,CommandLine,CreationDate FROM Win32_Process WHERE Name='1cv8.exe'{filter}"))
        .context("Не удалось получить список 1cv8.exe")
}

fn inspect(row: WmiProcess) -> Result<Identity> {
    let command = row
        .command_line
        .context("Командная строка недоступна; режим определить нельзя")?;
    ensure!(win::is_designer(&command)?, "Режим не DESIGNER");
    let process = Process::open(row.process_id, false)?;
    let created = wmi_created_micros(
        &row.creation_date
            .context("Дата создания процесса недоступна")?,
    )?;
    let native = (process.identity.created_time / 10) as i64 - 11_644_473_600_000_000;
    ensure!(
        native == created,
        "WMI и native identity расходятся; процесс изменился"
    );
    validate_platform(&process.identity.exe)?;
    Ok(process.identity)
}

pub fn validate_platform(exe: &Path) -> Result<()> {
    ensure!(
        exe.file_name()
            .is_some_and(|n| n.to_string_lossy().eq_ignore_ascii_case("1cv8.exe")),
        "Ожидался 1cv8.exe"
    );
    // Validate the PE architecture and version resource, without pinning a release.
    win::platform_version(exe)?;
    Ok(())
}

pub fn discover() -> Result<Vec<Candidate>> {
    Ok(rows(None)?
        .into_iter()
        .filter_map(|row| {
            let pid = row.process_id;
            let command_line = row.command_line.clone();
            match inspect(row) {
                Ok(identity) => Some(Candidate {
                    pid,
                    command_line,
                    identity: Some(identity),
                    eligible: true,
                    reason: "Готов к проверке модулей".into(),
                }),
                Err(error) if error.is::<ProcessExited>() => None,
                Err(error) => Some(Candidate {
                    pid,
                    command_line,
                    identity: None,
                    eligible: false,
                    reason: format!("{error:#}"),
                }),
            }
        })
        .collect())
}

pub(crate) fn identity(pid: u32) -> Result<Identity> {
    inspect(
        rows(Some(pid))?
            .into_iter()
            .next()
            .context("Процесс 1cv8.exe не найден")?,
    )
}

pub fn data_dir() -> Result<PathBuf> {
    let dirs =
        ProjectDirs::from("", "", crate::APP_NAME).context("Не найден пользовательский каталог")?;
    let path = dirs.data_local_dir().to_owned();
    fs::create_dir_all(&path)?;
    win::canonical(&path)
}

pub fn hash(bytes: &[u8]) -> String {
    format!("{:x}", Sha256::digest(bytes))
}

fn materialize(root: &Path) -> Result<PathBuf> {
    let digest = hash(theme::DLL);
    let directory = root.join("runtime").join(digest);
    fs::create_dir_all(&directory)?;
    let _lock = Lock::acquire(&directory.join("runtime.lock"))?;
    let dll = directory.join(DLL_NAME);
    if dll.exists() {
        ensure!(
            hash(&fs::read(&dll)?) == hash(theme::DLL),
            "Повреждена DLL в runtime; файл не заменялся"
        );
    } else {
        atomic_write(&dll, theme::DLL)?;
    }
    win::canonical(&dll)
}

#[derive(Debug, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Session {
    schema_version: u32,
    identity: Identity,
    dll: PathBuf,
    dll_sha256: String,
    log: PathBuf,
    stage: String,
    theme: String,
    custom_icons: bool,
}

fn read_session(path: &Path) -> Result<Session> {
    let bytes = fs::read(path)?;
    let header: serde_json::Value = serde_json::from_slice(&bytes)?;
    ensure!(header["schema_version"] == 2, ENGINE_MISMATCH);
    Ok(serde_json::from_slice(&bytes)?)
}

#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct AppliedConfig {
    config_sha256: String,
    generation: u32,
}

#[derive(Debug, Serialize)]
pub struct Applied {
    pub identity: Identity,
    pub theme: String,
    pub action: String,
    pub generation: u32,
    pub patched_imports: u32,
    pub icons: String,
    pub icon_warning: Option<String>,
    pub session: PathBuf,
}

struct EngineSnapshot {
    theme: String,
    generation: u32,
    hooks: u32,
    icon_mode: u32,
    icon_status: u32,
}
fn read_snapshot(log: &Path) -> Result<EngineSnapshot> {
    let data = fs::read_to_string(log)?;
    let field = |name: &str| {
        data.lines()
            .find_map(|line| line.strip_prefix(&format!("# {name}=")))
            .map(str::to_owned)
            .with_context(|| format!("Нет поля {name} в snapshot"))
    };
    ensure!(field("protocol")? == "3", "Несовместимый протокол snapshot");
    let base: u32 = field("base_patched_imports")?.parse()?;
    let icon_hooks: u32 = field("icon_patched_imports")?.parse()?;
    let hooks: u32 = field("patched_imports")?.parse()?;
    let icon_status: u32 = field("icon_status")?.parse()?;
    let icon_mode: u32 = field("icon_mode")?.parse()?;
    let pack: u32 = field("icon_pack")?.parse()?;
    ensure!(
        matches!(base, 14 | 22 | 23)
            && hooks == base + icon_hooks
            && icon_mode <= 2
            && pack == u32::from(icon_mode != 0)
            && if icon_status == 0 {
                icon_hooks == 3
            } else {
                icon_hooks == 0 && icon_mode == 0
            },
        "Snapshot не подтвердил состояние подсистем движка"
    );
    if base == 23 {
        ensure!(
            field("browser_lifetime_guard")? == "1"
                && field("browser_patched_imports")? == "9"
                && field("browser_guard_failures")? == "0",
            "Snapshot не подтвердил защиту HTML-окна при закрытии"
        );
    }
    Ok(EngineSnapshot {
        theme: field("theme")?,
        generation: field("generation")?.parse()?,
        hooks,
        icon_mode,
        icon_status,
    })
}
fn snapshot(process: &Process, address: usize, log: &Path) -> Result<EngineSnapshot> {
    let code = process.call(address, None)?;
    ensure!(code == 0, "ThemeSnapshot Win32={code}");
    read_snapshot(log)
}
fn icon_result(snapshot: &EngineSnapshot, requested: bool) -> (String, Option<String>) {
    if requested && snapshot.icon_status != 0 {
        (
            "unavailable".into(),
            Some(format!(
                "Тема применена; не удалось включить новые значки (Win32={}). Используются штатные значки.",
                snapshot.icon_status
            )),
        )
    } else {
        (
            (match snapshot.icon_mode {
                1 => "light",
                2 => "dark",
                _ => "stock",
            })
            .into(),
            None,
        )
    }
}

/// Public entry point: candidates must pass WMI mode and native identity checks.
pub fn attach(pid: u32, theme_id: &str) -> Result<Applied> {
    attach_with_icons(pid, theme_id, false)
}
pub fn attach_with_icons(pid: u32, theme_id: &str, custom_icons: bool) -> Result<Applied> {
    attach_with_options(pid, theme_id, custom_icons, true)
}

pub fn attach_with_options(
    pid: u32,
    theme_id: &str,
    custom_icons: bool,
    semantic_highlighting: bool,
) -> Result<Applied> {
    // Validate the theme before opening a writable process handle.
    theme::config(theme_id, Path::new("x"), Path::new("y"))?;
    let expected = identity(pid)?;
    attach_to_with_options_wait(
        &expected,
        theme_id,
        custom_icons,
        semantic_highlighting,
        THEME_WAIT,
    )
}

/// GUI requests carry the identity observed in the process list, not only a PID.
pub fn attach_to(expected: &Identity, theme_id: &str) -> Result<Applied> {
    attach_to_with_icons(expected, theme_id, false)
}
pub fn attach_to_with_icons(
    expected: &Identity,
    theme_id: &str,
    custom_icons: bool,
) -> Result<Applied> {
    attach_to_with_icons_wait(expected, theme_id, custom_icons, THEME_WAIT)
}

/// The wait covers module readiness only. A failed remote operation is never replayed here.
pub fn attach_to_with_icons_wait(
    expected: &Identity,
    theme_id: &str,
    custom_icons: bool,
    wait: Duration,
) -> Result<Applied> {
    attach_to_with_options_wait(expected, theme_id, custom_icons, true, wait)
}

pub fn attach_to_with_options_wait(
    expected: &Identity,
    theme_id: &str,
    custom_icons: bool,
    semantic_highlighting: bool,
    wait: Duration,
) -> Result<Applied> {
    theme::config(theme_id, Path::new("x"), Path::new("y"))?;
    ensure!(
        identity(expected.pid)? == *expected,
        "Процесс в списке изменился; обновите список"
    );
    let root = data_dir()?;
    let directory = root
        .join("sessions")
        .join(format!("{}-{}", expected.pid, expected.created_time));
    attach_appearance_verified(
        expected,
        theme_id,
        &directory,
        &materialize(&root)?,
        custom_icons,
        semantic_highlighting,
        wait,
    )
}

#[derive(Clone, Serialize)]
pub struct LastApplied {
    pub theme: String,
    pub generation: u32,
    pub icons: String,
    pub icon_warning: Option<String>,
}
/// Read the last successful application; never invokes a remote thread while polling UI.
pub fn last_applied(expected: &Identity) -> Result<Option<LastApplied>> {
    let root = data_dir()?;
    let directory = root
        .join("sessions")
        .join(format!("{}-{}", expected.pid, expected.created_time));
    let path = directory.join("session.json");
    if !path.exists() {
        return Ok(None);
    }
    let saved: Session = read_session(&path)?;
    ensure!(
        saved.schema_version == 2 && saved.identity == *expected && saved.stage == "ready",
        "Предыдущее применение не завершено или несовместимо"
    );
    let dll = engine_dll::session_path(
        &root.join("runtime"),
        DLL_NAME,
        &saved.dll_sha256,
        theme::DLL,
    )
    .and_then(|path| win::canonical(&path))
    .context(ENGINE_MISMATCH)?;
    ensure!(win::same_path(&saved.dll, &dll), ENGINE_MISMATCH);
    ensure!(
        win::same_path(
            &win::canonical(&saved.log)?,
            &win::canonical(&directory.join("colors.csv"))?
        ),
        "Некорректный путь журнала сессии"
    );
    let snapshot = read_snapshot(&saved.log)?;
    let (icons, icon_warning) =
        icon_result(&snapshot, saved.custom_icons && saved.theme != "diagnostic");
    Ok(Some(LastApplied {
        theme: snapshot.theme,
        generation: snapshot.generation,
        icons,
        icon_warning,
    }))
}

#[cfg(test)]
fn attach_verified(
    expected: &Identity,
    theme_id: &str,
    directory: &Path,
    dll: &Path,
) -> Result<Applied> {
    attach_appearance_verified(expected, theme_id, directory, dll, false, true, THEME_WAIT)
}

fn attach_appearance_verified(
    expected: &Identity,
    theme_id: &str,
    directory: &Path,
    dll: &Path,
    custom_icons: bool,
    semantic_highlighting: bool,
    wait: Duration,
) -> Result<Applied> {
    fs::create_dir_all(directory)?;
    let directory = win::canonical(directory)?;
    let _lock = Lock::acquire(&directory.join("control.lock"))?;
    let process = Process::open(expected.pid, true)?;
    process.matches(expected)?;
    let log = directory.join("colors.csv");
    // MSIX file virtualization can redirect an individual file even when its
    // parent directory exists outside LocalCache. Resolve the existing file.
    let log = if log.exists() {
        win::canonical(&log)?
    } else {
        log
    };
    let state_path = directory.join("session.json");
    let mut state = if state_path.exists() {
        let state: Session = read_session(&state_path)?;
        ensure!(state.schema_version == 2, "Несовместимая схема сессии");
        ensure!(
            state.identity == *expected,
            "Идентичность процесса отличается от сохранённой сессии"
        );
        ensure!(
            win::same_path(&state.log, &log),
            "Путь журнала отличается от сохранённой сессии"
        );
        ensure!(
            state.stage == "ready",
            "Предыдущая операция не завершена; автоматический повтор запрещён ({}). Закройте 1С штатно",
            state.stage
        );
        Some(state)
    } else {
        None
    };
    let deadline = Instant::now() + wait;
    let modules = loop {
        match process.module_snapshot() {
            Ok(modules) if THEME_MODULES.iter().all(|name| modules.contains_key(*name)) => {
                break modules;
            }
            Ok(_) if Instant::now() >= deadline => return Err(ThemeNotReady.into()),
            Ok(_) => {}
            Err(error) if win::module_list_changing(&error) => {
                if Instant::now() >= deadline {
                    return Err(error.context(ThemeNotReady));
                }
            }
            Err(error) => return Err(error),
        }
        std::thread::sleep(
            Duration::from_millis(100).min(deadline.saturating_duration_since(Instant::now())),
        );
    };
    for name in THEME_MODULES {
        ensure!(
            win::same_path(
                &modules[name].1,
                &expected
                    .exe
                    .parent()
                    .context("Нет каталога exe")?
                    .join(name)
            ),
            "Модуль {name} загружен из неожиданного каталога"
        );
    }
    let cfg = theme::config_with_options(
        theme_id,
        expected.exe.parent().context("Нет каталога exe")?,
        &log,
        custom_icons,
        semantic_highlighting,
    )?;
    // A preset name can stay the same when its palette/profile is corrected.
    // Keep this marker separate so older managers can still read session.json.
    // Generation also detects a ThemeApply performed by an older manager.
    let marker_path = directory.join("applied-config.json");
    let config_sha256 = hash(cfg.bytes());
    let action;
    let exports;
    if let Some((_, loaded)) = modules.get("themeprobe.dll") {
        let saved = state
            .as_mut()
            .context("DLL есть, но реестр сессии отсутствует; повторная инициализация запрещена")?;
        let runtime = dll
            .parent()
            .and_then(Path::parent)
            .context("Нет каталога движков")?;
        let recorded = engine_dll::session_path(runtime, DLL_NAME, &saved.dll_sha256, theme::DLL)
            .and_then(|path| win::canonical(&path))
            .context(ENGINE_MISMATCH)?;
        ensure!(
            win::same_path(loaded, &recorded) && win::same_path(&saved.dll, &recorded),
            ENGINE_MISMATCH
        );
        exports = process.exports(&recorded)?;
        let current = snapshot(&process, exports["ThemeSnapshot"], &log)?;
        let marker = fs::read(&marker_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<AppliedConfig>(&bytes).ok());
        if current.theme == theme_id
            && saved.theme == theme_id
            && marker.is_some_and(|m| {
                m.config_sha256 == config_sha256 && m.generation == current.generation
            })
        {
            action = "unchanged";
        } else {
            saved.stage = "applying".into();
            atomic_write(&state_path, &serde_json::to_vec_pretty(saved)?)?;
            let code = process.call(exports["ThemeApply"], Some(cfg.bytes()))?;
            ensure!(
                code == 0,
                "ThemeApply Win32={code}; остановленная DLL требует перезапуска 1С"
            );
            action = "applied";
        }
    } else {
        ensure!(
            state.is_none(),
            "Реестр сессии есть, но DLL отсутствует; состояние несогласованно"
        );
        let saved = Session {
            schema_version: 2,
            identity: expected.clone(),
            dll: dll.into(),
            dll_sha256: hash(theme::DLL),
            log: log.clone(),
            stage: "initializing".into(),
            theme: theme_id.into(),
            custom_icons,
        };
        atomic_write(&state_path, &serde_json::to_vec_pretty(&saved)?)?;
        process.load_dll(dll)?;
        exports = process.exports(dll)?;
        let code = process.call(exports["ThemeInitialize"], Some(cfg.bytes()))?;
        ensure!(
            code == 0,
            "ThemeInitialize Win32={code}; повторная инициализация запрещена"
        );
        state = Some(saved);
        action = "attached";
    }
    win::redraw(expected.pid, action != "unchanged");
    let current = snapshot(&process, exports["ThemeSnapshot"], &log)?;
    let generation = current.generation;
    ensure!(
        current.theme == theme_id
            && (current.icon_status != 0 || current.icon_mode == cfg.icon_mode),
        "Snapshot не подтвердил применение темы"
    );
    let saved = state.as_mut().context("Нет состояния сессии")?;
    saved.stage = "ready".into();
    saved.theme = theme_id.into();
    saved.custom_icons = custom_icons;
    atomic_write(&state_path, &serde_json::to_vec_pretty(saved)?)?;
    atomic_write(
        &marker_path,
        &serde_json::to_vec_pretty(&AppliedConfig {
            config_sha256,
            generation,
        })?,
    )?;
    let (icons, icon_warning) = icon_result(&current, cfg.icon_mode != 0);
    Ok(Applied {
        identity: expected.clone(),
        theme: theme_id.into(),
        action: action.into(),
        generation,
        patched_imports: current.hooks,
        icons,
        icon_warning,
        session: state_path,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::OpenOptions;
    #[test]
    fn wmi_date_preserves_microseconds_and_timezone() {
        let expected = 134375992620716457u64 / 10 - 11_644_473_600_000_000;
        assert_eq!(
            wmi_created_micros("20261027182742.071645+000").unwrap(),
            expected as i64
        );
        assert_eq!(
            wmi_created_micros("20261027212742.071645+180").unwrap(),
            expected as i64
        );
        assert!(wmi_created_micros("20261027182742.******+***").is_err());
    }

    #[test]
    fn only_brief_transient_discovery_failures_are_silent() {
        let wmi_error = |hres: u32| {
            anyhow::Error::new(wmi::WMIError::HResultError { hres: hres as i32 }).context("WMI")
        };
        let mut failures = DiscoveryFailures::default();
        let transient = wmi_error(0x80041069);
        assert!(!failures.should_report(&transient));
        assert!(!failures.should_report(&transient));
        assert!(failures.should_report(&transient));
        assert!(failures.should_report(&transient));
        failures.recovered();
        assert!(!failures.should_report(&transient));
        assert!(failures.should_report(&wmi_error(0x80041003)));
        failures.recovered();
        assert!(failures.should_report(&wmi_error(0x80070005)));
        failures.recovered();
        assert!(failures.should_report(&wmi_error(0x80041017)));
    }
    #[test]
    fn rejects_non_platform_executable() {
        assert!(validate_platform(&std::env::current_exe().unwrap()).is_err());
    }
    #[test]
    fn browser_lifetime_capability_requires_a_confirmed_guard() -> Result<()> {
        let directory = tempfile::tempdir()?;
        let log = directory.path().join("snapshot.csv");
        let valid = "# protocol=3\n# theme=dracula\n# generation=1\n# base_patched_imports=23\n# patched_imports=26\n# icon_patched_imports=3\n# icon_status=0\n# icon_mode=2\n# icon_pack=1\n# browser_lifetime_guard=1\n# browser_patched_imports=9\n# browser_guard_failures=0\n";
        fs::write(&log, valid)?;
        assert_eq!(read_snapshot(&log)?.hooks, 26);
        for invalid in [
            valid.replace("browser_lifetime_guard=1", "browser_lifetime_guard=0"),
            valid.replace("browser_patched_imports=9", "browser_patched_imports=8"),
            valid.replace("browser_guard_failures=0", "browser_guard_failures=1"),
        ] {
            fs::write(&log, invalid)?;
            assert!(read_snapshot(&log).is_err());
        }
        Ok(())
    }
    #[test]
    fn installed_platform_releases_are_not_filtered_by_version() -> Result<()> {
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        let mut checked = vec![];
        for release in ["8.3.26.1656", "8.3.27.2325", "8.5.1.1529", "8.5.4.1878"] {
            let exe = Path::new("C:/Program Files/1cv8")
                .join(release)
                .join("bin/1cv8.exe");
            if exe.exists() {
                validate_platform(&exe)?;
                checked.push(release);
            }
        }
        fs::create_dir_all(root.join("evidence/ui-processes"))?;
        fs::write(
            root.join("evidence/ui-processes/platform-validation.json"),
            serde_json::to_vec_pretty(&checked)?,
        )?;
        Ok(())
    }
    #[test]
    fn real_wmi_rejects_enterprise_and_unversioned_designer() -> Result<()> {
        use windows_sys::Win32::System::Threading::{EVENT_MODIFY_STATE, OpenEventW, SetEvent};
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        fs::create_dir_all(root.join("evidence"))
            .context("Create WMI fixture evidence directory")?;
        let mut results = vec![];
        for mode in ["ENTERPRISE", "DESIGNER"] {
            let temp = tempfile::Builder::new()
                .prefix("themes-Команда-主题 UI ")
                .tempdir()
                .context("Create Unicode directory for WMI fixture")?;
            let exe = temp.path().join("1cv8.exe");
            fs::copy(root.join("tests/fixtures/mode_host.exe"), &exe)
                .with_context(|| format!("Copy WMI fixture to {}", exe.display()))?;
            let mut child = std::process::Command::new(&exe)
                .arg(mode)
                .spawn()
                .with_context(|| format!("Launch {mode} fixture {}", exe.display()))?;
            let result = (|| -> Result<_> {
                let row = discover()?
                    .into_iter()
                    .find(|r| r.pid == child.id())
                    .context("WMI не вернул mock процесс")?;
                ensure!(!row.eligible, "Недопустимый процесс признан поддержанным");
                let command = row
                    .command_line
                    .as_deref()
                    .context("Нет командной строки")?;
                ensure!(
                    command.contains(&exe.display().to_string()) && command.ends_with(mode),
                    "WMI command line lost Unicode, spaces or mode"
                );
                ensure!(
                    attach(child.id(), "dracula").is_err(),
                    "Недопустимый процесс получил тему"
                );
                let process =
                    Process::open(child.id(), false).context("Open WMI fixture process")?;
                ensure!(
                    !process
                        .modules()
                        .context("Enumerate WMI fixture modules")?
                        .contains_key("themeprobe.dll"),
                    "DLL загружена в исключённый процесс"
                );
                Ok(serde_json::json!({"mode":mode,"candidate":row,"no_dll":true}))
            })();
            let name = win::wide(std::ffi::OsStr::new(&format!(
                "Local\\ThemeModeExit-{}",
                child.id()
            )));
            let event = win::Handle::new(
                unsafe { OpenEventW(EVENT_MODIFY_STATE, 0, name.as_ptr()) },
                "Open mode test exit",
            )?;
            ensure!(unsafe { SetEvent(event.0) } != 0, "Signal mode test exit");
            ensure!(
                child.wait().context("Wait for WMI fixture exit")?.success(),
                "Mode host exit failed"
            );
            results.push(result?);
        }
        fs::write(
            root.join("evidence/wmi-exclusions.json"),
            serde_json::to_vec_pretty(&results)?,
        )
        .context("Write WMI fixture evidence")?;
        Ok(())
    }
    #[test]
    fn session_lock_serializes_independent_handles() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("test.lock");
        let _lock = Lock::acquire(&path).unwrap();
        let file = OpenOptions::new()
            .create(true)
            .truncate(false)
            .read(true)
            .write(true)
            .open(path)
            .unwrap();
        assert!(fs2::FileExt::try_lock_exclusive(&file).is_err());
    }
    #[test]
    fn embedded_dll_roundtrip_and_corruption_guard() {
        let temp = tempfile::tempdir().unwrap();
        let dll = materialize(temp.path()).unwrap();
        assert_eq!(fs::read(&dll).unwrap(), theme::DLL);
        fs::write(&dll, b"broken").unwrap();
        assert!(materialize(temp.path()).is_err());
    }
    #[test]
    fn atomic_state_replaces_complete_document() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        atomic_write(&path, b"first").unwrap();
        atomic_write(&path, b"second").unwrap();
        assert_eq!(fs::read(path).unwrap(), b"second");
    }
    #[test]
    fn missing_theme_modules_do_not_start_a_remote_operation() -> Result<()> {
        let fixtures = Path::new(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures");
        let mut child = std::process::Command::new(fixtures.join("mode_host.exe")).spawn()?;
        let result = (|| -> Result<()> {
            let expected = Process::open(child.id(), false)?.identity;
            let temp = tempfile::tempdir()?;
            let directory = temp.path().join("session");
            let dll = materialize(&temp.path().join("runtime"))?;
            let attempt = attach_appearance_verified(
                &expected,
                "dracula",
                &directory,
                &dll,
                false,
                true,
                Duration::ZERO,
            );
            ensure!(attempt.is_err_and(|error| error.is::<ThemeNotReady>()));
            ensure!(!directory.join("session.json").exists());
            ensure!(!directory.join("colors.csv").exists());
            ensure!(
                !Process::open(child.id(), false)?
                    .modules()?
                    .contains_key("themeprobe.dll")
            );
            Ok(())
        })();
        let _ = child.kill();
        child.wait()?;
        result
    }

    #[test]
    fn late_attach_native_recovery_and_live_switching() -> Result<()> {
        use windows_sys::Win32::{
            Foundation::FreeLibrary,
            System::{
                LibraryLoader::{DONT_RESOLVE_DLL_REFERENCES, GetProcAddress, LoadLibraryExW},
                Threading::{EVENT_MODIFY_STATE, OpenEventW, SetEvent},
            },
        };
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        let temp = tempfile::Builder::new()
            .prefix("themes-Команда-主题 UI ")
            .tempdir()?;
        let directory = temp.path().join("session");
        fs::create_dir_all(&directory)?;
        let directory = win::canonical(&directory)?;
        let fixtures = win::canonical(&root.join("tests/fixtures"))?;
        let exe = fixtures.join("probe_target.exe");
        let config = theme::config("dracula", &fixtures, &directory.join("colors.csv"))?;
        let cfg_path = temp.path().join("expected.bin");
        fs::write(&cfg_path, config.bytes())?;
        let report = temp.path().join("native.json");
        let mut child = std::process::Command::new(&exe)
            .args([
                report.as_os_str(),
                cfg_path.as_os_str(),
                std::ffi::OsStr::new("hold"),
            ])
            .current_dir(&fixtures)
            .spawn()?;
        let expected = Process::open(child.id(), false)?.identity;
        let signal = |name: &str| -> Result<()> {
            let name = win::wide(std::ffi::OsStr::new(&format!(
                "Local\\ThemeProbe{name}-{}",
                expected.pid
            )));
            let deadline = Instant::now() + Duration::from_secs(5);
            loop {
                let raw = unsafe { OpenEventW(EVENT_MODIFY_STATE, 0, name.as_ptr()) };
                if !raw.is_null() {
                    let event = win::Handle(raw);
                    ensure!(unsafe { SetEvent(event.0) } != 0, "SetEvent");
                    return Ok(());
                }
                ensure!(Instant::now() < deadline, "Не появился native test event");
                std::thread::sleep(Duration::from_millis(25));
            }
        };
        signal("Ready")?;
        let result = (|| -> Result<_> {
            let dll = materialize(&temp.path().join("runtime"))?;
            let initial = attach_verified(&expected, "dracula", &directory, &dll)?;
            ensure!(
                initial.action == "attached" && initial.generation == 1,
                "initial attach"
            );
            let repeated = attach_verified(&expected, "dracula", &directory, &dll)?;
            ensure!(
                repeated.action == "unchanged" && repeated.generation == 1,
                "duplicate attach"
            );
            let enabled = attach_appearance_verified(
                &expected, "dracula", &directory, &dll, true, true, THEME_WAIT,
            )?;
            ensure!(
                enabled.action == "applied"
                    && enabled.icons == "unavailable"
                    && enabled.icon_warning.is_some(),
                "unsupported icons must be reported without breaking theme"
            );
            let disabled = attach_verified(&expected, "dracula", &directory, &dll)?;
            ensure!(
                disabled.action == "applied"
                    && disabled.icons == "stock"
                    && disabled.icon_warning.is_none(),
                "icon-only toggle must reapply once"
            );
            // Migration from a manager without a marker, changed profile content,
            // and a native update by an older manager must each reapply once.
            let marker_path = directory.join("applied-config.json");
            for reason in ["legacy", "changed-profile", "external-generation"] {
                if reason == "legacy" {
                    fs::remove_file(&marker_path)?;
                } else {
                    let mut marker: AppliedConfig =
                        serde_json::from_slice(&fs::read(&marker_path)?)?;
                    if reason == "changed-profile" {
                        marker.config_sha256 = "previous profile".into();
                    } else {
                        marker.generation -= 1;
                    }
                    atomic_write(&marker_path, &serde_json::to_vec(&marker)?)?;
                }
                let updated = attach_verified(&expected, "dracula", &directory, &dll)?;
                ensure!(updated.action == "applied", "missed update: {reason}");
                let duplicate = attach_verified(&expected, "dracula", &directory, &dll)?;
                ensure!(
                    duplicate.action == "unchanged" && duplicate.generation == updated.generation,
                    "repeated update: {reason}"
                );
            }
            let process = Process::open(expected.pid, true)?;
            let local = unsafe {
                LoadLibraryExW(
                    win::wide(fixtures.join("grphcs.dll").as_os_str()).as_ptr(),
                    std::ptr::null_mut(),
                    DONT_RESOLVE_DLL_REFERENCES,
                )
            };
            ensure!(!local.is_null(), "load mock metadata");
            let rva = unsafe { GetProcAddress(local, c"graphics_probe".as_ptr() as _) }
                .context("graphics export")? as usize
                - local as usize;
            unsafe {
                FreeLibrary(local);
            }
            let graphics = process.modules()?["grphcs.dll"].0 + rva;
            let mut transitions = vec![];
            for id in theme::IDS.iter().copied().chain(["diagnostic", "dracula"]) {
                let applied = attach_verified(&expected, id, &directory, &dll)?;
                let cfg = theme::config(id, &fixtures, &directory.join("colors.csv"))?;
                ensure!(
                    process.call(graphics, Some(cfg.bytes()))? == 1,
                    "graphics invariants {id}"
                );
                ensure!(applied.identity == expected, "process restarted");
                transitions.push(applied);
            }
            let original = fs::read(directory.join("session.json"))?;
            let mut stale: Session = serde_json::from_slice(&original)?;
            stale.dll_sha256 = "previous engine".into();
            atomic_write(
                &directory.join("session.json"),
                &serde_json::to_vec(&stale)?,
            )?;
            let mismatch = attach_verified(&expected, "dracula", &directory, &dll).unwrap_err();
            ensure!(
                mismatch.to_string().contains("перезапустите"),
                "unclear engine upgrade error"
            );
            stale = serde_json::from_slice(&original)?;
            stale.identity.created_time += 1;
            atomic_write(
                &directory.join("session.json"),
                &serde_json::to_vec(&stale)?,
            )?;
            ensure!(
                attach_verified(&expected, "dracula", &directory, &dll).is_err(),
                "stale registry accepted"
            );
            stale.identity = expected.clone();
            stale.stage = "initializing".into();
            atomic_write(
                &directory.join("session.json"),
                &serde_json::to_vec(&stale)?,
            )?;
            ensure!(
                attach_verified(&expected, "dracula", &directory, &dll).is_err(),
                "unfinished intent accepted"
            );
            atomic_write(&directory.join("session.json"), &original)?;
            let resumed = attach_verified(&expected, "dracula", &directory, &dll)?;
            ensure!(
                resumed.generation == transitions.last().unwrap().generation,
                "failed checks mutated generation"
            );
            Ok(
                serde_json::json!({"initial":initial,"duplicate":repeated,"transitions":transitions,
                "stale_registry_rejected":true,"unfinished_intent_rejected":true,"same_process":true}),
            )
        })();
        signal("Continue")?;
        let status = child.wait()?;
        ensure!(
            status.success(),
            "Native test exit={status}; report={:?}",
            fs::read_to_string(&report)
        );
        let mut evidence = result?;
        evidence["native"] = serde_json::from_slice(&fs::read(&report)?)?;
        evidence["native_exit"] = serde_json::json!(status.code());
        fs::write(
            root.join("evidence/native-late-attach.json"),
            serde_json::to_vec_pretty(&evidence)?,
        )?;
        Ok(())
    }
}
