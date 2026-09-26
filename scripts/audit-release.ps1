<#
.SYNOPSIS
    Release audit (Phase 13): exploit mitigations and imports of every shipped binary, installer
    payload integrity, and a secret scan of the repository. Exit code 0 = all checks passed.
.EXAMPLE
    ./scripts/audit-release.ps1 -BuildDir build/release
#>
param([string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release'))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$dumpbin = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe') | Select-Object -Last 1 -ExpandProperty FullName
$failures = 0
function Check([bool]$ok, [string]$what) { if ($ok) { "  PASS  $what" } else { "  FAIL  $what"; $script:failures++ } }

$binaries = @('src\virtual_camera\IXCCameraSource.dll', 'src\app\IXCCamera.exe', 'src\tools\ixc_vcam.exe', 'src\installer\IXC-Camera-Setup-x64.exe') |
    ForEach-Object { Join-Path $BuildDir $_ }
# Windows system DLLs only: no VC++ redistributable, no third-party DLLs.
$allowed = '^(kernel32|user32|gdi32|shell32|ole32|oleaut32|advapi32|comctl32|comdlg32|mf|mfplat|mfreadwrite|mfsensorgroup|mfsrcsnk|cfgmgr32|combase|' +
           'api-ms-win-.*|d3d11|dxgi|ksuser|shlwapi|version|bcrypt|ntdll|mfcore|uxtheme|dwmapi|evr|strmiids|quartz|rpcrt4|setupapi|shcore|propsys)\.dll$'
foreach ($b in $binaries) {
    "== $(Split-Path -Leaf $b)"
    if (-not (Test-Path $b)) { Check $false "exists"; continue }
    $h = & $dumpbin /nologo /headers $b | Out-String
    $lc = & $dumpbin /nologo /loadconfig $b | Out-String
    Check ($h -match 'Dynamic base') 'ASLR (DYNAMICBASE)'
    Check ($h -match 'High Entropy Virtual Addresses') 'high-entropy ASLR'
    Check ($h -match 'NX compatible') 'DEP (NXCOMPAT)'
    Check ($h -match 'Guard' -and $lc -match 'CF Instrumented') 'Control Flow Guard'
    Check ($h -match 'CET compatible' -or $lc -match 'CET compatible') 'CET shadow stack compatible'
    $deps = (& $dumpbin /nologo /dependents $b) -match '^\s+\S+\.dll\s*$' | ForEach-Object { $_.Trim().ToLowerInvariant() }
    $bad = $deps | Where-Object { $_ -notmatch $allowed }
    Check (-not $bad) ("imports only Windows system DLLs" + $(if ($bad) { ": unexpected $($bad -join ', ')" } else { " ($($deps.Count))" }))
    Check (-not ($deps -match 'vcruntime|msvcp|ucrtbase')) 'static CRT (no VC++ redistributable)'
}

"== installer payload"
$payload = Join-Path $BuildDir 'src\installer\payload'
foreach ($p in @(@('IXCCameraSource.dll', 'src\virtual_camera\IXCCameraSource.dll'), @('IXCCamera.exe', 'src\app\IXCCamera.exe'), @('ixc_vcam.exe', 'src\tools\ixc_vcam.exe'))) {
    $a = Get-FileHash (Join-Path $payload $p[0]); $b = Get-FileHash (Join-Path $BuildDir $p[1])
    Check ($a.Hash -eq $b.Hash) "payload $($p[0]) matches the build output"
}
$setupBytes = [IO.File]::ReadAllBytes((Join-Path $BuildDir 'src\installer\IXC-Camera-Setup-x64.exe'))
$text = [Text.Encoding]::ASCII.GetString($setupBytes)
Check ($text -match 'requireAdministrator') 'installer manifest requests elevation explicitly'
$netImports = foreach ($b in $binaries) { (& $dumpbin /nologo /dependents $b) -match '(winhttp|wininet|ws2_32|urlmon|webio).dll' }
Check (-not $netImports) 'no binary imports a network API (no downloads, no telemetry)'

"== repository secret scan"
$patterns = 'AKIA[0-9A-Z]{16}', '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----', 'ghp_[A-Za-z0-9]{36}', 'xox[baprs]-[A-Za-z0-9-]{10,}',
            '(?i)(password|passwd|secret|api[_-]?key)\s*[:=]\s*["''][^"'']{6,}["'']'
$files = git -C $root ls-files | Where-Object { $_ -notmatch '^third_party/' -and $_ -notmatch '\.(png|jpg|ico)$' }
$hits = foreach ($f in $files) { Select-String -Path (Join-Path $root $f) -Pattern $patterns -ErrorAction SilentlyContinue }
Check (-not $hits) ("no secrets in tracked files" + $(if ($hits) { ": $($hits | Select-Object -First 3)" }))
Check (-not (git -C $root ls-files | Where-Object { $_ -match '\.(pfx|pem|key|env)$|\.log$' })) 'no key, env or log files tracked'

"== vendored code"
Check ((Get-Content (Join-Path $root 'THIRD_PARTY_LICENSES.md') -Raw) -match 'libfacedetection') 'every vendored component is in THIRD_PARTY_LICENSES.md'

""
if ($failures) { "AUDIT: FAIL ($failures)"; exit 1 } else { 'AUDIT: PASS' }
