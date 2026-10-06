<#
.SYNOPSIS
    Automated UI test of IXCCamera.exe (the redesigned control panel). Requires a physical camera
    and an interactive desktop.
.DESCRIPTION
    Drives the window through Win32 messages, the same ones a user's clicks produce:
      * launch opens straight to a live preview;
      * every picture slider, Mirror and Reset;
      * every effect switch and its own strength slider, the Effects master switch, "All off";
      * every feature switch (Smooth motion, Face tracking, Show face markers, GPU) and the
        global hotkeys switch; face tracking really stops when off;
      * profiles (save/switch) and hotkeys (simulated WM_HOTKEY);
      * layout: no visible controls overlap or leave the window, at a normal and the minimum size;
      * settings survive an application restart;
      * minimize/restore, stop, close.
    The preview is checked numerically and never saved to disk (it may show a person). The user's
    profiles and app settings are backed up first and restored at the end.
    Exit codes: 0 pass, 1 fail, 77 skipped (no camera / no interactive desktop).
#>
param([Parameter(Mandatory)][string]$Exe)

$ErrorActionPreference = 'Stop'
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;
public static class Ui {
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern IntPtr GetWindow(IntPtr h, uint cmd);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static string Text(IntPtr h) { var sb = new StringBuilder(1024); SendMessageW(h, 0x000D, (IntPtr)1024, sb); return sb.ToString(); }
    public static string ComboText(IntPtr h) {
        int i = (int)SendMessageW(h, 0x0147, IntPtr.Zero, IntPtr.Zero);
        if (i < 0) return "";
        var sb = new StringBuilder(512); SendMessageW(h, 0x0148, (IntPtr)i, sb); return sb.ToString();
    }
    public static int ComboCount(IntPtr h) { return (int)SendMessageW(h, 0x0146, IntPtr.Zero, IntPtr.Zero); }
    // Visible children of a window that overlap each other (combo drop-downs excluded) or stick out of `bounds`.
    public static string LayoutProblems(IntPtr parent, RECT bounds) {
        var rects = new List<KeyValuePair<string, RECT>>();
        for (IntPtr c = GetWindow(parent, 5); c != IntPtr.Zero; c = GetWindow(c, 2)) {
            if (!IsWindowVisible(c)) continue;
            RECT r; GetWindowRect(c, out r);
            if (r.R - r.L <= 0 || r.B - r.T <= 0) continue;
            var cls = new StringBuilder(64); GetClassNameW(c, cls, 64);
            if (cls.ToString() == "ComboBox") r.B = r.T + 26;  // ignore the (closed) list height
            rects.Add(new KeyValuePair<string, RECT>(Text(c) + "#" + cls, r));
        }
        var sb = new StringBuilder();
        for (int i = 0; i < rects.Count; i++) {
            RECT a = rects[i].Value;
            if (a.L < bounds.L - 1 || a.R > bounds.R + 1) sb.Append("outside: " + rects[i].Key + "; ");
            for (int j = i + 1; j < rects.Count; j++) {
                RECT b = rects[j].Value;
                if (a.L < b.R && b.L < a.R && a.T < b.B && b.T < a.B) sb.Append("overlap: " + rects[i].Key + " / " + rects[j].Key + "; ");
            }
        }
        return sb.ToString();
    }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    public static double[] PreviewStats(IntPtr h) {
        RECT r; GetWindowRect(h, out r);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w <= 0 || ht <= 0) return new double[] {0, 0, 0};
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

$BM_CLICK = 0x00F5; $BM_GETCHECK = 0x00F0; $WM_CLOSE = 0x0010; $TBM_GETPOS = 0x0400; $TBM_SETPOS = 0x0405; $WM_HSCROLL = 0x0114
$WM_SETTEXT = 0x000C; $WM_HOTKEY = 0x0312
# Main window controls (src/app/main.cpp) and settings column controls (src/app/settings_panel.h).
$IdCamera = 101; $IdFormat = 103; $IdStart = 104; $IdPreview = 105; $IdStatus = 106; $IdHint = 107
$IdProfile = 200; $IdProfileSave = 201; $IdHotkeys = 205; $IdReset = 206; $IdMirror = 207; $IdSlider0 = 210
$IdMaster = 230; $IdAllOff = 231; $IdEffect0 = 240; $IdStrength0 = 260
$IdSmooth = 280; $IdFace = 281; $IdMarkers = 282; $IdGpu = 283; $IdAutoFrame = 284; $IdVcamStatus = 290; $IdVcamUse = 291
$effectIds = 'blush.tone', 'beauty.basic', 'portrait.soft', 'color.warm', 'color.cool', 'color.mono', 'color.vivid', 'lighting.soft'
$IdxMono = [array]::IndexOf($effectIds, 'color.mono'); $IdxVivid = [array]::IndexOf($effectIds, 'color.vivid')

$failures = @()
function Check([bool]$ok, [string]$what) { if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:failures += $what } }
function Click($h) { [Ui]::SendMessageW($h, $BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null }
function Checked($h) { [int][Ui]::SendMessageW($h, $BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero) -eq 1 }
function SetSlider($h, [int]$pos) {
    [Ui]::SendMessageW($h, $TBM_SETPOS, [IntPtr]1, [IntPtr]$pos) | Out-Null
    [Ui]::SendMessageW($script:content, $WM_HSCROLL, [IntPtr]8, $h) | Out-Null   # TB_ENDTRACK, like a released drag
}
$settingsFile = Join-Path $env:ProgramData 'IXC Camera\active-profile.json'
$dataDir = Join-Path $env:LOCALAPPDATA 'IXC Camera'
$appSettings = Join-Path $dataDir 'app-settings.json'
function Published { Start-Sleep -Milliseconds 450; if (Test-Path $settingsFile) { Get-Content $settingsFile -Raw } else { '' } }
function AppSettingsText { if (Test-Path $appSettings) { Get-Content $appSettings -Raw } else { '' } }

function Launch {
    $script:p = Start-Process $Exe -PassThru
    $script:hwnd = [IntPtr]::Zero
    for ($i = 0; $i -lt 50 -and $script:hwnd -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $script:hwnd = [Ui]::FindWindowW('IXCCameraMainWindow', [NullString]::Value) }
    if ($script:hwnd -eq [IntPtr]::Zero) { return $false }
    $host_ = [Ui]::FindWindowExW($script:hwnd, [IntPtr]::Zero, 'IXCSettingsHost', [NullString]::Value)
    $script:content = [Ui]::FindWindowExW($host_, [IntPtr]::Zero, 'IXCSettingsContent', [NullString]::Value)
    $script:start = [Ui]::GetDlgItem($script:hwnd, $IdStart); $script:status = [Ui]::GetDlgItem($script:hwnd, $IdStatus)
    $script:preview = [Ui]::GetDlgItem($script:hwnd, $IdPreview)
    return $true
}
function P([int]$id) { [Ui]::GetDlgItem($script:content, $id) }   # a settings column control
function Close {
    [Ui]::PostMessageW($script:hwnd, $WM_CLOSE, 0, 0) | Out-Null
    $ok = $script:p.WaitForExit(6000)
    if (-not $ok) { $script:p | Stop-Process -Force }
    return $ok -and $script:p.ExitCode -eq 0
}

if (Get-Process IXCCamera -ErrorAction SilentlyContinue) { throw 'IXCCamera.exe is already running; close it first.' }
# Back up the user's settings; restored at the end whatever happens.
$backup = Join-Path $env:TEMP ("ixc-ui-backup-" + [guid]::NewGuid())
New-Item -ItemType Directory $backup | Out-Null
if (Test-Path $dataDir) { Copy-Item "$dataDir\profiles" "$backup\profiles" -Recurse -ErrorAction SilentlyContinue; if (Test-Path $appSettings) { Copy-Item $appSettings $backup } }
if (Test-Path $settingsFile) { Copy-Item $settingsFile "$backup\active-profile.json" }

try {
    if (-not (Launch)) { 'SKIP: no window (non-interactive session?)'; exit 77 }
    $camera = [Ui]::GetDlgItem($hwnd, $IdCamera); $format = [Ui]::GetDlgItem($hwnd, $IdFormat)
    if ([Ui]::ComboCount($camera) -eq 0) { Close | Out-Null; 'SKIP: no camera'; exit 77 }

    # --- Opens straight to the live preview -------------------------------------------------------
    for ($i = 0; $i -lt 40 -and -not ([Ui]::Text($status) -match 'receiving ([\d.]+) FPS' -and [double]$Matches[1] -gt 1); $i++) { Start-Sleep -Milliseconds 200 }
    "status:  $([Ui]::Text($status))"
    Check ([Ui]::Text($status) -match 'receiving ([\d.]+) FPS' -and [double]$Matches[1] -gt 1) 'launch opens straight to a live preview'
    Check ([Ui]::Text($start) -eq 'Stop preview') 'Start/Stop button shows Stop preview'
    Check ([Ui]::ComboCount($format) -gt 1) 'format list populated from the camera'
    $vcamText = [Ui]::Text((P $IdVcamStatus))
    "ixc cam: $vcamText"
    Check ($vcamText -like 'IXC Camera*') 'IXC Camera status shown in its card'
    Start-Sleep -Seconds 2
    $cpu0 = $p.TotalProcessorTime; $w0 = [DateTime]::UtcNow; Start-Sleep -Seconds 4; $p.Refresh()
    $cpuPct = 100 * ($p.TotalProcessorTime - $cpu0).TotalSeconds / ([DateTime]::UtcNow - $w0).TotalSeconds
    $st = [Ui]::PreviewStats($preview)
    "preview: {0:P0} non-black, luma std-dev {1:N1}" -f $st[0], $st[1]
    Check ($st[0] -gt 0.5 -and $st[1] -gt 3) 'preview shows a live image (not black/flat)'

    # --- Known starting state (the user's settings are restored at the end) -----------------------
    Click (P $IdAllOff)
    if (-not (Checked (P $IdMaster))) { Click (P $IdMaster) }
    if (-not (Checked (P $IdMarkers))) { Click (P $IdMarkers) }
    if (Checked (P $IdFace)) { Click (P $IdFace) }
    if (-not (Checked (P $IdSmooth))) { Click (P $IdSmooth) }
    if (-not (Checked (P $IdGpu))) { Click (P $IdGpu) }
    if (-not (Checked (P $IdHotkeys))) { Click (P $IdHotkeys) }
    Start-Sleep -Milliseconds 800

    # --- Layout -------------------------------------------------------------------------------------
    foreach ($size in @(@(1280, 820), @(860, 560))) {
        [Ui]::SetWindowPos($hwnd, [IntPtr]::Zero, 30, 30, $size[0], $size[1], 0x0014) | Out-Null
        Start-Sleep -Milliseconds 600
        $wr = New-Object Ui+RECT; [Ui]::GetWindowRect($hwnd, [ref]$wr) | Out-Null
        $cr = New-Object Ui+RECT; [Ui]::GetWindowRect($content, [ref]$cr) | Out-Null
        $main = [Ui]::LayoutProblems($hwnd, $wr); $panel = [Ui]::LayoutProblems($content, $cr)
        Check (-not $main -and -not $panel) "no overlapping or clipped controls at $($size[0])x$($size[1]) $main $panel"
    }
    [Ui]::SetWindowPos($hwnd, [IntPtr]::Zero, 30, 30, 1280, 820, 0x0014) | Out-Null

    # --- Picture: every slider, then Reset -------------------------------------------------------------
    $sliders = @(
        @('brightness', 40, '"brightness": 40'), @('contrast', 40, '"contrast": 40'), @('saturation', 40, '"saturation": 40'),
        @('warmth', 40, '"temperature": 40'), @('tint', 40, '"tint": 40'), @('exposure', 10, '"exposureEv": 1'),
        @('highlights', 40, '"highlights": 40'), @('shadows', 40, '"shadows": 40'), @('low-light', 40, '"lowLight": 40'),
        @('noise reduction', 40, '"denoise": 40'), @('gamma', 150, '"gamma": 1.5'), @('sharpness', 40, '"sharpness": 40'), @('zoom', 150, '"zoom": 1.5'))
    $before = [Ui]::PreviewStats($preview)[2]
    for ($i = 0; $i -lt $sliders.Count; $i++) {
        $s = P ($IdSlider0 + $i)
        SetSlider $s $sliders[$i][1]
        $pub = Published
        Check ($pub.Contains($sliders[$i][2]) -and [int][Ui]::SendMessageW($s, $TBM_GETPOS, [IntPtr]::Zero, [IntPtr]::Zero) -eq $sliders[$i][1]) "slider '$($sliders[$i][0])' applies and is published ($($sliders[$i][2]))"
        if ($i -eq 0) {
            Start-Sleep -Milliseconds 600
            $after = [Ui]::PreviewStats($preview)[2]
            "preview: mean luma {0:N1} -> {1:N1} after Brightness +40" -f $before, $after
            Check ($after -gt $before + 8) 'Brightness changes the live preview'
        }
    }
    Click (P $IdMirror); Check ((Published) -match '"mirror": true') 'Mirror on is published'
    Click (P $IdReset)
    $pub = Published
    Check ($pub -match '"brightness": 0' -and $pub -match '"zoom": 1,' -and $pub -match '"gamma": 1,' -and $pub -match '"sharpness": 15' -and $pub -match '"mirror": false') 'Reset restores every picture setting (and Mirror)'
    Check ([int][Ui]::SendMessageW((P $IdSlider0), $TBM_GETPOS, [IntPtr]::Zero, [IntPtr]::Zero) -eq 0) 'Reset moves the sliders back'

    # --- Effects: each switch + its own strength slider, master switch, All off -----------------------------
    for ($i = 0; $i -lt $effectIds.Count; $i++) {
        $t = P ($IdEffect0 + $i); $s = P ($IdStrength0 + $i)
        Check (-not [Ui]::IsWindowVisible($s)) "$($effectIds[$i]): strength slider hidden while off"
        Click $t
        $pub = Published
        Check ((Checked $t) -and $pub.Contains('"' + $effectIds[$i] + '"') -and [Ui]::IsWindowVisible($s)) "$($effectIds[$i]): switch on applies it (published) and shows its strength slider"
        SetSlider $s 40
        $pub = Published
        Check ($pub -match ('"' + [regex]::Escape($effectIds[$i]) + '",\s*"strength": 40')) "$($effectIds[$i]): its own strength slider is published"
        if ($effectIds[$i] -eq 'blush.tone') {
            Start-Sleep -Milliseconds 2200
            Check ([Ui]::Text($status) -match 'face: (searching|tracking)') 'a face effect starts face tracking automatically'
        }
        Click $t
        $pub = Published
        Check (-not (Checked $t) -and -not $pub.Contains('"' + $effectIds[$i] + '"') -and -not [Ui]::IsWindowVisible($s)) "$($effectIds[$i]): switch off removes it"
    }
    Click (P ($IdEffect0 + $IdxMono)); Click (P ($IdEffect0 + $IdxVivid))   # mono + vivid
    Click (P $IdMaster)
    $pub = Published
    Check ($pub -match '"effectsEnabled": false' -and -not [Ui]::IsWindowEnabled((P ($IdEffect0 + $IdxMono)))) 'Effects master switch off: published, effect rows disabled'
    Click (P $IdMaster)
    Check ((Published) -match '"effectsEnabled": true') 'Effects master switch back on'
    Click (P $IdAllOff)
    Check ((Published) -match '"effects": \[\]' -and -not (Checked (P ($IdEffect0 + $IdxMono)))) 'All off clears every effect'

    # --- Feature switches ---------------------------------------------------------------------------------
    Check (Checked (P $IdSmooth)) 'Smooth motion is on by default'
    Click (P $IdSmooth); Check ((Published) -match '"smoothMotion": false') 'Smooth motion off is published'
    Click (P $IdSmooth); Check ((Published) -match '"smoothMotion": true') 'Smooth motion back on is published'
    Click (P $IdGpu); Check ((Published) -match '"processing": "cpu"') 'GPU acceleration off is published (CPU only)'
    Click (P $IdGpu); Check ((Published) -match '"processing": "auto"') 'GPU acceleration back on is published'
    Check (-not (Checked (P $IdAutoFrame))) 'Auto-framing is off by default'
    Click (P $IdAutoFrame); Check ((Published) -match '"autoFraming": true') 'Auto-framing on is published'
    Click (P $IdAutoFrame); Check ((Published) -match '"autoFraming": false') 'Auto-framing back off is published'
    Start-Sleep -Milliseconds 1500
    $threadsOff = $p.Threads.Count; $p.Refresh(); $threadsOff = $p.Threads.Count
    Check (-not (Checked (P $IdFace)) -and -not ([Ui]::Text($status) -match 'face:')) 'Face tracking off by default: not running'
    Click (P $IdFace)
    Start-Sleep -Milliseconds 2200
    Check ((Published) -match '"enabled": true' -and [Ui]::Text($status) -match 'face: (searching|tracking)') 'Face tracking on: published and running'
    $p.Refresh(); $threadsOn = $p.Threads.Count
    Click (P $IdMarkers); Start-Sleep -Milliseconds 300
    Check ((AppSettingsText) -match '"showFaceMarkers": false') 'Show face markers off is saved'
    Click (P $IdMarkers); Start-Sleep -Milliseconds 300
    Check ((AppSettingsText) -match '"showFaceMarkers": true') 'Show face markers back on is saved'
    Click (P $IdFace)
    $pub = Published
    for ($t = 0; $t -lt 15; $t++) { $p.Refresh(); if ($p.Threads.Count -lt $threadsOn) { break }; Start-Sleep -Milliseconds 200 }
    Check ($pub -match '"enabled": false' -and $p.Threads.Count -lt $threadsOn -and -not ([Ui]::Text($status) -match 'face:')) "Face tracking off: its thread stops ($threadsOff -> $threadsOn -> $($p.Threads.Count) threads)"
    Click (P $IdHotkeys); Start-Sleep -Milliseconds 300
    Check ((AppSettingsText) -match '"hotkeysEnabled": false') 'Global hotkeys off is saved'
    Click (P $IdHotkeys); Start-Sleep -Milliseconds 300
    Check ((AppSettingsText) -match '"hotkeysEnabled": true') 'Global hotkeys back on is saved'

    # --- Profiles and hotkeys -------------------------------------------------------------------------------
    $combo = P $IdProfile
    $original = [Ui]::Text($combo)
    [Ui]::SendMessageW($combo, $WM_SETTEXT, [IntPtr]::Zero, (New-Object Text.StringBuilder 'Smoke Test')) | Out-Null
    Click (P $IdProfileSave); Start-Sleep -Milliseconds 500
    Check (Test-Path (Join-Path $dataDir 'profiles\smoke-test.json')) 'Save with a new name creates a profile'
    Check ((AppSettingsText) -match '"smoke-test"') 'the new profile becomes the active one'
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    Check ((Published) -match '"effectsEnabled": false' -and -not (Checked (P $IdMaster))) 'Ctrl+Alt+F8: effects off (published, switch follows)'
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]1, [IntPtr]::Zero) | Out-Null
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]4, [IntPtr]::Zero) | Out-Null
    $pub = Published
    Check ($pub -match '"effectsEnabled": true' -and $pub -match '"mirror": true' -and (Checked (P $IdMirror))) 'Ctrl+Alt+F8 back on; Ctrl+Alt+F11 mirror (switch follows)'
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]4, [IntPtr]::Zero) | Out-Null
    # Lens hotkeys (Ctrl+Alt+F7 next = id 5, F6 previous = id 6): one cycled effect, replaced each time.
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]5, [IntPtr]::Zero) | Out-Null
    $pub = Published
    Check ($pub.Contains('"' + $effectIds[0] + '"') -and (Checked (P $IdEffect0))) 'Ctrl+Alt+F7: first lens on (published, switch follows)'
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]5, [IntPtr]::Zero) | Out-Null
    $pub = Published
    Check ($pub.Contains('"' + $effectIds[1] + '"') -and -not $pub.Contains('"' + $effectIds[0] + '"')) 'Ctrl+Alt+F7 again: next lens replaces it'
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]6, [IntPtr]::Zero) | Out-Null
    [Ui]::PostMessageW($hwnd, $WM_HOTKEY, [IntPtr]6, [IntPtr]::Zero) | Out-Null
    Check ((Published) -match '"effects": \[\]') 'Ctrl+Alt+F6 twice: back to no lens'
    $idx = [int][Ui]::SendMessageW($combo, 0x0158, [IntPtr](-1), (New-Object Text.StringBuilder $original))
    [Ui]::SendMessageW($combo, 0x014E, [IntPtr]$idx, [IntPtr]::Zero) | Out-Null
    [Ui]::SendMessageW($content, 0x0111, [IntPtr]($IdProfile -bor (1 -shl 16)), $combo) | Out-Null
    Start-Sleep -Milliseconds 500
    Check ((AppSettingsText) -match ('"' + $original + '"')) "selecting '$original' switches back"
    [IO.File]::Delete((Join-Path $dataDir 'profiles\smoke-test.json'))

    # --- Persistence across a restart -------------------------------------------------------------------------
    SetSlider (P $IdSlider0) 33
    Click (P ($IdEffect0 + $IdxVivid))    # Vivid on
    SetSlider (P ($IdStrength0 + $IdxVivid)) 55
    Click (P $IdMarkers)                  # face markers off (app setting)
    Start-Sleep -Milliseconds 600
    $p.Refresh(); $mb = $p.PrivateMemorySize64 / 1MB
    Check (Close) 'window closes and process exits cleanly'
    Check ((Launch)) 'relaunch'
    Start-Sleep -Milliseconds 1500
    Check ([int][Ui]::SendMessageW((P $IdSlider0), $TBM_GETPOS, [IntPtr]::Zero, [IntPtr]::Zero) -eq 33) 'restart: Brightness 33 restored'
    Check ((Checked (P ($IdEffect0 + $IdxVivid))) -and [int][Ui]::SendMessageW((P ($IdStrength0 + $IdxVivid)), $TBM_GETPOS, [IntPtr]::Zero, [IntPtr]::Zero) -eq 55) 'restart: Vivid on at 55% restored'
    Check (-not (Checked (P $IdMarkers))) 'restart: Show face markers off restored'
    Click (P $IdReset); Click (P $IdAllOff); Click (P $IdMarkers); Start-Sleep -Milliseconds 500

    # --- Minimize / restore / stop / close ---------------------------------------------------------------------
    for ($i = 0; $i -lt 30 -and [Ui]::Text($start) -ne 'Stop preview'; $i++) { Start-Sleep -Milliseconds 200 }
    [Ui]::ShowWindow($hwnd, 6) | Out-Null; Start-Sleep -Milliseconds 800
    Check ([Ui]::Text($start) -eq 'Start preview') 'minimize stops the preview (camera released)'
    [Ui]::ShowWindow($hwnd, 9) | Out-Null; Start-Sleep -Seconds 3
    Check ([Ui]::Text($start) -eq 'Stop preview' -and ([Ui]::Text($status) -match 'receiving')) 'restore resumes the preview'
    Click $start; Start-Sleep -Milliseconds 500
    Check ([Ui]::Text($start) -eq 'Start preview' -and [Ui]::Text($status) -match 'stopped') 'Stop preview'
    Check ([Ui]::PreviewStats($preview)[1] -lt 30) 'preview cleared after stop'
    "memory:  {0:N1} MB private while previewing; cpu {1:N1}% of one core (UI process, default settings)" -f $mb, $cpuPct
    Check (Close) 'window closes and process exits cleanly (second run)'
} finally {
    if ($script:p -and -not $script:p.HasExited) { $script:p | Stop-Process -Force }
    if (Test-Path "$backup\profiles") { Get-ChildItem "$dataDir\profiles" -Filter *.json | ForEach-Object { [IO.File]::Delete($_.FullName) }; Copy-Item "$backup\profiles\*" "$dataDir\profiles" -Force }
    if (Test-Path "$backup\app-settings.json") { Copy-Item "$backup\app-settings.json" $appSettings -Force }
    if (Test-Path "$backup\active-profile.json") { Copy-Item "$backup\active-profile.json" $settingsFile -Force }
    [IO.Directory]::Delete($backup, $true)
}

if ($failures.Count) { "RESULT: FAIL ($($failures.Count))"; exit 1 }
'RESULT: PASS'; exit 0
