#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-FrameSyncCaptureAhead-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$aheadRepo = Split-Path $PSScriptRoot -Parent
$aheadOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $aheadOutput -Force | Out-Null
& python -B (Join-Path $PSScriptRoot 'prepare_frame_sync_capture_ahead_test.py') $aheadOutput
if ($LASTEXITCODE) { throw 'Production capture-ahead extraction failed.' }
$aheadVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$aheadVs = & $aheadVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $aheadVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $aheadVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
foreach ($aheadVariant in @('production', 'negative')) {
    $aheadInclude = if ($aheadVariant -eq 'negative') { Join-Path $aheadOutput 'negative' } else { $aheadOutput }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$aheadInclude" `
        "/I$aheadRepo/src/Magpie.Core/include" "/I$aheadRepo/src/Magpie.Core" `
        (Join-Path $PSScriptRoot 'FrameSyncCaptureAheadTests.cpp') "/Fe:$aheadOutput/$aheadVariant.exe" "/Fo:$aheadOutput/$aheadVariant.obj" d3d11.lib dxgi.lib user32.lib
    if ($LASTEXITCODE) { throw "Capture-ahead WARP compilation failed: $aheadVariant" }
    & (Join-Path $aheadOutput "$aheadVariant.exe")
    $aheadExpected = if ($aheadVariant -eq 'negative') { 42 } else { 0 }
    if ($LASTEXITCODE -ne $aheadExpected) { throw "Capture-ahead WARP result unexpected: $aheadVariant, exit=$LASTEXITCODE" }
    if ($aheadVariant -eq 'negative') { Write-Host 'PASS: rejected unbounded production capture-admission mutation.' }
}
Write-Host "CPU/WARP capture-ahead tests complete. Output: $aheadOutput"
