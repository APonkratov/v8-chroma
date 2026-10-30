use super::FontSettings;
use crate::{
    manager,
    storage::{Lock, atomic_write},
    win::{self, Identity, Process},
};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use std::{collections::BTreeMap, fs, path::Path};
use windows_sys::Win32::Graphics::Gdi::*;

const DLL: &[u8] = include_bytes!("../../assets/FontProbe.dll");
const EXPORTS: &[&str] = &[
    "FontProbeStart",
    "FontProbeApply",
    "FontProbeStop",
    "FontProbeRefresh",
    "FontProbeCapabilities",
];

/// GDI enumerates local fonts; no catalog request or font download is made.
pub fn installed() -> Result<Vec<String>> {
    unsafe extern "system" fn visit(
        lf: *const LOGFONTW,
        tm: *const TEXTMETRICW,
        _kind: u32,
        data: isize,
    ) -> i32 {
        unsafe {
            // Despite its name, GDI sets TMPF_FIXED_PITCH for proportional fonts.
            let monospace = (*tm).tmPitchAndFamily & TMPF_FIXED_PITCH == 0;
            let face = &(*lf).lfFaceName;
            let name = String::from_utf16_lossy(
                &face[..face.iter().position(|v| *v == 0).unwrap_or(face.len())],
            );
            if !name.starts_with('@') && !name.is_empty() {
                (&mut *(data as *mut BTreeMap<String, bool>))
                    .entry(name)
                    .and_modify(|fixed| *fixed |= monospace)
                    .or_insert(monospace);
            }
        }
        1
    }
    let mut faces = BTreeMap::new();
    unsafe {
        let dc = CreateCompatibleDC(std::ptr::null_mut());
        ensure!(!dc.is_null(), "Не удалось прочитать установленные шрифты");
        let mut filter: LOGFONTW = std::mem::zeroed();
        filter.lfCharSet = DEFAULT_CHARSET;
        EnumFontFamiliesExW(dc, &filter, Some(visit), &mut faces as *mut _ as isize, 0);
        DeleteDC(dc);
    }
    Ok(sort_fonts(faces))
}

fn sort_fonts(faces: BTreeMap<String, bool>) -> Vec<String> {
    let popular = [
        "JetBrains Mono Medium",
        "JetBrains Mono",
        "Cascadia Code",
        "Fira Code",
        "Consolas",
        "Courier New",
    ];
    let mut faces: Vec<_> = faces.into_iter().collect();
    faces.sort_by_key(|(name, monospace)| {
        (
            !monospace,
            popular
                .iter()
                .position(|p| name.eq_ignore_ascii_case(p))
                .unwrap_or(popular.len()),
            name.to_lowercase(),
        )
    });
    faces.into_iter().map(|(name, _)| name).collect()
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct FontApplied {
    pub label: String,
    pub warning: Option<String>,
}
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
struct Session {
    identity: Identity,
    dll_sha256: String,
    stage: String,
    requested: FontSettings,
    applied: Option<FontApplied>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    failure: Option<String>,
}
fn session_result(saved: &Session) -> Result<Option<FontApplied>> {
    if saved.stage == "failed" {
        anyhow::bail!(
            "{}",
            saved
                .failure
                .as_deref()
                .unwrap_or("Ошибка движка шрифтов; перезапустите этот Конфигуратор")
        );
    }
    ensure!(
        saved.stage == "ready" || saved.stage == "unsupported",
        "Предыдущее применение шрифта не завершено. Повтор запрещён; перезапустите этот Конфигуратор"
    );
    Ok(saved.applied.clone())
}
fn unsupported_session(state: &mut Session, path: &Path) -> Result<FontApplied> {
    let applied = FontApplied {
        label: "Шрифт 1С".into(),
        warning: state.requested.enabled.then(|| {
            "API шрифта этой платформы несовместим с движком; тема применяется независимо".into()
        }),
    };
    state.stage = "unsupported".into();
    state.applied = Some(applied.clone());
    state.failure = None;
    atomic_write(path, &serde_json::to_vec_pretty(state)?)?;
    Ok(applied)
}
fn prepare_engine<T>(
    state: &mut Session,
    path: &Path,
    probe: impl FnOnce() -> Result<(T, u32)>,
) -> Result<Option<(T, u32)>> {
    match probe() {
        Ok((exports, capabilities)) if capabilities & 1 != 0 => Ok(Some((exports, capabilities))),
        Ok(_) => {
            unsupported_session(state, path)?;
            Ok(None)
        }
        Err(error) => {
            let error = error.context("Не удалось подготовить движок шрифтов; повтор запрещён до перезапуска Конфигуратора");
            state.stage = "failed".into();
            state.failure = Some(format!("{error:#}"));
            state.applied = None;
            atomic_write(path, &serde_json::to_vec_pretty(state)?)?;
            Err(error)
        }
    }
}
fn directory(identity: &Identity) -> Result<std::path::PathBuf> {
    Ok(manager::data_dir()?
        .join("fonts")
        .join("sessions")
        .join(format!("{}-{}", identity.pid, identity.created_time)))
}
pub fn last_applied(identity: &Identity) -> Result<Option<FontApplied>> {
    let path = directory(identity)?.join("session.json");
    if !path.exists() {
        return Ok(None);
    }
    let saved: Session = serde_json::from_slice(&fs::read(path)?)?;
    ensure!(
        saved.identity == *identity && saved.dll_sha256 == manager::hash(DLL),
        "Изменилась версия движка шрифтов; перезапустите этот Конфигуратор"
    );
    session_result(&saved)
}
fn push_wide(out: &mut Vec<u8>, value: &str, capacity: usize) -> Result<()> {
    let units: Vec<_> = value.encode_utf16().collect();
    ensure!(
        units.len() < capacity && !units.contains(&0),
        "Строка не помещается в протокол шрифта"
    );
    for unit in units.into_iter().chain(std::iter::repeat(0)).take(capacity) {
        out.extend(unit.to_le_bytes());
    }
    Ok(())
}
fn config(font: &FontSettings, log: &Path) -> Result<Vec<u8>> {
    let mut bytes = 1176u32.to_le_bytes().to_vec();
    bytes.extend(font.points10.to_le_bytes());
    push_wide(&mut bytes, "Courier New", 32)?;
    push_wide(&mut bytes, &font.family, 32)?;
    push_wide(
        &mut bytes,
        log.to_str().context("Путь журнала не Unicode")?,
        520,
    )?;
    Ok(bytes)
}
fn field(data: &str, key: &str) -> Result<u64> {
    data.lines()
        .find_map(|line| line.strip_prefix(&format!("{key}=")))
        .context("Нет поля в ответе движка шрифта")?
        .parse()
        .context("Некорректный ответ движка шрифта")
}

fn prepare_log(directory: &Path) -> Result<std::path::PathBuf> {
    let log = directory.join("font.txt");
    // Resolve an existing file, not a prospective path: Windows may redirect
    // the manager's AppData writes while Designer runs outside that package.
    // Append preserves diagnostics from an earlier application.
    fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(&log)
        .context("Не удалось подготовить журнал шрифта")?;
    win::canonical(&log)
}

const REOPEN_EDITOR: &str = "Переоткройте модуль для применения шрифта: механизм обновления открытого редактора не распознан.";

fn confirm_refresh(code: u32, data: &str, enabled: bool, replaced: u64) -> Result<Option<String>> {
    // Native NOT_SUPPORTED is returned before changing any editor's font.
    // The conversion hook is already configured and can serve new editors.
    if code == windows_sys::Win32::Foundation::ERROR_NOT_SUPPORTED {
        return Ok(Some(REOPEN_EDITOR.into()));
    }
    ensure!(
        code == 0,
        "Шрифт настроен, но обновление редактора завершилось ошибкой Win32={code}. Повтор запрещён до перезапуска Конфигуратора"
    );
    ensure!(
        field(data, "enabled")? == u64::from(enabled) && field(data, "editor_status")? == 0,
        "Движок не подтвердил применение шрифта"
    );
    Ok((enabled && field(data, "editor_last_count")? > 0 && field(data, "replaced")? == replaced)
        .then(|| "Редактор не использует Courier New. Выберите его в параметрах шрифта 1С и примените снова.".into()))
}

pub fn apply_to(expected: &Identity, font: &FontSettings) -> Result<FontApplied> {
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
    let modules = process.modules()?;
    let state_path = directory.join("session.json");
    let mut saved: Option<Session> = if state_path.exists() {
        Some(serde_json::from_slice(&fs::read(&state_path)?)?)
    } else {
        None
    };
    let stock = FontApplied {
        label: "Шрифт 1С".into(),
        warning: None,
    };
    // A disabled choice must not inject a DLL into an untouched process.
    if !font.enabled && saved.is_none() {
        return Ok(if modules.contains_key("fontprobe.dll") {
            FontApplied {label:"Не управляется".into(),warning:Some("Движок шрифтов загружен вне менеджера. Перезапустите этот Конфигуратор для управления из менеджера.".into())}
        } else {
            stock
        });
    }
    if font.enabled {
        ensure!(
            installed()?
                .iter()
                .any(|name| name.eq_ignore_ascii_case(&font.family)),
            "Шрифт «{}» не установлен",
            font.family
        );
    }
    for name in ["grphcs.dll", "core83.dll", "core85.dll", "txtedui.dll"] {
        if let Some((_, path)) = modules.get(name) {
            ensure!(
                win::same_path(
                    path,
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
    let runtime = manager::data_dir()?
        .join("fonts")
        .join("runtime")
        .join(manager::hash(DLL));
    fs::create_dir_all(&runtime)?;
    let dll = runtime.join("FontProbe.dll");
    {
        let _lock = Lock::acquire(&runtime.join("runtime.lock"))?;
        if dll.exists() {
            ensure!(
                manager::hash(&fs::read(&dll)?) == manager::hash(DLL),
                "Повреждена DLL шрифтов"
            );
        } else {
            atomic_write(&dll, DLL)?;
        }
    }
    let dll = win::canonical(&dll)?;
    let log = prepare_log(&directory)?;
    let payload = config(font, &log)?;
    if let Some((_, loaded)) = modules.get("fontprobe.dll") {
        ensure!(
            win::same_path(loaded, &dll),
            "В процессе загружена другая версия шрифтов; перезапустите этот Конфигуратор"
        );
        let state = saved
            .as_ref()
            .context("Движок шрифтов загружен вне менеджера; перезапустите этот Конфигуратор")?;
        ensure!(
            state.identity == *expected && state.dll_sha256 == manager::hash(DLL),
            "Незавершённое или несовместимое применение шрифта. Повтор запрещён; перезапустите этот Конфигуратор"
        );
        if state.stage == "unsupported" {
            let state = saved.as_mut().context("Нет сессии шрифта")?;
            state.requested = font.clone();
            return unsupported_session(state, &state_path);
        }
        session_result(state)?;
    } else {
        ensure!(
            saved.is_none(),
            "Движок шрифтов отсутствует при существующей сессии; перезапустите Конфигуратор"
        );
        saved = Some(Session {
            identity: expected.clone(),
            dll_sha256: manager::hash(DLL),
            stage: "loading".into(),
            requested: font.clone(),
            applied: None,
            failure: None,
        });
        atomic_write(&state_path, &serde_json::to_vec_pretty(&saved)?)?;
    }
    let state = saved.as_mut().context("Нет сессии шрифта")?;
    state.requested = font.clone();
    let Some((exports, capabilities)) = prepare_engine(state, &state_path, || {
        if !modules.contains_key("fontprobe.dll") {
            process.load_dll(&dll)?;
        }
        let exports = process.named_exports(&dll, EXPORTS)?;
        let capabilities = process.call(exports["FontProbeCapabilities"], None)?;
        Ok((exports, capabilities))
    })?
    else {
        return state
            .applied
            .clone()
            .context("Нет результата проверки API шрифта");
    };
    let first = state.stage == "loading";
    state.stage = "applying".into();
    state.requested = font.clone();
    atomic_write(&state_path, &serde_json::to_vec_pretty(state)?)?;
    let command = if first {
        "FontProbeStart"
    } else if font.enabled {
        "FontProbeApply"
    } else {
        "FontProbeStop"
    };
    let code = process.call(
        exports[command],
        if font.enabled { Some(&payload) } else { None },
    )?;
    ensure!(
        code == 0,
        "{command}: Win32={code}; повтор запрещён до перезапуска Конфигуратора"
    );
    let before = String::from_utf8_lossy(&fs::read(&log)?).into_owned();
    let replaced = field(&before, "replaced")?;
    let mut warning = None;
    if modules.contains_key("txtedui.dll") && capabilities & 2 != 0 {
        let mut refresh = 8u32.to_le_bytes().to_vec();
        refresh.extend(4u32.to_le_bytes());
        let code = process.call(exports["FontProbeRefresh"], Some(&refresh))?;
        let data = String::from_utf8_lossy(&fs::read(&log)?).into_owned();
        warning = confirm_refresh(code, &data, font.enabled, replaced)?;
    } else if modules.contains_key("txtedui.dll") {
        warning = Some(REOPEN_EDITOR.into());
    }
    let applied = if font.enabled {
        FontApplied {
            label: format!("{} · {} pt", font.family, font.points10 as f32 / 10.),
            warning,
        }
    } else {
        FontApplied { warning, ..stock }
    };
    state.stage = "ready".into();
    state.applied = Some(applied.clone());
    atomic_write(&state_path, &serde_json::to_vec_pretty(state)?)?;
    Ok(applied)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn loading_session() -> Session {
        Session {
            identity: Identity {
                pid: 42,
                created_time: 7,
                exe: "C:/Platform/1cv8.exe".into(),
            },
            dll_sha256: manager::hash(DLL),
            stage: "loading".into(),
            requested: FontSettings {
                enabled: true,
                ..Default::default()
            },
            applied: None,
            failure: None,
        }
    }
    #[test]
    fn preparation_failures_remain_terminal_and_keep_original_diagnostics() {
        for cause in [
            "LoadLibrary failed",
            "missing FontProbeCapabilities export",
            "capabilities Win32=1460",
        ] {
            let directory = tempfile::tempdir().unwrap();
            let path = directory.path().join("session.json");
            let mut state = loading_session();
            let error =
                prepare_engine::<()>(&mut state, &path, || anyhow::bail!("{cause}")).unwrap_err();
            let restored: Session = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();
            assert_eq!(restored.stage, "failed");
            let diagnostic = format!("{:#}", session_result(&restored).unwrap_err());
            assert_eq!(diagnostic, format!("{error:#}"));
            assert!(diagnostic.contains(cause) && diagnostic.contains("перезапуска"));
            assert!(!diagnostic.contains("не завершено"));
        }
    }
    #[test]
    fn unsupported_api_is_a_persisted_warning_and_can_return_to_stock() {
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("session.json");
        let mut state = loading_session();
        assert!(
            prepare_engine(&mut state, &path, || Ok(((), 0)))
                .unwrap()
                .is_none()
        );
        let mut restored: Session = serde_json::from_slice(&fs::read(&path).unwrap()).unwrap();
        assert_eq!(restored.stage, "unsupported");
        let applied = session_result(&restored).unwrap().unwrap();
        assert_eq!(applied.label, "Шрифт 1С");
        assert!(
            applied
                .warning
                .unwrap()
                .contains("тема применяется независимо")
        );
        restored.requested.enabled = false;
        assert!(
            unsupported_session(&mut restored, &path)
                .unwrap()
                .warning
                .is_none()
        );
        let disabled: Session = serde_json::from_slice(&fs::read(path).unwrap()).unwrap();
        assert!(
            session_result(&disabled)
                .unwrap()
                .unwrap()
                .warning
                .is_none()
        );
    }
    #[test]
    fn interrupted_sessions_and_old_documents_still_fail_closed() {
        let mut state = loading_session();
        for stage in ["loading", "applying"] {
            state.stage = stage.into();
            let bytes = serde_json::to_vec(&state).unwrap();
            assert!(!String::from_utf8_lossy(&bytes).contains("failure"));
            let restored: Session = serde_json::from_slice(&bytes).unwrap();
            assert!(session_result(&restored).is_err());
        }
    }
    #[test]
    fn font_payload_layout_and_unicode_log_path() {
        let data = config(&FontSettings::default(), Path::new("C:/Профиль/шрифт.txt")).unwrap();
        assert_eq!(data.len(), 1176);
        assert_eq!(&data[..8], &[152, 4, 0, 0, 130, 0, 0, 0]);
        assert!(config(&FontSettings::default(), Path::new(&"x".repeat(520))).is_err());
    }
    #[test]
    fn local_catalog_includes_proportional_and_monospace_fonts() {
        let fonts = installed().unwrap();
        assert!(fonts.iter().any(|name| name == "Courier New"));
        assert!(fonts.iter().any(|name| name == "Arial"));
        assert!(fonts.iter().any(|name| name == "Segoe UI"));
        assert!(fonts.iter().all(|name| !name.starts_with('@')));
        assert!(
            fonts.iter().position(|name| name == "Courier New")
                < fonts.iter().position(|name| name == "Arial")
        );
    }
    #[test]
    fn monospace_priority_does_not_filter_other_families() {
        let faces = BTreeMap::from([
            ("Arial".into(), false),
            ("Consolas".into(), true),
            ("Other Fixed".into(), true),
            ("Segoe UI".into(), false),
            ("JetBrains Mono Medium".into(), true),
        ]);
        assert_eq!(
            sort_fonts(faces),
            [
                "JetBrains Mono Medium",
                "Consolas",
                "Other Fixed",
                "Arial",
                "Segoe UI"
            ]
        );
    }
    #[test]
    fn remote_log_resolves_an_existing_file_without_erasing_diagnostics() {
        let directory = tempfile::tempdir().unwrap();
        let log = prepare_log(directory.path()).unwrap();
        assert!(log.is_absolute() && log.is_file());
        fs::write(&log, "previous snapshot").unwrap();
        assert_eq!(prepare_log(directory.path()).unwrap(), log);
        assert_eq!(fs::read_to_string(log).unwrap(), "previous snapshot");
    }
    #[test]
    fn unknown_editor_can_reopen_but_timeouts_and_unconfirmed_success_are_errors() {
        assert_eq!(
            confirm_refresh(50, "", true, 0).unwrap().as_deref(),
            Some(REOPEN_EDITOR)
        );
        assert_eq!(
            confirm_refresh(50, "", false, 0).unwrap().as_deref(),
            Some(REOPEN_EDITOR)
        );
        assert!(confirm_refresh(1460, "", true, 0).is_err());
        assert!(confirm_refresh(0, "enabled=1\neditor_status=50", true, 0).is_err());
        let snapshot = "enabled=1\neditor_status=0\neditor_last_count=1\nreplaced=2";
        assert!(confirm_refresh(0, snapshot, true, 1).unwrap().is_none());
    }
}
