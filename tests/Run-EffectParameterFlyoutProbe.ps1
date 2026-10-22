#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../.tools/parameter-flyout-probe'),
    [ValidateSet('programmatic','mouse','keyboard')][string]$Mode = 'programmatic',
    [string]$RuntimeDirectory, [string]$IntermediateRoot)
$ErrorActionPreference = 'Stop'
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$exe = Join-Path $OutputDirectory 'EffectParameterFlyoutProbe.exe'
$compileArgs = @()
$linkArgs = @()
if ($RuntimeDirectory -or $IntermediateRoot) {
    if (!$RuntimeDirectory -or !$IntermediateRoot) { throw 'Specify both runtime and intermediate roots to test WinUI.' }
    $common = & git -C $PSScriptRoot rev-parse --path-format=absolute --git-common-dir
    $cachedRepo = Split-Path $common -Parent
    $winmd = Join-Path $cachedRepo 'packages/Microsoft.UI.Xaml.2.8.7/lib/uap10.0/Microsoft.UI.Xaml.winmd'
    $manifest = Join-Path $OutputDirectory 'WinUI.manifest'
    & mt.exe -nologo "-winmd:$winmd" '-dll:Microsoft.UI.Xaml.dll' "-out:$manifest"
    if ($LASTEXITCODE) { throw 'Generate WinUI activation manifest failed' }
    Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'Microsoft.UI.Xaml.dll') -Destination $OutputDirectory -Force
    Copy-Item -LiteralPath (Join-Path $IntermediateRoot 'x64/Release/x64/WinUI/Microsoft.UI.Xaml.pri') -Destination (Join-Path $OutputDirectory 'resources.pri') -Force
    $compileArgs = @('/DMAGPIE_FLYOUT_PROBE_WINUI', "/I$IntermediateRoot/x64/Release/Magpie/Generated Files")
    $linkArgs = @("/MANIFESTINPUT:$manifest")
}
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /DUNICODE /D_UNICODE @compileArgs (Join-Path $PSScriptRoot 'EffectParameterFlyoutProbe.cpp') "/Fe:$exe" "/Fo:$OutputDirectory/" /link WindowsApp.lib user32.lib /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot/EffectChoiceItemsTests.manifest" @linkArgs
if ($LASTEXITCODE) { throw 'Compile native flyout probe failed' }
& $exe $Mode 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory "events-$Mode.log")
if ($LASTEXITCODE) { throw 'Native flyout probe failed; inspect events.log' }
