use super::*;
use crate::{fonts::FontSettings, ui_fonts::UiFontSettings};
use std::collections::BTreeSet;

const SIZE: usize = 148;
struct Appearance {
    editor: FontSettings,
    ui: UiFontSettings,
}
impl Default for Appearance {
    fn default() -> Self {
        Self {
            editor: FontSettings::default(),
            ui: UiFontSettings {
                enabled: false,
                ..Default::default()
            },
        }
    }
}
fn path(config: &Path) -> PathBuf {
    let mut name = config.as_os_str().to_os_string();
    name.push(".fonts");
    name.into()
}
fn family(bytes: &[u8]) -> Result<String> {
    let units: Vec<u16> = bytes
        .chunks_exact(2)
        .map(|b| u16::from_le_bytes([b[0], b[1]]))
        .take_while(|c| *c != 0)
        .collect();
    ensure!(units.len() < 32, "Повреждено имя шрифта");
    Ok(String::from_utf16(&units)?)
}
impl Appearance {
    fn read(file: &Path) -> Result<Self> {
        let bytes = fs::read(file)?;
        ensure!(bytes.len() == SIZE, "Повреждён профиль шрифтов");
        let value = |i: usize| -> u32 { u32::from_le_bytes(bytes[i..i + 4].try_into().unwrap()) };
        ensure!(
            value(0) == SIZE as u32 && value(4) == 1 && value(8) <= 1 && value(16) <= 1,
            "Неизвестный профиль шрифтов"
        );
        let result = Self {
            editor: FontSettings {
                enabled: value(8) != 0,
                points10: value(12),
                family: family(&bytes[20..84])?,
            },
            ui: UiFontSettings {
                enabled: value(16) != 0,
                family: family(&bytes[84..148])?,
            },
        };
        result.editor.validate()?;
        result.ui.validate()?;
        Ok(result)
    }
    fn bytes(&self) -> Result<Vec<u8>> {
        self.editor.validate()?;
        self.ui.validate()?;
        let mut bytes = Vec::with_capacity(SIZE);
        for value in [
            SIZE as u32,
            1,
            u32::from(self.editor.enabled),
            self.editor.points10,
            u32::from(self.ui.enabled),
        ] {
            bytes.extend_from_slice(&value.to_le_bytes());
        }
        for name in [&self.editor.family, &self.ui.family] {
            let start = bytes.len();
            for unit in name.encode_utf16() {
                bytes.extend_from_slice(&unit.to_le_bytes());
            }
            bytes.resize(start + 64, 0);
        }
        Ok(bytes)
    }
    fn matches(&self, status: &Status, editor: bool, ui: bool) -> bool {
        let ui_match = self.ui.family == status.ui_family
            || (self.ui.family == "Segoe UI Variable Small" && status.ui_fallback);
        (!editor
            || (status.editor_enabled == self.editor.enabled
                && (!self.editor.enabled
                    || (status.editor_family == self.editor.family
                        && status.editor_points10 == self.editor.points10))))
            && (!ui || (status.ui_enabled == self.ui.enabled && (!self.ui.enabled || ui_match)))
    }
}
impl Manager {
    pub fn installed_fonts() -> Result<Vec<String>> {
        let result = Command::new("fc-list")
            .arg("--format=%{family}\n")
            .output()
            .context("Не удалось получить список шрифтов Fontconfig")?;
        ensure!(
            result.status.success(),
            "Fontconfig не вернул список шрифтов"
        );
        let mut names: BTreeSet<String> = String::from_utf8(result.stdout)?
            .lines()
            .flat_map(|line| line.split(','))
            .map(str::trim)
            .filter(|name| crate::fonts::validate_family(name).is_ok())
            .map(str::to_owned)
            .collect();
        if names.contains("Segoe UI Variable") {
            names.insert("Segoe UI Variable Small".into());
        }
        Ok(names.into_iter().collect())
    }
    pub fn apply_fonts_to(
        &self,
        expected: &Identity,
        editor: Option<&FontSettings>,
        ui: Option<&UiFontSettings>,
    ) -> Result<Status> {
        let _lock = Lock::acquire(&self.root.join("manager.lock"))?;
        let session = self.session(expected.pid)?;
        ensure!(
            session.identity == *expected,
            "Идентичность Конфигуратора изменилась"
        );
        let deadline = Instant::now() + Duration::from_secs(2);
        let mut status = read_status(&session.config)?;
        while status.fonts_available == 0 && Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(50));
            status = read_status(&session.config)?;
        }
        ensure!(
            status.fonts_available != 0,
            "Перезапустите этот Конфигуратор, чтобы загрузить движок с поддержкой шрифтов"
        );
        let file = path(&session.config);
        let mut request = if file.exists() {
            Appearance::read(&file)?
        } else {
            Appearance::default()
        };
        if let Some(editor) = editor {
            request.editor = editor.clone();
        }
        if let Some(ui) = ui {
            request.ui = ui.clone();
        }
        let bytes = request.bytes()?;
        let changed = fs::read(&file).ok().as_deref() != Some(bytes.as_slice())
            || !status.font_error.is_empty();
        let generation = status.font_generation;
        if changed {
            atomic_write(&file, &bytes)?;
        }
        let deadline = Instant::now() + Duration::from_secs(3);
        loop {
            status = read_status(&session.config)?;
            if request.matches(&status, editor.is_some(), ui.is_some()) && status.font_refresh != 1
            {
                break;
            }
            let relevant_error = (editor.is_some() && status.font_error.starts_with("editor-"))
                || (ui.is_some() && status.font_error.starts_with("interface-"))
                || status.font_error == "invalid-font-config";
            if ((!changed || status.font_generation != generation) && relevant_error)
                || Instant::now() >= deadline
            {
                bail!(
                    "Не удалось применить шрифт: {}",
                    if status.font_error.is_empty() {
                        "движок не подтвердил настройки"
                    } else {
                        &status.font_error
                    }
                );
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        ensure!(
            identity(expected.pid)? == *expected,
            "Процесс изменился во время применения шрифтов"
        );
        Ok(status)
    }
}
