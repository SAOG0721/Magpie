#include "pch.h"
#include "XeSSZeroMVUpscaler.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "HalfResOpticalFlow.h"
#include "Logger.h"

#ifdef MP_ENABLE_XESS_ZEROMV
#include "XeSSUpscaleFence.h"
#include <d3d12.h>
#include <xess/xess.h>
#include <xess/xess_d3d12.h>

namespace Magpie {

struct XeSSZeroMVUpscaler::Impl {
	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D11Texture2D> sharedOutput11;
	winrt::com_ptr<ID3D11Texture2D> sharedMotion11;
	winrt::com_ptr<ID3D11ShaderResourceView> inputSrv11;
	winrt::com_ptr<ID3D11UnorderedAccessView> sharedInputUav11;
	winrt::com_ptr<ID3D11ComputeShader> colorConvertShader11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D12Resource> sharedOutput12;
	winrt::com_ptr<ID3D12Resource> sharedMotion12;
	winrt::com_ptr<ID3D12Resource> zeroMotion12;
	winrt::com_ptr<ID3D12Resource> flatDepth12;
	winrt::com_ptr<ID3D12Resource> responsiveMask12;
	winrt::com_ptr<ID3D12DescriptorHeap> descriptorHeap12;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	wil::unique_event_nothrow fenceEvent;
	xess_context_handle_t xessContext = nullptr;
	std::unique_ptr<HalfResOpticalFlow> opticalFlow;
	uint64_t fenceValue = 0;
	uint32_t inputWidth = 0;
	uint32_t inputHeight = 0;
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint64_t lastSubmittedValue = 0;
	bool submitted = false;
	bool convertInputToRgba = false;
	bool enableOpticalFlow = false;
	bool enableJitter = false;
	uint32_t frameIndex = 0;
	bool resetHistory = true;
};

static constexpr char COLOR_CONVERT_HLSL[] = R"(
Texture2D<float4> InputColor : register(t0);
RWTexture2D<float4> OutputColor : register(u0);

[numthreads(8, 8, 1)]
void ConvertToRgba(uint3 tid : SV_DispatchThreadID) {
    uint width, height;
    OutputColor.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    OutputColor[tid.xy] = InputColor.Load(int3(tid.xy, 0));
}
)";

static bool XessSucceeded(xess_result_t result, std::string_view operation) noexcept {
	if (result == XESS_RESULT_SUCCESS) {
		return true;
	}
	Logger::Get().Error(fmt::format("{} failed (XeSS result {})", operation, (int)result));
	return false;
}

static bool WaitForFenceValue(XeSSZeroMVUpscaler::Impl& impl, uint64_t value) noexcept {
	const HRESULT hr = WaitForXeSSUpscaleFence(impl.device12.get(), impl.fence12.get(), impl.fenceEvent.get(), value);
	if (FAILED(hr)) Logger::Get().ComError("Wait for XeSS upscaler fence", hr);
	return SUCCEEDED(hr);
}

static bool WaitForD3D12(XeSSZeroMVUpscaler::Impl& impl) noexcept {
	const uint64_t value = ++impl.fenceValue;
	const HRESULT hr = impl.queue12->Signal(impl.fence12.get(), value);
	if (FAILED(hr)) {
		Logger::Get().ComError("Signal XeSS D3D12 fence failed", hr);
		return false;
	}
	return WaitForFenceValue(impl, value);
}

void XeSSZeroMVUpscaler::ImplDeleter::operator()(Impl* impl) const noexcept {
	if (!impl) return;
	const bool safeToDestroy = !impl->submitted || WaitForD3D12(*impl) ||
		FAILED(impl->device12->GetDeviceRemovedReason());
	if (safeToDestroy && (!impl->xessContext ||
		XessSucceeded(xessDestroyContext(impl->xessContext), "xessDestroyContext"))) {
		delete impl;
		return;
	}
	// If the queue or SDK will not retire, retain its owner and reject a new session.
	xessUpscaleRestartRequired.store(true, std::memory_order_release);
	Logger::Get().Error("XeSS upscaler resources retained after failed teardown; restart Magpie before using XeSS SR");
}

static bool CreateSharedTexture(
	XeSSZeroMVUpscaler::Impl& impl,
	const D3D11_TEXTURE2D_DESC& sourceDesc,
	winrt::com_ptr<ID3D11Texture2D>& texture11,
	winrt::com_ptr<ID3D12Resource>& texture12
) noexcept {
	D3D11_TEXTURE2D_DESC desc = sourceDesc;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = impl.device11->CreateTexture2D(&desc, nullptr, texture11.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS shared D3D11 texture failed", hr);
		return false;
	}

	winrt::com_ptr<IDXGIResource1> dxgiResource;
	hr = texture11->QueryInterface(IID_PPV_ARGS(dxgiResource.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Query XeSS shared IDXGIResource1 failed", hr);
		return false;
	}

	HANDLE rawHandle = nullptr;
	hr = dxgiResource->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawHandle);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS texture shared handle failed", hr);
		return false;
	}
	wil::unique_handle sharedHandle(rawHandle);
	hr = impl.device12->OpenSharedHandle(sharedHandle.get(), IID_PPV_ARGS(texture12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open XeSS texture in D3D12 failed", hr);
		return false;
	}
	return true;
}

static xess_quality_settings_t SelectQuality(float scale) noexcept {
	if (scale <= 1.0f) return XESS_QUALITY_SETTING_AA;
	if (scale <= 1.3f) return XESS_QUALITY_SETTING_ULTRA_QUALITY;
	if (scale <= 1.5f) return XESS_QUALITY_SETTING_QUALITY;
	if (scale <= 1.7f) return XESS_QUALITY_SETTING_BALANCED;
	if (scale <= 2.0f) return XESS_QUALITY_SETTING_PERFORMANCE;
	return XESS_QUALITY_SETTING_ULTRA_PERFORMANCE;
}

XeSSZeroMVUpscaler::XeSSZeroMVUpscaler() = default;

XeSSZeroMVUpscaler::~XeSSZeroMVUpscaler() = default;

bool XeSSZeroMVUpscaler::Initialize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output,
	bool enableOpticalFlow,
	bool enableJitter
) noexcept {
	_enableOpticalFlow = enableOpticalFlow;
	_enableJitter = enableJitter;
	_impl.reset();
	if (xessUpscaleRestartRequired.load(std::memory_order_acquire)) {
		Logger::Get().Error("Restart Magpie before creating another XeSS upscaler");
		return false;
	}
	std::unique_ptr<Impl, ImplDeleter> impl(new Impl);
	impl->enableOpticalFlow = enableOpticalFlow;
	impl->enableJitter = enableJitter;
	impl->device11 = deviceResources.GetD3DDevice();
	impl->context11 = deviceResources.GetD3DDC();

	D3D11_TEXTURE2D_DESC inputDesc{};
	D3D11_TEXTURE2D_DESC outputDesc{};
	input->GetDesc(&inputDesc);
	output->GetDesc(&outputDesc);
	if (inputDesc.Width > outputDesc.Width || inputDesc.Height > outputDesc.Height) {
		Logger::Get().Error(fmt::format(
			"XeSS Zero-MV only supports upscaling: {}x{} -> {}x{}",
			inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
		return false;
	}
	const bool supportedInputFormat = inputDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
		inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;
	if (!supportedInputFormat || outputDesc.Format != DXGI_FORMAT_R8G8B8A8_UNORM) {
		Logger::Get().Error(fmt::format(
			"XeSS Zero-MV unsupported texture formats: input={}, output={}",
			(uint32_t)inputDesc.Format, (uint32_t)outputDesc.Format));
		return false;
	}
	const float scaleX = (float)outputDesc.Width / inputDesc.Width;
	const float scaleY = (float)outputDesc.Height / inputDesc.Height;
	const float scale = (std::max)(scaleX, scaleY);
	if (scale > 3.0f) {
		Logger::Get().Error("XeSS Zero-MV supports up to a 3x scale");
		return false;
	}
	impl->inputWidth = inputDesc.Width;
	impl->inputHeight = inputDesc.Height;
	impl->outputWidth = outputDesc.Width;
	impl->outputHeight = outputDesc.Height;
	impl->convertInputToRgba = inputDesc.Format == DXGI_FORMAT_B8G8R8A8_UNORM;

	HRESULT hr = D3D12CreateDevice(deviceResources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(impl->device12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS D3D12 device failed", hr);
		return false;
	}
	D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_4 };
	hr = impl->device12->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel));
	if (FAILED(hr) || shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_4) {
		Logger::Get().Error("XeSS cross-vendor path requires Shader Model 6.4 / DP4a support");
		return false;
	}

	D3D12_COMMAND_QUEUE_DESC queueDesc{};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	hr = impl->device12->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(impl->queue12.put()));
	if (SUCCEEDED(hr)) hr = impl->device12->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(impl->allocator12.put()));
	if (SUCCEEDED(hr)) hr = impl->device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		impl->allocator12.get(), nullptr, IID_PPV_ARGS(impl->commandList12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS D3D12 command objects failed", hr);
		return false;
	}

	D3D11_TEXTURE2D_DESC xessInputDesc = inputDesc;
	xessInputDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	xessInputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	if (!CreateSharedTexture(*impl, xessInputDesc, impl->sharedInput11, impl->sharedInput12) ||
		!CreateSharedTexture(*impl, outputDesc, impl->sharedOutput11, impl->sharedOutput12)) {
		return false;
	}
	if (impl->convertInputToRgba) {
		hr = impl->device11->CreateShaderResourceView(input, nullptr, impl->inputSrv11.put());
		if (SUCCEEDED(hr)) hr = impl->device11->CreateUnorderedAccessView(
			impl->sharedInput11.get(), nullptr, impl->sharedInputUav11.put());
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (SUCCEEDED(hr) && !DirectXHelper::CompileComputeShader(
			COLOR_CONVERT_HLSL, "ConvertToRgba", shaderBlob.put(), "XeSSColorConvert")) {
			hr = E_FAIL;
		}
		if (SUCCEEDED(hr)) hr = impl->device11->CreateComputeShader(
			shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
			impl->colorConvertShader11.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create XeSS BGRA-to-RGBA conversion resources failed", hr);
			return false;
		}
	}

	D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
	heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	heapDesc.NumDescriptors = 3;
	heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	hr = impl->device12->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(impl->descriptorHeap12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS descriptor heap failed", hr);
		return false;
	}

	D3D12_HEAP_PROPERTIES heapProperties{};
	heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
	if (impl->enableOpticalFlow) {
		impl->opticalFlow = std::make_unique<HalfResOpticalFlow>();
		if (!impl->opticalFlow->Initialize(impl->device11, impl->context11, input)) {
			Logger::Get().Error("Initialize XeSS 50% optical flow failed");
			return false;
		}
		D3D11_TEXTURE2D_DESC motionDesc11{};
		motionDesc11.Width = inputDesc.Width;
		motionDesc11.Height = inputDesc.Height;
		motionDesc11.MipLevels = 1;
		motionDesc11.ArraySize = 1;
		motionDesc11.Format = DXGI_FORMAT_R16G16_FLOAT;
		motionDesc11.SampleDesc.Count = 1;
		motionDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		if (!CreateSharedTexture(*impl, motionDesc11, impl->sharedMotion11, impl->sharedMotion12)) {
			return false;
		}
	} else {
		D3D12_RESOURCE_DESC motionDesc{};
		motionDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		motionDesc.Width = outputDesc.Width;
		motionDesc.Height = outputDesc.Height;
		motionDesc.DepthOrArraySize = 1;
		motionDesc.MipLevels = 1;
		motionDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		motionDesc.SampleDesc.Count = 1;
		motionDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		motionDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		hr = impl->device12->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &motionDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(impl->zeroMotion12.put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Create XeSS zero motion-vector texture failed", hr);
			return false;
		}
	}

	// Supply a real (but constant) depth resource in both modes. XeSS can omit
	// depth with high-resolution motion vectors, but a non-null flat texture
	// keeps the colour-only Zero-MV experiment explicit and comparable with the
	// DLSS/FSR2 adapters.
	D3D12_RESOURCE_DESC depthDesc{};
	depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthDesc.Width = inputDesc.Width;
	depthDesc.Height = inputDesc.Height;
	depthDesc.DepthOrArraySize = 1;
	depthDesc.MipLevels = 1;
	depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthDesc.SampleDesc.Count = 1;
	depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	hr = impl->device12->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &depthDesc,
		D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(impl->flatDepth12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS flat depth texture failed", hr);
		return false;
	}
	D3D12_RESOURCE_DESC responsiveMaskDesc = depthDesc;
	responsiveMaskDesc.Format = DXGI_FORMAT_R8_UNORM;
	hr = impl->device12->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE,
		&responsiveMaskDesc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
		IID_PPV_ARGS(impl->responsiveMask12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS responsive pixel mask failed", hr);
		return false;
	}

	const UINT descriptorSize = impl->device12->GetDescriptorHandleIncrementSize(
		D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
	const D3D12_CPU_DESCRIPTOR_HANDLE depthCpu = impl->descriptorHeap12->GetCPUDescriptorHandleForHeapStart();
	const D3D12_GPU_DESCRIPTOR_HANDLE depthGpu = impl->descriptorHeap12->GetGPUDescriptorHandleForHeapStart();
	D3D12_UNORDERED_ACCESS_VIEW_DESC depthUavDesc{};
	depthUavDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl->device12->CreateUnorderedAccessView(impl->flatDepth12.get(), nullptr, &depthUavDesc, depthCpu);
	ID3D12DescriptorHeap* heaps[] = { impl->descriptorHeap12.get() };
	impl->commandList12->SetDescriptorHeaps(1, heaps);
	const float flatDepthValue[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
	impl->commandList12->ClearUnorderedAccessViewFloat(
		depthGpu, depthCpu, impl->flatDepth12.get(), flatDepthValue, 0, nullptr);
	D3D12_CPU_DESCRIPTOR_HANDLE responsiveCpu = depthCpu;
	responsiveCpu.ptr += descriptorSize;
	D3D12_GPU_DESCRIPTOR_HANDLE responsiveGpu = depthGpu;
	responsiveGpu.ptr += descriptorSize;
	D3D12_UNORDERED_ACCESS_VIEW_DESC responsiveUavDesc{};
	responsiveUavDesc.Format = DXGI_FORMAT_R8_UNORM;
	responsiveUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
	impl->device12->CreateUnorderedAccessView(
		impl->responsiveMask12.get(), nullptr, &responsiveUavDesc, responsiveCpu);
	const float responsiveValue[4]{ 0.5f, 0.5f, 0.5f, 0.5f };
	impl->commandList12->ClearUnorderedAccessViewFloat(
		responsiveGpu, responsiveCpu, impl->responsiveMask12.get(), responsiveValue, 0, nullptr);

	D3D12_RESOURCE_BARRIER auxiliaryBarriers[3]{};
	auxiliaryBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	auxiliaryBarriers[0].Transition.pResource = impl->flatDepth12.get();
	auxiliaryBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	auxiliaryBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	auxiliaryBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	auxiliaryBarriers[1] = auxiliaryBarriers[0];
	auxiliaryBarriers[1].Transition.pResource = impl->responsiveMask12.get();
	UINT auxiliaryBarrierCount = 2;
	if (!impl->enableOpticalFlow) {
		D3D12_CPU_DESCRIPTOR_HANDLE motionCpu = depthCpu;
		motionCpu.ptr += descriptorSize * 2;
		D3D12_GPU_DESCRIPTOR_HANDLE motionGpu = depthGpu;
		motionGpu.ptr += descriptorSize * 2;
		D3D12_UNORDERED_ACCESS_VIEW_DESC motionUavDesc{};
		motionUavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		motionUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		impl->device12->CreateUnorderedAccessView(
			impl->zeroMotion12.get(), nullptr, &motionUavDesc, motionCpu);
		const float zeroMotionValue[4]{};
		impl->commandList12->ClearUnorderedAccessViewFloat(
			motionGpu, motionCpu, impl->zeroMotion12.get(), zeroMotionValue, 0, nullptr);
		auxiliaryBarriers[2] = auxiliaryBarriers[0];
		auxiliaryBarriers[2].Transition.pResource = impl->zeroMotion12.get();
		auxiliaryBarrierCount = 3;
	}
	impl->commandList12->ResourceBarrier(auxiliaryBarrierCount, auxiliaryBarriers);
	hr = impl->commandList12->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("Close XeSS initialization command list failed", hr);
		return false;
	}
	hr = impl->device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(impl->fence11.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS shared fence failed", hr);
		return false;
	}
	HANDLE rawFenceHandle = nullptr;
	hr = impl->fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &rawFenceHandle);
	if (FAILED(hr)) {
		Logger::Get().ComError("Create XeSS fence shared handle failed", hr);
		return false;
	}
	wil::unique_handle fenceHandle(rawFenceHandle);
	hr = impl->device12->OpenSharedHandle(fenceHandle.get(), IID_PPV_ARGS(impl->fence12.put()));
	if (FAILED(hr)) {
		Logger::Get().ComError("Open XeSS fence in D3D12 failed", hr);
		return false;
	}
	if (FAILED(impl->fenceEvent.create())) {
		Logger::Get().Error("Create XeSS fence event failed");
		return false;
	}

	ID3D12CommandList* commandLists[] = { impl->commandList12.get() };
	impl->queue12->ExecuteCommandLists(1, commandLists);
	impl->submitted = true;
	if (!WaitForD3D12(*impl)) return false;

	const xess_result_t createResult = xessD3D12CreateContext(impl->device12.get(), &impl->xessContext);
	if (!XessSucceeded(createResult, "xessD3D12CreateContext")) return false;
	const xess_quality_settings_t quality = SelectQuality(scale);
	xess_d3d12_init_params_t initParams{};
	initParams.outputResolution = { outputDesc.Width, outputDesc.Height };
	initParams.qualitySetting = quality;
	initParams.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR | XESS_INIT_FLAG_RESPONSIVE_PIXEL_MASK;
	if (!impl->enableOpticalFlow) initParams.initFlags |= XESS_INIT_FLAG_HIGH_RES_MV;
	if (!XessSucceeded(xessD3D12Init(impl->xessContext, &initParams), "xessD3D12Init") ||
		!XessSucceeded(xessSetVelocityScale(impl->xessContext, 1.0f, 1.0f), "xessSetVelocityScale")) {
		return false;
	}

	Logger::Get().Info(fmt::format(
		"XeSS experimental D3D11/D3D12 backend initialized (quality {}, {}, jitter={}, BGRA conversion={}): {}x{} -> {}x{}",
		(int)quality, impl->enableOpticalFlow ? "OpticalFlow50" : "Zero-MV", impl->enableJitter,
		impl->convertInputToRgba,
		inputDesc.Width, inputDesc.Height, outputDesc.Width, outputDesc.Height));
	_impl = std::move(impl);
	return true;
}

bool XeSSZeroMVUpscaler::Resize(
	DeviceResources& deviceResources,
	ID3D11Texture2D* input,
	ID3D11Texture2D* output
) noexcept {
	return Initialize(deviceResources, input, output, _enableOpticalFlow, _enableJitter);
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

bool XeSSZeroMVUpscaler::Draw(const NativeEffectDrawContext& drawContext) noexcept {
	ID3D11Texture2D* input = drawContext.input;
	ID3D11Texture2D* output = drawContext.output;
	if (!_impl || !_impl->xessContext) return false;
	Impl& impl = *_impl;
	const bool resetInput = drawContext.inputHistoryReset || !drawContext.isNewCaptureFrame;
	impl.resetHistory |= resetInput;
	if (resetInput && impl.opticalFlow) impl.opticalFlow->ResetHistory();
	// A command allocator cannot be reset while its previous D3D12 submission
	// is still executing. Usually the D3D11 consumer has already waited for it.
	if (!WaitForFenceValue(impl, impl.lastSubmittedValue)) return false;
	if (impl.convertInputToRgba) {
		ID3D11ShaderResourceView* inputSrv = impl.inputSrv11.get();
		ID3D11UnorderedAccessView* outputUav = impl.sharedInputUav11.get();
		impl.context11->CSSetShader(impl.colorConvertShader11.get(), nullptr, 0);
		impl.context11->CSSetShaderResources(0, 1, &inputSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &outputUav, nullptr);
		impl.context11->Dispatch((impl.inputWidth + 7) / 8, (impl.inputHeight + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		impl.context11->CSSetShaderResources(0, 1, &nullSrv);
		impl.context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		impl.context11->CSSetShader(nullptr, nullptr, 0);
	} else {
		impl.context11->CopyResource(impl.sharedInput11.get(), input);
	}
	if (impl.enableOpticalFlow) {
		if (!impl.opticalFlow->Estimate(input)) {
			Logger::Get().Error("Estimate XeSS 50% optical flow failed");
			return false;
		}
		impl.context11->CopyResource(impl.sharedMotion11.get(), impl.opticalFlow->GetMotionTexture());
	}
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	if (FAILED(hr)) {
		Logger::Get().ComError("Signal XeSS input-ready fence failed", hr);
		return false;
	}
	impl.context11->Flush();
	hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (FAILED(hr)) {
		Logger::Get().ComError("Wait for XeSS D3D11 input failed", hr);
		return false;
	}

	hr = impl.allocator12->Reset();
	if (SUCCEEDED(hr)) hr = impl.commandList12->Reset(impl.allocator12.get(), nullptr);
	if (FAILED(hr)) {
		Logger::Get().ComError("Reset XeSS command list failed", hr);
		return false;
	}
	D3D12_RESOURCE_BARRIER barriers[3]{};
	barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[0].Transition.pResource = impl.sharedInput12.get();
	barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barriers[1].Transition.pResource = impl.sharedOutput12.get();
	barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
	barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	const UINT barrierCount = impl.enableOpticalFlow ? 3 : 2;
	if (impl.enableOpticalFlow) {
		barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barriers[2].Transition.pResource = impl.sharedMotion12.get();
		barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
		barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	}
	impl.commandList12->ResourceBarrier(barrierCount, barriers);

	xess_d3d12_execute_params_t params{};
	params.pColorTexture = impl.sharedInput12.get();
	params.pVelocityTexture = impl.enableOpticalFlow ? impl.sharedMotion12.get() : impl.zeroMotion12.get();
	params.pDepthTexture = impl.flatDepth12.get();
	params.pResponsivePixelMaskTexture = impl.responsiveMask12.get();
	params.pOutputTexture = impl.sharedOutput12.get();
	if (impl.enableJitter) {
		// Metadata-only jitter: the source application's projection is unchanged.
		const uint32_t phase = drawContext.isNewCaptureFrame ? impl.frameIndex++ : (impl.frameIndex ? impl.frameIndex - 1 : 0);
		const uint32_t sample = (phase & 7u) + 1u;
		params.jitterOffsetX = Halton(sample, 2) - 0.5f;
		params.jitterOffsetY = Halton(sample, 3) - 0.5f;
	} else {
		params.jitterOffsetX = 0.0f;
		params.jitterOffsetY = 0.0f;
	}
	params.exposureScale = 1.0f;
	params.resetHistory = impl.resetHistory ? 1u : 0u;
	params.inputWidth = impl.inputWidth;
	params.inputHeight = impl.inputHeight;
	const xess_result_t result = xessD3D12Execute(impl.xessContext, impl.commandList12.get(), &params);
	if (!XessSucceeded(result, "xessD3D12Execute")) return false;
	std::swap(barriers[0].Transition.StateBefore, barriers[0].Transition.StateAfter);
	std::swap(barriers[1].Transition.StateBefore, barriers[1].Transition.StateAfter);
	if (impl.enableOpticalFlow) {
		std::swap(barriers[2].Transition.StateBefore, barriers[2].Transition.StateAfter);
	}
	impl.commandList12->ResourceBarrier(barrierCount, barriers);
	hr = impl.commandList12->Close();
	if (FAILED(hr)) {
		Logger::Get().ComError("Close XeSS command list failed", hr);
		return false;
	}
	ID3D12CommandList* lists[] = { impl.commandList12.get() };
	impl.queue12->ExecuteCommandLists(1, lists);
	const uint64_t outputReady = ++impl.fenceValue;
	hr = impl.queue12->Signal(impl.fence12.get(), outputReady);
	impl.lastSubmittedValue = outputReady;
	if (SUCCEEDED(hr)) hr = impl.context11->Wait(impl.fence11.get(), outputReady);
	if (FAILED(hr)) {
		Logger::Get().ComError("Synchronize XeSS output failed", hr);
		return false;
	}
	impl.context11->CopyResource(output, impl.sharedOutput11.get());
	impl.resetHistory = false;
	return true;
}

}

#else

namespace Magpie {

struct XeSSZeroMVUpscaler::Impl {};
void XeSSZeroMVUpscaler::ImplDeleter::operator()(Impl* impl) const noexcept { delete impl; }
XeSSZeroMVUpscaler::XeSSZeroMVUpscaler() = default;
XeSSZeroMVUpscaler::~XeSSZeroMVUpscaler() = default;
bool XeSSZeroMVUpscaler::Initialize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*, bool, bool) noexcept {
	Logger::Get().Error("XeSS Zero-MV support is not enabled in this build");
	return false;
}
bool XeSSZeroMVUpscaler::Resize(DeviceResources&, ID3D11Texture2D*, ID3D11Texture2D*) noexcept { return false; }
bool XeSSZeroMVUpscaler::Draw(const NativeEffectDrawContext&) noexcept { return false; }

}

#endif
