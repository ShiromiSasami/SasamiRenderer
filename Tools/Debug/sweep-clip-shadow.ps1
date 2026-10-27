# Sweeps near clip and shadow distance, capturing a screenshot at each setting.
#
# Why this exists: two symptoms were traced to these two values and both need a visual
# A/B to confirm, but comparing captures taken minutes apart is worthless -- the earlier
# perf work in this repo already produced one wrong conclusion that way. This sweep takes
# every capture back-to-back from one fixed camera pose so the only thing that differs
# between images is the parameter being swept.
#
# Symptom 1 (near clip): dashed black outlines along architectural detail, and speckle in
#   the Normal / NdotL debug views. Suspected depth-precision Z-fighting from near=0.0005
#   against far=500 with no reversed-Z.
# Symptom 2 (shadow distance): lighting "suddenly arrives" as the camera moves, because
#   PBR_Shadow.hlsli returns full visibility for pixels past the last cascade split, and
#   LightSystem's shadow distance is only 80 world units.
#
# Output file names carry the swept value so the images stay interpretable on their own.

param(
    [string]$OutDir = (Join-Path $PSScriptRoot "Screenshots\sweep"),
    # Debug views captured at every step. 0=FinalLit 2=Normal 11=NdotL 6=Shadow.
    [int[]]$Views = @(0, 2, 11, 6)
)

$ErrorActionPreference = "Stop"
$pipeScript = Join-Path $PSScriptRoot "pipe.ps1"
$shotScript = Join-Path $PSScriptRoot "screenshot.ps1"

if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir -Force | Out-Null }

function Send-Cmd([string]$cmd) {
    $reply = & $pipeScript -Script $cmd 2>&1 | Where-Object { $_ -like "< *" }
    return ($reply -join " ").TrimStart("< ")
}

function Capture([string]$label) {
    $path = Join-Path $OutDir "$label.png"
    & $shotScript -Path $path | Out-Null
    Write-Output "  captured $label"
}

# One fixed pose for every capture; without this the sweep compares different scenes.
Write-Output "Fixing camera pose..."
Send-Cmd "camera.setTarget 46 3.5 -3" | Out-Null
Send-Cmd "camera.setYawPitch 2.35 -0.045" | Out-Null

$baselineNear = Send-Cmd "render.nearclip"
$baselineShadow = Send-Cmd "render.shadowdistance"
Write-Output "Baseline: $baselineNear / $baselineShadow"

Write-Output "`n=== Sweep 1: near clip (shadow distance held at baseline) ==="
foreach ($n in @(0.0005, 0.01, 0.05, 0.1, 0.5)) {
    $applied = Send-Cmd "render.nearclip $n"
    Write-Output "near=$n -> $applied"
    foreach ($v in $Views) {
        Send-Cmd "render.gbuffer $v" | Out-Null
        Start-Sleep -Milliseconds 600
        Capture ("near_{0}_view{1}" -f $n, $v)
    }
}
Send-Cmd "render.nearclip 0.0005" | Out-Null   # restore before the second sweep

Write-Output "`n=== Sweep 2: shadow distance (near clip back at baseline) ==="
foreach ($d in @(80, 150, 300, 500)) {
    $applied = Send-Cmd "render.shadowdistance $d"
    Write-Output "shadowDistance=$d -> $applied"
    foreach ($v in $Views) {
        Send-Cmd "render.gbuffer $v" | Out-Null
        Start-Sleep -Milliseconds 600
        Capture ("shadow_{0}_view{1}" -f $d, $v)
    }
}

Write-Output "`n=== Sweep 3: both improved together ==="
Send-Cmd "render.nearclip 0.05" | Out-Null
Send-Cmd "render.shadowdistance 300" | Out-Null
foreach ($v in $Views) {
    Send-Cmd "render.gbuffer $v" | Out-Null
    Start-Sleep -Milliseconds 600
    Capture ("both_view{0}" -f $v)
}

# Leave the app on FinalLit rather than whatever view the loop ended on.
Send-Cmd "render.gbuffer 0" | Out-Null
Write-Output "`nDone. Images in $OutDir"
