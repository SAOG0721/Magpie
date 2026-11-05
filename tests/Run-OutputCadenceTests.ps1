#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $env:TEMP ('Magpie-OutputCadence-' + [guid]::NewGuid())))
$ErrorActionPreference = 'Stop'
$cadenceRepo = Split-Path $PSScriptRoot -Parent
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$cadenceVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$cadenceVs = & $cadenceVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $cadenceVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $cadenceVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/prepare_frame_sync_timer_test.py" $OutputDirectory
if ($LASTEXITCODE) { throw 'Production StepTimer extraction failed' }
& python "$PSScriptRoot/prepare_output_refresh_selection_test.py" $OutputDirectory
if ($LASTEXITCODE) { throw 'Production refresh-selection extraction failed' }
foreach ($cadenceTest in @(
    @{ Source = "$PSScriptRoot/OutputCadenceTests.cpp"; Name = 'output-cadence' },
    @{ Source = "$OutputDirectory/frame_sync_timer.cpp"; Name = 'step-timer' },
    @{ Source = "$OutputDirectory/output_refresh_selection.cpp"; Name = 'refresh-selection' },
    @{ Source = "$OutputDirectory/output_refresh_reflex.cpp"; Name = 'refresh-reflex' }
)) {
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX "/I$cadenceRepo/src/Magpie.Core" `
        $cadenceTest.Source "/Fe:$OutputDirectory/$($cadenceTest.Name).exe" "/Fo:$OutputDirectory/$($cadenceTest.Name).obj"
    if ($LASTEXITCODE) { throw "Cadence test compilation failed: $($cadenceTest.Name)" }
    & "$OutputDirectory/$($cadenceTest.Name).exe"
    if ($LASTEXITCODE) { throw "Cadence test failed: $($cadenceTest.Name)" }
}
