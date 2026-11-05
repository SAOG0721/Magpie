#Requires -Version 7.0
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/prepare_overlay_contention_test.py" $outputPath
if ($LASTEXITCODE) { throw 'Production overlay/notification extraction failed' }
foreach ($test in @('overlay-contention','overlay-contention-negative')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "$outputPath/$test.cpp" "/Fe:$outputPath/$test.exe" "/Fo:$outputPath/$test.obj"
    if ($LASTEXITCODE) { throw "CPU test compilation failed: $test" }
    & "$outputPath/$test.exe"
    $expected = if ($test -eq 'overlay-contention') { 0 } else { 42 }
    if ($LASTEXITCODE -ne $expected) { throw "CPU test failed: $test (expected $expected)" }
}
