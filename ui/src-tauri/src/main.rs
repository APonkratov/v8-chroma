#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]
mod service;
mod shell;
#[cfg(all(not(debug_assertions), not(feature = "custom-protocol")))]
compile_error!("Release desktop builds require --features custom-protocol (embedded frontend)");
use service::{Service, Snapshot};
use std::path::PathBuf;
use tauri::{
    Manager, State,
    menu::{Menu, MenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
};
use tauri_plugin_autostart::ManagerExt;
use v8_chroma_core::{
    settings::{SettingsPatch, Store, WindowPatch},
    theme,
};

#[derive(serde::Serialize)]
struct UiState {
    #[serde(flatten)]
    snapshot: Snapshot,
    autostart_enabled: Option<bool>,
    autostart_error: Option<String>,
    custom_profile: bool,
}
struct Profile {
    custom: bool,
}
#[tauri::command]
fn get_state(
    app: tauri::AppHandle,
    service: State<'_, Service>,
    profile: State<'_, Profile>,
) -> UiState {
    let autostart = app.autolaunch().is_enabled();
    UiState {
        snapshot: service.snapshot(),
        autostart_enabled: autostart.as_ref().ok().copied(),
        autostart_error: autostart.err().map(|e| e.to_string()),
        custom_profile: profile.custom,
    }
}
#[tauri::command]
fn get_themes() -> Result<Vec<theme::Preview>, String> {
    theme::catalog().map_err(|e| format!("{e:#}"))
}
#[tauri::command]
async fn save_settings(service: State<'_, Service>, patch: SettingsPatch) -> Result<(), String> {
    let service = service.inner().clone();
    tauri::async_runtime::spawn_blocking(move || service.save(patch))
        .await
        .map_err(|e| e.to_string())?
        .map(|_| ())
        .map_err(|e| format!("{e:#}"))
}
#[tauri::command]
async fn reset_settings(service: State<'_, Service>) -> Result<(), String> {
    let service = service.inner().clone();
    tauri::async_runtime::spawn_blocking(move || service.reset())
        .await
        .map_err(|e| e.to_string())?
        .map(|_| ())
        .map_err(|e| format!("{e:#}"))
}
#[tauri::command]
async fn apply_theme(
    service: State<'_, Service>,
    pid: Option<u32>,
    created_time: Option<String>,
) -> Result<(), String> {
    let target = match (pid, created_time) {
        (Some(pid), Some(created)) => Some((
            pid,
            created
                .parse::<u64>()
                .map_err(|_| "Некорректное время создания процесса")?,
        )),
        (None, None) => None,
        _ => return Err("Неполная идентичность процесса".into()),
    };
    let service = service.inner().clone();
    tauri::async_runtime::spawn_blocking(move || service.request_apply(target))
        .await
        .map_err(|e| e.to_string())?
        .map_err(|e| format!("{e:#}"))
}
#[tauri::command]
fn refresh_processes(service: State<'_, Service>) {
    service.refresh();
}
#[tauri::command]
fn quit_app(service: State<'_, Service>) {
    service.quit();
}
#[tauri::command]
fn set_autostart(
    app: tauri::AppHandle,
    enabled: bool,
    profile: State<'_, Profile>,
) -> Result<(), String> {
    if profile.custom {
        return Err("Автозапуск доступен для основного профиля пользователя".into());
    }
    if enabled {
        app.autolaunch().enable()
    } else {
        app.autolaunch().disable()
    }
    .map_err(|e| e.to_string())
}
#[tauri::command]
async fn open_settings_folder(service: State<'_, Service>) -> Result<(), String> {
    let path = service.store.path();
    let parent = path
        .parent()
        .ok_or("Не найден каталог настроек")?
        .to_owned();
    tauri::async_runtime::spawn_blocking(move || shell::open_folder(parent))
        .await
        .map_err(|e| e.to_string())?
}
fn show(app: &tauri::AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

fn run() -> anyhow::Result<()> {
    let mut directory: Option<PathBuf> = None;
    let mut background = false;
    let mut args = std::env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--config-dir" => {
                directory = Some(
                    args.next()
                        .ok_or_else(|| anyhow::anyhow!("Укажите каталог после --config-dir"))?
                        .into(),
                )
            }
            "--background" => background = true,
            _ => anyhow::bail!("Неизвестный параметр: {arg}"),
        }
    }
    let custom = directory.is_some();
    let store = match directory {
        Some(path) => Store::at(path)?,
        None => Store::system()?,
    };
    let app = tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _, _| show(app)))
        .plugin(
            tauri_plugin_autostart::Builder::new()
                .app_name(v8_chroma_core::APP_NAME)
                .arg("--background")
                .build(),
        )
        .manage(Profile { custom })
        .invoke_handler(tauri::generate_handler![
            get_state,
            get_themes,
            save_settings,
            reset_settings,
            apply_theme,
            refresh_processes,
            quit_app,
            set_autostart,
            open_settings_folder
        ])
        .setup(move |app| {
            let handle = app.handle().clone();
            let on_exit = handle.clone();
            let service = Service::start(store, move || on_exit.exit(0));
            let saved = service
                .snapshot()
                .settings
                .map(|l| l.settings.window)
                .unwrap_or_default();
            app.manage(service);
            let open = MenuItem::with_id(app, "open", "Открыть v8-chroma", true, None::<&str>)?;
            let toggle = MenuItem::with_id(
                app,
                "toggle",
                "Включить / выключить авто-применение",
                true,
                None::<&str>,
            )?;
            let exit = MenuItem::with_id(app, "quit", "Завершить приложение", true, None::<&str>)?;
            let menu = Menu::with_items(app, &[&open, &toggle, &exit])?;
            TrayIconBuilder::new()
                .icon(tauri::image::Image::new_owned(
                    include_bytes!("../icons/tray.rgba").to_vec(),
                    32,
                    32,
                ))
                .tooltip("v8-chroma")
                .menu(&menu)
                .show_menu_on_left_click(false)
                .on_menu_event(|app, event| match event.id().as_ref() {
                    "open" => show(app),
                    "quit" => app.state::<Service>().quit(),
                    "toggle" => {
                        let service = app.state::<Service>().inner().clone();
                        std::thread::spawn(move || {
                            if let Ok(loaded) = service.store.load()
                                && let Err(error) = service.save(SettingsPatch {
                                    auto_apply: Some(!loaded.settings.auto_apply),
                                    ..Default::default()
                                })
                            {
                                eprintln!("{error:#}");
                            }
                        });
                    }
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if matches!(
                        event,
                        TrayIconEvent::Click {
                            button: MouseButton::Left,
                            button_state: MouseButtonState::Up,
                            ..
                        }
                    ) {
                        show(tray.app_handle());
                    }
                })
                .build(app)?;
            if let Some(window) = app.get_webview_window("main") {
                let monitor = window.current_monitor()?.or(window.primary_monitor()?);
                let (max_width, max_height) = monitor
                    .map(|m| {
                        let size = m.size().to_logical::<u32>(m.scale_factor());
                        (
                            size.width.saturating_sub(48).max(800),
                            size.height.saturating_sub(64).max(600),
                        )
                    })
                    .unwrap_or((3840, 2160));
                window.set_size(tauri::LogicalSize::new(
                    saved.width.clamp(800, 3840).min(max_width),
                    saved.height.clamp(600, 2160).min(max_height),
                ))?;
                let _ = window.center();
                if saved.maximized {
                    window.maximize()?;
                }
                if !background {
                    window.show()?;
                }
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            let service = window.state::<Service>();
            match event {
                tauri::WindowEvent::CloseRequested { api, .. } => {
                    api.prevent_close();
                    let _ = window.hide();
                }
                tauri::WindowEvent::Resized(size) => {
                    if size.width < 100
                        || size.height < 100
                        || window.is_minimized().unwrap_or(false)
                    {
                        return;
                    }
                    let maximized = window.is_maximized().unwrap_or(false);
                    let scale = window.scale_factor().unwrap_or(1.0);
                    let size = size.to_logical::<u32>(scale);
                    service.geometry(WindowPatch {
                        width: (!maximized).then_some(size.width.clamp(640, 3840)),
                        height: (!maximized).then_some(size.height.clamp(480, 2160)),
                        maximized: Some(maximized),
                    });
                }
                _ => {}
            }
        })
        .build(tauri::generate_context!())?;
    app.run(|app, event| {
        if let tauri::RunEvent::ExitRequested { api, .. } = event {
            let service = app.state::<Service>();
            if !service.finished() {
                api.prevent_exit();
                service.quit();
            }
        }
    });
    Ok(())
}
fn main() {
    if let Err(error) = run() {
        use std::os::windows::ffi::OsStrExt;
        let message: Vec<u16> =
            std::ffi::OsStr::new(&format!("Не удалось открыть v8-chroma:\n{error:#}"))
                .encode_wide()
                .chain(Some(0))
                .collect();
        // A native error dialog remains available even if the webview cannot be created.
        unsafe extern "system" {
            fn MessageBoxW(
                window: *mut std::ffi::c_void,
                text: *const u16,
                caption: *const u16,
                flags: u32,
            ) -> i32;
        }
        let caption: Vec<u16> = "v8-chroma".encode_utf16().chain(Some(0)).collect();
        unsafe {
            MessageBoxW(
                std::ptr::null_mut(),
                message.as_ptr(),
                caption.as_ptr(),
                0x10,
            );
        }
    }
}
