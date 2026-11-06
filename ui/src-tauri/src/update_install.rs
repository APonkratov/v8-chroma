//! The running EXE cannot be overwritten on Windows. A copy waits for its parent,
//! verifies the signed payload again, swaps files, and waits for startup acknowledgement.
use crate::update_release::{self as release, GUI, PUBLIC_KEY, SIGNATURE, SUMS};
use anyhow::{Context, Result, bail, ensure};
use serde::{Deserialize, Serialize};
use std::{
    ffi::OsString,
    fs,
    io::Write,
    os::windows::process::CommandExt,
    path::{Path, PathBuf},
    process::{Child, Command},
    time::{Duration, Instant},
};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0},
    System::Threading::{
        OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_SYNCHRONIZE,
        QueryFullProcessImageNameW, WaitForSingleObject,
    },
};

pub const PREFIX: &str = ".v8-chroma-update-";
pub fn record_error(stage: &Path, error: &anyhow::Error) {
    // Internal flags must not become an arbitrary-file-write interface.
    if let (Ok(stage), Ok(exe)) = (
        dunce::canonicalize(stage),
        std::env::current_exe().and_then(dunce::canonicalize),
    ) && stage
        .file_name()
        .and_then(|n| n.to_str())
        .is_some_and(|n| n.starts_with(PREFIX))
        && exe == stage.join("helper.exe")
    {
        let _ = fs::write(stage.join("error.txt"), format!("{error:#}"));
    }
}
pub fn restart_args() -> Vec<OsString> {
    let mut args = std::env::args_os().skip(1);
    let mut saved = Vec::new();
    while let Some(arg) = args.next() {
        if arg == "--update-ready" {
            args.next();
        } else {
            saved.push(arg);
        }
    }
    saved
}
#[derive(Serialize, Deserialize)]
pub struct Plan {
    pub target: PathBuf,
    pub args: Vec<OsString>,
    pub cwd: PathBuf,
    pub parent: u32,
    pub previous_hash: String,
    pub tag: String,
}

pub fn stage(target: &Path) -> Result<tempfile::TempDir> {
    tempfile::Builder::new()
        .prefix(PREFIX)
        .tempdir_in(target.parent().context("Нет каталога приложения")?)
        .context("Нет прав записи в каталог приложения. Переместите EXE в доступную папку")
}

fn read_plan(stage: &Path) -> Result<Plan> {
    let bytes = fs::read(stage.join("plan.json"))?;
    ensure!(bytes.len() <= 65536, "Слишком большой план обновления");
    Ok(serde_json::from_slice(&bytes)?)
}

fn validate_stage(stage: &Path, target: &Path) -> Result<()> {
    ensure!(
        stage
            .file_name()
            .and_then(|n| n.to_str())
            .is_some_and(|n| n.starts_with(PREFIX)),
        "Неизвестный каталог обновления"
    );
    ensure!(
        !fs::symlink_metadata(stage)?.file_type().is_symlink(),
        "Каталог обновления — ссылка"
    );
    ensure!(
        dunce::canonicalize(stage.parent().context("Нет каталога")?)?
            == dunce::canonicalize(target.parent().context("Нет каталога EXE")?)?,
        "Каталог обновления находится вне каталога приложения"
    );
    ensure!(
        target
            .file_name()
            .and_then(|n| n.to_str())
            .is_some_and(|n| n.to_ascii_lowercase().ends_with(".exe")),
        "Цель обновления не EXE"
    );
    Ok(())
}

fn verified_hash(stage: &Path, tag: &str) -> Result<String> {
    let sums = fs::read(stage.join(SUMS))?;
    let signature = fs::read_to_string(stage.join(SIGNATURE))?;
    ensure!(
        sums.len() <= 65536 && signature.len() <= 8192,
        "Превышен размер подписи или списка"
    );
    let hashes = release::verify_manifest(PUBLIC_KEY, tag, &sums, &signature)?;
    Ok(hashes[GUI].clone())
}

pub fn launch_helper(stage: &Path, plan: &Plan) -> Result<Child> {
    validate_stage(stage, &plan.target)?;
    let bytes = serde_json::to_vec(plan)?;
    let mut file = fs::File::create(stage.join("plan.json"))?;
    file.write_all(&bytes)?;
    file.sync_all()?;
    fs::copy(std::env::current_exe()?, stage.join("helper.exe"))?;
    let mut child = Command::new(stage.join("helper.exe"))
        .arg("--apply-update")
        .arg(stage)
        .creation_flags(0x08000000)
        .spawn()
        .context("Запуск помощника обновления")?;
    let deadline = Instant::now() + Duration::from_secs(15);
    while Instant::now() < deadline {
        if stage.join("helper-ready").exists() {
            return Ok(child);
        }
        if let Some(status) = child.try_wait()? {
            bail!(
                "Помощник обновления завершился ({status}): {}",
                fs::read_to_string(stage.join("error.txt")).unwrap_or_default()
            );
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    // Do not leave a helper which could install after the user continued working.
    let _ = child.kill();
    let _ = child.wait();
    bail!("Помощник обновления не готов; приложение продолжает работу")
}

struct Process(HANDLE);
impl Drop for Process {
    fn drop(&mut self) {
        unsafe {
            CloseHandle(self.0);
        }
    }
}

fn parent_process(plan: &Plan) -> Result<Process> {
    let handle = unsafe {
        OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE,
            0,
            plan.parent,
        )
    };
    ensure!(!handle.is_null(), "Не удалось открыть исходный процесс");
    let handle = Process(handle);
    let mut path = vec![0u16; 32768];
    let mut len = path.len() as u32;
    ensure!(
        unsafe { QueryFullProcessImageNameW(handle.0, 0, path.as_mut_ptr(), &mut len) } != 0,
        "Не удалось определить EXE исходного процесса"
    );
    let actual = PathBuf::from(String::from_utf16(&path[..len as usize])?);
    ensure!(
        dunce::canonicalize(actual)? == dunce::canonicalize(&plan.target)?,
        "Цель обновления не соответствует исходному процессу"
    );
    Ok(handle)
}

fn rename_retry(from: &Path, to: &Path) -> Result<()> {
    let deadline = Instant::now() + Duration::from_secs(10);
    loop {
        match fs::rename(from, to) {
            Ok(()) => return Ok(()),
            Err(e) if Instant::now() >= deadline => {
                return Err(e).context("Замена EXE; возможно, файл занят или недоступен");
            }
            Err(_) => std::thread::sleep(Duration::from_millis(100)),
        }
    }
}

// The backup remains on the same volume. Never discard it if rollback failed.
fn replace(target: &Path, source: &Path, backup: &Path) -> Result<()> {
    ensure!(!backup.exists(), "Резервная копия уже существует");
    rename_retry(target, backup)?;
    if let Err(error) = rename_retry(source, target) {
        rename_retry(backup, target)
            .context("Не удалось восстановить старый EXE; он сохранён в каталоге обновления")?;
        return Err(error);
    }
    Ok(())
}

fn start(plan: &Plan, stage: Option<&Path>) -> Result<Child> {
    let mut command = Command::new(&plan.target);
    command
        .args(&plan.args)
        .current_dir(&plan.cwd)
        .creation_flags(0x08000000);
    if let Some(stage) = stage {
        command.arg("--update-ready").arg(stage);
    }
    command.spawn().context("Перезапуск приложения")
}

pub fn apply(stage: &Path) -> Result<()> {
    let stage = dunce::canonicalize(stage)?;
    let plan = read_plan(&stage)?;
    validate_stage(&stage, &plan.target)?;
    ensure!(
        dunce::canonicalize(std::env::current_exe()?)? == stage.join("helper.exe"),
        "Помощник запущен из другого каталога"
    );
    ensure!(
        release::release_version(&plan.tag)? > semver::Version::parse(env!("CARGO_PKG_VERSION"))?,
        "Обновление не новее установленной версии"
    );
    let lock = fs::File::create(stage.join("helper.lock"))?;
    fs2::FileExt::try_lock_exclusive(&lock)?;
    let parent = parent_process(&plan)?;
    release::verify_file(&plan.target, &plan.previous_hash)?;
    let hash = verified_hash(&stage, &plan.tag)?;
    release::verify_file(&stage.join("new.exe"), &hash)?;
    fs::write(stage.join("helper-ready"), b"ready")?;
    ensure!(
        unsafe { WaitForSingleObject(parent.0, 60000) } == WAIT_OBJECT_0,
        "Исходное приложение не завершилось; обновление отменено"
    );
    // Check again after waiting: an external writer must not change either file.
    release::verify_file(&plan.target, &plan.previous_hash)?;
    release::verify_file(&stage.join("new.exe"), &hash)?;
    let backup = stage.join("previous.exe");
    if let Err(error) = replace(&plan.target, &stage.join("new.exe"), &backup) {
        if plan.target.exists() {
            let _ = start(&plan, None);
        }
        return Err(error);
    }
    let result = (|| -> Result<()> {
        let mut child = start(&plan, Some(&stage))?;
        let deadline = Instant::now() + Duration::from_secs(45);
        loop {
            if stage.join("app-ready").exists() {
                return Ok(());
            }
            if let Some(status) = child.try_wait()? {
                bail!("Новая версия завершилась при запуске: {status}");
            }
            if Instant::now() >= deadline {
                child
                    .kill()
                    .context("Не удалось остановить новую версию для восстановления")?;
                child.wait()?;
                bail!("Новая версия не подтвердила запуск");
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    })();
    if let Err(error) = result {
        rename_retry(&plan.target, &stage.join("failed.exe"))?;
        rename_retry(&backup, &plan.target)
            .context("Не удалось восстановить старую версию; сохранена previous.exe")?;
        let _ = start(&plan, None);
        return Err(error);
    }
    Ok(())
}

pub fn acknowledge(stage: &Path) -> Result<()> {
    let plan = read_plan(stage)?;
    validate_stage(stage, &plan.target)?;
    ensure!(
        dunce::canonicalize(std::env::current_exe()?)? == dunce::canonicalize(&plan.target)?,
        "Подтверждение от другого EXE"
    );
    ensure!(
        plan.tag == format!("v{}", env!("CARGO_PKG_VERSION")),
        "Номер запущенной версии отличается от подписанного релиза"
    );
    release::verify_file(&plan.target, &verified_hash(stage, &plan.tag)?)?;
    fs::write(stage.join("app-ready"), b"ready")?;
    Ok(())
}

pub fn cleanup(target: &Path) {
    let Some(parent) = target.parent() else {
        return;
    };
    let Ok(entries) = fs::read_dir(parent) else {
        return;
    };
    for entry in entries.flatten() {
        let stage = entry.path();
        // Only acknowledged installs for this exact EXE; retain failed updates as evidence/backup.
        if !stage.join("app-ready").is_file() || validate_stage(&stage, target).is_err() {
            continue;
        }
        let Ok(plan) = read_plan(&stage) else {
            continue;
        };
        if plan.target != target {
            continue;
        }
        let Ok(lock) = fs::OpenOptions::new()
            .read(true)
            .write(true)
            .open(stage.join("helper.lock"))
        else {
            continue;
        };
        if fs2::FileExt::try_lock_exclusive(&lock).is_err() {
            continue;
        }
        // A running helper keeps its image locked even after the advisory lock closes.
        if fs::remove_file(stage.join("helper.exe")).is_err() {
            continue;
        }
        drop(lock);
        let _ = fs::remove_dir_all(&stage);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn failed_swap_restores_original_and_keeps_payload() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("программа с пробелами.exe");
        let backup = dir.path().join("backup.exe");
        fs::write(&target, b"old").unwrap();
        // Missing source fails after the original was moved to backup.
        assert!(replace(&target, &dir.path().join("missing.exe"), &backup).is_err());
        assert_eq!(fs::read(&target).unwrap(), b"old");
        assert!(!backup.exists());
    }
    #[test]
    fn successful_swap_retains_original_backup() {
        let dir = tempfile::tempdir().unwrap();
        let target = dir.path().join("app.exe");
        let source = dir.path().join("new.exe");
        let backup = dir.path().join("old.exe");
        fs::write(&target, b"old").unwrap();
        fs::write(&source, b"new").unwrap();
        replace(&target, &source, &backup).unwrap();
        assert_eq!(fs::read(target).unwrap(), b"new");
        assert_eq!(fs::read(backup).unwrap(), b"old");
    }
}
