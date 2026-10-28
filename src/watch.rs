//! Application-independent application policy, also usable by the future Tauri worker.
use crate::{process::Identity, settings::Settings};
use std::collections::BTreeMap;

#[derive(Default)]
pub struct Controller {
    attempted: BTreeMap<(u32, u64), String>,
    enabled: bool,
}
impl Controller {
    pub fn pause(&mut self) {
        self.enabled = false;
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
        let mut requests = vec![];
        for identity in identities {
            let key = (identity.pid, identity.created_time);
            if self.attempted.get(&key) == Some(&settings.selected_theme) {
                continue;
            }
            self.attempted.insert(key, settings.selected_theme.clone());
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
