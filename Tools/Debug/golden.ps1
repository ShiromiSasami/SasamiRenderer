# Golden-image regression gate: boots PBRApp headless, drives fixed camera/render views through
# the debug pipe, and either records baselines (capture) or compares against them (check).
#   .\golden.ps1 -Mode capture
#   .\golden.ps1 -Mode check [-Views bistro_street,bistro_street_ibl]
# Exit codes: 0 = all passed, 1 = a view FAILED or has no baseline, 3 = PBRApp already running,
# 4 = boot timeout or pipe failure.
param(
    [ValidateSet('capture', 'check')]
    [string]$Mode = 'check',
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',
    [string]$Views = '',
    [string]$BaselineDir = '',
    [string]$OutDir = '',
    [int]$BootTimeoutSeconds = 360,
    [switch]$KeepRunning
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$exePath = Join-Path $repoRoot "Build\bin\x64\$Configuration\PBRApp.exe"
$imgdiffScript = Join-Path $PSScriptRoot 'imgdiff.ps1'
$viewsPath = Join-Path $PSScriptRoot 'golden-views.json'
if ([string]::IsNullOrWhiteSpace($BaselineDir)) { $BaselineDir = Join-Path $PSScriptRoot "Golden\$Configuration" }
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $PSScriptRoot ("Screenshots\golden\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}

$launchedProcess = $null
$pipe = $null
$writer = $null
$reader = $null

# One line in, one line out. A single persistent connection keeps the server's per-connection
# state stable and avoids reconnect races while the main thread is inside a long frame.
function Send-Cmd([string]$Text) {
    $script:writer.WriteLine($Text)
    $reply = $script:reader.ReadLine()
    if ($null -eq $reply) { throw "Pipe closed while sending: $Text" }
    return $reply
}

function Get-Frame {
    $reply = Send-Cmd 'app.frame'
    if ($reply -notmatch '^OK frame=(\d+)') { throw "Unexpected app.frame reply: $reply" }
    return [long]$Matches[1]
}

function Wait-Frames([long]$Count, [int]$TimeoutSeconds = 120) {
    $start = Get-Frame
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Frame) -lt ($start + $Count)) {
        if ((Get-Date) -gt $deadline) { throw "Timed out waiting for $Count frames" }
        Start-Sleep -Milliseconds 200
    }
}

function Save-Screenshot([string]$Path, [int]$TimeoutSeconds = 30) {
    $reply = Send-Cmd "debug.screenshot $Path"
    if (-not $reply.StartsWith('OK')) { throw "debug.screenshot rejected: $reply" }
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $status = 'PENDING'
    while ($status -eq 'PENDING') {
        if ((Get-Date) -gt $deadline) { throw "Screenshot did not complete within ${TimeoutSeconds}s: $Path" }
        Start-Sleep -Milliseconds 100
        $status = Send-Cmd 'debug.screenshot.status'
    }
    if ($status.StartsWith('ERR')) { throw "Screenshot failed: $status" }
    if (-not (Test-Path -LiteralPath $Path)) { throw "Screenshot was not written: $Path" }
}

function Invoke-ImgDiff([string]$A, [string]$B, [double]$MaxPct, [int]$MaxAbsValue) {
    $lines = @(& $imgdiffScript -A $A -B $B -MaxChangedOver8Pct $MaxPct -MaxAbs $MaxAbsValue 2>&1 |
        ForEach-Object { $_.ToString() })
    return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Text = ($lines -join ' | ') }
}

function Stop-LaunchedProcess {
    if ($null -eq $script:launchedProcess) { return }
    $script:launchedProcess.Refresh()
    if ($script:launchedProcess.HasExited) { return }
    Write-Host "Stopping golden-owned PBRApp (PID=$($script:launchedProcess.Id))..."
    [void]$script:launchedProcess.CloseMainWindow()
    if (-not $script:launchedProcess.WaitForExit(5000)) {
        Stop-Process -Id $script:launchedProcess.Id
        [void]$script:launchedProcess.WaitForExit(5000)
    }
}

$results = @()
$exitCode = 0
$uiHidden = $false

try {
    # A stray instance owns the single debug pipe, so our commands would drive the wrong app.
    $existing = @(Get-Process -Name 'PBRApp' -ErrorAction SilentlyContinue)
    if ($existing.Count -gt 0) {
        $ids = ($existing | ForEach-Object { $_.Id }) -join ','
        Write-Host "PBRApp is already running (PID=$ids). Close it first: a stray instance steals the debug pipe."
        exit 3
    }
    if (-not (Test-Path -LiteralPath $exePath)) { throw "Executable not found: $exePath. Build the $Configuration PBRApp first." }
    if (-not (Test-Path -LiteralPath $viewsPath)) { throw "View list not found: $viewsPath" }

    $config = Get-Content -LiteralPath $viewsPath -Raw | ConvertFrom-Json
    $defaultPct = [double]$config.defaultMaxChangedOver8Pct
    $defaultAbs = [int]$config.defaultMaxAbs
    $viewList = @($config.views)
    if (-not [string]::IsNullOrWhiteSpace($Views)) {
        $wanted = @($Views.Split(',') | ForEach-Object { $_.Trim() } | Where-Object { $_ })
        $unknown = @($wanted | Where-Object { $viewList.name -notcontains $_ })
        if ($unknown.Count -gt 0) { throw "Unknown view(s): $($unknown -join ', ')" }
        $viewList = @($viewList | Where-Object { $wanted -contains $_.name })
    }
    if ($viewList.Count -eq 0) { throw 'No views selected.' }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

    # Env vars are inherited by Start-Process; restore them so this shell stays clean.
    $names = 'SASAMI_DEBUG_REMOTE', 'SASAMI_HEADLESS', 'SASAMI_FIXED_DT'
    $values = @{ SASAMI_DEBUG_REMOTE = '1'; SASAMI_HEADLESS = '1'; SASAMI_FIXED_DT = '0.016666667' }
    $saved = @{}
    foreach ($n in $names) { $saved[$n] = [System.Environment]::GetEnvironmentVariable($n, 'Process') }
    try {
        foreach ($n in $names) { [System.Environment]::SetEnvironmentVariable($n, $values[$n], 'Process') }
        $launchedProcess = Start-Process -FilePath $exePath -WorkingDirectory $repoRoot -PassThru
    } finally {
        foreach ($n in $names) { [System.Environment]::SetEnvironmentVariable($n, $saved[$n], 'Process') }
    }
    Write-Host "Started headless $Configuration PBRApp (PID=$($launchedProcess.Id))."

    try {
        $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'SasamiRenderer.Debug', [System.IO.Pipes.PipeDirection]::InOut)
        $deadline = (Get-Date).AddSeconds($BootTimeoutSeconds)
        while (-not $pipe.IsConnected) {
            $launchedProcess.Refresh()
            if ($launchedProcess.HasExited) { throw "PBRApp exited before the pipe opened (exitCode=$($launchedProcess.ExitCode))." }
            if ((Get-Date) -gt $deadline) { throw "Pipe did not open within ${BootTimeoutSeconds}s." }
            try { $pipe.Connect(2000) } catch [System.TimeoutException] { }
        }
        $writer = New-Object System.IO.StreamWriter($pipe)
        $writer.AutoFlush = $true
        $reader = New-Object System.IO.StreamReader($pipe)

        # render.get only answers once the renderer exists; "exposure" proves it is a real reply
        # and not the server's startup ERR timeout.
        $booted = $false
        while ((Get-Date) -lt $deadline) {
            $launchedProcess.Refresh()
            if ($launchedProcess.HasExited) { throw "PBRApp exited during boot (exitCode=$($launchedProcess.ExitCode))." }
            $reply = Send-Cmd 'render.get'
            if ($reply -match 'exposure') { $booted = $true; break }
            Start-Sleep -Seconds 2
        }
        if (-not $booted) { throw "Renderer did not become ready within ${BootTimeoutSeconds}s." }

        # Progressive boot keeps streaming assets after render.get works; capturing earlier
        # would bake half-loaded scenes into the baseline.
        $ready = $false
        while ((Get-Date) -lt $deadline) {
            $reply = Send-Cmd 'app.ready'
            if ($reply.StartsWith('ERR')) {
                Write-Host 'app.ready unsupported by this binary; sleeping 30s instead.'
                Start-Sleep -Seconds 30
                $ready = $true
                break
            }
            if ($reply -eq 'OK ready=1') { $ready = $true; break }
            Start-Sleep -Seconds 2
        }
        if (-not $ready) { throw "Boot phase did not reach Running within ${BootTimeoutSeconds}s." }
    } catch {
        Write-Host "BOOT/PIPE FAILURE: $($_.Exception.Message)"
        exit 4
    }

    try {
        Write-Host (Send-Cmd 'app.ui hide')
        $uiHidden = $true

        foreach ($view in $viewList) {
            $name = [string]$view.name
            Write-Host "== $name =="
            foreach ($cmd in ([string]$view.setup).Split(@(';;'), [System.StringSplitOptions]::RemoveEmptyEntries)) {
                $cmd = $cmd.Trim()
                if ($cmd) { Write-Host "> $cmd"; Write-Host "< $(Send-Cmd $cmd)" }
            }
            Wait-Frames ([long]$view.settleFrames)

            $shot = Join-Path $OutDir "$name.png"
            $repeat = Join-Path $OutDir "${name}_repeat.png"
            Save-Screenshot $shot
            Wait-Frames 10
            Save-Screenshot $repeat

            # Within-run noise: same pose, 10 frames apart. Anything here is nondeterminism,
            # not a regression, so it bounds how tight the cross-run threshold can be.
            $noise = Invoke-ImgDiff $shot $repeat -1 -1

            $row = [ordered]@{ View = $name; Result = ''; Diff = ''; Noise = $noise.Text }
            if ($Mode -eq 'capture') {
                New-Item -ItemType Directory -Path $BaselineDir -Force | Out-Null
                Copy-Item -LiteralPath $shot -Destination (Join-Path $BaselineDir "$name.png") -Force
                $row.Result = 'CAPTURED'
            } else {
                $baseline = Join-Path $BaselineDir "$name.png"
                if (-not (Test-Path -LiteralPath $baseline)) {
                    $row.Result = 'MISSING'
                    $row.Diff = "no baseline: $baseline"
                } else {
                    $pct = $defaultPct
                    if ($view.PSObject.Properties['maxChangedOver8Pct']) { $pct = [double]$view.maxChangedOver8Pct }
                    $abs = $defaultAbs
                    if ($view.PSObject.Properties['maxAbs']) { $abs = [int]$view.maxAbs }
                    $diff = Invoke-ImgDiff $baseline $shot $pct $abs
                    $row.Diff = $diff.Text
                    $row.Result = if ($diff.ExitCode -eq 0) { 'PASS' } else { 'FAIL' }
                }
            }
            $results += [pscustomobject]$row
        }

        if ($Mode -eq 'capture') {
            $head = 'unknown'
            try {
                $out = & git -C $repoRoot rev-parse --short HEAD 2>$null
                if ($LASTEXITCODE -eq 0 -and $out) { $head = [string]$out }
            } catch { }
            $manifest = [ordered]@{
                gitHead       = $head
                date          = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss')
                configuration = $Configuration
                views         = @($results | ForEach-Object { $_.View })
            }
            $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $BaselineDir 'manifest.json') -Encoding UTF8
            Write-Host "Baselines written to $BaselineDir"
        }
    } catch {
        Write-Host "BOOT/PIPE FAILURE: $($_.Exception.Message)"
        $exitCode = 4
    }
} finally {
    if ($null -ne $writer) {
        # Restore the UI even after a failure; a hidden panel would confuse the next manual session.
        try { [void](Send-Cmd 'app.ui show') } catch { }
    }
    if ($null -ne $pipe) { $pipe.Dispose() }
    if (-not $KeepRunning) {
        Stop-LaunchedProcess
    } elseif ($null -ne $launchedProcess) {
        Write-Host "Keeping golden-owned PBRApp running (PID=$($launchedProcess.Id))."
    }
}

if ($results.Count -gt 0) {
    Write-Host ''
    foreach ($r in $results) {
        Write-Host ("{0,-22} {1,-9} diff: {2}" -f $r.View, $r.Result, $r.Diff)
        Write-Host ("{0,-22} {1,-9} noise: {2}" -f '', '', $r.Noise)
    }
    if ($exitCode -eq 0 -and @($results | Where-Object { $_.Result -eq 'FAIL' -or $_.Result -eq 'MISSING' }).Count -gt 0) {
        $exitCode = 1
    }
}
exit $exitCode
