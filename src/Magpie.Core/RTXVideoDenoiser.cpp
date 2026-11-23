#include "pch.h"
#include "RTXVideoDenoiser.h"
#include "RTXVideoParameters.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "ScalingWindow.h"
#include "StrHelper.h"
#include "Win32Helper.h"

#ifdef MP_ENABLE_RTX_VIDEO_DENOISE

#include <nvCVImage.h>
#include <nvVideoEffects.h>
#include <nvTransferD3D11.h>

// Required by NVIDIA's MIT-licensed proxy loader. USE_APP_PATH below makes it
// load the runtime copied beside Magpie.exe rather than an obsolete system SDK.
// The common NvidiaVfxRuntime owns the SDK proxy path symbol.

namespace Magpie {

using NvVFXCudaStreamSynchronizeFn = NvCV_Status(NvVFX_API*)(CUstream);

static std::wstring GetModulePath(HMODULE module) noexcept {
	if (!module) return {};
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) return {};
	path.resize(length);
	return path;
}

static HMODULE LoadRuntimeModule(const std::filesystem::path& directory, const wchar_t* name) noexcept {
	const auto expectedPath = directory / name;
	HMODULE module = GetModuleHandleW(name);
	if (!module) {
		SetLastError(ERROR_SUCCESS);
		module = LoadLibraryExW(
			expectedPath.c_str(), nullptr,
			LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	}
	if (!module) {
		const DWORD error = GetLastError();
		Logger::Get().ComError(fmt::format(
			"RTX Video LoadLibraryExW failed: {}", StrHelper::UTF16ToUTF8(expectedPath.native())),
			HRESULT_FROM_WIN32(error));
		return nullptr;
	}
	const std::wstring loadedPath = GetModulePath(module);
	Logger::Get().Info(fmt::format(
		"RTX Video runtime module: {} handle=0x{:X}",
		StrHelper::UTF16ToUTF8(loadedPath), reinterpret_cast<uintptr_t>(module)));
	if (_wcsicmp(loadedPath.c_str(), expectedPath.c_str()) != 0) {
		Logger::Get().Error(fmt::format(
			"RTX Video runtime module path mismatch: expected={}, loaded={}",
			StrHelper::UTF16ToUTF8(expectedPath.native()), StrHelper::UTF16ToUTF8(loadedPath)));
		return nullptr;
	}
	return module;
}

static HMODULE LoadSystemModule(const wchar_t* name) noexcept {
	HMODULE module = GetModuleHandleW(name);
	if (!module) {
		module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	}
	const DWORD error = module ? ERROR_SUCCESS : GetLastError();
	Logger::Get().Info(fmt::format(
		"RTX Video system module {}: handle=0x{:X} path={} win32={} hr=0x{:08X}",
		StrHelper::UTF16ToUTF8(name), reinterpret_cast<uintptr_t>(module),
		StrHelper::UTF16ToUTF8(GetModulePath(module)), error,
		static_cast<unsigned long>(HRESULT_FROM_WIN32(error))));
	return module;
}

static void LogExport(HMODULE module, const char* name) noexcept {
	const auto address = reinterpret_cast<uintptr_t>(GetProcAddress(module, name));
	const DWORD error = address ? ERROR_SUCCESS : GetLastError();
	Logger::Get().Info(fmt::format(
		"RTX Video export {}: module={} address=0x{:X} win32={} hr=0x{:08X}",
		name, StrHelper::UTF16ToUTF8(GetModulePath(module)), address, error,
		static_cast<unsigned long>(HRESULT_FROM_WIN32(error))));
}

static bool VFXSucceeded(NvCV_Status status, const char* operation) noexcept {
	if (status == NVCV_SUCCESS) {
		return true;
	}

	const char* message = NvCV_GetErrorStringFromCode(status);
	Logger::Get().Error(fmt::format(
		"{} failed: {} ({})", operation, message ? message : "unknown VFX error", (int)status));
	return false;
}

struct RTXVideoDenoiser::Impl {
	NvVFX_Handle effect = nullptr;
	CUstream stream = nullptr;
	ID3D11DeviceContext4* d3dContext = nullptr;
	NvCVImage inputD3D;
	NvCVImage outputD3D;
	NvCVImage inputGPU;
	NvCVImage outputGPU;
	NvCVImage outputCPU;
	NvCVImage_PixelFormat nativePixelFormat = NVCV_RGBA;
	unsigned outputWidth = 0;
	unsigned outputHeight = 0;
	NvCVImage temporaryGPU;
	NvVFXCudaStreamSynchronizeFn synchronize = nullptr;
	uint64_t drawCount = 0;
	float inputScale = 1.0f;
	float outputScale = 1.0f;

	~Impl() {
		if (effect) {
			NvVFX_DestroyEffect(effect);
			effect = nullptr;
		}

		// Dealloc also unregisters D3D resources initialized through
		// NvCVImage_InitFromD3D11Texture. It clears the descriptors, making
		// their subsequent C++ destructors no-ops.
		NvCVImage_Dealloc(&temporaryGPU);
		NvCVImage_Dealloc(&outputGPU);
		NvCVImage_Dealloc(&outputCPU);
		NvCVImage_Dealloc(&inputGPU);
		NvCVImage_Dealloc(&outputD3D);
		NvCVImage_Dealloc(&inputD3D);

		if (stream) {
			NvVFX_CudaStreamDestroy(stream);
			stream = nullptr;
		}
	}
};

RTXVideoDenoiser::RTXVideoDenoiser() = default;
RTXVideoDenoiser::~RTXVideoDenoiser() = default;

bool RTXVideoDenoiser::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	uint32_t qualityLevel,
	RtxVideoEffectKind kind
) noexcept {
	_impl.reset();
	_qualityLevel = qualityLevel;
	_kind = kind;
	_initializationError = ScalingError::NoError;

	const bool isVsr = kind == RtxVideoEffectKind::Vsr;
	const bool isDenoise = kind == RtxVideoEffectKind::Denoise;
	const bool validVsrQuality = isVsr && qualityLevel >= 1 && qualityLevel <= 4;
	const bool validDenoiseQuality = isDenoise && qualityLevel >= 8 && qualityLevel <= 11;
	const bool validHighBitrateQuality = isDenoise && qualityLevel >= 16 && qualityLevel <= 19;
	if (!validVsrQuality && !validDenoiseQuality && !validHighBitrateQuality) {
		Logger::Get().Error(fmt::format("Invalid RTX Video {} quality level: {}",
			isVsr ? "VSR" : "Denoise", qualityLevel));
		return false;
	}

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if ((isDenoise &&
		(inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height)) ||
		inputDesc.SampleDesc.Count != 1 || outputDesc.SampleDesc.Count != 1) {
		Logger::Get().Error("RTX Video received invalid output dimensions or an MSAA texture");
		return false;
	}

	const auto runtimeDirectory = Win32Helper::GetExePath().parent_path();
	if (runtimeDirectory.empty()) {
		Logger::Get().Error("RTX Video executable directory lookup failed");
		return false;
	}
	Logger::Get().Info(fmt::format(
		"RTX Video runtime directory: {}",
		StrHelper::UTF16ToUTF8(runtimeDirectory.native())));
	// The SDK proxies load by name. USE_APP_PATH suppresses their global
	// directory change, and these absolute-path loads supply their modules.
	if (!SetEnvironmentVariableW(L"NV_VIDEO_EFFECTS_PATH", L"USE_APP_PATH")) {
		Logger::Get().Win32Error("Set RTX Video SDK application-path mode failed");
		return false;
	}
	HMODULE cudaModule = LoadSystemModule(L"nvcuda.dll");
	HMODULE ngxModule = LoadRuntimeModule(runtimeDirectory, L"nvngxruntime.dll");
	HMODULE cvModule = LoadRuntimeModule(runtimeDirectory, L"NVCVImage.dll");
	HMODULE vfxModule = LoadRuntimeModule(runtimeDirectory, L"NVVideoEffects.dll");
	HMODULE superResModule = LoadRuntimeModule(runtimeDirectory, L"nvVFXVideoSuperRes.dll");
	if (!cudaModule || !ngxModule || !cvModule || !vfxModule || !superResModule) {
		Logger::Get().Error("RTX Video runtime dependency initialization failed");
		return false;
	}
	// Newer packages include the feature payload as a separate DLL. Older
	// legacy SDK layouts keep their own NGX feature discovery.
	if (Win32Helper::FileExists((runtimeDirectory / L"nvngx_vsr.dll").c_str()) &&
		!LoadRuntimeModule(runtimeDirectory, L"nvngx_vsr.dll")) return false;
	LogExport(cvModule, "NvCVImage_Alloc");
	LogExport(vfxModule, "NvVFX_CreateEffect");
	LogExport(vfxModule, "NvVFX_GetVersion");
	unsigned int runtimeVersion = 0;
	const NvCV_Status versionStatus = NvVFX_GetVersion(&runtimeVersion);
	if (!VFXSucceeded(versionStatus, "NvVFX_GetVersion")) {
		return false;
	}
	Logger::Get().Info(fmt::format(
		"RTX Video runtime version: 0x{:08X} ({}.{}.{})",
		runtimeVersion, (runtimeVersion >> 24) & 0xFF, (runtimeVersion >> 16) & 0xFF,
		(runtimeVersion >> 8) & 0xFF));

	auto impl = std::make_unique<Impl>();
	impl->d3dContext = resources.GetD3DDC();
	if (ScalingWindow::Get().Options().IsHdrCompatibilityEnabled()) {
		// The HDR boundary uses an explicit normalized-D3D/U8 bridge.
		impl->inputScale = 1.0f / 255.0f;
		impl->outputScale = 255.0f;
	} else {
		const auto isFloatFormat = [](DXGI_FORMAT format) noexcept {
			return format == DXGI_FORMAT_R16G16B16A16_FLOAT ||
				format == DXGI_FORMAT_R32G32B32A32_FLOAT ||
				format == DXGI_FORMAT_R11G11B10_FLOAT;
		};
		impl->inputScale = isFloatFormat(inputDesc.Format) ? 255.0f : 1.0f;
		impl->outputScale = isFloatFormat(outputDesc.Format) ? (1.0f / 255.0f) : 1.0f;
	}
	// Keep the native model in an explicit RGBA order. D3D11's endpoint is
	// BGRA8, so the boundary performs the channel swizzle exactly once.
	const NvCVImage_PixelFormat pixelFormat = NVCV_RGBA;
	impl->nativePixelFormat = pixelFormat;
	impl->outputWidth = outputDesc.Width;
	impl->outputHeight = outputDesc.Height;
	Logger::Get().Info(fmt::format(
		"RTX Video endpoint channel order: {}",
		pixelFormat == NVCV_BGRA ? "BGRA" : "RGBA"));
	if (!VFXSucceeded(NvVFX_CudaStreamCreate(&impl->stream), "NvVFX_CudaStreamCreate") ||
		!VFXSucceeded(NvCVImage_InitFromD3D11Texture(&impl->inputD3D, input),
			"NvCVImage_InitFromD3D11Texture(input)") ||
		!VFXSucceeded(NvCVImage_InitFromD3D11Texture(&impl->outputD3D, output),
			"NvCVImage_InitFromD3D11Texture(output)") ||
		!VFXSucceeded(NvCVImage_Alloc(&impl->inputGPU, inputDesc.Width, inputDesc.Height,
			pixelFormat, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32), "NvCVImage_Alloc(input)") ||
		!VFXSucceeded(NvCVImage_Alloc(&impl->outputGPU, outputDesc.Width, outputDesc.Height,
			pixelFormat, NVCV_U8, NVCV_INTERLEAVED, NVCV_GPU, 32), "NvCVImage_Alloc(output)")) {
		return false;
	}
	Logger::Get().Info(fmt::format(
		"RTX Video NvCV formats: inputD3D={} inputGPU={} outputD3D={} outputGPU={} "
		"types={}/{}/{}/{} layouts={}/{}/{}/{}",
		static_cast<int>(impl->inputD3D.pixelFormat),
		static_cast<int>(impl->inputGPU.pixelFormat),
		static_cast<int>(impl->outputD3D.pixelFormat),
		static_cast<int>(impl->outputGPU.pixelFormat),
		static_cast<int>(impl->inputD3D.componentType),
		static_cast<int>(impl->inputGPU.componentType),
		static_cast<int>(impl->outputD3D.componentType),
		static_cast<int>(impl->outputGPU.componentType),
		static_cast<int>(impl->inputD3D.planar),
		static_cast<int>(impl->inputGPU.planar),
		static_cast<int>(impl->outputD3D.planar),
		static_cast<int>(impl->outputGPU.planar)));

	const NvCV_Status createEffectStatus =
		NvVFX_CreateEffect("VideoSuperRes", &impl->effect);
	if (!VFXSucceeded(createEffectStatus, "NvVFX_CreateEffect(VideoSuperRes)")) {
		if (createEffectStatus == NVCV_ERR_UNIMPLEMENTED) {
			_initializationError = ScalingError::NvidiaVsrPathUnsupported;
		}
		return false;
	}

	if (!VFXSucceeded(NvVFX_SetImage(impl->effect, NVVFX_INPUT_IMAGE, &impl->inputGPU),
			"NvVFX_SetImage(input)") ||
		!VFXSucceeded(NvVFX_SetImage(impl->effect, NVVFX_OUTPUT_IMAGE, &impl->outputGPU),
			"NvVFX_SetImage(output)") ||
		!VFXSucceeded(NvVFX_SetCudaStream(impl->effect, NVVFX_CUDA_STREAM, impl->stream),
			"NvVFX_SetCudaStream") ||
		!VFXSucceeded(NvVFX_SetU32(impl->effect, "QualityLevel", qualityLevel),
			"NvVFX_SetU32(QualityLevel)") ||
		!VFXSucceeded(NvVFX_Load(impl->effect), "NvVFX_Load(VideoSuperRes)")) {
		return false;
	}

	HMODULE module = GetModuleHandleW(L"NVVideoEffects.dll");
	if (module) {
		impl->synchronize = reinterpret_cast<NvVFXCudaStreamSynchronizeFn>(
			GetProcAddress(module, "NvVFX_CudaStreamSynchronize"));
	}
	if (!impl->synchronize) {
		Logger::Get().Error("NvVFX_CudaStreamSynchronize is unavailable");
		return false;
	}

	Logger::Get().Info(fmt::format(
		"RTX Video {} initialized: quality={}, {}x{}, inputFormat={}, outputFormat={}, scales={}/{} staging=CPU-U8",
		isVsr ? "VSR" : "Denoise", qualityLevel, inputDesc.Width, inputDesc.Height,
		(int)inputDesc.Format, (int)outputDesc.Format, impl->inputScale, impl->outputScale));
	_impl = std::move(impl);
	return true;
}

bool RTXVideoDenoiser::Resize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	return Initialize(deviceResources, input, output, _qualityLevel, _kind);
}

bool RTXVideoDenoiser::ApplyParameters(const EffectOption& option,
	std::span<const std::string> names) noexcept {
	if (!_impl || std::ranges::any_of(names, [](const auto& name) { return name != "strength"; })) return false;
	const auto value = option.parameters.find("strength");
	if (value == option.parameters.end() || value->second != float(NormalizeRTXVideoStrength(value->second))) return false;
	const uint32_t quality = RTXVideoQualityLevel(_kind == RtxVideoEffectKind::Vsr ? 1 : 0,
		NormalizeRTXVideoStrength(value->second));
	if (quality == _qualityLevel) return true;
	// This executes on the existing backend thread, between frames. The UI /
	// presentation thread remains available while NvVFX_Load prepares the model.
	// Both handles use the same synchronized endpoints; never Run them together.
	if (!VFXSucceeded(_impl->synchronize(_impl->stream), "RTX Video synchronize before tier change")) return false;
	NvVFX_Handle candidate = nullptr;
	auto cleanup = wil::scope_exit([&] { if (candidate) NvVFX_DestroyEffect(candidate); });
	Logger::Get().Info(fmt::format("RTX Video tier applying: {} -> {}", _qualityLevel, quality));
	if (!VFXSucceeded(NvVFX_CreateEffect("VideoSuperRes", &candidate), "RTX Video create candidate") ||
		!VFXSucceeded(NvVFX_SetImage(candidate, NVVFX_INPUT_IMAGE, &_impl->inputGPU), "RTX Video candidate input") ||
		!VFXSucceeded(NvVFX_SetImage(candidate, NVVFX_OUTPUT_IMAGE, &_impl->outputGPU), "RTX Video candidate output") ||
		!VFXSucceeded(NvVFX_SetCudaStream(candidate, NVVFX_CUDA_STREAM, _impl->stream), "RTX Video candidate stream") ||
		!VFXSucceeded(NvVFX_SetU32(candidate, "QualityLevel", quality), "RTX Video candidate quality") ||
		!VFXSucceeded(NvVFX_Load(candidate), "RTX Video load candidate")) {
		Logger::Get().Warn(fmt::format("RTX Video tier rejected; retained quality={}", _qualityLevel));
		return false;
	}
	std::swap(_impl->effect, candidate);
	_qualityLevel = quality;
	Logger::Get().Info(fmt::format("RTX Video tier applied: quality={}", quality));
	return true;
}

bool RTXVideoDenoiser::Draw(const NativeEffectDrawContext& drawContext) noexcept {
	ID3D11Texture2D* input = drawContext.input;
	ID3D11Texture2D* output = drawContext.output;
	if (!_impl) {
		return false;
	}

	// Resize should have recreated the wrappers whenever these resources change.
	(void)input;
	D3D11_TEXTURE2D_DESC outputDesc{};
	output->GetDesc(&outputDesc);

	bool inputMapped = false;
	auto unmapResources = [&]() noexcept {
		if (inputMapped) {
			NvCVImage_UnmapResource(&_impl->inputD3D, _impl->stream);
			inputMapped = false;
		}
	};

	if (!VFXSucceeded(NvCVImage_MapResource(&_impl->inputD3D, _impl->stream),
		"NvCVImage_MapResource(input)")) {
		return false;
	}
	inputMapped = true;
	if (!VFXSucceeded(NvCVImage_Transfer(&_impl->inputD3D, &_impl->inputGPU,
		_impl->inputScale, _impl->stream, &_impl->temporaryGPU), "NvCVImage_Transfer(input)")) {
		unmapResources();
		return false;
	}
	if (!VFXSucceeded(NvCVImage_UnmapResource(&_impl->inputD3D, _impl->stream),
		"NvCVImage_UnmapResource(input)")) {
		inputMapped = false;
		return false;
	}
	inputMapped = false;

	if (!VFXSucceeded(NvVFX_Run(_impl->effect, 0), "NvVFX_Run(VideoSuperRes)")) {
		return false;
	}

	// HDR compatibility owns the explicit CPU-U8 bridge below. With HDR off,
	// preserve the original native D3D11/CUDA interop path byte-for-byte in
	// behavior: the effect writes directly back to its D3D11 endpoint.
	if (!_hdrBoundary.hdrEnabled) {
		bool outputMapped = false;
		if (!VFXSucceeded(NvCVImage_MapResource(&_impl->outputD3D, _impl->stream),
			"NvCVImage_MapResource(output)")) {
			return false;
		}
		outputMapped = true;
		if (!VFXSucceeded(NvCVImage_Transfer(&_impl->outputGPU, &_impl->outputD3D,
			_impl->outputScale, _impl->stream, &_impl->temporaryGPU),
			"NvCVImage_Transfer(output)")) {
			NvCVImage_UnmapResource(&_impl->outputD3D, _impl->stream);
			return false;
		}
		if (!VFXSucceeded(NvCVImage_UnmapResource(&_impl->outputD3D, _impl->stream),
			"NvCVImage_UnmapResource(output)")) {
			outputMapped = false;
			return false;
		}
		outputMapped = false;
		if (!VFXSucceeded(_impl->synchronize(_impl->stream),
			"NvVFX_CudaStreamSynchronize")) {
			return false;
		}
		++_impl->drawCount;
		if (_impl->drawCount <= 2) {
			Logger::Get().Info(fmt::format(
				"RTX Video native Draw succeeded: mode=SDR-direct frame={} output={}x{} format={}",
				_impl->drawCount, outputDesc.Width, outputDesc.Height,
				static_cast<int>(outputDesc.Format)));
		}
		return true;
	}

	// The SDK accepts the D3D11 input interop path, while this runtime rejects
	// direct GPU-U8 -> D3D11-U8 output transfer with NVCV_ERR_PIXELFORMAT.
	// Transfer the native GPU result to a CPU U8 image, then upload the exact
	// interleaved bytes into the effect-local D3D11 endpoint.
	if (!_impl->outputCPU.pixels && !VFXSucceeded(NvCVImage_Alloc(
		&_impl->outputCPU, _impl->outputWidth, _impl->outputHeight,
		_impl->nativePixelFormat, NVCV_U8, NVCV_INTERLEAVED, NVCV_CPU, 32),
		"NvCVImage_Alloc(outputCPU)")) {
		return false;
	}
	if (!VFXSucceeded(NvCVImage_Transfer(&_impl->outputGPU, &_impl->outputCPU,
		_impl->outputScale, _impl->stream, &_impl->temporaryGPU), "NvCVImage_Transfer(outputCPU)")) {
		unmapResources();
		return false;
	}
	if (!VFXSucceeded(_impl->synchronize(_impl->stream), "NvVFX_CudaStreamSynchronize(outputCPU)")) {
		return false;
	}
	if (!_impl->d3dContext || !_impl->outputCPU.pixels || _impl->outputCPU.pitch <= 0) {
		Logger::Get().Error("RTX Video CPU output staging is unavailable");
		return false;
	}
	std::vector<uint8_t> bgra(
		size_t(_impl->outputCPU.pitch) * outputDesc.Height);
	const auto* rgba = static_cast<const uint8_t*>(_impl->outputCPU.pixels);
	for (UINT y = 0; y < outputDesc.Height; ++y) {
		const auto* src = rgba + size_t(y) * _impl->outputCPU.pitch;
		auto* dst = bgra.data() + size_t(y) * _impl->outputCPU.pitch;
		for (UINT x = 0; x < outputDesc.Width; ++x) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	_impl->d3dContext->UpdateSubresource(
		output, 0, nullptr, bgra.data(),
		static_cast<UINT>(_impl->outputCPU.pitch), 0);
	{
		++_impl->drawCount;
		if (_impl->drawCount <= 2) {
			Logger::Get().Info(fmt::format(
				"RTX Video native Draw succeeded: frame={} outputProtocol=RGBA/BGRA U8 [0,255] "
				"canonicalScale={}/{} output={}x{} format={}",
				_impl->drawCount, _impl->inputScale, _impl->outputScale,
				outputDesc.Width, outputDesc.Height, static_cast<int>(outputDesc.Format)));
		}
	}
	return true;
}

}

#else

namespace Magpie {

struct RTXVideoDenoiser::Impl {};

RTXVideoDenoiser::RTXVideoDenoiser() = default;
RTXVideoDenoiser::~RTXVideoDenoiser() = default;

bool RTXVideoDenoiser::Initialize(
	DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, uint32_t, RtxVideoEffectKind) noexcept {
	Logger::Get().Error("RTX Video denoise is disabled at build time");
	return false;
}

bool RTXVideoDenoiser::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept {
	return false;
}

bool RTXVideoDenoiser::Draw(const NativeEffectDrawContext&) noexcept {
	return false;
}

bool RTXVideoDenoiser::ApplyParameters(const EffectOption&, std::span<const std::string>) noexcept {
	return false;
}

}

#endif

namespace Magpie {
EffectParameterApplyMode RTXVideoDenoiser::GetParameterApplyMode(std::string_view name) const noexcept {
	return name == "strength" ? EffectParameterApplyMode::Live : EffectParameterApplyMode::Restricted;
}
}
