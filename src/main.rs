use anyhow::{Result, ensure};
use clap::{Parser, Subcommand, ValueEnum};
use std::{
    path::{Path, PathBuf},
    time::{Duration, Instant},
};
use v8_chroma_core::{
    manager,
    settings::{Loaded, SettingsPatch, Store, WindowPatch},
    theme,
    watch::Controller,
};

#[derive(Parser)]
#[command(
    version,
    about = "Менеджер тем конфигуратора 1С: Rust core с пользовательскими настройками"
)]
struct Cli {
    /// Отдельный каталог предпочтений; по умолчанию системный каталог текущего пользователя.
    #[arg(long, global = true)]
    config_dir: Option<PathBuf>,
    #[command(subcommand)]
    command: Option<Command>,
}
#[derive(Subcommand)]
enum Command {
    /// Найти 1cv8.exe и показать пригодность для подключения; процессы не изменяются.
    List,
    /// Список встроенных тем.
    Themes,
    /// Ручное подключение. Без --theme используется сохранённый пресет.
    Attach {
        #[arg(long)]
        pid: u32,
        #[arg(long)]
        theme: Option<String>,
    },
    /// Ручное применение ко всем подходящим процессам, независимо от auto_apply.
    ApplyAll {
        #[arg(long)]
        theme: Option<String>,
    },
    /// Наблюдение с сохранёнными настройками. --theme переопределяет только пресет на этот запуск.
    #[command(alias = "run")]
    Watch {
        #[arg(long)]
        theme: Option<String>,
        #[arg(long, default_value_t = 0)]
        seconds: u64,
    },
    /// Просмотр и сохранение предпочтений текущего пользователя.
    Settings {
        #[command(subcommand)]
        command: SettingsCommand,
    },
}
#[derive(Clone, Copy, ValueEnum)]
enum Switch {
    On,
    Off,
}
impl Switch {
    fn value(self) -> bool {
        matches!(self, Self::On)
    }
}
#[derive(Subcommand)]
enum SettingsCommand {
    Show,
    Set {
        #[arg(long)]
        theme: Option<String>,
        #[arg(long, value_enum)]
        auto_apply: Option<Switch>,
        #[arg(long, value_enum)]
        custom_icons: Option<Switch>,
        #[arg(long)]
        width: Option<u32>,
        #[arg(long)]
        height: Option<u32>,
        #[arg(long, value_enum)]
        maximized: Option<Switch>,
    },
    /// Вернуть defaults, сохранив предыдущий исправный файл/повреждённые данные.
    Reset,
}

fn print(value: &impl serde::Serialize) -> Result<()> {
    println!("{}", serde_json::to_string_pretty(value)?);
    Ok(())
}
fn show(loaded: Loaded) -> Result<()> {
    if let Some(warning) = &loaded.warning {
        eprintln!("Предупреждение: {warning}");
    }
    print(&loaded)
}
fn store(directory: Option<PathBuf>) -> Result<Store> {
    match directory {
        Some(path) => Store::at(path),
        None => Store::system(),
    }
}
fn selected(
    store: &Store,
    override_theme: Option<String>,
) -> Result<(String, bool, v8_chroma_core::fonts::FontSettings)> {
    let (id, icons, font) = if let Some(id) = override_theme {
        // Explicit manual theme remains usable even with broken preferences.
        (
            id,
            store
                .load()
                .map(|s| s.settings.custom_icons)
                .unwrap_or(false),
            store
                .load()
                .map(|s| s.settings.editor_font)
                .unwrap_or_default(),
        )
    } else {
        let loaded = store.load()?;
        if let Some(warning) = loaded.warning {
            eprintln!("Предупреждение: {warning}");
        }
        (
            loaded.settings.selected_theme,
            loaded.settings.custom_icons,
            loaded.settings.editor_font,
        )
    };
    theme::config(&id, Path::new("x"), Path::new("y"))?;
    Ok((id, icons, font))
}
fn main() -> std::process::ExitCode {
    match run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("Ошибка: {error:#}");
            std::process::ExitCode::FAILURE
        }
    }
}
fn run() -> Result<()> {
    let cli = Cli::parse();
    // Read-only process/theme listing doesn't depend on a preferences file or its validity.
    match cli.command {
        Some(Command::List) => print(&manager::discover()?)?,
        Some(Command::Themes) => print(&theme::IDS)?,
        Some(Command::Attach { pid, theme }) => {
            let (id, icons, font) = selected(&store(cli.config_dir)?, theme)?;
            let applied = manager::attach_with_icons(pid, &id, icons)?;
            let mut result = serde_json::to_value(&applied)?;
            result["font"] =
                serde_json::to_value(v8_chroma_core::fonts::apply_to(&applied.identity, &font)?)?;
            print(&result)?;
        }
        Some(Command::ApplyAll { theme }) => {
            let (id, icons, font) = selected(&store(cli.config_dir)?, theme)?;
            let mut failed = false;
            for candidate in manager::discover()?.into_iter().filter(|c| c.eligible) {
                match manager::attach_with_icons(candidate.pid, &id, icons) {
                    Ok(result) => {
                        let mut value = serde_json::to_value(&result)?;
                        match v8_chroma_core::fonts::apply_to(&result.identity, &font) {
                            Ok(font) => value["font"] = serde_json::to_value(font)?,
                            Err(error) => {
                                failed = true;
                                value["font_error"] = serde_json::json!(format!("{error:#}"));
                            }
                        }
                        print(&value)?;
                    }
                    Err(error) => {
                        failed = true;
                        print(
                            &serde_json::json!({"pid":candidate.pid,"error":format!("{error:#}")}),
                        )?;
                    }
                }
            }
            ensure!(
                !failed,
                "Некоторые процессы не удалось оформить; подробности выше"
            );
        }
        Some(Command::Settings { command }) => {
            let store = store(cli.config_dir)?;
            match command {
                SettingsCommand::Show => show(store.load()?)?,
                SettingsCommand::Reset => show(store.reset()?)?,
                SettingsCommand::Set {
                    theme,
                    auto_apply,
                    custom_icons,
                    width,
                    height,
                    maximized,
                } => {
                    ensure!(
                        theme.is_some()
                            || auto_apply.is_some()
                            || custom_icons.is_some()
                            || width.is_some()
                            || height.is_some()
                            || maximized.is_some(),
                        "Укажите хотя бы один изменяемый параметр"
                    );
                    let window = if width.is_some() || height.is_some() || maximized.is_some() {
                        Some(WindowPatch {
                            width,
                            height,
                            maximized: maximized.map(Switch::value),
                        })
                    } else {
                        None
                    };
                    show(store.update(SettingsPatch {
                        selected_theme: theme,
                        auto_apply: auto_apply.map(Switch::value),
                        custom_icons: custom_icons.map(Switch::value),
                        window,
                        ..Default::default()
                    })?)?;
                }
            }
        }
        Some(Command::Watch { theme, seconds }) => watch(&store(cli.config_dir)?, theme, seconds)?,
        None => watch(&store(cli.config_dir)?, None, 0)?,
    }
    Ok(())
}

fn watch(store: &Store, override_theme: Option<String>, seconds: u64) -> Result<()> {
    if let Some(id) = &override_theme {
        theme::config(id, Path::new("x"), Path::new("y"))?;
    }
    store.ensure_initialized()?;
    let mut controller = Controller::default();
    let started = Instant::now();
    let mut last_policy = None;
    let mut last_message = None;
    loop {
        match store.load() {
            Ok(loaded) => {
                let message = loaded.warning.clone();
                if message != last_message {
                    if let Some(warning) = &message {
                        eprintln!("Предупреждение: {warning}");
                    }
                    last_message = message;
                }
                let mut settings = loaded.settings;
                if let Some(id) = &override_theme {
                    settings.selected_theme = id.clone();
                }
                let policy = (
                    settings.selected_theme.clone(),
                    settings.auto_apply,
                    settings.custom_icons,
                );
                if last_policy.as_ref() != Some(&policy) {
                    print(
                        &serde_json::json!({"event":"settings","theme":policy.0,"auto_apply":policy.1,"custom_icons":policy.2,"path":loaded.path}),
                    )?;
                    last_policy = Some(policy);
                }
                if !settings.auto_apply {
                    controller.pause();
                } else {
                    match manager::discover() {
                        Ok(candidates) => {
                            let identities: Vec<_> = candidates
                                .into_iter()
                                .filter(|c| c.eligible)
                                .filter_map(|c| c.identity)
                                .collect();
                            for identity in controller.plan(&identities, &settings) {
                                match manager::attach_to_with_icons(
                                    &identity,
                                    &settings.selected_theme,
                                    settings.custom_icons,
                                ) {
                                    Ok(result) => {
                                        print(&result)?;
                                        match v8_chroma_core::fonts::apply_to(
                                            &identity,
                                            &settings.editor_font,
                                        ) {
                                            Ok(font) => print(
                                                &serde_json::json!({"pid":identity.pid,"font":font}),
                                            )?,
                                            Err(error) => print(
                                                &serde_json::json!({"pid":identity.pid,"font_error":format!("{error:#}")}),
                                            )?,
                                        }
                                    }
                                    Err(error) => print(
                                        &serde_json::json!({"pid":identity.pid,"error":format!("{error:#}")}),
                                    )?,
                                }
                            }
                        }
                        Err(error) => {
                            controller.pause();
                            eprintln!("Наблюдение приостановлено: {error:#}");
                        }
                    }
                }
            }
            Err(error) => {
                controller.pause();
                last_policy = None;
                let message = Some(format!(
                    "Автоматическое применение приостановлено: {error:#}"
                ));
                if message != last_message {
                    eprintln!("{}", message.as_deref().unwrap_or_default());
                    last_message = message;
                }
            }
        }
        if seconds > 0 && started.elapsed() >= Duration::from_secs(seconds) {
            break;
        }
        std::thread::sleep(Duration::from_secs(2));
    }
    Ok(())
}
