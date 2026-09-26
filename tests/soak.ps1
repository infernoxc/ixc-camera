<#
.SYNOPSIS
    Long-duration test: streams IXC Camera for N minutes and samples the camera service's CPU and
    memory, to find leaks (memory drift), slowdowns and stalls. Frames are never saved.
.EXAMPLE
    ./tests/soak.ps1 -Minutes 30 -ProfileFile $env:TEMP\ixc-prof-fx.json
#>
param(
    [int]$Minutes = 30,
    [string]$ProfileFile = '',
    [string]$Probe = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release\src\tools\ixc_probe.exe')
)
$ErrorActionPreference = 'Stop'
$settings = Join-Path $env:ProgramData 'IXC Camera\active-profile.json'
$saved = if (Test-Path $settings) { [IO.File]::ReadAllText($settings) } else { $null }
$enc = New-Object Text.UTF8Encoding($false)
if ($ProfileFile) { [IO.File]::WriteAllText($settings, [IO.File]::ReadAllText((Resolve-Path $ProfileFile)), $enc) }
$out = Join-Path $env:TEMP 'ixc-soak-capture.txt'
try {
    $seconds = $Minutes * 60
    $cap = Start-Process $Probe -ArgumentList "--capture $seconds --camera IXC --output nv12" -RedirectStandardOutput $out -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 20
    $fs = Get-Process -Id (Get-CimInstance Win32_Service -Filter "Name='FrameServer'").ProcessId
    $rows = @()
    $c0 = $fs.TotalProcessorTime.TotalSeconds; $t0 = Get-Date
    while (-not $cap.HasExited) {
        Start-Sleep -Seconds 30
        $fs.Refresh()
        $c1 = $fs.TotalProcessorTime.TotalSeconds; $t1 = Get-Date
        $rows += [pscustomobject]@{ Min = [math]::Round(($t1 - $t0).TotalMinutes, 1); Cpu = [math]::Round(100 * ($c1 - $c0) / ($t1 - $t0).TotalSeconds, 1)
                                    Mb = [math]::Round($fs.PrivateMemorySize64 / 1MB, 1); Handles = $fs.HandleCount }
        $c0 = $c1; $t0 = $t1
    }
} finally {
    if ($ProfileFile) { if ($null -ne $saved) { [IO.File]::WriteAllText($settings, $saved, $enc) } else { [IO.File]::Delete($settings) } }
}
$rows | Format-Table -AutoSize | Out-String -Width 200
$first = $rows | Select-Object -First 3; $last = $rows | Select-Object -Last 3
"memory: first {0:N1} MB -> last {1:N1} MB (min {2:N1}, max {3:N1})" -f ($first | Measure-Object Mb -Average).Average, ($last | Measure-Object Mb -Average).Average, ($rows | Measure-Object Mb -Minimum).Minimum, ($rows | Measure-Object Mb -Maximum).Maximum
"cpu: average {0:N1}% of one core (max {1:N1})" -f ($rows | Measure-Object Cpu -Average).Average, ($rows | Measure-Object Cpu -Maximum).Maximum
"handles: first {0} -> last {1}" -f $rows[0].Handles, $rows[-1].Handles
Get-Content $out | Select-String 'frames received|dropped|stream ticks|interval mean|latency|UNDER-SPEED|final state'
