#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$CachedIntermediateRoot,
    [Parameter(Mandatory)][string]$CachedRepoDirectory,
    [string]$OutputDirectory,
    [string]$PagePath,
    [string[]]$HeaderPaths,
    [string]$MetadataPath,
    [ValidateSet('nested','application')][string]$ComponentResourceLocation = 'nested'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$CachedRepoDirectory = [IO.Path]::GetFullPath($CachedRepoDirectory)
$CachedIntermediateRoot = [IO.Path]::GetFullPath($CachedIntermediateRoot)
$output = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repo 'obj/tests/source' }
if (!$PagePath) { $PagePath = Join-Path $repo 'src/Magpie/ScalingModesPage.xaml' }
if (!$HeaderPaths) { $HeaderPaths = @(Join-Path $repo 'src/Magpie/ScalingModesPage.h') }
$xamlOutput = Join-Path $output 'Xaml'
New-Item -ItemType Directory -Path $xamlOutput -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cached = Join-Path $CachedIntermediateRoot 'x64/Release/Magpie'
if (!$MetadataPath) {
if (!(Test-Path (Join-Path $output 'Unmerged/App.winmd'))) {
    throw 'Fresh source metadata is required; generate obj/tests/source/Unmerged metadata first or specify MetadataPath.'
}
$rsp = Get-Content -LiteralPath (Join-Path $cached 'Magpie.vcxproj.mdmerge.rsp') -Raw
# Reuse reference directories only; all metadata inputs/outputs are isolated.
$rsp = [regex]::Replace($rsp, '-i\s+"[^"]+App.winmd"', ('-i "' + (Join-Path $output 'Unmerged/App.winmd') + '"'))
$rsp = [regex]::Replace($rsp, '-i\s+"[^"]+XamlMetaDataProvider.winmd"', ('-i "' + (Join-Path $output 'Unmerged/XamlMetaDataProvider.winmd') + '"'))
$rsp = [regex]::Replace($rsp, '-o\s+"[^"]+"', ('-o "' + (Join-Path $output 'Merged') + '"'))
$response = Join-Path $output 'merge.rsp'
[IO.File]::WriteAllText($response, $rsp)
& mdmerge.exe "@$response" *> (Join-Path $output 'merge.log')
if ($LASTEXITCODE) { throw 'Isolated metadata merge failed' }
$MetadataPath = Join-Path $output 'Merged/Magpie.winmd'
}
$project = @'
<Project xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <UsingTask TaskName="Microsoft.Windows.UI.Xaml.Build.Tasks.CompileXaml" AssemblyFile="@TASK@" />
  <ItemGroup>
    <Pages Include="@PAGE@"><Link>ScalingModesPage.xaml</Link></Pages>
    <References Include="@SDKREFS@\**\*.winmd" />
    <References Include="@WINUI@" />
    <References Include="@WEBVIEW@" />
    <Local Include="@LOCAL@" />
    @HEADERS@
  </ItemGroup>
  <Target Name="Verify">
    <CompileXaml LanguageSourceExtension=".cpp" Language="CppWinRT" RootNamespace="Magpie"
      XamlPages="@(Pages)" ProjectName="Magpie" ProjectPath="@PROJECT@"
      IsPass1="True" CompileMode="RealBuildPass1" OutputType="winexe"
      OutputPath="@OUTPUT@" ReferenceAssemblies="@(References)" ReferenceAssemblyPaths="@REFPATH@" CppWinRTLocalAssembly="@(Local)"
      WindowsSdkPath="@SDK@" TargetPlatformMinVersion="10.0.19041.0" SavedStateFile="@STATE@"
      EnableTypeInfoReflection="false" ForceSharedStateShutdown="false" />
    <CompileXaml LanguageSourceExtension=".cpp" Language="CppWinRT" RootNamespace="Magpie"
      XamlPages="@(Pages)" ProjectName="Magpie" ProjectPath="@PROJECT@"
      IsPass1="False" CompileMode="RealBuildPass2" OutputType="winexe" LocalAssembly="@LOCAL@"
      ClIncludeFiles="@(Headers)" CIncludeDirectories="@INCLUDES@" PlatformXmlDir="@PLATFORM@" XamlComponentResourceLocation="@LOCATION@"
      OutputPath="@OUTPUT@" ReferenceAssemblies="@(References)" ReferenceAssemblyPaths="@REFPATH@" CppWinRTLocalAssembly="@(Local)"
      WindowsSdkPath="@SDK@" TargetPlatformMinVersion="10.0.19041.0" SavedStateFile="@STATE@"
      EnableTypeInfoReflection="false" ForceSharedStateShutdown="false" />
  </Target>
</Project>
'@
$sdkVersion = $env:WindowsSDKVersion.TrimEnd('\')
$paths = @{
    TASK = Join-Path $env:WindowsSdkDir "bin/$sdkVersion/XamlCompiler/Microsoft.Windows.UI.Xaml.Build.Tasks.dll"
    PAGE = [IO.Path]::GetFullPath($PagePath)
    PROJECT = Join-Path $repo 'src/Magpie/Magpie.vcxproj'
    SDKREFS = Join-Path $env:WindowsSdkDir "References/$sdkVersion"
    REFPATH = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319'
    WINUI = Join-Path $CachedRepoDirectory 'packages/Microsoft.UI.Xaml.2.8.7/lib/uap10.0/Microsoft.UI.Xaml.winmd'
    WEBVIEW = Join-Path $CachedRepoDirectory 'packages/Microsoft.Web.WebView2.1.0.4078.44/lib/Microsoft.Web.WebView2.Core.winmd'
    LOCAL = [IO.Path]::GetFullPath($MetadataPath)
    INCLUDES = (Split-Path $PagePath -Parent) + ';' + (Join-Path $repo 'src/Magpie')
    PLATFORM = Join-Path $env:WindowsSdkDir "Platforms/UAP/$sdkVersion/"
    LOCATION = $ComponentResourceLocation
    OUTPUT = $xamlOutput + '\'
    SDK = $env:WindowsSdkDir
    STATE = Join-Path $xamlOutput 'state.xml'
}
$headersXml = ($HeaderPaths | ForEach-Object { '<Headers Include="' + [Security.SecurityElement]::Escape([IO.Path]::GetFullPath($_)) + '" />' }) -join "`n"
$project = $project.Replace('@HEADERS@', $headersXml)
foreach ($entry in $paths.GetEnumerator()) { $project = $project.Replace('@' + $entry.Key + '@', [Security.SecurityElement]::Escape($entry.Value)) }
$projectPath = Join-Path $output 'xaml.proj'
[IO.File]::WriteAllText($projectPath, $project)
& (Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe') $projectPath /nologo /v:minimal /t:Verify
if ($LASTEXITCODE) { throw 'OKLab XAML binding compilation failed' }
foreach ($name in @('ScalingModesPage.xaml.g.hpp', 'ScalingModesPage.xbf')) {
    $artifact = Join-Path $xamlOutput $name
    if (!(Test-Path -LiteralPath $artifact) -or (Get-Item -LiteralPath $artifact).Length -eq 0) {
        throw "XAML pass 2 did not produce $name"
    }
}
'PASS isolated XAML pass 1/2 with generated binding implementation and XBF.'
