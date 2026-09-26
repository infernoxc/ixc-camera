<#
.SYNOPSIS
    Configure, build and (optionally) test IXC Camera from any PowerShell prompt.
.DESCRIPTION
    Locates Visual Studio (Build Tools or IDE) with vswhere, enters the x64 developer
    environment in-process, then runs the CMake preset. No global PATH changes.
.EXAMPLE
    ./scripts/build.ps1 -Preset debug -Test
    ./scripts/build.ps1 -Preset release -Test
    ./scripts/build.ps1 -Preset asan -Test
#>
[CmdletBinding()]
param(
    [ValidateSet('debug', 'release', 'asan')]
    [string]$Preset = 'debug',
    [switch]$Test,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Enter-IxcDevShell {
    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64' -and (Get-Command cl.exe -ErrorAction SilentlyContinue)) { return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload (see MISSING_TOOLS.md).' }

    $env:PATH = (Split-Path $vswhere) + ';' + $env:PATH  # VsDevCmd.bat invokes vswhere by name
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio installation with the x64 C++ tools was found.' }

    Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}

function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { throw "$what failed with exit code $LASTEXITCODE" }
}

Enter-IxcDevShell
Push-Location $root
try {
    if ($Clean -and (Test-Path "build/$Preset")) { Remove-Item -Recurse -Force "build/$Preset" }
    Invoke-Checked 'configure' { cmake --preset $Preset }
    Invoke-Checked 'build'     { cmake --build --preset $Preset }
    if ($Test) {
        Invoke-Checked 'tests' { ctest --preset $Preset }
    }
} finally {
    Pop-Location
}
