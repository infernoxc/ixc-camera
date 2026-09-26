<#
.SYNOPSIS
    Captures IXC Camera media source TraceLogging events (including from inside Frame Server).
.DESCRIPTION
    Uses only built-in Windows tools (logman, tracerpt). Requires an elevated prompt.
    The provider costs nothing when no trace session is running.
.EXAMPLE
    ./scripts/trace-vcam.ps1 -Start
    ... reproduce ...
    ./scripts/trace-vcam.ps1 -Stop          # prints decoded events
#>
param([switch]$Start, [switch]$Stop, [string]$OutDir = (Join-Path $env:TEMP 'ixc-trace'))

$ErrorActionPreference = 'Stop'
$session = 'IXCCameraSourceTrace'
$provider = '{6880FEE1-8B41-4322-ACF6-E6C1BF585B50}'  # IXC.Camera.Source
$etl = Join-Path $OutDir 'ixc-source.etl'

if ($Start) {
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    logman stop $session -ets 2>$null | Out-Null
    Remove-Item $etl -ErrorAction SilentlyContinue
    logman start $session -p $provider 0xFFFFFFFF 0xFF -o $etl -ets | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "logman start failed ($LASTEXITCODE)" }
    "trace started -> $etl"
}
if ($Stop) {
    logman stop $session -ets | Out-Null
    $xml = Join-Path $OutDir 'ixc-source.xml'
    tracerpt $etl -o $xml -of XML -y | Out-Null
    [xml]$doc = Get-Content $xml -Raw
    foreach ($e in $doc.Events.Event) {
        $name = $e.RenderingInfo.Task
        if (-not $name) { $name = $e.System.Task }
        $fields = @($e.EventData.Data | ForEach-Object { "$($_.Name)=$($_.'#text')" }) -join ' '
        "{0}  {1,-28} {2}" -f ([datetime]$e.System.TimeCreated.SystemTime).ToString('HH:mm:ss.fff'), $name, $fields
    }
}
