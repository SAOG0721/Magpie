#Requires -Version 7.0
param([Parameter(Mandatory)][string]$IntermediateDirectory,
      [string]$OutputDirectory=(Join-Path $env:TEMP ('Magpie-profiler-gpu-'+[guid]::NewGuid())),
      [switch]$EnableFrameTrace)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory=(Resolve-Path -LiteralPath $OutputDirectory).Path
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
# Only logging is replaced; the actual production query/lock/history code is compiled.
@'
#pragma once
namespace Magpie {
struct Logger {
 static Logger& Get() {static Logger value; return value;}
 template<class T> void Info(const T&) {}
 template<class T> void Warn(const T&) {}
 void ComError(const char*, HRESULT) {}
};
}
'@ | Set-Content -LiteralPath (Join-Path $OutputDirectory 'Logger.h') -Encoding utf8NoBOM
$commands=Get-Content -LiteralPath (Join-Path $IntermediateDirectory 'Magpie.Core.tlog/CL.command.1.tlog') -Raw
$includes=@("/I$OutputDirectory", "/I$repo/src/Magpie.Core")
foreach($match in [regex]::Matches($commands,'(?i)/(?:external:)?I(?:"(?<quoted>[^"]+)"|(?<bare>[^\s]+))')){
 $include=$match.Groups['quoted'].Value
 if(!$include){$include=$match.Groups['bare'].Value}
 if(![IO.Path]::IsPathRooted($include)){$include=Join-Path (Join-Path $repo 'src/Magpie.Core') $include}
 $includes+='/I'+$include
}
$includes=@($includes | Select-Object -Unique)
Push-Location $OutputDirectory
try {
 $traceArguments = if ($EnableFrameTrace) { @('/DMP_ENABLE_FRAME_TRACE', "$repo/src/Magpie.Core/FrameTrace.cpp") } else { @() }
 & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE /DFMT_HEADER_ONLY @includes @traceArguments "$repo/tests/EffectsProfilerGpuTests.cpp" "$repo/src/Magpie.Core/EffectsProfiler.cpp" "$repo/src/Shared/SmallVector.cpp" /Fe:EffectsProfilerGpuTests.exe d3d11.lib dxgi.lib runtimeobject.lib oleaut32.lib
 if($LASTEXITCODE){throw 'GPU profiler test compile failed'}
 & '.\EffectsProfilerGpuTests.exe'
 if($LASTEXITCODE){throw 'GPU profiler tests failed'}
 if ($EnableFrameTrace) {
  $trace = Get-ChildItem -LiteralPath (Join-Path $OutputDirectory 'logs/frame-traces') -Filter '*.csv' |
      Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if (!$trace) { throw 'GPU trace export missing' }
  $events = @(Get-Content -LiteralPath $trace.FullName | Where-Object { $_ -like 'recent,*' } |
      ConvertFrom-Csv -Header stream,thread,event,start_us,duration_us,frame_id,a,b)
  $samples = @($events | Where-Object event -eq 'GpuEffectsSample')
  $passes = @($events | Where-Object event -eq 'GpuPassSample')
  if ($samples.Count -ne 40 -or $passes.Count -ne 80) { throw 'Missing real GPU query trace samples' }
  foreach ($sample in @($samples) + @($passes)) {
   if ([uint64]$sample.frame_id -lt 100 -or [uint64]$sample.frame_id -gt 119 -or [double]$sample.duration_us -ne 0) {
    throw 'GPU query was attributed to polling frame or plotted as CPU duration'
   }
  }
  if (@($samples | Where-Object b -ne '0').Count) { throw 'Full effect coverage flag was lost' }
  Write-Output 'GPU trace: 40 total + 80 pass samples retain original submission frame and instant CPU location.'
 }
} finally {Pop-Location}
