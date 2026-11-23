#Requires -Version 7.0
param(
    [string]$OutputDirectory,
    [string]$ReferenceCommit = '03b39be4',
    [switch]$CompileOnly,
    [switch]$RunGpu
)
$ErrorActionPreference = 'Stop'
if ($CompileOnly -and $RunGpu) { throw 'Choose CompileOnly or RunGpu.' }
if ($ReferenceCommit -notmatch '^[0-9a-fA-F]{7,40}$') { throw 'ReferenceCommit must be a fixed commit ID.' }
$repo = Split-Path $PSScriptRoot -Parent
if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $env:TEMP ('Magpie-amd-confidence-' + [guid]::NewGuid().ToString('N'))
}
$outputPath = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$relativeSource = 'src/Magpie.Core/AmdOpticalFlowProvider.cpp'
$candidate = (Get-Content -LiteralPath (Join-Path $repo $relativeSource) -Raw) -replace "\r\n?", "`n"
$referenceLines = & git -C $repo show "$($ReferenceCommit):$relativeSource"
if ($LASTEXITCODE) { throw 'Cannot read the fixed unmodified provider reference.' }
$reference = ($referenceLines -join "`n") -replace "\r\n?", "`n"
function Extract-One([string]$Text, [string]$Pattern, [string]$Name) {
    $matches = [regex]::Matches($Text, $Pattern)
    if ($matches.Count -ne 1) { throw "Expected one production $Name, found $($matches.Count)." }
    return $matches[0]
}
$shaderPattern = '(?s)constexpr char DENSIFY_HLSL\[\] = R"\((?<body>.*?)\)";'
$methodPattern = '(?ms)^\tbool Densify\(\) noexcept \{.*?^\t\}\r?$'
$resetPattern = '(?ms)^\tif \(resetFrame\) \{.*?^\t\}\r?$'
$newShader = (Extract-One $candidate $shaderPattern 'candidate Densify shader').Groups['body'].Value
$oldShader = (Extract-One $reference $shaderPattern 'reference Densify shader').Groups['body'].Value
@('inline constexpr char AMD_CONFIDENCE_CANDIDATE_SHADER[] = R"(' + $newShader + ')";',
  'inline constexpr char AMD_CONFIDENCE_REFERENCE_SHADER[] = R"(' + $oldShader + ')";') -join "`n" |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceShadersUnderTest.h') -Encoding utf8NoBOM
(Extract-One $candidate $methodPattern 'candidate Densify method').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceDensifyUnderTest.inl') -Encoding utf8NoBOM
(Extract-One $reference $methodPattern 'reference Densify method').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceReferenceDensify.inl') -Encoding utf8NoBOM
(Extract-One $candidate $resetPattern 'candidate reset/valid branch').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceResetUnderTest.inl') -Encoding utf8NoBOM
(Extract-One $reference $resetPattern 'reference reset/valid branch').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceReferenceReset.inl') -Encoding utf8NoBOM
(Extract-One $candidate '(?m)^\tbool confidenceIsNormal = false;\r?$' 'confidence member initialization').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceStateUnderTest.inl') -Encoding utf8NoBOM
$destroy = (Extract-One $candidate '(?ms)^\tvoid Destroy\(\) noexcept \{(?<body>.*?)^\t\}\r?$' 'provider resource destruction').Groups['body'].Value
(Extract-One $destroy '(?m)^\t\tconfidenceIsNormal = false;\r?$' 'confidence resource-reset statement').Value |
    Set-Content -LiteralPath (Join-Path $outputPath 'AmdConfidenceDestroyUnderTest.inl') -Encoding utf8NoBOM
$newShader | Set-Content -LiteralPath (Join-Path $outputPath 'candidate.hlsl') -Encoding utf8NoBOM
$oldShader | Set-Content -LiteralPath (Join-Path $outputPath 'reference.hlsl') -Encoding utf8NoBOM
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
if (!$vs) { throw 'Visual C++ x64 tools were not found.' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
Push-Location $outputPath
try {
    foreach ($name in @('candidate', 'reference')) {
        & fxc.exe /nologo /T cs_5_0 /E Densify /O3 "/Fo:$outputPath/$name.cso" "$outputPath/$name.hlsl" `
            2>&1 | Tee-Object -FilePath (Join-Path $outputPath "$name-shader-build.log")
        if ($LASTEXITCODE) { throw "$name production shader compilation failed." }
    }
    & cl.exe /nologo /std:c++20 /EHsc /utf-8 /MT /O2 /W4 /WX /DNOMINMAX /DUNICODE /D_UNICODE `
        "/I$outputPath" "$PSScriptRoot/AmdOpticalFlowConfidenceTests.cpp" "/Fe:$outputPath/AmdOpticalFlowConfidenceTests.exe" `
        "/Fo:$outputPath/AmdOpticalFlowConfidenceTests.obj" d3d11.lib d3dcompiler.lib dxguid.lib `
        2>&1 | Tee-Object -FilePath (Join-Path $outputPath 'build.log')
    if ($LASTEXITCODE) { throw 'Confidence fixture compilation failed.' }
    if ($RunGpu) {
        & "$outputPath/AmdOpticalFlowConfidenceTests.exe" 2>&1 | Tee-Object -FilePath (Join-Path $outputPath 'test.log')
        if ($LASTEXITCODE) { throw 'Confidence GPU behavior test failed.' }
    } else {
        Write-Output 'Compile-only completed. No GPU fixture was run; use -RunGpu only in an approved sampling gap.'
    }
    Write-Output "AMDOF confidence artifacts: $outputPath"
} finally {
    Pop-Location
}
