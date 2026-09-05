@echo off
setlocal EnableExtensions
title Export Falcon 4.0 game data

REM ============================================================================
REM export-game-data.bat
REM
REM Double-clickable wrapper around scripts\export-game-data.sh — prompts for
REM the Falcon 4.0 install directory (offering an auto-detected default), then
REM runs the full converter pipeline into the repo's Data\ tree. No command
REM line knowledge required.
REM
REM Power users can skip the prompt:  export-game-data.bat "D:\path\to\Falcon 4.0"
REM Non-default build dir: set F4_BUILD=<dir> before running (the export
REM script also honors it).
REM ============================================================================

set "REPO=%~dp0"

REM ── Locate Git Bash (the export script is bash) ─────────────────────────────
REM Registry first (Git for Windows records its root), then the usual
REM install dirs. Deliberately NOT using `where bash`: on some machines that
REM finds the WSL stub in System32, which cannot run this script.
set "BASH_EXE="
for %%K in ("HKLM\SOFTWARE\GitForWindows" "HKCU\SOFTWARE\GitForWindows") do (
    if not defined BASH_EXE (
        for /f "tokens=2*" %%A in ('reg query %%K /v InstallPath 2^>nul') do (
            if exist "%%B\bin\bash.exe" set "BASH_EXE=%%B\bin\bash.exe"
        )
    )
)
if not defined BASH_EXE if exist "%ProgramFiles%\Git\bin\bash.exe" set "BASH_EXE=%ProgramFiles%\Git\bin\bash.exe"
if not defined BASH_EXE if exist "%ProgramFiles(x86)%\Git\bin\bash.exe" set "BASH_EXE=%ProgramFiles(x86)%\Git\bin\bash.exe"
if not defined BASH_EXE if exist "%LocalAppData%\Programs\Git\bin\bash.exe" set "BASH_EXE=%LocalAppData%\Programs\Git\bin\bash.exe"
if not defined BASH_EXE (
    echo ERROR: Git Bash ^(bash.exe^) not found.
    echo        Install Git for Windows, then run this again.
    pause
    exit /b 1
)

REM ── Auto-detect a default Falcon 4.0 install to offer ───────────────────────
REM Steam's registry path first, then the common SteamLibrary locations.
set "DEFAULT_INSTALL="
for /f "tokens=2*" %%A in ('reg query "HKCU\Software\Valve\Steam" /v SteamPath 2^>nul') do set "STEAMROOT=%%B"
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\WOW6432Node\Valve\Steam" /v InstallPath 2^>nul') do if not defined STEAMROOT set "STEAMROOT=%%B"
if defined STEAMROOT if exist "%STEAMROOT%\steamapps\common\Falcon 4.0" set "DEFAULT_INSTALL=%STEAMROOT%\steamapps\common\Falcon 4.0"
for %%D in (C D E F G) do if not defined DEFAULT_INSTALL if exist "%%D:\SteamLibrary\steamapps\common\Falcon 4.0" set "DEFAULT_INSTALL=%%D:\SteamLibrary\steamapps\common\Falcon 4.0"
if not defined DEFAULT_INSTALL if exist "%ProgramFiles(x86)%\Steam\steamapps\common\Falcon 4.0" set "DEFAULT_INSTALL=%ProgramFiles(x86)%\Steam\steamapps\common\Falcon 4.0"

REM ── Prompt (skipped when the install dir is passed as %1) ───────────────────
set "INSTALL_DIR=%~1"
if defined INSTALL_DIR goto validate

echo.
echo   Falcon 4.0 data export
echo   ----------------------
echo   Converts the install's campaign, theater, class table, aircraft and
echo   SimData files into the repo's Data\ folder (JSON only, no binaries).
echo.
if defined DEFAULT_INSTALL (
    set /p "INSTALL_DIR=Install directory [Enter = %DEFAULT_INSTALL%]: "
) else (
    set /p "INSTALL_DIR=Install directory (e.g. D:\SteamLibrary\steamapps\common\Falcon 4.0): "
)
if not defined INSTALL_DIR (
    if defined DEFAULT_INSTALL ( set "INSTALL_DIR=%DEFAULT_INSTALL%" ) else (
        echo   A install directory is required.
        goto prompt-again
    )
)
REM Strip any surrounding quotes the user may have pasted.
set "INSTALL_DIR=%INSTALL_DIR:"=%"

:prompt-again
if defined INSTALL_DIR goto validate
set /p "INSTALL_DIR=Install directory: "
if not defined INSTALL_DIR goto prompt-again
set "INSTALL_DIR=%INSTALL_DIR:"=%"

:validate
if exist "%INSTALL_DIR%\" goto run
echo.
echo   ERROR: not a directory: %INSTALL_DIR%
set "INSTALL_DIR="
goto prompt-again

REM ── Run the export pipeline ─────────────────────────────────────────────────
:run
set "BUILD_DIR=%REPO%build"
if defined F4_BUILD set "BUILD_DIR=%F4_BUILD%"
echo.
echo   Repo:    %REPO%
echo   Bash:    %BASH_EXE%
echo   Install: %INSTALL_DIR%
echo   Build:   %BUILD_DIR%
echo.
pushd "%REPO%"
"%BASH_EXE%" "scripts/export-game-data.sh" --install "%INSTALL_DIR%"
set "RC=%ERRORLEVEL%"
popd

echo.
if "%RC%"=="0" (
    echo   Export complete. Data\ is ready to commit.
) else (
    echo   Export FAILED ^(exit code %RC%^). See messages above.
    echo   If converters are missing, configure + build the tree first:
    echo       cmake -B build ^&^& cmake --build build --config Debug
)
pause
exit /b %RC%
