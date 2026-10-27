# Re-captures the exact poses that Screenshots\sweep was taken from, so the light-space
# depth-slab fix can be judged against images that already exist rather than against memory.
#
# What changed: LightSystemShadow sized the cascade's depth slab from a half-depth floored at
# (m_lightFar - m_lightNear) * 0.5 = 15 while pulling the light back by max(m_lightDistance, ...)
# = 50. The two floors disagreed, so the slab came out as roughly [35, 65] in light space and
# DepthClipEnable discarded every caster between the light and z=35. The near cascades leaked.
# The slab now starts at the light plane and ends past the cascade sphere.
#
# Why re-use the sweep pose: Screenshots\sweep\shadow_80_view*.png were captured from
# "camera.setTarget 46 3.5 -3" / "camera.setYawPitch 2.35 -0.045" at shadow distance 80. Holding
# the same pose and the same debug views makes those files a real before-image. Any other pose
# would compare two different scenes and prove nothing -- the same mistake the earlier perf work
# in this repo already made once.
#
# The second pose is the high-altitude one the over-occlusion was reported from. It has no
# baseline, so it is captured for its own sake: the near cascades are the ones the old slab
# clipped, and looking down from height is where they cover the most screen.

param(
    [string]$OutDir = (Join-Path $PSScriptRoot "Screenshots\slab"),
    # 0=FinalLit 2=Normal 11=NdotL 6=Shadow. Same set the sweep used.
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
    & $shotScript -Path (Join-Path $OutDir "$label.png") | Out-Null
    Write-Output "  captured $label"
}

function Capture-Pose([string]$prefix, [string]$target, [string]$yawPitch) {
    Write-Output "`n=== $prefix ==="
    Send-Cmd "camera.setTarget $target"    | Out-Null
    Send-Cmd "camera.setYawPitch $yawPitch" | Out-Null
    Start-Sleep -Milliseconds 400
    foreach ($v in $Views) {
        Send-Cmd "render.gbuffer $v" | Out-Null
        # The debug view is latched on the next frame; the sweep used 600ms and it was enough.
        Start-Sleep -Milliseconds 600
        Capture ("{0}_view{1}" -f $prefix, $v)
    }
    Send-Cmd "render.gbuffer 0" | Out-Null
}

# Hold shadow distance at the value the baseline images were taken at, whatever the scene set.
$restoreShadow = Send-Cmd "render.shadowdistance"
Write-Output "Baseline shadow distance reported as: $restoreShadow"
Send-Cmd "render.shadowdistance 80" | Out-Null

# Directly comparable with Screenshots\sweep\shadow_80_view*.png.
Capture-Pose "slab_streetlevel" "46 3.5 -3" "2.35 -0.045"

# The over-occlusion report came from looking down from height; no baseline exists for it.
# Pitch is positive-down here: the street pose above sits at -0.045 and reports forward.y
# = +0.045, so a negative pitch tilts the camera up into empty sky.
Capture-Pose "slab_high" "46 40 -3" "2.35 0.9"

Write-Output "`nDone. Compare slab_streetlevel_view*.png against Screenshots\sweep\shadow_80_view*.png"
