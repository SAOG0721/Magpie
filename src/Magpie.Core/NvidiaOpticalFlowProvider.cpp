#include "pch.h"
#include "FrameTrace.h"
#include "NvidiaOpticalFlowProvider.h"
#include "NativeBackendTiming.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"
#include "ScalingWindow.h"

#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
#include <nvOpticalFlowD3D11.h>

namespace Magpie {

namespace {

constexpr char DENSIFY_FLOW_HLSL[] = R"(
Texture2D<int2> ForwardFlow : register(t0);
Texture2D<int2> BackwardFlow : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
RWTexture2D<float2> DenseMotion : register(u0);
RWTexture2D<float> DenseConfidence : register(u1);
RWTexture2D<float2> DenseReverseMotion : register(u2);
RWTexture2D<float> DenseReverseConfidence : register(u3);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 FlowExtent;
    uint GridSize;
    uint HasForwardCost;
    uint HasBackward;
    uint HasBackwardCost;
};

float2 LoadFlow(Texture2D<int2> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float2(field.Load(int3(p, 0))) / 32.0;
}

float2 SampleFlow(Texture2D<int2> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadFlow(field, p0), LoadFlow(field, p0 + int2(1, 0)), f.x),
        lerp(LoadFlow(field, p0 + int2(0, 1)),
             LoadFlow(field, p0 + int2(1, 1)), f.x),
        f.y);
}

float LoadCost(Texture2D<uint> field, int2 p) {
    p = clamp(p, int2(0, 0), int2(FlowExtent) - 1);
    return float(field.Load(int3(p, 0))) / 255.0;
}

float SampleCost(Texture2D<uint> field, float2 sourcePixel) {
    float2 gridPos = sourcePixel / float(GridSize) - 0.5;
    int2 p0 = int2(floor(gridPos));
    float2 f = frac(gridPos);
    return lerp(
        lerp(LoadCost(field, p0), LoadCost(field, p0 + int2(1, 0)), f.x),
        lerp(LoadCost(field, p0 + int2(0, 1)),
             LoadCost(field, p0 + int2(1, 1)), f.x),
        f.y);
}

[numthreads(8, 8, 1)]
void Densify(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= SourceExtent)) return;

    float2 p = float2(tid.xy) + 0.5;
    float2 forward = SampleFlow(ForwardFlow, p);
    // Turing has no hardware cost output. A conservative non-zero baseline
    // still lets downstream consumers use coherent motion while reset frames
    // remain explicitly zero-confidence.
    float confidence = HasForwardCost != 0 ?
        1.0 - SampleCost(ForwardCost, p) : 0.65;

    if (HasBackward != 0) {
        float2 referencePixel = p + forward;
        bool inside = all(referencePixel >= 0.0) &&
            all(referencePixel < float2(SourceExtent));
        float2 backward = SampleFlow(BackwardFlow, referencePixel);
        float fbError = length(forward + backward);
        float threshold = 0.75 + 0.05 * length(forward);
        confidence *= inside ? saturate(1.0 - fbError / threshold) : 0.0;
        if (HasBackwardCost != 0) {
            confidence *= 1.0 - SampleCost(BackwardCost, referencePixel);
        }
    }

    DenseMotion[tid.xy] = forward;
    DenseConfidence[tid.xy] = saturate(confidence);
    if ((HasBackward & 2) != 0) {
        // Bit 0 enables consistency checks; bit 1 exports the optional field.
        // Backward is sampled in previous-frame coordinates; it is not -forward.
        float2 reverse = SampleFlow(BackwardFlow, p);
        float reverseConfidence = HasBackwardCost != 0 ?
            1.0 - SampleCost(BackwardCost, p) : 0.65;
        float2 currentPixel = p + reverse;
        bool inside = all(currentPixel >= 0.0) && all(currentPixel < float2(SourceExtent));
        float2 pairedForward = SampleFlow(ForwardFlow, currentPixel);
        float threshold = 0.75 + 0.05 * length(reverse);
        reverseConfidence *= inside ? saturate(1.0 - length(reverse + pairedForward) / threshold) : 0.0;
        if (HasForwardCost != 0) reverseConfidence *= 1.0 - SampleCost(ForwardCost, currentPixel);
        DenseReverseMotion[tid.xy] = reverse;
        DenseReverseConfidence[tid.xy] = saturate(reverseConfidence);
    }
}
)";

constexpr char HDR_TO_NVOF_HLSL[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Target : register(u0);
[numthreads(8, 8, 1)]
void Convert(uint3 tid : SV_DispatchThreadID) {
    uint width, height; Target.GetDimensions(width, height);
    if (tid.x >= width || tid.y >= height) return;
    float4 value = Source.Load(int3(tid.xy, 0));
    Target[tid.xy] = float4(saturate(max(value.rgb, 0.0) / 4.5), saturate(value.a));
}
)";

template <typename T>
T GetExport(HMODULE module, const char* name) noexcept {
	return reinterpret_cast<T>(GetProcAddress(module, name));
}

bool HasFormat(const std::vector<DXGI_FORMAT>& formats, DXGI_FORMAT value) {
	return std::find(formats.begin(), formats.end(), value) != formats.end();
}

constexpr std::array<float, 2> DecodeS105(int16_t x, int16_t y) noexcept {
	return { float(x) / 32.0f, float(y) / 32.0f };
}

// Compile-time convention probes for the two axis cases used by the manual
// translation test. NVOF stores five fractional bits and we preserve X/Y signs.
static_assert(DecodeS105(64, 0)[0] == 2.0f);
static_assert(DecodeS105(0, -96)[1] == -3.0f);

struct NvofProfile {
	uint32_t gridSize;
	NV_OF_PERF_LEVEL perfLevel;
	std::string_view label;
	std::string_view perfLabel;
};

constexpr NvofProfile ResolveProfile(
	NvidiaOpticalFlowQuality quality
) noexcept {
	switch (quality) {
	case NvidiaOpticalFlowQuality::Performance:
		return { 4, NV_OF_PERF_LEVEL_FAST, "4F", "FAST" };
	case NvidiaOpticalFlowQuality::Quality:
		return { 4, NV_OF_PERF_LEVEL_SLOW, "4S", "SLOW" };
	case NvidiaOpticalFlowQuality::HighQuality:
		return { 2, NV_OF_PERF_LEVEL_MEDIUM, "2M", "MEDIUM" };
	case NvidiaOpticalFlowQuality::HighestQuality:
		return { 2, NV_OF_PERF_LEVEL_SLOW, "2S", "SLOW" };
	case NvidiaOpticalFlowQuality::Balanced:
	default:
		return { 4, NV_OF_PERF_LEVEL_MEDIUM, "4M", "MEDIUM" };
	}
}

FrameGuidanceMetadata MakeMetadata(
	const FrameGuidanceFrame& frame,
	FrameGuidanceResetReason resetReason,
	bool isZero
) noexcept {
	return {
		.frameId = frame.frameId,
		.captureSequence = frame.captureSequence,
		.resourceGeneration = frame.resourceGeneration,
		.timestamp100ns = frame.timestamp100ns,
		.sourceExtent = frame.sourceExtent,
		.validRegion = frame.validRegion,
		.resetReason = resetReason,
		.valid = true,
		.isZero = isZero,
		.requiresHistoryReset = resetReason != FrameGuidanceResetReason::None
	};
}

struct NvofTimingSummary {
	double average = 0.0;
	double p95 = 0.0;
	double p99 = 0.0;
	double maximum = 0.0;
	size_t count = 0;
};

struct NvofTimingWindow {
	static constexpr size_t CAPACITY = 120;
	std::array<double, CAPACITY> values{};
	size_t count = 0;
	size_t next = 0;

	void Add(double value) noexcept {
		values[next] = value;
		next = (next + 1) % CAPACITY;
		count = std::min(count + 1, CAPACITY);
	}

	NvofTimingSummary Summarize() const noexcept {
		NvofTimingSummary result{ .count = count };
		if (!count) return result;
		std::array<double, CAPACITY> sorted{};
		std::copy_n(values.begin(), count, sorted.begin());
		std::sort(sorted.begin(), sorted.begin() + count);
		double total = 0.0;
		for (size_t i = 0; i < count; ++i) total += sorted[i];
		result.average = total / static_cast<double>(count);
		result.p95 = sorted[std::min(count - 1, (count * 95 + 99) / 100 - 1)];
		result.p99 = sorted[std::min(count - 1, (count * 99 + 99) / 100 - 1)];
		result.maximum = sorted[count - 1];
		return result;
	}
};

}

struct NvidiaOpticalFlowProvider::Impl {
	using CreateApiFn = NV_OF_STATUS(NVOFAPI*)(
		uint32_t, NV_OF_D3D11_API_FUNCTION_LIST*);
	using GetMaxVersionFn = NV_OF_STATUS(NVOFAPI*)(uint32_t*);
	static constexpr uint32_t GPU_QUERY_SLOT_COUNT = 4;
	struct GpuQuerySlot {
		winrt::com_ptr<ID3D11Query> disjoint;
		winrt::com_ptr<ID3D11Query> start;
		winrt::com_ptr<ID3D11Query> end;
		bool pending = false;
	};

	~Impl() { DestroySession(); }

	void DestroySession() noexcept {
		if (session && api.nvOFUnregisterResourceD3D11) {
			for (NvOFGPUBufferHandle& handle : registered) {
				if (handle) {
					api.nvOFUnregisterResourceD3D11(handle);
					handle = nullptr;
				}
			}
		}
		for (NvOFGPUBufferHandle& handle : registered) handle = nullptr;
		if (session && api.nvOFDestroy) {
			api.nvOFDestroy(session);
		}
		session = nullptr;
		api = {};
		if (module) {
			FreeLibrary(module);
			module = nullptr;
		}

		for (auto& texture : input) texture = nullptr;
		for (auto& texture : flow) texture = nullptr;
		for (auto& texture : cost) texture = nullptr;
		for (auto& srv : flowSrv) srv = nullptr;
		for (auto& srv : costSrv) srv = nullptr;
		motion = nullptr;
		confidence = nullptr;
		motionUav = nullptr;
		confidenceUav = nullptr;
		reverseMotion = nullptr;
		reverseConfidence = nullptr;
		reverseMotionUav = nullptr;
		reverseConfidenceUav = nullptr;
		densifyShader = nullptr;
		paramsBuffer = nullptr;
		hdrToNvofShader = nullptr;
		hdrSourceSrv = nullptr;
		hdrSourceTexture = nullptr;
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			slot.disjoint = nullptr;
			slot.start = nullptr;
			slot.end = nullptr;
			slot.pending = false;
		}
		gpuTimingWindow = {};
		nextGpuQuerySlot = 0;
		gpuTimingSampleCount = 0;
		gpuTimingAvailable = false;
		profileLabel = {};
		inputDxgiFormat = DXGI_FORMAT_UNKNOWN;
		inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		gridSize = 0;
		costEnabled = false;
		previousSlot = 0;
		bidirectional = false;
		historyValid = false;
		previousFrame = {};
		resources = nullptr;
		device = nullptr;
		context = nullptr;
		extent = {};
	}

	bool CreateGpuTimingQueries() noexcept {
		D3D11_QUERY_DESC desc{ .Query = D3D11_QUERY_TIMESTAMP_DISJOINT };
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (FAILED(device->CreateQuery(&desc, slot.disjoint.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, slot.start.put())) ||
				FAILED(device->CreateQuery(&desc, slot.end.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		}
		return true;
	}

	void PollGpuTimings() noexcept {
		for (GpuQuerySlot& slot : gpuQuerySlots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			if (context->GetData(
				slot.disjoint.get(), &disjoint, sizeof(disjoint),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			uint64_t start = 0;
			uint64_t end = 0;
			const bool ready = context->GetData(
				slot.start.get(), &start, sizeof(start),
				D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
				context->GetData(
					slot.end.get(), &end, sizeof(end),
					D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
			if (!ready) continue;
			slot.pending = false;
			if (disjoint.Disjoint || !disjoint.Frequency || end < start) continue;
			gpuTimingWindow.Add(
				double(end - start) * 1000.0 / double(disjoint.Frequency));
			++gpuTimingSampleCount;
			if (gpuTimingSampleCount <= 4 || gpuTimingSampleCount % 120 == 0) {
				const NvofTimingSummary timing = gpuTimingWindow.Summarize();
				Logger::Get().Info(fmt::format(
					"Frame Guidance NVOF GPU interval: profile={} samples={} avg={:.3f} ms "
					"p95={:.3f} ms p99={:.3f} ms max={:.3f} ms",
					profileLabel, timing.count, timing.average, timing.p95,
					timing.p99, timing.maximum));
			}
		}
	}

	GpuQuerySlot* BeginGpuTiming() noexcept {
		if (!gpuTimingAvailable) return nullptr;
		PollGpuTimings();
		GpuQuerySlot& slot =
			gpuQuerySlots[nextGpuQuerySlot++ % GPU_QUERY_SLOT_COUNT];
		if (slot.pending) return nullptr;
		context->Begin(slot.disjoint.get());
		context->End(slot.start.get());
		return &slot;
	}

	void EndGpuTiming(GpuQuerySlot* slot) noexcept {
		if (!slot) return;
		context->End(slot->end.get());
		context->End(slot->disjoint.get());
		slot->pending = true;
	}

	bool QueryFormats(
		NV_OF_BUFFER_USAGE usage,
		std::vector<DXGI_FORMAT>& formats
	) noexcept {
		uint32_t count = 0;
		if (api.nvOFGetSurfaceFormatCountD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return false;
		}
		formats.resize(count);
		return api.nvOFGetSurfaceFormatD3D11(
			session, usage, NV_OF_MODE_OPTICALFLOW,
			formats.data()) == NV_OF_SUCCESS;
	}

	std::vector<uint32_t> QueryCaps(NV_OF_CAPS cap) noexcept {
		uint32_t count = 0;
		if (!api.nvOFGetCaps ||
			api.nvOFGetCaps(session, cap, nullptr, &count) != NV_OF_SUCCESS ||
			count == 0 || count > 64) {
			return {};
		}
		std::vector<uint32_t> values(count);
		if (api.nvOFGetCaps(session, cap, values.data(), &count) != NV_OF_SUCCESS) {
			return {};
		}
		values.resize(count);
		return values;
	}

	bool Register(ID3D11Resource* resource, NvOFGPUBufferHandle& handle) noexcept {
		return api.nvOFRegisterResourceD3D11 &&
			api.nvOFRegisterResourceD3D11(
				session, resource, &handle) == NV_OF_SUCCESS && handle;
	}

	bool CreateTextures() noexcept {
		const UINT sourceBind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		for (auto& texture : input) {
			texture = DirectXHelper::CreateTexture2D(
				device, inputDxgiFormat, extent.width, extent.height,
				sourceBind);
			if (!texture || FAILED(device->CreateUnorderedAccessView(texture.get(), nullptr,
				inputUav[&texture - &input[0]].put()))) return false;
		}

		const uint32_t flowWidth = (extent.width + gridSize - 1) / gridSize;
		const uint32_t flowHeight = (extent.height + gridSize - 1) / gridSize;
		for (size_t i = 0; i < flow.size(); ++i) {
			flow[i] = DirectXHelper::CreateTexture2D(
				device, DXGI_FORMAT_R16G16_SINT, flowWidth, flowHeight,
				D3D11_BIND_SHADER_RESOURCE);
			if (!flow[i] || FAILED(device->CreateShaderResourceView(
				flow[i].get(), nullptr, flowSrv[i].put()))) {
				return false;
			}
		}
		if (costEnabled) {
			for (size_t i = 0; i < cost.size(); ++i) {
				cost[i] = DirectXHelper::CreateTexture2D(
					device, DXGI_FORMAT_R8_UINT, flowWidth, flowHeight,
					D3D11_BIND_SHADER_RESOURCE);
				if (!cost[i] || FAILED(device->CreateShaderResourceView(
					cost[i].get(), nullptr, costSrv[i].put()))) {
					return false;
				}
			}
		}

		constexpr UINT GUIDE_BIND =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		motion = DirectXHelper::CreateSharedTexture2D(
			device, DXGI_FORMAT_R16G16_FLOAT, extent.width, extent.height,
			GUIDE_BIND, "NVOF/Motion");
		confidence = DirectXHelper::CreateSharedTexture2D(
			device, DXGI_FORMAT_R8_UNORM, extent.width, extent.height,
			GUIDE_BIND, "NVOF/Confidence");
		if (!motion || !confidence ||
			FAILED(device->CreateUnorderedAccessView(
				motion.get(), nullptr, motionUav.put())) ||
			FAILED(device->CreateUnorderedAccessView(
				confidence.get(), nullptr, confidenceUav.put()))) {
			return false;
		}
		if (bidirectional) {
			reverseMotion = DirectXHelper::CreateSharedTexture2D(
				device, DXGI_FORMAT_R16G16_FLOAT, extent.width, extent.height,
				GUIDE_BIND, "NVOF/ReverseMotion");
			reverseConfidence = DirectXHelper::CreateSharedTexture2D(
				device, DXGI_FORMAT_R8_UNORM, extent.width, extent.height,
				GUIDE_BIND, "NVOF/ReverseConfidence");
			if (!reverseMotion || !reverseConfidence ||
				FAILED(device->CreateUnorderedAccessView(reverseMotion.get(), nullptr, reverseMotionUav.put())) ||
				FAILED(device->CreateUnorderedAccessView(reverseConfidence.get(), nullptr, reverseConfidenceUav.put()))) {
				reverseMotion = nullptr; reverseConfidence = nullptr;
				reverseMotionUav = nullptr; reverseConfidenceUav = nullptr;
				Logger::Get().Warn("NVOF reverse export unavailable; ordinary forward guidance remains enabled");
			}
		}

		for (size_t i = 0; i < input.size(); ++i) {
			if (!Register(input[i].get(), registered[i])) return false;
		}
		for (size_t i = 0; i < flow.size(); ++i) {
			if (!Register(flow[i].get(), registered[2 + i])) return false;
		}
		if (costEnabled) {
			for (size_t i = 0; i < cost.size(); ++i) {
				if (!Register(cost[i].get(), registered[4 + i])) return false;
			}
		}
		return true;
	}

	bool CreatePostProcess() noexcept {
		winrt::com_ptr<ID3DBlob> hdrBlob;
		if (!DirectXHelper::CompileComputeShader(
			HDR_TO_NVOF_HLSL, "Convert", hdrBlob.put(),
			"FrameGuidance/NVOF_HdrToInput.hlsl") ||
			FAILED(device->CreateComputeShader(hdrBlob->GetBufferPointer(),
				hdrBlob->GetBufferSize(), nullptr, hdrToNvofShader.put()))) return false;
		winrt::com_ptr<ID3DBlob> shaderBlob;
		if (!DirectXHelper::CompileComputeShader(
			DENSIFY_FLOW_HLSL, "Densify", shaderBlob.put(),
			"FrameGuidance/NVOF_Densify.hlsl")) {
			return false;
		}
		if (FAILED(device->CreateComputeShader(
			shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr,
			densifyShader.put()))) {
			return false;
		}
		const D3D11_BUFFER_DESC desc{
			.ByteWidth = 32,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		return SUCCEEDED(device->CreateBuffer(&desc, nullptr, paramsBuffer.put()));
	}

	bool CreateSession(
		DeviceResources& deviceResources,
		FrameGuidanceExtent newExtent,
		NvidiaOpticalFlowQuality requestedQuality
	) noexcept {
		DestroySession();
		initializationError = OpticalFlowInitializationError::ProviderUnavailable;
		bool initialized = false;
		auto rollback = wil::scope_exit([&]() noexcept {
			if (initialized) return;
			const OpticalFlowInitializationError error = initializationError;
			DestroySession();
			initializationError = error;
		});
		resources = &deviceResources;
		device = deviceResources.GetD3DDevice();
		context = deviceResources.GetD3DDC();
		extent = newExtent;
		if (!device || !context || !extent.IsValid()) return false;
		DXGI_ADAPTER_DESC1 adapterDesc{};
		if (!deviceResources.GetGraphicsAdapter() || FAILED(
			deviceResources.GetGraphicsAdapter()->GetDesc1(&adapterDesc)) ||
			adapterDesc.VendorId != 0x10DE) {
			Logger::Get().Warn(
				"NVOF unavailable: selected adapter is not an NVIDIA adapter; "
				"select AMD OF or None");
			return false;
		}

		module = LoadLibraryExW(
			L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (!module) {
			Logger::Get().Warn(
				"NVOF unavailable: driver nvofapi64.dll was not found");
			return false;
		}
		auto createApi = GetExport<CreateApiFn>(
			module, "NvOFAPICreateInstanceD3D11");
		if (!createApi) return false;
		uint32_t driverVersion = NV_OF_API_VERSION;
		if (auto getMaxVersion = GetExport<GetMaxVersionFn>(
			module, "NvOFGetMaxSupportedApiVersion")) {
			if (getMaxVersion(&driverVersion) != NV_OF_SUCCESS ||
				driverVersion < NV_OF_API_VERSION) {
				Logger::Get().Warn("NVOF unavailable: NVIDIA driver API is too old");
				return false;
			}
		}
		if (createApi(NV_OF_API_VERSION, &api) != NV_OF_SUCCESS ||
			!api.nvCreateOpticalFlowD3D11 || !api.nvOFInit ||
			!api.nvOFExecute || !api.nvOFRegisterResourceD3D11 ||
			!api.nvOFUnregisterResourceD3D11) {
			return false;
		}
		if (api.nvCreateOpticalFlowD3D11(
			device, context, &session) != NV_OF_SUCCESS || !session) {
			return false;
		}

		std::vector<DXGI_FORMAT> inputFormats;
		std::vector<DXGI_FORMAT> outputFormats;
		std::vector<DXGI_FORMAT> costFormats;
		const bool costFormatAvailable =
			QueryFormats(NV_OF_BUFFER_USAGE_COST, costFormats) &&
			HasFormat(costFormats, DXGI_FORMAT_R8_UINT);
		if (!QueryFormats(NV_OF_BUFFER_USAGE_INPUT, inputFormats) ||
			!QueryFormats(NV_OF_BUFFER_USAGE_OUTPUT, outputFormats) ||
			!HasFormat(outputFormats, DXGI_FORMAT_R16G16_SINT)) {
			Logger::Get().Warn("NVOF required S10.5 output format unavailable");
			return false;
		}
		const bool hdrEnabled = ScalingWindow::Get().Options().IsHdrCompatibilityEnabled();
		if (!hdrEnabled) {
			if (!HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM)) {
				Logger::Get().Warn("NVOF required ABGR8 input format unavailable");
				return false;
			}
			inputDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		} else if (HasFormat(inputFormats, DXGI_FORMAT_B8G8R8A8_UNORM)) {
			inputDxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
			inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		} else if (HasFormat(inputFormats, DXGI_FORMAT_R8G8B8A8_UNORM)) {
			inputDxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
		} else if (HasFormat(inputFormats, DXGI_FORMAT_R8_UNORM)) {
			inputDxgiFormat = DXGI_FORMAT_R8_UNORM;
			inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
		} else if (HasFormat(inputFormats, DXGI_FORMAT_NV12)) {
			inputDxgiFormat = DXGI_FORMAT_NV12;
			inputBufferFormat = NV_OF_BUFFER_FORMAT_NV12;
		} else {
			Logger::Get().Warn("NVOF input formats unavailable (ABGR8/GRAYSCALE8/NV12)");
			return false;
		}

		const std::vector<uint32_t> grids = QueryCaps(
			NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES);
		const NvofProfile profile = ResolveProfile(requestedQuality);
		if (std::find(grids.begin(), grids.end(), profile.gridSize) == grids.end()) {
			initializationError = OpticalFlowInitializationError::QualityUnsupported;
			Logger::Get().Warn(fmt::format(
				"NVOF quality unsupported: profile={} requires {}x{} output grid; "
				"select a lower quality, AMD OF, or None",
				profile.label, profile.gridSize, profile.gridSize));
			return false;
		}
		gridSize = profile.gridSize;
		profileLabel = profile.label;
		initializationError = OpticalFlowInitializationError::InteropFailed;

		NV_OF_INIT_PARAMS init{
			.width = extent.width,
			.height = extent.height,
			.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(gridSize),
			.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED,
			.mode = NV_OF_MODE_OPTICALFLOW,
			.perfLevel = profile.perfLevel,
			.enableExternalHints = NV_OF_FALSE,
			.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE,
			.hPrivData = nullptr,
			.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED,
			.enableRoi = NV_OF_FALSE,
			.predDirection = NV_OF_PRED_DIRECTION_BOTH,
			.enableGlobalFlow = NV_OF_FALSE,
			.inputBufferFormat = inputBufferFormat
		};
		NV_OF_STATUS status = api.nvOFInit(session, &init);
		bidirectional = status == NV_OF_SUCCESS;
		costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
		if (status != NV_OF_SUCCESS && costFormatAvailable) {
			init.enableOutputCost = NV_OF_FALSE;
			status = api.nvOFInit(session, &init);
			bidirectional = status == NV_OF_SUCCESS;
			costEnabled = false;
		}
		if (status != NV_OF_SUCCESS) {
			init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
			init.enableOutputCost = costFormatAvailable ? NV_OF_TRUE : NV_OF_FALSE;
			status = api.nvOFInit(session, &init);
			costEnabled = costFormatAvailable && status == NV_OF_SUCCESS;
			if (status != NV_OF_SUCCESS && costFormatAvailable) {
				init.enableOutputCost = NV_OF_FALSE;
				status = api.nvOFInit(session, &init);
				costEnabled = false;
			}
			bidirectional = false;
		}
		if (status != NV_OF_SUCCESS || !CreateTextures() || !CreatePostProcess()) {
			return false;
		}
		gpuTimingAvailable = NativeBackendTiming::Enabled && CreateGpuTimingQueries();
		if constexpr (NativeBackendTiming::Enabled) {
			if (!gpuTimingAvailable) {
				Logger::Get().Warn(
					"Frame Guidance NVOF GPU timing unavailable; continuing without telemetry");
			}
		}

		resetReason = FrameGuidanceResetReason::Initialize;
		historyValid = false;
		previousSlot = 0;
		Logger::Get().Info(fmt::format(
			"Frame Guidance NVOF initialized: profile={} grid={}x{} perf={} "
			"driverApi={}.{} cost={} bidirectional={} input={}x{} output={}x{} "
			"convention=current-to-previous/source-pixels/S10.5-self-test-passed",
			profile.label, gridSize, gridSize, profile.perfLabel,
			driverVersion >> 4, driverVersion & 0xf,
			costEnabled ? "R8_UINT" : "none", bidirectional,
			extent.width, extent.height,
			(extent.width + gridSize - 1) / gridSize,
			(extent.height + gridSize - 1) / gridSize));
		initializationError = OpticalFlowInitializationError::None;
		initialized = true;
		return true;
	}

	void ClearDenseOutput() noexcept {
		static constexpr float ZERO[4]{};
		context->ClearUnorderedAccessViewFloat(motionUav.get(), ZERO);
		context->ClearUnorderedAccessViewFloat(confidenceUav.get(), ZERO);
		if (reverseMotionUav) context->ClearUnorderedAccessViewFloat(reverseMotionUav.get(), ZERO);
		if (reverseConfidenceUav) context->ClearUnorderedAccessViewFloat(reverseConfidenceUav.get(), ZERO);
	}

	bool Densify() noexcept {
		struct alignas(16) Params {
			uint32_t sourceWidth;
			uint32_t sourceHeight;
			uint32_t flowWidth;
			uint32_t flowHeight;
			uint32_t gridSize;
			uint32_t hasForwardCost;
			uint32_t hasBackward;
			uint32_t hasBackwardCost;
		};
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(
			paramsBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			return false;
		}
		*static_cast<Params*>(mapped.pData) = {
			extent.width, extent.height,
			(extent.width + gridSize - 1) / gridSize,
			(extent.height + gridSize - 1) / gridSize,
			gridSize, costEnabled ? 1u : 0u,
			(bidirectional ? 1u : 0u) | (reverseMotionUav && reverseConfidenceUav ? 2u : 0u),
			bidirectional && costEnabled ? 1u : 0u
		};
		context->Unmap(paramsBuffer.get(), 0);

		ID3D11ShaderResourceView* srvs[]{
			flowSrv[0].get(), bidirectional ? flowSrv[1].get() : nullptr,
			costSrv[0].get(), bidirectional ? costSrv[1].get() : nullptr
		};
		ID3D11UnorderedAccessView* uavs[]{ motionUav.get(), confidenceUav.get(),
			reverseMotionUav.get(), reverseConfidenceUav.get() };
		ID3D11Buffer* buffers[]{ paramsBuffer.get() };
		context->CSSetShader(densifyShader.get(), nullptr, 0);
		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, buffers);
		context->Dispatch((extent.width + 7) / 8, (extent.height + 7) / 8, 1);

		ID3D11ShaderResourceView* nullSrvs[ARRAYSIZE(srvs)]{};
		ID3D11UnorderedAccessView* nullUavs[ARRAYSIZE(uavs)]{};
		context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		return true;
	}

	DeviceResources* resources = nullptr;
	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* context = nullptr;
	HMODULE module = nullptr;
	NV_OF_D3D11_API_FUNCTION_LIST api{};
	NvOFHandle session = nullptr;
	std::array<NvOFGPUBufferHandle, 8> registered{};
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> input;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> flow;
	std::array<winrt::com_ptr<ID3D11Texture2D>, 2> cost;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> flowSrv;
	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 2> costSrv;
	winrt::com_ptr<ID3D11Texture2D> motion;
	winrt::com_ptr<ID3D11Texture2D> confidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> motionUav;
	winrt::com_ptr<ID3D11UnorderedAccessView> confidenceUav;
	winrt::com_ptr<ID3D11Texture2D> reverseMotion, reverseConfidence;
	winrt::com_ptr<ID3D11UnorderedAccessView> reverseMotionUav, reverseConfidenceUav;
	winrt::com_ptr<ID3D11ComputeShader> densifyShader;
	winrt::com_ptr<ID3D11Buffer> paramsBuffer;
	std::array<GpuQuerySlot, GPU_QUERY_SLOT_COUNT> gpuQuerySlots;
	NvofTimingWindow gpuTimingWindow;
	FrameGuidanceExtent extent{};
	FrameGuidanceResetReason resetReason = FrameGuidanceResetReason::Initialize;
	uint32_t nextGpuQuerySlot = 0;
	uint32_t gridSize = 0;
	uint32_t previousSlot = 0;
	uint64_t gpuTimingSampleCount = 0;
	std::string_view profileLabel;
	bool bidirectional = false;
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 2> inputUav;
	winrt::com_ptr<ID3D11ShaderResourceView> hdrSourceSrv;
	ID3D11Texture2D* hdrSourceTexture = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> hdrToNvofShader;
	bool costEnabled = false;
	bool gpuTimingAvailable = false;
	bool historyValid = false;
	FrameGuidanceFrame previousFrame{};
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
	DXGI_FORMAT inputDxgiFormat = DXGI_FORMAT_UNKNOWN;
	NV_OF_BUFFER_FORMAT inputBufferFormat = NV_OF_BUFFER_FORMAT_ABGR8;
};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider(
	NvidiaOpticalFlowQuality quality
) : _quality(quality), _impl(std::make_unique<Impl>()) {}

NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;

bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources& resources,
	FrameGuidanceExtent sourceExtent
) noexcept {
	return _impl->CreateSession(resources, sourceExtent, _quality);
}

bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame& frame,
	MotionVectorProviderOutput& output
) noexcept {
	FrameTrace::Scope traceNvof(FrameTrace::Event::NvofSubmit, static_cast<int64_t>(_quality));
	Impl& impl = *_impl;
	output = {};
	if (!frame.color || frame.sourceExtent != impl.extent || !impl.session) {
		return false;
	}
	D3D11_TEXTURE2D_DESC frameDesc{};
	frame.color->GetDesc(&frameDesc);
	if (frameDesc.Width != impl.extent.width || frameDesc.Height != impl.extent.height ||
		(frameDesc.Format != impl.inputDxgiFormat &&
			!(ScalingWindow::Get().Options().IsHdrCompatibilityEnabled() &&
			 frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT))) {
		Logger::Get().Warn(fmt::format(
			"NVOF input format mismatch: frame={}x{} dxgi={}, session dxgi={}; "
			"supported contracts are ABGR8/GRAYSCALE8/NV12",
			frameDesc.Width, frameDesc.Height,
			static_cast<uint32_t>(frameDesc.Format),
			static_cast<uint32_t>(impl.inputDxgiFormat)));
		return false;
	}

	if (impl.historyValid && !frame.ContinuesMotionHistory(impl.previousFrame)) {
		impl.historyValid = false;
		impl.resetReason = FrameGuidanceResetReason::CaptureInterrupted;
	}
	const bool hadPrevious = impl.historyValid;
	const auto previousFrame = impl.previousFrame;
	const uint32_t currentSlot = impl.historyValid ? 1u - impl.previousSlot : 0u;
	if (frameDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
		if (impl.hdrSourceTexture != frame.color) {
			impl.hdrSourceSrv = nullptr;
			if (FAILED(impl.device->CreateShaderResourceView(
				frame.color, nullptr, impl.hdrSourceSrv.put()))) return false;
			impl.hdrSourceTexture = frame.color;
		}
		ID3D11ShaderResourceView* srv = impl.hdrSourceSrv.get();
		ID3D11UnorderedAccessView* uav = impl.inputUav[currentSlot].get();
		impl.context->CSSetShader(impl.hdrToNvofShader.get(), nullptr, 0);
		impl.context->CSSetShaderResources(0, 1, &srv);
		impl.context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		impl.context->Dispatch((impl.extent.width + 7) / 8,
			(impl.extent.height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		impl.context->CSSetShaderResources(0, 1, &nullSrv);
		impl.context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		impl.context->CSSetShader(nullptr, nullptr, 0);
	} else {
		impl.context->CopyResource(impl.input[currentSlot].get(), frame.color);
	}
	if (!impl.historyValid) {
		impl.ClearDenseOutput();
		impl.previousSlot = currentSlot;
		impl.historyValid = true;
	} else {
		NV_OF_EXECUTE_INPUT_PARAMS inputParams{
			.inputFrame = impl.registered[currentSlot],
			.referenceFrame = impl.registered[impl.previousSlot],
			.disableTemporalHints = impl.resetReason == FrameGuidanceResetReason::None ?
				NV_OF_FALSE : NV_OF_TRUE
		};
		NV_OF_EXECUTE_OUTPUT_PARAMS outputParams{
			.outputBuffer = impl.registered[2],
			.outputCostBuffer = impl.costEnabled ? impl.registered[4] : nullptr,
			.bwdOutputBuffer = impl.bidirectional ? impl.registered[3] : nullptr,
			.bwdOutputCostBuffer = impl.bidirectional && impl.costEnabled ?
				impl.registered[5] : nullptr
		};
		const auto begin = NativeBackendTiming::Now();
		Impl::GpuQuerySlot* gpuTiming = impl.BeginGpuTiming();
		const bool succeeded = impl.api.nvOFExecute(
			impl.session, &inputParams, &outputParams) == NV_OF_SUCCESS &&
			impl.Densify();
		impl.EndGpuTiming(gpuTiming);
		if (!succeeded) {
			impl.historyValid = false;
			impl.resetReason = FrameGuidanceResetReason::ProviderFailure;
			Logger::Get().Warn(fmt::format(
				"Frame Guidance NVOF failed at frameId={}", frame.frameId));
			return false;
		}
		impl.previousSlot = currentSlot;
		const auto elapsed = NativeBackendTiming::ElapsedMilliseconds(begin);
		if constexpr (NativeBackendTiming::Enabled) {
			if (frame.frameId <= 2) {
				Logger::Get().Info(fmt::format(
					"Frame Guidance NVOF submit+dense frameId={} CPU={:.3f} ms",
					frame.frameId, elapsed));
			}
		}
	}

	const bool isZero = impl.resetReason != FrameGuidanceResetReason::None;
	const FrameGuidanceMetadata metadata = MakeMetadata(
		frame, impl.resetReason, isZero);
	output.motion = {
		.texture = impl.motion.get(),
		.format = DXGI_FORMAT_R16G16_FLOAT,
		.metadata = metadata
	};
	output.confidence = {
		.texture = impl.confidence.get(),
		.format = DXGI_FORMAT_R8_UNORM,
		.metadata = metadata
	};
	if (hadPrevious && !isZero) {
		output.motionPair = {frame.frameId, previousFrame.frameId,
			frame.captureSequence, previousFrame.captureSequence, true};
		if (impl.bidirectional && impl.reverseMotion && impl.reverseConfidence) {
			auto reverseMetadata = metadata;
			reverseMetadata.validRegion = previousFrame.validRegion;
			output.reverseMotion = {impl.reverseMotion.get(), DXGI_FORMAT_R16G16_FLOAT, reverseMetadata};
			output.reverseConfidence = {impl.reverseConfidence.get(), DXGI_FORMAT_R8_UNORM, reverseMetadata};
		}
	}
	impl.previousFrame = frame;
	impl.resetReason = FrameGuidanceResetReason::None;
	return true;
}

void NvidiaOpticalFlowProvider::Reset(
	FrameGuidanceResetReason reason
) noexcept {
	_impl->historyValid = false;
	_impl->previousFrame = {};
	_impl->resetReason = reason;
}

bool NvidiaOpticalFlowProvider::Resize(
	FrameGuidanceExtent sourceExtent
) noexcept {
	if (!_impl->resources) return false;
	const bool result = _impl->CreateSession(
		*_impl->resources, sourceExtent, _quality);
	if (result) _impl->resetReason = FrameGuidanceResetReason::Resize;
	return result;
}

OpticalFlowInitializationError
NvidiaOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#else

namespace Magpie {

struct NvidiaOpticalFlowProvider::Impl {
	OpticalFlowInitializationError initializationError =
		OpticalFlowInitializationError::ProviderUnavailable;
};

NvidiaOpticalFlowProvider::NvidiaOpticalFlowProvider(
	NvidiaOpticalFlowQuality quality
) : _quality(quality), _impl(std::make_unique<Impl>()) {}
NvidiaOpticalFlowProvider::~NvidiaOpticalFlowProvider() = default;
bool NvidiaOpticalFlowProvider::Initialize(
	DeviceResources&, FrameGuidanceExtent) noexcept { return false; }
bool NvidiaOpticalFlowProvider::BeginFrame(
	const FrameGuidanceFrame&, MotionVectorProviderOutput&) noexcept { return false; }
void NvidiaOpticalFlowProvider::Reset(FrameGuidanceResetReason) noexcept {}
bool NvidiaOpticalFlowProvider::Resize(FrameGuidanceExtent) noexcept { return false; }
OpticalFlowInitializationError
NvidiaOpticalFlowProvider::InitializationError() const noexcept {
	return _impl->initializationError;
}

}

#endif
