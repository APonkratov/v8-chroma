//! Application-independent application policy, also usable by the future Tauri worker.
use crate::{process::Identity, settings::Settings};
use std::{
    collections::BTreeMap,
    time::{Duration, Instant},
};

type Appearance = (String, bool, Option<(String, u32)>, Option<String>);
fn appearance(settings: &Settings) -> Appearance {
    (
        settings.selected_theme.clone(),
        settings.custom_icons,
        settings.editor_font.appearance(),
        settings.ui_font.appearance(),
    )
}

#[derive(Default)]
pub struct Controller {
    attempted: BTreeMap<(u32, u64), Appearance>,
    pending_fonts: BTreeMap<(u32, u64, FontTarget), FontRetry>,
    enabled: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum FontTarget {
    Editor,
    Interface,
}

#[cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]
impl FontTarget {
    pub fn apply(
        self,
        identity: &Identity,
        settings: &Settings,
        wait: Duration,
    ) -> anyhow::Result<crate::fonts::FontApplied> {
        match self {
            Self::Editor => crate::fonts::apply_with_wait(identity, &settings.editor_font, wait),
            Self::Interface => crate::ui_fonts::apply_with_wait(identity, &settings.ui_font, wait),
        }
    }
    pub fn last_applied(
        self,
        identity: &Identity,
    ) -> anyhow::Result<Option<crate::fonts::FontApplied>> {
        match self {
            Self::Editor => crate::fonts::last_applied(identity),
            Self::Interface => crate::ui_fonts::last_applied(identity),
        }
    }
}

struct FontRetry {
    due: Instant,
    delay: Duration,
}

impl Controller {
    pub fn pause(&mut self) {
        self.enabled = false;
        self.pending_fonts.clear();
    }
    pub fn set_font_pending(
        &mut self,
        identity: &Identity,
        target: FontTarget,
        pending: bool,
        now: Instant,
    ) {
        let key = (identity.pid, identity.created_time, target);
        if !self.enabled || !pending {
            self.pending_fonts.remove(&key);
            return;
        }
        let delay = self
            .pending_fonts
            .get(&key)
            .map_or(Duration::from_secs(2), |retry| {
                (retry.delay * 2).min(Duration::from_secs(30))
            });
        self.pending_fonts.insert(
            key,
            FontRetry {
                due: now + delay,
                delay,
            },
        );
    }
    /// Call after normal apply attempts, so a settings change supersedes an older retry.
    pub fn font_retries(
        &self,
        identities: &[Identity],
        now: Instant,
    ) -> Vec<(Identity, FontTarget)> {
        self.pending_fonts
            .iter()
            .filter(|(_, retry)| retry.due <= now)
            .filter_map(|(&(pid, created, target), _)| {
                identities
                    .iter()
                    .find(|i| i.pid == pid && i.created_time == created)
                    .map(|i| (i.clone(), target))
            })
            .collect()
    }
    /// Explicit Apply commands work even while automatic application is paused.
    /// Keep other identities' attempts, so a single-process retry doesn't retry them.
    pub fn manual(&mut self, identities: &[Identity], settings: &Settings) -> Vec<Identity> {
        if settings.auto_apply {
            if !self.enabled {
                self.attempted.clear();
                self.enabled = true;
            }
            for identity in identities {
                self.attempted
                    .insert((identity.pid, identity.created_time), appearance(settings));
            }
        } else {
            self.pause();
        }
        identities.to_vec()
    }
    /// An attempt is recorded before dispatch, so a failing remote call isn't retried per tick.
    pub fn plan(&mut self, identities: &[Identity], settings: &Settings) -> Vec<Identity> {
        self.attempted.retain(|&(pid, created), _| {
            identities
                .iter()
                .any(|i| i.pid == pid && i.created_time == created)
        });
        self.pending_fonts.retain(|&(pid, created, _), _| {
            identities
                .iter()
                .any(|i| i.pid == pid && i.created_time == created)
        });
        if !settings.auto_apply {
            self.pause();
            return vec![];
        }
        if !self.enabled {
            self.attempted.clear();
            self.enabled = true;
        }
        let appearance = appearance(settings);
        let mut requests = vec![];
        for identity in identities {
            let key = (identity.pid, identity.created_time);
            if self.attempted.get(&key) == Some(&appearance) {
                continue;
            }
            self.attempted.insert(key, appearance.clone());
            requests.push(identity.clone());
        }
        requests
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn id(pid: u32, time: u64) -> Identity {
        Identity {
            pid,
            created_time: time,
            exe: "C:\\Platform\\1cv8.exe".into(),
        }
    }
    #[test]
    fn explicit_apply_works_with_auto_disabled() {
        let mut controller = Controller::default();
        let settings = Settings {
            auto_apply: false,
            ..Default::default()
        };
        let identities = [id(10, 1), id(20, 1)];
        assert_eq!(controller.manual(&identities, &settings), identities);
        assert!(controller.plan(&identities, &settings).is_empty());
        assert!(!settings.auto_apply);
    }
    #[test]
    fn manual_attempt_is_not_repeated_and_keeps_other_attempts() {
        let mut controller = Controller::default();
        let settings = Settings::default();
        let identities = [id(10, 1), id(20, 1)];
        controller.plan(&identities, &settings);
        assert_eq!(
            controller.manual(&identities[..1], &settings),
            identities[..1]
        );
        assert!(controller.plan(&identities, &settings).is_empty());
        let new = id(30, 1);
        assert_eq!(
            controller.plan(
                &[identities[0].clone(), identities[1].clone(), new.clone()],
                &settings
            ),
            vec![new]
        );
    }
    #[test]
    fn first_new_and_reused_processes_without_retries() {
        let mut controller = Controller::default();
        let settings = Settings::default();
        let a = id(10, 1);
        let b = id(20, 1);
        assert_eq!(
            controller.plan(std::slice::from_ref(&a), &settings),
            vec![a.clone()]
        );
        assert!(
            controller
                .plan(std::slice::from_ref(&a), &settings)
                .is_empty()
        );
        assert_eq!(controller.plan(&[a.clone(), b.clone()], &settings), vec![b]);
        assert_eq!(controller.plan(&[id(10, 2)], &settings), vec![id(10, 2)]);
    }
    #[test]
    fn pending_font_waits_retries_only_its_target_and_never_follows_a_reused_pid() {
        let mut controller = Controller::default();
        let settings = Settings::default();
        let identities = [id(10, 1)];
        let now = Instant::now();
        assert_eq!(controller.plan(&identities, &settings), identities);
        controller.set_font_pending(&identities[0], FontTarget::Editor, true, now);
        assert!(controller.font_retries(&identities, now).is_empty());
        let due = now + Duration::from_secs(2);
        assert!(controller.plan(&identities, &settings).is_empty());
        assert_eq!(
            controller.font_retries(&identities, due),
            vec![(identities[0].clone(), FontTarget::Editor)]
        );
        controller.set_font_pending(&identities[0], FontTarget::Editor, true, due);
        assert!(
            controller
                .font_retries(&identities, due + Duration::from_secs(2))
                .is_empty()
        );
        assert!(
            controller
                .font_retries(&[id(10, 2)], due + Duration::from_secs(4))
                .is_empty()
        );
        controller.pause();
        assert!(
            controller
                .font_retries(&identities, due + Duration::from_secs(30))
                .is_empty()
        );
    }
    #[test]
    fn theme_change_updates_all_but_geometry_change_does_not() {
        let mut controller = Controller::default();
        let mut settings = Settings::default();
        let identities = [id(10, 1), id(20, 1)];
        assert_eq!(controller.plan(&identities, &settings).len(), 2);
        settings.window.width = 1200;
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.selected_theme = "monokai".into();
        assert_eq!(controller.plan(&identities, &settings).len(), 2);
    }
    #[test]
    fn icon_toggle_reapplies_once_without_changing_theme() {
        let mut controller = Controller::default();
        let mut settings = Settings::default();
        let identities = [id(10, 1), id(20, 1)];
        assert_eq!(controller.plan(&identities, &settings).len(), 2);
        settings.custom_icons = true;
        assert_eq!(controller.plan(&identities, &settings).len(), 2);
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.custom_icons = false;
        assert_eq!(controller.plan(&identities, &settings).len(), 2);
    }
    #[test]
    fn fonts_reapply_once_and_disabled_preview_does_not_dispatch() {
        let mut controller = Controller::default();
        let mut settings = Settings::default();
        let identities = [id(10, 1)];
        controller.plan(&identities, &settings);
        settings.editor_font.points10 = 180;
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.editor_font.enabled = true;
        assert_eq!(controller.plan(&identities, &settings).len(), 1);
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.editor_font.family = "Consolas".into();
        assert_eq!(controller.plan(&identities, &settings).len(), 1);
        settings.editor_font.enabled = false;
        assert_eq!(controller.plan(&identities, &settings).len(), 1);
        settings.auto_apply = false;
        settings.editor_font.enabled = true;
        assert!(controller.plan(&identities, &settings).is_empty());
        assert_eq!(controller.manual(&identities, &settings).len(), 1);
    }
    #[test]
    fn interface_font_changes_apply_once_and_native_mode_remembers_family() {
        let mut controller = Controller::default();
        let mut settings = Settings::default();
        let identities = [id(10, 1)];
        controller.plan(&identities, &settings);
        settings.ui_font.family = "Segoe UI".into();
        assert_eq!(controller.plan(&identities, &settings), identities);
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.ui_font.enabled = false;
        assert_eq!(controller.plan(&identities, &settings), identities);
        settings.ui_font.family = "Roboto".into();
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.auto_apply = false;
        settings.ui_font.enabled = true;
        assert!(controller.plan(&identities, &settings).is_empty());
        assert_eq!(controller.manual(&identities, &settings), identities);
        settings.auto_apply = true;
        assert_eq!(controller.plan(&identities, &settings), identities);
        assert!(controller.plan(&identities, &settings).is_empty());
    }
    #[test]
    fn disabling_and_reenabling_reasserts_saved_theme() {
        let mut controller = Controller::default();
        let mut settings = Settings::default();
        let identities = [id(10, 1)];
        controller.plan(&identities, &settings);
        settings.auto_apply = false;
        assert!(controller.plan(&identities, &settings).is_empty());
        settings.auto_apply = true;
        assert_eq!(controller.plan(&identities, &settings).len(), 1);
    }
    #[test]
    fn malformed_settings_pause_then_recovery_reconciles() {
        let mut controller = Controller::default();
        let settings = Settings::default();
        let identities = [id(10, 1)];
        controller.plan(&identities, &settings);
        controller.pause();
        assert_eq!(controller.plan(&identities, &settings).len(), 1);
    }
}
