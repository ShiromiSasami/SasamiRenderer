# Compares two PNG dumps pixel by pixel and reports how much they differ.
# Use with debug.dumptarget to A/B intermediate render targets (shadow cascades, GBuffer),
# which the final composite cannot show: a shadow LOD change that moves 0.18% of the
# cascade texels is invisible in the back buffer.
#   .\imgdiff.ps1 -A shot-a.png -B shot-b.png
# nonzeroA = fraction of the first image that is not black (sanity check that the dump has content).
# Every comparison covers all three colour channels: a difference confined to one channel
# must not read as "identical". Alpha is ignored, the dumps are opaque.
# Optional gates turn the report into a pass/fail check (used by golden.ps1). Negative = no gate.
# Exit codes: 0 = pass / no gate, 1 = a gate was exceeded, 2 = size mismatch.
param([string]$A,[string]$B,[double]$MaxChangedOver8Pct=-1,[int]$MaxAbs=-1)
Add-Type -AssemblyName System.Drawing
$ba=[System.Drawing.Bitmap]::FromFile($A); $bb=[System.Drawing.Bitmap]::FromFile($B)
if ($ba.Width -ne $bb.Width -or $ba.Height -ne $bb.Height) { "SIZE MISMATCH"; $ba.Dispose(); $bb.Dispose(); exit 2 }
$r=New-Object System.Drawing.Rectangle 0,0,$ba.Width,$ba.Height
$da=$ba.LockBits($r,'ReadOnly','Format32bppArgb'); $db=$bb.LockBits($r,'ReadOnly','Format32bppArgb')
$n=$da.Stride*$ba.Height
$pa=New-Object byte[] $n; $pb=New-Object byte[] $n
[System.Runtime.InteropServices.Marshal]::Copy($da.Scan0,$pa,0,$n)
[System.Runtime.InteropServices.Marshal]::Copy($db.Scan0,$pb,0,$n)
$ba.UnlockBits($da); $bb.UnlockBits($db)
$diff=0; $big=0; $max=0; $px=0; $nonzeroA=0
for ($i=0; $i -lt $n; $i+=4) {
  $px++
  if (($pa[$i] -ne 0) -or ($pa[$i+1] -ne 0) -or ($pa[$i+2] -ne 0)) { $nonzeroA++ }
  # BGRA layout: worst channel of B, G, R decides whether this pixel counts as changed.
  $d=[Math]::Abs([int]$pa[$i]-[int]$pb[$i])
  $dg=[Math]::Abs([int]$pa[$i+1]-[int]$pb[$i+1])
  if ($dg -gt $d) { $d=$dg }
  $dr=[Math]::Abs([int]$pa[$i+2]-[int]$pb[$i+2])
  if ($dr -gt $d) { $d=$dr }
  if ($d -gt 0) { $diff++ }
  if ($d -gt 8) { $big++ }
  if ($d -gt $max) { $max=$d }
}
"{0}x{1}  nonzeroA={2:P1}  changed={3:P3}  changed>8={4:P3}  maxAbs={5}" -f $ba.Width,$ba.Height,($nonzeroA/$px),($diff/$px),($big/$px),$max
$ba.Dispose(); $bb.Dispose()
$bigPct=100.0*$big/$px
$failures=@()
if ($MaxChangedOver8Pct -ge 0 -and $bigPct -gt $MaxChangedOver8Pct) { $failures += ("changed>8 {0:F3}% > {1}%" -f $bigPct,$MaxChangedOver8Pct) }
if ($MaxAbs -ge 0 -and $max -gt $MaxAbs) { $failures += ("maxAbs {0} > {1}" -f $max,$MaxAbs) }
if ($failures.Count -gt 0) { "FAIL " + ($failures -join "; "); exit 1 }
exit 0
