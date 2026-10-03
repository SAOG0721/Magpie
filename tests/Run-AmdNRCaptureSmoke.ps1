#Requires -Version 7.0
param([string]$RuntimeDirectory, [ValidateSet('NR','FSR3','FSR4','FG2')][string]$Stage='NR',
 [ValidateRange(12,900)][int]$Seconds=16, [switch]$Fullscreen, [switch]$NoResize,
 [string]$PresentMonPath)
$ErrorActionPreference='Stop'
if($Stage -eq 'FG2' -and !$PresentMonPath) { throw 'FG2 acceptance requires -PresentMonPath for actually displayed frame evidence.' }
$repo=Split-Path $PSScriptRoot -Parent
if(!$RuntimeDirectory) { $RuntimeDirectory=Join-Path $repo 'bin/x64/Release' }
if(Get-Process Magpie -ErrorAction SilentlyContinue) { throw 'Close existing Magpie before isolated validation.' }
$output=Join-Path $repo ('validation/amd-nr/'+$Stage+'-'+(Get-Date -Format yyyyMMdd-HHmmss))
$runtime=Join-Path $output runtime
New-Item -ItemType Directory $runtime -Force | Out-Null
Get-ChildItem -LiteralPath $RuntimeDirectory -File | Where-Object Extension -NotIn '.lib','.pdb','.map','.exp' | Copy-Item -Destination $runtime
foreach($dir in @('effects','AMDNR')) { Copy-Item -LiteralPath (Join-Path $RuntimeDirectory $dir) -Destination $runtime -Recurse }
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$fixture=Join-Path $output AmdNRCaptureFixture.exe
& cl.exe /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /MT /O2 "$repo/tests/AmdNRCaptureFixture.cpp" "/Fe:$fixture" "/Fo:$output/fixture.obj"
if($LASTEXITCODE) { throw 'Capture fixture compilation failed' }
$effects=@(@{name='AMDNR\AMDNR_AI_Filter';scalingType=0;scale=@{x=1;y=1}})
if($Stage -ne 'NR') {
 $sr=if($Stage -eq 'FSR4') {'FSR4\FSR4_SR'} else {'FSR3\FSR3_SR'}
 $effects+=@{name=$sr;scalingType=0;scale=@{x=2;y=2};parameters=@{opticalFlowMethod=0}}
}
if($Stage -eq 'FG2') { $effects+=@{name='XeSSFG\XeSS_FrameGeneration';scalingType=0;scale=@{x=1;y=1};parameters=@{multiplier=2;opticalFlowMethod=1;amdOpticalFlowMode=1}} }
$profiles=@(@{scalingMode=-1})
foreach($mode in @(2,3)) {
 $profiles+=@{name="AMDNR $mode";packaged=$false;pathRule=$fixture;classNameRule="MagpieAMDNRCapture$mode";
 autoScale=$(if($Fullscreen){1}else{2});scalingMode=0;captureMethod=0;initialWindowedScaleFactor=1;
 parameterFocusSwitching=$false;enableHdrCompatibility=$false}
}
$config=@{alwaysRunAsAdmin=$false;autoCheckForUpdates=$false;showNotifyIcon=$true;
 stopEffectsOnTaskSwitch=$false;frontEdgeSync=$false;frameSyncMode=1;vrr=$false;
 smoothMotionCompatibilityMode=$false;experimentalXeSSFGSettingsVersion=1;experimentalDlssnrSettingsVersion=2;
 experimentalDlssSrSettingsVersion=1;experimentalDepthRemovalVersion=1;experimentalOpticalFlowDefaultsVersion=1;
 profiles=$profiles;scalingModes=@(@{name="AMDNR $Stage validation";effects=$effects})}
$configDir=Join-Path $runtime config/v4e
New-Item -ItemType Directory $configDir -Force | Out-Null
$config | ConvertTo-Json -Depth 12 | Set-Content (Join-Path $configDir config.json) -Encoding utf8
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class AmdNRQuit {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern uint RegisterWindowMessage(string n);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr w,uint m,IntPtr p,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint p);
}
'@
$oldValidation=$env:MAGPIE_VALIDATE_NATIVE_OUTPUT; $env:MAGPIE_VALIDATE_NATIVE_OUTPUT='1'
try { $app=Start-Process (Join-Path $runtime Magpie.exe) -ArgumentList '-t' -WorkingDirectory $runtime -WindowStyle Hidden -PassThru }
finally { $env:MAGPIE_VALIDATE_NATIVE_OUTPUT=$oldValidation }
try {
 if($PresentMonPath) {
  $pm=Start-Process -FilePath ([IO.Path]::GetFullPath($PresentMonPath)) -ArgumentList @('--process_id',"$($app.Id)",'--output_file',"`"$output/presentmon.csv`"",'--session_name',"MagpieAMDNR_$($app.Id)",'--timed',"$($Seconds*2+30)",'--terminate_after_timed','--terminate_on_proc_exit','--no_console_stats','--v2_metrics','--track_frame_type') -WindowStyle Hidden -Verb RunAs -PassThru
 }
 Start-Sleep -Seconds 3
 $oldSeconds=$env:MAGPIE_CAPTURE_SECONDS; $env:MAGPIE_CAPTURE_SECONDS="$Seconds"
 $oldResize=$env:MAGPIE_CAPTURE_NO_RESIZE; $env:MAGPIE_CAPTURE_NO_RESIZE=$(if($NoResize){'1'}else{$null})
 try { & $fixture 2 3 | Tee-Object (Join-Path $output fixture.log); $fixtureExit=$LASTEXITCODE }
 finally { $env:MAGPIE_CAPTURE_SECONDS=$oldSeconds; $env:MAGPIE_CAPTURE_NO_RESIZE=$oldResize }
} finally {
 $window=[AmdNRQuit]::FindWindow('Magpie_NotifyIcon',$null); [uint32]$owner=0
 [void][AmdNRQuit]::GetWindowThreadProcessId($window,[ref]$owner)
 if($owner -eq $app.Id) { [void][AmdNRQuit]::PostMessage($window,[AmdNRQuit]::RegisterWindowMessage('WM_MAGPIE_QUIT'),[IntPtr]::Zero,[IntPtr]::Zero) }
 if(!$app.WaitForExit(15000)) { throw "Validation app did not exit: $($app.Id)" }
 if($pm -and !$pm.WaitForExit(60000)) { throw "PresentMon did not finish: $($pm.Id)" }
}
$logs=(Get-ChildItem (Join-Path $runtime logs) -Filter 'magpie*.log' | ForEach-Object { Get-Content $_.FullName -Raw }) -join "`n"
$logs | Set-Content (Join-Path $output magpie-combined.log)
if($fixtureExit -or $app.ExitCode) { throw "Capture test failed: fixture=$fixtureExit app=$($app.ExitCode); logs: $output" }
if($logs -notmatch 'AMD lmxxf NR frame=' -or $logs -match 'AMD lmxxf NR.*failed|AMD lmxxf NR initialize:') { throw "NR did not complete: $output" }
if($Stage -ne 'NR' -and $logs -notmatch '1280x720') { throw 'Expected 1280x720 SR not observed' }
if($logs -notmatch 'Native output validation: backend=AMDNR' -or $logs -match 'backend=AMDNR[^\r\n]*meanAbsoluteDifference=0\.0000') { throw "NR shared-output pixel validation missing or identical: $output" }
if($Stage -ne 'NR' -and $logs -notmatch 'FSR dispatch completed: [^\r\n]*input=640x360 output=1280x720 rc=0') { throw 'Expected successful SDK SR dispatch missing' }
if($Stage -eq 'FSR4' -and $logs -notmatch 'FSR dispatch completed: [^\r\n]*name=4\.1\.1') { throw 'FSR4 actual provider 4.1.1 was not observed' }
$analysisArgs=@("$repo/scripts/Analyze-AmdNRValidation.py",$output)
if($Stage -eq 'FG2') { $analysisArgs+='--require-fg2' }
& python @analysisArgs
if($LASTEXITCODE) { throw "Evidence analysis failed: $output" }
Write-Output "WGC + $Stage artificial-source smoke completed; logs: $output. This is not a real-game quality or display-FPS acceptance."
