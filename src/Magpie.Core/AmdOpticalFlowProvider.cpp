#include "pch.h"
#include "AmdOpticalFlowProvider.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

#ifdef MP_ENABLE_AMD_OPTICAL_FLOW
#include <d3d12.h>
#include <ffx_dx12.h>
#include <ffx_opticalflow.h>

namespace Magpie {

namespace {

constexpr char PREPARE_INPUT_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
cbuffer Params : register(b0) { uint2 SourceExtent; uint2 TargetExtent; };
[numthreads(8, 8, 1)]
void Prepare(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    uint2 begin = tid.xy * SourceExtent / TargetExtent;
    uint2 end = max(begin + 1, (tid.xy + 1) * SourceExtent / TargetExtent);
    end = min(end, SourceExtent);
    float4 value = 0;
    uint count = 0;
    for (uint y = begin.y; y < end.y; ++y) {
        for (uint x = begin.x; x < end.x; ++x) {
            value += Source.Load(int3(uint2(x, y), 0));
            ++count;
        }
    }
    Target[tid.xy] = value / max(count, 1);
}
)";

constexpr char DENSIFY_HLSL[] = R"(
Texture2D<int2> SparseFlow : register(t0);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);
cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 OpticalFlowExtent;
    uint2 SparseExtent;
    float2 VectorScale;
};
int2 LoadFlow(int2 p) {
    p = clamp(p, int2(0, 0), int2(SparseExtent) - 1);
    return SparseFlow.Load(int3(p, 0));
}
[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;
    float2 opticalPixel = (float2(tid.xy) + 0.5) *
        (float2(OpticalFlowExtent) / float2(SourceExtent));
    float2 sparsePos = opticalPixel / 8.0 - 0.5;
    int2 p0 = int2(floor(sparsePos));
    float2 f = frac(sparsePos);
    float2 a = float2(LoadFlow(p0));
    float2 b = float2(LoadFlow(p0 + int2(1, 0)));
    float2 c = float2(LoadFlow(p0 + int2(0, 1)));
    float2 d = float2(LoadFlow(p0 + int2(1, 1)));
    DenseMotion[tid.xy] = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y) *
        VectorScale;
    // FidelityFX OF does not expose a confidence surface. Preserve an honest,
    // conservative baseline; reset/first frames are cleared to zero instead.
    DenseConfidence[tid.xy] = 0.65;
}
)";

FrameGuidanceMetadata MakeMetadata(
	const FrameGuidanceFrame& frame,
	FrameGuidanceResetReason reason,
	bool isZero
) noexcept {
	return {
		.frameId = frame.frameId,
		.captureSequence = frame.captureSequence,
		.resourceGeneration = frame.resourceGeneration,
		.timestamp100ns = frame.timestamp100ns,
		.sourceExtent = frame.sourceExtent,
		.validRegion = frame.validRegion,
		.resetReason = reason,
		.valid = true,
		.isZero = isZero,
		.requiresHistoryReset = reason != FrameGuidanceResetReason::None
	};
}

bool WaitForFence(ID3D12Fence* fence, uint64_t value) noexcept {
	if (!value || fence->GetCompletedValue() >= value) return true;
	wil::unique_event_nothrow event;
	if (FAILED(event.create()) || FAILED(
		fence->SetEventOnCompletion(value, event.get()))) return false;
	return WaitForSingleObject(event.get(), 5000) == WAIT_OBJECT_0;
}

}

struct AmdOpticalFlowProvider::Impl {
	~Impl() { Destroy(); }

	void Destroy() noexcept {
		if (queue12 && fence12 && lastSubmittedValue) {
			WaitForFence(fence12.get(), lastSubmittedValue);
		}
		if (contextCreated) {
			ffxOpticalflowContextDestroy(&opticalFlowContext);
			contextCreated = false;
		}
		opticalFlowContext = {};
		backendScratch.clear();
		sharedInput11 = nullptr;
		sharedInput12 = nullptr;
		sparseFlow11 = nullptr;
		sparseFlow12 = nullptr;
		scd12 = nullptr;
		denseMotion = nullptr;
		denseConfidence = nullptr;
		inputSrv = nullptr;
		sharedInputUav = nullptr;
		sparseFlowSrv = nullptr;
		denseMotionUav = nullptr;
		denseConfidenceUav = nullptr;
		prepareShader = nullptr;
		densifyShader = nullptr;
		prepareParams = nullptr;
		densifyParams = nullptr;
		commandList12 = nullptr;
		allocator12 = nullptr;
		queue12 = nullptr;
		fence12 = nullptr;
		fence11 = nullptr;
		device12 = nullptr;
		device11 = nullptr;
		context11 = nullptr;
		extent = {};
		opticalFlowExtent = {};
		sparseExtent = {};
		lastSubmittedValue = 0;
		fenceValue = 0;
		historyValid = false;
	}

	bool CreateSharedTexture(
		DXGI_FORMAT format,
		uint32_t width,
		uint32_t height,
		UINT bindFlags,
		winrt::com_ptr<ID3D11Texture2D>& texture11,
		winrt::com_ptr<ID3D12Resource>& texture12
	) noexcept {
		D3D11_TEXTURE2D_DESC desc{
			.Width = width,
			.Height = height,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = bindFlags,
			.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
		};
		HRESULT hr = device11->CreateTexture2D(&desc, nullptr, texture11.put());
		winrt::com_ptr<IDXGIResource1> dxgiResource;
		if (SUCCEEDED(hr)) hr = texture11->QueryInterface(
			IID_PPV_ARGS(dxgiResource.put()));
		HANDLE rawHandle = nullptr;
		if (SUCCEEDED(hr)) hr = dxgiResource->CreateSharedHandle(
			nullptr, GENERIC_ALL, nullptr, &rawHandle);
		wil::unique_handle handle(rawHandle);
		if (SUCCEEDED(hr)) hr = device12->OpenSharedHandle(
			handle.get(), IID_PPV_ARGS(texture12.put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Create AMD OF shared texture failed", hr);
			return false;
		}
		return true;
	}

	bool CreateD3D12Texture(
		const FfxApiResourceDescription& source,
		winrt::com_ptr<ID3D12Resource>& resource
	) noexcept {
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = source.width;
		desc.Height = source.height;
		desc.DepthOrArraySize = static_cast<UINT16>(std::max(source.depth, 1u));
		desc.MipLevels = static_cast<UINT16>(std::max(source.mipCount, 1u));
		desc.Format = ffxGetDX12FormatFromSurfaceFormat(
			static_cast<FfxApiSurfaceFormat>(source.format));
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		D3D12_HEAP_PROPERTIES heap{ .Type = D3D12_HEAP_TYPE_DEFAULT };
		const HRESULT hr = device12->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(resource.put()));
		if (FAILED(hr)) Logger::Get().ComError("Create AMD OF D3D12 texture failed", hr);
		return SUCCEEDED(hr);
	}

	bool CheckCapabilities() noexcept {
		D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{
			.HighestShaderModel = D3D_SHADER_MODEL_6_2
		};
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1{};
		D3D12_FEATURE_DATA_FORMAT_SUPPORT format{
			.Format = DXGI_FORMAT_R16G16_SINT
		};
		return SUCCEEDED(device12->CheckFeatureSupport(
			D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) &&
			shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_2 &&
			SUCCEEDED(device12->CheckFeatureSupport(
				D3D12_FEATURE_D3D12_OPTIONS1, &options1, sizeof(options1))) &&
			options1.WaveOps &&
			SUCCEEDED(device12->CheckFeatureSupport(
				D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof(format))) &&
			(format.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0;
	}

	bool CreateShaders() noexcept {
		winrt::com_ptr<ID3DBlob> blob;
		if (!DirectXHelper::CompileComputeShader(
			PREPARE_INPUT_HLSL, "Prepare", blob.put(),
			"FrameGuidance/AMD_OF_Prepare.hlsl")) return false;
		HRESULT hr = device11->CreateComputeShader(blob->GetBufferPointer(),
			blob->GetBufferSize(), nullptr, prepareShader.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create AMD OF prepare shader failed", hr);
			return false;
		}
		blob = nullptr;
		if (!DirectXHelper::CompileComputeShader(
			DENSIFY_HLSL, "Densify", blob.put(),
			"FrameGuidance/AMD_OF_Densify.hlsl")) return false;
		hr = device11->CreateComputeShader(blob->GetBufferPointer(),
			blob->GetBufferSize(), nullptr, densifyShader.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create AMD OF densify shader failed", hr);
			return false;
		}
		const D3D11_BUFFER_DESC prepareDesc{
			.ByteWidth = 16, .Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		const D3D11_BUFFER_DESC densifyDesc{
			.ByteWidth = 32, .Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		hr = device11->CreateBuffer(&prepareDesc, nullptr, prepareParams.put());
		if (SUCCEEDED(hr)) hr = device11->CreateBuffer(&densifyDesc, nullptr, densifyParams.put());
		if (FAILED(hr)) Logger::Get().ComError("Create AMD OF shader constant buffers failed", hr);
		return SUCCEEDED(hr);
	}

	bool Create(
		DeviceResources& resources,
		FrameGuidanceExtent sourceExtent,
		AmdOpticalFlowMode requestedMode
	) noexcept {
		Destroy();
		initializationError = OpticalFlowInitializationError::CapabilityUnsupported;
		owner = &resources;
		mode = requestedMode;
		device11 = resources.GetD3DDevice();
		context11 = resources.GetD3DDC();
		extent = sourceExtent;
		Logger::Get().Info(fmt::format("Initialize AMD OF: mode={} source={}x{}",
			static_cast<uint32_t>(mode), extent.width, extent.height));
		opticalFlowExtent = mode == AmdOpticalFlowMode::Performance ?
			FrameGuidanceExtent{ (extent.width + 1) / 2, (extent.height + 1) / 2 } :
			extent;
		if (!device11 || !context11 || !extent.IsValid() || FAILED(
			D3D12CreateDevice(resources.GetGraphicsAdapter(), D3D_FEATURE_LEVEL_12_0,
				IID_PPV_ARGS(device12.put()))) || !CheckCapabilities()) {
			Logger::Get().Warn(
				"AMD OF unavailable: the selected adapter lacks D3D12 SM 6.2, "
				"wave operations, msad4, or R16G16_SINT UAV support");
			return false;
		}
		initializationError = OpticalFlowInitializationError::InteropFailed;

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		HRESULT hr = device12->CreateCommandQueue(
			&queueDesc, IID_PPV_ARGS(queue12.put()));
		if (SUCCEEDED(hr)) hr = device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator12.put()));
		if (SUCCEEDED(hr)) hr = device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator12.get(), nullptr,
			IID_PPV_ARGS(commandList12.put()));
		if (SUCCEEDED(hr)) hr = commandList12->Close();
		if (SUCCEEDED(hr)) hr = device11->CreateFence(
			0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(fence11.put()));
		HANDLE rawFence = nullptr;
		if (SUCCEEDED(hr)) hr = fence11->CreateSharedHandle(
			nullptr, GENERIC_ALL, nullptr, &rawFence);
		wil::unique_handle fenceHandle(rawFence);
		if (SUCCEEDED(hr)) hr = device12->OpenSharedHandle(
			fenceHandle.get(), IID_PPV_ARGS(fence12.put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Create AMD OF D3D12 queue/fence failed", hr);
			return false;
		}

		const size_t scratchSize = ffxGetScratchMemorySizeDX12(
			FFX_OPTICALFLOW_CONTEXT_COUNT);
		backendScratch.resize(scratchSize);
		FfxOpticalflowContextDescription contextDesc{
			.flags = 0,
			.resolution = { opticalFlowExtent.width, opticalFlowExtent.height }
		};
		FfxErrorCode ffxResult = ffxGetInterfaceDX12(
			&contextDesc.backendInterface, ffxGetDeviceDX12(device12.get()),
			backendScratch.data(), backendScratch.size(),
			FFX_OPTICALFLOW_CONTEXT_COUNT);
		if (ffxResult != FFX_OK) {
			Logger::Get().Error(fmt::format("AMD OF ffxGetInterfaceDX12 failed ({})",
				static_cast<int32_t>(ffxResult)));
			return false;
		}
		ffxResult = ffxOpticalflowContextCreate(&opticalFlowContext, &contextDesc);
		if (ffxResult != FFX_OK) {
			Logger::Get().Error(fmt::format("AMD OF ffxOpticalflowContextCreate failed ({})",
				static_cast<int32_t>(ffxResult)));
			return false;
		}
		contextCreated = true;

		FfxOpticalflowSharedResourceDescriptions shared{};
		ffxResult = ffxOpticalflowGetSharedResourceDescriptions(&opticalFlowContext, &shared);
		if (ffxResult != FFX_OK) {
			Logger::Get().Error(fmt::format("AMD OF query shared resources failed ({})",
				static_cast<int32_t>(ffxResult)));
			return false;
		}
		const auto& vectorDesc = shared.opticalFlowVector.resourceDescription;
		sparseExtent = { vectorDesc.width, vectorDesc.height };
		if (vectorDesc.format != FFX_API_SURFACE_FORMAT_R16G16_SINT) {
			Logger::Get().Error(fmt::format("AMD OF unexpected motion format {}",
				static_cast<uint32_t>(vectorDesc.format)));
			return false;
		}
		if (!CreateSharedTexture(DXGI_FORMAT_R8G8B8A8_UNORM,
				opticalFlowExtent.width, opticalFlowExtent.height,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				sharedInput11, sharedInput12) ||
			!CreateSharedTexture(DXGI_FORMAT_R16G16_SINT,
				vectorDesc.width, vectorDesc.height,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
				sparseFlow11, sparseFlow12) ||
			!CreateD3D12Texture(
				shared.opticalFlowSCD.resourceDescription, scd12)) return false;

		const UINT guideBind =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		const UINT guideMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		denseMotion = DirectXHelper::CreateTexture2D(
			device11, DXGI_FORMAT_R16G16_FLOAT, extent.width, extent.height,
			guideBind, D3D11_USAGE_DEFAULT, guideMisc);
		denseConfidence = DirectXHelper::CreateTexture2D(
			device11, DXGI_FORMAT_R8_UNORM, extent.width, extent.height,
			guideBind, D3D11_USAGE_DEFAULT, guideMisc);
		if (!denseMotion || !denseConfidence) {
			Logger::Get().Error("Create AMD OF dense output textures failed");
			return false;
		}
		hr = device11->CreateUnorderedAccessView(sharedInput11.get(), nullptr, sharedInputUav.put());
		if (SUCCEEDED(hr)) hr = device11->CreateShaderResourceView(sparseFlow11.get(), nullptr, sparseFlowSrv.put());
		if (SUCCEEDED(hr)) hr = device11->CreateUnorderedAccessView(denseMotion.get(), nullptr, denseMotionUav.put());
		if (SUCCEEDED(hr)) hr = device11->CreateUnorderedAccessView(denseConfidence.get(), nullptr, denseConfidenceUav.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("Create AMD OF input/output resource views failed", hr);
			return false;
		}
		if (!CreateShaders()) return false;

		resetReason = FrameGuidanceResetReason::Initialize;
		historyValid = false;
		Logger::Get().Info(fmt::format(
			"Frame Guidance AMD OF initialized: mode={} input={}x{} source={}x{} "
			"sparse={}x{} block=8x8 stages=7 vendorIndependent=true "
			"convention=current-to-previous/source-pixels",
			mode == AmdOpticalFlowMode::Performance ? "Performance-50%" :
				"Quality-100%",
			opticalFlowExtent.width, opticalFlowExtent.height,
			extent.width, extent.height, sparseExtent.width, sparseExtent.height));
		initializationError = OpticalFlowInitializationError::None;
		return true;
	}

	bool PrepareInput(ID3D11Texture2D* source) noexcept {
		if (inputTexture != source || !inputSrv) {
			inputTexture = source;
			inputSrv = nullptr;
			if (FAILED(device11->CreateShaderResourceView(
				source, nullptr, inputSrv.put()))) return false;
		}
		struct Params { uint32_t sw, sh, tw, th; };
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context11->Map(
			prepareParams.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		*static_cast<Params*>(mapped.pData) = {
			extent.width, extent.height,
			opticalFlowExtent.width, opticalFlowExtent.height
		};
		context11->Unmap(prepareParams.get(), 0);
		ID3D11ShaderResourceView* srv = inputSrv.get();
		ID3D11UnorderedAccessView* uav = sharedInputUav.get();
		ID3D11Buffer* cb = prepareParams.get();
		context11->CSSetShader(prepareShader.get(), nullptr, 0);
		context11->CSSetShaderResources(0, 1, &srv);
		context11->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context11->CSSetConstantBuffers(0, 1, &cb);
		context11->Dispatch(
			(opticalFlowExtent.width + 7) / 8,
			(opticalFlowExtent.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		context11->CSSetShaderResources(0, 1, &nullSrv);
		context11->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context11->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	bool Densify() noexcept {
		struct Params {
			uint32_t sourceWidth, sourceHeight;
			uint32_t ofWidth, ofHeight;
			uint32_t sparseWidth, sparseHeight;
			float scaleX, scaleY;
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context11->Map(
			densifyParams.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		*static_cast<Params*>(mapped.pData) = {
			extent.width, extent.height,
			opticalFlowExtent.width, opticalFlowExtent.height,
			sparseExtent.width, sparseExtent.height,
			float(extent.width) / float(opticalFlowExtent.width),
			float(extent.height) / float(opticalFlowExtent.height)
		};
		context11->Unmap(densifyParams.get(), 0);
		ID3D11ShaderResourceView* srv = sparseFlowSrv.get();
		ID3D11UnorderedAccessView* uavs[]{
			denseMotionUav.get(), denseConfidenceUav.get()
		};
		ID3D11Buffer* cb = densifyParams.get();
		context11->CSSetShader(densifyShader.get(), nullptr, 0);
		context11->CSSetShaderResources(0, 1, &srv);
		context11->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context11->CSSetConstantBuffers(0, 1, &cb);
		context11->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUavs[2]{};
		context11->CSSetShaderResources(0, 1, &nullSrv);
		context11->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
		context11->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	DeviceResources* owner = nullptr;
	ID3D11Device5* device11 = nullptr;
	ID3D11DeviceContext4* context11 = nullptr;
	winrt::com_ptr<ID3D12Device> device12;
	winrt::com_ptr<ID3D12CommandQueue> queue12;
	winrt::com_ptr<ID3D12CommandAllocator> allocator12;
	winrt::com_ptr<ID3D12GraphicsCommandList> commandList12;
	winrt::com_ptr<ID3D11Fence> fence11;
	winrt::com_ptr<ID3D12Fence> fence12;
	winrt::com_ptr<ID3D11Texture2D> sharedInput11;
	winrt::com_ptr<ID3D12Resource> sharedInput12;
	winrt::com_ptr<ID3D11Texture2D> sparseFlow11;
	winrt::com_ptr<ID3D12Resource> sparseFlow12;
	winrt::com_ptr<ID3D12Resource> scd12;
	winrt::com_ptr<ID3D11Texture2D> denseMotion;
	winrt::com_ptr<ID3D11Texture2D> denseConfidence;
	ID3D11Texture2D* inputTexture = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> inputSrv;
	winrt::com_ptr<ID3D11UnorderedAccessView> sharedInputUav;
	winrt::com_ptr<ID3D11ShaderResourceView> sparseFlowSrv;
	winrt::com_ptr<ID3D11UnorderedAccessView> denseMotionUav;
	winrt::com_ptr<ID3D11UnorderedAccessView> denseConfidenceUav;
	winrt::com_ptr<ID3D11ComputeShader> prepareShader;
	winrt::com_ptr<ID3D11ComputeShader> densifyShader;
	winrt::com_ptr<ID3D11Buffer> prepareParams;
	winrt::com_ptr<ID3D11Buffer> densifyParams;
	std::vector<uint8_t> backendScratch;
	FfxOpticalflowContext opticalFlowContext{};
	FrameGuidanceExtent extent{};
	FrameGuidanceExtent opticalFlowExtent{};
	FrameGuidanceExtent sparseExtent{};
	AmdOpticalFlowMode mode = AmdOpticalFlowMode::Quality;
	AmdOpticalFlowHdrProtocol hdrProtocol{};
	FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::Initialize;
	uint64_t fenceValue = 0;
	uint64_t lastSubmittedValue = 0;
	uint64_t executeCount = 0;
	FrameGuidanceFrameId previousProcessedFrameId = 0;
	bool contextCreated = false;
	bool historyValid = false;
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
};

AmdOpticalFlowProvider::AmdOpticalFlowProvider(AmdOpticalFlowMode mode) :
	_mode(mode), _impl(std::make_unique<Impl>()) {}

AmdOpticalFlowProvider::~AmdOpticalFlowProvider() = default;

bool AmdOpticalFlowProvider::Initialize(
	DeviceResources& resources,
	FrameGuidanceExtent sourceExtent
) noexcept {
	_impl->hdrProtocol = _hdrProtocol;
	return _impl->Create(resources, sourceExtent, _mode);
}

bool AmdOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame& frame,
	MotionVectorProviderOutput& output
) noexcept {
	Impl& impl = *_impl;
	const auto prepareStart = std::chrono::steady_clock::now();
	if (!frame.color || frame.sourceExtent != impl.extent ||
		!impl.contextCreated || !WaitForFence(impl.fence12.get(), impl.lastSubmittedValue) ||
		!impl.PrepareInput(frame.color)) return false;

	const auto prepareEnd = std::chrono::steady_clock::now();
	const uint64_t inputReady = ++impl.fenceValue;
	HRESULT hr = impl.context11->Signal(impl.fence11.get(), inputReady);
	impl.context11->Flush();
	if (SUCCEEDED(hr)) hr = impl.queue12->Wait(impl.fence12.get(), inputReady);
	if (SUCCEEDED(hr)) hr = impl.allocator12->Reset();
	if (SUCCEEDED(hr)) hr = impl.commandList12->Reset(
		impl.allocator12.get(), nullptr);
	if (FAILED(hr)) return false;

	FfxOpticalflowDispatchDescription dispatch{
		.commandList = ffxGetCommandListDX12(impl.commandList12.get()),
		.color = ffxGetResourceDX12(
			impl.sharedInput12.get(),
			ffxGetResourceDescriptionDX12(impl.sharedInput12.get()),
			L"Magpie AMD OF color", FFX_API_RESOURCE_STATE_COMMON),
		.opticalFlowVector = ffxGetResourceDX12(
			impl.sparseFlow12.get(),
			ffxGetResourceDescriptionDX12(
				impl.sparseFlow12.get(), FFX_API_RESOURCE_USAGE_UAV),
			L"Magpie AMD OF vector", FFX_API_RESOURCE_STATE_COMMON),
		.opticalFlowSCD = ffxGetResourceDX12(
			impl.scd12.get(),
			ffxGetResourceDescriptionDX12(
				impl.scd12.get(), FFX_API_RESOURCE_USAGE_UAV),
			L"Magpie AMD OF SCD", FFX_API_RESOURCE_STATE_COMMON),
		.reset = !impl.historyValid ||
			impl.resetReason != FrameGuidanceResetReason::None,
		.backbufferTransferFunction =
#ifdef FFX_API_BACKBUFFER_TRANSFER_FUNCTION_LINEAR
			impl.hdrProtocol.transfer == GroupBTransfer::Linear ?
			FFX_API_BACKBUFFER_TRANSFER_FUNCTION_LINEAR :
#endif
			FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB,
		.minMaxLuminance = { impl.hdrProtocol.minMaxLuminance[0], impl.hdrProtocol.minMaxLuminance[1] }
	};
	const auto opticalFlowStart = std::chrono::steady_clock::now();
	if (ffxOpticalflowContextDispatch(
		&impl.opticalFlowContext, &dispatch) != FFX_OK || FAILED(
		impl.commandList12->Close())) {
		impl.historyValid = false;
		impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
		return false;
	}
	ID3D12CommandList* lists[]{ impl.commandList12.get() };
	impl.queue12->ExecuteCommandLists(1, lists);
	const uint64_t outputReady = ++impl.fenceValue;
	impl.lastSubmittedValue = outputReady;
	if (FAILED(impl.queue12->Signal(impl.fence12.get(), outputReady)) || FAILED(
		impl.context11->Wait(impl.fence11.get(), outputReady))) return false;
#ifdef MP_ENABLE_NATIVE_BACKEND_TIMING
	if (!WaitForFence(impl.fence12.get(), outputReady)) return false;
#endif
	const auto opticalFlowEnd = std::chrono::steady_clock::now();

	const bool resetFrame = dispatch.reset;
	if (resetFrame) {
		static constexpr float ZERO[4]{};
		impl.context11->ClearUnorderedAccessViewFloat(
			impl.denseMotionUav.get(), ZERO);
		impl.context11->ClearUnorderedAccessViewFloat(
			impl.denseConfidenceUav.get(), ZERO);
	} else if (!impl.Densify()) {
		impl.historyValid = false;
		impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
		return false;
	}
#ifdef MP_ENABLE_NATIVE_BACKEND_TIMING
	const uint64_t denseReady = ++impl.fenceValue;
	if (FAILED(impl.context11->Signal(impl.fence11.get(), denseReady))) return false;
	impl.context11->Flush();
	impl.lastSubmittedValue = denseReady;
	if (!WaitForFence(impl.fence12.get(), denseReady)) return false;
	Logger::Get().Info(fmt::format("AMD OF synchronous validation: frame={} previous={} extent={}x{} reset={} synchronized_wall_ms={:.3f}",
		frame.frameId, impl.previousProcessedFrameId, impl.extent.width, impl.extent.height, dispatch.reset,
		std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count()));
#endif
	const auto densifyEnd = std::chrono::steady_clock::now();
	++impl.executeCount;
	if (impl.executeCount <= 2 || impl.executeCount % 120 == 0) {
		Logger::Get().Info(fmt::format(
			"Frame Guidance AMD OF timing frameId={} executeCount={} "
			"prepareSubmit={:.3f} ms opticalFlowSubmit={:.3f} ms "
			"densifySubmit={:.3f} ms totalSubmit={:.3f} ms",
			frame.frameId, impl.executeCount,
			std::chrono::duration<double, std::milli>(
				prepareEnd - prepareStart).count(),
			std::chrono::duration<double, std::milli>(
				opticalFlowEnd - opticalFlowStart).count(),
			std::chrono::duration<double, std::milli>(
				densifyEnd - opticalFlowEnd).count(),
			std::chrono::duration<double, std::milli>(
				densifyEnd - opticalFlowStart).count()));
	}

	const FrameGuidanceMetadata metadata = MakeMetadata(
		frame, impl.resetReason, resetFrame);
	output.motion = {
		.texture = impl.denseMotion.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = metadata
	};
	output.confidence = {
		.texture = impl.denseConfidence.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = metadata
	};
	impl.historyValid = true;
	impl.previousProcessedFrameId = frame.frameId;
	impl.resetReason = FrameGuidanceResetReason::None;
	return true;
}

void AmdOpticalFlowProvider::Reset(FrameGuidanceResetReason reason) noexcept {
	_impl->historyValid = false;
	_impl->resetReason = reason;
}

bool AmdOpticalFlowProvider::Resize(FrameGuidanceExtent sourceExtent) noexcept {
	if (!_impl->owner) return false;
	const bool result = _impl->Create(*_impl->owner, sourceExtent, _mode);
	if (result) _impl->resetReason = FrameGuidanceResetReason::Resize;
	return result;
}

OpticalFlowInitializationError
AmdOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#else

namespace Magpie {

struct AmdOpticalFlowProvider::Impl {
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
};
AmdOpticalFlowProvider::AmdOpticalFlowProvider(AmdOpticalFlowMode mode) :
	_mode(mode), _impl(std::make_unique<Impl>()) {}
AmdOpticalFlowProvider::~AmdOpticalFlowProvider() = default;
bool AmdOpticalFlowProvider::Initialize(
	DeviceResources&, FrameGuidanceExtent) noexcept { return false; }
bool AmdOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame&, MotionVectorProviderOutput&) noexcept { return false; }
void AmdOpticalFlowProvider::Reset(FrameGuidanceResetReason) noexcept {}
bool AmdOpticalFlowProvider::Resize(FrameGuidanceExtent) noexcept { return false; }
OpticalFlowInitializationError
AmdOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#endif
