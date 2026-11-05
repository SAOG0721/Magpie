#include "pch.h"
#include "FrameTrace.h"
#include "PassThroughFrames.h"
#include "DeviceResources.h"
#include "DirectXHelper.h"
#include "Logger.h"

namespace Magpie {

namespace {
constexpr char REFERENCE_LDR_HLSL[] = R"(
Texture2D<float4> Input : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);
[numthreads(8, 8, 1)]
void Reference(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    Output[id.xy] = float4(Input.SampleLevel(LinearClamp, uv, 0).rgb, 1.0);
}
)";

constexpr char REFERENCE_HDR_HLSL[] = R"(
cbuffer Transform : register(b0) {
    uint hdrEnabled;
    float exposure;
    float sdrWhiteNits;
    float shoulder;
};
Texture2D<float4> Input : register(t0);
SamplerState LinearClamp : register(s0);
RWTexture2D<float4> Output : register(u0);

[numthreads(8, 8, 1)]
void Reference(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    Output.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);
    float3 rgb = Input.SampleLevel(LinearClamp, uv, 0).rgb;
    // Canonical HDR and presentation both use linear scRGB.
    Output[id.xy] = float4(rgb, 1.0);
}
)";
}

void PassThroughFrames::_ClearBackend() noexcept {
	_sharingEnabled = false;
	for (auto& slot : _backendSlots) slot = {};
	_handles.fill(nullptr);
	_valid.fill(false);
	_frameIds.fill(0);
	_metadata.fill({});
	_current = nullptr;
	_previous = nullptr;
	_inputView = nullptr;
	_outputView = nullptr;
	_shader = nullptr;
	_sampler = nullptr;
	_constants = nullptr;
	_hdrEnabled = false;
	_hdrParameters = {};
	_currentValid = false;
	_previousValid = false;
	_currentMetadata = {};
	_previousMetadata = {};
	_captureHistory.Reset();
	for (auto& reference : _captureReferences) reference = {};
	_selectedTarget = {};
	_selectedPrevious = {};
	_selectedTargetValid = _selectedPreviousValid = false;
	_baseMetadata = {};
	_presentedMetadata = {};
}

bool PassThroughFrames::InitializeBackend(DeviceResources& resources,
	ID3D11Texture2D* input, ID3D11Texture2D* output, uint32_t slotCount,
	bool hdrEnabled, const HdrTransformParameters& hdrParameters,
	const HdrFrameMetadata& frameMetadata, std::optional<bool> sourceHdr) noexcept {
	const bool inputHdr = sourceHdr.value_or(hdrEnabled);
	_ClearBackend();
	_backendResources = &resources;
	_hdrEnabled = hdrEnabled;
	_hdrParameters = hdrParameters.IsValid() ? hdrParameters : HdrTransformParameters{};
	_currentMetadata = frameMetadata;
	if (!input || !output || slotCount == 0 || slotCount > MAX_SLOTS) return false;
	D3D11_TEXTURE2D_DESC outputDesc{};
	output->GetDesc(&outputDesc);
	_width = outputDesc.Width;
	_height = outputDesc.Height;
	const DXGI_FORMAT referenceFormat = hdrEnabled
		? DXGI_FORMAT_R16G16B16A16_FLOAT
		: DXGI_FORMAT_R8G8B8A8_UNORM;
	auto device = resources.GetD3DDevice();
	auto fail = [&]() {
		Logger::Get().Warn("Pass-through reference resources unavailable; effects remain enabled");
		_ClearBackend();
		return false;
	};
	_current = DirectXHelper::CreateTexture2D(device, referenceFormat,
		_width, _height, D3D11_BIND_UNORDERED_ACCESS);
	if (!_current) return fail();
	if (slotCount > 1) {
		_previous = DirectXHelper::CreateTexture2D(device, referenceFormat,
			_width, _height, 0);
		if (!_previous) return fail();
	}
	HRESULT hr = device->CreateShaderResourceView(input, nullptr, _inputView.put());
	if (SUCCEEDED(hr)) hr = device->CreateUnorderedAccessView(_current.get(), nullptr, _outputView.put());
	if (FAILED(hr)) return fail();
	winrt::com_ptr<ID3DBlob> blob;
	std::string referenceShader = hdrEnabled ? REFERENCE_HDR_HLSL : REFERENCE_LDR_HLSL;
	// Comparison must enter the same output domain even when conversion effects
	// change it. Preserve source appearance using a display mapping, not the AI.
	if (hdrEnabled && !inputHdr) {
		const std::string marker = "Output[id.xy] = float4(rgb, 1.0);";
		const auto position = referenceShader.find(marker);
		referenceShader.replace(position, marker.size(),
			"rgb = lerp(pow(max((rgb + 0.055) / 1.055, 0.0), 2.4), rgb / 12.92, step(rgb, 0.04045));"
			"Output[id.xy] = float4(rgb * (sdrWhiteNits / 80.0), 1.0);");
	} else if (!hdrEnabled && inputHdr) {
		referenceShader = REFERENCE_HDR_HLSL;
		const std::string marker = "Output[id.xy] = float4(rgb, 1.0);";
		const auto position = referenceShader.find(marker);
		referenceShader.replace(position, marker.size(),
			"rgb = max(rgb, 0.0) / (sdrWhiteNits / 80.0);"
			"rgb = saturate(rgb / (1.0 + dot(rgb, float3(0.2126, 0.7152, 0.0722))));"
			"rgb = lerp(1.055 * pow(rgb, 1.0 / 2.4) - 0.055, 12.92 * rgb, step(rgb, 0.0031308));"
			"Output[id.xy] = float4(rgb, 1.0);");
	}
	if (!DirectXHelper::CompileComputeShader(
		referenceShader.c_str(),
		"Reference", blob.put(),
		"PassThroughReference", nullptr, {}, true)) return fail();
	hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, _shader.put());
	if (FAILED(hr)) return fail();
	if (hdrEnabled || inputHdr) {
		struct ReferenceConstants {
			uint32_t hdrEnabled;
			float exposure;
			float sdrWhiteNits;
			float shoulder;
		} constants{
			1u,
			_hdrParameters.exposure,
			_hdrParameters.sdrWhiteNits,
			_hdrParameters.shoulder
		};
		D3D11_BUFFER_DESC constantsDesc{
			.ByteWidth = sizeof(constants),
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_CONSTANT_BUFFER
		};
		D3D11_SUBRESOURCE_DATA constantsData{ .pSysMem = &constants };
		hr = device->CreateBuffer(&constantsDesc, &constantsData, _constants.put());
		if (FAILED(hr)) return fail();
	}
	D3D11_SAMPLER_DESC samplerDesc{};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	hr = device->CreateSamplerState(&samplerDesc, _sampler.put());
	if (FAILED(hr)) return fail();
	for (uint32_t i = 0; i < slotCount; ++i) {
		auto& slot = _backendSlots[i];
		slot.texture = DirectXHelper::CreateTexture2D(device, referenceFormat,
			_width, _height, D3D11_BIND_SHADER_RESOURCE, D3D11_USAGE_DEFAULT,
			D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX);
		if (!slot.texture) return fail();
		slot.mutex = slot.texture.try_as<IDXGIKeyedMutex>();
		auto dxgi = slot.texture.try_as<IDXGIResource>();
		if (!slot.mutex || !dxgi || FAILED(dxgi->GetSharedHandle(&_handles[i]))) return fail();
	}
	_sharingEnabled = true;
	return true;
}

bool PassThroughFrames::EnableCaptureHistory(uint32_t capacity) noexcept {
	const auto fail = [&]() {
		_ClearBackend();
		Logger::Get().Warn("Capture-aligned original comparison resources unavailable; effects remain enabled");
		return false;
	};
	if (!_sharingEnabled || !_backendResources || !_current || capacity == 0 ||
		capacity > PassThroughFrameHistory::MaxCapacity) return fail();
	D3D11_TEXTURE2D_DESC desc{};
	_current->GetDesc(&desc);
	const auto create = [&]() {
		return DirectXHelper::CreateTexture2D(_backendResources->GetD3DDevice(),
			desc.Format, _width, _height, 0);
	};
	for (auto& reference : _captureReferences) reference = {};
	for (uint32_t i = 0; i < capacity; ++i) {
		_captureReferences[i].texture = create();
		if (!_captureReferences[i].texture) return fail();
	}
	_selectedTarget = {};
	_selectedPrevious = {};
	_selectedTarget.texture = create();
	_selectedPrevious.texture = create();
	if (!_selectedTarget.texture || !_selectedPrevious.texture) return fail();
	_captureHistory.Reset(capacity);
	_selectedTargetValid = _selectedPreviousValid = false;
	// Generated frames now select the previous output target explicitly; a
	// physical-previous image is unnecessary once capture history is enabled.
	_previous = nullptr;
	_previousValid = false;
	_previousFrameId = 0;
	_previousMetadata = {};
	return true;
}

bool PassThroughFrames::OpenFrontend(DeviceResources& resources, uint32_t slotCount) noexcept {
	_frontendResources = &resources;
	for (auto& slot : _frontendSlots) slot = {};
	_base = nullptr;
	_presented = nullptr;
	_baseValid = false;
	_presentedValid = false;
	_allocationFailed = false;
	_baseFrameId = _presentedFrameId = 0;
	_baseMetadata = _presentedMetadata = {};
	if (!_handles[0]) return true; // Optional backend resource creation failed.
	auto fail = [&]() {
		_sharingEnabled = false;
		for (auto& slot : _frontendSlots) slot = {};
		return false;
	};
	for (uint32_t i = 0; i < slotCount; ++i) {
		auto& slot = _frontendSlots[i];
		const HRESULT hr = resources.GetD3DDevice()->OpenSharedResource(
			_handles[i], IID_PPV_ARGS(slot.texture.put()));
		if (FAILED(hr)) {
			Logger::Get().ComError("Open pass-through shared texture failed", hr);
			return fail();
		}
		slot.mutex = slot.texture.try_as<IDXGIKeyedMutex>();
		if (!slot.mutex) return fail();
	}
	return true;
}

void PassThroughFrames::UpdateBackend(uint64_t captureFrameId, bool newCapture) noexcept {
	_UpdateBackend(captureFrameId, newCapture, nullptr);
}

void PassThroughFrames::_UpdateBackend(uint64_t captureFrameId, bool newCapture,
	const HdrFrameMetadata* sourceMetadata) noexcept {
	FrameTrace::Scope traceReference(FrameTrace::Event::ReferencePrepare);
	if (!_sharingEnabled || !_current || !_shader || (!newCapture && _currentValid)) return;
	auto context = _backendResources->GetD3DDC();
	if (newCapture && _previous && _currentValid) {
		context->CopyResource(_previous.get(), _current.get());
		_previousFrameId = _currentFrameId;
		_previousMetadata = _currentMetadata;
		_previousValid = true;
	}
	context->ClearState();
	auto input = _inputView.get();
	auto output = _outputView.get();
	auto sampler = _sampler.get();
	context->CSSetShader(_shader.get(), nullptr, 0);
	if (_constants) {
		auto constants = _constants.get();
		context->CSSetConstantBuffers(0, 1, &constants);
	}
	context->CSSetShaderResources(0, 1, &input);
	context->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
	context->CSSetSamplers(0, 1, &sampler);
	context->Dispatch((_width + 7) / 8, (_height + 7) / 8, 1);
	context->ClearState();
	_currentFrameId = captureFrameId;
	if (sourceMetadata) _currentMetadata = *sourceMetadata;
	_currentMetadata.frameId = captureFrameId;
	_currentValid = true;
	// A resize refresh can materialize an already accepted capture for the
	// first time. Retain that real identity without inventing a new capture;
	// subsequent same-capture refreshes have already returned above.
	const auto slot = _captureHistory.Capture(captureFrameId);
	if (slot != PassThroughFrameHistory::Missing) {
		auto& reference = _captureReferences[slot];
		context->CopyResource(reference.texture.get(), _current.get());
		reference.metadata = _currentMetadata;
	}
}

HdrFrameMetadata PassThroughFrames::_ReferenceMetadata(const HdrFrameMetadata& source, bool canonical) const noexcept {
	if (canonical) {
		auto result = source;
		result.stage = HdrFrameStage::CanonicalInput;
		return result;
	}
	// SDR callers may have only capture identities, with no canonical HDR
	// texture or color description. Keep the initialized comparison domain.
	auto result = _currentMetadata;
	result.frameId = source.frameId;
	result.previousFrameId = source.previousFrameId;
	result.captureSequence = source.captureSequence;
	result.resourceGeneration = source.resourceGeneration;
	result.timestamp100ns = source.timestamp100ns;
	result.captureAdmissionTime100ns = source.captureAdmissionTime100ns;
	result.parameterRevision = source.parameterRevision;
	result.outputHistoryRevision = source.outputHistoryRevision;
	result.contentRedraw = source.contentRedraw;
	result.updateReason = source.updateReason;
	result.generated = source.generated;
	result.frameIndex = source.frameIndex;
	result.frameMultiplier = source.frameMultiplier;
	return result;
}

void PassThroughFrames::UpdateBackend(const HdrFrame& frame, bool newCapture) noexcept {
	const bool canonical = _hdrEnabled && frame.IsCanonical();
	const auto metadata = _ReferenceMetadata(frame.metadata, canonical);
	_UpdateBackend(frame.metadata.frameId, newCapture, &metadata);
	if (!_captureHistory.Capacity() && canonical) {
		// Preserve legacy metadata refresh for same-capture redraws. Buffered
		// references retain the metadata captured with their immutable pixels.
		_currentMetadata = metadata;
	}
}

bool PassThroughFrames::SelectBackendFrame(uint64_t targetFrameId, uint64_t previousOutputFrameId) noexcept {
	if (!_sharingEnabled || !_captureHistory.Capacity()) return false;
	const auto selected = _captureHistory.Select(targetFrameId, previousOutputFrameId);
	if (selected.swapSelected) std::swap(_selectedTarget, _selectedPrevious);
	const auto copy = [&](Reference& destination, const Reference& source) {
		_backendResources->GetD3DDC()->CopyResource(destination.texture.get(), source.texture.get());
		destination.metadata = source.metadata;
	};
	if (selected.previousFromTarget) copy(_selectedPrevious, _selectedTarget);
	else if (selected.previousCaptureSlot != PassThroughFrameHistory::Missing)
		copy(_selectedPrevious, _captureReferences[selected.previousCaptureSlot]);
	if (selected.targetCaptureSlot != PassThroughFrameHistory::Missing)
		copy(_selectedTarget, _captureReferences[selected.targetCaptureSlot]);
	_selectedTargetValid = selected.targetAvailable;
	_selectedPreviousValid = selected.previousAvailable;
	return _selectedTargetValid;
}

void PassThroughFrames::Publish(uint32_t slot, bool generatedFrame) noexcept {
	FrameTrace::Scope traceReference(FrameTrace::Event::ReferencePublish);
	if (slot >= MAX_SLOTS) return;
	_valid[slot] = false;
	_frameIds[slot] = 0;
	if (!_sharingEnabled || !_backendSlots[slot].texture || !_currentValid) {
		_metadata[slot] = {};
		return;
	}
	if (_captureHistory.Capacity()) {
		if (generatedFrame ? !_selectedPreviousValid : !_selectedTargetValid) {
			_metadata[slot] = {};
			return;
		}
		const auto& reference = generatedFrame ? _selectedPrevious : _selectedTarget;
		_backendResources->GetD3DDC()->CopyResource(_backendSlots[slot].texture.get(), reference.texture.get());
		_frameIds[slot] = reference.metadata.frameId;
		_metadata[slot] = reference.metadata;
		_metadata[slot].stage = HdrFrameStage::PublishedOutput;
		_metadata[slot].generated = generatedFrame;
		_valid[slot] = true;
		return;
	}
	const bool previous = generatedFrame && _previousValid;
	_backendResources->GetD3DDC()->CopyResource(_backendSlots[slot].texture.get(),
		previous ? _previous.get() : _current.get());
	_frameIds[slot] = previous ? _previousFrameId : _currentFrameId;
	_metadata[slot] = previous ? _previousMetadata : _currentMetadata;
	_metadata[slot].stage = HdrFrameStage::PublishedOutput;
	_metadata[slot].generated = generatedFrame;
	_valid[slot] = true;
}

bool PassThroughFrames::Consume(uint32_t slot) noexcept {
	FrameTrace::Scope traceReference(FrameTrace::Event::ReferenceConsume);
	_baseValid = false;
	if (!_sharingEnabled || _allocationFailed || !_frontendSlots[slot].texture || !_valid[slot]) return false;
	if (!_base || !_presented) {
		D3D11_TEXTURE2D_DESC desc{};
		_frontendSlots[slot].texture->GetDesc(&desc);
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		_base = nullptr;
		_presented = nullptr;
		auto device = _frontendResources->GetD3DDevice();
		HRESULT hr = device->CreateTexture2D(&desc, nullptr, _base.put());
		if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, _presented.put());
		if (FAILED(hr)) {
			_allocationFailed = true;
			Logger::Get().ComError("Create stable pass-through texture failed", hr);
			return false;
		}
	}
	_frontendResources->GetD3DDC()->CopyResource(_base.get(), _frontendSlots[slot].texture.get());
	_baseFrameId = _frameIds[slot];
	_baseMetadata = _metadata[slot];
	_baseValid = true;
	return true;
}

void PassThroughFrames::OnPresented() noexcept {
	FrameTrace::Scope traceReference(FrameTrace::Event::ReferencePresented);
	_presentedValid = _baseValid && _presented;
	if (_presentedValid) {
		_frontendResources->GetD3DDC()->CopyResource(_presented.get(), _base.get());
		_presentedFrameId = _baseFrameId;
		_presentedMetadata = _baseMetadata;
		_presentedMetadata.stage = HdrFrameStage::PresentedOutput;
	} else {
		_presentedFrameId = 0;
		_presentedMetadata = {};
	}
}

}
