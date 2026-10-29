//! Application-independent application policy, also usable by the future Tauri worker.
use crate::{process::Identity, settings::Settings};
use std::collections::BTreeMap;

#[derive(Default)]
pub struct Controller {
    attempted: BTreeMap<(u32, u64), (String, bool)>,
    enabled: bool,
}
impl Controller {
    pub fn pause(&mut self) {
        self.enabled = false;
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
                self.attempted.insert(
                    (identity.pid, identity.created_time),
                    (settings.selected_theme.clone(), settings.custom_icons),
                );
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
        if !settings.auto_apply {
            self.pause();
            return vec![];
        }
        if !self.enabled {
            self.attempted.clear();
            self.enabled = true;
        }
        let appearance = (settings.selected_theme.clone(), settings.custom_icons);
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
