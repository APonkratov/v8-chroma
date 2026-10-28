use std::{os::windows::ffi::OsStrExt, path::PathBuf};
use windows_sys::Win32::{
    System::Com::{COINIT_APARTMENTTHREADED, CoInitializeEx, CoUninitialize},
    UI::{Shell::ShellExecuteW, WindowsAndMessaging::SW_SHOWNORMAL},
};

/// Open through the user's Windows shell, with a visible window and native error
/// handling. Spawning explorer.exe only confirms process creation; it does not
/// report whether the shell accepted the folder-open request.
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
