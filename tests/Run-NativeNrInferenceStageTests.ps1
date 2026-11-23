#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $env:TEMP ('Magpie-native-nr-stage-' + [guid]::NewGuid()) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
& python -B (Join-Path $PSScriptRoot 'prepare_native_nr_inference_stage_test.py') $OutputDirectory
if ($LASTEXITCODE) { throw 'Could not extract the production inference wrappers.' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
if (!$vs) { throw 'Visual C++ tools unavailable.' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Push-Location $OutputDirectory
try {
    foreach ($enabled in @($true, $false)) {
        $name = if ($enabled) { 'trace-enabled' } else { 'trace-compiled-out' }
        $arguments = @('/nologo', '/std:c++20', '/EHsc', '/utf-8', '/MT', '/O2', '/W4', '/WX', '/DNOMINMAX',
            '/DUNICODE', '/D_UNICODE', "/I$repo/src/Magpie.Core", "/I$OutputDirectory",
            "$PSScriptRoot/NativeNrInferenceStageTests.cpp", "/Fe:$OutputDirectory/$name.exe", "/Fo:$OutputDirectory/$name.obj")
        if ($enabled) { $arguments += '/DMP_ENABLE_FRAME_TRACE' }
        & cl.exe @arguments 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory "$name-build.log")
        if ($LASTEXITCODE) { throw "Native NR $name compilation failed." }
        & (Join-Path $OutputDirectory "$name.exe") 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory "$name-test.log")
        if ($LASTEXITCODE) { throw "Native NR $name behavior check failed." }
    }
    $sdkInclude = @('../dependencies/DLSS-main/include', '../../dependencies/DLSS-main/include') |
        ForEach-Object { [IO.Path]::GetFullPath((Join-Path $repo $_)) } |
        Where-Object { Test-Path -LiteralPath (Join-Path $_ 'nvsdk_ngx_defs.h') } | Select-Object -First 1
    if (!$sdkInclude) { throw 'The project NVIDIA NGX SDK headers were not found.' }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE /DMP_ENABLE_FRAME_TRACE `
        "/I$repo/src/Magpie.Core" "/I$OutputDirectory" "/I$sdkInclude" "$PSScriptRoot/NativeNrResourceReleaseProbeTests.cpp" `
        "/Fe:$OutputDirectory/resource-release-probe.exe" "/Fo:$OutputDirectory/resource-release-probe.obj" `
        2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'resource-release-probe-build.log')
    if ($LASTEXITCODE) { throw 'NR release probe contract compilation failed.' }
    & (Join-Path $OutputDirectory 'resource-release-probe.exe') 2>&1 | Tee-Object -FilePath (Join-Path $OutputDirectory 'resource-release-probe-test.log')
    if ($LASTEXITCODE) { throw 'NR release probe contract behavior failed.' }
} finally { Pop-Location }
Write-Output "CPU-only native NR stage artifacts: $OutputDirectory"
