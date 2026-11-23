#Requires -Version 7.0
param(
    [string]$OutputDirectory,
    [switch]$CompileOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $env:TEMP ('Magpie-publication-interop-' + [guid]::NewGuid().ToString('N'))
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (!$vs) { throw 'Visual C++ x64 tools were not found.' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$source = Join-Path $PSScriptRoot 'PublicationInteropTests.cpp'
$executable = Join-Path $outputPath 'PublicationInteropTests.exe'
Push-Location $outputPath
try {
    # Standalone SDK fixture: the same D3D11.4/D3D12 interfaces as production,
    # without Magpie, NGX, application configuration or extra dependencies.
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 `
        $source "/Fe:$executable" "/Fo:$outputPath/PublicationInteropTests.obj" d3d11.lib d3d12.lib dxgi.lib dxguid.lib `
        2>&1 | Tee-Object -FilePath (Join-Path $outputPath 'build.log')
    if ($LASTEXITCODE) { throw "Publication interop fixture compilation failed ($LASTEXITCODE)." }
    if (!$CompileOnly) {
        & $executable 2>&1 | Tee-Object -FilePath (Join-Path $outputPath 'test.log')
        if ($LASTEXITCODE) { throw "Publication interop behavior test failed ($LASTEXITCODE)." }
    }
    Write-Output "Publication interop artifacts: $outputPath"
} finally {
    Pop-Location
}
