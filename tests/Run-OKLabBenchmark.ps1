#Requires -Version 7.0
param([string]$ProductionShaderDirectory = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$ProductionShaderDirectory) { $ProductionShaderDirectory = Join-Path $repo 'obj/tests/oklab-compiler' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$output = Join-Path $repo 'obj/tests/oklab'
New-Item -ItemType Directory -Path $output -Force | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /DNOMINMAX "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" "$repo/tests/OKLabBenchmark.cpp" "/Fe:$output/OKLabBenchmark.exe" "/Fo:$output/OKLabBenchmark.obj" /link d3d11.lib d3dcompiler.lib dxgi.lib
if ($LASTEXITCODE) { throw 'OKLab benchmark compilation failed' }
& "$output/OKLabBenchmark.exe" $ProductionShaderDirectory
if ($LASTEXITCODE) { throw 'OKLab benchmark failed' }
