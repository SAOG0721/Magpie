#Requires -Version 7.0
param([switch]$NativePrototype, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$workspace = Split-Path $repo -Parent
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else {
    Join-Path $workspace '.tools/067-beta4/parameter-input'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$imguiPackage = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p/b') -Directory -Filter 'imgui*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/lib/imgui.lib') } | Select-Object -First 1
if (!$imguiPackage) { throw 'Restore imgui before running parameter input tests.' }
$imgui = Join-Path $imguiPackage.FullName 'p'
New-Item -ItemType Directory -Force $output | Out-Null
foreach ($test in @('parameter_input', 'overlay_window_layout', 'cursor_letterboxing')) {
    & python (Join-Path $repo "scripts/tests/test_$test.py") $output
    if ($LASTEXITCODE) { throw "Extract $test failed" }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /DNOMINMAX "/I$repo/src/Magpie.Core/include" "/I$imgui/include" "$output/$test.cpp" "$imgui/lib/imgui.lib" user32.lib imm32.lib "/Fe:$output/$test.exe" "/Fo:$output/$test.obj"
    if ($LASTEXITCODE) { throw "Compile $test failed" }
    & "$output/$test.exe"
    if ($LASTEXITCODE) { throw "$test failed" }
}
& python (Join-Path $repo 'scripts/tests/test_parameter_focus_settings.py') $output
if ($LASTEXITCODE) { throw 'Extract parameter focus setting failed' }
$rapid = Get-ChildItem -LiteralPath (Join-Path $env:USERPROFILE '.conan2/p') -Directory -Filter 'rapid*' |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'p/include/rapidjson/document.h') } | Select-Object -First 1
if (!$rapid) { throw 'Restore rapidjson before running setting tests.' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "/I$($rapid.FullName)/p/include" "$output/parameter_focus_settings.cpp" "/Fe:$output/parameter_focus_settings.exe" "/Fo:$output/parameter_focus_settings.obj"
if ($LASTEXITCODE) { throw 'Compile parameter focus setting failed' }
& "$output/parameter_focus_settings.exe"
if ($LASTEXITCODE) { throw 'Parameter focus setting failed' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "/I$($rapid.FullName)/p/include" "$output/task_switch_settings.cpp" "/Fe:$output/task_switch_settings.exe" "/Fo:$output/task_switch_settings.obj"
if ($LASTEXITCODE) { throw 'Compile task-switch setting failed' }
& "$output/task_switch_settings.exe"
if ($LASTEXITCODE) { throw 'Task-switch setting failed' }
& python (Join-Path $repo 'scripts/tests/test_task_switch_option.py') $output
if ($LASTEXITCODE) { throw 'Extract task-switch routing failed' }
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "$output/task_switch_option.cpp" "/Fe:$output/task_switch_option.exe" "/Fo:$output/task_switch_option.obj"
if ($LASTEXITCODE) { throw 'Compile task-switch routing failed' }
& "$output/task_switch_option.exe"
if ($LASTEXITCODE) { throw 'Task-switch routing failed' }
if ($NativePrototype) {
    # Creates two small test-owned windows, tests real input routing and restores
    # the previous cursor/focus. No game or Magpie configuration is modified.
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 "$repo/scripts/tests/parameter_input_host_prototype.cpp" "/Fe:$output/prototype.exe" "/Fo:$output/prototype.obj"
    if ($LASTEXITCODE) { throw 'Compile native input prototype failed' }
    & "$output/prototype.exe"
    if ($LASTEXITCODE) { throw 'Native input prototype failed' }
}
