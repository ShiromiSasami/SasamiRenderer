@echo off
REM Launches the RELEASE PBRApp with the debug pipe enabled (perf measurement). Pass 1 as the first argument for headless.
REM
REM The repo root is derived from this script's own location (Tools\Debug\ -> two levels up)
REM rather than hardcoded, so a clone in a different directory still works.
REM
REM NOTE: launching this from WSL via interop lets the app be torn down when the launching
REM shell exits -- "start" does not detach it from that teardown. From WSL, use:
REM   powershell.exe -NoProfile -Command "Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{ CommandLine = 'cmd.exe /c <repo>\Tools\Debug\run-debug.bat' }"
setlocal
for %%I in ("%~dp0..\..") do set "REPO_ROOT=%%~fI"
cd /d "%REPO_ROOT%"
set SASAMI_DEBUG_REMOTE=1
if not "%~1"=="" set SASAMI_HEADLESS=%~1
start "SasamiRenderer" "Build\bin\x64\Release\PBRApp.exe"
