param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [int]$StartupTimeoutSeconds = 120,
    [int]$CaptureTimeoutSeconds = 30,
    [switch]$SkipScreenshot,
    [switch]$RequireRunning,
    [switch]$KeepRunning
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\.."))
$pipeScript = Join-Path $PSScriptRoot "pipe.ps1"
$screenshotScript = Join-Path $PSScriptRoot "screenshot.ps1"
$exePath = Join-Path $repoRoot "Build\bin\x64\$Configuration\PBRApp.exe"
$logPath = "$exePath.log"
$launchedProcess = $null
$usingExistingInstance = $false
$logStartOffset = 0L

function Invoke-CliRaw([string]$ScriptText) {
    $lines = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $pipeScript -Script $ScriptText 2>&1 |
        ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    $responses = @($lines | Where-Object { $_.StartsWith("< ") } |
        ForEach-Object { $_.Substring(2) })
    return [pscustomobject]@{
        ExitCode = $exitCode
        Lines = $lines
        Responses = $responses
    }
}

function Invoke-CliChecked([string]$ScriptText) {
    $result = Invoke-CliRaw $ScriptText
    foreach ($line in $result.Lines) {
        Write-Host $line
    }
    if ($result.ExitCode -ne 0) {
        throw "CLI command failed: $ScriptText"
    }
    return $result.Responses
}

# A command can reach the pipe while the main thread is inside a long startup frame and
# receive the server's ERR timeout even though the renderer becomes responsive immediately
# afterwards. Retrying is safe here because this helper is only used for read-only queries.
# Do not use it for setters, scene.save/load, or screenshot requests.
function Invoke-ReadOnlyCliChecked([string]$Command, [int]$Attempts = 12) {
    for ($attempt = 1; $attempt -le $Attempts; ++$attempt) {
        $result = Invoke-CliRaw $Command
        foreach ($line in $result.Lines) {
            Write-Host $line
        }
        if ($result.ExitCode -eq 0 -and $result.Responses.Count -eq 1 -and
            $result.Responses[0] -ne "ERR timeout") {
            return $result.Responses[0]
        }
        if ($attempt -lt $Attempts) {
            Write-Host "Read-only CLI query not ready; retrying ($attempt/$Attempts)..."
            Start-Sleep -Milliseconds 500
        }
    }
    throw "Read-only CLI query failed after $Attempts attempts: $Command"
}

function Test-CliReady {
    $result = Invoke-CliRaw "ping"
    return $result.ExitCode -eq 0 -and
        $result.Responses.Count -eq 1 -and
        $result.Responses[0] -eq "pong"
}

function Read-AppendedLog([string]$Path, [long]$Offset) {
    if (-not (Test-Path -LiteralPath $Path)) {
        return ""
    }

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open,
        [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    try {
        if ($stream.Length -lt $Offset) {
            $Offset = 0
        }
        [void]$stream.Seek($Offset, [System.IO.SeekOrigin]::Begin)
        $reader = New-Object System.IO.StreamReader($stream)
        try {
            return $reader.ReadToEnd()
        } finally {
            $reader.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Stop-LaunchedProcess {
    if ($null -eq $script:launchedProcess) {
        return
    }

    $script:launchedProcess.Refresh()
    if ($script:launchedProcess.HasExited) {
        return
    }

    Write-Host "Stopping test-owned PBRApp (PID=$($script:launchedProcess.Id))..."
    [void]$script:launchedProcess.CloseMainWindow()
    if (-not $script:launchedProcess.WaitForExit(5000)) {
        Stop-Process -Id $script:launchedProcess.Id
        [void]$script:launchedProcess.WaitForExit(5000)
    }
}

try {
    if (-not (Test-Path -LiteralPath $pipeScript)) {
        throw "CLI client not found: $pipeScript"
    }

    $cliReady = Test-CliReady
    $runningPbrApps = @(Get-Process -Name "PBRApp" -ErrorAction SilentlyContinue)
    if (-not $cliReady -and $runningPbrApps.Count -gt 0) {
        try {
            $cliReady = (Invoke-ReadOnlyCliChecked "ping") -eq "pong"
        } catch {
            $cliReady = $false
        }
    }

    if ($cliReady) {
        $usingExistingInstance = $true
        Write-Host "Using an existing CLI-enabled PBRApp. It will not be stopped."
    } else {
        if ($RequireRunning) {
            throw "No CLI-enabled PBRApp is responding."
        }
        if ($runningPbrApps.Count -gt 0) {
            $ids = ($runningPbrApps | ForEach-Object { $_.Id }) -join ","
            throw "PBRApp is already running without a responsive debug CLI (PID=$ids). Refusing to start a competing instance."
        }
        if (-not (Test-Path -LiteralPath $exePath)) {
            throw "Executable not found: $exePath. Build the $Configuration PBRApp first."
        }

        if (Test-Path -LiteralPath $logPath) {
            $logStartOffset = (Get-Item -LiteralPath $logPath).Length
        }

        $remoteWasSet = Test-Path Env:SASAMI_DEBUG_REMOTE
        $headlessWasSet = Test-Path Env:SASAMI_HEADLESS
        $oldRemote = $env:SASAMI_DEBUG_REMOTE
        $oldHeadless = $env:SASAMI_HEADLESS
        try {
            $env:SASAMI_DEBUG_REMOTE = "1"
            $env:SASAMI_HEADLESS = "1"
            $launchedProcess = Start-Process -FilePath $exePath -WorkingDirectory $repoRoot -PassThru
        } finally {
            if ($remoteWasSet) { $env:SASAMI_DEBUG_REMOTE = $oldRemote } else { Remove-Item Env:SASAMI_DEBUG_REMOTE }
            if ($headlessWasSet) { $env:SASAMI_HEADLESS = $oldHeadless } else { Remove-Item Env:SASAMI_HEADLESS }
        }

        Write-Host "Started headless $Configuration PBRApp (PID=$($launchedProcess.Id))."
        $deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
        $ready = $false
        while ((Get-Date) -lt $deadline) {
            $launchedProcess.Refresh()
            if ($launchedProcess.HasExited) {
                throw "PBRApp exited before the CLI became ready (exitCode=$($launchedProcess.ExitCode))."
            }
            if (Test-CliReady) {
                $ready = $true
                break
            }
            Start-Sleep -Milliseconds 250
        }
        if (-not $ready) {
            throw "CLI did not become ready within $StartupTimeoutSeconds seconds."
        }
    }

    Write-Host "Running read-only CLI checks..."
    $responses = @(
        Invoke-ReadOnlyCliChecked "ping"
        Invoke-ReadOnlyCliChecked "render.get"
        Invoke-ReadOnlyCliChecked "camera.get"
        Invoke-ReadOnlyCliChecked "light.get"
        Invoke-ReadOnlyCliChecked "light.spot.list"
        Invoke-ReadOnlyCliChecked "render.gbuffer list"
    )
    if ($responses[0] -ne "pong") {
        throw "Unexpected ping response: $($responses[0])"
    }
    foreach ($index in 1..5) {
        if (-not $responses[$index].StartsWith("OK")) {
            throw "CLI state check failed: $($responses[$index])"
        }
    }

    Write-Host "Waiting for GPU timestamp data..."
    $gpuDeadline = (Get-Date).AddSeconds(15)
    $gpuResponse = "PENDING"
    while ((Get-Date) -lt $gpuDeadline) {
        $gpuResponse = Invoke-ReadOnlyCliChecked "render.gputime"
        if ($gpuResponse.StartsWith("OK")) {
            break
        }
        if ($gpuResponse.StartsWith("ERR")) {
            throw "GPU timing check failed: $gpuResponse"
        }
        if (-not $gpuResponse.StartsWith("PENDING")) {
            throw "Unexpected GPU timing response: $gpuResponse"
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $gpuResponse.StartsWith("OK")) {
        throw "GPU timing stayed pending for 15 seconds."
    }

    $capturePath = ""
    if (-not $SkipScreenshot) {
        $timestamp = Get-Date -Format "yyyyMMdd-HHmmss"
        $capturePath = Join-Path $PSScriptRoot "Screenshots\smoke\cli-smoke-$timestamp.png"
        Write-Host "Capturing the rendered back buffer..."
        $captureOutput = @(& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $screenshotScript `
            -Path $capturePath -TimeoutSeconds $CaptureTimeoutSeconds 2>&1 |
            ForEach-Object { $_.ToString() })
        $captureExitCode = $LASTEXITCODE
        foreach ($line in $captureOutput) {
            Write-Host $line
        }
        if ($captureExitCode -ne 0) {
            throw "Screenshot CLI failed."
        }
        if (-not (Test-Path -LiteralPath $capturePath)) {
            throw "Screenshot was not written: $capturePath"
        }

        $captureInfo = Get-Item -LiteralPath $capturePath
        if ($captureInfo.Length -le 8) {
            throw "Screenshot is empty: $capturePath"
        }
        $stream = [System.IO.File]::OpenRead($capturePath)
        try {
            $signature = New-Object byte[] 8
            if ($stream.Read($signature, 0, 8) -ne 8 -or
                [System.BitConverter]::ToString($signature) -ne "89-50-4E-47-0D-0A-1A-0A") {
                throw "Screenshot is not a valid PNG: $capturePath"
            }
        } finally {
            $stream.Dispose()
        }
    }

    if ($null -ne $launchedProcess -and $Configuration -eq "Debug") {
        $newLog = Read-AppendedLog $logPath $logStartOffset
        # DebugLogSystem may truncate/rotate the active log on launch. If the new file grows
        # past the old byte offset during this run, a length-only check cannot notice that it
        # is a different stream and would skip the two self-test lines at the new file's start.
        if ($newLog -notmatch "\[JobSystemSelfTest\]") {
            $newLog = Read-AppendedLog $logPath 0
        }
        if ($newLog -match "\[JobSystemSelfTest\].*FAIL") {
            throw "JobSystem self-test reported FAIL in this run."
        }
        if ($newLog -notmatch "\[JobSystemSelfTest\] FanOutFanIn: PASS" -or
            $newLog -notmatch "\[JobSystemSelfTest\] ParallelFor: PASS") {
            throw "Debug JobSystem self-test PASS lines were not found in this run."
        }
    }

    $instanceLabel = if ($usingExistingInstance) { "existing instance" } else { "test-owned instance" }
    Write-Output "CLI_SMOKE_TEST PASS ($Configuration, $instanceLabel)"
    if ($capturePath) {
        Write-Output "SCREENSHOT $capturePath"
    }
} catch {
    Write-Error $_
    exit 1
} finally {
    if (-not $KeepRunning) {
        Stop-LaunchedProcess
    } elseif ($null -ne $launchedProcess) {
        Write-Host "Keeping test-owned PBRApp running (PID=$($launchedProcess.Id))."
    }
}

exit 0
