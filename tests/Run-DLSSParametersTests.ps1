#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'obj/tests/dlss-parameters'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$repo/tests/prepare_dlss_parameters_test.py" $output
if ($LASTEXITCODE) { throw 'DLSS parameter descriptor preparation failed' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /DNOMINMAX "/I$output" "/I$repo/src/Shared" "/I$repo/src/Magpie.Core/include" "$repo/tests/DLSSParametersTests.cpp" "/Fe:$output/DLSSParametersTests.exe" "/Fo:$output/DLSSParametersTests.obj"
if ($LASTEXITCODE) { throw 'DLSS parameter compilation failed' }
& "$output/DLSSParametersTests.exe"
if ($LASTEXITCODE) { throw 'DLSS parameter regression failed' }
