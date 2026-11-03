param(
    [string]$Distribution = 'Ubuntu-24.04',
    [string]$LinuxExecutable,
    [switch]$Desktop
)
$ErrorActionPreference = 'Stop'

if (-not $LinuxExecutable) {
    $dataHome = (& wsl.exe -d $Distribution --exec printenv XDG_DATA_HOME | Out-String).Trim()
    if (-not $dataHome) {
        $linuxHome = (& wsl.exe -d $Distribution --exec printenv HOME | Out-String).Trim()
        if (-not $linuxHome) { throw 'Cannot determine the Linux home directory' }
        $dataHome = "$linuxHome/.local/share"
    }
    $LinuxExecutable = "$dataHome/v8-chroma/app/current/v8-chroma"
}
& wsl.exe -d $Distribution --exec test -x $LinuxExecutable
if ($LASTEXITCODE -ne 0) { throw 'Install the Linux application with scripts/install_linux.py first' }
if ($Distribution -notmatch '^[\p{L}\p{N}._-]+$' -or $Distribution.StartsWith('-')) {
    throw 'WSLg requires a distribution name without whitespace, quotes or command delimiters'
}
if (-not $LinuxExecutable.StartsWith('/') -or $LinuxExecutable -match '["''\x00-\x1F\x7F]') {
    throw 'The installed executable must be an absolute Linux path without quotes or control characters'
}
$launcher = (Get-Command wslg.exe -ErrorAction SilentlyContinue).Source
if (-not $launcher) { $launcher = Join-Path $env:ProgramFiles 'WSL/wslg.exe' }
if (-not (Test-Path -LiteralPath $launcher)) { throw 'WSLg launcher was not found' }

$iconDirectory = Join-Path $env:LOCALAPPDATA 'v8-chroma/linux-launcher'
New-Item -ItemType Directory -Path $iconDirectory -Force | Out-Null
$icon = Join-Path $iconDirectory 'icon.ico'
Copy-Item -LiteralPath (Join-Path $PSScriptRoot '../ui/src-tauri/icons/icon.ico') -Destination $icon -Force
$menu = Join-Path ([Environment]::GetFolderPath('StartMenu')) 'Programs'
$name = "V8 Chroma ($Distribution)"
foreach ($invalid in [IO.Path]::GetInvalidFileNameChars()) { $name = $name.Replace($invalid, '_') }
$path = Join-Path $menu "$name.lnk"
# Use WSLg's direct GUI entry point, without a console launcher.
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($path)
$shortcut.TargetPath = $launcher
# WSLg treats the command suffix as a Linux shell command. Single quotes keep
# spaces, dollar signs, backticks and backslashes literal. Its -d parser rejects
# quoted distribution names, so that token is validated separately above.
$shortcut.Arguments = "-d $Distribution --cd ~ -- '$LinuxExecutable'"
$shortcut.WorkingDirectory = $env:USERPROFILE
$shortcut.Description = 'V8 Chroma - Linux/WSLg'
$shortcut.IconLocation = "$icon,0"
$shortcut.Save()
Write-Output $path
if ($Desktop) {
    $desktopPath = Join-Path ([Environment]::GetFolderPath('DesktopDirectory')) "$name.lnk"
    Copy-Item -LiteralPath $path -Destination $desktopPath -Force
    Write-Output $desktopPath
}
