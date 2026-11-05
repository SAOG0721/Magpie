#pragma once
#include "DLSSNRPerformanceState.h"
#include <array>
#include <cstddef>
#include <limits>

namespace Magpie {

// Storage is bounded independently of inference concurrency and source age.
// The owner supplies a completed GPU fence, never a CPU "finished using" flag.
class DLSSNREnhancementPool {
public:
	static constexpr size_t Capacity = 3;
	static constexpr size_t None = Capacity;
	struct Slot {
		uint64_t sequence = 0, epoch = 0, version = 0;
		uint64_t readyFence = 0, readFence = 0;
		bool writing = false;
		bool expirationCounted = false;
	};

	size_t Reserve(uint64_t completed) noexcept {
		size_t choice = None;
		for (size_t i = 0; i < Capacity; ++i) {
			const auto& s = _slots[i];
			if (s.writing || s.readyFence > completed || s.readFence > completed) continue;
			if (choice == None || s.version < _slots[choice].version) choice = i;
		}
		if (choice != None) { _slots[choice] = {}; _slots[choice].writing = true; }
		return choice;
	}
	void Cancel(size_t slot) noexcept { if (slot < Capacity) _slots[slot] = {}; }
	void Publish(size_t slot, uint64_t sequence, uint64_t epoch, uint64_t ready) noexcept {
		auto& s = _slots[slot];
		s = { sequence, epoch, ++_version, ready, 0, false };
	}
	uint64_t Version() const noexcept { return _version; }
	size_t Latest(const DLSSNRPerformanceState& state, uint64_t completed) const noexcept {
		size_t choice = None;
		for (size_t i = 0; i < Capacity; ++i) {
			const auto& s = _slots[i];
			if (s.writing || !s.version || s.readyFence > completed || !state.Usable(s.sequence, s.epoch)) continue;
			if (choice == None || s.sequence > _slots[choice].sequence ||
				(s.sequence == _slots[choice].sequence && s.version > _slots[choice].version)) choice = i;
		}
		return choice;
	}
	void ReadSubmitted(size_t slot, uint64_t fence) noexcept { _slots[slot].readFence = fence; }
	uint64_t ExpireUnconsumed(const DLSSNRPerformanceState& state) noexcept {
		uint64_t count = 0;
		for (auto& slot : _slots) if (!slot.writing && slot.version && !slot.readFence &&
			!slot.expirationCounted && slot.epoch == state.epoch &&
			slot.sequence < state.sequence && state.sequence-slot.sequence > state.maxAgeFrames) {
			slot.expirationCounted = true; ++count;
		}
		return count;
	}
	const Slot& operator[](size_t i) const noexcept { return _slots[i]; }
private:
	std::array<Slot, Capacity> _slots{};
	uint64_t _version = 0;
};

}
