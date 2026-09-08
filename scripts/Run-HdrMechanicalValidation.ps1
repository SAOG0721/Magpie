# Lightweight source-level validation for the HDR mechanical slice.
# This script does not compile; it checks that the required configuration,
# protocol, dispatcher, diagnostics, and capture-processor scaffolding is
# present in the expected source files. Run from the repository root:
#
#   powershell -ExecutionPolicy Bypass -File scripts\Run-HdrMechanicalValidation.ps1

$ErrorActionPreference = 'Stop'

function Assert-Contains {
    param(
        [string]$Path,
        [string[]]$Expected,
        [string]$Label
    )

    if (-not (Test-Path $Path)) {
        throw "Missing file: $Path ($Label)"
    }

    $content = Get-Content $Path -Raw
    foreach ($needle in $Expected) {
        if (-not $content.Contains($needle)) {
            throw "Missing '$needle' in $Path ($Label)"
        }
    }

    Write-Host "PASS $Label"
}

$root = Split-Path -Parent $PSScriptRoot
$core = Join-Path $root 'src\Magpie.Core'
$app = Join-Path $root 'src\Magpie'

Assert-Contains (Join-Path $core 'include\ScalingOptions.h') @(
    'EnableHdrCompatibility = 1 << 23',
    'DEFINE_FLAG_ACCESSOR(IsHdrCompatibilityEnabled, ScalingFlags::EnableHdrCompatibility, flags)'
) 'ScalingOptions flag/accessor'

Assert-Contains (Join-Path $core 'ScalingOptions.cpp') @(
    'IsHdrCompatibilityEnabled: {}',
    'IsHdrCompatibilityEnabled(),'
) 'ScalingOptions log'

Assert-Contains (Join-Path $app 'Profile.h') @(
    'DEFINE_FLAG_ACCESSOR(IsHdrCompatibilityEnabled, ScalingFlags::EnableHdrCompatibility, scalingFlags)'
) 'Profile flag accessor'

Assert-Contains (Join-Path $app 'AppSettings.cpp') @(
    'writer.Key("enableHdrCompatibility")',
    'writer.Bool(profile.IsHdrCompatibilityEnabled())',
    'JsonHelper::ReadBoolFlag(profileObj, "enableHdrCompatibility", ScalingFlags::EnableHdrCompatibility, profile.scalingFlags);'
) 'Profile save/load'

Assert-Contains (Join-Path $app 'ProfilePage.xaml') @(
    'x:Uid="Profile_General_HdrCompatibility"',
    'IsHdrCompatibilityEnabled'
) 'Profile UI'

Assert-Contains (Join-Path $app 'ProfileViewModel.idl') @('Boolean IsHdrCompatibilityEnabled;') 'ProfileViewModel idl'
Assert-Contains (Join-Path $app 'ProfileViewModel.h') @('bool IsHdrCompatibilityEnabled() const noexcept;') 'ProfileViewModel header'
Assert-Contains (Join-Path $app 'ProfileViewModel.cpp') @('ProfileViewModel::IsHdrCompatibilityEnabled') 'ProfileViewModel cpp'

Assert-Contains (Join-Path $core 'HdrFrame.h') @(
    'struct HdrFormatRoute',
    'effectId',
    'optionId',
    'inputFormat',
    'outputFormat',
    'inputTransfer',
    'outputTransfer',
    'inputRange',
    'outputRange',
    'alphaMode',
    'evidenceLevel',
    'hdrNative',
    'adapterProfile',
    'defaultForHdr',
    'defaultForSdr'
) 'HDR route data structure'

Assert-Contains (Join-Path $core 'HdrProtocol.h') @(
    'SelectDefaultHdrRoute',
    'SelectDefaultSdrRoute',
    'GetAcceptedFormatRoutes',
    'GetHdrNativeFormatRoutes',
    'GetHdrAdapterFormatRoutes',
    'SerializeHdrFormatRoute'
) 'HDR protocol helpers'

Assert-Contains (Join-Path $core 'HdrAdapterDispatcher.h') @(
    'class HdrAdapterDispatcher',
    'DXGI_FORMAT_R16G16B16A16_FLOAT',
    'forwardParameters',
    'inverseParameters'
) 'HDR adapter dispatcher'

Assert-Contains (Join-Path $core 'HdrAdapterDispatcher.cpp') @(
    'case HdrAdapterProfile::DirectFP16',
    'case HdrAdapterProfile::BoundedHDR',
    'case HdrAdapterProfile::SDRCompatible',
    'case HdrAdapterProfile::ConditionalFP16',
    'case HdrAdapterProfile::Unknown',
    'case HdrAdapterProfile::PresentationTerminal'
) 'HDR adapter profiles'

Assert-Contains (Join-Path $core 'HdrColorTransform.cpp') @(
    'ShoulderCoefficients BuildShoulder',
    'value / (1.0f + c.k * (value - 1.0f))',
    'c.target + c.tailSlope * (value - c.peak)',
    'y * (1.0f - c.k) / std::max(1.0f - c.k * y, 1e-6f)',
    'BoundedRouteHighlightTarget = HdrColorTransform::BoundedRouteHighlightTarget',
    'SDR-compatible replicate contract',
    'float HdrColorTransform::EncodeBoundedHdr',
    'float HdrColorTransform::DecodeBoundedHdr'
) 'anchored monotone invertible shoulder curve family (CPU)'

Assert-Contains (Join-Path $core 'HdrSurfaceAdapter.cpp') @(
    'float ApplyShoulder(float value, float peak, float target, float k, float tailSlope)',
    'float InvertShoulder(float value, float peak, float target, float k, float tailSlope)',
    'value / (1.0 + k * (value - 1.0))',
    'target + tailSlope * (value - peak)',
    'value * (1.0 - k) / max(1.0 - k * value, 1e-6)',
    'Replicate contract for SDR-compatible backends',
    'max(value.rgb, 0.0) * normalizationScale',
    'float3 MapRec2020ToPqGamut(float3 value)',
    'float3 rec2020 = MapRec2020ToPqGamut(Rec709ToRec2020(value.rgb));',
    'result = Rec2020ToRec709(rec2020);'
) 'GPU shoulder curve matches CPU contract'

Assert-Contains (Join-Path $core 'FrameSourceBase.cpp') @(
    'HDR source SDR-white measurement failed'
) 'SDR white measurement fallback is logged'

Assert-Contains (Join-Path $core 'Renderer.cpp') @(
    'sdrWhite={:.1f} peak={:.1f} curvePeak={:.3f} curveTarget={:.3f}'
) 'DLSSNR boundary log carries curve parameters'

Assert-Contains (Join-Path $core 'DLSSSRUpscaler.cpp') @(
    '_hdrProtocol.hdrColorInput',
    'NVSDK_NGX_DLSS_Feature_Flags_IsHDR'
) 'DLSS HDR feature creation flag'

Assert-Contains (Join-Path $core 'EffectProtocolCatalogC.h') @(
    'optionId = "canonical-FP16-marker"',
    'adapterProfile = HdrAdapterProfile::PresentationTerminal'
) 'frame-generation marker preserves canonical FP16'

Assert-Contains (Join-Path $core 'HdrDiagnostics.h') @(
    'struct HdrDiagnostics',
    'hdrOptionEnabled',
    'captureMethod',
    'sourceFormat',
    'sourceColorDescription',
    'canonicalFormat',
    'selectedAdapterProfile',
    'selectedRouteId',
    'conversionPath',
    'fallbackReason'
) 'HDR diagnostics record'

Assert-Contains (Join-Path $core 'HdrCaptureProcessor.h') @(
    'Process(',
    'GetCanonicalTexture()',
    'GetFrameMetadata()',
    'ResetForResize()',
    'LastAssumption()'
) 'HdrCaptureProcessor interface'

Assert-Contains (Join-Path $core 'DLSSNRFilter.cpp') @(
    'const float hdrScale = hdrEnabled ? getParameter("experimentalHdrScale", 1.0f) : 1.0f;',
    'const bool hdrPath = hdrEnabled && getParameter("experimentalHdrPath", 0.0f) >= 0.5f;',
    'impl->useResolutionScaling = settings.enableInputResolutionScaling;',
    '.preserveHdrRange = impl.experimentalHdrPath ? 1u : 0u,',
    'chroma * chromaScale * directionalMultiplier;',
    'return original + residual;',
    'float3 output = PreserveHdrRange != 0 ? original + residual :'
) 'DLSSNR HDR setting boundary'

Assert-Contains (Join-Path $root 'src\Effects\DLSSNR\DLSSNR_AI_Filter.hlsl') @(
    '//!LABEL NR Intensity',
    '//!LABEL Local Tone Strength',
    '//!LABEL Local Structure Strength',
    '//!MAX 2'
) 'DLSSNR detail controls expose the signed-snippet range'

Assert-Contains (Join-Path $core 'DLSSNRFilter.cpp') @(
    'getClamped("intensity", 1.0f, 0.0f, 2.0f)',
    'getClamped("localToneStrength", 1.0f, 0.0f, 2.0f)',
    '"localStructureStrength", 1.0f, 0.0f, 2.0f)'
) 'DLSSNR settings preserve the signed-snippet strength range'

Assert-Contains (Join-Path $core 'NativeEffectBackendFactory.cpp') @(
    'if (!hdrEnabled) {',
    'if (IsSuperResolutionEffect(effectName)) {',
    'const DLSSNRSettings settings = ParseDLSSNRSettings(option, hdrEnabled);'
) 'Native backend SDR factory path'

Assert-Contains (Join-Path $core 'Renderer.cpp') @(
    'if (!hdrEnabled) {',
    'return {};'
) 'Renderer HDR route global gate'

Assert-Contains (Join-Path $core 'XeSSUpscaler.cpp') @(
    'if (!hdrEnabled) {',
    'inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||',
    'outputDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM',
    '(impl->hdrEnabled ? COLOR_CONVERT_HLSL : COLOR_CONVERT_LDR_HLSL)',
    'initParams.initFlags |= XESS_INIT_FLAG_LDR_INPUT_COLOR;'
) 'XeSS SDR U8 contract'

Assert-Contains (Join-Path $core 'XeSSFGPresenter.cpp') @(
    'return hdr ? HDR_COLOR_FORMAT : LDR_COLOR_FORMAT;',
    'return hdr ? OVERLAY_FORMAT : LDR_COLOR_FORMAT;',
    'if (impl->hdrEnabled) {',
    'DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020'
) 'XeSSFG terminal format boundary'

Assert-Contains (Join-Path $core 'FSR2Upscaler.cpp') @(
    'if (_hdrProtocol.hdrColorInput) {',
    '_exposure ? L"FSR2_Exposure" : L"FSR2_AutoExposure"'
) 'FSR2 exposure boundary'

Assert-Contains (Join-Path $core 'RTXVideoDenoiser.cpp') @(
    'if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {',
    'impl->inputScale = isFloatFormat(inputDesc.Format) ? 255.0f : 1.0f;',
    'if (!_hdrBoundary.hdrEnabled) {'
) 'RTX Video SDR U8 boundary'

Assert-Contains (Join-Path $core 'include\EffectCompiler.h') @(
    'static constexpr uint32_t HdrCompatibility = 1u << 12;'
) 'Effect cache HDR variant flag'

Assert-Contains (Join-Path $core 'EffectCompiler.cpp') @(
    'macros.emplace_back("MP_HDR_SATURATE",',
    'macros.emplace_back("MP_HDR_ALPHA",',
    'macros.emplace_back("MP_HDR_COMPATIBILITY", "1");'
) 'Effect shader HDR variants'

Assert-Contains (Join-Path $core 'NvidiaOpticalFlowProvider.cpp') @(
    'if (!hdrEnabled) {',
    'if (!HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM)) {',
    'inputDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;'
) 'NVIDIA Optical Flow SDR input contract'

Assert-Contains (Join-Path $core 'PassThroughFrames.cpp') @(
    'constexpr char REFERENCE_LDR_HLSL[]',
    'constexpr char REFERENCE_HDR_HLSL[]',
    'hdrEnabled ? REFERENCE_HDR_HLSL : REFERENCE_LDR_HLSL'
) 'Pass-through SDR shader contract'

Write-Host ''
Write-Host 'All HDR mechanical source-level validation checks passed.'
