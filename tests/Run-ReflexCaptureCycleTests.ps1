#Requires -Version 7.0
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$cycleRepo = Split-Path $PSScriptRoot -Parent
$cycleOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $cycleOutput -Force | Out-Null
$cycleVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$cycleVs = & $cycleVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
Import-Module (Join-Path $cycleVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $cycleVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& python "$PSScriptRoot/prepare_reflex_capture_cycle_test.py" $cycleOutput
if ($LASTEXITCODE) { throw 'Production Reflex capture extraction failed.' }
foreach ($cycleCase in @('reflex-capture-cycle', 'legacy-capture-hook', 'broaden-nr-scope', 'broaden-fg-scope', 'future-candidate-identity', 'prior-output-identity')) {
    $cycleTraceOptions = if ($cycleCase -eq 'reflex-capture-cycle') { @($true, $false) } else { @($true) }
    foreach ($cycleTrace in $cycleTraceOptions) {
        $cycleLabel = $cycleCase + $(if ($cycleTrace) { '-trace' } else { '-untraced' })
        [string[]]$cycleDefines = @(if ($cycleTrace) { '/DMP_ENABLE_FRAME_TRACE' })
        & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX @cycleDefines `
            "/I$cycleRepo/src/Magpie.Core" "/I$cycleRepo/src/Magpie.Core/include" `
            "$cycleOutput/$cycleCase.cpp" "/Fe:$cycleOutput/$cycleLabel.exe" "/Fo:$cycleOutput/$cycleLabel.obj"
        if ($LASTEXITCODE) { throw "Reflex capture test compilation failed: $cycleLabel" }
        & "$cycleOutput/$cycleLabel.exe"
        $cycleExpected = if ($cycleCase -eq 'reflex-capture-cycle') { 0 } else { 42 }
        if ($LASTEXITCODE -ne $cycleExpected) { throw "Reflex capture test failed: $cycleLabel (expected $cycleExpected)" }
        if ($cycleExpected) { Write-Host "PASS: rejected production mutation $cycleCase" }
    }
}
