param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    # Which app to launch (exe name without .exe): PBRApp, FluidApp, ToonApp, WaveApp, RayMarchApp, ...
    [string]$App = "PBRApp",
    [switch]$Headless
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# A Windows process started directly through WSL interop is terminated together with the
# interop command tree. Win32_Process.Create asks the Windows CIM provider to create the
# launcher outside that tree, so the app remains alive after this script and powershell.exe exit.
#
# The launcher console is created HIDDEN (ShowWindow = SW_HIDE) and the app is started with
# "start /b" so it shares that hidden console. Without this, every launch opened two console
# windows (cmd + the app's own console) that Windows Terminal, as the default terminal, keeps
# open and empty after the app exits.
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$exePath = Join-Path $repoRoot ("Build\bin\x64\{0}\{1}.exe" -f $Configuration, $App)
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "Executable not found (build it first): $exePath"
}

$envs = "set SASAMI_DEBUG_REMOTE=1&&"
if ($Headless) { $envs += " set SASAMI_HEADLESS=1&&" }
$commandLine = 'cmd.exe /d /c "{0} start "" /b "{1}""' -f $envs, $exePath
$startup = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly -Property @{ ShowWindow = [uint16]0 }
$result = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
    CommandLine = $commandLine
    CurrentDirectory = $repoRoot
    ProcessStartupInformation = $startup
}

if ($null -eq $result -or $result.ReturnValue -ne 0) {
    $returnValue = if ($null -eq $result) { "no result" } else { $result.ReturnValue }
    throw "Win32_Process.Create failed: $returnValue"
}

$headlessValue = if ($Headless) { 1 } else { 0 }
Write-Output "OK launch requested ($App, $Configuration, headless=$headlessValue)"
