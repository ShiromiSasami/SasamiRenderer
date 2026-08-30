param(
    [string]$ProcessName = "PBRApp",
    [string]$OutputPath = ""
)

if (-not $OutputPath) {
    $dir = "C:\temp"
    if (-not (Test-Path $dir)) {
        $dir = $env:TEMP
    }
    $OutputPath = Join-Path $dir "sasami_verify.png"
}

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Drawing;
using System.Drawing.Imaging;

public class WindowCapture {
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hwnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left, Top, Right, Bottom;
    }

    public static string Capture(IntPtr hwnd, string path) {
        RECT rect;
        GetWindowRect(hwnd, out rect);
        int width = rect.Right - rect.Left;
        int height = rect.Bottom - rect.Top;
        if (width <= 0 || height <= 0) return "ERROR: invalid window size";
        Bitmap bmp = new Bitmap(width, height);
        Graphics g = Graphics.FromImage(bmp);
        IntPtr hdc = g.GetHdc();
        bool ok = PrintWindow(hwnd, hdc, 2);
        g.ReleaseHdc(hdc);
        g.Dispose();
        if (!ok) return "ERROR: PrintWindow failed";
        bmp.Save(path, ImageFormat.Png);
        bmp.Dispose();
        return path;
    }
}
"@ -ReferencedAssemblies System.Drawing

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue
if (-not $proc) {
    Write-Error "Process '$ProcessName' not found"
    exit 1
}

$hwnd = $proc.MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    Write-Error "No main window found for '$ProcessName'"
    exit 1
}

$result = [WindowCapture]::Capture($hwnd, $OutputPath)
if ($result -like "ERROR:*") {
    Write-Error $result
    exit 1
}

Write-Output $result
