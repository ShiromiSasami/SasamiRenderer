param(
    [string]$Path = "",
    [int]$TimeoutSeconds = 15,
    [int]$PollIntervalMs = 100
)

if ([string]::IsNullOrWhiteSpace($Path)) {
    # Captures live next to the debug tooling that produces them, not in the repo
    # root. Derived from $PSScriptRoot rather than the caller's working directory,
    # which may not be the repo root when this is invoked from elsewhere.
    $timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
    $Path = Join-Path $PSScriptRoot "Screenshots\shot-$timestamp.png"
}

# The renderer writes the file itself, so the directory has to exist before the
# capture is queued -- otherwise the PNG encode fails at the very end of the round trip.
$outputDir = Split-Path -Parent $Path
if (-not [string]::IsNullOrWhiteSpace($outputDir) -and -not (Test-Path $outputDir)) {
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
}

$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'SasamiRenderer.Debug', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(5000) } catch { Write-Output "CONNECT_FAILED: $($_.Exception.Message)"; exit 1 }

try {
    $writer = New-Object System.IO.StreamWriter($pipe); $writer.AutoFlush = $true
    $reader = New-Object System.IO.StreamReader($pipe)

    $writer.WriteLine("debug.screenshot $Path")
    $reply = $reader.ReadLine()
    Write-Host "> debug.screenshot $Path"
    Write-Host "< $reply"

    if (-not $reply.StartsWith("OK")) {
        Write-Output $reply
        exit 1
    }

    $stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
    $status = "PENDING"
    while ($status -eq "PENDING") {
        if ($stopwatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
            Write-Output "TIMEOUT: capture did not complete within ${TimeoutSeconds}s"
            exit 1
        }
        Start-Sleep -Milliseconds $PollIntervalMs
        $writer.WriteLine("debug.screenshot.status")
        $status = $reader.ReadLine()
        Write-Host "> debug.screenshot.status"
        Write-Host "< $status"
    }

    if ($status.StartsWith("ERR")) {
        Write-Output $status
        exit 1
    }

    # Only the resolved path goes through Write-Output so a caller capturing
    # this script's stdout gets a clean value; everything else is Write-Host.
    $resolvedPath = $status.Substring(3)
    Write-Output $resolvedPath
} finally {
    $pipe.Dispose()
}

exit 0
