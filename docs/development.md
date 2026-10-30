# Разработка v8-chroma

## Структура

| Каталог | Назначение |
|---|---|
| `src/` | Rust core и CLI |
| `native/src/` | C++ движок, протокол и перехваты отрисовки |
| `native/tests/` | Собственные mock DLL и проверки с настоящим Cairo |
| `ui/src/` | Svelte интерфейс |
| `ui/src-tauri/` | Rust desktop shell |
| `assets/themes/` | Палитры |
| `assets/branding/` | SVG-мастера и экспорт логотипа |
| `tests/reference/` | Независимый Python-кодировщик протокола |
| `tests/golden/` | Эталонные протокольные данные |
| `scripts/` | Локальные проверки и экспорт иконок |

Rust workspace содержит `v8-chroma-core` и `v8-chroma-app`.
Имена исполняемых целей: `v8-chroma-cli` и `v8-chroma`.

## Инструменты

Rust 1.97.1 фиксируется в rust-toolchain.toml. Общие команды написаны на Python
(в CI 3.14.5), UI использует Node.js 24.13.0. C++ собирают CMake 4.4.4 и Ninja
1.13.2; достаточно CMake 3.25+ для самостоятельного запуска presets.
Cargo.lock и package-lock.json фиксируют зависимости приложения.

Полная сборка Windows x64 требует MSVC и Windows SDK. Запускайте команды из
x64 Developer Command Prompt/PowerShell. Нужные CMake/Ninja устанавливаются
`python -m pip install -r scripts/requirements-ci.txt`.
Для разработки допустим MinGW; Zig остаётся дополнительным CMake toolchain.
См. [native/README](../native/README.md).

## Проверки

Полный цикл Windows x64 из корня проекта:

```text
python scripts/build.py check
```

Вариант для установленного MinGW и Rust GNU toolchain:

```text
python scripts/build.py check --native-preset windows-mingw --toolchain stable-x86_64-pc-windows-gnu --target x86_64-pc-windows-gnu
```

g++, CMake и Ninja должны находиться в PATH. Для отдельной сборки C++ используйте
`python scripts/build.py native` с нужным `--native-preset`.

Переносимое ядро на текущей платформе:

```text
python scripts/build.py core
```

Команда выбирает Rust target хоста; его можно задать через `--target`.
Проверяются настройки, политика применения, палитры и протокол без feature
`windows-engine`. См. [границы переносимости](portability.md).

Полный цикл собирает DLL и собственные fixtures, устанавливает их для встраивания
и тестов, проверяет goldens, frontend, Rust tests, Clippy и Python gates выпуска.
Это локальные проверки разработки, а не выпуск релизных EXE.

Установленная 1С для mock-тестов не нужна. Дополнительные проверки с настоящим
Cairo включаются параметром `--cairo <platform-bin>/cairo.dll`.
Платформенные DLL не хранятся в репозитории и не входят в поставку.
Уведомления о лицензиях генерируются при выпуске; см.
[licenses/README](../licenses/README.md).

Созданные assets/ThemeProbe.dll, assets/FontProbe.dll, native/bin/, tests/fixtures/
и build/ игнорируются Git. Обе DLL встраиваются в EXE. Проверки шрифтов и лигатур
описаны в [fonts.md](fonts.md).

## Кэш CI

Полная Windows-задача сохраняет скомпилированные Rust-зависимости. Ключ учитывает
target, workflow, версию компилятора, манифесты, lock-файлы и флаги окружения.
Проверки и выпуск используют отдельные ключи; собственные пакеты workspace
собираются из исходников. Этот кэш записывается при push в main или по тегу,
в том числе при падении теста после компиляции зависимостей.

Core-задачи кэшируют только загруженные пакеты Cargo и получают зависимости
для `--no-default-features` во время сборки. Общий fetch desktop-workspace
в них не выполняется. npm и pip используют кэш загрузок по своим lock-файлам
и requirements. `npm ci`, тесты, Clippy и сборка выполняются при каждом прогоне.

## Собственные значки

Собственные значки встроены в основной движок. SVG-мастера находятся в
`assets/toolbar-icons/light/` и `dark/`; это единственный источник рисунков.
`scripts/icon_raster.py` растеризует их, `scripts/build_icon_assets.py` формирует
встроенный пакет по профилям отпечатков. Установленная 1С для сборки не нужна.
Устройство и проверки описаны в [custom-icons.md](custom-icons.md).

## Протокол

```powershell
python tests/generate_golden.py --check
```

Команда только проверяет эталоны. После осознанного изменения протокола или
профиля перегенерация выполняется без `--check`, а diff проверяется отдельно.
Кодировщик независим от Rust-реализации и служит эталоном формата протокола.

## Пользовательские данные и автозапуск

В Windows основной профиль хранится в `%APPDATA%\v8-chroma\config\settings.json`,
а runtime DLL и сведения о подключениях — в `%LOCALAPPDATA%\v8-chroma\data\`.
CLI и GUI используют один профиль; `--config-dir` задаёт отдельный каталог
настроек. Каталог настроек можно открыть кнопкой в интерфейсе.

Автозапуск включается пользователем в настройках GUI. Запись `v8-chroma` в разделе
`Software\Microsoft\Windows\CurrentVersion\Run` реестра Windows запускает текущий EXE
с аргументом `--background`. Для отдельного профиля автозапуск недоступен.
После переноса EXE в другой каталог запись нужно обновить выключением и повторным
включением автозапуска.

Для разработки используйте отдельный профиль через `--config-dir`, чтобы
проверки не меняли основные пользовательские настройки. Один конфигуратор
должен обслуживаться одним экземпляром менеджера тем.
