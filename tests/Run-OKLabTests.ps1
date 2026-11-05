#Requires -Version 7.0
param([string]$ProductionShaderDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'obj/tests/oklab'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$repo/tools/generate_oklab_parameters.py" --check
if ($LASTEXITCODE) { throw 'OKLab metadata is stale' }
if (!$ProductionShaderDirectory) { $ProductionShaderDirectory = Join-Path $repo 'obj/tests/oklab-compiler' }
if (!(Test-Path (Join-Path $ProductionShaderDirectory 'fp32-pass2.cso'))) { throw 'Run Run-OKLabCompilerTests.ps1 first to produce the actual effect compiler shaders.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /DNOMINMAX "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" "$repo/tests/OKLabTests.cpp" "/Fe:$output/OKLabTests.exe" "/Fo:$output/OKLabTests.obj" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE) { throw 'OKLab test compilation failed' }
& "$output/OKLabTests.exe" $ProductionShaderDirectory
if ($LASTEXITCODE) { throw 'OKLab tests failed' }
