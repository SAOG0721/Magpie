#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildRoot, [switch]$Timing,
    [string]$CoreLibrary, [string]$Output, [switch]$PerformanceOnly, [switch]$CompileOnly)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$arguments = @((Join-Path $PSScriptRoot 'run_dlssnr_pipeline_native.py'), (Resolve-Path -LiteralPath $BuildRoot).Path)
if ($Timing) { $arguments += '--timing' }
if ($CoreLibrary) { $arguments += @('--core-library', (Resolve-Path -LiteralPath $CoreLibrary).Path) }
if ($Output) { $arguments += @('--output', [IO.Path]::GetFullPath($Output)) }
if ($PerformanceOnly) { $arguments += '--performance-only' }
if ($CompileOnly) { $arguments += '--compile-only' }
& python @arguments
if ($LASTEXITCODE) { throw 'Native DLSSNR pipeline test failed; inspect DLSSNRPipelineTests.log beside the test executable.' }
