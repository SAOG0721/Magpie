#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$CachedIntermediateRoot,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$output=[IO.Path]::GetFullPath($OutputDirectory)
$cached=Join-Path $CachedIntermediateRoot 'x64/Release/Magpie'
$generated=Join-Path $cached 'Generated Files'
New-Item -ItemType Directory -Path $output -Force | Out-Null
& python (Join-Path $PSScriptRoot 'prepare_oklab_binding_test.py') $output
if($LASTEXITCODE){throw 'Prepare binding regression failed'}
& (Join-Path $PSScriptRoot 'Validate-OKLabXaml.ps1') -CachedIntermediateRoot $CachedIntermediateRoot -CachedRepoDirectory $repo `
    -OutputDirectory $output -PagePath (Join-Path $output 'ScalingModesPage.xaml') `
    -HeaderPaths (Join-Path $output 'ScalingModesPage.h') -MetadataPath (Join-Path $cached 'Merged/Magpie.winmd') `
    -ComponentResourceLocation application
foreach($name in @('ScalingModesPage.xaml.g.h','ScalingModesPage.xaml.g.hpp','ScalingModesPage.xbf','XamlBindingInfo.xaml.g.h','XamlBindingInfo.xaml.g.hpp')){
    Copy-Item -LiteralPath (Join-Path "$output/Xaml" $name) -Destination $output -Force
}
Copy-Item -LiteralPath (Join-Path $generated 'ScalingModesPage.g.h') -Destination $output -Force
Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'Microsoft.UI.Xaml.dll') -Destination $output -Force
$pri=Join-Path $CachedIntermediateRoot 'x64/Release/x64/WinUI/Microsoft.UI.Xaml.pri'
Copy-Item -LiteralPath $pri -Destination (Join-Path $output 'resources.pri') -Force
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& mt.exe -nologo "-winmd:$repo/packages/Microsoft.UI.Xaml.2.8.7/lib/uap10.0/Microsoft.UI.Xaml.winmd" '-dll:Microsoft.UI.Xaml.dll' "-out:$output/WinUI.manifest"
if($LASTEXITCODE){throw 'WinUI activation manifest failed'}
$includes=@("/I$output","/I$repo/src/Magpie","/I$repo/src/Shared","/I$repo/src/Magpie.Core/include","/I$generated")
$commands=Get-Content -LiteralPath (Join-Path $cached 'Magpie.tlog/CL.command.1.tlog') -Raw
foreach($match in [regex]::Matches($commands,'(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')){
    $inc=$match.Groups['quoted'].Value
    if(!$inc){$inc=$match.Groups['bare'].Value}
    if(![IO.Path]::IsPathRooted($inc)){$inc=Join-Path "$repo/src/Magpie" $inc}
    $includes+="/I$inc"
}
$includes=@($includes | Select-Object -Unique)
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /bigobj /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DWINRT_NO_MODULE_LOCK /DWIL_SUPPRESS_EXCEPTIONS /DWIL_USE_STL=1 /D_UNICODE /DUNICODE @includes `
    (Join-Path $PSScriptRoot 'OKLabBindingTests.cpp') "/Fe:$output/OKLabBindingTests.exe" "/Fo:$output/OKLabBindingTests.obj" `
    /link WindowsApp.lib user32.lib /MANIFEST:EMBED "/MANIFESTINPUT:$PSScriptRoot/EffectChoiceItemsTests.manifest" "/MANIFESTINPUT:$output/WinUI.manifest"
if($LASTEXITCODE){throw 'Compile native binding regression failed'}
Push-Location $output
try { & './OKLabBindingTests.exe'; if($LASTEXITCODE){throw 'Native binding regression failed'} } finally {Pop-Location}
