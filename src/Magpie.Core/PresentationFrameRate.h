#pragma once
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Magpie {

enum class ContentFrameCountSource { Reported, Submitted };
struct ContentFrameCount {
	std::optional<uint32_t> total;
	ContentFrameCountSource source = ContentFrameCountSource::Reported;
};

struct PresentationRateSnapshot {
	uint32_t total = 0;
	uint32_t real = 0;
	bool totalKnown = true;
	uint32_t idleSubmissions = 0;
	uint32_t generated = 0;
	ContentFrameCountSource source = ContentFrameCountSource::Reported;
	std::chrono::steady_clock::time_point windowStart{}, windowEnd{};
};

// Frontend-thread counters. Content excludes overlay/idle replays and configured
// multipliers. Idle submissions are separate; unavailable SDK counts stay unknown.
class PresentationFrameRate {
public:
	using Clock = std::chrono::steady_clock;
	void Record(std::optional<uint32_t> total, uint32_t real,
		Clock::time_point now = Clock::now(),
		ContentFrameCountSource source = ContentFrameCountSource::Reported) noexcept {
		_Update(now);
		if (_windowHasContent && _source != source) _known = false;
		_source = source;
		_windowHasContent = true;
		if (total) {
			_total += *total;
			_generated += *total - std::min(*total, real);
		} else _known = false;
		_real += real;
		_lastContent = now;
		_hasContent = true;
	}
	void RecordIdleSubmission(Clock::time_point now = Clock::now()) noexcept {
		_Update(now); ++_idle;
	}
	bool WaitingForContent(Clock::time_point now = Clock::now()) const noexcept {
		return _hasContent && now - _lastContent >= std::chrono::milliseconds(500);
	}
	PresentationRateSnapshot Get(Clock::time_point now = Clock::now()) noexcept {
		_Update(now);
		return _snapshot;
	}
	void Reset(ContentFrameCountSource source = ContentFrameCountSource::Reported) noexcept {
		*this = {}; _source = source; _snapshot.source = source;
	}

private:
	void _Update(Clock::time_point now) noexcept {
		if (!_started) { _start = now; _started = true; return; }
		const double elapsed = std::chrono::duration<double>(now - _start).count();
		if (elapsed < 1.0) return;
		_snapshot = { uint32_t(std::llround(_total / elapsed)),
			uint32_t(std::llround(_real / elapsed)), _known,
			uint32_t(std::llround(_idle / elapsed)), uint32_t(std::llround(_generated / elapsed)), _source, _start, now };
		_start = now;
		_total = _real = _idle = _generated = 0;
		_known = true;
		_windowHasContent = false;
	}
	Clock::time_point _start{};
	uint64_t _total = 0, _real = 0, _idle = 0, _generated = 0;
	Clock::time_point _lastContent{};
	bool _hasContent = false;
	bool _started = false, _known = true;
	ContentFrameCountSource _source = ContentFrameCountSource::Reported;
	bool _windowHasContent = false;
	PresentationRateSnapshot _snapshot{};
};

}
