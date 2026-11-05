#Requires -Version 7.0
# Compilation is intentional: run only in the separately authorized build/test session.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path (Split-Path $repo -Parent) '.tools/dlssnr-performance/tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
foreach ($test in @('DLSSNRPerformanceStateTests', 'DLSSNRPerformanceShaderTests')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 "/I$repo/src/Magpie.Core" "/I$repo/src/Magpie.Core/include" "$repo/tests/$test.cpp" "/Fe:$output/$test.exe" "/Fo:$output/$test.obj" /link d3d11.lib d3dcompiler.lib
    if ($LASTEXITCODE) { throw "Performance test compilation failed: $test" }
    & "$output/$test.exe"
    if ($LASTEXITCODE) { throw "Performance test failed: $test" }
}
