#Requires -Version 7.0
param([string]$Output = (Join-Path $PSScriptRoot '../out/tests/dlssnr-sparse'))
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$target=[IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $target -Force | Out-Null
$exe=Join-Path $target 'DLSSNRSparseStateTests.exe'
$object=Join-Path $target 'DLSSNRSparseStateTests.obj'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT (Join-Path $PSScriptRoot 'DLSSNRSparseStateTests.cpp') "/Fe:$exe" "/Fo:$object" /link d3dcompiler.lib
if ($LASTEXITCODE) { throw "Sparse CPU test compilation failed ($LASTEXITCODE)." }
& $exe
if ($LASTEXITCODE) { throw "Sparse CPU tests failed ($LASTEXITCODE)." }
