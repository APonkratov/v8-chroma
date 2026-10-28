//! Windows boundary. Handles own their lifetime; remote calls never outlive argument memory.
use anyhow::{Context, Result, bail, ensure};
use std::{
    collections::BTreeMap,
    ffi::{OsStr, OsString, c_void},
    fs::File,
    io::{Read, Seek, SeekFrom},
    os::windows::ffi::{OsStrExt, OsStringExt},
    path::{Path, PathBuf},
    ptr::{null, null_mut},
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::*,
    Graphics::Gdi::*,
    Security::*,
    Storage::FileSystem::*,
    System::{
        Diagnostics::{Debug::WriteProcessMemory, ToolHelp::*},
        LibraryLoader::*,
        Memory::*,
        ProcessStatus::K32GetModuleFileNameExW,
        RemoteDesktop::ProcessIdToSessionId,
        Threading::*,
    },
    UI::{Shell::CommandLineToArgvW, WindowsAndMessaging::*},
};

fn check(ok: i32, label: &str) -> Result<()> {
    if ok == 0 {
        return Err(std::io::Error::last_os_error()).context(label.to_owned());
    }
    Ok(())
}
pub fn wide(value: &OsStr) -> Vec<u16> {
    value.encode_wide().chain(Some(0)).collect()
}
pub fn canonical(path: &Path) -> Result<PathBuf> {
    let path = path
        .canonicalize()
        .with_context(|| format!("Нормализация пути {}", path.display()))?;
    let text = path.to_str().context("Путь не представлен в Unicode")?;
    Ok(PathBuf::from(text.strip_prefix(r"\\?\").unwrap_or(text)))
}
pub fn same_path(a: &Path, b: &Path) -> bool {
    a.as_os_str()
        .to_string_lossy()
        .eq_ignore_ascii_case(&b.as_os_str().to_string_lossy())
}

pub struct Handle(pub HANDLE);
impl Handle {
    pub fn new(raw: HANDLE, label: &str) -> Result<Self> {
        if raw.is_null() || raw == INVALID_HANDLE_VALUE {
            return Err(std::io::Error::last_os_error()).context(label.to_owned());
        }
        Ok(Self(raw))
    }
}
impl Drop for Handle {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

pub use crate::process::Identity;

pub struct Process {
    pub handle: Handle,
    pub identity: Identity,
}
impl Process {
    pub fn open(pid: u32, writable: bool) -> Result<Self> {
        unsafe {
            let rights = PROCESS_QUERY_LIMITED_INFORMATION
                | SYNCHRONIZE
                | if writable {
                    PROCESS_CREATE_THREAD
                        | PROCESS_VM_OPERATION
                        | PROCESS_VM_WRITE
                        | PROCESS_VM_READ
                        | PROCESS_QUERY_INFORMATION
                } else {
                    0
                };
            let handle = Handle::new(OpenProcess(rights, 0, pid), "OpenProcess")?;
            let mut buffer = vec![0u16; 32768];
            let mut length = buffer.len() as u32;
            check(
                QueryFullProcessImageNameW(handle.0, 0, buffer.as_mut_ptr(), &mut length),
                "QueryFullProcessImageNameW",
            )?;
            let exe = canonical(Path::new(&OsString::from_wide(&buffer[..length as usize])))?;
            let mut times = [FILETIME {
                dwLowDateTime: 0,
                dwHighDateTime: 0,
            }; 4];
            check(
                GetProcessTimes(
                    handle.0,
                    &mut times[0],
                    &mut times[1],
                    &mut times[2],
                    &mut times[3],
                ),
                "GetProcessTimes",
            )?;
            let created_time =
                ((times[0].dwHighDateTime as u64) << 32) | times[0].dwLowDateTime as u64;
            let process = Self {
                handle,
                identity: Identity {
                    pid,
                    created_time,
                    exe,
                },
            };
            process.check_owner()?;
            process.alive()?;
            Ok(process)
        }
    }
    pub fn alive(&self) -> Result<()> {
        ensure!(
            unsafe { WaitForSingleObject(self.handle.0, 0) } == WAIT_TIMEOUT,
            "Целевой процесс уже завершён"
        );
        Ok(())
    }
    pub fn matches(&self, expected: &Identity) -> Result<()> {
        ensure!(
            self.identity.pid == expected.pid
                && self.identity.created_time == expected.created_time
                && same_path(&self.identity.exe, &expected.exe),
            "Идентичность процесса изменилась (PID использован повторно)"
        );
        self.alive()
    }
    fn check_owner(&self) -> Result<()> {
        unsafe {
            let mut mine = 0;
            let mut target = 0;
            check(
                ProcessIdToSessionId(GetCurrentProcessId(), &mut mine),
                "current session",
            )?;
            check(
                ProcessIdToSessionId(self.identity.pid, &mut target),
                "target session",
            )?;
            ensure!(
                mine == target,
                "Процесс находится в другой интерактивной сессии"
            );
            let a = token_sid(GetCurrentProcess())?;
            let b = token_sid(self.handle.0)?;
            ensure!(
                EqualSid(a.as_ptr() as _, b.as_ptr() as _) != 0,
                "Процесс принадлежит другому пользователю"
            );
            Ok(())
        }
    }
    pub fn modules(&self) -> Result<BTreeMap<String, (usize, PathBuf)>> {
        let deadline = Instant::now() + Duration::from_secs(2);
        loop {
            self.alive()?;
            match self.modules_once() {
                Err(e)
                    if e.downcast_ref::<std::io::Error>()
                        .is_some_and(|e| matches!(e.raw_os_error(), Some(24 | 299)))
                        && Instant::now() < deadline =>
                {
                    std::thread::sleep(Duration::from_millis(50))
                }
                result => {
                    self.alive()?;
                    return result;
                }
            }
        }
    }
    fn modules_once(&self) -> Result<BTreeMap<String, (usize, PathBuf)>> {
        unsafe {
            let snapshot = Handle::new(
                CreateToolhelp32Snapshot(
                    TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                    self.identity.pid,
                ),
                "module snapshot",
            )?;
            let reader = Handle::new(
                OpenProcess(
                    PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
                    0,
                    self.identity.pid,
                ),
                "OpenProcess for module paths",
            )?;
            self.alive()?;
            let mut item: MODULEENTRY32W = std::mem::zeroed();
            item.dwSize = size_of::<MODULEENTRY32W>() as u32;
            let mut ok = Module32FirstW(snapshot.0, &mut item);
            let mut modules = BTreeMap::new();
            if ok == 0 {
                check(ok, "Module32FirstW")?;
            }
            let mut buffer = vec![0u16; 32768];
            while ok != 0 {
                // Toolhelp can replace characters outside the system ANSI code
                // page with '?' even in MODULEENTRY32W. Read the Unicode path
                // from the module itself; keep discovery's handle read-limited.
                let length = K32GetModuleFileNameExW(
                    reader.0,
                    item.hModule,
                    buffer.as_mut_ptr(),
                    buffer.len() as u32,
                );
                if length == 0 {
                    return Err(std::io::Error::last_os_error()).context("K32GetModuleFileNameExW");
                }
                ensure!(
                    (length as usize) < buffer.len() - 1,
                    "Путь модуля не помещается в буфер"
                );
                let path = canonical(Path::new(&OsString::from_wide(&buffer[..length as usize])))?;
                let name = path
                    .file_name()
                    .context("Имя файла модуля отсутствует")?
                    .to_string_lossy()
                    .to_lowercase();
                modules.insert(name, (item.modBaseAddr as usize, path));
                ok = Module32NextW(snapshot.0, &mut item);
            }
            ensure!(
                GetLastError() == ERROR_NO_MORE_FILES,
                "Не удалось прочитать полный список модулей"
            );
            Ok(modules)
        }
    }
    pub fn call(&self, address: usize, bytes: Option<&[u8]>) -> Result<u32> {
        self.alive()?;
        unsafe {
            let mut allocation = null_mut();
            if let Some(bytes) = bytes {
                allocation = VirtualAllocEx(
                    self.handle.0,
                    null(),
                    bytes.len(),
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_READWRITE,
                );
                ensure!(
                    !allocation.is_null(),
                    "VirtualAllocEx: {}",
                    std::io::Error::last_os_error()
                );
                let mut written = 0;
                let ok = WriteProcessMemory(
                    self.handle.0,
                    allocation,
                    bytes.as_ptr() as _,
                    bytes.len(),
                    &mut written,
                );
                if ok == 0 || written != bytes.len() {
                    let error = std::io::Error::last_os_error();
                    VirtualFreeEx(self.handle.0, allocation, 0, MEM_RELEASE);
                    return Err(error).context("WriteProcessMemory");
                }
            }
            let start: unsafe extern "system" fn(*mut c_void) -> u32 = std::mem::transmute(address);
            let raw = CreateRemoteThread(
                self.handle.0,
                null(),
                0,
                Some(start),
                allocation,
                0,
                null_mut(),
            );
            if raw.is_null() {
                let error = std::io::Error::last_os_error();
                if !allocation.is_null() {
                    VirtualFreeEx(self.handle.0, allocation, 0, MEM_RELEASE);
                }
                return Err(error).context("CreateRemoteThread");
            }
            let thread = Handle(raw);
            if WaitForSingleObject(thread.0, 15000) != WAIT_OBJECT_0 {
                // Keep argument memory: the remote thread may still read it. No retries.
                bail!(
                    "REMOTE_TIMEOUT: состояние неизвестно; память аргумента сохранена, автоматический повтор запрещён"
                );
            }
            if !allocation.is_null() {
                VirtualFreeEx(self.handle.0, allocation, 0, MEM_RELEASE);
            }
            let mut code = 0;
            check(GetExitCodeThread(thread.0, &mut code), "GetExitCodeThread")?;
            Ok(code)
        }
    }
    pub fn load_dll(&self, dll: &Path) -> Result<()> {
        unsafe {
            let kernel = GetModuleHandleW(wide(OsStr::new("kernel32.dll")).as_ptr());
            let address = GetProcAddress(kernel, c"LoadLibraryW".as_ptr() as _)
                .context("LoadLibraryW export")? as usize;
            let mut owner = null_mut();
            check(
                GetModuleHandleExW(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    address as _,
                    &mut owner,
                ),
                "LoadLibrary owner",
            )?;
            let mut file = [0u16; 32768];
            let length = GetModuleFileNameW(owner, file.as_mut_ptr(), file.len() as u32) as usize;
            ensure!(length > 0 && length < file.len(), "LoadLibrary owner path");
            let owner_name = PathBuf::from(OsString::from_wide(&file[..length]));
            let name = owner_name
                .file_name()
                .context("owner filename")?
                .to_string_lossy()
                .to_lowercase();
            let modules = self.modules()?;
            let remote_owner = modules
                .get(&name)
                .context("Нет модуля-владельца LoadLibrary")?
                .0;
            let data = wide(dll.as_os_str());
            let bytes = std::slice::from_raw_parts(data.as_ptr() as *const u8, data.len() * 2);
            self.call(remote_owner + address - owner as usize, Some(bytes))?;
            Ok(())
        }
    }
    pub fn exports(&self, dll: &Path) -> Result<BTreeMap<&'static str, usize>> {
        let modules = self.modules()?;
        let (base, path) = modules
            .get("themeprobe.dll")
            .context("ThemeProbe.dll не загружена")?;
        ensure!(
            same_path(path, dll),
            "В процессе уже загружена другая ThemeProbe.dll; требуется штатный перезапуск 1С"
        );
        unsafe {
            // Read export RVAs without running DllMain in the manager.
            let local = LoadLibraryExW(
                wide(dll.as_os_str()).as_ptr(),
                null_mut(),
                DONT_RESOLVE_DLL_REFERENCES,
            );
            ensure!(
                !local.is_null(),
                "LoadLibraryExW metadata: {}",
                std::io::Error::last_os_error()
            );
            let result = (|| {
                let mut result = BTreeMap::new();
                for name in [
                    "ThemeInitialize",
                    "ThemeApply",
                    "ThemeSnapshot",
                    "ThemeStop",
                ] {
                    let name_c = std::ffi::CString::new(name)?;
                    let address = GetProcAddress(local, name_c.as_ptr() as _)
                        .context("Отсутствует экспорт ThemeProbe")?
                        as usize;
                    result.insert(name, base + address - local as usize);
                }
                Ok(result)
            })();
            FreeLibrary(local);
            result
        }
    }
}

fn token_sid(process: HANDLE) -> Result<Vec<u8>> {
    unsafe {
        let mut token = null_mut();
        check(
            OpenProcessToken(process, TOKEN_QUERY, &mut token),
            "OpenProcessToken",
        )?;
        let token = Handle(token);
        let mut length = 0;
        GetTokenInformation(token.0, TokenUser, null_mut(), 0, &mut length);
        ensure!(length > 0, "GetTokenInformation size");
        let mut buffer = vec![0usize; (length as usize).div_ceil(size_of::<usize>())];
        check(
            GetTokenInformation(
                token.0,
                TokenUser,
                buffer.as_mut_ptr() as _,
                length,
                &mut length,
            ),
            "GetTokenInformation",
        )?;
        let user = &*(buffer.as_ptr() as *const TOKEN_USER);
        let length = GetLengthSid(user.User.Sid);
        ensure!(length > 0, "GetLengthSid");
        let mut result = vec![0u8; length as usize];
        check(
            CopySid(length, result.as_mut_ptr() as _, user.User.Sid),
            "CopySid",
        )?;
        Ok(result)
    }
}

pub fn is_designer(command: &str) -> Result<bool> {
    ensure!(
        !command.encode_utf16().any(|c| c == 0),
        "NUL в командной строке"
    );
    unsafe {
        let mut argc = 0;
        let argv = CommandLineToArgvW(wide(OsStr::new(command)).as_ptr(), &mut argc);
        ensure!(!argv.is_null(), "CommandLineToArgvW");
        let result = if argc >= 2 {
            let value = *argv.add(1);
            let mut n = 0;
            while *value.add(n) != 0 {
                n += 1;
            }
            String::from_utf16_lossy(std::slice::from_raw_parts(value, n))
                .eq_ignore_ascii_case("DESIGNER")
        } else {
            false
        };
        LocalFree(argv as _);
        Ok(result)
    }
}

pub fn platform_version(path: &Path) -> Result<[u16; 4]> {
    let mut file = File::open(path)?;
    let mut dos = [0u8; 64];
    file.read_exact(&mut dos)?;
    ensure!(&dos[..2] == b"MZ", "Не PE-файл");
    let offset = u32::from_le_bytes(dos[60..64].try_into()?);
    file.seek(SeekFrom::Start(offset as u64))?;
    let mut pe = [0u8; 6];
    file.read_exact(&mut pe)?;
    ensure!(
        &pe[..4] == b"PE\0\0" && u16::from_le_bytes(pe[4..6].try_into()?) == 0x8664,
        "Поддерживается только AMD64"
    );
    unsafe {
        let filename = wide(path.as_os_str());
        let mut ignored = 0;
        let length = GetFileVersionInfoSizeW(filename.as_ptr(), &mut ignored);
        ensure!(length > 0, "Нет версии файла");
        let mut data = vec![0u32; (length as usize).div_ceil(4)];
        check(
            GetFileVersionInfoW(filename.as_ptr(), 0, length, data.as_mut_ptr() as _),
            "GetFileVersionInfoW",
        )?;
        let mut fixed = null_mut();
        let mut size = 0;
        check(
            VerQueryValueW(
                data.as_ptr() as _,
                wide(OsStr::new("\\")).as_ptr(),
                &mut fixed,
                &mut size,
            ),
            "VerQueryValueW",
        )?;
        ensure!(size >= 16, "Некорректная версия");
        let fixed = fixed as *const u32;
        Ok([
            (*fixed.add(2) >> 16) as u16,
            *fixed.add(2) as u16,
            (*fixed.add(3) >> 16) as u16,
            *fixed.add(3) as u16,
        ])
    }
}

pub fn redraw(pid: u32, theme_changed: bool) {
    unsafe extern "system" fn theme_changed_child(window: HWND, _: LPARAM) -> i32 {
        unsafe {
            PostMessageW(window, WM_THEMECHANGED, 0, 0);
        }
        1
    }
    if theme_changed {
        unsafe extern "system" fn notify(window: HWND, arg: LPARAM) -> i32 {
            unsafe {
                let mut owner = 0;
                GetWindowThreadProcessId(window, &mut owner);
                if owner == arg as u32 {
                    PostMessageW(window, WM_THEMECHANGED, 0, 0);
                    EnumChildWindows(window, Some(theme_changed_child), 0);
                }
            }
            1
        }
        // WebKit retains painted pages in a backing store. Its normal theme
        // notification drops that cache on the UI thread without navigation.
        unsafe {
            EnumWindows(Some(notify), pid as LPARAM);
        }
    }
    unsafe extern "system" fn visit(window: HWND, arg: LPARAM) -> i32 {
        unsafe {
            let mut owner = 0;
            GetWindowThreadProcessId(window, &mut owner);
            if owner == arg as u32 {
                RedrawWindow(
                    window,
                    null(),
                    null_mut(),
                    RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN,
                );
            }
        }
        1
    }
    unsafe {
        EnumWindows(Some(visit), pid as LPARAM);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn classifies_exact_mode_without_substring_false_positives() {
        assert!(is_designer(r#""C:\Program Files\1cv8.exe" DESIGNER /F "C:\test""#).unwrap());
        assert!(is_designer("1cv8.exe designer /F test").unwrap());
        for command in [
            "1cv8.exe ENTERPRISE /F DESIGNER",
            "1cv8.exe DESIGNER-extra",
            "1cv8.exe /F DESIGNER",
            "1cv8.exe",
            "",
        ] {
            assert!(!is_designer(command).unwrap(), "{command}");
        }
        assert!(is_designer("1cv8.exe\0 DESIGNER").is_err());
    }
    #[test]
    fn process_identity_and_owner_on_current_process() {
        let p = Process::open(std::process::id(), false).unwrap();
        p.matches(&p.identity).unwrap();
        let mut stale = p.identity.clone();
        stale.created_time += 1;
        assert!(p.matches(&stale).is_err());
    }
}
