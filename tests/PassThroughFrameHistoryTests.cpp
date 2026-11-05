// Execute the real selection/publication functions with CPU texture tokens.
// The doubles verify copy ownership and HDR identity without creating a device.
#include "PassThroughFrameHistory.h"
#include "HdrFrame.h"
#include "FrameTrace.h"
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Magpie {
struct CpuReferenceTexture { uint64_t pixels = 0; };
struct CpuReferenceHandle {
	std::shared_ptr<CpuReferenceTexture> value;
	CpuReferenceTexture* get() const noexcept { return value.get(); }
	explicit operator bool() const noexcept { return bool(value); }
};
struct CpuReferenceContext {
	std::vector<uint64_t> copiedPixels;
	CpuReferenceTexture* input = nullptr;
	CpuReferenceTexture* output = nullptr;
	uint32_t dispatches = 0;
	void CopyResource(CpuReferenceTexture* destination, CpuReferenceTexture* source) {
		assert(destination && source);
		copiedPixels.push_back(source->pixels);
		destination->pixels = source->pixels;
	}
	void ClearState() noexcept { input = output = nullptr; }
	void CSSetShader(CpuReferenceTexture*, void*, uint32_t) noexcept {}
	void CSSetConstantBuffers(uint32_t, uint32_t, CpuReferenceTexture**) noexcept {}
	void CSSetShaderResources(uint32_t, uint32_t, CpuReferenceTexture** resources) noexcept { input = *resources; }
	void CSSetUnorderedAccessViews(uint32_t, uint32_t, CpuReferenceTexture** resources, void*) noexcept { output = *resources; }
	void CSSetSamplers(uint32_t, uint32_t, CpuReferenceTexture**) noexcept {}
	void Dispatch(uint32_t, uint32_t, uint32_t) noexcept {
		assert(input && output);
		output->pixels = input->pixels;
		++dispatches;
	}
};
struct CpuReferenceResources {
	CpuReferenceContext context;
	CpuReferenceContext* GetD3DDC() noexcept { return &context; }
};

class PassThroughFrames {
public:
	static constexpr uint32_t MAX_SLOTS = 4;
	struct Slot { CpuReferenceHandle texture; };
	struct Reference { CpuReferenceHandle texture; HdrFrameMetadata metadata{}; };
	bool SelectBackendFrame(uint64_t targetFrameId, uint64_t previousOutputFrameId) noexcept;
	void Publish(uint32_t slot, bool generatedFrame) noexcept;
	HdrFrameMetadata _ReferenceMetadata(const HdrFrameMetadata& source, bool canonical) const noexcept;
	void _UpdateBackend(uint64_t captureFrameId, bool newCapture, const HdrFrameMetadata* sourceMetadata) noexcept;

	explicit PassThroughFrames(uint32_t capacity = 0) {
		assert(_captureHistory.Reset(capacity));
		const auto texture = [] { return CpuReferenceHandle{std::make_shared<CpuReferenceTexture>()}; };
		_current = texture(); _previous = texture();
		_inputView = texture(); _outputView = _current;
		_shader = texture(); _sampler = texture();
		for (auto& slot : _backendSlots) slot.texture = texture();
		if (capacity) {
			for (uint32_t i = 0; i < capacity; ++i) _captureReferences[i].texture = texture();
			_selectedTarget.texture = texture(); _selectedPrevious.texture = texture();
		}
	}
	void Capture(uint64_t frame) {
		if (_currentValid) {
			_resources.context.CopyResource(_previous.get(), _current.get());
			_previousFrameId = _currentFrameId; _previousMetadata = _currentMetadata; _previousValid = true;
		}
		_current.value->pixels = frame;
		_currentFrameId = frame;
		_currentMetadata = {};
		_currentMetadata.frameId = frame;
		_currentMetadata.captureSequence = ++_accepted;
		_currentMetadata.timestamp100ns = int64_t(frame * 100);
		_currentMetadata.resourceGeneration = 7;
		_currentMetadata.color.sdrWhiteNits = 80.f + float(frame);
		_currentMetadata.color.primaries = HdrColorPrimaries::Rec2020;
		_currentMetadata.routeId = "source-" + std::to_string(frame);
		_currentMetadata.stage = HdrFrameStage::CanonicalInput;
		_currentMetadata.valid = true;
		_currentValid = true;
		const auto slot = _captureHistory.Capture(frame);
		if (slot != PassThroughFrameHistory::Missing) {
			_resources.context.CopyResource(_captureReferences[slot].texture.get(), _current.get());
			_captureReferences[slot].metadata = _currentMetadata;
		}
	}
	void ExpectPublished(uint32_t slot, uint64_t frame, bool generated = false) const {
		assert(_valid[slot] && _frameIds[slot] == frame);
		assert(_backendSlots[slot].texture.get()->pixels == frame);
		const auto& metadata = _metadata[slot];
		assert(metadata.frameId == frame && metadata.timestamp100ns == int64_t(frame * 100));
		assert(metadata.color.sdrWhiteNits == 80.f + float(frame));
		assert(metadata.color.primaries == HdrColorPrimaries::Rec2020 && metadata.resourceGeneration == 7);
		assert(metadata.routeId == "source-" + std::to_string(frame));
		assert(metadata.stage == HdrFrameStage::PublishedOutput && metadata.generated == generated);
	}
	void ExpectUnavailable(uint32_t slot) const {
		assert(!_valid[slot] && _frameIds[slot] == 0 && _metadata[slot].frameId == 0);
	}

	CpuReferenceResources _resources;
	CpuReferenceResources* _backendResources = &_resources;
	std::array<Slot, MAX_SLOTS> _backendSlots;
	std::array<uint64_t, MAX_SLOTS> _frameIds{};
	std::array<HdrFrameMetadata, MAX_SLOTS> _metadata{};
	std::array<bool, MAX_SLOTS> _valid{};
	bool _sharingEnabled = true;
	CpuReferenceHandle _current, _previous;
	CpuReferenceHandle _inputView, _outputView, _shader, _sampler, _constants;
	uint32_t _width = 32, _height = 16;
	uint64_t _currentFrameId = 0, _previousFrameId = 0, _accepted = 0;
	HdrFrameMetadata _currentMetadata{}, _previousMetadata{};
	bool _currentValid = false, _previousValid = false;
	PassThroughFrameHistory _captureHistory;
	std::array<Reference, PassThroughFrameHistory::MaxCapacity> _captureReferences;
	Reference _selectedTarget, _selectedPrevious;
	bool _selectedTargetValid = false, _selectedPreviousValid = false;
};

#include "PassThroughFrameSelectionUnderTest.h"
}

int main() {
	using namespace Magpie;
	PassThroughFrameHistory bounds;
	assert(bounds.Capture(1) == bounds.Missing);
	assert(!bounds.Select(1,0).targetAvailable);
	assert(bounds.Reset(8) && bounds.Capacity() == 8);
	assert(!bounds.Reset(9) && bounds.Capacity() == 0);
	assert(bounds.Reset(2));
	assert(bounds.Capture(0) == bounds.Missing);
	assert(bounds.Capture(10) == 0 && bounds.Capture(30) == 1);
	assert(bounds.Capture(30) == 1 && bounds.Capture(70) == 0);
	assert(bounds.CaptureFrameId(0) == 70 && bounds.CaptureFrameId(1) == 30);
	assert(bounds.CaptureFrameId(8) == 0);
	{
		// Resize/reconfiguration recreates raw-reference resources while the
		// physical capture can remain stationary. Exercise the actual worker with
		// newCapture=false: it must seed the retained existing identity once.
		PassThroughFrames resized(8);
		resized._currentMetadata.sourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		resized._currentMetadata.color.transfer = HdrTransferFunction::SRGB;
		resized._currentMetadata.color.sdrWhiteNits = 203;
		resized._currentMetadata.valid = true;
		HdrFrameMetadata source;
		source.frameId = 73; source.previousFrameId = 51;
		source.captureSequence = 19; source.resourceGeneration = 11;
		source.timestamp100ns = 12345678; source.captureAdmissionTime100ns = 23456789;
		const auto reference = resized._ReferenceMetadata(source,false);
		resized._inputView.value->pixels = 73;
		resized._UpdateBackend(73,false,&reference);
		assert(resized._currentValid && resized._currentFrameId == 73);
		assert(resized._resources.context.dispatches == 1 && resized._accepted == 0);
		assert(resized._captureHistory.CaptureFrameId(0) == 73);
		assert(resized.SelectBackendFrame(73,0));
		resized.Publish(0,false);
		assert(resized._valid[0] && resized._frameIds[0] == 73 && resized._backendSlots[0].texture.get()->pixels == 73);
		assert(resized._metadata[0].captureSequence == 19 && resized._metadata[0].resourceGeneration == 11);
		assert(resized._metadata[0].timestamp100ns == 12345678 && resized._metadata[0].captureAdmissionTime100ns == 23456789);
		assert(resized._metadata[0].sourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM &&
			resized._metadata[0].color.transfer == HdrTransferFunction::SRGB && resized._metadata[0].color.sdrWhiteNits == 203);
		auto duplicate = reference;
		duplicate.captureSequence = 88; duplicate.timestamp100ns = 99999999;
		resized._UpdateBackend(73,false,&duplicate);
		assert(resized._resources.context.dispatches == 1 && resized._accepted == 0);
		assert(resized.SelectBackendFrame(73,0));
		resized.Publish(0,false);
		assert(resized._frameIds[0] == 73 && resized._metadata[0].captureSequence == 19 &&
			resized._metadata[0].timestamp100ns == 12345678);
	}
	{
		PassThroughFrames metadataOwner(8);
		auto& configured = metadataOwner._currentMetadata;
		configured.width = 2560; configured.height = 1440;
		configured.sourceFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
		configured.color.primaries = HdrColorPrimaries::Rec709;
		configured.color.transfer = HdrTransferFunction::SRGB;
		configured.color.sdrWhiteNits = 203;
		configured.routeId = "initialized-sdr-comparison";
		configured.stage = HdrFrameStage::CanonicalOutput;
		configured.valid = true;
		HdrFrameMetadata identityOnly;
		identityOnly.frameId = 73; identityOnly.previousFrameId = 51;
		identityOnly.captureSequence = 19; identityOnly.resourceGeneration = 11;
		identityOnly.timestamp100ns = 12345678; identityOnly.captureAdmissionTime100ns = 23456789;
		identityOnly.parameterRevision = 4; identityOnly.outputHistoryRevision = 8;
		const auto sdr = metadataOwner._ReferenceMetadata(identityOnly,false);
		assert(sdr.frameId == 73 && sdr.previousFrameId == 51 && sdr.captureSequence == 19);
		assert(sdr.resourceGeneration == 11 && sdr.timestamp100ns == 12345678 &&
			sdr.captureAdmissionTime100ns == 23456789 && sdr.parameterRevision == 4 && sdr.outputHistoryRevision == 8);
		assert(sdr.sourceFormat == configured.sourceFormat && sdr.color == configured.color);
		assert(sdr.width == 2560 && sdr.height == 1440 && sdr.routeId == configured.routeId && sdr.valid);
		assert(sdr.stage == configured.stage);
		const auto sdrSlot = metadataOwner._captureHistory.Capture(sdr.frameId);
		metadataOwner._captureReferences[sdrSlot].texture.value->pixels = sdr.frameId;
		metadataOwner._captureReferences[sdrSlot].metadata = sdr;
		assert(metadataOwner.SelectBackendFrame(73,0));
		for (uint64_t frame=100;frame<124;++frame) metadataOwner.Capture(frame);
		assert(metadataOwner.SelectBackendFrame(73,0));
		metadataOwner.Publish(0,false);
		assert(metadataOwner._frameIds[0] == 73 && metadataOwner._backendSlots[0].texture.get()->pixels == 73);
		assert(metadataOwner._metadata[0].captureSequence == 19 && metadataOwner._metadata[0].resourceGeneration == 11);
		assert(metadataOwner._metadata[0].timestamp100ns == 12345678 && metadataOwner._metadata[0].captureAdmissionTime100ns == 23456789);
		assert(metadataOwner._metadata[0].sourceFormat == DXGI_FORMAT_R8G8B8A8_UNORM &&
			metadataOwner._metadata[0].color == sdr.color);
		auto hdrSource = identityOnly;
		hdrSource.width = 1920; hdrSource.height = 1080;
		hdrSource.sourceFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		hdrSource.color.primaries = HdrColorPrimaries::Rec2020;
		hdrSource.color.transfer = HdrTransferFunction::Linear;
		hdrSource.color.sdrWhiteNits = 160;
		hdrSource.routeId = "canonical-hdr-source"; hdrSource.valid = true;
		const auto hdr = metadataOwner._ReferenceMetadata(hdrSource,true);
		assert(hdr.frameId == 73 && hdr.captureSequence == 19 && hdr.resourceGeneration == 11);
		assert(hdr.timestamp100ns == 12345678 && hdr.captureAdmissionTime100ns == 23456789);
		assert(hdr.sourceFormat == hdrSource.sourceFormat && hdr.color == hdrSource.color &&
			hdr.width == 1920 && hdr.height == 1080 && hdr.routeId == hdrSource.routeId);
		assert(hdr.stage == HdrFrameStage::CanonicalInput && hdr.valid);
	}
	{
		PassThroughFrames legacy;
		legacy.Capture(10);
		legacy.Publish(0,true); legacy.ExpectPublished(0,10,true);
		legacy.Capture(30);
		legacy.Publish(0,false); legacy.ExpectPublished(0,30);
		legacy.Publish(1,true); legacy.ExpectPublished(1,10,true);
		assert(!legacy.SelectBackendFrame(10,0));
		legacy.Publish(0,false); legacy.ExpectPublished(0,30);
		assert(!legacy._selectedTarget.texture && !legacy._captureReferences[0].texture);
	}
	PassThroughFrames buffered(8);
	buffered.Capture(10); buffered.Capture(30); buffered.Capture(70);
	assert(buffered.SelectBackendFrame(30,10));
	buffered.Publish(0,false); buffered.ExpectPublished(0,30);
	buffered.Publish(1,true); buffered.ExpectPublished(1,10,true);
	// A target may be held for arbitrarily more captures than ring capacity.
	for (uint64_t frame=100;frame<124;++frame) buffered.Capture(frame);
	assert(buffered.SelectBackendFrame(30,10));
	buffered.Publish(0,false); buffered.ExpectPublished(0,30);
	buffered.Publish(1,true); buffered.ExpectPublished(1,10,true);
	// Save old target 30 into the previous pin BEFORE overwriting target with 123.
	buffered._resources.context.copiedPixels.clear();
	assert(buffered.SelectBackendFrame(123,30));
	assert((buffered._resources.context.copiedPixels == std::vector<uint64_t>{30,123}));
	buffered.Publish(0,false); buffered.ExpectPublished(0,123);
	buffered.Publish(1,true); buffered.ExpectPublished(1,30,true);
	// Role swaps do not overwrite either payload or its HDR metadata.
	buffered._resources.context.copiedPixels.clear();
	assert(buffered.SelectBackendFrame(30,123));
	assert(buffered._resources.context.copiedPixels.empty());
	buffered.Publish(0,false); buffered.ExpectPublished(0,30);
	buffered.Publish(1,true); buffered.ExpectPublished(1,123,true);
	// A miss must clear an already valid publication slot, never substitute the
	// physical-now image 123 or mislabel a previously selected image as 999.
	assert(!buffered.SelectBackendFrame(999,123));
	buffered.Publish(0,false); buffered.ExpectUnavailable(0);
	buffered.Publish(1,true); buffered.ExpectPublished(1,123,true);
	assert(buffered.SelectBackendFrame(30,888));
	buffered.Publish(0,false); buffered.ExpectPublished(0,30);
	buffered.Publish(1,true); buffered.ExpectUnavailable(1);
	assert(buffered.SelectBackendFrame(30,0));
	buffered.Publish(1,true); buffered.ExpectUnavailable(1);
	buffered._sharingEnabled = false;
	buffered.Publish(0,false); buffered.ExpectUnavailable(0);
	{
		PassThroughFrames minimal(1);
		minimal.Capture(1); assert(minimal.SelectBackendFrame(1,0));
		for (uint64_t frame=2;frame<30;++frame) minimal.Capture(frame);
		assert(minimal.SelectBackendFrame(1,1));
		minimal.Publish(0,false); minimal.ExpectPublished(0,1);
		minimal.Publish(1,true); minimal.ExpectPublished(1,1,true);
	}
	std::cout << "Original-reference identities: legacy path, sparse IDs, bounded history, held pins, copy ordering, generated predecessor, misses and HDR metadata passed (CPU only).\n";
}
