#[cfg(windows)]
use std::os::windows::ffi::OsStrExt;
use std::path::PathBuf;
#[cfg(windows)]
use windows_sys::Win32::{
    System::Com::{COINIT_APARTMENTTHREADED, CoInitializeEx, CoUninitialize},
    UI::{Shell::ShellExecuteW, WindowsAndMessaging::SW_SHOWNORMAL},
};

/// Open through the user's Windows shell, with a visible window and native error
/// handling. Spawning explorer.exe only confirms process creation; it does not
/// report whether the shell accepted the folder-open request.
#[cfg(windows)]
pub fn open_folder(folder: PathBuf) -> Result<(), String> {
    if !folder.is_dir() {
        return Err(format!("Каталог настроек недоступен: {}", folder.display()));
    }
    let folder = dunce::canonicalize(folder).map_err(|e| e.to_string())?;
    // Shell extensions may require STA. Use a dedicated COM apartment instead of
    // inheriting the webview/runtime worker's apartment or blocking the UI thread.
    std::thread::spawn(move || {
        let result = unsafe { CoInitializeEx(std::ptr::null(), COINIT_APARTMENTTHREADED as u32) };
        if result < 0 {
            return Err(format!(
                "Не удалось открыть каталог: COM 0x{:08X}",
                result as u32
            ));
        }
        struct ComApartment;
        impl Drop for ComApartment {
            fn drop(&mut self) {
                unsafe { CoUninitialize() };
            }
        }
        let _apartment = ComApartment;
        let path: Vec<u16> = folder.as_os_str().encode_wide().chain(Some(0)).collect();
        let code = unsafe {
            ShellExecuteW(
                std::ptr::null_mut(),
                windows_sys::core::w!("open"),
                path.as_ptr(),
                std::ptr::null(),
                std::ptr::null(),
                SW_SHOWNORMAL,
            )
        } as isize;
        if code <= 32 {
            Err(format!(
                "Не удалось открыть каталог {}: ошибка Windows Shell {code}",
                folder.display()
            ))
        } else {
            Ok(())
        }
    })
    .join()
    .map_err(|_| "Не удалось завершить открытие каталога настроек".to_owned())?
}

pub fn is_wsl() -> bool {
    cfg!(target_os = "linux")
        && (std::env::var_os("WSL_DISTRO_NAME").is_some()
            || std::fs::read_to_string("/proc/sys/kernel/osrelease")
                .is_ok_and(|value| value.to_lowercase().contains("microsoft")))
}
pub fn tray_available() -> bool {
    #[cfg(windows)]
    {
        true
    }
    #[cfg(target_os = "linux")]
    {
        std::process::Command::new("gdbus")
            .args([
                "call",
                "--session",
                "--dest",
                "org.kde.StatusNotifierWatcher",
                "--object-path",
                "/StatusNotifierWatcher",
                "--method",
                "org.freedesktop.DBus.Properties.Get",
                "org.kde.StatusNotifierWatcher",
                "IsStatusNotifierHostRegistered",
            ])
            .output()
            .is_ok_and(|result| {
                result.status.success() && String::from_utf8_lossy(&result.stdout).contains("true")
            })
    }
}
#[cfg(target_os = "linux")]
pub fn open_folder(folder: PathBuf) -> Result<(), String> {
    use std::process::{Command, Stdio};
    if !folder.is_dir() {
        return Err(format!("Каталог настроек недоступен: {}", folder.display()));
    }
    let folder = std::fs::canonicalize(folder).map_err(|error| error.to_string())?;
    if is_wsl() {
        let path = Command::new("wslpath")
            .arg("-w")
            .arg(&folder)
            .output()
            .map_err(|error| error.to_string())?;
        if !path.status.success() {
            return Err("Не удалось получить путь каталога для Проводника".into());
        }
        let path = std::str::from_utf8(&path.stdout)
            .map_err(|error| error.to_string())?
            .trim_end();
        Command::new("explorer.exe")
            .arg(path)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .map(|_| ())
            .map_err(|error| format!("Не удалось открыть Проводник: {error}"))
    } else {
        let status = Command::new("xdg-open")
            .arg(folder)
            .status()
            .map_err(|error| error.to_string())?;
        if status.success() {
            Ok(())
        } else {
            Err("Не удалось открыть каталог в файловом менеджере".into())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn unavailable_settings_folder_is_reported_without_launching_shell() {
        let temp = tempfile::tempdir().unwrap();
        let path = temp.path().join("Не каталог с пробелами");
        std::fs::write(&path, "file").unwrap();
        assert!(
            open_folder(path)
                .unwrap_err()
                .contains("Каталог настроек недоступен")
        );
        assert!(open_folder(temp.path().join("missing")).is_err());
    }
}
