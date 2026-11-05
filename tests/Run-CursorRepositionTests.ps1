#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-CursorReposition-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$repositionRepo = Split-Path $PSScriptRoot -Parent
$repositionOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $repositionOutput -Force | Out-Null
$repositionVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$repositionVs = & $repositionVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
Import-Module (Join-Path $repositionVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $repositionVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/prepare_cursor_reposition_test.py" $repositionOutput
if ($LASTEXITCODE) { throw 'Production cursor extraction/binding checks failed.' }
foreach ($repositionCase in @('CursorRepositionTests', 'RuntimeQuitAdmissionTests')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE /DMP_ENABLE_FRAME_TRACE `
        "/I$repositionOutput" "/I$repositionRepo/src/Magpie.Core/include" `
        "$PSScriptRoot/$repositionCase.cpp" "/Fe:$repositionOutput/$repositionCase.exe" "/Fo:$repositionOutput/$repositionCase.obj" `
        /link user32.lib coremessaging.lib windowsapp.lib
    if ($LASTEXITCODE) { throw "Production cursor fixture compilation failed: $repositionCase" }
    & "$repositionOutput/$repositionCase.exe"
    if ($LASTEXITCODE) { throw "Production cursor test failed: $repositionCase" }
}
Write-Host "Native dispatcher/coroutine tests passed with mocked USER32 input; no real cursor movement or product/GPU run. Output: $repositionOutput"
