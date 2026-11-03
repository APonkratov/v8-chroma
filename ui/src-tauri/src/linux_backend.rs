//! Linux adaptation of the shared desktop watcher and native appearance engine.
use anyhow::{Result, ensure};
use v8_chroma_core::{linux::Manager, process::Identity};

pub mod manager {
    use super::*;
    pub struct Candidate {
        pub pid: u32,
        pub identity: Option<Identity>,
        pub command_line: Option<String>,
        pub eligible: bool,
        pub reason: String,
    }
    pub struct Applied {
        pub theme: String,
        pub icons: String,
        pub icon_warning: Option<String>,
        pub pending: bool,
    }
    pub fn discover() -> Result<Vec<Candidate>> {
        Ok(Manager::new(None)?
            .discover()?
            .into_iter()
            .map(|candidate| Candidate {
                pid: candidate.pid,
                identity: candidate.identity,
                command_line: None,
                eligible: candidate.eligible,
                reason: candidate.reason,
            })
            .collect())
    }
    pub fn last_applied(identity: &Identity) -> Result<Option<Applied>> {
        let manager = Manager::new(None)?;
        // An ordinary Designer has no session until the watcher attaches it.
        if !manager.has_session(identity.pid) {
            return Ok(None);
        }
        let status = manager.status_to(identity)?;
        ensure!(status.error.is_empty(), "Ошибка движка: {}", status.error);
        let pending = manager.requested_theme(identity.pid)? != status.theme
            || manager.requested_icon_mode(identity.pid)? != status.icon_mode
            || status.hook_mask == 0;
        let (icons, icon_warning) = icon_result(&status);
        Ok(Some(Applied {
            theme: status.theme,
            icons,
            icon_warning,
            pending,
        }))
    }
    pub fn attach_to_with_icons(
        identity: &Identity,
        theme: &str,
        custom_icons: bool,
    ) -> Result<Applied> {
        let executable = std::env::current_exe()?;
        let result = Manager::new(None)?.attach_to_with_icons(
            identity,
            theme,
            custom_icons,
            &executable.with_file_name("libv8-chroma-linux.so"),
            &executable.with_file_name("v8-chroma-injector"),
        )?;
        let (icons, mut icon_warning) = result
            .status
            .as_ref()
            .map(icon_result)
            .unwrap_or(("stock".into(), None));
        if custom_icons
            && result
                .status
                .as_ref()
                .is_some_and(|status| !status.icons_available)
        {
            icon_warning = Some(
                "Для новых значков перезапустите этот Конфигуратор после обновления движка".into(),
            );
        }
        Ok(Applied {
            theme: result.requested_theme,
            icons,
            icon_warning,
            pending: !result.confirmed,
        })
    }
    fn icon_result(status: &v8_chroma_core::linux::Status) -> (String, Option<String>) {
        if status.icon_mode == 0 {
            return ("stock".into(), None);
        }
        if !status.icons_available {
            return ("unavailable".into(),Some("Этот движок не поддерживает значки; перезапустите Конфигуратор после обновления".into()));
        }
        (
            (if status.icon_mode == 1 {
                "light"
            } else {
                "dark"
            })
            .into(),
            None,
        )
    }
}
pub struct NativeFont {
    pub label: String,
    pub warning: Option<String>,
}
pub mod fonts {
    use super::*;
    pub fn installed() -> Result<Vec<String>> {
        Manager::installed_fonts()
    }
    pub fn last_applied(identity: &Identity) -> Result<Option<NativeFont>> {
        let manager = Manager::new(None)?;
        if !manager.has_session(identity.pid) {
            return Ok(None);
        }
        let status = manager.status_to(identity)?;
        Ok((status.fonts_available != 0 && status.font_generation != 0)
            .then(|| describe(&status, false)))
    }
    pub fn apply_to(
        identity: &Identity,
        settings: &v8_chroma_core::fonts::FontSettings,
    ) -> Result<NativeFont> {
        Ok(describe(
            &Manager::new(None)?.apply_fonts_to(identity, Some(settings), None)?,
            false,
        ))
    }
}
pub mod ui_fonts {
    use super::*;
    pub fn last_applied(identity: &Identity) -> Result<Option<NativeFont>> {
        let manager = Manager::new(None)?;
        if !manager.has_session(identity.pid) {
            return Ok(None);
        }
        let status = manager.status_to(identity)?;
        Ok((status.fonts_available != 0 && status.font_generation != 0)
            .then(|| describe(&status, true)))
    }
    pub fn apply_to(
        identity: &Identity,
        settings: &v8_chroma_core::ui_fonts::UiFontSettings,
    ) -> Result<NativeFont> {
        Ok(describe(
            &Manager::new(None)?.apply_fonts_to(identity, None, Some(settings))?,
            true,
        ))
    }
}
fn describe(status: &v8_chroma_core::linux::Status, ui: bool) -> NativeFont {
    let enabled = if ui {
        status.ui_enabled
    } else {
        status.editor_enabled
    };
    let label = if !enabled {
        "Как в 1С".into()
    } else if ui {
        status.ui_family.clone()
    } else {
        format!(
            "{}, {} pt",
            status.editor_family,
            status.editor_points10 as f64 / 10.
        )
    };
    NativeFont {
        label,
        warning: if status.font_refresh == 95 {
            Some("Для обновления этого окна повторно откройте редактор или диалог".into())
        } else if ui && status.ui_fallback {
            Some(format!(
                "В Linux используется семейство {}",
                status.ui_family
            ))
        } else {
            None
        },
    }
}
