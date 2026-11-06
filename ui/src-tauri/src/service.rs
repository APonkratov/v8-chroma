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
    fonts::{FontApplied, FontNotReady},
    manager::{self, DiscoveryFailures, ThemeNotReady},
    settings::{Loaded, Settings, SettingsPatch, Store, WindowPatch},
    watch::{ApplyTarget, Controller, FontTarget},
    win::{Identity, Process},
};

#[derive(Clone, Default)]
struct FontIssue {
    pending: bool,
    error: Option<String>,
}

#[derive(Clone, Default, Serialize)]
pub struct ProcessRow {
    pub pid: u32,
    pub created_time: Option<String>,
    pub title: String,
    pub exe: Option<String>,
    pub command_line: Option<String>,
    pub eligible: bool,
    pub status: String,
    pub theme: Option<String>,
    pub theme_warning: Option<String>,
    pub detail: Option<String>,
    pub icons: Option<String>,
    pub icon_warning: Option<String>,
    pub font: Option<String>,
    pub font_warning: Option<String>,
    pub ui_font: Option<String>,
    pub ui_font_warning: Option<String>,
    #[serde(skip)]
    theme_error: Option<String>,
    #[serde(skip)]
    theme_pending: bool,
    #[serde(skip)]
    editor_issue: FontIssue,
    #[serde(skip)]
    interface_issue: FontIssue,
}

impl ProcessRow {
    fn matches(&self, identity: &Identity) -> bool {
        self.pid == identity.pid
            && self
                .created_time
                .as_deref()
                .and_then(|value| value.parse().ok())
                == Some(identity.created_time)
    }

    fn record_theme(&mut self, result: Result<manager::Applied>) {
        self.theme_error = None;
        self.theme_pending = false;
        match result {
            Ok(applied) => {
                self.theme = Some(applied.theme);
                self.icons = Some(applied.icons);
                self.icon_warning = applied.icon_warning;
            }
            Err(error) if error.is::<ThemeNotReady>() => self.theme_pending = true,
            Err(error) => self.theme_error = Some(format!("{error:#}")),
        }
    }

    fn record_font(&mut self, target: FontTarget, result: Result<FontApplied>, auto_apply: bool) {
        let (label, warning, issue) = match target {
            FontTarget::Editor => (
                &mut self.font,
                &mut self.font_warning,
                &mut self.editor_issue,
            ),
            FontTarget::Interface => (
                &mut self.ui_font,
                &mut self.ui_font_warning,
                &mut self.interface_issue,
            ),
        };
        *issue = FontIssue::default();
        match result {
            Ok(font) => {
                *label = Some(font.label);
                *warning = font.warning;
            }
            Err(error) if error.is::<FontNotReady>() => {
                issue.pending = true;
                *warning = (!auto_apply).then(|| error.to_string());
            }
            Err(error) => {
                *label = None;
                *warning = None;
                issue.error = Some(format!("{error:#}"));
            }
        }
    }

    fn summarize(&mut self, auto_apply: bool) {
        self.theme_warning = (self.theme_pending && !auto_apply).then(|| ThemeNotReady.to_string());
        let errors = [
            &self.theme_error,
            &self.editor_issue.error,
            &self.interface_issue.error,
        ]
        .into_iter()
        .flatten()
        .map(String::as_str)
        .collect::<Vec<_>>();
        self.detail = (!errors.is_empty()).then(|| errors.join("\n"));
        self.status = if self.detail.is_some() {
            "error"
        } else if auto_apply
            && (self.theme_pending || self.editor_issue.pending || self.interface_issue.pending)
        {
            "waiting"
        } else if self.theme.is_some() {
            "applied"
        } else {
            "idle"
        }
        .into();
    }
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
    on_auto_apply: Box<dyn Fn(Option<bool>) + Send + Sync>,
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
    pub fn start(
        store: Store,
        on_auto_apply: impl Fn(Option<bool>) + Send + Sync + 'static,
        on_exit: impl FnOnce() + Send + 'static,
    ) -> Self {
        let loaded = store.ensure_initialized();
        let (settings, error) = match loaded {
            Ok(value) => (Some(value), None),
            Err(error) => (None, Some(format!("{error:#}"))),
        };
        on_auto_apply(settings.as_ref().map(|loaded| loaded.settings.auto_apply));
        let shared = Arc::new(Shared {
            on_auto_apply: Box::new(on_auto_apply),
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
            let mut discovery_failures = DiscoveryFailures::default();
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
                                        background.apply(
                                            &identity,
                                            &settings,
                                            &mut controller,
                                            &mut results,
                                        );
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
                        background.edit(|s| s.settings = Some(loaded));
                        if !settings.auto_apply {
                            controller.pause();
                        }
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
                        discovery_failures.recovered();
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
                                        ui_font: None,
                                        ui_font_warning: None,
                                        detail: if candidate.eligible {
                                            None
                                        } else {
                                            Some(candidate.reason)
                                        },
                                        ..Default::default()
                                    });
                                row.command_line = candidate.command_line;
                                if let Some(identity) = identity {
                                    if row.theme_error.is_none() {
                                        match manager::last_applied(identity) {
                                            Ok(Some(applied)) => {
                                                row.theme = Some(applied.theme);
                                                row.icons = Some(applied.icons);
                                                row.icon_warning = applied.icon_warning;
                                            }
                                            Err(error) => {
                                                row.theme_error = Some(format!("{error:#}"));
                                            }
                                            _ => {}
                                        }
                                    }
                                    let auto_apply = settings
                                        .as_ref()
                                        .is_some_and(|settings| settings.auto_apply);
                                    for target in [FontTarget::Editor, FontTarget::Interface] {
                                        let issue = match target {
                                            FontTarget::Editor => &row.editor_issue,
                                            FontTarget::Interface => &row.interface_issue,
                                        };
                                        if issue.pending {
                                            if !auto_apply {
                                                row.record_font(
                                                    target,
                                                    Err(FontNotReady.into()),
                                                    false,
                                                );
                                            }
                                        } else if issue.error.is_none() {
                                            match target.last_applied(identity) {
                                                Ok(Some(font)) => {
                                                    row.record_font(target, Ok(font), auto_apply)
                                                }
                                                Err(error) => {
                                                    row.record_font(target, Err(error), auto_apply)
                                                }
                                                Ok(None) => {}
                                            }
                                        }
                                    }
                                    row.summarize(auto_apply);
                                }
                                row
                            })
                            .collect();
                        background.edit(|s| {
                            s.processes = rows;
                            if settings.is_some() {
                                s.error = None;
                            }
                        });
                        if let Some(settings) = settings
                            && settings.auto_apply
                            && background
                                .shared
                                .busy
                                .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
                                .is_ok()
                        {
                            for identity in controller.plan(&identities, &settings) {
                                if background.shared.stop.load(Ordering::Acquire) {
                                    break;
                                }
                                background.apply(
                                    &identity,
                                    &settings,
                                    &mut controller,
                                    &mut results,
                                );
                            }
                            for (identity, target) in
                                controller.retries(&identities, Instant::now())
                            {
                                if background.shared.stop.load(Ordering::Acquire) {
                                    break;
                                }
                                background.retry_appearance(
                                    &identity,
                                    target,
                                    &settings,
                                    &mut controller,
                                    &mut results,
                                );
                            }
                            background.shared.busy.store(false, Ordering::Release);
                        }
                    }
                    Err(error) => {
                        if discovery_failures.should_report(&error) {
                            background.edit(|s| {
                                let message = format!("Не удалось обновить процессы: {error:#}");
                                s.error = match s.error.take().filter(|_| settings.is_none()) {
                                    Some(settings_error) => {
                                        Some(format!("{settings_error}\n{message}"))
                                    }
                                    None => Some(message),
                                };
                            });
                        } else if settings.is_some() {
                            background.edit(|s| s.error = None);
                        }
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
        let mut state = self
            .shared
            .snapshot
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let previous = state
            .settings
            .as_ref()
            .map(|loaded| loaded.settings.auto_apply);
        update(&mut state);
        let current = state
            .settings
            .as_ref()
            .map(|loaded| loaded.settings.auto_apply);
        if previous != current {
            (self.shared.on_auto_apply)(current);
        }
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
                    || patch.editor_font.is_some()
                    || patch.ui_font.is_some()),
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
    fn forget_gone(
        &self,
        identity: &Identity,
        controller: &mut Controller,
        results: &mut BTreeMap<(u32, u64), ProcessRow>,
    ) -> bool {
        if !Process::is_gone(identity) {
            return false;
        }
        controller.forget_pending(identity);
        results.remove(&(identity.pid, identity.created_time));
        self.edit(|s| s.processes.retain(|row| !row.matches(identity)));
        true
    }

    fn publish_row(
        &self,
        identity: &Identity,
        row: ProcessRow,
        results: &mut BTreeMap<(u32, u64), ProcessRow>,
    ) {
        results.insert((identity.pid, identity.created_time), row.clone());
        self.edit(|s| {
            if let Some(existing) = s.processes.iter_mut().find(|row| row.matches(identity)) {
                let command_line = existing.command_line.take();
                *existing = row;
                existing.command_line = command_line;
            }
        });
    }

    fn apply_font(
        identity: &Identity,
        target: FontTarget,
        settings: &Settings,
        controller: &mut Controller,
        row: &mut ProcessRow,
        wait: Duration,
    ) {
        let result = target.apply(identity, settings, wait);
        let pending = result
            .as_ref()
            .is_err_and(|error| error.is::<FontNotReady>());
        controller.set_pending(identity, ApplyTarget::Font(target), pending, Instant::now());
        row.record_font(target, result, settings.auto_apply);
    }

    fn apply_theme(
        identity: &Identity,
        settings: &Settings,
        controller: &mut Controller,
        row: &mut ProcessRow,
        wait: Duration,
    ) {
        row.record_theme(manager::attach_to_with_icons_wait(
            identity,
            &settings.selected_theme,
            settings.custom_icons,
            wait,
        ));
        controller.set_pending(
            identity,
            ApplyTarget::Theme,
            row.theme_pending,
            Instant::now(),
        );
    }

    fn retry_appearance(
        &self,
        identity: &Identity,
        target: ApplyTarget,
        settings: &Settings,
        controller: &mut Controller,
        results: &mut BTreeMap<(u32, u64), ProcessRow>,
    ) {
        if self.forget_gone(identity, controller, results) {
            return;
        }
        let Some(mut row) = results.get(&(identity.pid, identity.created_time)).cloned() else {
            return;
        };
        match target {
            ApplyTarget::Theme => {
                Self::apply_theme(identity, settings, controller, &mut row, Duration::ZERO)
            }
            ApplyTarget::Font(target) => Self::apply_font(
                identity,
                target,
                settings,
                controller,
                &mut row,
                Duration::ZERO,
            ),
        }
        if !self.forget_gone(identity, controller, results) {
            row.summarize(settings.auto_apply);
            self.publish_row(identity, row, results);
        }
    }

    fn apply(
        &self,
        identity: &Identity,
        settings: &Settings,
        controller: &mut Controller,
        results: &mut BTreeMap<(u32, u64), ProcessRow>,
    ) {
        if self.forget_gone(identity, controller, results) {
            return;
        }
        self.edit(|s| {
            if let Some(row) = s.processes.iter_mut().find(|row| row.matches(identity)) {
                row.status = "applying".into();
                row.detail = None;
            }
        });
        let mut row = ProcessRow {
            pid: identity.pid,
            created_time: Some(identity.created_time.to_string()),
            title: "Конфигуратор".into(),
            exe: Some(identity.exe.display().to_string()),
            eligible: true,
            ..Default::default()
        };
        Self::apply_theme(
            identity,
            settings,
            controller,
            &mut row,
            Duration::from_secs(5),
        );
        for target in [FontTarget::Editor, FontTarget::Interface] {
            if self.forget_gone(identity, controller, results) {
                return;
            }
            Self::apply_font(
                identity,
                target,
                settings,
                controller,
                &mut row,
                Duration::from_secs(5),
            );
        }
        if !self.forget_gone(identity, controller, results) {
            row.summarize(settings.auto_apply);
            self.publish_row(identity, row, results);
        }
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
                on_auto_apply: Box::new(|_| {}),
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
    fn auto_apply_listener_tracks_saved_reset_and_unavailable_settings() {
        let (_temp, mut service, _requests) = idle_service();
        let (tx, rx) = mpsc::channel();
        Arc::get_mut(&mut service.shared).unwrap().on_auto_apply = Box::new(move |value| {
            tx.send(value).unwrap();
        });
        for enabled in [true, false] {
            service
                .save(SettingsPatch {
                    auto_apply: Some(enabled),
                    ..Default::default()
                })
                .unwrap();
            assert_eq!(rx.try_recv().unwrap(), Some(enabled));
        }
        service.reset().unwrap();
        assert_eq!(rx.try_recv().unwrap(), Some(true));

        // Unrelated changes and failed saves must not flip the menu indicator.
        service
            .save(SettingsPatch {
                selected_theme: Some("night-owl".into()),
                ..Default::default()
            })
            .unwrap();
        assert!(
            service
                .save(SettingsPatch {
                    auto_apply: Some(false),
                    selected_theme: Some("unknown".into()),
                    ..Default::default()
                })
                .is_err()
        );
        assert!(rx.try_recv().is_err());

        service.edit(|s| s.settings = None);
        assert_eq!(rx.try_recv().unwrap(), None);
        let loaded = service.store.load().unwrap();
        service.edit(|s| s.settings = Some(loaded));
        assert_eq!(rx.try_recv().unwrap(), Some(true));
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
                ui_font: Some(v8_chroma_core::ui_fonts::UiFontSettings {
                    enabled: true,
                    family: "Segoe UI".into(),
                }),
                ..Default::default()
            },
            SettingsPatch {
                ui_font: Some(v8_chroma_core::ui_fonts::UiFontSettings {
                    enabled: false,
                    family: "Segoe UI".into(),
                }),
                ..Default::default()
            },
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
                ui_font: Some(v8_chroma_core::ui_fonts::UiFontSettings {
                    enabled: true,
                    family: "Segoe UI".into(),
                }),
                ..Default::default()
            },
            SettingsPatch {
                ui_font: Some(v8_chroma_core::ui_fonts::UiFontSettings {
                    enabled: false,
                    family: "Segoe UI".into(),
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
                ui_font: None,
                ui_font_warning: None,
                ..Default::default()
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
            ui_font: None,
            ui_font_warning: None,
            detail: None,
            ..Default::default()
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
    fn waiting_and_successful_font_retries_preserve_other_errors() {
        let mut row = ProcessRow {
            theme: Some("monokai".into()),
            ..Default::default()
        };
        row.record_font(FontTarget::Editor, Err(FontNotReady.into()), true);
        row.summarize(true);
        assert_eq!(row.status, "waiting");
        assert!(row.detail.is_none());

        row.record_font(
            FontTarget::Interface,
            Err(anyhow::anyhow!("Win32=1460")),
            true,
        );
        row.record_font(
            FontTarget::Editor,
            Ok(FontApplied {
                label: "Consolas".into(),
                warning: None,
            }),
            true,
        );
        row.summarize(true);
        assert_eq!(row.font.as_deref(), Some("Consolas"));
        assert_eq!(row.status, "error");
        assert_eq!(row.detail.as_deref(), Some("Win32=1460"));
    }
    #[test]
    fn theme_preflight_waits_and_does_not_hide_mutation_errors() {
        let mut row = ProcessRow {
            theme: Some("dracula".into()),
            ..Default::default()
        };
        row.record_theme(Err(ThemeNotReady.into()));
        row.summarize(true);
        assert_eq!(row.status, "waiting");
        assert_eq!(row.theme.as_deref(), Some("dracula"));
        assert!(row.theme_warning.is_none());
        assert!(row.detail.is_none());

        row.summarize(false);
        assert!(row.theme_warning.is_some());
        assert!(row.detail.is_none());

        row.record_theme(Err(anyhow::anyhow!("ThemeApply: Win32=1460")));
        row.summarize(true);
        assert_eq!(row.status, "error");
        assert!(!row.theme_pending);
        assert_eq!(row.detail.as_deref(), Some("ThemeApply: Win32=1460"));
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
        let service = Service::start(
            store.clone(),
            |_| {},
            move || {
                let _ = tx.send(());
            },
        );
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
        let service = Service::start(
            store.clone(),
            |_| {},
            move || {
                let _ = tx.send(());
            },
        );
        assert!(service.snapshot().settings.is_none());
        assert!(service.snapshot().error.is_some());
        service.quit();
        rx.recv_timeout(Duration::from_secs(5)).unwrap();
        assert_eq!(std::fs::read(store.path()).unwrap(), future);
    }
}
