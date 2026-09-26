<#
.SYNOPSIS
    Browser compatibility test: a Chromium browser (Edge, Chrome or Brave) opens IXC Camera through
    getUserMedia at 720p and 1080p and reports the delivered frame rate. Frames are only counted,
    never displayed or saved.
.DESCRIPTION
    Runs the browser headless with a throwaway profile (no access to the user's browser data) and
    --use-fake-ui-for-media-stream (auto-accepts the camera prompt for this test profile only).
    A localhost-only listener serves tests/browser_camera.html and receives its result.
#>
param([string]$Browser = '', [int]$Port = 8765, [int]$TimeoutSec = 60)
$ErrorActionPreference = 'Stop'
if (-not $Browser) {
    $Browser = @("${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe", "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
                 "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe", "$env:ProgramFiles\BraveSoftware\Brave-Browser\Application\brave.exe",
                 # A 32-bit PowerShell (as ctest may start) sees "Program Files (x86)" as ProgramFiles.
                 "$env:ProgramW6432\Google\Chrome\Application\chrome.exe", "$env:ProgramW6432\BraveSoftware\Brave-Browser\Application\brave.exe") |
        Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Browser) { 'NOT TESTED: no Chromium browser found'; exit 77 }
# The 64-bit registry view explicitly: ctest may start a 32-bit PowerShell, which would otherwise
# look at the (empty) WOW6432Node copy of the COM registration.
$clsid = [Microsoft.Win32.RegistryKey]::OpenBaseKey('LocalMachine', 'Registry64').OpenSubKey('SOFTWARE\Classes\CLSID\{3011A045-BC7A-469D-86D0-2800938E32BF}\InprocServer32')
if (-not $clsid) { 'NOT TESTED: IXC Camera is not installed'; exit 77 }
$clsid.Close()
$page = [IO.File]::ReadAllBytes((Join-Path $PSScriptRoot 'browser_camera.html'))
$profileDir = Join-Path $env:TEMP ("ixc-browser-test-" + [guid]::NewGuid())
$listener = New-Object Net.HttpListener
$listener.Prefixes.Add("http://localhost:$Port/")
$listener.Start()
$proc = Start-Process $Browser -ArgumentList "--headless=new --user-data-dir=`"$profileDir`" --use-fake-ui-for-media-stream --no-first-run --autoplay-policy=no-user-gesture-required http://localhost:$Port/" -PassThru
$result = $null
$deadline = (Get-Date).AddSeconds($TimeoutSec)
try {
    while (-not $result -and (Get-Date) -lt $deadline) {
        $ctxTask = $listener.GetContextAsync()
        while (-not $ctxTask.Wait(500)) { if ((Get-Date) -gt $deadline) { break } }
        if (-not $ctxTask.IsCompleted) { break }
        $c = $ctxTask.Result
        if ($c.Request.HttpMethod -eq 'POST') {
            $result = (New-Object IO.StreamReader($c.Request.InputStream)).ReadToEnd()
        } elseif ($c.Request.Url.AbsolutePath -eq '/') {
            $c.Response.ContentType = 'text/html; charset=utf-8'
            $c.Response.OutputStream.Write($page, 0, $page.Length)
        } else { $c.Response.StatusCode = 404 }
        $c.Response.Close()
    }
} finally {
    $listener.Stop()
    Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like "*$profileDir*" } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if (Test-Path $profileDir) { [IO.Directory]::Delete($profileDir, $true) }
}
"browser: $Browser"
if (-not $result) { 'FAIL: no result (timeout)'; exit 1 }
$result
if ($result -match 'ERROR|not found') { exit 1 }
