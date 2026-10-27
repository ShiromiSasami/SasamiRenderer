# Captures the A/B pair for the indirect-specular double-count fix (PBR_BRDF.hlsli:
# ReflectionCoverage). The bug only shows where the reflection composite actually adds
# something, and coverage = alpha * strength * mask * roughnessVisibility with
# mask = max(authored, metallic * (1 - roughness)) -- so on a dielectric or a rough
# surface both the old and the new code produce the identical image. ReflectionAlpha is
# captured alongside FinalLit for exactly that reason: an all-black alpha view means the
# pose has no pixels the fix can affect, and the FinalLit pair proves nothing.
#
# SWRT reflections force SSR off (Renderer::BuildRenderPassExecutionPolicy), so the
# toggle picks between the two reflection sources rather than merely adding one.
param(
    [string]$OutDir      = (Join-Path $PSScriptRoot "Screenshots\reflection"),
    [string]$Label       = "refl",
    [string]$Target      = "46 3.5 -3",
    [string]$YawPitch    = "2.35 -0.045",
    # Skip the pose commands and shoot wherever the camera already is, for hunting a
    # pose interactively before committing to one.
    [switch]$KeepCamera
)

$ErrorActionPreference = "Stop"
$pipeScript = Join-Path $PSScriptRoot "pipe.ps1"
$shotScript = Join-Path $PSScriptRoot "screenshot.ps1"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

function Send-Cmd([string]$cmd) {
    $reply = & $pipeScript -Script $cmd 2>&1 | Where-Object { $_ -like "< *" }
    return ($reply -join " ").TrimStart("< ")
}

function Capture([string]$name) {
    & $shotScript -Path (Join-Path $OutDir "$name.png") | Out-Null
    Write-Output "    captured $name"
}

# Fail loudly if the running instance predates the toggle -- a stale PBRApp still owns the
# pipe and answers with old code even when the binary on disk is current.
$probe = Send-Cmd "render.swrtreflection"
Write-Output "render.swrtreflection -> $probe"
if ($probe -notlike "OK*") {
    Write-Output "ABORT: the running PBRApp does not have render.swrtreflection. Kill it and relaunch the fresh build."
    exit 1
}

if (-not $KeepCamera) {
    Send-Cmd "camera.setTarget $Target"   | Out-Null
    Send-Cmd "camera.setYawPitch $YawPitch" | Out-Null
    Start-Sleep -Milliseconds 400
}

Write-Output "`n=== reflections OFF (IBL specular at full strength) ==="
Send-Cmd "render.swrtreflection 0" | Out-Null
Send-Cmd "render.gbuffer 0"        | Out-Null
Start-Sleep -Milliseconds 600
Capture "${Label}_off_finallit"

Write-Output "`n=== reflections ON (IBL * (1 - coverage) + traced * coverage) ==="
Send-Cmd "render.swrtreflection 1" | Out-Null
# The reflection texture is temporally accumulated, so the first frames after the toggle
# are not what the pass converges to. Give it time before shooting.
Start-Sleep -Seconds 2
Send-Cmd "render.gbuffer 0" | Out-Null
Start-Sleep -Milliseconds 600
Capture "${Label}_on_finallit"

foreach ($view in @("ReflectionAlpha", "ReflectionRadiance", "SwrtReflectionComposite")) {
    $reply = Send-Cmd "render.gbuffer $view"
    if ($reply -like "OK*") {
        Start-Sleep -Milliseconds 600
        Capture ("{0}_on_{1}" -f $Label, $view.ToLower())
    } else {
        Write-Output "    skipped $view -> $reply"
    }
}

Send-Cmd "render.gbuffer 0" | Out-Null
Write-Output "`nDone. Images in $OutDir"
