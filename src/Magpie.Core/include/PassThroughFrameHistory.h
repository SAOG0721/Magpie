#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace Magpie {

// Identity and copy ordering for optional original-image history. Texture and
// metadata ownership stay in PassThroughFrames. Selected references have two
// independent storage slots, so capture-ring eviction cannot change a held
// image's reference or the previous output used by a generated frame.
class PassThroughFrameHistory {
public:
	static constexpr uint32_t MaxCapacity = 8;
	static constexpr uint32_t Missing = std::numeric_limits<uint32_t>::max();
	struct Selection {
		bool swapSelected = false;
		bool previousFromTarget = false;
		uint32_t previousCaptureSlot = Missing;
		uint32_t targetCaptureSlot = Missing;
		bool targetAvailable = false;
		bool previousAvailable = false;
	};

	bool Reset(uint32_t capacity = 0) noexcept {
		_capacity = 0;
		_next = 0;
		_frames.fill(0);
		_target = _previous = 0;
		if (capacity > MaxCapacity) return false;
		_capacity = capacity;
		return true;
	}
	uint32_t Capacity() const noexcept { return _capacity; }
	uint64_t TargetFrameId() const noexcept { return _target; }
	uint64_t PreviousFrameId() const noexcept { return _previous; }
	uint64_t CaptureFrameId(uint32_t slot) const noexcept {
		return slot < _capacity ? _frames[slot] : 0;
	}

	uint32_t Capture(uint64_t frame) noexcept {
		if (!_capacity || !frame) return Missing;
		const auto retained = _FindCapture(frame);
		if (retained != Missing) return retained;
		const uint32_t slot = _next;
		_next = (_next + 1) % _capacity;
		_frames[slot] = frame;
		return slot;
	}

	// Apply a plan in this order: swap selected slots; copy the previous
	// reference; copy the target reference. Copying the target first could
	// destroy the previous output's only retained reference.
	Selection Select(uint64_t target, uint64_t previousOutput) noexcept {
		Selection result;
		if (!_capacity) return result;
		if (target && target != _target && target == _previous) {
			std::swap(_target, _previous);
			result.swapSelected = true;
		}
		if (previousOutput) {
			if (previousOutput == _previous) result.previousAvailable = true;
			else if (previousOutput == _target) {
				result.previousFromTarget = true;
				result.previousAvailable = true;
				_previous = _target;
			} else {
				result.previousCaptureSlot = _FindCapture(previousOutput);
				if (result.previousCaptureSlot != Missing) {
					_previous = previousOutput;
					result.previousAvailable = true;
				}
			}
		}
		if (target) {
			if (target == _target) result.targetAvailable = true;
			else {
				result.targetCaptureSlot = _FindCapture(target);
				if (result.targetCaptureSlot != Missing) {
					_target = target;
					result.targetAvailable = true;
				}
			}
		}
		return result;
	}

private:
	uint32_t _FindCapture(uint64_t frame) const noexcept {
		if (frame) for (uint32_t i = 0; i < _capacity; ++i)
			if (_frames[i] == frame) return i;
		return Missing;
	}
	std::array<uint64_t, MaxCapacity> _frames{};
	uint32_t _capacity = 0, _next = 0;
	uint64_t _target = 0, _previous = 0;
};

}
