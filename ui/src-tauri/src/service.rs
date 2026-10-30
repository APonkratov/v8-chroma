use anyhow::{Result, ensure};
use serde::Serialize;
use std::{
    collections::BTreeMap,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc::{self, Sender},
    },
    time::{Duration, Instant},
};
use v8_chroma_core::{
    fonts, manager,
    settings::{Loaded, Settings, SettingsPatch, Store, WindowPatch},
    watch::Controller,
    win::Identity,
};

#[derive(Clone, Serialize)]
pub struct ProcessRow {
    pub pid: u32,
    pub created_time: Option<String>,
    pub title: String,
    pub exe: Option<String>,
    pub command_line: Option<String>,
    pub eligible: bool,
    pub status: String,
    pub theme: Option<String>,
    pub detail: Option<String>,
    pub icons: Option<String>,
    pub icon_warning: Option<String>,
    pub font: Option<String>,
    pub font_warning: Option<String>,
}
#[derive(Clone, Serialize)]
pub struct Snapshot {
    pub settings: Option<Loaded>,
    pub processes: Vec<ProcessRow>,
    pub error: Option<String>,
    pub busy: bool,
    pub stopping: bool,
    pub version: &'static str,
}
enum Request {
    Wake,
    Apply(Vec<Identity>, Settings),
    Geometry(WindowPatch),
    Quit,
}
struct Shared {
    snapshot: Mutex<Snapshot>,
    busy: AtomicBool,
    stop: AtomicBool,
    finished: AtomicBool,
}
#[derive(Clone)]
pub struct Service {
    pub store: Store,
    shared: Arc<Shared>,
    tx: Sender<Request>,
}
impl Service {
    pub fn start(store: Store, on_exit: impl FnOnce() + Send + 'static) -> Self {
        let loaded = store.ensure_initialized();
        let (settings, error) = match loaded {
            Ok(value) => (Some(value), None),
            Err(error) => (None, Some(format!("{error:#}"))),
        };
        let shared = Arc::new(Shared {
            snapshot: Mutex::new(Snapshot {
                settings,
                processes: vec![],
                error,
                busy: false,
                stopping: false,
                version: env!("CARGO_PKG_VERSION"),
            }),
            busy: AtomicBool::new(false),
            stop: AtomicBool::new(false),
            finished: AtomicBool::new(false),
        });
        let (tx, rx) = mpsc::channel();
        let service = Self { store, shared, tx };
        let background = service.clone();
        std::thread::spawn(move || {
            let mut controller = Controller::default();
            let mut last_scan = Instant::now() - Duration::from_secs(3);
            let mut geometry: Option<(Instant, WindowPatch)> = None;
            let mut results: BTreeMap<(u32, u64), ProcessRow> = BTreeMap::new();
            while !background.shared.stop.load(Ordering::Acquire) {
                match rx.recv_timeout(Duration::from_millis(100)) {
                    Ok(request) => {
                        let requests = std::iter::once(request).chain(rx.try_iter());
                        for request in requests {
                            match request {
                                Request::Wake => {
                                    last_scan = Instant::now() - Duration::from_secs(3)
                                }
                                Request::Quit => {
                                    background.shared.stop.store(true, Ordering::Release)
                                }
                                Request::Geometry(patch) => {
                                    let previous =
                                        geometry.take().map(|(_, patch)| patch).unwrap_or_default();
                                    geometry =
                                        Some((Instant::now(), merge_geometry(previous, patch)));
                                }
                                Request::Apply(targets, settings) => {
                                    for identity in controller.manual(&targets, &settings) {
                                        if background.shared.stop.load(Ordering::Acquire) {
                                            break;
                                        }
                                        background.apply(&identity, &settings, &mut results);
                                    }
                                    background.shared.busy.store(false, Ordering::Release);
                                    last_scan = Instant::now() - Duration::from_secs(3);
                                }
                            }
                        }
                    }
                    Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => {}
                }
                if geometry
                    .as_ref()
                    .is_some_and(|(changed, _)| changed.elapsed() >= Duration::from_millis(500))
                    && let Some((_, patch)) = geometry.take()
                {
                    background.save_geometry(patch);
                }
                if background.shared.stop.load(Ordering::Acquire) {
                    break;
                }
                if last_scan.elapsed() < Duration::from_secs(2) {
                    continue;
                }
                last_scan = Instant::now();
                let loaded = background.store.load();
                let settings = match loaded {
                    Ok(loaded) => {
                        let settings = loaded.settings.clone();
                        background.edit(|s| {
                            s.settings = Some(loaded);
                            s.error = None;
                        });
                        Some(settings)
                    }
                    Err(error) => {
                        controller.pause();
                        background.edit(|s| {
                            s.settings = None;
                            s.error = Some(format!("{error:#}"));
                        });
                        None
                    }
                };
                match manager::discover() {
                    Ok(candidates) => {
                        let identities: Vec<_> = candidates
                            .iter()
                            .filter(|c| c.eligible)
                            .filter_map(|c| c.identity.clone())
                            .collect();
                        results.retain(|key, _| {
                            identities.iter().any(|i| (i.pid, i.created_time) == *key)
                        });
                        let rows = candidates
                            .into_iter()
                            .map(|candidate| {
                                let identity = candidate.identity.as_ref();
                                let key = identity.map(|i| (i.pid, i.created_time));
                                let mut row = key
                                    .and_then(|key| results.get(&key).cloned())
                                    .unwrap_or_else(|| ProcessRow {
                                        pid: candidate.pid,
                                        command_line: None,
                                        created_time: identity.map(|i| i.created_time.to_string()),
                                        title: if candidate.eligible {
                                            "Конфигуратор".into()
                                        } else {
                                            "1С:Предприятие".into()
                                        },
                                        exe: identity.map(|i| i.exe.display().to_string()),
                                        eligible: candidate.eligible,
                                        status: if candidate.eligible {
                                            "idle".into()
                                        } else {
                                            "unsupported".into()
                                        },
                                        theme: None,
                                        icons: None,
                                        icon_warning: None,
                                        font: None,
                                        font_warning: None,
                                        detail: if candidate.eligible {
                                            None
                                        } else {
                                            Some(candidate.reason)
                                        },
                                    });
                                row.command_line = candidate.command_line;
                                if row.status != "error"
                                    && let Some(identity) = identity
                                {
                                    match manager::last_applied(identity) {
                                        Ok(Some(applied)) => {
                                            row.theme = Some(applied.theme);
                                            row.icons = Some(applied.icons);
                                            row.icon_warning = applied.icon_warning;
                                            row.status = "applied".into();
                                        }
                                        Err(error) => {
                                            row.status = "error".into();
                                            row.detail = Some(format!("{error:#}"));
                                        }
                                        _ => {}
                                    }
                                }
                                if let Some(identity) = identity {
                                    match fonts::last_applied(identity) {
                                        Ok(Some(font)) => {
                                            row.font = Some(font.label);
                                            row.font_warning = font.warning;
                                        }
                                        Err(error) => {
                                            row.status = "error".into();
                                            row.detail = Some(format!("{error:#}"));
                                        }
                                        _ => {}
                                    }
                                }
                                row
                            })
                            .collect();
                        background.edit(|s| s.processes = rows);
                        if let Some(settings) = settings {
                            if !settings.auto_apply {
                                controller.pause();
                            } else if background
                                .shared
                                .busy
                                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                                .is_ok()
                            {
                                for identity in controller.plan(&identities, &settings) {
                                    if background.shared.stop.load(Ordering::Acquire) {
                                        break;
                                    }
                                    background.apply(&identity, &settings, &mut results);
                                }
                                background.shared.busy.store(false, Ordering::Release);
                            }
                        }
                    }
                    Err(error) => {
                        controller.pause();
                        background.edit(|s| {
                            s.error = Some(format!("Не удалось обновить процессы: {error:#}"))
                        });
                    }
                }
            }
            if let Some((_, patch)) = geometry {
                background.save_geometry(patch);
            }
            background.shared.finished.store(true, Ordering::Release);
            on_exit();
        });
        service
    }
    fn edit(&self, update: impl FnOnce(&mut Snapshot)) {
        update(
            &mut self
                .shared
                .snapshot
                .lock()
                .unwrap_or_else(|e| e.into_inner()),
        );
    }
    pub fn snapshot(&self) -> Snapshot {
        let mut state = self
            .shared
            .snapshot
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clone();
        state.busy = self.shared.busy.load(Ordering::Acquire);
        state.stopping = self.shared.stop.load(Ordering::Acquire);
        state
    }
    pub fn save(&self, patch: SettingsPatch) -> Result<Loaded> {
        ensure!(
            !self.shared.stop.load(Ordering::Acquire),
            "Приложение завершается"
        );
        let loaded = self.store.update(patch)?;
        self.edit(|s| {
            s.settings = Some(loaded.clone());
            s.error = None;
        });
        let _ = self.tx.send(Request::Wake);
        Ok(loaded)
    }
    pub fn set_appearance(&self, patch: SettingsPatch) -> Result<Loaded> {
        ensure!(
            patch.auto_apply.is_none()
                && patch.window.is_none()
                && (patch.selected_theme.is_some()
                    || patch.custom_icons.is_some()
                    || patch.editor_font.is_some()),
            "Укажите тему, значки или шрифт"
        );
        ensure!(
            !self.shared.busy.load(Ordering::Acquire),
            "Дождитесь завершения текущего применения"
        );
        // The watcher applies saved choices according to auto_apply. Only the
        // explicit Apply command is allowed to bypass that policy.
        self.save(patch)
    }
    pub fn reset(&self) -> Result<Loaded> {
        let loaded = self.store.reset()?;
        self.edit(|s| {
            s.settings = Some(loaded.clone());
            s.error = None;
        });
        let _ = self.tx.send(Request::Wake);
        Ok(loaded)
    }
    pub fn refresh(&self) {
        let _ = self.tx.send(Request::Wake);
    }
    pub fn geometry(&self, patch: WindowPatch) {
        let _ = self.tx.send(Request::Geometry(patch));
    }
    pub fn request_apply(&self, target: Option<(u32, u64)>) -> Result<()> {
        ensure!(
            !self.shared.stop.load(Ordering::Acquire),
            "Приложение завершается"
        );
        let loaded = self.store.load()?;
        let targets: Vec<_> = self
            .snapshot()
            .processes
            .into_iter()
            .filter(|r| {
                r.eligible
                    && target.is_none_or(|target| {
                        (
                            r.pid,
                            r.created_time
                                .as_deref()
                                .and_then(|v| v.parse().ok())
                                .unwrap_or(0),
                        ) == target
                    })
            })
            .filter_map(|r| {
                Some(Identity {
                    pid: r.pid,
                    created_time: r.created_time?.parse().ok()?,
                    exe: r.exe?.into(),
                })
            })
            .collect();
        ensure!(
            !targets.is_empty(),
            "Подходящий процесс не найден; обновите список"
        );
        ensure!(
            self.shared
                .busy
                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                .is_ok(),
            "Дождитесь завершения текущего применения"
        );
        if self
            .tx
            .send(Request::Apply(targets, loaded.settings))
            .is_err()
        {
            self.shared.busy.store(false, Ordering::Release);
            anyhow::bail!("Наблюдатель завершён");
        }
        Ok(())
    }
    pub fn quit(&self) {
        self.shared.stop.store(true, Ordering::Release);
        let _ = self.tx.send(Request::Quit);
    }
    pub fn finished(&self) -> bool {
        self.shared.finished.load(Ordering::Acquire)
    }
    fn save_geometry(&self, patch: WindowPatch) {
        if let Err(error) = self.store.update(SettingsPatch {
            window: Some(patch),
            ..Default::default()
        }) {
            self.edit(|s| s.error = Some(format!("Размер окна не сохранён: {error:#}")));
        }
    }
    fn apply(
        &self,
        identity: &Identity,
        settings: &Settings,
        results: &mut BTreeMap<(u32, u64), ProcessRow>,
    ) {
        self.edit(|s| {
            if let Some(row) = s.processes.iter_mut().find(|r| {
                r.pid == identity.pid
                    && r.created_time
                        .as_deref()
                        .and_then(|v| v.parse::<u64>().ok())
                        == Some(identity.created_time)
            }) {
                row.status = "applying".into();
                row.detail = None;
            }
        });
        let result = manager::attach_to_with_icons(
            identity,
            &settings.selected_theme,
            settings.custom_icons,
        );
        let font_result = fonts::apply_to(identity, &settings.editor_font);
        let detail = [result.as_ref().err(), font_result.as_ref().err()]
            .into_iter()
            .flatten()
            .map(|e| format!("{e:#}"))
            .collect::<Vec<_>>()
            .join("\n");
        let row = ProcessRow {
            pid: identity.pid,
            created_time: Some(identity.created_time.to_string()),
            title: "Конфигуратор".into(),
            exe: Some(identity.exe.display().to_string()),
            command_line: None,
            eligible: true,
            status: if result.is_ok() && font_result.is_ok() {
                "applied".into()
            } else {
                "error".into()
            },
            theme: result.as_ref().ok().map(|r| r.theme.clone()),
            icons: result.as_ref().ok().map(|r| r.icons.clone()),
            icon_warning: result.as_ref().ok().and_then(|r| r.icon_warning.clone()),
            font: font_result.as_ref().ok().map(|r| r.label.clone()),
            font_warning: font_result.as_ref().ok().and_then(|r| r.warning.clone()),
            detail: (!detail.is_empty()).then_some(detail),
        };
        results.insert((identity.pid, identity.created_time), row.clone());
        self.edit(|s| {
            if let Some(existing) = s.processes.iter_mut().find(|r| {
                r.pid == identity.pid
                    && r.created_time
                        .as_deref()
                        .and_then(|v| v.parse::<u64>().ok())
                        == Some(identity.created_time)
            }) {
                let command_line = existing.command_line.clone();
                *existing = row;
                existing.command_line = command_line;
            }
        });
    }
}

fn merge_geometry(previous: WindowPatch, next: WindowPatch) -> WindowPatch {
    WindowPatch {
        width: next.width.or(previous.width),
        height: next.height.or(previous.height),
        maximized: next.maximized.or(previous.maximized),
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    // No worker is started: tests can inspect dispatch without touching live 1C.
    fn idle_service() -> (tempfile::TempDir, Service, mpsc::Receiver<Request>) {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::at(temp.path()).unwrap();
        let loaded = store
            .update(SettingsPatch {
                auto_apply: Some(false),
                ..Default::default()
            })
            .unwrap();
        let (tx, rx) = mpsc::channel();
        let service = Service {
            store,
            tx,
            shared: Arc::new(Shared {
                snapshot: Mutex::new(Snapshot {
                    settings: Some(loaded),
                    processes: vec![],
                    error: None,
                    busy: false,
                    stopping: false,
                    version: "test",
                }),
                busy: AtomicBool::new(false),
                stop: AtomicBool::new(false),
                finished: AtomicBool::new(false),
            }),
        };
        (temp, service, rx)
    }
    #[test]
    fn appearance_changes_with_auto_off_only_save_and_refresh_preview() {
        let (_temp, service, rx) = idle_service();
        let identities = [Identity {
            pid: 123,
            created_time: 456,
            exe: "C:/Platform/1cv8.exe".into(),
        }];
        let mut controller = Controller::default();
        for patch in [
            SettingsPatch {
                editor_font: Some(v8_chroma_core::fonts::FontSettings {
                    enabled: true,
                    family: "Consolas".into(),
                    points10: 145,
                }),
                ..Default::default()
            },
            SettingsPatch {
                selected_theme: Some("monokai".into()),
                ..Default::default()
            },
            SettingsPatch {
                custom_icons: Some(true),
                ..Default::default()
            },
        ] {
            let loaded = service.set_appearance(patch).unwrap();
            assert!(!loaded.settings.auto_apply);
            assert_eq!(
                service.snapshot().settings.unwrap().settings,
                loaded.settings
            );
            assert_eq!(service.store.load().unwrap().settings, loaded.settings);
            assert!(!service.snapshot().busy);
            assert!(matches!(rx.try_recv().unwrap(), Request::Wake));
            assert!(rx.try_recv().is_err());
            assert!(controller.plan(&identities, &loaded.settings).is_empty());
        }
        let settings = service.store.load().unwrap().settings;
        assert_eq!(settings.selected_theme, "monokai");
        assert!(settings.custom_icons);
        // Enabling automatic application uses the latest saved appearance once.
        let loaded = service
            .save(SettingsPatch {
                auto_apply: Some(true),
                ..Default::default()
            })
            .unwrap();
        assert!(matches!(rx.try_recv().unwrap(), Request::Wake));
        assert_eq!(controller.plan(&identities, &loaded.settings), identities);
        assert!(controller.plan(&identities, &loaded.settings).is_empty());
    }
    #[test]
    fn appearance_changes_with_auto_on_apply_once_through_watcher() {
        let (_temp, service, rx) = idle_service();
        service
            .save(SettingsPatch {
                auto_apply: Some(true),
                ..Default::default()
            })
            .unwrap();
        assert!(matches!(rx.try_recv().unwrap(), Request::Wake));
        let identities = [Identity {
            pid: 123,
            created_time: 456,
            exe: "C:/Platform/1cv8.exe".into(),
        }];
        let mut controller = Controller::default();
        controller.plan(&identities, &service.store.load().unwrap().settings);
        for patch in [
            SettingsPatch {
                selected_theme: Some("monokai".into()),
                ..Default::default()
            },
            SettingsPatch {
                custom_icons: Some(true),
                ..Default::default()
            },
        ] {
            let loaded = service.set_appearance(patch).unwrap();
            assert!(matches!(rx.try_recv().unwrap(), Request::Wake));
            assert!(rx.try_recv().is_err());
            assert!(loaded.settings.auto_apply);
            assert_eq!(controller.plan(&identities, &loaded.settings), identities);
            assert!(controller.plan(&identities, &loaded.settings).is_empty());
        }
    }
    #[test]
    fn rejected_appearance_changes_do_not_save_or_dispatch() {
        let (_temp, service, rx) = idle_service();
        let before = std::fs::read(service.store.path()).unwrap();
        assert!(
            service
                .set_appearance(SettingsPatch {
                    auto_apply: Some(true),
                    ..Default::default()
                })
                .is_err()
        );
        service.shared.busy.store(true, Ordering::Release);
        assert!(
            service
                .set_appearance(SettingsPatch {
                    custom_icons: Some(true),
                    ..Default::default()
                })
                .is_err()
        );
        service.shared.busy.store(false, Ordering::Release);
        assert!(
            service
                .set_appearance(SettingsPatch {
                    selected_theme: Some("unknown".into()),
                    ..Default::default()
                })
                .is_err()
        );
        assert!(!service.snapshot().busy);
        service.shared.stop.store(true, Ordering::Release);
        assert!(
            service
                .set_appearance(SettingsPatch {
                    custom_icons: Some(true),
                    ..Default::default()
                })
                .is_err()
        );
        assert!(!service.snapshot().busy);
        assert_eq!(std::fs::read(service.store.path()).unwrap(), before);
        assert!(rx.try_recv().is_err());
    }
    #[test]
    fn explicit_apply_still_queues_with_auto_off() {
        let (_temp, service, rx) = idle_service();
        service.edit(|s| {
            s.processes = vec![ProcessRow {
                pid: 123,
                created_time: Some("456".into()),
                title: "Конфигуратор".into(),
                exe: Some("C:/Platform/1cv8.exe".into()),
                command_line: None,
                eligible: true,
                status: "idle".into(),
                theme: None,
                detail: None,
                icons: None,
                icon_warning: None,
                font: None,
                font_warning: None,
            }];
        });
        service.request_apply(Some((123, 456))).unwrap();
        let Request::Apply(targets, settings) = rx.try_recv().unwrap() else {
            panic!("Expected explicit manual apply");
        };
        assert!(!settings.auto_apply);
        assert_eq!(targets.len(), 1);
        assert_eq!(targets[0].pid, 123);
        assert_eq!(targets[0].created_time, 456);
        assert!(service.snapshot().busy);
        assert!(rx.try_recv().is_err());
    }
    #[test]
    fn javascript_identity_uses_lossless_decimal_string() {
        let time = 134376008137951638u64;
        let row = ProcessRow {
            pid: 123,
            created_time: Some(time.to_string()),
            title: String::new(),
            exe: None,
            command_line: Some(
                r#""C:\Program Files\1cv8\bin\1cv8.exe" DESIGNER /F "D:\Базы\Тест UI""#.into(),
            ),
            eligible: true,
            status: "idle".into(),
            theme: None,
            icons: None,
            icon_warning: None,
            font: None,
            font_warning: None,
            detail: None,
        };
        let json = serde_json::to_value(row).unwrap();
        assert_eq!(
            json["created_time"]
                .as_str()
                .unwrap()
                .parse::<u64>()
                .unwrap(),
            time
        );
        assert!(!json["created_time"].is_number());
        assert_eq!(
            json["command_line"],
            r#""C:\Program Files\1cv8\bin\1cv8.exe" DESIGNER /F "D:\Базы\Тест UI""#
        );
    }
    #[test]
    fn maximize_preserves_pending_normal_size() {
        let patch = merge_geometry(
            WindowPatch {
                width: Some(1200),
                height: Some(800),
                maximized: Some(false),
            },
            WindowPatch {
                maximized: Some(true),
                ..Default::default()
            },
        );
        assert_eq!(patch.width, Some(1200));
        assert_eq!(patch.height, Some(800));
        assert_eq!(patch.maximized, Some(true));
    }
    #[test]
    fn worker_saves_geometry_and_finishes_cleanly() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::at(temp.path()).unwrap();
        store
            .update(SettingsPatch {
                auto_apply: Some(false),
                ..Default::default()
            })
            .unwrap();
        let (tx, rx) = mpsc::channel();
        let service = Service::start(store.clone(), move || {
            let _ = tx.send(());
        });
        service
            .save(SettingsPatch {
                selected_theme: Some("night-owl".into()),
                ..Default::default()
            })
            .unwrap();
        service.geometry(WindowPatch {
            width: Some(1200),
            height: Some(800),
            maximized: Some(false),
        });
        service.geometry(WindowPatch {
            maximized: Some(true),
            ..Default::default()
        });
        let deadline = Instant::now() + Duration::from_secs(5);
        while !store.load().unwrap().settings.window.maximized {
            assert!(Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(50));
        }
        service.quit();
        rx.recv_timeout(Duration::from_secs(5)).unwrap();
        assert!(service.finished());
        let settings = store.load().unwrap().settings;
        assert_eq!(settings.window.width, 1200);
        assert_eq!(settings.window.height, 800);
        assert_eq!(settings.selected_theme, "night-owl");
    }
    #[test]
    fn incompatible_settings_remain_available_for_diagnosis() {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::at(temp.path()).unwrap();
        let future = br#"{"schema_version":999}"#;
        std::fs::write(store.path(), future).unwrap();
        let (tx, rx) = mpsc::channel();
        let service = Service::start(store.clone(), move || {
            let _ = tx.send(());
        });
        assert!(service.snapshot().settings.is_none());
        assert!(service.snapshot().error.is_some());
        service.quit();
        rx.recv_timeout(Duration::from_secs(5)).unwrap();
        assert_eq!(std::fs::read(store.path()).unwrap(), future);
    }
}
