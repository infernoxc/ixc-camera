<#
.SYNOPSIS
    Automated smoke test of IXCCamera.exe: launch, start preview, verify live pixels,
    minimize/restore, stop, close. Requires a physical camera and an interactive desktop.
.DESCRIPTION
    Drives the window through Win32 messages. The preview is checked numerically (share of
    non-black pixels and pixel variance) and never saved to disk, because it may show a person.
    Exit codes: 0 pass, 1 fail, 77 skipped (no camera / no interactive desktop).
#>
param([Parameter(Mandatory)][string]$Exe, [int]$PreviewSeconds = 5)

$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
public static class Ui {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static string Text(IntPtr h) { var sb = new StringBuilder(1024); SendMessageW(h, 0x000D, (IntPtr)1024, sb); return sb.ToString(); }
    public static string ComboText(IntPtr h) {
        int i = (int)SendMessageW(h, 0x0147, IntPtr.Zero, IntPtr.Zero);  // CB_GETCURSEL
        if (i < 0) return "";
        var sb = new StringBuilder(512); SendMessageW(h, 0x0148, (IntPtr)i, sb); return sb.ToString();  // CB_GETLBTEXT
    }
    public static int ComboCount(IntPtr h) { return (int)SendMessageW(h, 0x0146, IntPtr.Zero, IntPtr.Zero); }
    // Returns {nonBlackFraction, lumaStdDev} of the window's client area rendered off-screen.
    public static double[] PreviewStats(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w <= 0 || ht <= 0) return new double[] {0, 0};
        using (var bmp = new Bitmap(w, ht)) {
            using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
            long n = 0, lit = 0; double sum = 0, sumSq = 0;
            for (int y = 0; y < ht; y += 4) for (int x = 0; x < w; x += 4) {
                Color c = bmp.GetPixel(x, y); double l = 0.299*c.R + 0.587*c.G + 0.114*c.B;
                n++; sum += l; sumSq += l*l; if (l > 12) lit++;
            }
            double mean = sum / n;
            return new double[] { (double)lit / n, Math.Sqrt(Math.Max(0, sumSq / n - mean * mean)), mean };
        }
    }
}
'@

$BM_CLICK = 0x00F5; $BM_GETCHECK = 0x00F0; $WM_CLOSE = 0x0010
$IdCamera = 101; $IdFormat = 103; $IdStart = 104; $IdPreview = 105; $IdStatus = 106; $IdHint = 107
$IdVcamStatus = 109; $IdVcamUse = 110   # must match ControlId in src/app/main.cpp
$failures = @()
function Check([bool]$ok, [string]$what) { if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:failures += $what } }

if (Get-Process IXCCamera -ErrorAction SilentlyContinue) { throw 'IXCCamera.exe is already running; close it first.' }
$p = Start-Process $Exe -PassThru
$hwnd = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $hwnd = [Ui]::FindWindowW('IXCCameraMainWindow', [NullString]::Value) }
if ($hwnd -eq [IntPtr]::Zero) { $p | Stop-Process; 'SKIP: no window (non-interactive session?)'; exit 77 }
Start-Sleep -Milliseconds 800

$camera = [Ui]::GetDlgItem($hwnd, $IdCamera); $format = [Ui]::GetDlgItem($hwnd, $IdFormat)
$start = [Ui]::GetDlgItem($hwnd, $IdStart); $preview = [Ui]::GetDlgItem($hwnd, $IdPreview)
$status = [Ui]::GetDlgItem($hwnd, $IdStatus); $hint = [Ui]::GetDlgItem($hwnd, $IdHint)

"camera:  $([Ui]::ComboText($camera))"
"format:  $([Ui]::ComboText($format))  ($([Ui]::ComboCount($format)) entries)"
if ([Ui]::ComboCount($camera) -eq 0) { [Ui]::PostMessageW($hwnd, $WM_CLOSE, 0, 0) | Out-Null; 'SKIP: no camera'; exit 77 }
Check ([Ui]::ComboCount($format) -gt 1) 'format list populated from the camera'
Check ([Ui]::ComboText($format) -like 'Auto*') 'Auto format preselected'
$vcamStatus = [Ui]::GetDlgItem($hwnd, $IdVcamStatus); $vcamUse = [Ui]::GetDlgItem($hwnd, $IdVcamUse)
$vcamText = [Ui]::Text($vcamStatus)
"ixc cam: $vcamText  [button '$([Ui]::Text($vcamUse))' enabled: $([Ui]::IsWindowEnabled($vcamUse))]"
Check ($vcamText -like 'IXC Camera*') 'IXC Camera status line shown'
if ($vcamText -like '*available to other apps and uses*') {
    # The selected webcam is the one IXC Camera already uses, so the button must be disabled.
    Check (-not [Ui]::IsWindowEnabled($vcamUse)) '"Use this webcam" disabled for the webcam IXC Camera already uses'
}

$idle = (Get-Process -Id $p.Id); $idle.Refresh()
$idleMb = $idle.PrivateMemorySize64 / 1MB

# Start preview
[Ui]::SendMessageW($start, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Seconds 2   # let the stream settle before measuring
$cpu0 = (Get-Process -Id $p.Id).TotalProcessorTime; $wall0 = [DateTime]::UtcNow
Start-Sleep -Seconds $PreviewSeconds
$cpuPct = 100 * ((Get-Process -Id $p.Id).TotalProcessorTime - $cpu0).TotalSeconds / ([DateTime]::UtcNow - $wall0).TotalSeconds
$s = [Ui]::Text($status)
"status:  $s"
"hint:    $([Ui]::Text($hint))"
Check ($s -match 'receiving ([\d.]+) FPS' -and [double]$Matches[1] -gt 1) 'frames are arriving'
Check ([Ui]::Text($start) -eq 'Stop preview') 'button switched to Stop preview'
Check (-not [Ui]::IsWindowEnabled($camera)) 'camera selector locked while previewing'
$st = [Ui]::PreviewStats($preview)
"preview: {0:P0} non-black, luma std-dev {1:N1}" -f $st[0], $st[1]
Check ($st[0] -gt 0.5 -and $st[1] -gt 3) 'preview shows a live image (not black/flat)'

# Picture adjustments: move the Brightness slider like a user would (TBM_SETPOS + WM_HSCROLL).
$IdBrightnessTrack = 202; $IdReset = 238; $IdGpu = 239; $IdSmooth = 240; $IdFace = 241   # kFirstPanelId 200: header, label/track/value per slider, mirror, reset, gpu, smooth, face
$track = [Ui]::GetDlgItem($hwnd, $IdBrightnessTrack); $reset = [Ui]::GetDlgItem($hwnd, $IdReset)
$settingsFile = Join-Path $env:ProgramData 'IXC Camera\active-profile.json'
$before = [Ui]::PreviewStats($preview)[2]
[Ui]::SendMessageW($track, 0x0405, [IntPtr]1, [IntPtr]70) | Out-Null          # TBM_SETPOS(redraw, 70)
[Ui]::SendMessageW($hwnd, 0x0114, [IntPtr]8, $track) | Out-Null               # WM_HSCROLL(TB_ENDTRACK)
Start-Sleep -Milliseconds 1200
$after = [Ui]::PreviewStats($preview)[2]
"preview: mean luma {0:N1} -> {1:N1} after Brightness +70" -f $before, $after
Check ($after -gt $before + 15) 'Brightness slider changes the live preview'
if (Test-Path (Split-Path $settingsFile)) {
    $published = Get-Content $settingsFile -Raw -ErrorAction SilentlyContinue
    Check ($published -match '"brightness": 70') 'setting published for IXC Camera'
    [Ui]::SendMessageW($reset, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    $published = Get-Content $settingsFile -Raw -ErrorAction SilentlyContinue
    Check ($published -match '"brightness": 0') 'Reset picture restores and republishes neutral settings'
    # GPU switch: off publishes "gpu": "off"; turning it back on restores "auto".
    $gpuBox = [Ui]::GetDlgItem($hwnd, $IdGpu)
    [Ui]::SendMessageW($gpuBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw) -match '"gpu": "off"') 'GPU switch off is published (CPU only)'
    [Ui]::SendMessageW($gpuBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw) -match '"gpu": "auto"') 'GPU switch back on is published'
    # Smooth motion switch: on by default; off/on is published live.
    $smoothBox = [Ui]::GetDlgItem($hwnd, $IdSmooth)
    Check ([int][Ui]::SendMessageW($smoothBox, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1) 'Smooth motion is on by default'
    [Ui]::SendMessageW($smoothBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw) -match '"smoothMotion": false') 'Smooth motion off is published'
    [Ui]::SendMessageW($smoothBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 800
    Check ((Get-Content $settingsFile -Raw) -match '"smoothMotion": true') 'Smooth motion back on is published'
    # Face tracking: off by default (no thread); on runs in the preview and is published; off stops it.
    $faceBox = [Ui]::GetDlgItem($hwnd, $IdFace)
    Check ([int][Ui]::SendMessageW($faceBox, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0) 'Face tracking is off by default'
    $threadsOff = (Get-Process -Id $p.Id).Threads.Count
    [Ui]::SendMessageW($faceBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 2600
    Check ((Get-Content $settingsFile -Raw) -match '"enabled": true') 'Face tracking on is published'
    $faceStatus = [Ui]::Text($status)
    "status:  $faceStatus"
    Check ($faceStatus -match 'face: (searching|tracking)') 'Face tracking runs in the preview'
    $threadsOn = (Get-Process -Id $p.Id).Threads.Count
    [Ui]::SendMessageW($faceBox, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 1500
    Check ((Get-Content $settingsFile -Raw) -match '"enabled": false') 'Face tracking off is published'
    Check (-not ([Ui]::Text($status) -match 'face')) 'Face tracking off: nothing running'
    Check ((Get-Process -Id $p.Id).Threads.Count -lt $threadsOn) "Face tracking thread exits when off ($threadsOff -> $threadsOn -> $((Get-Process -Id $p.Id).Threads.Count) threads)"
}

$proc = Get-Process -Id $p.Id; $proc.Refresh()
$liveMb = $proc.PrivateMemorySize64 / 1MB
"memory:  {0:N1} MB private idle, {1:N1} MB previewing; {2} threads" -f $idleMb, $liveMb, $proc.Threads.Count
"cpu:     {0:N1}% of one core while previewing (UI process only)" -f $cpuPct

# Minimize releases the camera; restore resumes it
[Ui]::ShowWindow($hwnd, 6) | Out-Null   # SW_MINIMIZE
Start-Sleep -Milliseconds 800
Check ([Ui]::Text($start) -eq 'Start preview') 'minimize stops preview (camera released)'
[Ui]::ShowWindow($hwnd, 9) | Out-Null   # SW_RESTORE
Start-Sleep -Seconds 3
Check ([Ui]::Text($start) -eq 'Stop preview' -and ([Ui]::Text($status) -match 'receiving')) 'restore resumes preview'

# Stop
[Ui]::SendMessageW($start, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
Check ([Ui]::Text($start) -eq 'Start preview') 'stop preview'
$st = [Ui]::PreviewStats($preview)
Check ($st[1] -lt 30) 'preview cleared after stop'

# Close
[Ui]::PostMessageW($hwnd, $WM_CLOSE, 0, 0) | Out-Null
$exited = $p.WaitForExit(5000)
Check ($exited -and $p.ExitCode -eq 0) 'window closes and process exits cleanly'
if (-not $exited) { $p | Stop-Process -Force }

if ($failures.Count) { "RESULT: FAIL ($($failures.Count))"; exit 1 }
'RESULT: PASS'; exit 0
