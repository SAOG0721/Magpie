#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$SourceRoot,
    [Parameter(Mandatory)][string]$RuntimeDirectory,
    [Parameter(Mandatory)][string]$IntermediateDirectory,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$CoreLibrary,
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
$SourceRoot = (Resolve-Path -LiteralPath $SourceRoot).Path
$RuntimeDirectory = (Resolve-Path -LiteralPath $RuntimeDirectory).Path
$IntermediateDirectory = (Resolve-Path -LiteralPath $IntermediateDirectory).Path
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Use a new empty probe directory; no existing runtime is overwritten.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$generated = Join-Path $OutputDirectory 'generated'
& python (Join-Path $PSScriptRoot 'prepare_parameter_flyout_app_probe.py') $SourceRoot $generated
if ($LASTEXITCODE) { throw 'Probe generation failed' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$commands = Get-Content -LiteralPath (Join-Path $IntermediateDirectory 'Magpie.tlog/CL.command.1.tlog') -Raw
$includes = @("/I$SourceRoot/src/Magpie", "/I$PSScriptRoot", "/I$IntermediateDirectory/Generated Files")
foreach ($match in [regex]::Matches($commands, '(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
    $include = $match.Groups['quoted'].Value
    if (!$include) { $include = $match.Groups['bare'].Value }
    if (![IO.Path]::IsPathRooted($include)) { $include = Join-Path (Join-Path $SourceRoot 'src/Magpie') $include }
    $includes += "/I$include"
}
$defines = @()
foreach ($match in [regex]::Matches($commands, '/D\s+(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')) {
    $define = $match.Groups['quoted'].Value
    if (!$define) { $define = $match.Groups['bare'].Value }
    $defines += "/D$define"
}
$includes = @($includes | Select-Object -Unique)
$defines = @($defines | Select-Object -Unique)
& cl.exe /nologo /c /std:c++20 /EHsc /utf-8 /MT /O2 /bigobj /W4 /WX @defines @includes (Join-Path $generated 'App.cpp') "/Fo:$generated/App.obj"
if ($LASTEXITCODE) { throw 'Isolated App.cpp compile failed' }
$linkText = ((Get-Content -LiteralPath (Join-Path $IntermediateDirectory 'Magpie.tlog/link.command.1.tlog') | Where-Object { $_ -and !$_.StartsWith('^') }) -join "`n")
$appObjects = [regex]::Matches($linkText, '(?i)"[^"\r\n]*[/\\]APP\.OBJ"')
if ($appObjects.Count -ne 1) { throw 'Expected one App.obj input in candidate linker log' }
$linkText = $linkText.Replace($appObjects[0].Value, ('"' + (Join-Path $generated 'App.obj') + '"'))
if ($CoreLibrary) {
    $CoreLibrary = (Resolve-Path -LiteralPath $CoreLibrary).Path
    $coreInputs = [regex]::Matches($linkText, '(?i)"[^"\r\n]*[/\\]MAGPIE\.CORE\.LIB"')
    if ($coreInputs.Count -ne 1) { throw 'Expected one Magpie.Core.lib input in candidate linker log' }
    $linkText = $linkText.Replace($coreInputs[0].Value, ('"' + $CoreLibrary + '"'))
}
foreach ($entry in @(@('OUT','Magpie.exe'), @('PDB','Magpie.pdb'), @('MAP','Magpie.map'), @('IMPLIB','Magpie.lib'), @('WINMDFILE','Magpie.winmd'), @('LTCGOUT','Magpie.iobj'), @('MANIFESTFILE','Magpie.manifest'), @('PGD','Magpie.pgd'), @('ILK','Magpie.ilk'))) {
    $flag = $entry[0]; $path = Join-Path $OutputDirectory $entry[1]
    $pattern = '(?i)/' + $flag + ':(?:"[^"]*"|[^\s]+)'
    $replacement = '/' + $flag + ':"' + $path + '"'
    $linkText = [regex]::Replace($linkText, $pattern, [System.Text.RegularExpressions.MatchEvaluator]{ param($match) $replacement })
}
$rsp = Join-Path $generated 'link.rsp'
[IO.File]::WriteAllText($rsp, $linkText)
Push-Location (Join-Path $SourceRoot 'src/Magpie')
try {
    & link.exe ('@' + $rsp)
    if ($LASTEXITCODE) { throw 'Isolated probe link failed' }
} finally { Pop-Location }
# UI-only probe: copies are local, with no links to user configuration or runtime files.
Get-ChildItem -LiteralPath $RuntimeDirectory -File -Filter '*.dll' | Copy-Item -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'resources.pri') -Destination $OutputDirectory
Copy-Item -LiteralPath (Join-Path $RuntimeDirectory 'effects') -Destination $OutputDirectory -Recurse
$configDirectory = Join-Path $OutputDirectory 'config/v4e'
New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
$config = [ordered]@{
    alwaysRunAsAdmin = $false; showNotifyIcon = $false; autoCheckForUpdates = $false
    scalingModes = @(@{ name='VSR isolated probe'; effects=@(@{ name='RTXVideo\RTXVideo_VSR'; parameters=@{category=0; strength=1}; scalingType=1; scale=@{x=1;y=1} }) })
    profiles = @()
}
$config | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $configDirectory 'config.json') -Encoding utf8NoBOM
Set-Content -LiteralPath (Join-Path $OutputDirectory 'parameter-flyout-probe.marker') -Value 'Test executable. No global services. Do not distribute.'
Write-Output "Built isolated actual-template probe: $OutputDirectory"
if ($Run) {
    # Default mode manipulates only its own XAML objects. The optional mouse
    # mode checks target PID twice and never sends keyboard input.
    $process = Start-Process -FilePath (Join-Path $OutputDirectory 'Magpie.exe') -WorkingDirectory $OutputDirectory -WindowStyle Hidden -PassThru
    if (!$process.WaitForExit(60000)) {
        Stop-Process -Id $process.Id
        throw 'Probe exceeded 60 seconds; stopped only the child process started here.'
    }
    if ($process.ExitCode) { throw "Probe exit code $($process.ExitCode); inspect its own log." }
    $log = Get-Content -LiteralPath (Join-Path $OutputDirectory 'logs/Magpie.log') -Raw
    if ($log -notmatch 'ParameterFlyoutAppProbe PASS:') { throw 'Probe did not record a successful run.' }
    Write-Output 'PASS isolated actual-template probe; review local logs and saved config.'
}
