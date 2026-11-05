#pragma once
#include <cstdint>
#include <string_view>

namespace Magpie {

// Resource addresses are identities, never a substitute for a content version.
struct EffectFrameResource {
	uintptr_t texture = 0;
	uint32_t width = 0, height = 0, format = 0;
	bool operator==(const EffectFrameResource&) const noexcept = default;
};

struct EffectFrameKey {
	uint64_t frameId = 0;
	uint64_t inputRevision = 0;
	uint64_t inputHistoryRevision = 0;
	uint64_t parameterRevision = 0;
	uint64_t captureSequence = 0;
	uint64_t resourceGeneration = 0;
	EffectFrameResource input{}, output{};
	bool operator==(const EffectFrameKey&) const noexcept = default;
};

inline bool UsesCaptureFrameClock(std::string_view name) noexcept {
	return name == "OKLabImageAdjustment" || name == "SMAA\\SMAA_T2x_Experimental" ||
		name == "SMAA\\SMAA_T2x_NoJitter_Experimental" ||
		name == "SMAA\\SMAA_4x_Experimental" ||
		name == "SMAA\\SMAA_4x_NoJitter_Experimental";
}

// Owned by Renderer's backend thread. A successful draw publishes a new output
// version; reuse leaves it untouched. History epochs change on discontinuities,
// not on every ordinary new input (which would disable temporal accumulation).
class EffectFrameState {
public:
	uint64_t ParameterRevision() const noexcept { return _parameterRevision; }
	uint64_t OutputRevision() const noexcept { return _outputRevision; }
	uint64_t OutputHistoryRevision() const noexcept { return _outputHistoryRevision; }
	void ParametersChanged() noexcept { ++_parameterRevision; }
	void Invalidate() noexcept { _valid = false; }
	bool NeedsDraw(const EffectFrameKey& key, bool renderClock) const noexcept {
		return renderClock || !_valid || key != _key;
	}
	bool RequiresHistoryReset(const EffectFrameKey& key) const noexcept {
		return !_valid || key.inputHistoryRevision != _key.inputHistoryRevision ||
			key.captureSequence != _key.captureSequence ||
			key.resourceGeneration != _key.resourceGeneration ||
			key.input != _key.input || key.output != _key.output ||
			(key.frameId == _key.frameId && key.inputRevision != _key.inputRevision);
	}
	bool ParametersDiffer(const EffectFrameKey& key) const noexcept {
		return key.parameterRevision != _key.parameterRevision;
	}
	void Commit(const EffectFrameKey& key, bool succeeded,
		uint64_t nativeContentVersion = 0, uint64_t nativeHistoryVersion = 0) noexcept {
		const bool boundaryReset = RequiresHistoryReset(key) || ParametersDiffer(key);
		if (nativeContentVersion) {
			// A buffered producer can consume a new input epoch while still
			// presenting the preceding target. Only its published target may
			// advance downstream history; the physical key still controls Draw.
			const bool outputReset = !_valid || !_nativeOutputTracked || key.output != _key.output;
			if (outputReset || nativeHistoryVersion != _nativeOutputHistoryVersion) ++_outputHistoryRevision;
			if (outputReset || !succeeded || nativeContentVersion != _nativeOutputVersion) ++_outputRevision;
		} else {
			if (boundaryReset) ++_outputHistoryRevision;
			++_outputRevision;
		}
		_nativeOutputTracked = nativeContentVersion != 0;
		_nativeOutputVersion = nativeContentVersion;
		_nativeOutputHistoryVersion = nativeHistoryVersion;
		_key = key;
		_valid = succeeded;
	}
private:
	EffectFrameKey _key{};
	uint64_t _parameterRevision = 0, _outputRevision = 0, _outputHistoryRevision = 0;
	uint64_t _nativeOutputVersion = 0, _nativeOutputHistoryVersion = 0;
	bool _nativeOutputTracked = false;
	bool _valid = false;
};

}
