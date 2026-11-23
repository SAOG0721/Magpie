#pragma once
#include <chrono>
#include <optional>

namespace Magpie {

// USER32 can publish a maximized-state change before updating the rectangle.
// Retiring an owned output in that interval can interrupt the source transition.
class SourcePlacementTransition {
public:
	using Clock = std::chrono::steady_clock;

	bool ShouldDeferPlacement(bool maximizedChanged, bool rectUnchanged, bool eligible,
		Clock::time_point now) noexcept {
		if (!maximizedChanged || !rectUnchanged || !eligible) {
			Reset();
			return false;
		}
		if (!_deadline) _deadline = now + std::chrono::milliseconds(100);
		// Keep an expired deadline until the condition clears. Repeated polls
		// cannot renew the wait, including before the caller commits its state.
		return now < *_deadline;
	}

	void Reset() noexcept { _deadline.reset(); }

private:
	std::optional<Clock::time_point> _deadline;
};

}
