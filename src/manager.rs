use crate::{
    storage::{Lock, atomic_write},
    theme,
    win::{self, Identity, Process},
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
        .map(|row| {
            let pid = row.process_id;
            let command_line = row.command_line.clone();
            match inspect(row) {
                Ok(identity) => Candidate {
                    pid,
                    command_line,
                    identity: Some(identity),
                    eligible: true,
                    reason: "Готов к проверке модулей".into(),
                },
                Err(error) => Candidate {
                    pid,
                    command_line,
                    identity: None,
                    eligible: false,
                    reason: format!("{error:#}"),
                },
            }
        })
        .collect())
}

fn identity(pid: u32) -> Result<Identity> {
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
    let dll = directory.join("ThemeProbe.dll");
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
    pub session: PathBuf,
}

fn snapshot(process: &Process, address: usize, log: &Path) -> Result<(String, u32, u32)> {
    let code = process.call(address, None)?;
    ensure!(code == 0, "ThemeSnapshot Win32={code}");
    let data = fs::read_to_string(log)?;
    let field = |name: &str| {
        data.lines()
            .find_map(|line| line.strip_prefix(&format!("# {name}=")))
            .map(str::to_owned)
            .with_context(|| format!("Нет поля {name} в snapshot"))
    };
    Ok((
        field("theme")?,
        field("generation")?.parse()?,
        field("patched_imports")?.parse()?,
    ))
}

/// Public entry point: candidates must pass WMI mode and native identity checks.
pub fn attach(pid: u32, theme_id: &str) -> Result<Applied> {
    // Validate the theme before opening a writable process handle.
    theme::config(theme_id, Path::new("x"), Path::new("y"))?;
    let expected = identity(pid)?;
    attach_to(&expected, theme_id)
}

/// GUI requests carry the identity observed in the process list, not only a PID.
pub fn attach_to(expected: &Identity, theme_id: &str) -> Result<Applied> {
    theme::config(theme_id, Path::new("x"), Path::new("y"))?;
    ensure!(
        identity(expected.pid)? == *expected,
        "Процесс в списке изменился; обновите список"
    );
    let root = data_dir()?;
    let directory = root
        .join("sessions")
        .join(format!("{}-{}", expected.pid, expected.created_time));
    attach_verified(expected, theme_id, &directory, &materialize(&root)?)
}

#[derive(Clone, Serialize)]
pub struct LastApplied {
    pub theme: String,
    pub generation: u32,
}
/// Read the last successful application; never invokes a remote thread while polling UI.
pub fn last_applied(expected: &Identity) -> Result<Option<LastApplied>> {
    let directory = data_dir()?
        .join("sessions")
        .join(format!("{}-{}", expected.pid, expected.created_time));
    let path = directory.join("session.json");
    if !path.exists() {
        return Ok(None);
    }
    let saved: Session = serde_json::from_slice(&fs::read(path)?)?;
    ensure!(
        saved.schema_version == 1 && saved.identity == *expected && saved.stage == "ready",
        "Предыдущее применение не завершено или несовместимо"
    );
    ensure!(saved.dll_sha256 == hash(theme::DLL), ENGINE_MISMATCH);
    ensure!(
        saved.log == directory.join("colors.csv"),
        "Некорректный путь журнала сессии"
    );
    let text = fs::read_to_string(saved.log)?;
    let theme = text
        .lines()
        .find_map(|s| s.strip_prefix("# theme="))
        .context("Нет имени темы")?;
    let generation = text
        .lines()
        .find_map(|s| s.strip_prefix("# generation="))
        .context("Нет поколения")?
        .parse()?;
    Ok(Some(LastApplied {
        theme: theme.into(),
        generation,
    }))
}

fn attach_verified(
    expected: &Identity,
    theme_id: &str,
    directory: &Path,
    dll: &Path,
) -> Result<Applied> {
    fs::create_dir_all(directory)?;
    let directory = win::canonical(directory)?;
    let _lock = Lock::acquire(&directory.join("control.lock"))?;
    let process = Process::open(expected.pid, true)?;
    process.matches(expected)?;
    let log = directory.join("colors.csv");
    let state_path = directory.join("session.json");
    let mut state = if state_path.exists() {
        let state: Session = serde_json::from_slice(&fs::read(&state_path)?)?;
        ensure!(
            state.schema_version == 1 && state.identity == *expected && state.log == log,
            "Несовместимый/повреждённый реестр сессии"
        );
        ensure!(
            state.dll_sha256 == hash(theme::DLL) && win::same_path(&state.dll, dll),
            ENGINE_MISMATCH
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
    let deadline = Instant::now() + Duration::from_secs(30);
    let modules = loop {
        let modules = process.modules()?;
        if ["frame.dll", "grphcs.dll", "cairo.dll"]
            .iter()
            .all(|name| modules.contains_key(*name))
        {
            break modules;
        }
        ensure!(
            Instant::now() < deadline,
            "Не дождались готовности модулей конфигуратора"
        );
        std::thread::sleep(Duration::from_millis(100));
    };
    for name in ["frame.dll", "grphcs.dll", "cairo.dll"] {
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
    let cfg = theme::config(
        theme_id,
        expected.exe.parent().context("Нет каталога exe")?,
        &log,
    )?;
    // A preset name can stay the same when its palette/profile is corrected.
    // Keep this marker separate so older managers can still read session.json.
    // Generation also detects a ThemeApply performed by an older manager.
    let marker_path = directory.join("applied-config.json");
    let config_sha256 = hash(cfg.bytes());
    let action;
    let exports;
    if let Some((_, loaded)) = modules.get("themeprobe.dll") {
        ensure!(
            win::same_path(loaded, dll),
            "В процессе уже загружена другая DLL темы; требуется штатный перезапуск 1С"
        );
        let saved = state
            .as_mut()
            .context("DLL есть, но реестр сессии отсутствует; повторная инициализация запрещена")?;
        exports = process.exports(dll)?;
        let (current, generation, hooks) = snapshot(&process, exports["ThemeSnapshot"], &log)?;
        ensure!(
            matches!(hooks, 14 | 22),
            "Несовместимое число перехватов: {hooks}"
        );
        let marker = fs::read(&marker_path)
            .ok()
            .and_then(|bytes| serde_json::from_slice::<AppliedConfig>(&bytes).ok());
        if current == theme_id
            && saved.theme == theme_id
            && marker
                .is_some_and(|m| m.config_sha256 == config_sha256 && m.generation == generation)
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
            schema_version: 1,
            identity: expected.clone(),
            dll: dll.into(),
            dll_sha256: hash(theme::DLL),
            log: log.clone(),
            stage: "initializing".into(),
            theme: theme_id.into(),
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
    let (current, generation, hooks) = snapshot(&process, exports["ThemeSnapshot"], &log)?;
    ensure!(
        current == theme_id && matches!(hooks, 14 | 22),
        "Snapshot не подтвердил применение темы"
    );
    let saved = state.as_mut().context("Нет состояния сессии")?;
    saved.stage = "ready".into();
    saved.theme = theme_id.into();
    atomic_write(&state_path, &serde_json::to_vec_pretty(saved)?)?;
    atomic_write(
        &marker_path,
        &serde_json::to_vec_pretty(&AppliedConfig {
            config_sha256,
            generation,
        })?,
    )?;
    Ok(Applied {
        identity: expected.clone(),
        theme: theme_id.into(),
        action: action.into(),
        generation,
        patched_imports: hooks,
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
    fn rejects_non_platform_executable() {
        assert!(validate_platform(&std::env::current_exe().unwrap()).is_err());
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
                .prefix("themes-Команда UI ")
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
    fn late_attach_native_recovery_and_live_switching() -> Result<()> {
        use windows_sys::Win32::{
            Foundation::FreeLibrary,
            System::{
                LibraryLoader::{DONT_RESOLVE_DLL_REFERENCES, GetProcAddress, LoadLibraryExW},
                Threading::{EVENT_MODIFY_STATE, OpenEventW, SetEvent},
            },
        };
        let root = Path::new(env!("CARGO_MANIFEST_DIR"));
        let temp = tempfile::tempdir()?;
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
