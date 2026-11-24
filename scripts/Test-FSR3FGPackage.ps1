#Requires -Version 5.1
param([Parameter(Mandatory)][string]$PackageDirectory)
$ErrorActionPreference = 'Stop'
# Read-only payload validation. File hashes pin the matched SDK 2.3.0 pair;
# DLL product version does not describe the selected 3.1.6 algorithm provider.
$required = [ordered]@{
    'amd_fidelityfx_loader_dx12.dll' = 'E2D85AA05A9BD9ED8B38935FDF5199372CCA6F74C12015143BB6F945EE1608AA'
    'amd_fidelityfx_framegeneration_dx12.dll' = '02297BEEDD285E822D3A64F314CF00FAF378DCEC0EDC47FF0C4DD71B3A8C2F18'
    'effects/FSR3FG/FSR3_FrameGeneration.hlsl' = $null
    'AMD-FSR-SDK-LICENSE.md' = $null
    'AMD-FSR-SDK-THIRD-PARTY.md' = $null
    'FSR3FG-README.md' = $null
}
foreach ($entry in $required.GetEnumerator()) {
    $path = Join-Path $PackageDirectory $entry.Key
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "FSR3FG package is missing $($entry.Key)" }
    if ($entry.Value -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.Value) {
        throw "FSR3FG runtime differs from SDK 2.3.0: $($entry.Key). Validate provider/API compatibility before updating the pin."
    }
}
# Protect -SkipBuild from mixing new markers with an older binary/provider policy.
$marker = Get-Content -LiteralPath (Join-Path $PackageDirectory 'effects/FSR3FG/FSR3_FrameGeneration.hlsl') -Raw
if ($marker -match '(?m)^//!OPTION 0 None' -or $marker -match 'duplicateFrameFiltering' -or
    $marker -notmatch '(?s)//!LABEL Optical Flow Method\r?\n//!DEFAULT 1') {
    throw 'FSR3FG marker does not require AMDOF/NVOF with default AMDOF Quality.'
}
$amd = [regex]::Match($marker, '(?s)//!PARAMETER((?:(?!//!PARAMETER).)*)int amdOpticalFlowMode;')
if (!$amd.Success -or $amd.Groups[1].Value -notmatch '(?m)^//!DEFAULT 1\r?$') {
    throw 'FSR3FG marker AMDOF default is not Quality.'
}
$hasAmdCapability = $false
foreach ($binary in @('Magpie.exe', 'Magpie.Core.dll')) {
    $path = Join-Path $PackageDirectory $binary
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $hasAmdCapability = $hasAmdCapability -or [System.Text.Encoding]::ASCII.GetString($bytes).Contains('AMDOF capability v1: FSR3-FG required input')
    }
}
if (!$hasAmdCapability) { throw 'FSR3FG package has no compiled AMDOF capability marker. Rebuild the matching source with AMDOF enabled.' }
'FSR3FG payload matches pinned SDK, required AMDOF capability and parameter policy; GPU behavior remains unverified.'
