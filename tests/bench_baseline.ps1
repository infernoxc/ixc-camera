<#
.SYNOPSIS
    Repeatable resource baseline for IXC Camera (run before and after a feature to compare).
.DESCRIPTION
    Measures, with the installed IXC Camera:
      * app startup time (process start -> main window visible) and idle memory;
      * IXC Camera open -> first frame (10 cycles);
      * a 20 s IXC Camera stream: delivered FPS, capture -> app latency, dropped frames;
      * the Windows camera service (Frame Server, which hosts IXC's source): CPU and private
        memory while idle and while streaming.
    Uses the active-profile file only if -Profile is given (restored afterwards).
    Requires an elevated prompt (reads the service process) and no other app using the camera.
.EXAMPLE
    ./tests/bench_baseline.ps1 -Label "before face tracking"
#>
param(
    [string]$Label = 'baseline',
    [string]$Probe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release\src\tools\ixc_probe.exe'),
    [string]$App = (Join-Path $env:ProgramFiles 'IXC Camera\IXCCamera.exe'),
    [string]$Profile = '',
    [string]$ProfileFile = ''   # safer than -Profile across processes (quotes survive)
)
$ErrorActionPreference = 'Stop'
if ($ProfileFile) { $Profile = [IO.File]::ReadAllText((Resolve-Path $ProfileFile)) }
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class W { [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
                        [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
                        [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l); }
'@
function FsProcess { $id = (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId; if ($id) { Get-Process -Id $id } }
function Measure-Fs([int]$seconds) {
    $p = FsProcess; if (-not $p) { return [pscustomobject]@{ Cpu = 0; Mb = 0 } }
    $c0 = $p.TotalProcessorTime.TotalSeconds; $t0 = Get-Date; Start-Sleep -Seconds $seconds; $p.Refresh()
    [pscustomobject]@{ Cpu = 100 * ($p.TotalProcessorTime.TotalSeconds - $c0) / ((Get-Date) - $t0).TotalSeconds; Mb = $p.PrivateMemorySize64 / 1MB }
}
if (Get-Process obs64, Discord, IXCCamera -ErrorAction SilentlyContinue) { throw 'Close OBS/Discord/IXC Camera first: the camera must be free.' }

$settings = Join-Path $env:ProgramData 'IXC Camera\active-profile.json'
$saved = if (Test-Path $settings) { [IO.File]::ReadAllText($settings) } else { $null }

try {
    # 1. App startup (3 runs, median)
    $starts = @(); $appMb = 0
    for ($i = 0; $i -lt 3; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew(); $p = Start-Process $App -PassThru
        $h = [IntPtr]::Zero
        while ($sw.ElapsedMilliseconds -lt 10000) { $h = [W]::FindWindowW('IXCCameraMainWindow', [NullString]::Value); if ($h -ne [IntPtr]::Zero -and [W]::IsWindowVisible($h)) { break }; Start-Sleep -Milliseconds 5 }
        $starts += $sw.ElapsedMilliseconds
        Start-Sleep -Milliseconds 1500; $p.Refresh(); $appMb = $p.PrivateMemorySize64 / 1MB
        [W]::PostMessageW($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null; $p.WaitForExit(5000) | Out-Null
    }
    $startMs = ($starts | Sort-Object)[1]

    # The app publishes its own saved profile when it starts: apply the requested one afterwards.
    if ($Profile) { [IO.File]::WriteAllText($settings, $Profile, (New-Object Text.UTF8Encoding($false))) }

    # 2. Camera service idle
    Start-Sleep -Seconds 3; $idle = Measure-Fs 5

    # 3. Open -> first frame
    $cyc = & $Probe --cycles 10 --camera IXC --output nv12
    $open = ($cyc | Select-String 'open->first frame').Line -replace '.*frame:\s*', ''

    # 4. Streaming 20 s
    $out = "$env:TEMP\ixc-baseline-capture.txt"
    $cap = Start-Process $Probe -ArgumentList '--capture 20 --camera IXC --output nv12' -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 8; $stream = Measure-Fs 8; $cap.WaitForExit(60000) | Out-Null
    $c = Get-Content $out
    $fps = (($c | Select-String 'frames received').Line -replace '.*\(avg\s*', '') -replace '\).*', ''
    $lat = ($c | Select-String 'capture->app latency').Line -replace '.*latency:\s*', ''
    $drop = ($c | Select-String 'frames dropped').Line -replace '.*dropped:\s*', ''
} finally {
    if ($Profile) { if ($null -ne $saved) { [IO.File]::WriteAllText($settings, $saved, (New-Object Text.UTF8Encoding($false))) } else { [IO.File]::Delete($settings) } }
}

"=== IXC Camera resource baseline: $Label ==="
"app startup (median of 3):        {0} ms   | app idle private memory: {1:N1} MB" -f $startMs, $appMb
"camera service idle:              cpu {0:N2}% of one core | private {1:N1} MB" -f $idle.Cpu, $idle.Mb
"IXC Camera open -> first frame:   $open"
"streaming (20 s):                 $fps | latency $lat | dropped $drop"
"camera service while streaming:   cpu {0:N2}% of one core | private {1:N1} MB" -f $stream.Cpu, $stream.Mb
