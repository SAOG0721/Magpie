#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$DependencyDirectory
)
$ErrorActionPreference = 'Stop'
$reflexRepo = Split-Path $PSScriptRoot -Parent
$reflexOutput = [IO.Path]::GetFullPath($OutputDirectory)
$reflexDependencies = [IO.Path]::GetFullPath($DependencyDirectory)
New-Item -ItemType Directory -Path $reflexOutput -Force | Out-Null
$reflexVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$reflexVs = & $reflexVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (!$reflexVs) { throw 'Visual Studio C++ tools not found.' }
Import-Module (Join-Path $reflexVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $reflexVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$reflexNvApi = Join-Path $reflexDependencies 'nvapi-sdk'
$reflexNgx = Join-Path $reflexDependencies 'DLSS-main'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE `
    "/I$reflexNvApi" "/I$reflexNgx/include" "/I$reflexRepo/src/Shared" `
    "$PSScriptRoot/ReflexNativeProbe.cpp" "/Fe:$reflexOutput/ReflexNativeProbe.exe" "/Fo:$reflexOutput/ReflexNativeProbe.obj" `
    /link d3d11.lib d3d12.lib dxgi.lib user32.lib bcrypt.lib advapi32.lib `
    "$reflexNgx/lib/Windows_x86_64/x64/nvsdk_ngx_s.lib"
if ($LASTEXITCODE) { throw 'Reflex native probe compilation failed.' }
Get-FileHash -LiteralPath "$reflexOutput/ReflexNativeProbe.exe" -Algorithm SHA256
Write-Host 'Compiled only. Native GPU/Present execution is separately scheduled.'
