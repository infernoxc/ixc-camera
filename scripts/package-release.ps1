<#
.SYNOPSIS
    Assembles a release folder: installer, source archive, SHA-256 checksums and release notes.
    Nothing is uploaded or published.
.EXAMPLE
    ./scripts/build.ps1 -Preset release -Test
    ./scripts/audit-release.ps1
    ./scripts/package-release.ps1      # -> dist/IXC-Camera-<version>/
#>
param([string]$BuildDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\release'))
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = ((Get-Content (Join-Path $BuildDir 'generated\ixc\version.h')) -match 'IXC_VERSION_STRING' -replace '.*"(.*)".*', '$1') | Select-Object -First 1
if (-not $version) { throw 'Build the release preset first (version.h not found).' }
if (git -C $root status --porcelain) { throw 'The working tree has uncommitted changes: commit them first so the source archive matches.' }

$out = Join-Path $root "dist\IXC-Camera-$version"
if (Test-Path $out) { [IO.Directory]::Delete($out, $true) }
New-Item -ItemType Directory -Force $out | Out-Null

Copy-Item (Join-Path $BuildDir 'src\installer\IXC-Camera-Setup-x64.exe') $out
git -C $root archive --format=zip --prefix="IXC-Camera-$version/" -o (Join-Path $out "IXC-Camera-$version-source.zip") HEAD
if ($LASTEXITCODE -ne 0) { throw 'git archive failed' }
$notes = Join-Path $root "docs\release-notes-$version.md"
if (Test-Path $notes) { Copy-Item $notes (Join-Path $out 'RELEASE_NOTES.md') }
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $out 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_LICENSES.md') $out

$sums = Get-ChildItem $out -File | Sort-Object Name | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name }
[IO.File]::WriteAllText((Join-Path $out 'SHA256SUMS.txt'), ($sums -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
"release folder: $out  (commit $(git -C $root rev-parse --short HEAD))"
Get-ChildItem $out | Select-Object Name, Length | Format-Table -AutoSize | Out-String
Get-Content (Join-Path $out 'SHA256SUMS.txt')
