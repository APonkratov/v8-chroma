use crate::{
    storage::{Lock, atomic_write},
    theme,
};
use anyhow::{Context, Result, bail, ensure};
use directories::ProjectDirs;
use serde::{Deserialize, Serialize};
use std::{
    fs,
    io::Read,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

const LIMIT: u64 = 65536;

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WindowSettings {
    pub width: u32,
    pub height: u32,
    pub maximized: bool,
}
impl Default for WindowSettings {
    fn default() -> Self {
        Self {
            width: 1000,
            height: 700,
            maximized: false,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Settings {
    pub schema_version: u32,
    pub selected_theme: String,
    pub auto_apply: bool,
    pub custom_icons: bool,
    pub editor_font: crate::fonts::FontSettings,
    pub window: WindowSettings,
}
impl Default for Settings {
    fn default() -> Self {
        Self {
            schema_version: 3,
            selected_theme: "dracula".into(),
            auto_apply: true,
            custom_icons: false,
            editor_font: Default::default(),
            window: WindowSettings::default(),
        }
    }
}
impl Settings {
    pub fn validate(&self) -> Result<()> {
        ensure!(
            self.schema_version == 3,
            "Неизвестная схема настроек: {}",
            self.schema_version
        );
        ensure!(
            self.selected_theme == "diagnostic"
                || self.selected_theme == "standard"
                || theme::IDS.contains(&self.selected_theme.as_str()),
            "Неизвестный пресет: {}",
            self.selected_theme
        );
        ensure!(
            (640..=3840).contains(&self.window.width) && (480..=2160).contains(&self.window.height),
            "Размер окна вне диапазона 640..3840 x 480..2160"
        );
        self.editor_font.validate()
    }
}

#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct WindowPatch {
    pub width: Option<u32>,
    pub height: Option<u32>,
    pub maximized: Option<bool>,
}
#[derive(Debug, Default, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SettingsPatch {
    pub selected_theme: Option<String>,
    pub auto_apply: Option<bool>,
    pub custom_icons: Option<bool>,
    pub editor_font: Option<crate::fonts::FontSettings>,
    pub window: Option<WindowPatch>,
}
impl SettingsPatch {
    fn apply(self, mut settings: Settings) -> Result<Settings> {
        if let Some(value) = self.selected_theme {
            settings.selected_theme = value;
        }
        if let Some(value) = self.auto_apply {
            settings.auto_apply = value;
        }
        if let Some(value) = self.custom_icons {
            settings.custom_icons = value;
        }
        if let Some(value) = self.editor_font {
            settings.editor_font = value;
        }
        if let Some(window) = self.window {
            if let Some(value) = window.width {
                settings.window.width = value;
            }
            if let Some(value) = window.height {
                settings.window.height = value;
            }
            if let Some(value) = window.maximized {
                settings.window.maximized = value;
            }
        }
        settings.validate()?;
        Ok(settings)
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum Source {
    Primary,
    Backup,
    Defaults,
}
#[derive(Debug, Clone, Serialize)]
pub struct Loaded {
    pub path: PathBuf,
    pub source: Source,
    pub settings: Settings,
    pub warning: Option<String>,
}

#[derive(Debug)]
enum Document {
    Missing,
    Valid { settings: Settings, bytes: Vec<u8> },
    Corrupt(String),
}
#[derive(Debug)]
struct UnsupportedSchema(u32);
impl std::fmt::Display for UnsupportedSchema {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(
            f,
            "Версия настроек {} не поддерживается; файл оставлен без изменений",
            self.0
        )
    }
}
impl std::error::Error for UnsupportedSchema {}

fn parse(bytes: &[u8]) -> Result<Settings> {
    let bytes = bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes);
    #[derive(Deserialize)]
    struct Header {
        schema_version: u32,
    }
    let header: Header = serde_json::from_slice(bytes)?;
    let settings: Settings = match header.schema_version {
        1 => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Legacy {
                schema_version: u32,
                selected_theme: String,
                auto_apply: bool,
                window: WindowSettings,
            }
            let old: Legacy = serde_json::from_slice(bytes)?;
            ensure!(old.schema_version == 1, "Некорректная схема миграции");
            Settings {
                schema_version: 3,
                selected_theme: if old.selected_theme == "diagnostic" {
                    "standard".into()
                } else {
                    old.selected_theme
                },
                auto_apply: old.auto_apply,
                window: old.window,
                custom_icons: false,
                editor_font: Default::default(),
            }
        }
        2 => {
            #[derive(Deserialize)]
            #[serde(deny_unknown_fields)]
            struct Legacy {
                schema_version: u32,
                selected_theme: String,
                auto_apply: bool,
                custom_icons: bool,
                window: WindowSettings,
            }
            let old: Legacy = serde_json::from_slice(bytes)?;
            ensure!(old.schema_version == 2, "Некорректная схема миграции");
            Settings {
                schema_version: 3,
                selected_theme: old.selected_theme,
                auto_apply: old.auto_apply,
                custom_icons: old.custom_icons,
                window: old.window,
                editor_font: Default::default(),
            }
        }
        3 => serde_json::from_slice(bytes)?,
        version => return Err(UnsupportedSchema(version).into()),
    };
    settings.validate()?;
    Ok(settings)
}
fn read(path: &Path) -> Result<Document> {
    let file = match fs::File::open(path) {
        Ok(file) => file,
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => return Ok(Document::Missing),
        Err(error) => {
            return Err(error).with_context(|| format!("Не удалось открыть {}", path.display()));
        }
    };
    let mut bytes = vec![];
    file.take(LIMIT + 1)
        .read_to_end(&mut bytes)
        .with_context(|| format!("Не удалось прочитать {}", path.display()))?;
    if bytes.len() > LIMIT as usize {
        return Ok(Document::Corrupt("Размер превышает 64 KiB".into()));
    }
    match parse(&bytes) {
        Ok(settings) => Ok(Document::Valid { settings, bytes }),
        Err(error) if error.downcast_ref::<UnsupportedSchema>().is_some() => Err(error),
        Err(error) => Ok(Document::Corrupt(format!("{error:#}"))),
    }
}

#[derive(Clone)]
pub struct Store {
    directory: PathBuf,
}
impl Store {
    pub fn system() -> Result<Self> {
        let dirs = ProjectDirs::from("", "", crate::APP_NAME)
            .context("Не найден пользовательский каталог настроек")?;
        Self::at(dirs.config_dir())
    }
    /// Explicit profiles also make isolated integration tests independent of real user preferences.
    pub fn at(directory: impl AsRef<Path>) -> Result<Self> {
        let directory = directory.as_ref();
        Ok(Self {
            directory: if directory.is_absolute() {
                directory.to_owned()
            } else {
                std::env::current_dir()?.join(directory)
            },
        })
    }
    pub fn path(&self) -> PathBuf {
        self.directory.join("settings.json")
    }
    fn backup(&self) -> PathBuf {
        self.directory.join("settings.json.bak")
    }
    fn lock(&self) -> Result<Lock> {
        fs::create_dir_all(&self.directory)?;
        Lock::acquire(&self.directory.join("settings.lock"))
    }
    fn loaded(&self, primary: &Document, backup: &Document) -> Result<Loaded> {
        let (settings, source, warning) = match (primary, backup) {
            (Document::Valid { settings, .. }, backup) => (
                settings.clone(),
                Source::Primary,
                if let Document::Corrupt(reason) = backup {
                    Some(format!(
                        "Резервная копия повреждена ({reason}); используется исправный основной файл"
                    ))
                } else {
                    None
                },
            ),
            (Document::Missing, Document::Missing) => (Settings::default(), Source::Defaults, None),
            (Document::Missing, Document::Valid { settings, .. }) => (
                settings.clone(),
                Source::Backup,
                Some("Основной файл отсутствует; использована исправная резервная копия".into()),
            ),
            (Document::Corrupt(reason), Document::Valid { settings, .. }) => (
                settings.clone(),
                Source::Backup,
                Some(format!(
                    "Настройки повреждены ({reason}); использована исправная резервная копия"
                )),
            ),
            _ => bail!(
                "Настройки повреждены, исправная резервная копия отсутствует. Автоматическое применение приостановлено; исправьте файл или выполните settings reset"
            ),
        };
        Ok(Loaded {
            path: self.path(),
            source,
            settings,
            warning,
        })
    }
    fn documents(&self) -> Result<(Document, Document)> {
        let primary = read(&self.path())?;
        // Refuse unknown versions in either file instead of overwriting newer data.
        let backup = read(&self.backup())?;
        // Both versions were checked before writing; preserve exact legacy bytes in .bak.
        if let Document::Valid { settings, bytes } = &primary {
            let raw: serde_json::Value =
                serde_json::from_slice(bytes.strip_prefix(&[0xef, 0xbb, 0xbf]).unwrap_or(bytes))?;
            if raw["schema_version"] == 1 || raw["schema_version"] == 2 {
                self.persist(settings, &primary, &backup)?;
                return Ok((read(&self.path())?, read(&self.backup())?));
            }
        }
        Ok((primary, backup))
    }
    pub fn load(&self) -> Result<Loaded> {
        let _lock = self.lock()?;
        let (primary, backup) = self.documents()?;
        self.loaded(&primary, &backup)
    }
    pub fn ensure_initialized(&self) -> Result<Loaded> {
        let _lock = self.lock()?;
        let (primary, backup) = self.documents()?;
        let loaded = self.loaded(&primary, &backup)?;
        if loaded.source == Source::Defaults {
            self.persist(&loaded.settings, &primary, &backup)?;
            return self.loaded(&read(&self.path())?, &Document::Missing);
        }
        Ok(loaded)
    }
    pub fn update(&self, patch: SettingsPatch) -> Result<Loaded> {
        let _lock = self.lock()?;
        let (primary, backup) = self.documents()?;
        let loaded = self.loaded(&primary, &backup)?;
        let settings = patch.apply(loaded.settings)?;
        if let Document::Valid {
            settings: current, ..
        } = &primary
            && *current == settings
        {
            return self.loaded(&primary, &backup);
        }
        self.persist(&settings, &primary, &backup)?;
        self.loaded(&read(&self.path())?, &Document::Missing)
    }
    pub fn reset(&self) -> Result<Loaded> {
        let _lock = self.lock()?;
        let (primary, backup) = self.documents()?;
        let settings = Settings::default();
        self.persist(&settings, &primary, &backup)?;
        self.loaded(&read(&self.path())?, &Document::Missing)
    }
    fn quarantine(&self, path: &Path) -> Result<()> {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)?.as_nanos();
        let target = self.directory.join(format!(
            "{}-corrupt-{stamp}-{}.json",
            path.file_name()
                .context("Нет имени файла")?
                .to_string_lossy(),
            std::process::id()
        ));
        fs::rename(path, &target).context("Не удалось сохранить повреждённый файл")
    }
    fn persist(&self, settings: &Settings, primary: &Document, backup: &Document) -> Result<()> {
        settings.validate()?;
        let bytes = serde_json::to_vec_pretty(settings)?;
        let previous = match (primary, backup) {
            (Document::Valid { bytes, .. }, _) | (_, Document::Valid { bytes, .. }) => {
                bytes.as_slice()
            }
            _ => bytes.as_slice(),
        };
        if matches!(backup, Document::Corrupt(_)) {
            self.quarantine(&self.backup())?;
        }
        atomic_write(&self.backup(), previous)?;
        if matches!(primary, Document::Corrupt(_)) {
            self.quarantine(&self.path())?;
        }
        atomic_write(&self.path(), &bytes)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn profile() -> (tempfile::TempDir, Store) {
        let temp = tempfile::tempdir().unwrap();
        let store = Store::at(temp.path().join("Профиль пользователя")).unwrap();
        (temp, store)
    }
    fn preset(id: &str) -> SettingsPatch {
        SettingsPatch {
            selected_theme: Some(id.into()),
            ..Default::default()
        }
    }
    #[test]
    fn defaults_initialization_and_unicode_roundtrip() {
        let (_temp, store) = profile();
        assert_eq!(store.load().unwrap().source, Source::Defaults);
        assert!(!store.path().exists());
        store.ensure_initialized().unwrap();
        assert!(store.backup().exists());
        store.update(preset("github-light-default")).unwrap();
        assert_eq!(
            Store::at(&store.directory)
                .unwrap()
                .load()
                .unwrap()
                .settings
                .selected_theme,
            "github-light-default"
        );
    }
    #[test]
    fn updates_preserve_previous_valid_backup_and_noop() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        store.update(preset("darcula")).unwrap();
        let primary = fs::read(store.path()).unwrap();
        let backup = fs::read(store.backup()).unwrap();
        assert_eq!(parse(&backup).unwrap().selected_theme, "dracula");
        store.update(preset("darcula")).unwrap();
        assert_eq!(fs::read(store.path()).unwrap(), primary);
        assert_eq!(fs::read(store.backup()).unwrap(), backup);
    }
    #[test]
    fn recovers_backup_and_preserves_corruption_on_explicit_save() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        store.update(preset("darcula")).unwrap();
        fs::write(store.path(), b"broken").unwrap();
        let loaded = store.load().unwrap();
        assert_eq!(loaded.source, Source::Backup);
        assert!(loaded.warning.is_some());
        store.update(preset("monokai")).unwrap();
        assert_eq!(
            parse(&fs::read(store.backup()).unwrap())
                .unwrap()
                .selected_theme,
            "dracula"
        );
        assert!(fs::read_dir(&store.directory).unwrap().any(|e| {
            e.unwrap()
                .file_name()
                .to_string_lossy()
                .contains("corrupt-")
        }));
    }
    #[test]
    fn corrupt_both_requires_reset_and_preserves_invalid_files() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        fs::write(store.path(), b"primary broken").unwrap();
        fs::write(store.backup(), b"backup broken").unwrap();
        assert!(store.load().is_err());
        assert!(store.update(preset("monokai")).is_err());
        store.reset().unwrap();
        let corrupt: Vec<_> = fs::read_dir(&store.directory)
            .unwrap()
            .map(|e| e.unwrap().path())
            .filter(|p| {
                p.file_name()
                    .unwrap()
                    .to_string_lossy()
                    .contains("corrupt-")
            })
            .collect();
        assert_eq!(corrupt.len(), 2);
        assert_eq!(store.load().unwrap().settings, Settings::default());
    }
    #[test]
    fn legacy_settings_migrate_with_exact_backup_and_icons_disabled() {
        let (_temp, store) = profile();
        fs::create_dir_all(&store.directory).unwrap();
        let old=br#"{"schema_version":1,"selected_theme":"diagnostic","auto_apply":false,"window":{"width":1200,"height":800,"maximized":true}}"#;
        fs::write(store.path(), old).unwrap();
        let migrated = store.load().unwrap().settings;
        assert_eq!(migrated.schema_version, 3);
        assert_eq!(migrated.selected_theme, "standard");
        assert!(!migrated.custom_icons && !migrated.auto_apply);
        assert_eq!(migrated.window.width, 1200);
        assert_eq!(fs::read(store.backup()).unwrap(), old);
        assert_eq!(store.load().unwrap().settings, migrated);
        assert_eq!(fs::read(store.backup()).unwrap(), old);
        store
            .update(SettingsPatch {
                custom_icons: Some(true),
                ..Default::default()
            })
            .unwrap();
        assert!(store.load().unwrap().settings.custom_icons);
    }
    #[test]
    fn v2_migration_preserves_choices_and_font_is_opt_in() {
        let (_temp, store) = profile();
        fs::create_dir_all(&store.directory).unwrap();
        let old=br#"{"schema_version":2,"selected_theme":"monokai","auto_apply":false,"custom_icons":true,"window":{"width":1200,"height":800,"maximized":false}}"#;
        fs::write(store.path(), old).unwrap();
        let loaded = store.load().unwrap().settings;
        assert_eq!(loaded.schema_version, 3);
        assert_eq!(loaded.selected_theme, "monokai");
        assert!(loaded.custom_icons && !loaded.auto_apply && !loaded.editor_font.enabled);
        assert_eq!(fs::read(store.backup()).unwrap(), old);
        let font = crate::fonts::FontSettings {
            enabled: true,
            family: "Consolas".into(),
            points10: 145,
        };
        store
            .update(SettingsPatch {
                editor_font: Some(font.clone()),
                ..Default::default()
            })
            .unwrap();
        assert_eq!(store.load().unwrap().settings.editor_font, font);
        let before = fs::read(store.path()).unwrap();
        let invalid = crate::fonts::FontSettings {
            points10: 146,
            ..font
        };
        assert!(
            store
                .update(SettingsPatch {
                    editor_font: Some(invalid),
                    ..Default::default()
                })
                .is_err()
        );
        assert_eq!(fs::read(store.path()).unwrap(), before);
    }
    #[test]
    fn future_schema_is_never_overwritten() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let bytes = br#"{"schema_version":999,"future":"data"}"#;
        fs::write(store.path(), bytes).unwrap();
        assert!(store.load().is_err());
        assert!(store.update(preset("monokai")).is_err());
        assert!(store.reset().is_err());
        assert_eq!(fs::read(store.path()).unwrap(), bytes);
    }
    #[test]
    fn future_backup_is_preserved_even_with_valid_primary() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let future = br#"{"schema_version":999}"#;
        fs::write(store.backup(), future).unwrap();
        assert!(store.update(preset("monokai")).is_err());
        assert!(store.reset().is_err());
        assert_eq!(fs::read(store.backup()).unwrap(), future);
    }
    #[test]
    fn missing_primary_and_corrupt_backup_are_reported() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        fs::remove_file(store.path()).unwrap();
        let loaded = store.load().unwrap();
        assert_eq!(loaded.source, Source::Backup);
        assert!(loaded.warning.is_some());
        store.update(preset("darcula")).unwrap();
        fs::write(store.backup(), b"invalid backup").unwrap();
        assert_eq!(store.load().unwrap().source, Source::Primary);
        assert!(store.load().unwrap().warning.is_some());
        store.update(preset("monokai")).unwrap();
        assert!(store.load().unwrap().warning.is_none());
    }
    #[test]
    fn oversized_input_is_rejected_and_utf8_bom_is_supported() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let mut bytes = vec![0xef, 0xbb, 0xbf];
        bytes.extend(fs::read(store.path()).unwrap());
        assert_eq!(parse(&bytes).unwrap(), Settings::default());
        fs::write(store.path(), vec![b' '; LIMIT as usize + 1]).unwrap();
        assert_eq!(store.load().unwrap().source, Source::Backup);
    }
    #[test]
    fn strict_validation_is_atomic() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let original = fs::read(store.path()).unwrap();
        assert!(store.update(preset("unknown")).is_err());
        assert!(
            store
                .update(SettingsPatch {
                    window: Some(WindowPatch {
                        width: Some(1),
                        ..Default::default()
                    }),
                    ..Default::default()
                })
                .is_err()
        );
        assert_eq!(fs::read(store.path()).unwrap(), original);
        let valid = String::from_utf8(original).unwrap();
        assert!(
            parse(
                valid
                    .replace(
                        "\"auto_apply\": true",
                        "\"auto_apply\": true, \"auto_apply\": false"
                    )
                    .as_bytes()
            )
            .is_err()
        );
        assert!(
            parse(
                valid
                    .replace(
                        "\"schema_version\": 3",
                        "\"schema_version\": 3, \"typo\": 3"
                    )
                    .as_bytes()
            )
            .is_err()
        );
        assert!(
            parse(
                valid
                    .replace("\"auto_apply\": true", "\"auto_apply\": \"true\"")
                    .as_bytes()
            )
            .is_err()
        );
    }
    #[test]
    fn concurrent_patches_do_not_lose_independent_fields() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let a = store.clone();
        let b = store.clone();
        let t1 = std::thread::spawn(move || a.update(preset("monokai")).unwrap());
        let t2 = std::thread::spawn(move || {
            b.update(SettingsPatch {
                auto_apply: Some(false),
                ..Default::default()
            })
            .unwrap()
        });
        t1.join().unwrap();
        t2.join().unwrap();
        let loaded = store.load().unwrap();
        assert_eq!(loaded.settings.selected_theme, "monokai");
        assert!(!loaded.settings.auto_apply);
    }
    #[cfg(windows)]
    #[test]
    fn replacement_failure_keeps_primary_valid() {
        let (_temp, store) = profile();
        store.ensure_initialized().unwrap();
        let original = fs::read(store.path()).unwrap();
        let permissions = fs::metadata(store.path()).unwrap().permissions();
        let mut readonly = permissions.clone();
        readonly.set_readonly(true);
        fs::set_permissions(store.path(), readonly).unwrap();
        let result = store.update(preset("monokai"));
        fs::set_permissions(store.path(), permissions).unwrap();
        assert!(result.is_err());
        assert_eq!(fs::read(store.path()).unwrap(), original);
        assert_eq!(
            parse(&fs::read(store.backup()).unwrap()).unwrap(),
            Settings::default()
        );
    }
    #[test]
    fn separate_profiles_and_window_patch() {
        let (temp, store) = profile();
        let other = Store::at(temp.path().join("Другой пользователь")).unwrap();
        store.update(preset("monokai")).unwrap();
        assert_eq!(other.load().unwrap().settings, Settings::default());
        store
            .update(SettingsPatch {
                window: Some(WindowPatch {
                    width: Some(1200),
                    maximized: Some(true),
                    ..Default::default()
                }),
                ..Default::default()
            })
            .unwrap();
        let window = store.load().unwrap().settings.window;
        assert_eq!(
            window,
            WindowSettings {
                width: 1200,
                height: 700,
                maximized: true
            }
        );
    }
}
