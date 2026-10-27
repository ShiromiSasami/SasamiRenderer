# Confirms the directional-light gizmo still draws after it was moved out of the
# "PBR Controls" window callback and into an ImGuiCoordinator overlay.
#
# Why a script: the move fixed a bug where collapsing that unrelated panel also hid the
# gizmo (ImGuiCoordinator only invokes a window's callback while ImGui::Begin returns
# true). The structural half of that fix is obvious from the code -- the overlay loop runs
# outside Begin/End, so no window's state can reach it. What is NOT obvious is whether the
# gizmo still renders correctly after being lifted out of its old scope, and that needs a
# picture. Collapsing the panel by hand moves the camera, so the toggle has to come from
# the debug channel instead: light.gizmo.
#
# The camera yaw is swept rather than aimed: the gizmo sits at -lightDirection * distance
# * 2.5, and deriving the yaw that points at it would mean re-deriving
# Math::DirectionFromYawPitch's angle convention here, where getting it wrong yields an
# empty frame that looks exactly like a regression. Sweeping is convention-independent --
# the sun lands in one of the frames whatever the convention is.

param(
    [string]$OutDir = (Join-Path $PSScriptRoot "Screenshots\gizmo"),
    # Camera yaw steps in radians, a full turn in eighths.
    [double[]]$Yaws = @(0.0, 0.785, 1.571, 2.356, 3.142, 3.927, 4.712, 5.498),
    [double]$Pitch = 0.35
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

Write-Output "Light state: $(Send-Cmd 'light.get')"

# FinalLit: the gizmo is an ImGui overlay and draws over any debug view, but a debug view
# would make an actual rendering regression harder to notice in the same frame.
Send-Cmd "render.gbuffer 0" | Out-Null

$gizmoOn = Send-Cmd "light.gizmo 1"
Write-Output "Gizmo enabled -> $gizmoOn"
if (-not $gizmoOn.StartsWith("OK")) {
    Write-Output "light.gizmo not available -- is this build current?"
    exit 1
}

Write-Output "`n=== Yaw sweep (gizmo ON) ==="
Send-Cmd "camera.setTarget 46 3.5 -3" | Out-Null
foreach ($y in $Yaws) {
    Send-Cmd ("camera.setYawPitch {0} {1}" -f $y, $Pitch) | Out-Null
    Start-Sleep -Milliseconds 500
    Capture ("on_yaw{0}" -f $y)
}

# Same poses with the gizmo off. Diffing the pairs is what proves a yellow disc in an
# "on" frame is the gizmo and not some bright piece of the scene at that angle.
Write-Output "`n=== Same poses, gizmo OFF (control) ==="
Send-Cmd "light.gizmo 0" | Out-Null
foreach ($y in $Yaws) {
    Send-Cmd ("camera.setYawPitch {0} {1}" -f $y, $Pitch) | Out-Null
    Start-Sleep -Milliseconds 500
    Capture ("off_yaw{0}" -f $y)
}

Send-Cmd "light.gizmo 1" | Out-Null
Write-Output "`nDone. Images in $OutDir"
