#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Output)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$outputPath = [IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$test = 'DLSSNRSubmissionWorkerTests'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$repo/src/Magpie.Core/include" "$repo/tests/$test.cpp" "/Fe:$outputPath/$test.exe" "/Fo:$outputPath/$test.obj"
if ($LASTEXITCODE) { throw 'Submission worker CPU test compilation failed' }
& "$outputPath/$test.exe"
if ($LASTEXITCODE) { throw 'Submission worker CPU tests failed' }
