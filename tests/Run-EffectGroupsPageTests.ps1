#Requires -Version 7.0
param([string]$CachedRepoDirectory, [string]$RuntimeDirectory,
    [string]$IntermediateRoot, [string]$OutputDirectory,
    [string]$GeneratedFilesDirectory, [switch]$SyntaxOnly, [switch]$PairedCheckboxesOnly, [switch]$SinglePageOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$CachedRepoDirectory) {
    $common = & git -C $repo rev-parse --git-common-dir
    if (![IO.Path]::IsPathRooted($common)) { $common = Join-Path $repo $common }
    $CachedRepoDirectory = Split-Path ([IO.Path]::GetFullPath($common)) -Parent
}
$workspace = Split-Path $CachedRepoDirectory -Parent
if (!$RuntimeDirectory) { $RuntimeDirectory = Join-Path $workspace 'release/v0.6.9-beta1/Magpie-Experimental-x64' }
if (!$IntermediateRoot) { $IntermediateRoot = Join-Path $workspace 'release/v0.6.9-beta1/obj' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $workspace '.tools/069-effect-groups-page/tests' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if ($SinglePageOnly) {
    & python (Join-Path $PSScriptRoot 'prepare_parameter_single_page_test.py') $OutputDirectory
    if ($LASTEXITCODE) { throw 'Production single-page extraction failed' }
} elseif ($PairedCheckboxesOnly) {
    & python (Join-Path $PSScriptRoot 'prepare_paired_checkbox_test.py') $OutputDirectory
    if ($LASTEXITCODE) { throw 'Production paired-checkbox extraction failed' }
} else {
    & python (Join-Path $PSScriptRoot 'prepare_effect_groups_page_tests.py') $OutputDirectory
    if ($LASTEXITCODE) { throw 'Production layout extraction failed' }
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$winmd = Join-Path $CachedRepoDirectory 'packages/Microsoft.UI.Xaml.2.8.7/lib/uap10.0/Microsoft.UI.Xaml.winmd'
$manifest = Join-Path $OutputDirectory 'WinUI.manifest'
& mt.exe -nologo "-winmd:$winmd" '-dll:Microsoft.UI.Xaml.dll' "-out:$manifest"
if ($LASTEXITCODE) { throw 'Generate WinUI test activation manifest failed' }
# Read cached dependencies only. The test has its own Application, with no
# Magpie App.xbf/config/startup, and writes no product runtime/intermediates.
Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'Microsoft.UI.Xaml.dll') -Destination $OutputDirectory -Force
$winuiPri = Join-Path $IntermediateRoot 'x64/Release/x64/WinUI/Microsoft.UI.Xaml.pri'
if (!(Test-Path -LiteralPath $winuiPri)) { $winuiPri = Join-Path $IntermediateRoot 'x64/WinUI/Microsoft.UI.Xaml.pri' }
Copy-Item -LiteralPath $winuiPri -Destination (Join-Path $OutputDirectory 'resources.pri') -Force
$cachedGenerated = Join-Path $IntermediateRoot 'x64/Release/Magpie/Generated Files'
if (!$GeneratedFilesDirectory) { $GeneratedFilesDirectory = $cachedGenerated }
$generated = [IO.Path]::GetFullPath($GeneratedFilesDirectory)
if (!(Test-Path -LiteralPath (Join-Path $generated 'winrt/Magpie.h'))) {
    throw 'Current Magpie C++/WinRT projection is missing; specify GeneratedFilesDirectory.'
}
$includes = @("/I$repo/src/Magpie", "/I$repo/src/Shared", "/I$repo/src/Magpie.Core/include", "/I$OutputDirectory", "/I$generated",
    "/I$cachedGenerated",
    "/I$CachedRepoDirectory/packages/Microsoft.Windows.ImplementationLibrary.1.0.260126.7/include")
# Reuse the cached dependency include paths, including fmt, without rebuilding
# or overwriting the application's PCH, generated XAML files or EXE.
$commands = Get-Content -LiteralPath (Join-Path $IntermediateRoot 'x64/Release/Magpie/Magpie.tlog/CL.command.1.tlog') -Raw
foreach ($match in [regex]::Matches($commands, '(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
    $include = $match.Groups['quoted'].Value
    if (!$include) { $include = $match.Groups['bare'].Value }
    if (![IO.Path]::IsPathRooted($include)) { $include = Join-Path (Join-Path $CachedRepoDirectory 'src/Magpie') $include }
    if ($include -notmatch '(?i)\\src\\magpie\\?$|generated files|beta1-fix\d-work') { $includes += "/I$include" }
}
$includes = @($includes | Select-Object -Unique)
$defines = @('/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DWINRT_NO_MODULE_LOCK',
    '/DWIL_SUPPRESS_EXCEPTIONS', '/DWIL_USE_STL=1', '/D_UNICODE', '/DUNICODE')
if ($SyntaxOnly) {
    $syntaxDefines = @()
    foreach ($match in [regex]::Matches($commands, '/D\s+(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
        $define = $match.Groups['quoted'].Value
        if (!$define) { $define = $match.Groups['bare'].Value }
        $syntaxDefines += "/D$define"
    }
    $syntaxDefines = @($syntaxDefines | Select-Object -Unique)
    & cl.exe /nologo /Zs /std:c++20 /EHsc /utf-8 /bigobj /W4 /WX @syntaxDefines @includes (Join-Path $repo 'src/Magpie/ScalingModesPage.cpp')
    if ($LASTEXITCODE) { throw 'Changed page C++ syntax check failed' }
    Write-Output 'PASS changed page C++ syntax: MSVC /W4 /WX, cached generated headers; no application build or link.'
    return
}
if ($SinglePageOnly) {
    $sources = @((Join-Path $PSScriptRoot 'ParameterSinglePageLayoutTests.cpp'), (Join-Path $repo 'src/Magpie/SimpleStackPanel.cpp'), (Join-Path $repo 'src/Magpie/WrapPanel.cpp'), (Join-Path $repo 'src/Shared/SmallVector.cpp'))
} elseif ($PairedCheckboxesOnly) {
    $sources = @((Join-Path $PSScriptRoot 'PairedCheckboxLayoutTests.cpp'), (Join-Path $repo 'src/Magpie/SimpleStackPanel.cpp'))
} else {
    $sources = @((Join-Path $PSScriptRoot 'EffectGroupsPageLayoutTests.cpp'), (Join-Path $repo 'src/Magpie/SimpleStackPanel.cpp'))
}
$exe = Join-Path $OutputDirectory $(if ($SinglePageOnly) { 'ParameterSinglePageLayoutTests.exe' } elseif ($PairedCheckboxesOnly) { 'PairedCheckboxLayoutTests.exe' } else { 'EffectGroupsPageLayoutTests.exe' })
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /bigobj @defines @includes @sources "/Fe:$exe" "/Fo:$OutputDirectory/" `
    /link WindowsApp.lib user32.lib /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot/EffectChoiceItemsTests.manifest" "/MANIFESTINPUT:$manifest"
if ($LASTEXITCODE) { throw 'Compile native page layout regression failed' }
Push-Location $OutputDirectory
try { & $exe; if ($LASTEXITCODE) { throw 'Native page layout regression failed' } }
finally { Pop-Location }
if ($SinglePageOnly) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX "/I$OutputDirectory" `
        (Join-Path $PSScriptRoot 'ParameterSinglePageStateTests.cpp') "/Fe:$OutputDirectory/ParameterSinglePageStateTests.exe" "/Fo:$OutputDirectory/ParameterSinglePageStateTests.obj"
    if ($LASTEXITCODE) { throw 'Compile visible-group state tests failed' }
    & (Join-Path $OutputDirectory 'ParameterSinglePageStateTests.exe')
    if ($LASTEXITCODE) { throw 'Production pager state tests failed' }
}
if ($PairedCheckboxesOnly -or $SinglePageOnly) { return }
# Existing hidden-presenter regression guards the shared panel behavior.
$containers = Join-Path $OutputDirectory 'Beta1Fix2ContainerTests.exe'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /bigobj @defines @includes `
    (Join-Path $PSScriptRoot 'Beta1Fix2ContainerTests.cpp') (Join-Path $repo 'src/Magpie/SimpleStackPanel.cpp') `
    "/Fe:$containers" "/Fo:$OutputDirectory/" /link WindowsApp.lib user32.lib /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot/EffectChoiceItemsTests.manifest"
if ($LASTEXITCODE) { throw 'Compile existing container regression failed' }
& $containers
if ($LASTEXITCODE) { throw 'Existing container regression failed' }
$folder = Join-Path $OutputDirectory 'EffectGroupsConfigFolderTests.exe'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /DFMT_HEADER_ONLY @includes `
    (Join-Path $PSScriptRoot 'EffectGroupsConfigFolderTests.cpp') "/Fe:$folder" "/Fo:$OutputDirectory/EffectGroupsConfigFolderTests.obj"
if ($LASTEXITCODE) { throw 'Compile production folder-handler checks failed' }
& $folder
if ($LASTEXITCODE) { throw 'Production folder-handler checks failed' }
Write-Output 'PASS production folder handler: ordinary/portable paths, success/failure, localized feedback and resolved path; no shell launch.'
