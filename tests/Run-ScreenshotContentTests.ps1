param([string]$OutputDirectory, [switch]$SkipSyntaxCheck, [switch]$SyntaxOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'obj/tests/screenshot-content' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
if (!(Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
    Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$includes = @("$repo/src/Magpie.Core", "$repo/src/Magpie.Core/include", "$repo/src/Magpie", "$repo/src/Shared")
$headers = & rg --files --no-ignore (Join-Path $env:USERPROFILE '.conan2/p') -g document.h -g phmap.h -g format.h -g imgui.h
foreach ($tail in @('[\\/]rapidjson[\\/]document.h$', '[\\/]parallel_hashmap[\\/]phmap.h$', '[\\/]fmt[\\/]format.h$')) {
    $path = $headers | Where-Object { $_ -match $tail -and $_ -match '[\\/]p[\\/]include[\\/]' } | Select-Object -First 1
    if (!$path) { throw "Missing installed dependency: $tail" }
    $includes += Split-Path (Split-Path $path -Parent) -Parent
}
$imgui = $headers | Where-Object { $_ -match '[\\/]p[\\/]include[\\/]imgui.h$' } | Select-Object -First 1
if ($imgui) { $includes += Split-Path $imgui -Parent }
if (!$SkipSyntaxCheck) {
    foreach ($cache in @((Join-Path $env:USERPROFILE '.conan2/p'), (Join-Path $env:USERPROFILE '.conan2/p/b'))) {
        Get-ChildItem -LiteralPath $cache -Directory | ForEach-Object {
            $include = Join-Path $_.FullName 'p/include'
            if (Test-Path -LiteralPath $include) { $includes += $include }
        }
    }
}
$packages = @(Join-Path $repo 'packages')
$ancestor = (Get-Item -LiteralPath $repo).Parent
while ($ancestor) {
    $cache = Join-Path $ancestor.FullName 'source/packages'
    if (Test-Path -LiteralPath $cache) { $packages += $cache; break }
    $ancestor = $ancestor.Parent
}
$wil = $packages | Where-Object { Test-Path -LiteralPath $_ } | ForEach-Object {
    Get-ChildItem -LiteralPath $_ -Directory -Filter 'Microsoft.Windows.ImplementationLibrary.*'
} | Select-Object -First 1
if (!$wil) { throw 'Restore Microsoft.Windows.ImplementationLibrary or provide the workspace package cache.' }
$includes += Join-Path $wil.FullName 'include'
$common = @('/nologo','/std:c++20','/EHsc','/utf-8','/MT','/DNOMINMAX','/DUNICODE','/D_UNICODE','/DFMT_HEADER_ONLY','/DSPDLOG_FMT_EXTERNAL') + @($includes | ForEach-Object { "/I$_" })
if (!$SyntaxOnly) {
& python (Join-Path $PSScriptRoot 'validate_screenshot_content.py')
if ($LASTEXITCODE) { throw 'Screenshot wiring validation failed' }
& python (Join-Path $PSScriptRoot 'prepare_screenshot_save_tests.py') $OutputDirectory
if ($LASTEXITCODE) { throw 'Screenshot disk fixture extraction failed' }
& python (Join-Path $PSScriptRoot 'prepare_hdr_export_tests.py') $OutputDirectory
if ($LASTEXITCODE) { throw 'Existing HDR export fixture extraction failed' }
foreach ($test in @('ScreenshotContentTests', 'ScreenshotHandoffTests', 'ScreenshotSaveTests', 'HdrExportTests', 'config_recovery')) {
    $source = switch ($test) {
        'ScreenshotSaveTests' { Join-Path $OutputDirectory 'ScreenshotSaveTests.generated.cpp' }
        'HdrExportTests' { Join-Path $OutputDirectory 'HdrExportTests.cpp' }
        'config_recovery' { Join-Path $repo 'scripts/tests/config_recovery.cpp' }
        default { Join-Path $PSScriptRoot "$test.cpp" }
    }
    & cl.exe @common $source windowscodecs.lib windowsapp.lib ole32.lib d3d11.lib dxgi.lib "/Fe:$OutputDirectory/$test.exe" "/Fo:$OutputDirectory/$test.obj"
    if ($LASTEXITCODE) { throw "$test compilation failed" }
    & (Join-Path $OutputDirectory "$test.exe") $OutputDirectory
    if ($LASTEXITCODE) { throw "$test failed" }
}
}
if (!$SkipSyntaxCheck) {
    # Syntax only: no Magpie object, executable, package or release is built.
    foreach ($file in @('RendererScreenshot.cpp','Renderer.cpp','ScalingRuntime.cpp','ScreenshotHelper.cpp','TextureHelper.cpp')) {
        & cl.exe @common /Zs (Join-Path $repo "src/Magpie.Core/$file")
        if ($LASTEXITCODE) { throw "Syntax validation failed: $file" }
    }
    Write-Output 'PASS: syntax-only validation of five production Core translation units'
}
