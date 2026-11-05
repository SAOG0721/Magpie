#Requires -Version 7.0
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$dwmRepo = Split-Path $PSScriptRoot -Parent
$dwmOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $dwmOutput -Force | Out-Null
& python -B (Join-Path $PSScriptRoot 'prepare_dwm_capture_test.py') $dwmOutput
if ($LASTEXITCODE) { throw 'Production DWM capture extraction failed.' }
# Reuse the existing production base-pipeline test: a recovered sequence must
# bypass pixel duplicate filtering once, then resume ordinary filtering.
& python -B (Join-Path $PSScriptRoot 'prepare_duplicate_capture_pipeline_test.py') $dwmOutput
if ($LASTEXITCODE) { throw 'Production capture pipeline extraction failed.' }
$dwmVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$dwmVs = & $dwmVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
Import-Module (Join-Path $dwmVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $dwmVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
foreach ($dwmCase in @('dwm_capture', 'duplicate_capture_pipeline', 'unbounded-copy', 'unavailable-source')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX `
        "/I$dwmRepo/src/Magpie.Core" "$dwmOutput/$dwmCase.cpp" `
        "/Fe:$dwmOutput/$dwmCase.exe" "/Fo:$dwmOutput/$dwmCase.obj"
    if ($LASTEXITCODE) { throw "DWM capture CPU test compilation failed: $dwmCase" }
    & "$dwmOutput/$dwmCase.exe"
    $dwmExpected = if ($dwmCase -in @('unbounded-copy', 'unavailable-source')) { 42 } else { 0 }
    if ($LASTEXITCODE -ne $dwmExpected) { throw "DWM capture CPU test failed: $dwmCase (expected $dwmExpected, exit=$LASTEXITCODE)" }
    if ($dwmExpected) { Write-Host "PASS: rejected production DWM mutation $dwmCase" }
}
