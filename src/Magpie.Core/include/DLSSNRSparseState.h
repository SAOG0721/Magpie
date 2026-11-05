#pragma once
#include <algorithm>
#include <cstdint>

namespace Magpie {

// Accepted captures, not frame IDs, display refreshes, or wall time, pay for
// sparse work. A rare burst advances the next ordinary deadline as well.
struct DLSSNRSparseBudget {
	uint64_t lastSource = 0, refillAt = 0, currentCapture = 0, lastAdmission = 0;
	unsigned interval = 2;
	bool burstAvailable = true;
	void Advance(uint64_t capture) noexcept {
		currentCapture = capture;
		if (!burstAvailable && capture >= refillAt) burstAvailable = true;
	}
	bool CanSubmit(uint64_t source, bool urgent) const noexcept {
		if (!source || source <= lastSource) return false;
		return !lastSource || currentCapture-lastAdmission >= interval || (urgent && burstAvailable);
	}
	bool Submit(uint64_t source, bool urgent) noexcept {
		if (!CanSubmit(source, urgent)) return false;
		const bool burst = lastSource && currentCapture-lastAdmission < interval;
		lastSource = source; lastAdmission = currentCapture;
		if (burst) { burstAvailable = false; refillAt = currentCapture+2*interval; }
		return burst;
	}
	// A parameter or hard-epoch refresh may revisit the same capture. It still
	// pays at the current admission time and cannot rewind the source clock.
	void RecordRefresh(uint64_t source) noexcept {
		lastSource = (std::max)(lastSource, source);
		lastAdmission = currentCapture;
	}
};

// A result gets one observation only after the GPU confirms that its mapped
// correction actually supported output pixels. Each result owns one bounded
// asynchronous sample; unsupported output can try again on a later target.
struct DLSSNRSparseObservationClock {
	struct Result {
		bool observed = false;
		uint64_t source = 0, epoch = 0, version = 0, target = 0, ready = 0;
		uint64_t deferredTarget = 0;
		int64_t timestamp = 0, deferredTimestamp = 0;
		bool NeedsRetry(uint64_t contributionTarget) const noexcept {
			return !observed && !ready && deferredTarget > target && deferredTarget == contributionTarget;
		}
		void Cancel() noexcept {
			ready = 0; deferredTarget = 0; deferredTimestamp = 0;
		}
		bool Begin(uint64_t resultSource, uint64_t resultEpoch, uint64_t resultVersion,
			uint64_t contributionTarget, uint64_t completion, int64_t contributionTimestamp = 0) noexcept {
			if (observed || !resultSource || !resultEpoch || !resultVersion || !completion ||
				contributionTarget < resultSource) return false;
			const bool sameResult = source == resultSource && epoch == resultEpoch && version == resultVersion;
			if (ready) {
				// The first sample may be unsupported while a later output already
				// has support. Retain one latest target for a bounded retry after a
				// zero confirmation, including when capture stops on that target.
				if (sameResult && contributionTarget > target && contributionTarget > deferredTarget) {
					deferredTarget = contributionTarget; deferredTimestamp = contributionTimestamp;
				}
				return false;
			}
			if (sameResult && target == contributionTarget) return false;
			source = resultSource; epoch = resultEpoch; version = resultVersion;
			target = contributionTarget; ready = completion; timestamp = contributionTimestamp;
			deferredTarget = 0; deferredTimestamp = 0;
			return true;
		}
	};
	uint64_t revision = 0, observedEpoch = 0;
	int64_t timestamp = 0;
	bool Confirm(Result& result, uint64_t source, uint64_t epoch, uint64_t version,
		uint64_t activeEpoch, bool contributes) noexcept {
		if (!result.ready) return false;
		result.ready = 0;
		if (result.observed || result.source != source || result.epoch != epoch ||
			result.version != version || epoch != activeEpoch) { result.Cancel(); return false; }
		if (!contributes) return false;
		result.observed = true;
		result.Cancel();
		timestamp = observedEpoch == activeEpoch ? (std::max)(timestamp, result.timestamp) : result.timestamp;
		observedEpoch = activeEpoch;
		++revision;
		return true;
	}
};

}
