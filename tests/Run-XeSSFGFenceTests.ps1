#Requires -Version 7.0
param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) { $OutputDirectory = Join-Path $repo 'obj/tests/xessfg-fence' }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$source = Get-Content -LiteralPath (Join-Path $repo 'src/Magpie.Core/XeSSFGPresenter.cpp') -Raw
$functions = [regex]::Matches($source, '(?ms)^static bool WaitForFence\(XeSSFGPresenter::Impl& impl, uint64_t value\) noexcept \{\r?\n.*?^\}')
if ($functions.Count -ne 1) { throw 'Expected one production XeSSFG fence function.' }
$functions[0].Value | Set-Content -LiteralPath (Join-Path $OutputDirectory 'XeSSFGFence.production.inl') -Encoding utf8NoBOM
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT /O2 /DNOMINMAX /DUNICODE /D_UNICODE "/I$OutputDirectory" "/I$repo/src/Magpie.Core" `
    (Join-Path $PSScriptRoot 'XeSSFGFenceTests.cpp') "/Fe:$OutputDirectory/XeSSFGFenceTests.exe" "/Fo:$OutputDirectory/XeSSFGFenceTests.obj"
if ($LASTEXITCODE) { throw 'XeSSFG fence fixture compilation failed.' }
& (Join-Path $OutputDirectory 'XeSSFGFenceTests.exe')
if ($LASTEXITCODE) { throw 'XeSSFG fence fixture failed.' }
