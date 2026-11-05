#Requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=Join-Path $repo 'obj/tests/oklab-editor'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$imgui=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib') } | Select-Object -First 1
if(!$imgui) { throw 'Existing ImGui dependency not found.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$($imgui.FullName)/p/include" "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" `
    "$repo/tests/OKLabEditorTests.cpp" "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib "/Fe:$output/OKLabEditorTests.exe" "/Fo:$output/OKLabEditorTests.obj"
if($LASTEXITCODE) { throw 'Editor test compilation failed.' }
& "$output/OKLabEditorTests.exe"
if($LASTEXITCODE) { throw 'Editor test failed.' }
$rapid=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if(!$rapid) { throw 'Existing RapidJSON dependency not found.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$($rapid.FullName)/p/include" "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie" `
    "$repo/tests/OKLabConfigTests.cpp" "/Fe:$output/OKLabConfigTests.exe" "/Fo:$output/OKLabConfigTests.obj"
if($LASTEXITCODE) { throw 'Config test compilation failed.' }
& "$output/OKLabConfigTests.exe"
if($LASTEXITCODE) { throw 'Config test failed.' }
