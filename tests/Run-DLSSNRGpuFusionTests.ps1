#Requires -Version 7.0
param([string]$Output = (Join-Path $PSScriptRoot '../diagnostics/four-mode-opt/gpu/warp'), [switch]$Hardware)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$target = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $target -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python (Join-Path $PSScriptRoot 'prepare_dlssnr_detail_test.py') $target
if ($LASTEXITCODE) { throw 'Production shader extraction failed.' }
$exe = Join-Path $target 'DLSSNRGpuFusionTests.exe'
$object = Join-Path $target 'DLSSNRGpuFusionTests.obj'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT /O2 "/I$target" "/I$repo/src/Magpie.Core" (Join-Path $PSScriptRoot 'DLSSNRGpuFusionTests.cpp') "/Fe:$exe" "/Fo:$object" /link d3d11.lib d3dcompiler.lib dxgi.lib
if ($LASTEXITCODE) { throw "GPU fusion WARP test compilation failed ($LASTEXITCODE)." }
$log = Join-Path $target 'DLSSNRGpuFusionTests.stdout.log'
if ($Hardware) { & $exe '--hardware' 2>&1 | Tee-Object -FilePath $log } else { & $exe 2>&1 | Tee-Object -FilePath $log }
if ($LASTEXITCODE) { throw "GPU fusion tests failed ($LASTEXITCODE)." }
