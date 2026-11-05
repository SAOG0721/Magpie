#pragma once
#include "DLSSNRPerformanceParameters.h"
#include <algorithm>
#include <cstdint>

namespace Magpie {

// Accepted captures have a local ordinal: renderer frame IDs can contain gaps.
// GPU completion, redraws and interpolated display frames never advance it.
struct DLSSNRPerformanceState {
	uint64_t sequence = 0;
	uint64_t epoch = 1;
	uint64_t frame = 0;
	uint64_t revision = 0;
	uint64_t historyRevision = 0;
	int64_t timestamp100ns = 0;
	bool initialized = false;
	bool newCapture = false;
	bool reset = false;
	uint32_t updateInterval = 2;
	uint32_t maxAgeFrames = DLSSNR_RESIDUAL_MAX_AGE_FRAMES;

	void Configure(DLSSNRProcessingMode mode, uint32_t interval) noexcept {
		updateInterval = interval >= 2 && interval <= 4 ? interval : 2;
		// Pipeline tolerates two more capture transitions beyond the usual age1
		// result; publication/adoption never changes the original source ordinal.
		maxAgeFrames = mode == DLSSNRProcessingMode::Alternating ? updateInterval-1 :
			mode == DLSSNRProcessingMode::Pipeline ? 3u : DLSSNR_RESIDUAL_MAX_AGE_FRAMES;
	}

	void Begin(uint64_t id, uint64_t content, uint64_t history, int64_t timestamp,
		bool acceptedCapture, bool historyReset) noexcept {
		newCapture = !initialized || (acceptedCapture && id != frame);
		reset = !initialized || historyReset || history != historyRevision || id < frame ||
			(id == frame && content != revision) ||
			(timestamp > 0 && timestamp100ns > 0 && timestamp < timestamp100ns) ||
			(timestamp100ns && timestamp > timestamp100ns && timestamp - timestamp100ns > 5'000'000);
		if (reset) ++epoch;
		if (newCapture) ++sequence;
		frame = id; revision = content; historyRevision = history;
		timestamp100ns = timestamp; initialized = true;
	}

	void Invalidate() noexcept { ++epoch; }

	// The last successful complete-chain submission anchors the cadence. A hard
	// rebuild starts a fresh cycle; redraws and completion polls cannot spend it.
	bool NeedsFullUpdate(uint64_t origin, uint64_t sourceEpoch) const noexcept {
		return !origin || sourceEpoch != epoch || (newCapture && origin <= sequence &&
			sequence-origin >= updateInterval);
	}

	bool Usable(uint64_t origin, uint64_t sourceEpoch) const noexcept {
		return initialized && origin != 0 && sourceEpoch == epoch && origin <= sequence &&
			sequence - origin <= maxAgeFrames;
	}
};

// Completion polling may request continuation only for the snapshot whose
// output was already published. Accepting a new capture does not publish it.
struct DLSSNRPostOutputState {
	uint64_t sequence = 0, epoch = 0;
	void Published(const DLSSNRPerformanceState& source) noexcept {
		sequence = source.sequence; epoch = source.epoch;
	}
	bool CanSubmit(const DLSSNRPerformanceState& source, uint64_t latestSequence,
		uint64_t latestEpoch, bool waiting, bool pending) const noexcept {
		return waiting && !pending && source.initialized && sequence != 0 &&
			sequence == source.sequence && epoch == source.epoch &&
			latestSequence == sequence && latestEpoch == epoch;
	}
};

}
