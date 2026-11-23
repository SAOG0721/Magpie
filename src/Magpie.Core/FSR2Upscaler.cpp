#include "pch.h"
#include "FSR2Upscaler.h"
#include "DeviceResources.h"
#include "FidelityFxUpscaleRuntime.h"
#include "DirectXHelper.h"
#include "Logger.h"

#ifdef MP_ENABLE_FSR2_ZEROMV
#include <ffx_fsr2.h>
#include <dx11/ffx_fsr2_dx11.h>

namespace Magpie {

template <typename T>
static T LoadProc(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

FSR2Upscaler::~FSR2Upscaler() { _Reset(); }

void FSR2Upscaler::_Reset() noexcept {
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
	_exposureUav = nullptr;
	_exposure = nullptr;
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
	_lastGuidanceResetFrameId = std::numeric_limits<FrameGuidanceFrameId>::max();
	_enableOpticalFlow = false;
	_motionRequest = {};
}

bool FSR2Upscaler::Initialize(
	DeviceResources& resources, ID3D11Texture2D* input, ID3D11Texture2D* output,
	MotionVectorRequest motionRequest
) noexcept {
	_Reset();
	_motionRequest = motionRequest;
	_enableOpticalFlow = motionRequest.method != OpticalFlowMethod::None;
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
	desc.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE |
		(_hdrProtocol.depthInverted ? FFX_FSR2_ENABLE_DEPTH_INVERTED : 0) |
		(_hdrProtocol.depthInfinite ? FFX_FSR2_ENABLE_DEPTH_INFINITE : 0);
	if (_hdrProtocol.hdrColorInput) desc.flags |= FFX_FSR2_ENABLE_HIGH_DYNAMIC_RANGE;
	ec = reinterpret_cast<decltype(&ffxFsr2ContextCreate)>(_contextCreate)(
		static_cast<FfxFsr2Context*>(_context), &desc);
	if (ec != FFX_OK) { Logger::Get().Error(fmt::format("ffxFsr2ContextCreate failed ({})", (int)ec)); _Reset(); return false; }
	if (_hdrProtocol.hdrColorInput) {
		_exposure = DirectXHelper::CreateTexture2D(_device, DXGI_FORMAT_R32_FLOAT, 1, 1,
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
		if (_exposure) _device->CreateUnorderedAccessView(
			_exposure.get(), nullptr, _exposureUav.put());
	}
	Logger::Get().Info(fmt::format("FSR2 D3D11 initialized (opticalFlow={}, hdr={}, transfer={}, exposure={:.3f}): {}x{} -> {}x{}",
		_enableOpticalFlow, _hdrProtocol.hdrColorInput,
		static_cast<int>(_hdrProtocol.transfer), _hdrProtocol.exposure,
		inDesc.Width, inDesc.Height, outDesc.Width, outDesc.Height));
	return true;
}

bool FSR2Upscaler::Resize(DeviceResources& r, ID3D11Texture2D* i, ID3D11Texture2D* o) noexcept {
	return Initialize(r, i, o, _motionRequest);
}

bool FSR2Upscaler::Draw(const NativeEffectDrawContext& drawContext) noexcept {
	_resetHistory |= drawContext.inputHistoryReset || !drawContext.isNewCaptureFrame;
	ID3D11Texture2D* input = drawContext.input;
	ID3D11Texture2D* output = drawContext.output;
	if (!_context) return false;
	const bool guidanceReset = drawContext.frameGuidance.requiresHistoryReset &&
		_lastGuidanceResetFrameId != drawContext.frameId;
	_resetHistory |= guidanceReset;
	static constexpr float ZERO[4]{};
	static constexpr float REACTIVE_OF[4]{ 0.5f,0.5f,0.5f,0.5f };
	static constexpr float REACTIVE_ZEROMV[4]{ 0.9f,0.9f,0.9f,0.9f };
	_d3dDC->ClearUnorderedAccessViewFloat(_zeroDepthUav.get(), ZERO);
	if (_exposureUav) {
		const float exposure[4]{ _hdrProtocol.exposure, 0, 0, 0 };
		_d3dDC->ClearUnorderedAccessViewFloat(_exposureUav.get(), exposure);
	}
	ID3D11Texture2D* motionVectors = _zeroMotion.get();
	if (_enableOpticalFlow) {
		D3D11_TEXTURE2D_DESC desc{};
		input->GetDesc(&desc);
		if (!drawContext.frameGuidance.IsValidFor(drawContext.frameId, { desc.Width, desc.Height })) return false;
		const auto sync = drawContext.frameGuidance.motion.metadata.sync;
		if (sync.fence && sync.value && FAILED(_d3dDC->Wait(sync.fence, sync.value))) return false;
		motionVectors = drawContext.frameGuidance.motion.texture;
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
	d.exposure = getResource(static_cast<FfxFsr2Context*>(_context), _exposure.get(),
		_exposure ? L"FSR2_Exposure" : L"FSR2_AutoExposure", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.reactive = getResource(static_cast<FfxFsr2Context*>(_context), _reactive.get(), L"FSR2_FullReactive", FFX_RESOURCE_STATE_COMPUTE_READ);
	d.transparencyAndComposition = getResource(static_cast<FfxFsr2Context*>(_context), nullptr, nullptr, FFX_RESOURCE_STATE_COMPUTE_READ);
	d.output = getResource(static_cast<FfxFsr2Context*>(_context), output, L"FSR2_Output", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
	// Shared guidance stores motion in consumer pixel units, so no render-size
	// multiplication is needed. Applying width/height here made OF vectors huge.
	d.motionVectorScale = { 1.0f, 1.0f };
	d.jitterOffset = { 0.0f, 0.0f };
	d.renderSize = { inDesc.Width, inDesc.Height };
	d.enableSharpening = true;
	d.sharpness = 0.2f;
	d.frameTimeDelta = 16.6667f;
	d.preExposure = _hdrProtocol.preExposure;
	d.reset = _resetHistory;
	d.cameraNear = 1.0f;
	d.cameraFar = FLT_MAX;
	d.cameraFovAngleVertical = 1.04719755f;
	d.viewSpaceToMetersFactor = 1.0f;
	const FfxErrorCode ec = reinterpret_cast<decltype(&ffxFsr2ContextDispatch)>(_contextDispatch)(
		static_cast<FfxFsr2Context*>(_context), &d);
	if (ec != FFX_OK) { Logger::Get().Error(fmt::format("ffxFsr2ContextDispatch failed ({})", (int)ec)); return false; }
	_resetHistory = false;
	if (guidanceReset) _lastGuidanceResetFrameId = drawContext.frameId;
	return true;
}

}
#else
namespace Magpie {
FSR2Upscaler::~FSR2Upscaler() = default;
void FSR2Upscaler::_Reset() noexcept {}
bool FSR2Upscaler::Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, MotionVectorRequest) noexcept { return false; }
bool FSR2Upscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept { return false; }
bool FSR2Upscaler::Draw(const NativeEffectDrawContext&) noexcept { return false; }
}
#endif
