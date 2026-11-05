#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildRoot,[Parameter(Mandatory)][string]$CoreLibrary,
    [Parameter(Mandatory)][string]$Output,[switch]$CompileOnly,
    [Parameter(ValueFromRemainingArguments)][string[]]$BenchmarkArgs)
$ErrorActionPreference='Stop'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$arguments=@((Join-Path $PSScriptRoot 'run_dlssnr_flow_rejection_probe.py'),'--core-library',[IO.Path]::GetFullPath($CoreLibrary),'--output',[IO.Path]::GetFullPath($Output))
if($CompileOnly){$arguments+='--compile-only'}
$arguments+=(Resolve-Path -LiteralPath $BuildRoot).Path
if($BenchmarkArgs){$arguments+=@('--')+$BenchmarkArgs}
& python @arguments
if($LASTEXITCODE){throw "Flow rejection probe failed ($LASTEXITCODE)."}
