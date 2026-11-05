#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path (Split-Path $repo -Parent) '.tools/068-multipass/tests'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
# Include the actual production class; only its platform dependencies are doubled.
$source = Get-Content -LiteralPath (Join-Path $repo 'src/Magpie.Core/DLSSNRMultiPass.h') -Raw
$source = [regex]::Replace($source, '(?m)^#include .*\r?\n', '')
$source | Set-Content -LiteralPath (Join-Path $output 'DLSSNRMultiPassUnderTest.h') -Encoding utf8
& python "$repo/tests/prepare_dlssnr_multipass_test.py" $output
if ($LASTEXITCODE) { throw 'DLSSNR Multi Pass metadata/fixture checks failed.' }
foreach ($test in @('DlssNrStatusTests', 'DLSSNRMultiPassTests', 'DLSSNRSettingsTests', 'DLSSNRSessionTests', 'DLSSNRChainCacheTests', 'DLSSNRLiveParametersTests', 'DLSSNRGuidanceTests', 'DLSSNRPerformanceStateTests')) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 "/I$output" "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" "$repo/tests/$test.cpp" "/Fe:$output/$test.exe" "/Fo:$output/$test.obj"
    if ($LASTEXITCODE) { throw "DLSSNR test compilation failed: $test" }
    & "$output/$test.exe"
    if ($LASTEXITCODE) { throw "DLSSNR tests failed: $test" }
}
