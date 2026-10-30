/// Stable name for per-user storage and the desktop autostart entry.
pub const APP_NAME: &str = "v8-chroma";
pub mod fonts;
pub mod ui_fonts;

#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
pub mod manager;
pub mod process;
pub mod settings;
mod storage;
pub mod theme;
pub mod watch;
#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
pub mod win;
