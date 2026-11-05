param(
    [string]$CachedIntermediateRoot = 'C:/Users/81443/Documents/Magpie enhance/release/v0.6.10-beta2/obj',
    [string]$CachedRepoDirectory = 'C:/Users/81443/Documents/Magpie enhance/.worktrees/0610-beta2/source',
    [string]$OutputDirectory,
    [string[]]$SourceFiles,
    [switch]$SkipProjection
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'obj/tests/hdr-source-validation' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cachedMagpie = Join-Path $CachedIntermediateRoot 'x64/Release/Magpie'
$generated = Join-Path $OutputDirectory 'Generated'
$metadata = Join-Path $OutputDirectory 'Unmerged'
New-Item -ItemType Directory -Path $generated,$metadata -Force | Out-Null
if (!$SkipProjection) {
Copy-Item -Path (Join-Path $cachedMagpie 'Generated Files/*') -Destination $generated -Recurse -Force
Copy-Item -Path (Join-Path $cachedMagpie 'Unmerged/*.winmd') -Destination $metadata -Force
$response = Join-Path $cachedMagpie 'Magpie.vcxproj.midlrt.rsp'
foreach ($idl in @('App')) {
    & midl.exe /nologo /winrt /nomidl /env x64 /char signed /h NUL /dlldata NUL /iid NUL /proxy NUL /notlb /client none /server none /enum_class /ns_prefix /target NT60 `
        /metadata_dir "$env:WindowsSdkDir/References/$env:WindowsSDKVersion/Windows.Foundation.FoundationContract/4.0.0.0" `
        /winmd (Join-Path $metadata "$idl.winmd") "@$response" (Join-Path $repo "src/Magpie/$idl.idl") *> (Join-Path $OutputDirectory "$idl-metadata.log")
    if ($LASTEXITCODE) { throw "$idl metadata validation failed" }
}
$cppwinrt = Join-Path $CachedRepoDirectory 'packages/Microsoft.Windows.CppWinRT.3.0.260520.1/bin/cppwinrt.exe'
& $cppwinrt -input (Join-Path $metadata 'App.winmd') -input (Join-Path $metadata 'XamlMetaDataProvider.winmd') -reference "$env:WindowsSdkDir/UnionMetadata/$env:WindowsSDKVersion/Windows.winmd" `
    -reference (Join-Path $CachedRepoDirectory 'packages/Microsoft.UI.Xaml.2.8.7/lib/uap10.0/Microsoft.UI.Xaml.winmd') `
    -reference (Join-Path $CachedRepoDirectory 'packages/Microsoft.Web.WebView2.1.0.4078.44/lib/Microsoft.Web.WebView2.Core.winmd') `
    -output $generated -component -pch pch.h -optimize
if ($LASTEXITCODE) { throw 'Updated projection validation failed' }
foreach ($class in @('EffectParametersViewModel','EffectParameterGroupViewModel','ScalingModeParameter','ProfileViewModel','ScalingModeItem','ScalingModeEffectItem')) {
    foreach ($suffix in @('.g.h','.g.cpp')) {
        $path = Join-Path $generated "Magpie/$class$suffix"
        if (Test-Path -LiteralPath $path) { Copy-Item -LiteralPath $path -Destination $generated -Force }
    }
}
} elseif (!(Test-Path -LiteralPath (Join-Path $generated 'winrt/Magpie.h'))) {
    throw 'Run projection validation before reusing the isolated generated headers.'
}
$sourceChecks = 0
foreach ($project in @('Magpie','Magpie.Core','RtxVideoBridge')) {
    if ($SourceFiles -and !@($SourceFiles | Where-Object { $_.StartsWith("$project/") }).Count) { continue }
    $commands = Get-Content -LiteralPath (Join-Path $CachedIntermediateRoot "x64/Release/$project/$project.tlog/CL.command.1.tlog") -Raw
    $includes = @("/I$generated", "/I$repo/src/$project", "/I$repo/src/Shared", "/I$repo/src/Magpie.Core/include")
    foreach ($match in [regex]::Matches($commands, '(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
        $path = $match.Groups['quoted'].Value
        if (!$path) { $path = $match.Groups['bare'].Value }
        if (![IO.Path]::IsPathRooted($path)) { $path = Join-Path (Join-Path $CachedRepoDirectory "src/$project") $path }
        if ($path -notmatch '(?i)\\src\\(?:magpie|magpie.core|shared)\\?$|generated files') { $includes += "/I$path" }
    }
    $defines = @()
    foreach ($match in [regex]::Matches($commands, '/D\s+(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
        $define = $match.Groups['quoted'].Value
        if (!$define) { $define = $match.Groups['bare'].Value }
        $defines += "/D$define"
    }
    $files = switch ($project) {
        'Magpie' { @('AppSettings.cpp','EffectParametersViewModel.cpp','EffectParametersViewModel.OKLab.cpp','ScalingModesPage.cpp','ScalingMode.cpp','ScalingModeItem.cpp','ScalingModeEffectItem.cpp','ScalingModesService.cpp','ScalingService.cpp') }
        'Magpie.Core' { @('TextureHelper.cpp','RTXVideoHdr.cpp','EffectCompiler.cpp','EffectCacheManager.cpp','EffectDrawer.cpp','OverlayDrawer.cpp','Renderer.cpp','Renderer.OKLab.cpp','ScalingWindow.cpp','ScalingRuntime.cpp') }
        'RtxVideoBridge' { @('RtxVideoBridge.cpp') }
    }
    foreach ($file in $files) {
        if ($SourceFiles -and "$project/$file" -notin $SourceFiles) { continue }
        & cl.exe /nologo /Zs /std:c++20 /EHsc /utf-8 /bigobj /W3 @($defines | Select-Object -Unique) @($includes | Select-Object -Unique) (Join-Path $repo "src/$project/$file")
        if ($LASTEXITCODE) { throw "$project/$file source validation failed" }
        Write-Output "PASS source syntax: $project/$file"
        ++$sourceChecks
    }
}
if (!$sourceChecks) { throw 'No requested source files were validated' }
Write-Output "PASS $sourceChecks source syntax checks using isolated projection; no application EXE/DLL build, link or deployment."
