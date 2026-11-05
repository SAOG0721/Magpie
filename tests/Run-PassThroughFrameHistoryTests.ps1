#Requires -Version 7.0
param([string]$Output)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$Output) { $Output = Join-Path $repo 'obj/tests/pass-through-history' }
New-Item -ItemType Directory -Path $Output -Force | Out-Null
& python -B (Join-Path $PSScriptRoot 'prepare_passthrough_frame_history_test.py') $Output
if ($LASTEXITCODE) { throw 'Original-reference CPU test extraction failed.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$Output" "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" (Join-Path $PSScriptRoot 'PassThroughFrameHistoryTests.cpp') "/Fe:$Output/PassThroughFrameHistoryTests.exe" "/Fo:$Output/PassThroughFrameHistoryTests.obj"
if ($LASTEXITCODE) { throw 'Original-reference CPU test compilation failed.' }
& (Join-Path $Output 'PassThroughFrameHistoryTests.exe')
if ($LASTEXITCODE) { throw 'Original-reference CPU identity tests failed.' }
