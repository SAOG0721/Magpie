#include "pch.h"
#include "FrameGuidanceService.h"
#include "DeviceResources.h"
#include "Logger.h"
#include "DirectXHelper.h"
#include "ScalingWindow.h"
#include "AmdOpticalFlowProvider.h"
#include "NvidiaOpticalFlowProvider.h"

namespace Magpie {

namespace {

constexpr char ADAPT_GUIDANCE_HLSL[] = R"(
Texture2D<float2> SourceMotion : register(t0);
Texture2D<float> SourceDepth : register(t1);
Texture2D<float> SourceConfidence : register(t2);
Texture2D<float2> SourceReverseMotion : register(t3);
Texture2D<float> SourceReverseConfidence : register(t4);
RWTexture2D<float2> TargetMotion : register(u0);
RWTexture2D<float> TargetDepth : register(u1);
RWTexture2D<float> TargetConfidence : register(u2);
RWTexture2D<float2> TargetReverseMotion : register(u3);
RWTexture2D<float> TargetReverseConfidence : register(u4);

cbuffer Params : register(b0) {
    uint2 SourceExtent;
    uint2 TargetExtent;
    float2 MotionScale;
    uint HasReverse;
    uint Padding;
};

float2 SourcePosition(uint2 targetPixel) {
    return (float2(targetPixel) + 0.5) *
        (float2(SourceExtent) / float2(TargetExtent)) - 0.5;
}

int2 ClampSource(int2 pixel) {
    return clamp(pixel, int2(0, 0), int2(SourceExtent) - 1);
}

float4 LoadGuide(int2 pixel) {
    pixel = ClampSource(pixel);
    return float4(
        SourceMotion.Load(int3(pixel, 0)),
        SourceDepth.Load(int3(pixel, 0)),
        SourceConfidence.Load(int3(pixel, 0)));
}
float3 LoadReverse(int2 pixel) {
    pixel = ClampSource(pixel);
    return float3(SourceReverseMotion.Load(int3(pixel,0)), SourceReverseConfidence.Load(int3(pixel,0)));
}

[numthreads(8, 8, 1)]
void Adapt(uint3 tid : SV_DispatchThreadID) {
    if (any(tid.xy >= TargetExtent)) return;
    float2 sourcePosition = SourcePosition(tid.xy);
    int2 base = int2(floor(sourcePosition));
    float2 fraction = frac(sourcePosition);
    float4 top = lerp(LoadGuide(base), LoadGuide(base + int2(1, 0)), fraction.x);
    float4 bottom = lerp(
        LoadGuide(base + int2(0, 1)),
        LoadGuide(base + int2(1, 1)), fraction.x);
    float4 guide = lerp(top, bottom, fraction.y);
    TargetMotion[tid.xy] = guide.xy * MotionScale;
    TargetDepth[tid.xy] = guide.z;
    TargetConfidence[tid.xy] = saturate(guide.w);
    if (HasReverse != 0) {
        float3 topReverse = lerp(LoadReverse(base), LoadReverse(base+int2(1,0)), fraction.x);
        float3 bottomReverse = lerp(LoadReverse(base+int2(0,1)), LoadReverse(base+int2(1,1)), fraction.x);
        float3 reverse = lerp(topReverse, bottomReverse, fraction.y);
        TargetReverseMotion[tid.xy] = reverse.xy * MotionScale;
        TargetReverseConfidence[tid.xy] = saturate(reverse.z);
    }
}
)";

FrameGuidanceRegion ScaleRegion(
	FrameGuidanceRegion region,
	FrameGuidanceExtent source,
	FrameGuidanceExtent target
) noexcept {
	const auto scaleFloor = [](uint32_t value, uint32_t numerator,
		uint32_t denominator) noexcept {
		return static_cast<uint32_t>(
			(uint64_t(value) * numerator) / denominator);
	};
	const auto scaleCeil = [](uint32_t value, uint32_t numerator,
		uint32_t denominator) noexcept {
		return static_cast<uint32_t>(
			(uint64_t(value) * numerator + denominator - 1) / denominator);
	};
	const uint32_t left = std::min(
		scaleFloor(region.x, target.width, source.width), target.width);
	const uint32_t top = std::min(
		scaleFloor(region.y, target.height, source.height), target.height);
	const uint32_t right = std::min(
		scaleCeil(region.x + region.width, target.width, source.width),
		target.width);
	const uint32_t bottom = std::min(
		scaleCeil(region.y + region.height, target.height, source.height),
		target.height);
	return { left, top, right - left, bottom - top };
}

FrameGuidanceMetadata AdaptMetadata(
	const FrameGuidanceMetadata& source,
	FrameGuidanceExtent targetExtent,
	ID3D11Fence* fence,
	uint64_t fenceValue,
	bool forceReset = false
) noexcept {
	FrameGuidanceMetadata result = source;
	result.validRegion = ScaleRegion(
		source.validRegion, source.sourceExtent, targetExtent);
	result.sourceExtent = targetExtent;
	result.sync = { fence, fenceValue };
	result.requiresHistoryReset = source.requiresHistoryReset || forceReset;
	return result;
}

FrameGuidanceMetadata MakeTargetZeroMetadata(
	FrameGuidanceFrameId frameId,
	FrameGuidanceExtent extent,
	ID3D11Fence* fence,
	uint64_t fenceValue,
	FrameGuidanceResetReason reason,
	bool requiresHistoryReset
) noexcept {
	return {
		.frameId = frameId,
		.sourceExtent = extent,
		.validRegion = FrameGuidanceRegion::Full(extent),
		.sync = { fence, fenceValue },
		.resetReason = reason,
		.valid = true,
		.isZero = true,
		.requiresHistoryReset = requiresHistoryReset
	};
}

bool SameResource(const FrameGuidanceResource& a, const FrameGuidanceResource& b) noexcept {
	const auto& x = a.metadata; const auto& y = b.metadata;
	return a.texture == b.texture && a.format == b.format && x.frameId == y.frameId &&
		x.captureSequence == y.captureSequence && x.resourceGeneration == y.resourceGeneration &&
		x.timestamp100ns == y.timestamp100ns && x.sourceExtent == y.sourceExtent &&
		x.validRegion == y.validRegion && x.sync.fence == y.sync.fence && x.sync.value == y.sync.value &&
		x.resetReason == y.resetReason && x.valid == y.valid && x.isZero == y.isZero &&
		x.requiresHistoryReset == y.requiresHistoryReset;
}

bool SameView(const FrameGuidanceView& a, const FrameGuidanceView& b) noexcept {
	return SameResource(a.depth,b.depth) && SameResource(a.motion,b.motion) &&
		SameResource(a.confidence,b.confidence) && SameResource(a.reverseMotion,b.reverseMotion) &&
		SameResource(a.reverseConfidence,b.reverseConfidence) && a.motionPair == b.motionPair &&
		a.motionDirection == b.motionDirection && a.motionUnit == b.motionUnit &&
		a.requiresHistoryReset == b.requiresHistoryReset && a.motionAvailability == b.motionAvailability;
}

FrameGuidanceAvailability Availability(const FrameGuidanceView& view) noexcept {
	if (view.motionAvailability != FrameGuidanceAvailability::Unavailable) return view.motionAvailability;
	const auto& meta = view.motion.metadata;
	if (!view.motion.texture || !meta.valid) return FrameGuidanceAvailability::Unavailable;
	if (!meta.isZero) return FrameGuidanceAvailability::Valid;
	return FrameGuidanceAvailability::Unavailable;
}

bool HasFallbackProvenance(const FrameGuidanceView& view) noexcept {
	const auto value = Availability(view);
	return value != FrameGuidanceAvailability::Valid && value != FrameGuidanceAvailability::Unavailable;
}

}

struct FrameGuidanceService::AdapterCache {
	struct Entry {
		MotionVectorRequest request{};
		FrameGuidanceExtent extent{};
		std::array<winrt::com_ptr<ID3D11Texture2D>, 5> adaptedTextures;
		std::array<winrt::com_ptr<ID3D11Texture2D>, 3> zeroTextures;
		std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 5> adaptedUavs;
		std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 3> zeroUavs;
		std::array<ID3D11Texture2D*, 5> sourceTextures{};
		std::array<winrt::com_ptr<ID3D11ShaderResourceView>, 5> sourceSrvs;
		FrameGuidanceView sourceIdentity{}, zeroIdentity{};
		FrameGuidanceView produced{};
		FrameGuidanceView zero{};
		FrameGuidanceFrameId frameId = 0;
		bool hasFrame = false;
		bool fallbackActive = false;
	};

	bool Initialize(DeviceResources& resources) noexcept {
		device = resources.GetD3DDevice();
		context = resources.GetD3DDC();
		winrt::com_ptr<ID3DBlob> blob;
		if (!device || !context || !DirectXHelper::CompileComputeShader(
			ADAPT_GUIDANCE_HLSL, "Adapt", blob.put(),
			"FrameGuidance/AdaptToConsumer.hlsl") ||
			FAILED(device->CreateComputeShader(
				blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
				shader.put()))) {
			Logger::Get().Error("Create Frame Guidance consumer adapter shader failed");
			return false;
		}
		const D3D11_BUFFER_DESC bufferDesc{
			.ByteWidth = 32,
			.Usage = D3D11_USAGE_DYNAMIC,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER,
			.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE
		};
		if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, params.put())) ||
			FAILED(device->CreateFence(
				0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())))) {
			Logger::Get().Error("Create Frame Guidance consumer adapter resources failed");
			return false;
		}
		return true;
	}

	void Reset() noexcept {
		entries.clear();
	}

	Entry* GetOrCreate(
		FrameGuidanceExtent extent,
		MotionVectorRequest request
	) noexcept {
		for (const auto& entry : entries) {
			if (entry->extent == extent && entry->request == request) return entry.get();
		}
		auto entry = std::make_unique<Entry>();
		entry->extent = extent;
		entry->request = request;
		constexpr std::array<DXGI_FORMAT, 3> FORMATS{
			DXGI_FORMAT_R16G16_FLOAT,
			DXGI_FORMAT_R32_FLOAT,
			DXGI_FORMAT_R8_UNORM
		};
		constexpr UINT BIND_FLAGS =
			D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		constexpr std::array<std::string_view, 3> ADAPTED_ROLES{
			"Adapter/Motion", "Adapter/Depth", "Adapter/Confidence"
		};
		constexpr std::array<std::string_view, 3> ZERO_ROLES{
			"Adapter/Zero/Motion", "Adapter/Zero/Depth", "Adapter/Zero/Confidence"
		};
		for (size_t i = 0; i < FORMATS.size(); ++i) {
			entry->adaptedTextures[i] = DirectXHelper::CreateSharedTexture2D(
				device, FORMATS[i], extent.width, extent.height, BIND_FLAGS,
				ADAPTED_ROLES[i]);
			entry->zeroTextures[i] = DirectXHelper::CreateSharedTexture2D(
				device, FORMATS[i], extent.width, extent.height, BIND_FLAGS,
				ZERO_ROLES[i]);
			if (!entry->adaptedTextures[i] || !entry->zeroTextures[i] ||
				FAILED(device->CreateUnorderedAccessView(
					entry->adaptedTextures[i].get(), nullptr,
					entry->adaptedUavs[i].put())) ||
				FAILED(device->CreateUnorderedAccessView(
					entry->zeroTextures[i].get(), nullptr,
					entry->zeroUavs[i].put()))) {
				Logger::Get().Error(fmt::format(
					"Create Frame Guidance adapter textures failed for {}x{}",
					extent.width, extent.height));
				return nullptr;
			}
		}
		static constexpr float ZERO[4]{};
		static constexpr float ONE[4]{ 1.0f, 1.0f, 1.0f, 1.0f };
		const float* depthClear = (!ScalingWindow::Get().Options().hdrComponents.enabled && ScalingWindow::Get().Options().IsHdrCompatibilityEnabled())
			? ONE : ZERO;
		for (size_t i = 0; i < entry->zeroUavs.size(); ++i) {
			context->ClearUnorderedAccessViewFloat(
				entry->zeroUavs[i].get(), i == 1 ? depthClear : ZERO);
		}
		Entry* result = entry.get();
		entries.push_back(std::move(entry));
		Logger::Get().Info(fmt::format(
			"Frame Guidance consumer adapter created: target={}x{}",
			extent.width, extent.height));
		return result;
	}

	FrameGuidanceConsumerViews Adapt(
		const FrameGuidanceView& source,
		const FrameGuidanceView& sourceZero,
		FrameGuidanceFrameId frameId,
		FrameGuidanceExtent sourceExtent,
		FrameGuidanceExtent targetExtent,
		MotionVectorRequest request
	) noexcept {
		Entry* entry = GetOrCreate(targetExtent, request);
		if (!entry) return {};
		if (entry->hasFrame && entry->frameId == frameId &&
			SameView(entry->sourceIdentity,source) && SameView(entry->zeroIdentity,sourceZero)) {
			return { entry->produced, entry->zero, true, entry->fallbackActive ||
				HasFallbackProvenance(entry->produced) ||
				(request.method != OpticalFlowMethod::None && entry->produced.motion.metadata.isZero), Availability(entry->produced) };
		}

		const bool sourceValid = source.IsValidFor(frameId, sourceExtent);
		const bool sourceZeroValid = sourceZero.IsValidFor(frameId, sourceExtent);
		const bool allZero = sourceValid && source.depth.metadata.isZero &&
			source.motion.metadata.isZero && source.confidence.metadata.isZero;
		bool converted = sourceValid && sourceZeroValid;
		bool hasReverse = converted && source.HasReverseMotionForPair(frameId,source.motionPair.previousFrameId,sourceExtent);
		if (hasReverse) {
			for (size_t i = 3; i < 5; ++i) {
				if (entry->adaptedTextures[i] && entry->adaptedUavs[i]) continue;
				entry->adaptedTextures[i] = nullptr; entry->adaptedUavs[i] = nullptr;
				entry->adaptedTextures[i] = DirectXHelper::CreateSharedTexture2D(device,
					i == 3 ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_R8_UNORM,
					targetExtent.width,targetExtent.height,D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
					i == 3 ? "Adapter/ReverseMotion" : "Adapter/ReverseConfidence");
				if (!entry->adaptedTextures[i] || FAILED(device->CreateUnorderedAccessView(
					entry->adaptedTextures[i].get(),nullptr,entry->adaptedUavs[i].put()))) { hasReverse = false; break; }
			}
		}
		if (converted && !allZero) {
			for (const auto* resource : { &source.motion, &source.depth, &source.confidence }) {
				const auto sync = resource->metadata.sync;
				if (sync.fence && sync.value && FAILED(context->Wait(sync.fence, sync.value))) converted = false;
			}
			if (converted && hasReverse) for (const auto* resource : {&source.reverseMotion,&source.reverseConfidence}) {
				const auto sync = resource->metadata.sync;
				if (sync.fence && sync.value && FAILED(context->Wait(sync.fence,sync.value))) hasReverse = false;
			}
		}
		if (converted && !allZero) {
			const std::array<ID3D11Texture2D*, 5> sourceTextures{
				source.motion.texture, source.depth.texture, source.confidence.texture,
				hasReverse ? source.reverseMotion.texture : nullptr, hasReverse ? source.reverseConfidence.texture : nullptr
			};
			for (size_t i = 0; i < sourceTextures.size(); ++i) {
				if (!sourceTextures[i]) continue;
				if (entry->sourceTextures[i] == sourceTextures[i] &&
					entry->sourceSrvs[i]) continue;
				entry->sourceSrvs[i] = nullptr;
				if (FAILED(device->CreateShaderResourceView(
					sourceTextures[i], nullptr, entry->sourceSrvs[i].put()))) {
					if (i >= 3) hasReverse = false;
					else converted = false;
					break;
				}
				entry->sourceTextures[i] = sourceTextures[i];
			}
		}

		if (converted && !allZero) {
			struct Params {
				uint32_t sourceWidth, sourceHeight, targetWidth, targetHeight;
				float motionScaleX, motionScaleY; uint32_t hasReverse, padding;
			};
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(
				params.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				converted = false;
			} else {
				const Params values{
					sourceExtent.width, sourceExtent.height,
					targetExtent.width, targetExtent.height,
					float(targetExtent.width) / float(sourceExtent.width),
					float(targetExtent.height) / float(sourceExtent.height), hasReverse ? 1u : 0u, 0
				};
				std::memcpy(mapped.pData, &values, sizeof(values));
				context->Unmap(params.get(), 0);
				ID3D11ShaderResourceView* srvs[]{
					entry->sourceSrvs[0].get(), entry->sourceSrvs[1].get(),
					entry->sourceSrvs[2].get(), hasReverse ? entry->sourceSrvs[3].get() : nullptr,
					hasReverse ? entry->sourceSrvs[4].get() : nullptr
				};
				ID3D11UnorderedAccessView* uavs[]{
					entry->adaptedUavs[0].get(), entry->adaptedUavs[1].get(),
					entry->adaptedUavs[2].get(), entry->adaptedUavs[3].get(), entry->adaptedUavs[4].get()
				};
				ID3D11Buffer* buffers[]{ params.get() };
				context->CSSetShader(shader.get(), nullptr, 0);
				context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);
				context->CSSetUnorderedAccessViews(
					0, ARRAYSIZE(uavs), uavs, nullptr);
				context->CSSetConstantBuffers(0, 1, buffers);
				context->Dispatch(
					(targetExtent.width + 7) / 8,
					(targetExtent.height + 7) / 8, 1);
				ID3D11ShaderResourceView* nullSrvs[5]{};
				ID3D11UnorderedAccessView* nullUavs[5]{};
				context->CSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
				context->CSSetUnorderedAccessViews(
					0, ARRAYSIZE(nullUavs), nullUavs, nullptr);
			}
		}

		const bool wasFallback = entry->fallbackActive;
		const uint64_t readyValue = ++fenceValue;
		if (FAILED(context->Signal(fence.get(), readyValue))) {
			// Never publish a sync point that cannot be reached.
			entry->fallbackActive = true;
			entry->hasFrame = false;
			return {};
		}
		entry->fallbackActive = !converted;
		const bool fallbackTransition = wasFallback != entry->fallbackActive;
		auto adaptZeroMetadata = [&](const FrameGuidanceMetadata& metadata) noexcept {
			FrameGuidanceMetadata result = sourceZeroValid ?
				AdaptMetadata(metadata, targetExtent, fence.get(), readyValue,
					fallbackTransition) :
				MakeTargetZeroMetadata(
					frameId, targetExtent, fence.get(), readyValue,
					fallbackTransition ? FrameGuidanceResetReason::ProviderFailure :
						FrameGuidanceResetReason::None,
					fallbackTransition);
			if (fallbackTransition) {
				result.resetReason = FrameGuidanceResetReason::ProviderFailure;
			}
			return result;
		};
		const FrameGuidanceMetadata zeroDepthMetadata =
			adaptZeroMetadata(sourceZero.depth.metadata);
		const FrameGuidanceMetadata zeroMotionMetadata =
			adaptZeroMetadata(sourceZero.motion.metadata);
		const FrameGuidanceMetadata zeroConfidenceMetadata =
			adaptZeroMetadata(sourceZero.confidence.metadata);
		entry->zero = {
			.depth = {
				entry->zeroTextures[1].get(), DXGI_FORMAT_R32_FLOAT,
				zeroDepthMetadata },
			.motion = {
				entry->zeroTextures[0].get(), DXGI_FORMAT_R16G16_FLOAT,
				zeroMotionMetadata },
			.confidence = {
				entry->zeroTextures[2].get(), DXGI_FORMAT_R8_UNORM,
				zeroConfidenceMetadata },
			.requiresHistoryReset =
				zeroDepthMetadata.requiresHistoryReset ||
				zeroMotionMetadata.requiresHistoryReset ||
				zeroConfidenceMetadata.requiresHistoryReset
		};
		if (!converted) {
			entry->produced = entry->zero;
		} else {
			const bool recovered = fallbackTransition;
			// A provider's zero output still carries its own reset metadata.
			// Reusing the zero textures must not discard failure/recovery edges.
			const auto texture = [&](size_t i) noexcept {
				return allZero ? entry->zeroTextures[i].get() : entry->adaptedTextures[i].get();
			};
			entry->produced = {
				.depth = {
					texture(1), DXGI_FORMAT_R32_FLOAT,
					AdaptMetadata(source.depth.metadata, targetExtent,
						fence.get(), readyValue, recovered) },
				.motion = {
					texture(0), DXGI_FORMAT_R16G16_FLOAT,
					AdaptMetadata(source.motion.metadata, targetExtent,
						fence.get(), readyValue, recovered) },
				.confidence = {
					texture(2), DXGI_FORMAT_R8_UNORM,
					AdaptMetadata(source.confidence.metadata, targetExtent,
						fence.get(), readyValue, recovered) },
				.requiresHistoryReset = source.requiresHistoryReset || recovered
			};
			entry->produced.motionDirection = source.motionDirection;
			entry->produced.motionUnit = source.motionUnit;
			entry->produced.motionPair = source.motionPair;
			entry->produced.motionAvailability = Availability(source);
			if (hasReverse && !recovered) {
				entry->produced.reverseMotion = {entry->adaptedTextures[3].get(),DXGI_FORMAT_R16G16_FLOAT,
					AdaptMetadata(source.reverseMotion.metadata,targetExtent,fence.get(),readyValue)};
				entry->produced.reverseConfidence = {entry->adaptedTextures[4].get(),DXGI_FORMAT_R8_UNORM,
					AdaptMetadata(source.reverseConfidence.metadata,targetExtent,fence.get(),readyValue)};
			}
		}
		if (!converted) entry->produced.motionAvailability = FrameGuidanceAvailability::SynchronizationFailed;
		entry->sourceIdentity = source; entry->zeroIdentity = sourceZero;
		entry->frameId = frameId;
		entry->hasFrame = true;
		if (!converted && fallbackTransition) {
			Logger::Get().Warn(fmt::format(
				"Frame Guidance adaptation failed at frameId={} ({}x{} -> {}x{}); "
				"using target-size Zero guidance",
				frameId, sourceExtent.width, sourceExtent.height,
				targetExtent.width, targetExtent.height));
		}
		return { entry->produced, entry->zero, true, !converted ||
			HasFallbackProvenance(entry->produced) ||
			(request.method != OpticalFlowMethod::None && entry->produced.motion.metadata.isZero), Availability(entry->produced) };
	}

	ID3D11Device5* device = nullptr;
	ID3D11DeviceContext4* context = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> shader;
	winrt::com_ptr<ID3D11Buffer> params;
	winrt::com_ptr<ID3D11Fence> fence;
	uint64_t fenceValue = 0;
	std::vector<std::unique_ptr<Entry>> entries;
};

struct FrameGuidanceService::GpuTiming {
	struct Slot {
		winrt::com_ptr<ID3D11Query> disjoint, start, end;
		uint64_t generation = 0, sequence = 0;
		bool pending = false;
	};
	std::array<Slot, 8> slots;
	uint64_t generation = 1, sequence = 0, sample = 0;
	double milliseconds = 0;

	bool Initialize(ID3D11Device* device) noexcept {
		for (auto& slot : slots) {
			D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
			if (FAILED(device->CreateQuery(&desc, slot.disjoint.put()))) return false;
			desc.Query = D3D11_QUERY_TIMESTAMP;
			if (FAILED(device->CreateQuery(&desc, slot.start.put())) ||
				FAILED(device->CreateQuery(&desc, slot.end.put()))) return false;
		}
		return true;
	}
	void Reset() noexcept { ++generation; milliseconds = 0; sample = 0; }
	Slot* Begin(ID3D11DeviceContext* context) noexcept {
		for (auto& slot : slots) {
			if (!slot.pending) continue;
			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
			uint64_t start = 0, end = 0;
			if (context->GetData(slot.disjoint.get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				context->GetData(slot.start.get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
				context->GetData(slot.end.get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
			slot.pending = false;
			if (slot.generation == generation && slot.sequence > sample &&
				!disjoint.Disjoint && disjoint.Frequency && end >= start) {
				milliseconds = double(end - start) * 1000.0 / double(disjoint.Frequency);
				sample = slot.sequence;
			}
		}
		if (++sequence % 4 != 0) return nullptr;
		for (auto& slot : slots) {
			if (slot.pending) continue;
			slot.sequence = sequence; slot.generation = generation; slot.pending = true;
			context->Begin(slot.disjoint.get()); context->End(slot.start.get());
			return &slot;
		}
		return nullptr;
	}
	static void End(ID3D11DeviceContext* context, Slot* slot) noexcept {
		if (!slot) return;
		context->End(slot->end.get()); context->End(slot->disjoint.get());
	}
};

double FrameGuidanceService::LastGpuMilliseconds() const noexcept {
	return _gpuTiming ? _gpuTiming->milliseconds : 0;
}

uint64_t FrameGuidanceService::LastGpuSample() const noexcept {
	return _gpuTiming ? _gpuTiming->sample : 0;
}

static FrameGuidanceExtent GetTextureExtent(ID3D11Texture2D* texture) noexcept {
	if (!texture) {
		return {};
	}
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	return { desc.Width, desc.Height };
}

static std::string_view ResetReasonName(
	FrameGuidanceResetReason reason
) noexcept {
	switch (reason) {
	case FrameGuidanceResetReason::None: return "None";
	case FrameGuidanceResetReason::Initialize: return "Initialize";
	case FrameGuidanceResetReason::Resize: return "Resize";
	case FrameGuidanceResetReason::SceneChange: return "SceneChange";
	case FrameGuidanceResetReason::CaptureInterrupted: return "CaptureInterrupted";
	case FrameGuidanceResetReason::DeviceRecreated: return "DeviceRecreated";
	case FrameGuidanceResetReason::LongPause: return "LongPause";
	case FrameGuidanceResetReason::ProviderFailure: return "ProviderFailure";
	default: return "Unknown";
	}
}

FrameGuidanceService::FrameGuidanceService() noexcept :
	_zeroDepthProvider(_zeroResources),
	_zeroMotionProvider(_zeroResources) {}

FrameGuidanceService::~FrameGuidanceService() = default;

void FrameGuidanceService::_RollbackInitialization() noexcept {
	_adapterCache.reset();
	_gpuTiming.reset();
	_providers.clear();
	_selectedMotionRequest = {};
	_preparedMotionRequest = {};
	_zeroResources.Reset();
	_resources = nullptr;
	_sourceExtent = {};
	_view = {};
	_zeroView = {};
	_cachedRequirements = {};
	_cachedFrameId = 0;
	_hasCachedFrame = false;
	_hasLoggedRequirements = false;
}

bool FrameGuidanceService::SetMotionVectorProvider(
	MotionVectorRequest request, std::unique_ptr<IMotionVectorProvider> provider
) noexcept {
	if (IsInitialized() || !provider || request.method == OpticalFlowMethod::None) return false;
	for (const auto& entry : _providers) if (entry.request == request) return false;
	_providers.push_back({ request, std::move(provider) });
	return true;
}

bool FrameGuidanceService::Initialize(
	DeviceResources& resources,
	ID3D11Texture2D* sourceFrame,
	const FrameGuidanceRequirements& requirements
) noexcept {
	_selectedMotionRequest = requirements.PreferredMotion();
	_initializationFailedMethod = OpticalFlowMethod::None;
	_initializationError = OpticalFlowInitializationError::None;
	_resources = &resources;
	_sourceExtent = GetTextureExtent(sourceFrame);
	if (!_sourceExtent.IsValid() ||
		!_zeroDepthProvider.Initialize(resources, _sourceExtent) ||
		!_zeroMotionProvider.Initialize(resources, _sourceExtent)) {
		Logger::Get().Error("Initialize Frame Guidance zero providers failed");
		_RollbackInitialization();
		return false;
	}
	bool initialized = true;
	requirements.ForEachMotion([&](MotionVectorRequest request) {
		if (!initialized) return;
		if (PrepareMotionRequest(request)) return;
		auto it = std::ranges::find(_providers, request, &ProviderEntry::request);
		initialized = false;
		_initializationFailedMethod = request.method;
		if (_initializationError == OpticalFlowInitializationError::None)
			_initializationError = it != _providers.end() && it->provider ? it->provider->InitializationError() :
				OpticalFlowInitializationError::ProviderUnavailable;
		Logger::Get().Error(fmt::format("Frame Guidance initialization failed: method={} quality={}",
			uint32_t(request.method), request.quality));
	});
	if (!initialized) {
		_RollbackInitialization();
		return false;
	}
	if (requirements.HasMotion()) {
		_gpuTiming = std::make_unique<GpuTiming>();
		if (!_gpuTiming->Initialize(resources.GetD3DDevice())) {
			_gpuTiming.reset();
			Logger::Get().Warn("Frame Guidance GPU timing queries unavailable");
		}
	}

	// The frame-source output is only an allocation at this point; capture has
	// not started yet, so its contents are undefined. Keep every provider in its
	// Initialize reset state and let the first real capture frame seed history.
	_hasCachedFrame = false;
	_view = {};
	_zeroView = {};
	return true;
}

bool FrameGuidanceService::PrepareMotionRequest(MotionVectorRequest request) noexcept {
	if (request.method == OpticalFlowMethod::None) return true;
	if (!_resources || !_sourceExtent.IsValid()) return false;
	FrameGuidanceRequirements check;
	check.Add(request);
	if (!check.Contains(request)) return false;
	auto it = std::ranges::find(_providers, request, &ProviderEntry::request);
	if (it != _providers.end() && it->ready) {
		_preparedMotionRequest = request;
		return true;
	}
	ProviderEntry candidate{.request = request};
#ifdef MP_ENABLE_NVIDIA_OPTICAL_FLOW
	if (request.method == OpticalFlowMethod::Nvidia)
		candidate.provider = std::make_unique<NvidiaOpticalFlowProvider>(static_cast<NvidiaOpticalFlowQuality>(request.quality));
#endif
#ifdef MP_ENABLE_AMD_OPTICAL_FLOW
	if (request.method == OpticalFlowMethod::Amd)
		candidate.provider = std::make_unique<AmdOpticalFlowProvider>(static_cast<AmdOpticalFlowMode>(request.quality));
#endif
	// Keep injected providers available to the existing tests and callers.
	if (it != _providers.end() && it->provider) candidate.provider = std::move(it->provider);
	if (!candidate.provider) return false;
	if (!candidate.provider->Initialize(*_resources, _sourceExtent)) {
		_initializationFailedMethod = request.method;
		_initializationError = candidate.provider->InitializationError();
		// Failed candidate creation does not retire a currently usable provider.
		return false;
	}
	candidate.ready = true;
	if (it != _providers.end()) *it = std::move(candidate);
	else _providers.push_back(std::move(candidate));
	_preparedMotionRequest = request;
	if (_hasCachedFrame) std::erase_if(_providers, [&](const ProviderEntry& entry) {
		return !_cachedRequirements.Contains(entry.request) && entry.request != request;
	});
	Logger::Get().Info(fmt::format("Frame Guidance estimator prepared: method={} quality={} capture={}x{}",
		uint32_t(request.method), request.quality, _sourceExtent.width, _sourceExtent.height));
	return true;
}

const FrameGuidanceView& FrameGuidanceService::BeginFrame(
	FrameGuidanceFrameId frameId,
	ID3D11Texture2D* sourceFrame,
	const FrameGuidanceRequirements& requirements,
	uint64_t captureSequence,
	uint64_t resourceGeneration,
	int64_t timestamp100ns,
	const ColorDescription& colorDescription
) noexcept {
	const FrameGuidanceExtent extent = GetTextureExtent(sourceFrame);
	if (_hasCachedFrame && _cachedFrameId == frameId && extent == _sourceExtent &&
		requirements == _cachedRequirements) {
		return _view;
	}
	if (extent != _sourceExtent) {
		if (!Resize(extent, requirements)) {
			return _view;
		}
	}
	if (requirements != _cachedRequirements) {
		bool ready = true;
		requirements.ForEachMotion([&](auto request) {
			if (!PrepareMotionRequest(request)) ready = false;
		});
		if (!ready) { _view = {}; _hasCachedFrame = false; return _view; }
		_selectedMotionRequest = requirements.PreferredMotion();
		// Retain current consumers plus at most one prepared candidate, rather
		// than accumulating one estimator and adapter for every resolution used.
		std::erase_if(_providers, [&](const ProviderEntry& entry) {
			return !requirements.Contains(entry.request) && entry.request != _preparedMotionRequest;
		});
		if (_adapterCache) _adapterCache->Reset();
		if (_gpuTiming) _gpuTiming->Reset();
	}
	return _Produce({
		.color = sourceFrame,
		.frameId = frameId,
		.captureSequence = captureSequence,
		.resourceGeneration = resourceGeneration,
		.timestamp100ns = timestamp100ns,
		.colorDescription = colorDescription,
		.sourceExtent = _sourceExtent,
		.validRegion = FrameGuidanceRegion::Full(_sourceExtent)
	}, requirements);
}

bool FrameGuidanceService::Resize(
	FrameGuidanceExtent sourceExtent,
	const FrameGuidanceRequirements& requirements
) noexcept {
	if (!sourceExtent.IsValid() || !_resources) {
		return false;
	}
	if (!_zeroDepthProvider.Resize(sourceExtent) ||
		!_zeroMotionProvider.Resize(sourceExtent)) {
		Logger::Get().Error("Resize Frame Guidance zero providers failed");
		return false;
	}
	for (auto& entry : _providers) {
		if (requirements.Contains(entry.request) && entry.ready &&
			!entry.provider->Resize(sourceExtent)) {
			entry.ready = false;
			_zeroMotionProvider.Reset(FrameGuidanceResetReason::ProviderFailure);
			Logger::Get().Warn(fmt::format("Frame Guidance resize failed: method={} quality={}; using Zero Motion",
				uint32_t(entry.request.method), entry.request.quality));
		}
		entry.view = {};
	}

	_sourceExtent = sourceExtent;
	if (_adapterCache) _adapterCache->Reset();
	if (_gpuTiming) _gpuTiming->Reset();
	_hasCachedFrame = false;
	_view = {};
	_zeroView = {};
	return true;
}

FrameGuidanceConsumerViews FrameGuidanceService::GetConsumerViews(
	FrameGuidanceFrameId frameId,
	FrameGuidanceExtent targetExtent,
	MotionVectorRequest request
) noexcept {
	// Every enabled consumer uses the session's selected provider. Disabled
	// consumers still get Zero guidance, regardless of other effects' requests.
	request = FrameGuidanceRequirements::ResolveConsumer(request, _selectedMotionRequest);
	const auto it = std::ranges::find(_providers, request, &ProviderEntry::request);
	const FrameGuidanceView& requestedView = it != _providers.end() ? it->view : _zeroView;
	// Preserve provider failure provenance even if adaptation/coherence fails.
	const auto finish = [&](FrameGuidanceConsumerViews result) noexcept {
		if (request.method == OpticalFlowMethod::None || it == _providers.end()) {
			result.motionAvailability = FrameGuidanceAvailability::Unavailable;
			result.produced.motionAvailability = FrameGuidanceAvailability::Unavailable;
			return result;
		}
		if (!it->ready) result.motionAvailability = FrameGuidanceAvailability::ProviderStopped;
		else if (it->consecutiveFailures) result.motionAvailability = FrameGuidanceAvailability::ProviderFailed;
		else if (result.produced.IsValidFor(frameId, targetExtent)) {
			const auto& motion = result.produced.motion.metadata;
			if (!motion.isZero) result.motionAvailability = FrameGuidanceAvailability::Valid;
			else if (motion.requiresHistoryReset && motion.resetReason != FrameGuidanceResetReason::ProviderFailure)
				result.motionAvailability = FrameGuidanceAvailability::ResetOnly;
		}
		result.produced.motionAvailability = result.motionAvailability;
		return result;
	};
	if (!targetExtent.IsValid() ||
		!requestedView.IsValidFor(frameId, _sourceExtent) ||
		!_zeroView.IsValidFor(frameId, _sourceExtent)) {
		return finish({});
	}
	if (targetExtent == _sourceExtent) {
		return finish({ requestedView, _zeroView, false,
			request.method != OpticalFlowMethod::None &&
			requestedView.motion.metadata.isZero });
	}
	if (!_adapterCache) {
		_adapterCache = std::make_unique<AdapterCache>();
		if (!_resources || !_adapterCache->Initialize(*_resources)) {
			_adapterCache.reset();
			Logger::Get().Warn(
				"Frame Guidance consumer-size adaptation is unavailable");
			return finish({});
		}
	}
	return finish(_adapterCache->Adapt(
		requestedView, _zeroView, frameId, _sourceExtent, targetExtent,
		request));
}

FrameGuidanceConsumerViews FrameGuidanceService::AdaptConsumerViews(
	const FrameGuidanceView& produced, const FrameGuidanceView& zero,
	FrameGuidanceExtent targetExtent) noexcept {
	const auto frameId = zero.depth.metadata.frameId;
	const auto sourceExtent = zero.depth.metadata.sourceExtent;
	if (!targetExtent.IsValid() || !produced.IsValidFor(frameId,sourceExtent) ||
		!zero.IsValidFor(frameId,sourceExtent)) return {};
	const auto availability = Availability(produced);
	if (targetExtent == sourceExtent) {
		return {produced,zero,false,availability != FrameGuidanceAvailability::Valid &&
			availability != FrameGuidanceAvailability::Unavailable,availability};
	}
	if (!_adapterCache) {
		if (!_resources) return {};
		auto cache = std::make_unique<AdapterCache>();
		if (!cache->Initialize(*_resources)) return {};
		_adapterCache = std::move(cache);
	}
	// This view is already the selected, retained provider result. Do not resolve
	// a request against the current physical frame's provider or source extent.
	return _adapterCache->Adapt(produced,zero,frameId,sourceExtent,targetExtent,{});
}

void FrameGuidanceService::ResetHistory(
	FrameGuidanceResetReason reason
) noexcept {
	Logger::Get().Info(fmt::format(
		"Frame Guidance history reset: reason={}", ResetReasonName(reason)));
	_zeroDepthProvider.Reset(reason);
	_zeroMotionProvider.Reset(reason);
	for (auto& entry : _providers) {
		if (entry.provider) entry.provider->Reset(reason);
		entry.view = {};
	}
	_view = {};
	if (_adapterCache) _adapterCache->Reset();
	if (_gpuTiming) _gpuTiming->Reset();
	_hasCachedFrame = false;
}

const FrameGuidanceView& FrameGuidanceService::_Produce(
	const FrameGuidanceFrame& frame,
	const FrameGuidanceRequirements& requirements
) noexcept {
	auto* dc = _resources->GetD3DDC();
	auto* timing = _gpuTiming ? _gpuTiming->Begin(dc) : nullptr;
	auto endTiming = wil::scope_exit([&] { GpuTiming::End(dc, timing); });
	if (!_hasLoggedRequirements || requirements != _lastLoggedRequirements) {
		Logger::Get().Info(fmt::format("Frame Guidance selected shared provider: method={} quality={} instances={}",
			uint32_t(_selectedMotionRequest.method), _selectedMotionRequest.quality, _providers.size()));
		_lastLoggedRequirements = requirements;
		_hasLoggedRequirements = true;
	}
	DepthProviderOutput zeroDepth;
	MotionVectorProviderOutput zeroMotion;
	if (!_zeroDepthProvider.BeginFrame(frame, zeroDepth) ||
		!_zeroMotionProvider.BeginFrame(frame, zeroMotion) ||
		!zeroDepth.depth.IsValid(
			DXGI_FORMAT_R32_FLOAT, frame.frameId, frame.sourceExtent) ||
		!zeroMotion.motion.IsValid(
			DXGI_FORMAT_R16G16_FLOAT, frame.frameId, frame.sourceExtent) ||
		!zeroMotion.confidence.IsValid(
			DXGI_FORMAT_R8_UNORM, frame.frameId, frame.sourceExtent)) {
		Logger::Get().Error("Produce zero Frame Guidance failed");
		_view = {};
		_zeroView = {};
		_hasCachedFrame = false;
		return _view;
	}
	_zeroView = {
		.depth = zeroDepth.depth,
		.motion = zeroMotion.motion,
		.confidence = zeroMotion.confidence,
		.requiresHistoryReset =
			zeroDepth.depth.metadata.requiresHistoryReset ||
			zeroMotion.motion.metadata.requiresHistoryReset
	};

	const auto produceMethod = [&](const FrameGuidanceFrame& providerFrame,
		const FrameGuidanceView& providerZero, IMotionVectorProvider* provider, bool& ready,
		bool& fallbackActive, uint32_t& consecutiveFailures, bool requested,
		std::string_view methodName) noexcept {
		if (!requested) return providerZero;

		MotionVectorProviderOutput motion;
		const bool attempted = ready && provider;
		const bool valid = attempted && provider->BeginFrame(providerFrame, motion) &&
			motion.motion.IsValid(
				DXGI_FORMAT_R16G16_FLOAT, providerFrame.frameId, providerFrame.sourceExtent) &&
			motion.confidence.IsValid(
				DXGI_FORMAT_R8_UNORM, providerFrame.frameId, providerFrame.sourceExtent) &&
			motion.motion.metadata.validRegion ==
				motion.confidence.metadata.validRegion &&
			motion.motion.metadata.validRegion == providerZero.depth.metadata.validRegion;
		const bool transition = fallbackActive != !valid;
		fallbackActive = !valid;
		if (!valid) {
			motion = {providerZero.motion, providerZero.confidence};
			if (attempted && ++consecutiveFailures >= 3) {
				ready = false;
				Logger::Get().Warn(fmt::format(
					"Frame Guidance {} stopped after {} consecutive failures; "
					"using Zero Motion for this scaling session",
					methodName, consecutiveFailures));
			}
		} else {
			consecutiveFailures = 0;
		}
		if (transition) {
			for (auto* resource : { &motion.motion, &motion.confidence, &motion.reverseMotion, &motion.reverseConfidence }) {
				resource->metadata.resetReason = FrameGuidanceResetReason::ProviderFailure;
				resource->metadata.requiresHistoryReset = true;
			}
		}

		FrameGuidanceView result{
			.depth = providerZero.depth,
			.motion = motion.motion,
			.confidence = motion.confidence,
			.requiresHistoryReset =
				providerZero.depth.metadata.requiresHistoryReset ||
				motion.motion.metadata.requiresHistoryReset ||
				motion.confidence.metadata.requiresHistoryReset
		};
		result.reverseMotion = motion.reverseMotion;
		result.reverseConfidence = motion.reverseConfidence;
		result.motionPair = motion.motionPair;
		result.motionAvailability = !valid ? (ready ? FrameGuidanceAvailability::ProviderFailed : FrameGuidanceAvailability::ProviderStopped) :
			(!motion.motion.metadata.isZero ? FrameGuidanceAvailability::Valid :
			(motion.motion.metadata.requiresHistoryReset ? FrameGuidanceAvailability::ResetOnly : FrameGuidanceAvailability::Unavailable));
		if (!result.IsValidFor(providerFrame.frameId, providerFrame.sourceExtent)) {
			Logger::Get().Warn(fmt::format(
				"Frame Guidance {} coherence failure at frameId={}; using Zero",
				methodName, providerFrame.frameId));
			result = providerZero;
			result.requiresHistoryReset = true;
			result.motionAvailability = FrameGuidanceAvailability::ProviderFailed;
		}
		return result;
	};

	for (auto& entry : _providers) {
		if (!requirements.Contains(entry.request)) continue;
		entry.view = produceMethod(frame, _zeroView, entry.provider.get(), entry.ready, entry.fallbackActive,
			entry.consecutiveFailures, true,
			entry.request.method == OpticalFlowMethod::Nvidia ? "NVOF" : "AMD OF");
	}
	const auto primary = std::ranges::find(_providers, requirements.FirstMotion(), &ProviderEntry::request);
	_view = primary == _providers.end() ? _zeroView : primary->view;
	_cachedFrameId = frame.frameId;
	_cachedRequirements = requirements;
	_hasCachedFrame = true;
	return _view;
}

}
