<#
.SYNOPSIS
    Installs or removes IXC Camera (system camera + app) on this PC. Requires administrator.
.DESCRIPTION
    Install:   copies binaries to %ProgramFiles%\IXC Camera, registers the media source COM
               class (HKLM, this DLL's CLSID only), and registers the "IXC Camera" system camera.
               Any failure rolls back everything done so far.
    Uninstall: removes the system camera, the COM registration and the installed files.
               -RemoveUserData also deletes %LOCALAPPDATA%\IXC Camera (profiles, logs).

    Registry footprint: HKLM\Software\Classes\CLSID\{3011A045-...} and HKLM\Software\IXC Camera only.
    Nothing else is touched: no services, drivers, startup entries, browser settings or other cameras.
    A DLL still loaded by the Windows camera service is renamed and deleted at next reboot
    instead of stopping the service (which would interrupt other apps using cameras).

    This is the development installer; the packaged IXC-Camera-Setup-x64.exe comes in Phase 12.

    Exit codes: 0 ok, 1 failed (rolled back), 2 unsupported system, 3 not administrator.
.EXAMPLE
    ./scripts/install-ixc.ps1 -BuildDir build/release
    ./scripts/install-ixc.ps1 -Uninstall
#>
[CmdletBinding()]
param(
    [string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build/release'),
    [switch]$Uninstall,
    [switch]$RemoveUserData,
    [string]$Camera = ''
)

$ErrorActionPreference = 'Stop'
$InstallDir = Join-Path $env:ProgramFiles 'IXC Camera'
$Clsid = '{3011A045-BC7A-469D-86D0-2800938E32BF}'
$ClsidKey = "HKLM:\SOFTWARE\Classes\CLSID\$Clsid"
$ManifestName = 'install-manifest.json'
$Log = Join-Path $env:TEMP 'ixc-install.log'
$Files = @(
    @{ Name = 'IXCCameraSource.dll'; From = 'src/virtual_camera/IXCCameraSource.dll' },
    @{ Name = 'ixc_vcam.exe';        From = 'src/tools/ixc_vcam.exe' },
    @{ Name = 'IXCCamera.exe';       From = 'src/app/IXCCamera.exe' }
)

function Write-Log([string]$msg) {
    $line = "{0}  {1}" -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $msg
    Add-Content -Path $Log -Value $line -Encoding utf8
    Write-Host $msg
}

function Invoke-Native([string]$stage, [string]$exe, [string[]]$arguments, [int[]]$ok = @(0)) {
    $out = & $exe @arguments 2>&1 | Out-String
    $code = $LASTEXITCODE
    Write-Log ("[{0}] {1} {2} -> exit {3}{4}" -f $stage, (Split-Path -Leaf $exe), ($arguments -join ' '), $code, $(if ($out.Trim()) { "`n    " + $out.Trim().Replace("`n", "`n    ") } else { '' }))
    if ($ok -notcontains $code) { throw "Stage '$stage' failed with exit code $code." }
}

# Deletes a file; if it's in use (DLL loaded by the camera service), rename it aside and
# schedule the renamed copy for deletion at next reboot.
Add-Type -Namespace IxcNative -Name Kernel -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool MoveFileEx(string existing, string newName, int flags);
'@
function Remove-FileSafely([string]$path) {
    if (-not (Test-Path -LiteralPath $path)) { return }
    try {
        Remove-Item -LiteralPath $path -Force
        Write-Log "removed $path"
    } catch {
        $aside = "$path.old-$([DateTime]::Now.ToString('yyyyMMddHHmmss'))"
        Rename-Item -LiteralPath $path -NewName (Split-Path -Leaf $aside) -Force
        Register-DeleteAtReboot $aside
        Write-Log "in use: renamed to $aside (deleted at next reboot)"
    }
}

# [NullString]::Value is required: PowerShell would pass $null to a string parameter as "".
function Register-DeleteAtReboot([string]$path) {
    if (-not [IxcNative.Kernel]::MoveFileEx($path, [NullString]::Value, 4)) {  # MOVEFILE_DELAY_UNTIL_REBOOT
        throw "could not schedule '$path' for deletion at reboot (Win32 error $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))"
    }
}

# Files still loaded by the camera service are renamed *.old-*; make sure every one of them and
# the then-empty folder are removed at the next reboot (files first, folder last).
function Register-LeftoversForReboot {
    if (-not (Test-Path $InstallDir)) { return }
    $left = @(Get-ChildItem $InstallDir -Force -Filter '*.old-*')
    foreach ($f in $left) { Register-DeleteAtReboot $f.FullName }
    if ($left.Count -and -not (Get-ChildItem $InstallDir -Force | Where-Object { $_.Name -notlike '*.old-*' })) {
        Register-DeleteAtReboot $InstallDir
        Write-Log "$InstallDir and $($left.Count) in-use file(s) will be removed at next reboot"
    }
}

function Assert-Prerequisites {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host 'Administrator rights are required. Run this from an elevated PowerShell.'; exit 3
    }
    $build = [int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
    if ($build -lt 22000) { Write-Host "Windows 11 (build 22000+) is required; this PC is build $build."; exit 2 }
    if (-not [Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64') {
        Write-Host 'IXC Camera requires 64-bit (x64) Windows.'; exit 2
    }
    return $build
}

# Every step runs even if an earlier one fails, so a failed rollback never strands the rest.
# Throws at the end, listing the steps that failed.
function Uninstall-Ixc([bool]$quietIfAbsent) {
    $step ={ param($name, [scriptblock]$body) try { & $body } catch { Write-Log "step $name failed: $($_.Exception.Message)"; $script:failedSteps += $name } }
    $script:failedSteps = @()

    $vcam = Join-Path $InstallDir 'ixc_vcam.exe'
    & $step 'RemoveSystemCamera' {
        if (Test-Path $vcam) { Invoke-Native 'RemoveSystemCamera' $vcam @('unregister') }
        elseif (-not $quietIfAbsent) { Write-Log 'ixc_vcam.exe not found; skipping system camera removal' }
    }
    & $step 'RemoveSettingsKey' {
        # IXC-owned key (wrapped camera record); ixc_vcam removes it too, this covers a missing tool.
        if (Test-Path 'HKLM:\SOFTWARE\IXC Camera') { Remove-Item 'HKLM:\SOFTWARE\IXC Camera' -Recurse -Force; Write-Log 'removed HKLM\SOFTWARE\IXC Camera' }
    }
    $dll = Join-Path $InstallDir 'IXCCameraSource.dll'
    & $step 'UnregisterComServer' {
        if (Test-Path $ClsidKey) {
            if (Test-Path $dll) { Invoke-Native 'UnregisterComServer' "$env:WINDIR\System32\regsvr32.exe" @('/s', '/u', $dll) }
            if (Test-Path $ClsidKey) { Remove-Item -Path $ClsidKey -Recurse -Force; Write-Log "removed $ClsidKey" }
        }
    }
    & $step 'RemoveFiles' {
        $manifest = Join-Path $InstallDir $ManifestName
        $names = if (Test-Path $manifest) { (Get-Content $manifest -Raw | ConvertFrom-Json).files | ForEach-Object { $_.name } } else { $Files | ForEach-Object { $_.Name } }
        foreach ($n in $names) { Remove-FileSafely (Join-Path $InstallDir $n) }
        Remove-FileSafely $manifest
        if (Test-Path $InstallDir) {
            if (-not (Get-ChildItem $InstallDir -Force)) { Remove-Item $InstallDir -Force; Write-Log "removed $InstallDir" }
            elseif (-not (Get-ChildItem $InstallDir -Force | Where-Object { $_.Name -notlike '*.old-*' })) { Write-Log "$InstallDir kept until reboot (files pending deletion)" }
        }
    }
    if ($script:failedSteps.Count) { throw "uninstall steps failed: $($script:failedSteps -join ', ')" }
}

# ------------------------------------------------------------------------------------------------
"" | Set-Content -Path $Log -Encoding utf8
$build = Assert-Prerequisites
Write-Log "IXC Camera $(if ($Uninstall) { 'uninstall' } else { 'install' }) on Windows build $build; log: $Log"

if ($Uninstall) {
    try {
        Uninstall-Ixc $false
        Register-LeftoversForReboot
        if ($RemoveUserData) {
            $data = Join-Path $env:LOCALAPPDATA 'IXC Camera'
            if (Test-Path $data) { Remove-Item $data -Recurse -Force; Write-Log "removed user data $data" }
        }
        Write-Log 'Uninstall complete.'
        exit 0
    } catch {
        Write-Log "UNINSTALL FAILED: $($_.Exception.Message)"
        exit 1
    }
}

# Install ------------------------------------------------------------------------------------------
foreach ($f in $Files) {
    $src = Join-Path $BuildDir $f.From
    if (-not (Test-Path $src)) { Write-Log "Missing build output: $src (build the release preset first)."; exit 1 }
}

try {
    # Upgrade in place: remove any previous installation first (keeps user data).
    if (Test-Path $InstallDir) { Write-Log 'existing installation found; replacing it'; Uninstall-Ixc $true }

    New-Item -ItemType Directory -Force $InstallDir | Out-Null
    $entries = @()
    foreach ($f in $Files) {
        $dest = Join-Path $InstallDir $f.Name
        Copy-Item -LiteralPath (Join-Path $BuildDir $f.From) -Destination $dest -Force
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $dest).Hash
        $entries += [ordered]@{ name = $f.Name; sha256 = $hash }
        Write-Log "installed $dest  sha256=$hash"
    }
    [ordered]@{ product = 'IXC Camera'; installedUtc = (Get-Date).ToUniversalTime().ToString('o'); files = $entries } |
        ConvertTo-Json -Depth 4 | Set-Content -Path (Join-Path $InstallDir $ManifestName) -Encoding utf8

    Invoke-Native 'RegisterComServer' "$env:WINDIR\System32\regsvr32.exe" @('/s', (Join-Path $InstallDir 'IXCCameraSource.dll'))
    $registered = (Get-ItemProperty "$ClsidKey\InprocServer32" -ErrorAction SilentlyContinue).'(default)'
    if ($registered -ne (Join-Path $InstallDir 'IXCCameraSource.dll')) { throw "COM registration points to '$registered'." }

    $vcamArgs = @('register'); if ($Camera) { $vcamArgs += @('--camera', $Camera) }
    Invoke-Native 'RegisterSystemCamera' (Join-Path $InstallDir 'ixc_vcam.exe') $vcamArgs
    Invoke-Native 'VerifySystemCamera' (Join-Path $InstallDir 'ixc_vcam.exe') @('status')
    Write-Log 'Install complete.'
    exit 0
} catch {
    Write-Log "INSTALL FAILED: $($_.Exception.Message) -- rolling back"
    try { Uninstall-Ixc $true } catch { Write-Log "rollback problem: $($_.Exception.Message)" }
    exit 1
}
