//! Persisted editor font choice. Disabled by default; installed fonts stay local.
use anyhow::{Result, ensure};
use serde::{Deserialize, Serialize};

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct FontSettings {
    pub enabled: bool,
    pub family: String,
    /// Tenths of a point; half-point steps avoid floating-point persistence.
    pub points10: u32,
}
impl Default for FontSettings {
    fn default() -> Self {
        Self {
            enabled: false,
            family: "JetBrains Mono Medium".into(),
            points10: 130,
        }
    }
}
impl FontSettings {
    pub fn validate(&self) -> Result<()> {
        validate_family(&self.family)?;
        ensure!(
            (80..=320).contains(&self.points10) && self.points10.is_multiple_of(5),
            "Размер шрифта: от 8 до 32 pt с шагом 0,5"
        );
        Ok(())
    }
    pub fn appearance(&self) -> Option<(String, u32)> {
        self.enabled.then(|| (self.family.clone(), self.points10))
    }
}
pub(crate) fn validate_family(family: &str) -> Result<()> {
    ensure!(
        !family.trim().is_empty()
            && family.encode_utf16().count() < 32
            && !family.chars().any(char::is_control),
        "Некорректное имя шрифта (до 31 символа UTF-16)"
    );
    Ok(())
}

#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
mod platform;
#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
pub use platform::*;

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validates_sizes_and_utf16_capacity() {
        let mut font = FontSettings::default();
        for size in [80, 85, 130, 315, 320] {
            font.points10 = size;
            assert!(font.validate().is_ok());
        }
        for size in [0, 79, 131, 321, u32::MAX] {
            font.points10 = size;
            assert!(font.validate().is_err());
        }
        font.points10 = 130;
        for family in ["", "x\0y", "line\nfont", "😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀"]
        {
            font.family = family.into();
            assert!(font.validate().is_err());
        }
    }
}
