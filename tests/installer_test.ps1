<#
.SYNOPSIS
    Installer lifecycle test (elevated; changes the system): install/upgrade, Start Menu and
    desktop shortcuts, launching through the desktop shortcut, settings kept on upgrade,
    uninstall, reinstall. Run only with no camera apps open.
#>
param([string]$Setup = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release\src\installer\IXC-Camera-Setup-x64.exe'),
      [string]$ExpectedVersion = '')
$ErrorActionPreference = 'Stop'
$failures = @()
function Check([bool]$ok, [string]$what) { if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:failures += $what } }
$startLnk = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\IXC Camera.lnk'
$deskLnk = Join-Path $env:PUBLIC 'Desktop\IXC Camera.lnk'
$exe = Join-Path $env:ProgramFiles 'IXC Camera\IXCCamera.exe'
$key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\IXC Camera'
$profiles = Join-Path $env:LOCALAPPDATA 'IXC Camera\profiles'
$shell = New-Object -ComObject WScript.Shell
function Lnk($path) { $s = $shell.CreateShortcut($path); [pscustomobject]@{ Target = $s.TargetPath; Icon = $s.IconLocation } }
function RunSetup([string]$args_) { (Start-Process $Setup -ArgumentList $args_ -Wait -PassThru).ExitCode }
function VcamPresent { (& (Join-Path $env:ProgramFiles 'IXC Camera\ixc_vcam.exe') status 2>&1 | Out-String) -match 'system camera present:\s+yes' }
if (Get-Process IXCCamera, obs64 -ErrorAction SilentlyContinue) { throw 'Close IXC Camera and OBS first.' }

$profileHash = if (Test-Path $profiles) { (Get-ChildItem $profiles -Filter *.json | Get-FileHash | ForEach-Object Hash) -join ',' } else { '' }

"== install / upgrade"
$code = RunSetup '/quiet'
Check ($code -in 0, 3010) "setup /quiet exit code $code (0, or 3010 = a file freed at restart)"
foreach ($l in $startLnk, $deskLnk) {
    $info = if (Test-Path $l) { Lnk $l } else { $null }
    Check ($info -and $info.Target -ieq $exe -and $info.Icon -like "$exe*") "shortcut $l -> IXCCamera.exe with the IXC icon"
}
$reg = Get-ItemProperty $key -ErrorAction SilentlyContinue
Check ($reg -and (-not $ExpectedVersion -or $reg.DisplayVersion -eq $ExpectedVersion)) "Apps & features entry (version $($reg.DisplayVersion))"
Check (VcamPresent) 'IXC Camera system camera registered'
$after = if (Test-Path $profiles) { (Get-ChildItem $profiles -Filter *.json | Get-FileHash | ForEach-Object Hash) -join ',' } else { '' }
Check ($after -eq $profileHash) 'user profiles untouched by install/upgrade'

"== reinstall over the top (no duplicates)"
$code = RunSetup '/quiet'
Check ($code -in 0, 3010) "second setup /quiet exit code $code"
Check (@(Get-ChildItem (Split-Path $startLnk) -Filter 'IXC Camera*.lnk').Count -eq 1 -and @(Get-ChildItem (Split-Path $deskLnk) -Filter 'IXC Camera*.lnk').Count -eq 1) 'exactly one Start Menu and one desktop shortcut'

"== launch through the desktop shortcut"
Start-Process $deskLnk
$proc = $null
for ($i = 0; $i -lt 50 -and -not $proc; $i++) { Start-Sleep -Milliseconds 200; $proc = Get-Process IXCCamera -ErrorAction SilentlyContinue | Select-Object -First 1 }
Check ($proc -and $proc.Path -ieq $exe) 'desktop shortcut starts the installed IXC Camera app'
if ($proc) {
    Start-Sleep -Seconds 4
    Check ($proc.MainWindowTitle -eq 'IXC Camera') 'its control panel window opens'
    $proc.CloseMainWindow() | Out-Null
    if (-not $proc.WaitForExit(6000)) { $proc | Stop-Process -Force }
}

"== uninstall"
$code = RunSetup '/uninstall /quiet'
Check ($code -in 0, 3010) "uninstall exit code $code"
Check (-not (Test-Path $startLnk) -and -not (Test-Path $deskLnk)) 'both shortcuts removed'
Check (-not (Test-Path $key)) 'Apps & features entry removed'
Check (-not (Test-Path 'HKLM:\SOFTWARE\Classes\CLSID\{3011A045-BC7A-469D-86D0-2800938E32BF}')) 'COM registration removed'
Check ((Test-Path $profiles) -or -not $profileHash) 'user profiles kept (no full reset requested)'

"== reinstall"
$code = RunSetup '/quiet'
Check ($code -in 0, 3010 -and (Test-Path $deskLnk) -and (VcamPresent)) "reinstall works (exit $code)"

if ($failures.Count) { "RESULT: FAIL ($($failures.Count))"; exit 1 }
'RESULT: PASS'; exit 0
