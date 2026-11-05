#Requires -Version 7.0
# Run Run-OKLabCompilerTests.ps1 first. This creates a test-only 20-step candidate;
# production source, package outputs and the current shader results stay intact.
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$baseline=Join-Path $repo 'obj/tests/oklab-compiler'
$candidate=Join-Path $repo 'obj/tests/oklab-gamut20'
New-Item -ItemType Directory -Path $candidate -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $baseline 'OKLabCompilerTests.exe') -Destination $candidate
New-Item -ItemType Directory -Path (Join-Path $candidate 'effects') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repo 'src/Effects/OKLabImageAdjustment.hlsl') -Destination (Join-Path $candidate 'effects')
Copy-Item -LiteralPath (Join-Path $repo 'src/Effects/OKLab') -Destination (Join-Path $candidate 'effects') -Recurse -Force
$color=Join-Path $candidate 'effects/OKLab/Color.hlsli'
$text=[IO.File]::ReadAllText($color)
if(([regex]::Matches($text,'i<24')).Count -ne 1){throw 'Production gamut search changed; review candidate experiment.'}
[IO.File]::WriteAllText($color,$text.Replace('i<24','i<20'))
Push-Location $candidate
try { & .\OKLabCompilerTests.exe; if($LASTEXITCODE){throw 'Candidate effect compilation failed'} }
finally { Pop-Location }
& (Join-Path $PSScriptRoot 'Run-OKLabTests.ps1') -ProductionShaderDirectory $candidate
if($LASTEXITCODE){throw 'Candidate numerical test failed'}
& (Join-Path $PSScriptRoot 'Run-OKLabBenchmark.ps1') -ProductionShaderDirectory $candidate
if($LASTEXITCODE){throw 'Candidate GPU benchmark failed'}
