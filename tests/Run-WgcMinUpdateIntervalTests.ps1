#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-WgcMinUpdateInterval-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$wgcIntervalRepo = Split-Path $PSScriptRoot -Parent
$wgcIntervalOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $wgcIntervalOutput -Force | Out-Null
& python -B (Join-Path $PSScriptRoot 'prepare_wgc_min_update_interval_test.py') $wgcIntervalOutput
if ($LASTEXITCODE) { throw 'Production WGC interval extraction failed.' }
$wgcIntervalVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$wgcIntervalVs = & $wgcIntervalVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $wgcIntervalVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $wgcIntervalVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$wgcIntervalRepo/src/Magpie.Core" "/I$wgcIntervalOutput" `
    (Join-Path $PSScriptRoot 'WgcMinUpdateIntervalTests.cpp') "/Fe:$wgcIntervalOutput/WgcMinUpdateIntervalTests.exe" "/Fo:$wgcIntervalOutput/WgcMinUpdateIntervalTests.obj"
if ($LASTEXITCODE) { throw 'WGC interval CPU test compilation failed.' }
& (Join-Path $wgcIntervalOutput 'WgcMinUpdateIntervalTests.exe')
if ($LASTEXITCODE) { throw 'WGC interval CPU regression failed.' }
