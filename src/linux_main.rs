#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
mod cli {
    use anyhow::Result;
    use clap::{Parser, Subcommand};
    use std::{ffi::OsString, path::PathBuf};
    use v8_chroma_core::{linux::Manager, theme};

    #[derive(Parser)]
    #[command(
        version,
        about = "Экспериментальный Linux x64 backend v8-chroma: темы Cairo, без значков и шрифтов"
    )]
    struct Cli {
        #[arg(long, global = true)]
        data_dir: Option<PathBuf>,
        #[command(subcommand)]
        command: Action,
    }
    #[derive(Subcommand)]
    enum Action {
        Themes,
        List,
        /// Подключиться к уже открытому Конфигуратору текущего пользователя.
        Attach {
            #[arg(long)]
            pid: u32,
            #[arg(long, default_value = "dracula")]
            theme: String,
            #[arg(long)]
            engine: Option<PathBuf>,
            #[arg(long)]
            injector: Option<PathBuf>,
        },
        /// Запустить новый Конфигуратор с движком. После -- передаются параметры 1С.
        Launch {
            #[arg(long)]
            platform: PathBuf,
            #[arg(long)]
            engine: Option<PathBuf>,
            #[arg(long, default_value = "dracula")]
            theme: String,
            #[arg(last = true)]
            arguments: Vec<OsString>,
        },
        /// Смена палитры при следующей отрисовке. standard возвращает штатные цвета.
        Apply {
            #[arg(long)]
            pid: u32,
            #[arg(long)]
            theme: String,
        },
        Status {
            #[arg(long)]
            pid: u32,
        },
    }
    fn print(value: &impl serde::Serialize) -> Result<()> {
        println!("{}", serde_json::to_string_pretty(value)?);
        Ok(())
    }
    pub fn run() -> Result<()> {
        let cli = Cli::parse();
        if matches!(cli.command, Action::Themes) {
            return print(&theme::IDS);
        }
        let manager = Manager::new(cli.data_dir)?;
        match cli.command {
            Action::Themes => unreachable!(),
            Action::List => print(&manager.discover()?),
            Action::Attach {
                pid,
                theme,
                engine,
                injector,
            } => {
                let executable = std::env::current_exe()?;
                print(&manager.attach(
                    pid,
                    &theme,
                    &engine.unwrap_or(executable.with_file_name("libv8-chroma-linux.so")),
                    &injector.unwrap_or(executable.with_file_name("v8-chroma-injector")),
                )?)
            }
            Action::Launch {
                platform,
                engine,
                theme,
                arguments,
            } => {
                let engine = engine
                    .unwrap_or(std::env::current_exe()?.with_file_name("libv8-chroma-linux.so"));
                print(&manager.launch(&platform, &engine, &theme, &arguments)?)
            }
            Action::Apply { pid, theme } => print(&manager.apply(pid, &theme)?),
            Action::Status { pid } => print(&manager.status(pid)?),
        }
    }
}
fn main() -> std::process::ExitCode {
    #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
    match cli::run() {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("Ошибка: {error:#}");
            std::process::ExitCode::FAILURE
        }
    }
    #[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
    {
        eprintln!("Этот backend поддерживает только Linux x64");
        std::process::ExitCode::FAILURE
    }
}
