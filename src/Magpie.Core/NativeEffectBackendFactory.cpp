#include "pch.h"
#include "RTXVideoHdr.h"
#include "NativeEffectBackendFactory.h"
#include "NgxD3D12Core.h"
#include "NgxRuntimeGuard.h"
#include "DLSSNRFilter.h"
#include "DLSSNRMultiPass.h"
#include "DLSSSRUpscaler.h"
#include "FSR2ZeroMVUpscaler.h"
#include "FSR3ZeroMVUpscaler.h"
#include "FSR2Upscaler.h"
#include "FSR3Upscaler.h"
#include "RTXVideoDenoiser.h"
#include "RTXVideoParameters.h"
#include "NvidiaVfxImageBackend.h"
#include "XeSSZeroMVUpscaler.h"
#include "XeSSUpscaler.h"
#include "FrameGuidanceDiagnostics.h"
#include "Logger.h"
#include "ScalingOptions.h"
#include "ScalingWindow.h"
#include "EffectParameterRules.h"
#include "OpticalFlowSettings.h"
#include "DeviceResources.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <winver.h>
#pragma comment(lib, "version.lib")

namespace Magpie {

static std::string DescribeNativeFailure(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* output, const Logger::DiagnosticCapture& diagnostic) {
	D3D11_TEXTURE2D_DESC inDesc{}, outDesc{};
	input->GetDesc(&inDesc);
	output->GetDesc(&outDesc);
	std::string result = fmt::format("Input={}x{} format={} / Output={}x{} format={}",
		inDesc.Width, inDesc.Height, static_cast<int>(inDesc.Format),
		outDesc.Width, outDesc.Height, static_cast<int>(outDesc.Format));
	if (auto* adapter = resources.GetGraphicsAdapter()) {
		DXGI_ADAPTER_DESC1 desc{};
		if (SUCCEEDED(adapter->GetDesc1(&desc))) result += fmt::format("\nGPU: {} / PCI={:04X}:{:04X} subsystem={:08X} revision={} LUID={:08X}:{:08X}",
			StrHelper::UTF16ToUTF8(desc.Description), desc.VendorId, desc.DeviceId, desc.SubSysId, desc.Revision,
			static_cast<uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart);
		LARGE_INTEGER driver{};
		if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver)))
			result += fmt::format(" / Driver: {}.{}.{}.{}", HIWORD(driver.HighPart), LOWORD(driver.HighPart),
				HIWORD(driver.LowPart), LOWORD(driver.LowPart));
	}
	if (!diagnostic.Details().empty()) result += "\n" + diagnostic.Details();
	return result;
}

// Read once at backend creation, not during Draw. No compatibility rule is
// inferred from a filename, display name, or an unverified version threshold.
static std::string DlssNrEnvironment(DeviceResources& resources, ID3D11Texture2D* input,
	ID3D11Texture2D* output, const Logger::DiagnosticCapture& diagnostic) {
	std::string result = DescribeNativeFailure(resources, input, output, diagnostic);
	const auto path = Win32Helper::GetExePath().parent_path() / L"nvngx_dlssnr.dll";
	result += "\nRequested DLSSNR DLL (on disk): " + StrHelper::UTF16ToUTF8(path.native());
	DWORD unused = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &unused);
	std::vector<uint8_t> data(size);
	VS_FIXEDFILEINFO* version = nullptr;
	UINT length = 0;
	if (size && GetFileVersionInfoW(path.c_str(), 0, size, data.data()) &&
		VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&version), &length) &&
		length >= sizeof(VS_FIXEDFILEINFO) && version->dwSignature == 0xFEEF04BD) {
		result += fmt::format(" / FileVersion: {}.{}.{}.{}", HIWORD(version->dwFileVersionMS),
			LOWORD(version->dwFileVersionMS), HIWORD(version->dwFileVersionLS), LOWORD(version->dwFileVersionLS));
	} else result += " / FileVersion: unavailable";
	return result;
}

template <typename T, typename... Args>
static NativeEffectBackendResult CreateBackend(
	std::string_view displayName,
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	Args&&... args
) noexcept {
	auto backend = std::make_unique<T>();
	Logger::DiagnosticCapture diagnostic;
	if (!backend->Initialize(
		resources, input, output, std::forward<Args>(args)...)) {
		Logger::Get().Error(fmt::format("Initialize native effect {} failed", displayName));
		return { true, nullptr, ScalingError::NoError,
			DescribeNativeFailure(resources, input, output, diagnostic), diagnostic.SystemError() };
	}
	return { true, std::move(backend) };
}

NativeEffectBackendResult CreateNativeEffectBackend(
	std::string_view effectName,
	const EffectOption& option,
	DeviceResources& resources,
	NgxD3D12Core& ngxCore,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	std::optional<bool> hdrDomain
) noexcept {
	const bool hdrEnabled = hdrDomain.value_or(ScalingWindow::Get().Options().IsHdrCompatibilityEnabled());
	if (effectName == "RTXVideo\\RTXVideo_HDR") {
#ifdef MP_ENABLE_NVIDIA_VFX
		auto result = CreateBackend<NvidiaVfxImageBackend>(effectName, resources, input, output, option, NvidiaVfxImageKind::TrueHdr);
#else
		auto result = CreateBackend<RTXVideoHdr>(effectName, resources, input, output, option);
#endif
		if (!result.backend) result.error = NgxRuntimeGuard::IsFaulted()
			? ScalingError::NgxRestartRequired : ScalingError::RtxHdrUnavailable;
		return result;
	}
	if (effectName == NVIDIA_VFX_UPSCALE_ID)
		return CreateBackend<NvidiaVfxImageBackend>(effectName, resources, input, output, option, NvidiaVfxImageKind::Upscale);
#ifdef MP_ENABLE_NVIDIA_VFX
	if (effectName == NVIDIA_VFX_VSR_ID)
		return CreateBackend<NvidiaVfxImageBackend>(effectName, resources, input, output, option, NvidiaVfxImageKind::Vsr);
#endif
	if (effectName == "Diagnostics\\FrameGuidance_Motion" ||
		effectName == "Diagnostics\\FrameGuidance_Confidence") {
		auto getParameter = [&](std::string_view name, float defaultValue) {
			auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? defaultValue : it->second;
		};
		FrameGuidanceDiagnosticKind kind =
			FrameGuidanceDiagnosticKind::Motion;
		if (effectName.ends_with("Confidence")) {
			kind = FrameGuidanceDiagnosticKind::Confidence;
		}
		return CreateBackend<FrameGuidanceDiagnostics>(
			effectName, resources, input, output,
			FrameGuidanceDiagnosticSettings{
				.kind = kind,
				.gain = std::max(0.001f, getParameter("gain",
					FrameGuidanceDiagnosticSettings{}.gain)),
				.motionRequest = ParseOpticalFlowRequest(option)
			});
	}

	if (effectName == "DLSSNR\\DLSSNR_AI_Filter") {
		// Renderer has selected the semantic HDR route before allocating these
		// endpoints. FP16 here confirms that decision, rather than classifying capture.
		D3D11_TEXTURE2D_DESC endpoint{};
		input->GetDesc(&endpoint);
		auto backend = std::make_unique<DLSSNRMultiPass>();
		Logger::DiagnosticCapture diagnostic;
		DXGI_ADAPTER_DESC1 adapter{};
		const bool nonNvidia = resources.GetGraphicsAdapter() &&
			SUCCEEDED(resources.GetGraphicsAdapter()->GetDesc1(&adapter)) && adapter.VendorId != 0x10DE;
		const bool initialized = !nonNvidia && backend->Initialize(resources, ngxCore, input, output, option,
			hdrEnabled && endpoint.Format == DXGI_FORMAT_R16G16B16A16_FLOAT);
		const auto environment = DlssNrEnvironment(resources, input, output, diagnostic) + "\n" +
			(backend->LibraryInfo().empty() ? "DLSSNR loaded module: not reached or unavailable" : backend->LibraryInfo());
		if (!initialized) {
			const bool multiPass = DLSSNRPassCount([&](std::string_view name, float fallback) {
				const auto it = option.parameters.find(std::string(name));
				return it == option.parameters.end() ? fallback : it->second;
			}) > 1;
			const bool stopped = multiPass || NgxRuntimeGuard::IsFaulted();
			const auto failure = nonNvidia
				? DlssNrFailure{ DlssNrStage::Initialize, DlssNrReason::UnsupportedAdapter, DlssNrCodeKind::None, 0, "Selected adapter is not NVIDIA" }
				: backend->GetDlssNrFailure();
			Logger::Get().Warn(fmt::format("DLSSNR STATUS: Feature=18 created=false fallback={}",
				stopped ? "none-chain-stopped" : "pass-through"));
			return { stopped, nullptr, stopped ? ScalingError::DlssNrStopped : ScalingError::DlssNrUnavailable,
				environment, 0, failure, environment };
		}
		return { true, std::move(backend), ScalingError::NoError, {}, 0, {}, environment };
	}


	if (!hdrEnabled) {
		if (IsSuperResolutionEffect(effectName)) {
			const auto motion = ParseOpticalFlowRequest(option);
			if (effectName == "DLSS\\DLSS_SR")
				return CreateBackend<DLSSSRUpscaler>(effectName, resources, input, output,
					DLSSSRSettings{ .motionRequest = motion, .renderPreset = ParseDLSSSRPreset(option) });
			if (effectName == "FSR2\\FSR2_SR")
				return CreateBackend<FSR2Upscaler>(effectName, resources, input, output, motion);
			if (effectName == "XeSS\\XeSS_SR")
				return CreateBackend<XeSSUpscaler>(effectName, resources, input, output, motion);
			return CreateBackend<FSR3Upscaler>(effectName, resources, input, output,
				motion, effectName == "FSR4\\FSR4_SR");
		}
	}

	// Keep the legacy ZeroMV/Jitter/OpticalFlow contracts on their original
	// backends. These names carry distinct temporal and auxiliary-resource
	// semantics even when HDR compatibility is disabled.
	if (hdrEnabled && (effectName == "DLSS\\DLSS_ZeroMV" ||
		effectName == "DLSS\\DLSS_ZeroMV_Jitter" ||
		effectName == "DLSS\\DLSS_OpticalFlow")) {
		auto getParameter = [&](std::string_view name, float defaultValue) {
			auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? defaultValue : it->second;
		};
		const bool isLegacyOpticalFlow = effectName == "DLSS\\DLSS_OpticalFlow";
		const auto quality = static_cast<NvidiaOpticalFlowQuality>(std::clamp(
			static_cast<int>(std::lround(getParameter("nvidiaOpticalFlowQuality", 2.0f))),
			0, int(NVIDIA_OPTICAL_FLOW_MAX_QUALITY)));
		return CreateBackend<DLSSSRUpscaler>(
			effectName, resources, input, output,
			DLSSSRSettings{
				.motionRequest = isLegacyOpticalFlow
					? MotionVectorRequest::Nvidia(quality)
					: MotionVectorRequest{},
				.renderPreset = 10 // Preserve the hidden legacy adapter's J contract.
			});
	}

	if (hdrEnabled && (effectName == "FSR2\\FSR2_ZeroMV" ||
		effectName == "FSR2\\FSR2_ZeroMV_Jitter" ||
		effectName == "FSR2\\FSR2_OpticalFlow")) {
		return CreateBackend<FSR2ZeroMVUpscaler>(effectName, resources, input, output,
			effectName == "FSR2\\FSR2_OpticalFlow",
			effectName == "FSR2\\FSR2_ZeroMV_Jitter");
	}

	const bool isFsr3 = effectName == "FSR3\\FSR3_ZeroMV" ||
		effectName == "FSR3\\FSR3_ZeroMV_Jitter" ||
		effectName == "FSR3\\FSR3_OpticalFlow";
	const bool isFsr4 = effectName == "FSR4\\FSR4_ZeroMV" ||
		effectName == "FSR4\\FSR4_ZeroMV_Jitter" ||
		effectName == "FSR4\\FSR4_OpticalFlow";
	if (hdrEnabled && (isFsr3 || isFsr4)) {
		return CreateBackend<FSR3ZeroMVUpscaler>(effectName, resources, input, output,
			effectName.ends_with("OpticalFlow"),
			effectName.ends_with("ZeroMV_Jitter"), isFsr4);
	}

	if (hdrEnabled && (effectName == "XeSS\\XeSS_ZeroMV" ||
		effectName == "XeSS\\XeSS_ZeroMV_Jitter" ||
		effectName == "XeSS\\XeSS_OpticalFlow")) {
		return CreateBackend<XeSSZeroMVUpscaler>(effectName, resources, input, output,
			effectName == "XeSS\\XeSS_OpticalFlow",
			effectName == "XeSS\\XeSS_ZeroMV_Jitter");
	}

	if (hdrEnabled && (effectName == "DLSS\\DLSS_SR" ||
		effectName == "FSR2\\FSR2_SR" ||
		effectName == "FSR3\\FSR3_SR" ||
		effectName == "FSR4\\FSR4_SR" ||
		effectName == "XeSS\\XeSS_SR")) {
		const auto motion = ParseOpticalFlowRequest(option);
		const D3D11_TEXTURE2D_DESC inputDesc = [&]() {
			D3D11_TEXTURE2D_DESC desc{};
			input->GetDesc(&desc);
			return desc;
		}();
		const bool hdrInput = hdrEnabled &&
			inputDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
		const FsrHdrProtocol hdrProtocol{
			.hdrColorInput = hdrInput,
			.transfer = GroupBTransfer::Linear,
			.preExposure = 1.0f,
			.exposure = 1.0f,
			.depthInverted = true,
			.depthInfinite = true,
			.useReactiveMask = true,
			.useTransparencyMask = true,
		};
		if (effectName == "DLSS\\DLSS_SR") {
			auto backend = std::make_unique<DLSSSRUpscaler>();
			backend->SetDlssHdrProtocol(hdrProtocol);
			Logger::DiagnosticCapture diagnostic;
			if (!backend->Initialize(resources, input, output,
				DLSSSRSettings{ .motionRequest = motion, .renderPreset = ParseDLSSSRPreset(option) })) {
				return { true, nullptr, NgxRuntimeGuard::IsFaulted() ?
					ScalingError::NgxRestartRequired : ScalingError::NoError,
					DescribeNativeFailure(resources, input, output, diagnostic), diagnostic.SystemError() };
			}
			return { true, std::move(backend) };
		}
		if (effectName == "FSR2\\FSR2_SR") {
			auto backend = std::make_unique<FSR2Upscaler>();
			backend->SetFsrHdrProtocol(hdrProtocol);
			Logger::DiagnosticCapture diagnostic;
			if (!backend->Initialize(resources, input, output, motion)) {
				return { true, nullptr, ScalingError::NoError,
					DescribeNativeFailure(resources, input, output, diagnostic), diagnostic.SystemError() };
			}
			return { true, std::move(backend) };
		}
		if (effectName == "XeSS\\XeSS_SR")
			return CreateBackend<XeSSUpscaler>(effectName, resources, input, output, motion);
		auto backend = std::make_unique<FSR3Upscaler>();
		backend->SetFsrHdrProtocol(hdrProtocol);
		Logger::DiagnosticCapture diagnostic;
		if (!backend->Initialize(resources, input, output, motion,
			effectName == "FSR4\\FSR4_SR")) {
			return { true, nullptr, ScalingError::NoError,
				DescribeNativeFailure(resources, input, output, diagnostic), diagnostic.SystemError() };
		}
		return { true, std::move(backend) };
	}

	const int rtxFamily = effectName == RTXVideoCanonicalId<char>(0) ? 0
		: effectName == RTXVideoCanonicalId<char>(1) ? 1 : -1;
	if (rtxFamily >= 0) {
		const auto categoryParameter = option.parameters.find("category");
		const int category = rtxFamily == 0 ? 3 : NvidiaVfxCategory(
			categoryParameter == option.parameters.end() ? 0.f : categoryParameter->second);
		// The legacy SDK supports Standard and Denoise only. Reject newer
		// categories rather than silently replacing them with another effect.
		if (category != 0 && category != 3) {
			Logger::Get().Error("This build's legacy RTX Video backend does not support the selected VFX category");
			return { true, nullptr, ScalingError::NoError, "Unsupported legacy RTX Video category" };
		}
		const bool isVsr = category == 0;
		const auto strength = option.parameters.find("strength");
		const int tier = strength == option.parameters.end() ? RTX_VIDEO_DEFAULT_STRENGTH
			: NormalizeRTXVideoStrength(strength->second);
		const uint32_t qualityLevel = RTXVideoQualityLevel(isVsr ? 1 : 0, tier);
		auto backend = std::make_unique<RTXVideoDenoiser>();
		Logger::DiagnosticCapture diagnostic;
		if (!backend->Initialize(resources, input, output, qualityLevel,
			isVsr ? RtxVideoEffectKind::Vsr : RtxVideoEffectKind::Denoise)) {
			Logger::Get().Error(fmt::format("Initialize native effect {} failed", effectName));
			return { true, nullptr, backend->InitializationError(),
				DescribeNativeFailure(resources, input, output, diagnostic), diagnostic.SystemError() };
		}
		return { true, std::move(backend) };
	}
	return {};
}

}
