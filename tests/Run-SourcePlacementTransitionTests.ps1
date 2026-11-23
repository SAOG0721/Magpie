#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-SourcePlacement-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$placementOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $placementOutput -Force | Out-Null
$placementVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$placementVs = & $placementVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (!$placementVs) { throw 'Visual Studio C++ tools not found.' }
Import-Module (Join-Path $placementVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $placementVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python (Join-Path $PSScriptRoot 'prepare_source_placement_test.py') $placementOutput
if ($LASTEXITCODE) { throw 'Source placement production extraction failed.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT /DNOMINMAX /DUNICODE /D_UNICODE `
    "/I$placementOutput" (Join-Path $PSScriptRoot 'SourcePlacementTransitionTests.cpp') `
    "/Fe:$placementOutput/SourcePlacementTransitionTests.exe" "/Fo:$placementOutput/SourcePlacementTransitionTests.obj"
if ($LASTEXITCODE) { throw 'Source placement CPU test compilation failed.' }
& (Join-Path $placementOutput 'SourcePlacementTransitionTests.exe')
if ($LASTEXITCODE) { throw 'Source placement CPU tests failed.' }
