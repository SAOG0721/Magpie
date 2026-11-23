#include "pch.h"
#include "FSR2ZeroMVUpscaler.h"
#include "DeviceResources.h"
#include "FidelityFxUpscaleRuntime.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "HalfResOpticalFlow.h"

#ifdef MP_ENABLE_FSR2_ZEROMV
#include <ffx_fsr2.h>
#include <dx11/ffx_fsr2_dx11.h>

namespace Magpie {

template <typename T>
static T LoadProc(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

FSR2ZeroMVUpscaler::~FSR2ZeroMVUpscaler() { _Reset(); }

void FSR2ZeroMVUpscaler::_Reset() noexcept {
	if (_context && _contextDestroy) {
		reinterpret_cast<decltype(&ffxFsr2ContextDestroy)>(_contextDestroy)(
			static_cast<FfxFsr2Context*>(_context));
	}
	delete static_cast<FfxFsr2Context*>(_context);
	_context = nullptr;
	delete[] static_cast<char*>(_scratch);
	_scratch = nullptr;
	_scratchSize = 0;
	_zeroMotionUav = nullptr;
	_zeroMotion = nullptr;
	_zeroDepthUav = nullptr;
	_zeroDepth = nullptr;
	_reactiveUav = nullptr;
	_reactive = nullptr;
	if (_backendModule) FreeLibrary(_backendModule);
	if (_coreModule) FreeLibrary(_coreModule);
	_backendModule = nullptr;
	_coreModule = nullptr;
	_contextCreate = _contextDestroy = _contextDispatch = nullptr;
	_getInterface = _getScratchSize = _getDevice = _getResource = nullptr;
	_device = nullptr;
	_d3dDC = nullptr;
	_resetHistory = true;
	_enableOpticalFlow = false;
	_enableJitter = false;
	_frameIndex = 0;
	_opticalFlow.reset();
}

bool FSR2ZeroMVUpscaler::Initialize(
	DeviceResources& resources, ID3D11Texture2D* input, ID3D11Texture2D* output,
	bool enableOpticalFlow, bool enableJitter
) noexcept {
	_Reset();
	_enableOpticalFlow = enableOpticalFlow;
	_enableJitter = enableJitter;
	_device = resources.GetD3DDevice();
	_d3dDC = resources.GetD3DDC();
	D3D11_TEXTURE2D_DESC inDesc{}, outDesc{};
	input->GetDesc(&inDesc);
	output->GetDesc(&outDesc);
	if (inDesc.Width > outDesc.Width || inDesc.Height > outDesc.Height) return false;

	_coreModule = LoadPackagedFidelityFxUpscaleModule(L"ffx_fsr2_api_x64.dll");
	if (_coreModule) _backendModule = LoadPackagedFidelityFxUpscaleModule(L"ffx_fsr2_api_dx11_x64.dll");
	if (!_coreModule || !_backendModule) {
		Logger::Get().Win32Error("Load FSR2 D3D11 runtime failed");
		_Reset();
		return false;
	}
	_contextCreate = LoadProc<decltype(&ffxFsr2ContextCreate)>(_coreModule, "ffxFsr2ContextCreate");
	_contextDestroy = LoadProc<decltype(&ffxFsr2ContextDestroy)>(_coreModule, "ffxFsr2ContextDestroy");
	_contextDispatch = LoadProc<decltype(&ffxFsr2ContextDispatch)>(_coreModule, "ffxFsr2ContextDispatch");
	_getInterface = LoadProc<decltype(&ffxFsr2GetInterfaceDX11)>(_backendModule, "ffxFsr2GetInterfaceDX11");
	_getScratchSize = LoadProc<decltype(&ffxFsr2GetScratchMemorySizeDX11)>(_backendModule, "ffxFsr2GetScratchMemorySizeDX11");
	_getDevice = LoadProc<decltype(&ffxGetDeviceDX11)>(_backendModule, "ffxGetDeviceDX11");
	_getResource = LoadProc<decltype(&ffxGetResourceDX11)>(_backendModule, "ffxGetResourceDX11");
	if (!_contextCreate || !_contextDestroy || !_contextDispatch || !_getInterface ||
		!_getScratchSize || !_getDevice || !_getResource) {
		Logger::Get().Error("FSR2 D3D11 runtime exports are incomplete");
		_Reset();
		return false;
	}

	_zeroMotion = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R16G16_FLOAT,
		inDesc.Width, inDesc.Height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	_zeroDepth = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R32_FLOAT,
		inDesc.Width, inDesc.Height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	_reactive = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R8_UNORM,
		inDesc.Width, inDesc.Height, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
	HRESULT hr = _device->CreateUnorderedAccessView(_zeroMotion.get(), nullptr, _zeroMotionUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(_zeroDepth.get(), nullptr, _zeroDepthUav.put());
	if (SUCCEEDED(hr)) hr = _device->CreateUnorderedAccessView(_reactive.get(), nullptr, _reactiveUav.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create FSR2 Zero-MV auxiliary resources failed", hr);
		_Reset();
		return false;
	}
	if (_enableOpticalFlow) {
		_opticalFlow = std::make_unique<HalfResOpticalFlow>();
		if (!_opticalFlow->Initialize(_device, _d3dDC, input)) {
			Logger::Get().Error("Initialize FSR2 half-resolution optical flow failed");
			_Reset();
			return false;
		}
	}

	FfxFsr2ContextDescription desc{};
	_scratchSize = reinterpret_cast<decltype(&ffxFsr2GetScratchMemorySizeDX11)>(_getScratchSize)();
	_scratch = new (std::nothrow) char[_scratchSize];
	_context = new (std::nothrow) FfxFsr2Context{};
	if (!_scratch || !_context) { _Reset(); return false; }
	FfxErrorCode ec = reinterpret_cast<decltype(&ffxFsr2GetInterfaceDX11)>(_getInterface)(
		&desc.callbacks, _device, _scratch, _scratchSize);
	if (ec != FFX_OK) { Logger::Get().Error(fmt::format("ffxFsr2GetInterfaceDX11 failed ({})", (int)ec)); _Reset(); return false; }
	desc.device = reinterpret_cast<decltype(&ffxGetDeviceDX11)>(_getDevice)(_device);
	desc.maxRenderSize = { inDesc.Width, inDesc.Height };
	desc.displaySize = { outDesc.Width, outDesc.Height };
	desc.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE | FFX_FSR2_ENABLE_DEPTH_INVERTED |
		FFX_FSR2_ENABLE_DEPTH_INFINITE;
	ec = reinterpret_cast<decltype(&ffxFsr2ContextCreate)>(_contextCreate)(
		static_cast<FfxFsr2Context*>(_context), &desc);
	if (ec != FFX_OK) { Logger::Get().Error(fmt::format("ffxFsr2ContextCreate failed ({})", (int)ec)); _Reset(); return false; }
	Logger::Get().Info(fmt::format("FSR2 D3D11 initialized (opticalFlow={}, jitter={}): {}x{} -> {}x{}",
		_enableOpticalFlow, _enableJitter, inDesc.Width, inDesc.Height, outDesc.Width, outDesc.Height));
	return true;
}

bool FSR2ZeroMVUpscaler::Resize(DeviceResources& r, ID3D11Texture2D* i, ID3D11Texture2D* o) noexcept {
	const bool enableOpticalFlow = _enableOpticalFlow;
	const bool enableJitter = _enableJitter;
	return Initialize(r, i, o, enableOpticalFlow, enableJitter);
}

static float Halton(uint32_t index, uint32_t base) noexcept {
	float result = 0.0f;
	float fraction = 1.0f;
	while (index) {
		fraction /= (float)base;
		result += fraction * (float)(index % base);
		index /= base;
	}
	return result;
}

bool FSR2ZeroMVUpscaler::Draw(const NativeEffectDrawContext& drawContext) noexcept {
	ID3D11Texture2D* input = drawContext.input;
	ID3D11Texture2D* output = drawContext.output;
	if (!_context) return false;
	const bool resetInput = drawContext.inputHistoryReset || !drawContext.isNewCaptureFrame;
	_resetHistory |= resetInput;
	if (resetInput && _opticalFlow) _opticalFlow->ResetHistory();
	static constexpr float ZERO[4]{};
	static constexpr float REACTIVE_OF[4]{ 0.5f,0.5f,0.5f,0.5f };
	static constexpr float REACTIVE_ZEROMV[4]{ 0.9f,0.9f,0.9f,0.9f };
	_d3dDC->ClearUnorderedAccessViewFloat(_zeroDepthUav.get(), ZERO);
	ID3D11Texture2D* motionVectors = _zeroMotion.get();
	if (_enableOpticalFlow) {
		if (!_opticalFlow || !_opticalFlow->Estimate(input)) return false;
		motionVectors = _opticalFlow->GetMotionTexture();
		_d3dDC->ClearUnorderedAccessViewFloat(_reactiveUav.get(), REACTIVE_OF);
	} else {
		_d3dDC->ClearUnorderedAccessViewFloat(_zeroMotionUav.get(), ZERO);
		_d3dDC->ClearUnorderedAccessViewFloat(_reactiveUav.get(), REACTIVE_ZEROMV);
	}
	D3D11_TEXTURE2D_DESC inDesc{};
	input->GetDesc(&inDesc);
	auto getResource = reinterpret_cast<decltype(&ffxGetResourceDX11)>(_getResource);
	FfxFsr2DispatchDescription d{};
	d.commandList = _d3dDC;
	d.color = getResource(static_cast<FfxFsr2Context*>(_context), input, L"FSR2_InputColor", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.depth = getResource(static_cast<FfxFsr2Context*>(_context), _zeroDepth.get(), L"FSR2_ZeroDepth", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.motionVectors = getResource(static_cast<FfxFsr2Context*>(_context), motionVectors,
		_enableOpticalFlow ? L"FSR2_OpticalFlow" : L"FSR2_ZeroMotion", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.exposure = getResource(static_cast<FfxFsr2Context*>(_context), nullptr, L"FSR2_AutoExposure", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.reactive = getResource(static_cast<FfxFsr2Context*>(_context), _reactive.get(), L"FSR2_FullReactive", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.transparencyAndComposition = getResource(static_cast<FfxFsr2Context*>(_context), nullptr, nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
	d.output = getResource(static_cast<FfxFsr2Context*>(_context), output, L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	// HalfResOpticalFlow stores motion directly in pixel units, so no render-size
	// multiplication is needed. Applying width/height here made OF vectors huge.
	d.motionVectorScale = { 1.0f, 1.0f };
	if (_enableJitter) {
		// Metadata-only jitter: Magpie cannot modify the source application's projection.
		const uint32_t phase = drawContext.isNewCaptureFrame ? _frameIndex++ : (_frameIndex ? _frameIndex - 1 : 0);
		const uint32_t sample = (phase & 7u) + 1u;
		d.jitterOffset = { Halton(sample, 2) - 0.5f, Halton(sample, 3) - 0.5f };
	} else {
		d.jitterOffset = { 0.0f, 0.0f };
	}
	d.renderSize = { inDesc.Width, inDesc.Height };
	d.enableSharpening = true;
	d.sharpness = 0.2f;
	d.frameTimeDelta = 16.6667f;
	d.preExposure = 1.0f;
	d.reset = _resetHistory;
	d.cameraNear = 1.0f;
	d.cameraFar = FLT_MAX;
	d.cameraFovAngleVertical = 1.04719755f;
	d.viewSpaceToMetersFactor = 1.0f;
	const FfxErrorCode ec = reinterpret_cast<decltype(&ffxFsr2ContextDispatch)>(_contextDispatch)(
		static_cast<FfxFsr2Context*>(_context), &d);
	if (ec != FFX_OK) { Logger::Get().Error(fmt::format("ffxFsr2ContextDispatch failed ({})", (int)ec)); return false; }
	_resetHistory = false;
	return true;
}

}
#else
namespace Magpie {
FSR2ZeroMVUpscaler::~FSR2ZeroMVUpscaler() = default;
void FSR2ZeroMVUpscaler::_Reset() noexcept {}
bool FSR2ZeroMVUpscaler::Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, bool, bool) noexcept { return false; }
bool FSR2ZeroMVUpscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept { return false; }
bool FSR2ZeroMVUpscaler::Draw(const NativeEffectDrawContext&) noexcept { return false; }
}
#endif
