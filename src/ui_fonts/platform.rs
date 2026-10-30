use super::{DEFAULT_FAMILY, FALLBACK_FAMILY, UiFontSettings};
use crate::{
    fonts::{self, FontApplied},
    manager,
    storage::{Lock, atomic_write},
    win::{self, Identity, Process},
};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use windows_sys::Win32::Foundation::ERROR_NOT_READY;

const DLL: &[u8] = include_bytes!("../../assets/UiFontEngine.dll");
const EXPORTS: &[&str] = &[
    "UiFontCapabilities",
    "UiFontStart",
    "UiFontApply",
    "UiFontRefresh",
    "UiFontSnapshot",
];

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Stage {
    Loading,
    Loaded,
    Applying,
    Ready,
    Unsupported,
    Failed,
}
#[derive(Clone, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Session {
    identity: Identity,
    dll_sha256: String,
    stage: Stage,
    requested: UiFontSettings,
    effective: Option<String>,
    applied: Option<FontApplied>,
    failure: Option<String>,
}
fn directory(identity: &Identity) -> Result<PathBuf> {
    Ok(manager::data_dir()?
        .join("ui-fonts/sessions")
        .join(format!("{}-{}", identity.pid, identity.created_time)))
}
fn session_result(session: &Session) -> Result<Option<FontApplied>> {
    ensure!(
        session.stage != Stage::Failed,
        "{}",
        session
            .failure
            .as_deref()
            .unwrap_or("Ошибка шрифта интерфейса; перезапустите этот Конфигуратор")
    );
    ensure!(
        matches!(
            session.stage,
            Stage::Loaded | Stage::Ready | Stage::Unsupported
        ),
        "Предыдущее применение шрифта интерфейса не завершено. Повтор запрещён до перезапуска этого Конфигуратора"
    );
    Ok(session.applied.clone())
}
pub fn last_applied(identity: &Identity) -> Result<Option<FontApplied>> {
    let path = directory(identity)?.join("session.json");
    if !path.exists() {
        return Ok(None);
    }
    let saved: Session = serde_json::from_slice(&fs::read(path)?)?;
    ensure!(
        saved.identity == *identity && saved.dll_sha256 == manager::hash(DLL),
        "Изменилась версия движка шрифта интерфейса; перезапустите этот Конфигуратор"
    );
    session_result(&saved)
}
fn save(path: &Path, session: &Session) -> Result<()> {
    atomic_write(path, &serde_json::to_vec_pretty(session)?)
}
fn run<T>(path: &Path, state: &mut Session, operation: impl FnOnce() -> Result<T>) -> Result<T> {
    match operation() {
        Ok(value) => Ok(value),
        Err(error) => {
            let error=error.context("Ошибка движка шрифта интерфейса; повтор запрещён до перезапуска этого Конфигуратора");
            state.stage = Stage::Failed;
            state.applied = None;
            state.failure = Some(format!("{error:#}"));
            save(path, state)?;
            Err(error)
        }
    }
}
fn configure(
    path: &Path,
    state: &mut Session,
    previous: Session,
    operation: impl FnOnce(&str) -> Result<u32>,
) -> Result<()> {
    let first = matches!(previous.stage, Stage::Loading | Stage::Loaded);
    let command = if first { "UiFontStart" } else { "UiFontApply" };
    state.stage = Stage::Applying;
    save(path, state)?;
    let code = run(path, state, || operation(command))?;
    if code == ERROR_NOT_READY {
        // Start/Apply preflight has not changed the target font or installed hooks.
        // The DLL is already loaded; retry Start without another LoadLibrary call.
        if first {
            state.stage = Stage::Loaded;
            state.applied = None;
            state.failure = None;
        } else {
            *state = previous;
        }
        save(path, state)?;
        anyhow::bail!(
            "Конфигуратор ещё запускается; повторите применение после открытия главного окна"
        );
    }
    run(path, state, || {
        ensure!(code == 0, "{command}: Win32={code}");
        Ok(())
    })
}
fn effective(
    font: &UiFontSettings,
    installed: &[String],
) -> Result<(Option<String>, Option<String>)> {
    if !font.enabled {
        return Ok((None, None));
    }
    if installed
        .iter()
        .any(|name| name.eq_ignore_ascii_case(&font.family))
    {
        return Ok((Some(font.family.clone()), None));
    }
    ensure!(
        font.family == DEFAULT_FAMILY,
        "Шрифт интерфейса «{}» не установлен",
        font.family
    );
    if installed
        .iter()
        .any(|name| name.eq_ignore_ascii_case(FALLBACK_FAMILY))
    {
        return Ok((
            Some(FALLBACK_FAMILY.into()),
            Some(format!(
                "{DEFAULT_FAMILY} не установлен; используется {FALLBACK_FAMILY}."
            )),
        ));
    }
    Ok((
        None,
        Some(format!(
            "{DEFAULT_FAMILY} и {FALLBACK_FAMILY} не установлены; оставлен штатный шрифт 1С."
        )),
    ))
}
fn config(family: Option<&str>, log: &Path) -> Result<Vec<u8>> {
    let mut result = 1108u32.to_le_bytes().to_vec();
    for (value, capacity) in [
        (family.unwrap_or(""), 32),
        (log.to_str().context("Путь журнала не Unicode")?, 520),
    ] {
        let units: Vec<_> = value.encode_utf16().collect();
        ensure!(
            units.len() < capacity && !units.contains(&0),
            "Строка не помещается в протокол шрифта интерфейса"
        );
        for unit in units.into_iter().chain(std::iter::repeat(0)).take(capacity) {
            result.extend(unit.to_le_bytes());
        }
    }
    Ok(result)
}
fn field<'a>(data: &'a str, key: &str) -> Result<&'a str> {
    data.trim_start_matches('\u{feff}')
        .lines()
        .find_map(|line| line.strip_prefix(&format!("{key}=")))
        .context("Нет поля в ответе движка шрифта интерфейса")
}
fn confirm(data: &str, family: Option<&str>) -> Result<Option<String>> {
    ensure!(
        field(data, "enabled")? == if family.is_some() { "1" } else { "0" }
            && field(data, "family")? == family.unwrap_or("")
            && field(data, "refresh_status")? == "0"
            && field(data, "style_notifications")?.parse::<u32>()? > 0,
        "Движок не подтвердил горячее применение шрифта интерфейса"
    );
    Ok((field(data,"reopen_dialogs")?=="1").then(||"Переоткройте уже открытые диалоги, если подписи обрезаны: их размеры могли сохраниться от предыдущего шрифта.".into()))
}
fn retry_ready(process: &Process, address: usize, payload: &[u8]) -> Result<u32> {
    let deadline = Instant::now() + Duration::from_secs(5);
    loop {
        let code = process.call(address, Some(payload))?;
        // NOT_READY is guaranteed to return before changing font state or notifying
        // any subscriber. New Designer windows can still be starting or minimized.
        if code != ERROR_NOT_READY || Instant::now() >= deadline {
            return Ok(code);
        }
        std::thread::sleep(Duration::from_millis(100));
    }
}
pub fn apply_to(expected: &Identity, font: &UiFontSettings) -> Result<FontApplied> {
    font.validate()?;
    ensure!(
        manager::identity(expected.pid)? == *expected,
        "Процесс изменился; обновите список"
    );
    let directory = directory(expected)?;
    fs::create_dir_all(&directory)?;
    let _lock = Lock::acquire(&directory.join("control.lock"))?;
    let process = Process::open(expected.pid, true)?;
    process.matches(expected)?;
    let mut modules = process.modules()?;
    let path = directory.join("session.json");
    let saved: Option<Session> = if path.exists() {
        Some(serde_json::from_slice(&fs::read(&path)?)?)
    } else {
        None
    };
    let stock = || FontApplied {
        label: "Штатный шрифт 1С".into(),
        warning: None,
    };
    if modules.contains_key("uifontprobe.dll") {
        return Ok(FontApplied {label:"Не управляется".into(),warning:Some("В этом Конфигураторе работает экспериментальный прототип шрифта интерфейса. Для управления из приложения штатно перезапустите только этот Конфигуратор.".into())});
    }
    if !font.enabled && saved.is_none() {
        ensure!(
            !modules.contains_key("uifontengine.dll"),
            "Движок шрифта интерфейса загружен вне менеджера; перезапустите этот Конфигуратор"
        );
        return Ok(stock());
    }
    // Returning to the platform font does not need the installed-font catalog.
    let installed = if font.enabled {
        fonts::installed()?
    } else {
        vec![]
    };
    let (family, fallback) = effective(font, &installed)?;
    if family.is_none() && saved.is_none() && !modules.contains_key("uifontengine.dll") {
        return Ok(FontApplied {
            warning: fallback,
            ..stock()
        });
    }
    // Discovery can see a new Designer before its drawing modules are loaded.
    // Wait before injecting; a missing startup module is not an unsupported ABI.
    let deadline = Instant::now() + Duration::from_secs(5);
    while !(["frame.dll", "grphcs.dll"]
        .iter()
        .all(|name| modules.contains_key(*name))
        && ["core83.dll", "core85.dll"]
            .iter()
            .any(|name| modules.contains_key(*name)))
    {
        ensure!(
            Instant::now() < deadline,
            "Конфигуратор ещё запускается; повторите применение после открытия главного окна"
        );
        std::thread::sleep(Duration::from_millis(100));
        process.matches(expected)?;
        modules = process.modules()?;
    }
    for name in ["frame.dll", "grphcs.dll", "core83.dll", "core85.dll"] {
        if let Some((_, file)) = modules.get(name) {
            ensure!(
                win::same_path(
                    file,
                    &expected
                        .exe
                        .parent()
                        .context("Нет каталога платформы")?
                        .join(name)
                ),
                "Модуль {name} загружен из неожиданного каталога"
            );
        }
    }
    let digest = manager::hash(DLL);
    let runtime = manager::data_dir()?.join("ui-fonts/runtime").join(&digest);
    fs::create_dir_all(&runtime)?;
    let dll = runtime.join("UiFontEngine.dll");
    {
        let _lock = Lock::acquire(&runtime.join("runtime.lock"))?;
        if dll.exists() {
            ensure!(
                manager::hash(&fs::read(&dll)?) == digest,
                "Повреждена DLL шрифта интерфейса"
            );
        } else {
            atomic_write(&dll, DLL)?;
        }
    }
    let dll = win::canonical(&dll)?;
    let log = directory.join("ui-font.txt");
    fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log)?;
    let log = win::canonical(&log)?;
    let payload = config(family.as_deref(), &log)?;
    let mut state = if let Some((_, loaded)) = modules.get("uifontengine.dll") {
        ensure!(
            win::same_path(loaded, &dll),
            "В процессе другая версия шрифта интерфейса; перезапустите этот Конфигуратор"
        );
        let state = saved.context(
            "Движок шрифта интерфейса загружен вне менеджера; перезапустите этот Конфигуратор",
        )?;
        ensure!(
            state.identity == *expected && state.dll_sha256 == digest,
            "Несовместимая сессия шрифта интерфейса; перезапустите этот Конфигуратор"
        );
        session_result(&state)?;
        if state.requested == *font
            && state.effective == family
            && let Some(applied) = &state.applied
        {
            return Ok(applied.clone());
        }
        state
    } else {
        ensure!(
            saved.is_none(),
            "Движок шрифта интерфейса отсутствует при существующей сессии; перезапустите этот Конфигуратор"
        );
        Session {
            identity: expected.clone(),
            dll_sha256: digest,
            stage: Stage::Loading,
            requested: font.clone(),
            effective: family.clone(),
            applied: None,
            failure: None,
        }
    };
    let load = state.stage == Stage::Loading;
    let previous = state.clone();
    state.requested = font.clone();
    state.effective = family.clone();
    if state.stage == Stage::Unsupported {
        let result=FontApplied {label:stock().label,warning:font.enabled.then(||"Платформа не поддерживает горячее обновление интерфейсного шрифта; тема и шрифт редактора применяются отдельно.".into())};
        state.applied = Some(result.clone());
        save(&path, &state)?;
        return Ok(result);
    }
    save(&path, &state)?;
    let exports = run(&path, &mut state, || {
        if load {
            process.load_dll(&dll)?;
        }
        process.named_exports(&dll, EXPORTS)
    })?;
    let capabilities = run(&path, &mut state, || {
        process.call(exports["UiFontCapabilities"], None)
    })?;
    if capabilities != 3 {
        state.stage = Stage::Unsupported;
        state.applied=Some(FontApplied {label:stock().label,warning:font.enabled.then(||"API горячего обновления шрифта интерфейса этой платформы не распознан; оставлен штатный шрифт 1С.".into())});
        save(&path, &state)?;
        return state
            .applied
            .context("Нет результата проверки шрифта интерфейса");
    }
    configure(&path, &mut state, previous, |command| {
        retry_ready(&process, exports[command], &payload)
    })?;
    let result = run(&path, &mut state, || {
        let mut refresh = 8u32.to_le_bytes().to_vec();
        refresh.extend(5u32.to_le_bytes());
        let code = retry_ready(&process, exports["UiFontRefresh"], &refresh)?;
        ensure!(
            code == 0,
            "Шрифт настроен, но горячее обновление завершилось ошибкой Win32={code}"
        );
        let data =
            String::from_utf8(fs::read(&log)?).context("Ответ шрифта интерфейса не UTF-8")?;
        let dialog_warning = confirm(&data, family.as_deref())?;
        let warning = [fallback, dialog_warning]
            .into_iter()
            .flatten()
            .collect::<Vec<_>>()
            .join(" ");
        Ok(FontApplied {
            label: family.unwrap_or_else(|| stock().label),
            warning: (!warning.is_empty()).then_some(warning),
        })
    })?;
    state.stage = Stage::Ready;
    state.applied = Some(result.clone());
    state.failure = None;
    save(&path, &state)?;
    Ok(result)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn session(stage: Stage) -> Session {
        Session {
            identity: Identity {
                pid: 1,
                created_time: 2,
                exe: "1cv8.exe".into(),
            },
            dll_sha256: "hash".into(),
            stage,
            requested: UiFontSettings::default(),
            effective: Some(DEFAULT_FAMILY.into()),
            applied: None,
            failure: None,
        }
    }
    fn restore(path: &Path) -> Session {
        serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
    }
    #[test]
    fn not_ready_start_can_be_retried_after_reloading_session() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        let mut state = session(Stage::Loading);
        for _ in 0..2 {
            let previous = state.clone();
            let error = configure(&path, &mut state, previous, |command| {
                assert_eq!(command, "UiFontStart");
                Ok(ERROR_NOT_READY)
            })
            .unwrap_err();
            assert!(error.to_string().contains("Конфигуратор ещё запускается"));
            state = restore(&path);
            assert_eq!(state.stage, Stage::Loaded);
            assert!(session_result(&state).unwrap().is_none());
            assert!(state.failure.is_none());
        }
        let previous = state.clone();
        configure(&path, &mut state, previous, |command| {
            assert_eq!(command, "UiFontStart");
            Ok(0)
        })
        .unwrap();
        assert_eq!(restore(&path).stage, Stage::Applying);
    }
    #[test]
    fn not_ready_apply_preserves_previous_font_and_allows_retry() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        let mut state = session(Stage::Ready);
        state.applied = Some(FontApplied {
            label: DEFAULT_FAMILY.into(),
            warning: Some("previous warning".into()),
        });
        let previous = state.clone();
        let expected = serde_json::to_value(&previous).unwrap();
        state.requested.family = "Roboto".into();
        state.effective = Some("Roboto".into());
        assert!(
            configure(&path, &mut state, previous, |command| {
                assert_eq!(command, "UiFontApply");
                Ok(ERROR_NOT_READY)
            })
            .is_err()
        );
        state = restore(&path);
        assert_eq!(serde_json::to_value(&state).unwrap(), expected);
        assert_eq!(
            session_result(&state).unwrap().unwrap().label,
            DEFAULT_FAMILY
        );
        let previous = state.clone();
        state.requested.family = "Roboto".into();
        state.effective = Some("Roboto".into());
        configure(&path, &mut state, previous, |command| {
            assert_eq!(command, "UiFontApply");
            Ok(0)
        })
        .unwrap();
        assert_eq!(restore(&path).effective.as_deref(), Some("Roboto"));
    }
    #[test]
    fn other_configuration_failures_remain_terminal() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        for stage in [Stage::Loading, Stage::Loaded, Stage::Ready] {
            for timeout in [false, true] {
                let mut state = session(stage);
                let previous = state.clone();
                let error = configure(&path, &mut state, previous, |_| {
                    if timeout {
                        anyhow::bail!("timeout 1460");
                    }
                    Ok(50)
                })
                .unwrap_err();
                let saved = restore(&path);
                assert_eq!(saved.stage, Stage::Failed);
                let diagnostic = format!("{:#}", session_result(&saved).unwrap_err());
                assert!(diagnostic.contains(if timeout { "1460" } else { "Win32=50" }));
                assert_eq!(diagnostic, format!("{error:#}"));
            }
        }
    }
    #[test]
    fn not_ready_refresh_after_successful_configuration_remains_terminal() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        let mut state = session(Stage::Loading);
        let previous = state.clone();
        configure(&path, &mut state, previous, |_| Ok(0)).unwrap();
        assert!(
            run::<()>(&path, &mut state, || {
                anyhow::bail!("UiFontRefresh: Win32={ERROR_NOT_READY}")
            })
            .is_err()
        );
        assert_eq!(restore(&path).stage, Stage::Failed);
    }
    #[test]
    fn default_fallback_and_custom_missing_fonts() {
        let font = UiFontSettings::default();
        assert_eq!(
            effective(&font, &[DEFAULT_FAMILY.into()]).unwrap(),
            (Some(DEFAULT_FAMILY.into()), None)
        );
        let (family, warning) = effective(&font, &[FALLBACK_FAMILY.into()]).unwrap();
        assert_eq!(family.as_deref(), Some(FALLBACK_FAMILY));
        assert!(warning.is_some());
        assert!(effective(&font, &[]).unwrap().0.is_none());
        assert!(
            effective(
                &UiFontSettings {
                    family: "Missing".into(),
                    ..font
                },
                &[]
            )
            .is_err()
        );
    }
    #[test]
    fn utf16_protocol_and_hot_confirmation_are_strict() {
        let payload = config(Some("Segoe UI"), Path::new("C:/шрифты/ui.txt")).unwrap();
        assert_eq!(payload.len(), 1108);
        assert_eq!(&payload[..4], &1108u32.to_le_bytes());
        assert!(config(Some(&"x".repeat(32)), Path::new("x")).is_err());
        let valid = "\u{feff}enabled=1\nfamily=Segoe UI\nrefresh_status=0\nstyle_notifications=1\nreopen_dialogs=0\n";
        assert!(confirm(valid, Some("Segoe UI")).unwrap().is_none());
        assert!(
            confirm(
                &valid.replace("reopen_dialogs=0", "reopen_dialogs=1"),
                Some("Segoe UI")
            )
            .unwrap()
            .is_some()
        );
        assert!(confirm(valid, Some("Roboto")).is_err());
        assert!(
            confirm(
                &valid.replace("refresh_status=0", "refresh_status=50"),
                Some("Segoe UI")
            )
            .is_err()
        );
        assert!(
            confirm(
                &valid.replace("style_notifications=1", "style_notifications=0"),
                Some("Segoe UI")
            )
            .is_err()
        );
    }
    #[test]
    fn timeout_failure_is_persisted_and_not_retried() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("session.json");
        let mut state = Session {
            identity: Identity {
                pid: 1,
                created_time: 2,
                exe: "1cv8.exe".into(),
            },
            dll_sha256: "hash".into(),
            stage: Stage::Applying,
            requested: UiFontSettings::default(),
            effective: Some(DEFAULT_FAMILY.into()),
            applied: None,
            failure: None,
        };
        assert!(run::<()>(&path, &mut state, || anyhow::bail!("timeout 1460")).is_err());
        let saved: Session = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();
        assert_eq!(saved.stage, Stage::Failed);
        assert!(format!("{:#}", session_result(&saved).unwrap_err()).contains("1460"));
    }
}
