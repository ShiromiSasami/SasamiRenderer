param([string]$Script)
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', 'SasamiRenderer.Debug', [System.IO.Pipes.PipeDirection]::InOut)
try { $pipe.Connect(5000) } catch { Write-Output "CONNECT_FAILED: $($_.Exception.Message)"; exit 1 }
$writer = New-Object System.IO.StreamWriter($pipe); $writer.AutoFlush = $true
$reader = New-Object System.IO.StreamReader($pipe)
foreach ($c in ($Script -split ';;')) {
    if ([string]::IsNullOrWhiteSpace($c)) { continue }
    $writer.WriteLine($c.Trim())
    Write-Output "> $($c.Trim())"
    Write-Output "< $($reader.ReadLine())"
}
$pipe.Dispose()
