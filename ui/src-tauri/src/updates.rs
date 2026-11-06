use crate::{
    service::Service,
    update_install,
    update_release::{self as release, Available, Release},
};
use anyhow::{Context, Result, ensure};
use serde::{Deserialize, Serialize};
use std::{
    fs,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        mpsc::{self, Sender},
    },
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

const INTERVAL: Duration = Duration::from_secs(6 * 60 * 60);
#[derive(Clone, Serialize, Deserialize)]
#[serde(default, deny_unknown_fields)]
pub struct Preferences {
    pub auto_check: bool,
    pub include_rc: bool,
    pub notified: Option<String>,
}
impl Default for Preferences {
    fn default() -> Self {
        Self {
            auto_check: true,
            include_rc: false,
            notified: None,
        }
    }
}
#[derive(Clone, Serialize)]
pub struct Snapshot {
    pub preferences: Preferences,
    pub checking: bool,
    pub installing: bool,
    pub available: Option<Available>,
    pub error: Option<String>,
    pub last_checked: Option<u64>,
    pub downloaded: u64,
    pub total: u64,
}
struct Shared {
    snapshot: Snapshot,
    release: Option<Release>,
}
#[derive(Clone)]
pub struct Updater {
    shared: Arc<Mutex<Shared>>,
    path: PathBuf,
    tx: Sender<Request>,
}
enum Request {
    Check,
    Changed,
    Install,
    NotificationShown(String),
}
impl Updater {
    pub fn start(
        path: PathBuf,
        service: Service,
        on_release: impl Fn(Option<String>, bool) + Send + 'static,
    ) -> Self {
        let preferences = (|| -> Result<Preferences> {
            match fs::read(&path) {
                Ok(bytes) => {
                    ensure!(
                        bytes.len() <= 8192,
                        "Слишком большой файл настроек обновлений"
                    );
                    Ok(serde_json::from_slice(&bytes)?)
                }
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Preferences::default()),
                Err(e) => Err(e.into()),
            }
        })();
        let error = preferences
            .as_ref()
            .err()
            .map(|e| format!("Настройки обновлений: {e:#}"));
        let (tx, rx) = mpsc::channel();
        let updater = Self {
            path,
            tx,
            shared: Arc::new(Mutex::new(Shared {
                snapshot: Snapshot {
                    // Fail closed on damaged preferences: don't initiate network requests.
                    preferences: preferences.unwrap_or(Preferences {
                        auto_check: false,
                        ..Default::default()
                    }),
                    checking: false,
                    installing: false,
                    available: None,
                    error,
                    last_checked: None,
                    downloaded: 0,
                    total: 0,
                },
                release: None,
            })),
        };
        let worker = updater.clone();
        std::thread::spawn(move || {
            let mut due = Instant::now() + Duration::from_secs(3);
            let mut cleanup_due = Instant::now() + Duration::from_secs(5);
            loop {
                let request = rx.recv_timeout(Duration::from_secs(1));
                if service.finished() || service.snapshot().stopping {
                    break;
                }
                if Instant::now() >= cleanup_due {
                    if let Ok(target) = std::env::current_exe().and_then(dunce::canonicalize) {
                        update_install::cleanup(&target);
                    }
                    cleanup_due = Instant::now() + Duration::from_secs(3600);
                }
                let check = match request {
                    Ok(Request::NotificationShown(version)) => {
                        worker.edit(|state| {
                            if state
                                .snapshot
                                .available
                                .as_ref()
                                .is_some_and(|available| available.version == version)
                            {
                                state.snapshot.preferences.notified = Some(version);
                                if let Err(e) = worker.persist(&state.snapshot.preferences) {
                                    state.snapshot.error = Some(format!(
                                        "Не удалось сохранить отметку уведомления: {e:#}"
                                    ));
                                }
                            }
                        });
                        continue;
                    }
                    Ok(Request::Install) => {
                        let result = worker.install(&service);
                        if let Err(e) = result {
                            worker.edit(|s| {
                                s.snapshot.installing = false;
                                s.snapshot.error =
                                    Some(format!("Обновление не установлено: {e:#}"));
                            });
                        }
                        continue;
                    }
                    Ok(Request::Check) => true,
                    Ok(Request::Changed) => {
                        due = Instant::now();
                        let version = worker.snapshot().available.map(|release| release.version);
                        on_release(version, false);
                        false
                    }
                    Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    Err(mpsc::RecvTimeoutError::Timeout) => {
                        worker.snapshot().preferences.auto_check && Instant::now() >= due
                    }
                };
                if !check {
                    continue;
                }
                due = Instant::now() + INTERVAL;
                worker.edit(|s| {
                    s.snapshot.checking = true;
                    s.snapshot.error = None;
                });
                let include_rc = worker.snapshot().preferences.include_rc;
                let result =
                    release::client().and_then(|client| release::discover(&client, include_rc));
                match result {
                    Ok(found) => {
                        let version = found.as_ref().map(|r| r.available().version);
                        worker.edit(|s| {
                            s.snapshot.available = found.as_ref().map(Release::available);
                            s.release = found;
                            s.snapshot.last_checked = Some(
                                SystemTime::now()
                                    .duration_since(UNIX_EPOCH)
                                    .unwrap_or_default()
                                    .as_secs(),
                            );
                            s.snapshot.checking = false;
                        });
                        let show_notice = version.as_ref().is_some_and(|version| {
                            worker.snapshot().preferences.notified.as_ref() != Some(version)
                        });
                        on_release(version, show_notice);
                    }
                    Err(e) => worker.edit(|s| {
                        s.snapshot.checking = false;
                        s.snapshot.error = Some(format!("Проверка обновлений: {e:#}"));
                    }),
                }
            }
        });
        updater
    }
    fn edit(&self, f: impl FnOnce(&mut Shared)) {
        f(&mut self.shared.lock().unwrap_or_else(|e| e.into_inner()));
    }
    pub fn snapshot(&self) -> Snapshot {
        self.shared
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .snapshot
            .clone()
    }
    fn persist(&self, preferences: &Preferences) -> Result<()> {
        let parent = self
            .path
            .parent()
            .context("Нет каталога настроек обновлений")?;
        fs::create_dir_all(parent)?;
        let mut temp = tempfile::NamedTempFile::new_in(parent)?;
        serde_json::to_writer_pretty(&mut temp, preferences)?;
        temp.as_file().sync_all()?;
        temp.persist(&self.path).map_err(|e| e.error)?;
        Ok(())
    }
    pub fn configure(&self, auto_check: bool, include_rc: bool) -> Result<()> {
        let mut state = self.shared.lock().unwrap_or_else(|e| e.into_inner());
        ensure!(
            !state.snapshot.checking && !state.snapshot.installing,
            "Дождитесь завершения операции обновления"
        );
        let prefs = Preferences {
            auto_check,
            include_rc,
            notified: state.snapshot.preferences.notified.clone(),
        };
        self.persist(&prefs)?;
        state.snapshot.preferences = prefs;
        state.snapshot.available = None;
        state.release = None;
        self.tx.send(Request::Changed)?;
        Ok(())
    }
    pub fn check(&self) -> Result<()> {
        let mut state = self.shared.lock().unwrap_or_else(|e| e.into_inner());
        ensure!(
            !state.snapshot.checking && !state.snapshot.installing,
            "Проверка или установка уже выполняется"
        );
        state.snapshot.checking = true;
        self.tx.send(Request::Check)?;
        Ok(())
    }
    pub fn notification_shown(&self, version: String) {
        let _ = self.tx.send(Request::NotificationShown(version));
    }
    pub fn request_install(&self) -> Result<()> {
        let mut state = self.shared.lock().unwrap_or_else(|e| e.into_inner());
        ensure!(
            !state.snapshot.checking && !state.snapshot.installing,
            "Проверка или установка уже выполняется"
        );
        ensure!(
            state.release.is_some(),
            "Сначала проверьте наличие обновлений"
        );
        state.snapshot.installing = true;
        state.snapshot.downloaded = 0;
        state.snapshot.total = 0;
        state.snapshot.error = None;
        self.tx.send(Request::Install)?;
        Ok(())
    }
    fn install(&self, service: &Service) -> Result<()> {
        let selected = self
            .shared
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .release
            .clone()
            .context("Релиз не выбран")?;
        let target = dunce::canonicalize(std::env::current_exe()?)?;
        let stage = update_install::stage(&target)?;
        let client = release::client()?;
        let sums = release::fetch(
            &client,
            &selected.asset(release::SUMS)?.browser_download_url,
            65536,
        )?;
        let signature = release::fetch(
            &client,
            &selected.asset(release::SIGNATURE)?.browser_download_url,
            8192,
        )?;
        let hashes = release::verify_manifest(
            release::PUBLIC_KEY,
            &selected.tag_name,
            &sums,
            std::str::from_utf8(&signature)?,
        )?;
        fs::write(stage.path().join(release::SUMS), sums)?;
        fs::write(stage.path().join(release::SIGNATURE), signature)?;
        release::download(
            &client,
            selected.asset(release::GUI)?,
            &stage.path().join("new.exe"),
            &hashes[release::GUI],
            |downloaded, total| {
                self.edit(|s| {
                    s.snapshot.downloaded = downloaded;
                    s.snapshot.total = total;
                });
            },
        )?;
        ensure!(
            !service.snapshot().stopping && !service.finished(),
            "Приложение уже завершается"
        );
        let plan = update_install::Plan {
            target: target.clone(),
            parent: std::process::id(),
            previous_hash: release::hash_file(&target)?,
            tag: selected.tag_name,
            args: update_install::restart_args(),
            cwd: std::env::current_dir()?,
        };
        update_install::launch_helper(stage.path(), &plan)?;
        // Once the helper acknowledged, it owns staging and the original backup.
        let _ = stage.keep();
        service.quit();
        Ok(())
    }
}
