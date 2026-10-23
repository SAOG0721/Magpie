#Requires -Version 7.0
param([string]$OutputDirectory=(Join-Path $env:TEMP ('Magpie-profiler-'+[guid]::NewGuid())))
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX (Join-Path $PSScriptRoot 'ProfilerHistoryTests.cpp') "/Fe:$OutputDirectory/ProfilerHistoryTests.exe" "/Fo:$OutputDirectory/ProfilerHistoryTests.obj"
if($LASTEXITCODE){throw 'Profiler history compile failed'}
& (Join-Path $OutputDirectory 'ProfilerHistoryTests.exe')
if($LASTEXITCODE){throw 'Profiler history tests failed'}
$imgui=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib')} | Select-Object -First 1
$fmt=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'fmt*' | Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/fmt/format.h')} | Select-Object -First 1
if(!$imgui -or !$fmt){throw 'Restore ImGui and fmt dependencies before profiler UI tests'}
& python (Join-Path $PSScriptRoot 'prepare_profiler_ui_test.py') $OutputDirectory
if($LASTEXITCODE){throw 'Production UI extraction failed'}
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$OutputDirectory" (Join-Path $PSScriptRoot 'ProfilerPresentationTests.cpp') "/Fe:$OutputDirectory/ProfilerPresentationTests.exe" "/Fo:$OutputDirectory/ProfilerPresentationTests.obj"
if($LASTEXITCODE){throw 'Profiler presentation compile failed'}
& (Join-Path $OutputDirectory 'ProfilerPresentationTests.exe')
if($LASTEXITCODE){throw 'Profiler presentation tests failed'}
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$OutputDirectory" "/I$($imgui.FullName)/p/include" "/I$($fmt.FullName)/p/include" (Join-Path $PSScriptRoot 'ProfilerUiTests.cpp') "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib "/Fe:$OutputDirectory/ProfilerUiTests.exe" "/Fo:$OutputDirectory/ProfilerUiTests.obj"
if($LASTEXITCODE){throw 'Profiler UI compile failed'}
& (Join-Path $OutputDirectory 'ProfilerUiTests.exe')
if($LASTEXITCODE){throw 'Profiler UI tests failed'}
