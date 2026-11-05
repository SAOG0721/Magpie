#Requires -Version 7.0
param([string]$Output)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$Output) {
    $workspace = Split-Path (Split-Path $repo -Parent) -Parent
    $Output = Join-Path $workspace 'diagnostics/20261101-dlssnr-three-routes/cpu-guidance-temporal'
}
New-Item -ItemType Directory -Path $Output -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
foreach ($name in @('DLSSNRSparseTemporalStateTests','FrameGuidancePairContractTests','DLSSNRGuidanceShaderCompileTests')) {
    $arguments = @('/nologo','/std:c++20','/EHsc','/utf-8','/MT','/O2','/W4','/WX',
        "/I$repo/src/Magpie.Core", "/I$repo/src/Magpie.Core/include", "$repo/tests/$name.cpp",
        "/Fe:$Output/$name.exe", "/Fo:$Output/$name.obj")
    if ($name -eq 'DLSSNRGuidanceShaderCompileTests') { $arguments += @('/link','d3dcompiler.lib','dxguid.lib') }
    & cl.exe @arguments
    if ($LASTEXITCODE) { throw "CPU test compilation failed: $name" }
    if ($name -eq 'DLSSNRGuidanceShaderCompileTests') {
        & "$Output/$name.exe" "$repo/src/Magpie.Core/NvidiaOpticalFlowProvider.cpp" "$repo/src/Magpie.Core/FrameGuidanceService.cpp"
    } else { & "$Output/$name.exe" }
    if ($LASTEXITCODE) { throw "CPU test failed: $name" }
}
