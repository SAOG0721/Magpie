#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-PresenterRecovery-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$recoveryRepo = Split-Path $PSScriptRoot -Parent
$recoveryOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $recoveryOutput -Force | Out-Null
$recoveryVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$recoveryVs = & $recoveryVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $recoveryVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $recoveryVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/prepare_adaptive_presenter_recovery_test.py" $recoveryOutput
if ($LASTEXITCODE) { throw 'Presenter recovery production extraction failed.' }
foreach ($recoveryTest in @('recovery', 'early-detach', 'bound-buffer', 'no-retry', 'no-static-redraw')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$recoveryRepo/src/Magpie.Core" `
        "$recoveryOutput/$recoveryTest.cpp" "/Fe:$recoveryOutput/$recoveryTest.exe" "/Fo:$recoveryOutput/$recoveryTest.obj"
    if ($LASTEXITCODE) { throw "Presenter recovery test compilation failed: $recoveryTest" }
    & "$recoveryOutput/$recoveryTest.exe"
    $recoveryExpected = if ($recoveryTest -eq 'recovery') { 0 } else { 42 }
    if ($LASTEXITCODE -ne $recoveryExpected) { throw "Presenter recovery test failed: $recoveryTest (expected $recoveryExpected)" }
    if ($recoveryExpected) { Write-Host "PASS: rejected production mutation $recoveryTest" }
}
