#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$IntermediateDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$IntermediateDirectory = (Resolve-Path -LiteralPath $IntermediateDirectory).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$commands = Get-Content -LiteralPath (Join-Path $IntermediateDirectory 'Magpie.Core.tlog/CL.command.1.tlog') -Raw
$includes = @("/I$repo/src/Magpie.Core", "/I$repo/src/Magpie.Core/include")
foreach ($match in [regex]::Matches($commands, '(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
    $include = $match.Groups['quoted'].Value
    if (!$include) { $include = $match.Groups['bare'].Value }
    if (![IO.Path]::IsPathRooted($include)) { $include = Join-Path "$repo/src/Magpie.Core" $include }
    $includes += "/I$include"
}
$includes = @($includes | Select-Object -Unique)
$exe = Join-Path $OutputDirectory 'StepTimerCaptureTests.exe'
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE @includes `
    (Join-Path $PSScriptRoot 'StepTimerCaptureTests.cpp') "/Fe:$exe" "/Fo:$OutputDirectory/StepTimerCaptureTests.obj" `
    /link user32.lib WindowsApp.lib
if ($LASTEXITCODE) { throw 'Production StepTimer test compilation failed' }
& $exe
if ($LASTEXITCODE) { throw 'Production StepTimer capture accounting failed' }
