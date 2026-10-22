#pragma once
#include "DLSSNRParameters.h"
#include <cmath>
#include <cstdint>

namespace Magpie {
// Capture timestamps, not presentation frequency, determine the EMA duration.
struct DLSSNRTemporalState {
	uint64_t frame = 0, revision = 0, generation = 0;
	int64_t timestamp = 0;
	bool valid = false;
	bool Duplicate(uint64_t nextFrame, uint64_t nextRevision) const noexcept {
		return valid && frame == nextFrame && revision == nextRevision;
	}
	bool MotionPairMatches(uint64_t nextFrame, uint64_t previousCaptureFrame) const noexcept {
		return !valid || nextFrame == frame || (previousCaptureFrame
			? previousCaptureFrame == frame : nextFrame == frame + 1);
	}
	float Weight(uint64_t nextFrame, uint64_t nextRevision, uint64_t nextGeneration,
		int64_t nextTimestamp, bool reset,
		float timeMs = DLSSNR_ANTI_FLICKER_TIME_DEFAULT_MS) const noexcept {
		if (!valid || reset || nextFrame <= frame || nextRevision != revision ||
			nextGeneration != generation || nextTimestamp <= timestamp) return 0;
		const double seconds = double(nextTimestamp - timestamp) * 1e-7;
		if (seconds > 0.25) return 0;
		return static_cast<float>(std::exp(-seconds / (NormalizeDLSSNRAntiFlickerTimeMs(timeMs) * 0.001)));
	}
	void Commit(uint64_t nextFrame, uint64_t nextRevision, uint64_t nextGeneration,
		int64_t nextTimestamp) noexcept {
		frame = nextFrame; revision = nextRevision; generation = nextGeneration;
		timestamp = nextTimestamp; valid = true;
	}
};
}
