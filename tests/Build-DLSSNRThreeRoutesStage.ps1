#Requires -Version 7.0
param([string]$OutputRoot,
    [switch]$DependenciesOnly,
    [switch]$CoreOnly,
    [switch]$DisableChangedSdkPaths,
    [switch]$DisableNativeTiming,
    [int]$CompilerProcesses=4)
$ErrorActionPreference='Stop'
$repo=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$workspace=[IO.Path]::GetFullPath((Join-Path $repo '..\..'))
if(!$OutputRoot) { $OutputRoot=Join-Path $workspace 'diagnostics\20261101-dlssnr-three-routes\stage' }
if($DisableChangedSdkPaths) { $OutputRoot += '-disabled-sdk' }
$OutputRoot=[IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
$packageDirectory=Join-Path $repo 'packages'
if(!(Test-Path -LiteralPath $packageDirectory)) {
    $existingPackages=Join-Path $workspace 'source\packages'
    if(!(Test-Path -LiteralPath $existingPackages)) { throw 'Existing restored NuGet packages are unavailable.' }
    New-Item -ItemType Junction -Path $packageDirectory -Target $existingPackages | Out-Null
}
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmakeDirectory=Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
if(Test-Path -LiteralPath (Join-Path $cmakeDirectory 'cmake.exe')) { $env:Path=$cmakeDirectory+';'+$env:Path }
$msbuild=Join-Path $vs 'MSBuild\Current\Bin\amd64\MSBuild.exe'
$project=if($DependenciesOnly) {'src\_ConanDeps\_ConanDeps.vcxproj'} elseif($CoreOnly) {'src\Magpie.Core\Magpie.Core.vcxproj'} else {'Magpie.slnx'}
$label=if($DependenciesOnly){'dependencies'}elseif($CoreOnly){'core'}else{'solution'}
$buildArguments=@($project,'/t:Build','/m:2','/nr:false','/v:minimal',
    '/p:Configuration=Release','/p:Platform=x64','/p:PreferredToolArchitecture=x64',
    '/p:MajorVersion=0','/p:MinorVersion=6','/p:PatchVersion=12','/p:VersionString=0.6.12-beta3',
    '/p:GenerateReleaseSymbols=true','/p:DisablePDB=false','/p:EnableFrameTrace=true',
    ('/p:EnableNativeBackendTiming='+$(if($DisableNativeTiming){'false'}else{'true'})),
    '/p:UseMultiToolTask=true',"/p:CL_MPCount=$CompilerProcesses","/p:MultiProcMaxCount=$CompilerProcesses",'/p:EnforceProcessCountAcrossBuilds=true',
    ('/p:SolutionDir='+$repo.Replace('\','/')+'/'),
    ('/p:OutDir='+(Join-Path $OutputRoot 'runtime').Replace('\','/')+'/'),
    ('/p:MagpieIntermediateRoot='+(Join-Path $OutputRoot 'obj').Replace('\','/')+'/'),
    ('/bl:'+(Join-Path $OutputRoot ($label+'.binlog'))))
if($DisableChangedSdkPaths) {
    $buildArguments += @('/p:EnableDLSSNR=false','/p:EnableDLSSFrameGeneration=false','/p:EnableNvidiaOpticalFlow=false')
}
Push-Location $repo
try {
    & $msbuild @buildArguments *> (Join-Path $OutputRoot ($label+'.log'))
    $buildExit=$LASTEXITCODE
} finally { Pop-Location }
[ordered]@{project=$project;exitCode=$buildExit;source=$repo;output=$OutputRoot;arguments=$buildArguments} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutputRoot ($label+'-result.json')) -Encoding utf8
Get-Content -LiteralPath (Join-Path $OutputRoot ($label+'.log')) -Tail 35
if($buildExit) { throw "$label build failed: $buildExit" }
