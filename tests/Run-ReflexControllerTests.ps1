#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$reflexRepo = Split-Path $PSScriptRoot -Parent
$reflexOutput = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $env:TEMP ('Magpie-Reflex-' + [guid]::NewGuid()) }
New-Item -ItemType Directory -Path $reflexOutput -Force | Out-Null
$reflexVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$reflexVs = & $reflexVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $reflexVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $reflexVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$reflexRepo/src/Magpie.Core" `
    (Join-Path $PSScriptRoot 'ReflexControllerTests.cpp') "/Fe:$reflexOutput/reflex-controller-tests.exe" "/Fo:$reflexOutput/reflex-controller-tests.obj"
if ($LASTEXITCODE) { throw 'Reflex controller test compilation failed' }
& "$reflexOutput/reflex-controller-tests.exe"
if ($LASTEXITCODE) { throw 'Reflex controller test failed' }
& python (Join-Path $PSScriptRoot 'prepare_reflex_marker_boundary_test.py') $reflexOutput
if ($LASTEXITCODE) { throw 'Reflex marker production extraction failed' }
foreach ($reflexTest in @('reflex-marker-boundaries', 'late-copy-marker', 'late-capacity')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$reflexRepo/src/Magpie.Core" `
        "$reflexOutput/$reflexTest.cpp" "/Fe:$reflexOutput/$reflexTest.exe" "/Fo:$reflexOutput/$reflexTest.obj"
    if ($LASTEXITCODE) { throw "Reflex marker test compilation failed: $reflexTest" }
    & "$reflexOutput/$reflexTest.exe"
    $reflexExpected = if ($reflexTest -eq 'reflex-marker-boundaries') { 0 } else { 42 }
    if ($LASTEXITCODE -ne $reflexExpected) { throw "Reflex marker test failed: $reflexTest (expected $reflexExpected)" }
    if ($reflexExpected) { Write-Host "PASS: rejected production mutation $reflexTest" }
}
