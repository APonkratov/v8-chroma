//! Own the native tray identity so portable builds can display Windows balloons
//! without an installer, a PowerShell notification identity, or a second icon.
use crate::{service::Service, updates::Updater};
use std::{
    cell::RefCell,
    time::{Duration, Instant},
};
use tauri::Manager;
use tray_icon::{
    Icon, MouseButton, MouseButtonState, TrayIcon, TrayIconBuilder, TrayIconEvent,
    menu::{IconMenuItem, Menu, MenuEvent, MenuItem},
};
use windows_sys::{
    Win32::{
        Foundation::HWND,
        UI::{
            Shell::{
                DefSubclassProc, NIF_GUID, NIF_INFO, NIIF_INFO, NIIF_RESPECT_QUIET_TIME,
                NIM_MODIFY, NIN_BALLOONSHOW, NOTIFYICONDATAW, RemoveWindowSubclass,
                SetWindowSubclass, Shell_NotifyIconW,
            },
            WindowsAndMessaging::WM_NCDESTROY,
        },
    },
    core::GUID,
};

const GUID_VALUE: u128 = 0x452e7a18_073d_4585_bf71_506ad820acc2;
// The native tray window in the pinned tray-icon 0.25.1 uses this callback message.
const TRAY_CALLBACK: u32 = 6002;
const NOTICE_SUBCLASS: usize = 1;
thread_local! {
    static TRAY: RefCell<Option<Items>> = const { RefCell::new(None) };
    static NOTICE: RefCell<Option<PendingNotice>> = const { RefCell::new(None) };
}
struct Items {
    tray: TrayIcon,
    toggle: IconMenuItem,
    update: MenuItem,
}
struct PendingNotice {
    requested: Instant,
    on_shown: Box<dyn FnOnce()>,
}

fn auto_apply_icon(enabled: bool) -> tray_icon::menu::Icon {
    let color = if enabled {
        [34, 197, 94]
    } else {
        [138, 138, 138]
    };
    let mut rgba = Vec::with_capacity(16 * 16 * 4);
    for y in 0..16 {
        for x in 0..16 {
            let dx = x as f32 + 0.5 - 8.0;
            let dy = y as f32 + 0.5 - 8.0;
            let alpha = ((5.0 - (dx * dx + dy * dy).sqrt()).clamp(0.0, 1.0) * 255.0).round() as u8;
            rgba.extend_from_slice(&[color[0], color[1], color[2], alpha]);
        }
    }
    tray_icon::menu::Icon::from_rgba(rgba, 16, 16).expect("fixed-size RGBA")
}

pub fn sync_auto_apply(enabled: Option<bool>) {
    TRAY.with_borrow(|items| {
        if let Some(items) = items {
            items
                .toggle
                .set_icon(Some(auto_apply_icon(enabled.unwrap_or(false))));
            items.toggle.set_enabled(enabled.is_some());
        }
    });
}

pub fn setup(app: &tauri::AppHandle) -> anyhow::Result<()> {
    let open = MenuItem::with_id("open", "Открыть", true, None);
    let toggle = IconMenuItem::with_id(
        "toggle",
        "Автоприменение",
        false,
        Some(auto_apply_icon(false)),
        None,
    );
    let update = MenuItem::with_id("update", "Проверить обновления", true, None);
    let exit = MenuItem::with_id("quit", "Завершить", true, None);
    let menu = Menu::new();
    menu.append_items(&[&open, &toggle, &update, &exit])?;
    let tray = TrayIconBuilder::new()
        .with_id("main")
        .with_guid(GUID_VALUE)
        .with_icon(Icon::from_rgba(
            include_bytes!("../icons/tray.rgba").to_vec(),
            32,
            32,
        )?)
        .with_tooltip("v8-chroma")
        .with_menu(Box::new(menu))
        .with_menu_on_left_click(false)
        .build()?;
    if unsafe {
        SetWindowSubclass(
            tray.window_handle(),
            Some(notice_callback),
            NOTICE_SUBCLASS,
            0,
        )
    } == 0
    {
        eprintln!("Не удалось подключить подтверждение показа уведомления");
    }
    TRAY.with_borrow_mut(|items| {
        *items = Some(Items {
            tray,
            toggle,
            update,
        })
    });
    let handle = app.clone();
    MenuEvent::set_event_handler(Some(move |event: MenuEvent| {
        let app = handle.clone();
        let _ = handle.run_on_main_thread(move || match event.id.as_ref() {
            "open" => super::show(&app),
            "quit" => app.state::<Service>().quit(),
            "update" => {
                super::show(&app);
                let _ = app.state::<Updater>().check();
            }
            "toggle" => {
                let service = app.state::<Service>().inner().clone();
                std::thread::spawn(move || {
                    let result = service.store.load().and_then(|loaded| {
                        service.save(v8_chroma_core::settings::SettingsPatch {
                            auto_apply: Some(!loaded.settings.auto_apply),
                            ..Default::default()
                        })
                    });
                    if let Err(error) = result {
                        eprintln!("{error:#}");
                    }
                });
            }
            _ => {}
        });
    }));
    let handle = app.clone();
    TrayIconEvent::set_event_handler(Some(move |event: TrayIconEvent| {
        if matches!(
            event,
            TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            }
        ) {
            let app = handle.clone();
            let _ = handle.run_on_main_thread(move || super::show(&app));
        }
    }));
    Ok(())
}

fn utf16<const N: usize>(text: &str) -> [u16; N] {
    let mut buffer = [0; N];
    for (to, from) in buffer[..N - 1].iter_mut().zip(text.encode_utf16()) {
        *to = from;
    }
    buffer
}

unsafe extern "system" fn notice_callback(
    hwnd: HWND,
    msg: u32,
    wp: usize,
    lp: isize,
    _: usize,
    _: usize,
) -> isize {
    if msg == TRAY_CALLBACK && (lp as u32 & 0xffff) == NIN_BALLOONSHOW {
        if let Ok(Some(notice)) = NOTICE.try_with(|notice| notice.borrow_mut().take()) {
            (notice.on_shown)();
        }
    } else if msg == WM_NCDESTROY {
        unsafe { RemoveWindowSubclass(hwnd, Some(notice_callback), NOTICE_SUBCLASS) };
    }
    unsafe { DefSubclassProc(hwnd, msg, wp, lp) }
}

pub fn sync_update(version: Option<&str>) {
    TRAY.with_borrow(|items| {
        if let Some(items) = items {
            let (label, tooltip) = match version {
                Some(version) => (
                    format!("Доступна версия {version}"),
                    format!("v8-chroma — доступна версия {version}"),
                ),
                None => ("Проверить обновления".to_owned(), "v8-chroma".to_owned()),
            };
            items.update.set_text(label);
            let _ = items.tray.set_tooltip(Some(tooltip));
        }
    });
}

pub fn notify(version: &str, on_shown: impl FnOnce() + 'static) {
    let Some(hwnd) =
        TRAY.with_borrow(|items| items.as_ref().map(|items| items.tray.window_handle()))
    else {
        return;
    };

    let requested = NOTICE.with_borrow_mut(|pending| {
        if pending
            .as_ref()
            .is_some_and(|notice| notice.requested.elapsed() < Duration::from_secs(60))
        {
            return false;
        }
        *pending = Some(PendingNotice {
            requested: Instant::now(),
            on_shown: Box::new(on_shown),
        });
        true
    });
    if !requested {
        return;
    }

    let data = NOTIFYICONDATAW {
        cbSize: std::mem::size_of::<NOTIFYICONDATAW>() as u32,
        hWnd: hwnd,
        uFlags: NIF_GUID | NIF_INFO,
        guidItem: GUID::from_u128(GUID_VALUE),
        szInfoTitle: utf16("Обновление v8-chroma"),
        szInfo: utf16(&format!(
            "Доступна версия {version}. Откройте приложение для установки."
        )),
        dwInfoFlags: NIIF_INFO | NIIF_RESPECT_QUIET_TIME,
        ..unsafe { std::mem::zeroed() }
    };
    if unsafe { Shell_NotifyIconW(NIM_MODIFY, &data) } == 0 {
        NOTICE.with_borrow_mut(|pending| {
            pending.take();
        });
        eprintln!("Windows отклонила запрос на показ уведомления об обновлении");
    }
}
