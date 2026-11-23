#pragma once
#include <atomic>
#include <cstdint>

namespace Magpie {

class ScalingSessionLifetime {
public:
	explicit ScalingSessionLifetime(uint32_t runId) noexcept : _runId(runId) {}

	void RequestStop() noexcept { _stopping.store(true, std::memory_order_release); }
	bool IsStopping() const noexcept { return _stopping.load(std::memory_order_acquire); }
	bool MatchesRun(uint32_t runId) const noexcept { return _runId == runId; }
	bool IsCurrent(uint32_t runId) const noexcept { return MatchesRun(runId) && !IsStopping(); }

private:
	const uint32_t _runId;
	std::atomic<bool> _stopping = false;
};

}
