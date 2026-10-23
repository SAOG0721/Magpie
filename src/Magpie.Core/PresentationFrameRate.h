#pragma once
#include <chrono>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include "ProfilerHistory.h"
#include "PresentationIntervals.h"

namespace Magpie {

enum class ContentFrameCountSource { Reported, Submitted };
struct ContentFrameCount {
	std::optional<uint32_t> total;
	ContentFrameCountSource source = ContentFrameCountSource::Reported;
	// Only a directly observed single-frame content submission can supply this.
	// SDK batch/status counts must leave it empty, even when a batch equals one.
	std::optional<std::chrono::steady_clock::time_point> individualTimestamp = std::nullopt;
};

struct PresentationProfilerSnapshot {
	ProfilerHistory::Plot plot;
	ContentFrameCountSource source = ContentFrameCountSource::Reported;
	bool active = false;
	bool individualTimestamps = false;
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
		ContentFrameCountSource source = ContentFrameCountSource::Reported,
		std::optional<Clock::time_point> individualTimestamp = std::nullopt) noexcept {
		_Update(now);
		if (_profiling && !_history.Suspended()) {
			if (_profileSource != source) _ResetProfile(now);
			_profileSource = source;
			if (total == 1u && individualTimestamp && *individualTimestamp >= _profileStarted &&
				*individualTimestamp <= now && (!_lastIndividualTimestamp || *individualTimestamp > *_lastIndividualTimestamp)) {
				_history.Frame(*individualTimestamp);
				_lastIndividualTimestamp = individualTimestamp;
				_individualTimestamps = true;
			} else {
				_history.CountFrames(now, total);
				if (!total || *total != 0) {
					_history.BreakIntervals();
					_lastIndividualTimestamp.reset();
				}
			}
		}
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
		const bool profiling = _profiling;
		*this = {}; _source = source; _snapshot.source = source;
		if (profiling) StartProfile();
	}
	void StartProfile(Clock::time_point now = Clock::now()) noexcept {
		_profiling = true;
		_profileGeneration.reset();
		_profileSource = _source;
		_ResetProfile(now);
	}
	void StopProfile() noexcept {
		_profiling = false;
		_history = {};
		_individualTimestamps = false;
		_lastIndividualTimestamp.reset();
	}
	bool Profiling() const noexcept { return _profiling; }
	void RecordIntervals(const PresentationIntervals& intervals, Clock::time_point now = Clock::now()) noexcept {
		if (!_profiling || _history.Suspended()) return;
		for (size_t i = 0; i < intervals.count; ++i) {
			const auto& sample = intervals.samples[i];
			if (sample.begin < _profileStarted || sample.end > now || sample.end <= sample.begin ||
				(_lastIntervalEnd && sample.end <= *_lastIntervalEnd)) continue;
			_history.Interval(sample.begin, sample.end);
			_lastIntervalEnd = sample.end;
			_individualTimestamps = true;
		}
	}
	// Called on the frontend with the backend's coherent profiler snapshot.
	// A UI generation transition may drop early output samples, but cannot mix
	// old sequence/chain data into the newly displayed input/output comparison.
	void SynchronizeProfile(uint64_t generation, bool suspended,
		Clock::time_point now = Clock::now()) noexcept {
		if (!_profiling) return;
		if ((_profileGeneration && *_profileGeneration != generation) || (_history.Suspended() && !suspended)) _ResetProfile(now);
		_profileGeneration = generation;
		_history.Suspend(suspended, now);
		if (suspended) _lastIndividualTimestamp.reset();
	}
	PresentationProfilerSnapshot Profile(Clock::time_point now = Clock::now()) noexcept {
		// The frontend owns all output events, so observing an idle interval here
		// is valid. This advances time only; it never creates frame samples.
		_history.Observe(now);
		return { _history.Read(now), _profileSource, _profiling, _individualTimestamps };
	}

private:
	void _ResetProfile(Clock::time_point now) noexcept {
		_history.Reset(now);
		_profileStarted = now;
		_lastIndividualTimestamp.reset();
		_individualTimestamps = false;
		_lastIntervalEnd.reset();
	}
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
	ProfilerHistory _history;
	Clock::time_point _profileStarted{};
	std::optional<Clock::time_point> _lastIndividualTimestamp;
	std::optional<Clock::time_point> _lastIntervalEnd;
	std::optional<uint64_t> _profileGeneration;
	ContentFrameCountSource _profileSource = ContentFrameCountSource::Reported;
	bool _profiling = false, _individualTimestamps = false;
};

}
