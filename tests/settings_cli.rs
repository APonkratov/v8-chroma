#![cfg(all(
    target_os = "windows",
    target_arch = "x86_64",
    feature = "windows-engine"
))]

use serde_json::Value;
use std::{
    fs,
    io::Read,
    path::Path,
    process::{Command, Output, Stdio},
    time::{Duration, Instant},
};
use v8_chroma_core::settings::{Settings, SettingsPatch, Store};

fn command(profile: &Path, args: &[&str]) -> Command {
    let mut cmd = Command::new(env!("CARGO_BIN_EXE_v8-chroma-cli"));
    cmd.arg("--config-dir").arg(profile).args(args);
    cmd
}
fn cli(profile: &Path, args: &[&str]) -> Output {
    command(profile, args).output().unwrap()
}
fn value(output: Output) -> Value {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    serde_json::from_slice(&output.stdout).unwrap()
}
fn events(path: &Path) -> Vec<Value> {
    let mut bytes = vec![];
    fs::File::open(path)
        .unwrap()
        .read_to_end(&mut bytes)
        .unwrap();
    serde_json::Deserializer::from_slice(&bytes)
        .into_iter()
        .filter_map(Result::ok)
        .collect()
}
fn until(mut predicate: impl FnMut() -> bool) {
    let deadline = Instant::now() + Duration::from_secs(6);
    while !predicate() {
        assert!(Instant::now() < deadline, "Timed out waiting for CLI event");
        std::thread::sleep(Duration::from_millis(25));
    }
}

#[test]
fn interface_font_flags_persist_native_mode_and_reject_invalid_names() {
    let temp = tempfile::tempdir().unwrap();
    let profile = temp.path().join("profile");
    let saved = value(cli(
        &profile,
        &[
            "settings",
            "set",
            "--auto-apply",
            "off",
            "--ui-font",
            "Segoe UI",
        ],
    ));
    assert_eq!(
        saved["settings"]["ui_font"],
        serde_json::json!({"enabled":true,"family":"Segoe UI"})
    );
    let saved = value(cli(&profile, &["settings", "set", "--native-ui-font"]));
    assert_eq!(
        saved["settings"]["ui_font"],
        serde_json::json!({"enabled":false,"family":"Segoe UI"})
    );
    assert_eq!(saved["settings"]["auto_apply"], false);
    let before = fs::read(profile.join("settings.json")).unwrap();
    assert!(
        !cli(&profile, &["settings", "set", "--ui-font", "bad\nfont"])
            .status
            .success()
    );
    assert_eq!(fs::read(profile.join("settings.json")).unwrap(), before);
    assert!(
        !cli(
            &profile,
            &[
                "settings",
                "set",
                "--native-ui-font",
                "--ui-font",
                "Segoe UI"
            ]
        )
        .status
        .success()
    );
    assert_eq!(fs::read(profile.join("settings.json")).unwrap(), before);
}
#[test]
fn separate_processes_persist_preferences_and_override_is_temporary() {
    let temp = tempfile::tempdir().unwrap();
    let profile = temp.path().join("Настройки пользователя");
    let shown = value(cli(&profile, &["settings", "show"]));
    assert_eq!(shown["source"], "defaults");
    assert!(!profile.join("settings.json").exists());
    value(cli(
        &profile,
        &[
            "settings",
            "set",
            "--theme",
            "night-owl",
            "--auto-apply",
            "off",
            "--custom-icons",
            "on",
            "--width",
            "1200",
        ],
    ));
    let shown = value(cli(&profile, &["settings", "show"]));
    assert_eq!(shown["settings"]["selected_theme"], "night-owl");
    assert_eq!(shown["settings"]["auto_apply"], false);
    assert_eq!(shown["settings"]["custom_icons"], true);
    let run = cli(&profile, &["watch", "--theme", "dracula", "--seconds", "1"]);
    assert!(run.status.success());
    let event: Value = serde_json::from_slice(&run.stdout).unwrap();
    assert_eq!(event["theme"], "dracula");
    assert_eq!(event["auto_apply"], false);
    let shown = value(cli(&profile, &["settings", "show"]));
    assert_eq!(shown["settings"]["selected_theme"], "night-owl");
    assert_eq!(shown["settings"]["window"]["width"], 1200);
    let primary = fs::read(profile.join("settings.json")).unwrap();
    assert!(
        !cli(&profile, &["settings", "set", "--theme", "unknown"])
            .status
            .success()
    );
    assert_eq!(fs::read(profile.join("settings.json")).unwrap(), primary);
    assert!(!cli(&profile, &["settings", "set"]).status.success());
}

#[test]
fn malformed_and_future_files_do_not_block_explicit_manual_override() {
    let temp = tempfile::tempdir().unwrap();
    let profile = temp.path();
    fs::write(profile.join("settings.json"), br#"{"schema_version":999}"#).unwrap();
    assert!(!cli(profile, &["settings", "reset"]).status.success());
    let rejected = cli(profile, &["attach", "--pid", "0"]);
    assert!(String::from_utf8_lossy(&rejected.stderr).contains("Версия настроек"));
    let explicit = cli(profile, &["attach", "--pid", "0", "--theme", "dracula"]);
    assert!(String::from_utf8_lossy(&explicit.stderr).contains("Процесс 1cv8.exe не найден"));
    assert!(cli(profile, &["themes"]).status.success());
    assert_eq!(
        fs::read(profile.join("settings.json")).unwrap(),
        br#"{"schema_version":999}"#
    );
}

#[test]
fn running_watcher_reloads_pauses_and_recovers_without_native_actions() {
    let temp = tempfile::tempdir().unwrap();
    let profile = temp.path().join("profile");
    let store = Store::at(&profile).unwrap();
    store
        .update(SettingsPatch {
            auto_apply: Some(false),
            ..Default::default()
        })
        .unwrap();
    let stdout = temp.path().join("stdout.json");
    let stderr = temp.path().join("stderr.txt");
    let mut child = command(&profile, &["watch", "--seconds", "8"])
        .stdout(Stdio::from(fs::File::create(&stdout).unwrap()))
        .stderr(Stdio::from(fs::File::create(&stderr).unwrap()))
        .spawn()
        .unwrap();
    until(|| events(&stdout).iter().any(|e| e["theme"] == "dracula"));
    store
        .update(SettingsPatch {
            selected_theme: Some("night-owl".into()),
            ..Default::default()
        })
        .unwrap();
    until(|| events(&stdout).iter().any(|e| e["theme"] == "night-owl"));
    fs::write(store.path(), b"broken").unwrap();
    fs::write(profile.join("settings.json.bak"), b"broken").unwrap();
    until(|| {
        fs::read_to_string(&stderr)
            .unwrap()
            .contains("приостановлено")
    });
    let repaired = Settings {
        selected_theme: "github-light-default".into(),
        auto_apply: false,
        ..Default::default()
    };
    fs::write(store.path(), serde_json::to_vec(&repaired).unwrap()).unwrap();
    fs::write(
        profile.join("settings.json.bak"),
        serde_json::to_vec(&repaired).unwrap(),
    )
    .unwrap();
    until(|| {
        events(&stdout)
            .iter()
            .any(|e| e["theme"] == "github-light-default")
    });
    assert!(child.wait().unwrap().success());
    let events = events(&stdout);
    assert!(
        events
            .iter()
            .all(|event| event["event"] == "settings" && event["auto_apply"] == false)
    );
}

#[test]
fn no_subcommand_uses_saved_preferences() {
    let temp = tempfile::tempdir().unwrap();
    let profile = temp.path().join("profile");
    value(cli(
        &profile,
        &[
            "settings",
            "set",
            "--theme",
            "monokai",
            "--auto-apply",
            "off",
        ],
    ));
    let stdout = temp.path().join("stdout.json");
    let mut child = command(&profile, &[])
        .stdout(Stdio::from(fs::File::create(&stdout).unwrap()))
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    until(|| {
        events(&stdout)
            .iter()
            .any(|e| e["theme"] == "monokai" && e["auto_apply"] == false)
    });
    // Only the isolated, auto_apply=false CLI test process is terminated here; no 1C is touched.
    child.kill().unwrap();
    child.wait().unwrap();
}
