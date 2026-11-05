#Requires -Version 7.0
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=Join-Path $repo 'obj/tests/single-page-overlay'
New-Item -ItemType Directory -Force $output | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$imgui=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib') } | Select-Object -First 1
if(!$imgui) { throw 'Existing ImGui dependency not found.' }

$fmt=Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'fmt*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/fmt/format.h') } | Select-Object -First 1
if(!$fmt) { throw 'Existing fmt dependency not found.' }
& python "$repo/tests/prepare_single_page_overlay_test.py" $output
if($LASTEXITCODE) { throw 'Production layout extraction failed.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$output" "/I$($fmt.FullName)/p/include" "/I$($imgui.FullName)/p/include" "/I$repo/src/Magpie.Core/include" "/I$repo/src/Magpie.Core" `
    "$repo/tests/SinglePageOverlayTests.cpp" "$($imgui.FullName)/p/lib/imgui.lib" user32.lib imm32.lib "/Fe:$output/SinglePageOverlayTests.exe" "/Fo:$output/SinglePageOverlayTests.obj"
if($LASTEXITCODE) { throw 'Single-page overlay test compilation failed.' }
& "$output/SinglePageOverlayTests.exe"
if($LASTEXITCODE) { throw 'Single-page overlay test failed.' }
