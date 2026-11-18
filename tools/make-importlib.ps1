<#
.SYNOPSIS
  Generates build\ue4ss\UE4SS.lib (an import library) from an installed UE4SS.dll's export table.

.DESCRIPTION
  The mod links against UE4SS.dll's exported C++ API (include\ue4ss_abi.hpp). RE-UE4SS's own build needs the
  private UEPseudo submodule, so instead we read the exports of the UE4SS.dll the game actually loads and make an
  import library from them with the MSVC tools (dumpbin + lib, found through vswhere - no hardcoded paths, rule 4).

  The UE4SS.dll must be the SAME build the mod will run against (v4.0.0, git 30afe5ff). Get it from
  https://github.com/UE4SS-RE/RE-UE4SS/releases/tag/v4.0.0 (UE4SS_v4.0.0.zip or zDEV-UE4SS_v4.0.0.zip) if it is not
  installed.

.PARAMETER Ue4ssDll
  Path to UE4SS.dll. Defaults to $env:UE4SS_DLL; fails clearly when neither is given.
#>
param([string]$Ue4ssDll = $env:UE4SS_DLL)

$ErrorActionPreference = 'Stop'
if (-not $Ue4ssDll) { throw "Set UE4SS_DLL (or pass -Ue4ssDll) to the UE4SS.dll the mod will run against (v4.0.0)." }
if (-not (Test-Path $Ue4ssDll)) { throw "UE4SS.dll not found: $Ue4ssDll" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found - install Visual Studio 2022+ with the C++ workload." }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio with the x64 C++ tools found." }
$msvc = Get-ChildItem (Join-Path $vs 'VC\Tools\MSVC') -Directory | Sort-Object Name | Select-Object -Last 1
$bin = Join-Path $msvc.FullName 'bin\Hostx64\x64'
$dumpbin = Join-Path $bin 'dumpbin.exe'
$lib = Join-Path $bin 'lib.exe'

$root = Split-Path $PSScriptRoot -Parent
$outDir = Join-Path $root 'build\ue4ss'
New-Item -ItemType Directory -Force $outDir | Out-Null

$ver = (Get-Item $Ue4ssDll).VersionInfo
$hash = (Get-FileHash $Ue4ssDll -Algorithm SHA256).Hash
Write-Host "UE4SS.dll: $Ue4ssDll (sha256 $hash)"

$names = New-Object System.Collections.Generic.List[string]
$inTable = $false
foreach ($line in (& $dumpbin /nologo /exports $Ue4ssDll)) {
    if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $inTable = $true; continue }
    if (-not $inTable) { continue }
    if ($line -match '^\s*Summary') { break }
    if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { $names.Add($Matches[1]) }
}
if ($names.Count -lt 100) { throw "Only $($names.Count) exports parsed from $Ue4ssDll - is it really UE4SS.dll?" }

$def = Join-Path $outDir 'UE4SS.def'
$text = "LIBRARY UE4SS`r`nEXPORTS`r`n" + (($names | ForEach-Object { "    $_" }) -join "`r`n") + "`r`n"
[System.IO.File]::WriteAllText($def, $text, (New-Object System.Text.UTF8Encoding $false))
$outLib = Join-Path $outDir 'UE4SS.lib'
& $lib /nologo "/def:$def" /machine:x64 "/out:$outLib" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "lib.exe failed ($LASTEXITCODE)" }
"$hash" | Set-Content -Encoding ascii (Join-Path $outDir 'UE4SS.dll.sha256')
Write-Host "Wrote $outDir\UE4SS.lib from $($names.Count) exports."
