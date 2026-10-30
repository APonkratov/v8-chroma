//! Interface typography, independent of the editor's font and point size.
use anyhow::Result;
use serde::{Deserialize, Serialize};

pub const DEFAULT_FAMILY: &str = "Segoe UI Variable Small";
pub const FALLBACK_FAMILY: &str = "Segoe UI";

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct UiFontSettings {
    pub enabled: bool,
    pub family: String,
}
impl Default for UiFontSettings {
    fn default() -> Self {
        Self {
            enabled: true,
            family: DEFAULT_FAMILY.into(),
        }
    }
}
impl UiFontSettings {
    pub fn validate(&self) -> Result<()> {
        crate::fonts::validate_family(&self.family)
    }
    pub fn appearance(&self) -> Option<String> {
        self.enabled.then(|| self.family.clone())
    }
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
