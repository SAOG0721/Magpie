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

// The transport clock advances with displayed content targets; this separate
// clock advances only when a genuinely different NR observation is adopted.
// Current support retains its real source age as targets advance. Reuse and
// asynchronous confirmation never refresh the original contribution timestamp
// or its bounded support deadline.
struct DLSSNRSparseTemporalObservation {
	static constexpr int64_t MAX_IDLE_100NS = 2500000; // 250 ms without a new NR observation.
	uint64_t source = 0, epoch = 0, revision = 0;
	int64_t timestamp = 0;
	bool valid = false;

	bool Accepts(uint64_t target, uint64_t nextSource, uint64_t nextEpoch,
		uint64_t nextRevision, unsigned maxSourceAge) const noexcept {
		if (!nextSource || !nextEpoch || !nextRevision || nextSource > target || target-nextSource > maxSourceAge) return false;
		if (valid && (nextEpoch < epoch || (nextEpoch == epoch && nextRevision < revision))) return false;
		return true;
	}
	bool Expired(int64_t nextTimestamp) const noexcept {
		return valid && (nextTimestamp < timestamp ||
			uint64_t(nextTimestamp)-uint64_t(timestamp) > uint64_t(MAX_IDLE_100NS));
	}
	float Weight(uint64_t nextEpoch, uint64_t nextRevision, int64_t nextTimestamp,
		bool reset, float timeMs, int64_t nextObservationTimestamp = 0) const noexcept {
		if (!valid || reset || nextEpoch != epoch || Expired(nextTimestamp)) return 0;
		if (nextRevision == revision) return 1; // Transport, not another observation.
		const int64_t observationTime=nextObservationTimestamp>0?nextObservationTimestamp:nextTimestamp;
		if (nextRevision < revision || observationTime <= timestamp) return 0;
		const double seconds = double(observationTime-timestamp)*1e-7;
		return static_cast<float>(std::exp(-seconds/(NormalizeDLSSNRAntiFlickerTimeMs(timeMs)*.001)));
	}
	void Observe(uint64_t nextSource, uint64_t nextEpoch, uint64_t nextRevision,
		int64_t nextTimestamp) noexcept {
		if (!nextSource || !nextEpoch || !nextRevision ||
			(valid && (nextEpoch < epoch || (nextEpoch == epoch && nextRevision < revision)))) return;
		// This is the current real support source, independent of NR time.
		source=nextSource;
		if (!valid || nextEpoch != epoch || nextRevision != revision) {
			epoch=nextEpoch; revision=nextRevision;
			timestamp=nextTimestamp; valid=true;
		}
	}
};

// Successful output identity, independent of physical capture/transport flags.
// A held target must reuse its post-filtered pixels even though it is no
// longer marked isNewCaptureFrame on the next physical render opportunity.
struct DLSSNRSparseTemporalDrawIdentity {
	uint64_t frame = 0, inputRevision = 0, historyRevision = 0;
	uint64_t target = 0, source = 0, epoch = 0, observationRevision = 0, outputRevision = 0;
	uint64_t zeroGeneration = 0, motionGeneration = 0;
	uintptr_t input = 0, output = 0;
	uint32_t resetFlags = 0;
	bool operator==(const DLSSNRSparseTemporalDrawIdentity&) const noexcept = default;
	bool ReusesPixelsFrom(const DLSSNRSparseTemporalDrawIdentity& previous) const noexcept {
		auto identity = previous;
		identity.resetFlags = resetFlags;
		// A consumed pulse may deassert on hold; only a newly asserted reset
		// changes the cached output. The caller records the deassertion too.
		return *this == identity && (resetFlags & ~previous.resetFlags) == 0;
	}
};
}
