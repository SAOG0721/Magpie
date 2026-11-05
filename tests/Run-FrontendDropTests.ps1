#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$dropRepo = Split-Path $PSScriptRoot -Parent
$dropOutput = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else {
    Join-Path $dropRepo 'obj/tests/frontend-drop'
}
python (Join-Path $PSScriptRoot 'prepare_frontend_drop_test.py') $dropOutput
if ($LASTEXITCODE) { throw 'Production extraction failed' }
$dropVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$dropVs = & $dropVswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $dropVs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $dropVs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT "/I$dropRepo/src/Magpie.Core/include" "$dropOutput/frontend_drop.cpp" "/Fe:$dropOutput/frontend_drop.exe" "/Fo:$dropOutput/frontend_drop.obj"
if ($LASTEXITCODE) { throw 'Frontend drop regression compilation failed' }
& "$dropOutput/frontend_drop.exe"
if ($LASTEXITCODE) { throw 'Frontend drop regression failed' }
