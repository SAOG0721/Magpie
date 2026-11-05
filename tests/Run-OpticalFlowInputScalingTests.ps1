#Requires -Version 7.0
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$output = Join-Path $repo 'obj/tests/optical-flow-input-scaling'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$source = Get-Content -LiteralPath (Join-Path $repo 'src/Magpie.Core/FrameGuidanceService.cpp') -Raw
$shader = [regex]::Match($source, '(?s)constexpr char ADAPT_GUIDANCE_HLSL\[\] = R"\((.*?)\)";')
if (!$shader.Success) { throw 'Production guidance adapter shader not found' }
Set-Content -LiteralPath (Join-Path $output 'OpticalFlowAdaptShaderUnderTest.h') -Value ('inline constexpr char OPTICAL_FLOW_ADAPT_UNDER_TEST[] = R"(' + $shader.Groups[1].Value + ')";') -Encoding utf8
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /DNOMINMAX "/I$output" "/I$repo/src/Magpie.Core" "$PSScriptRoot/OpticalFlowInputScalingTests.cpp" "/Fe:$output/OpticalFlowInputScalingTests.exe" "/Fo:$output/OpticalFlowInputScalingTests.obj" /link d3d11.lib d3dcompiler.lib
if ($LASTEXITCODE) { throw 'Optical flow consumer-adapter test compilation failed' }
& "$output/OpticalFlowInputScalingTests.exe"
if ($LASTEXITCODE) { throw 'Optical flow consumer-adapter shader regression failed' }
