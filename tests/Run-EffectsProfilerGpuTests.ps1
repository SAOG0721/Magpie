#Requires -Version 7.0
param([Parameter(Mandatory)][string]$IntermediateDirectory,
      [string]$OutputDirectory=(Join-Path $env:TEMP ('Magpie-profiler-gpu-'+[guid]::NewGuid())))
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
 & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE /DFMT_HEADER_ONLY @includes "$repo/tests/EffectsProfilerGpuTests.cpp" "$repo/src/Magpie.Core/EffectsProfiler.cpp" "$repo/src/Shared/SmallVector.cpp" /Fe:EffectsProfilerGpuTests.exe d3d11.lib dxgi.lib runtimeobject.lib oleaut32.lib
 if($LASTEXITCODE){throw 'GPU profiler test compile failed'}
 & '.\EffectsProfilerGpuTests.exe'
 if($LASTEXITCODE){throw 'GPU profiler tests failed'}
} finally {Pop-Location}
