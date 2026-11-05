#pragma once
#include "FrameUpdateReason.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>

namespace Magpie {

// Owned by one FIFO notification, including across retries. These are wall
// intervals between attempts (which may service input), not GPU durations.
struct PresentationJobTiming {
	using Clock = std::chrono::steady_clock;
	enum class Wait { None, Deadline, Capacity, Resource };
	Clock::time_point enqueued = Clock::now();
	Clock::time_point retryStarted{};
	Wait waiting = Wait::None;
	std::chrono::nanoseconds deadline{}, capacity{}, resource{}, cpu{};
	std::chrono::nanoseconds beginFrame{}, draw{}, endFrame{};
	uint32_t attempts = 0;
	void Resume(Clock::time_point now) noexcept {
		const auto elapsed = std::max(now - retryStarted, Clock::duration::zero());
		switch (waiting) {
		case Wait::Deadline: deadline += elapsed; break;
		case Wait::Capacity: capacity += elapsed; break;
		case Wait::Resource: resource += elapsed; break;
		default: break;
		}
		waiting = Wait::None;
	}
	void Retry(Wait reason, Clock::time_point now) noexcept {
		Resume(now);
		waiting = reason;
		retryStarted = now;
	}
};

// One deadline per submitted image/input. Small lateness can correct phase,
// but never admits a new frame less than 75% of a period after the previous
// one. Long stalls re-anchor instead of accumulating catch-up submissions.
class FrontEdgeSyncClock {
public:
	using Clock = std::chrono::steady_clock;
	void SetInterval(std::chrono::nanoseconds interval) noexcept {
		interval = std::max(interval, std::chrono::nanoseconds(1));
		if (_interval != interval) { _interval = interval; Reset(); }
	}
	Clock::time_point Due(Clock::time_point now) const noexcept {
		return _started ? _next : now;
	}
	void Submitted(Clock::time_point now) noexcept {
		const auto due = Due(now);
		_next = now > due + _interval ? now + _interval :
			std::max(due + _interval, now + _interval * 3 / 4);
		_started = true;
	}
	void Reset() noexcept { _started = false; _next = {}; }
private:
	std::chrono::nanoseconds _interval{16'666'667};
	Clock::time_point _next{};
	bool _started = false;
};

// Limit repeats of the last content image, not delivery of new content. Every
// successful shared-layer content frame also carries the current overlay.
class OverlayPresentationClock {
public:
	using Clock = std::chrono::steady_clock;
	void SetRefreshRate(double refreshRate) noexcept {
		if (!(refreshRate >= 10.0 && refreshRate <= 1000.0)) refreshRate = 60.0;
		_interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(1.0 / refreshRate));
	}
	bool IsDue(Clock::time_point now, bool immediate) const noexcept {
		return immediate || !_started || now - _last >= _interval;
	}
	void Presented(Clock::time_point now) noexcept { _last = now; _started = true; }
	std::chrono::nanoseconds PollInterval() const noexcept {
		return std::min(_interval, std::chrono::nanoseconds(std::chrono::milliseconds(8)));
	}
	std::chrono::nanoseconds Interval() const noexcept { return _interval; }
private:
	Clock::time_point _last{};
	std::chrono::nanoseconds _interval{ 16'666'666 };
	bool _started = false;
};

// Estimate pacing without feeding downstream queue waits back into the next
// interval. Those waits are consequences of presentation, not source cadence.
class CaptureFrameCadence {
public:
	using Clock = std::chrono::steady_clock;
	bool Observe(Clock::time_point now,
		Clock::duration downstreamWait = Clock::duration::zero()) noexcept {
		const auto elapsed = now - _last;
		_lastDownstreamWait = std::clamp(downstreamWait, Clock::duration::zero(),
			std::max(elapsed, Clock::duration::zero()));
		const bool interrupted = _started && elapsed >= std::chrono::milliseconds(500);
		const auto sourceElapsed = elapsed - _lastDownstreamWait;
		if (_started && !interrupted && sourceElapsed > Clock::duration::zero()) {
			const double seconds = std::chrono::duration<double>(sourceElapsed).count();
			_seconds = _hasSequenceEstimate ? _seconds + (seconds - _seconds) * 0.2 : seconds;
			_hasSequenceEstimate = true;
		} else if (interrupted) {
			// Keep the last healthy interval as a fallback, but do not blend the
			// first new pair with the previous sequence's samples.
			_hasSequenceEstimate = false;
		}
		_started = true;
		_last = now;
		return interrupted;
	}
	Clock::duration LastDownstreamWait() const noexcept { return _lastDownstreamWait; }
	std::chrono::nanoseconds Interval(uint32_t multiplier, double baseLimit) const noexcept {
		const double minimum = baseLimit > 0 ? 1.0 / baseLimit : 0.0;
		const double seconds = std::max(_seconds > 0 ? _seconds : 1.0 / 60.0, minimum);
		return std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::duration<double>(seconds / std::max(multiplier, 1u)));
	}
	void Reset() noexcept { *this = {}; }
	void RestartSequence() noexcept {
		_started = false;
		_hasSequenceEstimate = false;
		_lastDownstreamWait = {};
	}
private:
	Clock::time_point _last{};
	Clock::duration _lastDownstreamWait{};
	double _seconds = 0;
	bool _started = false;
	bool _hasSequenceEstimate = false;
};

// An output target describes our capacity, not when the source will capture
// again. Learn from native capture timestamps, retain a recent slow/jitter
// envelope, and wait through two source periods before a final quality redraw.
class CaptureIdleClock {
public:
	using Clock = std::chrono::steady_clock;
	void Observe(int64_t timestamp100ns, Clock::time_point now) noexcept {
		if (timestamp100ns > _timestamp100ns && _timestamp100ns > 0) {
			const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>(timestamp100ns - _timestamp100ns));
			// A resumed static source must not teach the detector its idle gap.
			if (interval <= std::chrono::seconds(2) &&
				(_samples < 2 || interval <= std::chrono::milliseconds(250) || interval < IdleInterval())) {
				_intervals[_next++ % _intervals.size()] = interval;
				_samples = std::min(_samples + 1, _intervals.size());
			}
		}
		_timestamp100ns = timestamp100ns;
		_last = now;
		_started = true;
	}
	std::chrono::nanoseconds Remaining(Clock::time_point now) const noexcept {
		if (!_started) return std::chrono::milliseconds(250);
		return std::max(std::chrono::nanoseconds::zero(),
			std::chrono::duration_cast<std::chrono::nanoseconds>(_last - now) + IdleInterval());
	}
	void Reset() noexcept { *this = {}; }
	std::chrono::nanoseconds IdleInterval() const noexcept {
		if (_samples < 2) return std::chrono::milliseconds(250);
		auto minimum = _intervals[0], maximum = minimum;
		for (size_t i = 1; i < _samples; ++i) {
			minimum = std::min(minimum, _intervals[i]);
			maximum = std::max(maximum, _intervals[i]);
		}
		return maximum * 2 + std::max(maximum - minimum,
			std::chrono::nanoseconds(std::chrono::milliseconds(1)));
	}
private:
	std::array<std::chrono::nanoseconds, 12> _intervals{};
	size_t _samples = 0, _next = 0;
	int64_t _timestamp100ns = 0;
	Clock::time_point _last{};
	bool _started = false;
};

// Bounded frontend samples of successful Present calls. These are submission
// intervals, not physical scanout timestamps or GPU execution durations.
struct OutputCadenceSnapshot {
	std::array<uint32_t, 5> counts{};
	double captureRate = 0;
	double p50Ms = 0, p95Ms = 0, p99Ms = 0, maximumMs = 0, outputMaximumMs = 0;
};
class OutputCadenceStatistics {
public:
	using Clock = std::chrono::steady_clock;
	std::optional<OutputCadenceSnapshot> Submitted(FrameUpdateReason reason, Clock::time_point now) noexcept {
		++_counts[static_cast<size_t>(reason)];
		if (_lastOutput != Clock::time_point{})
			_outputMaximum = std::max(_outputMaximum, now - _lastOutput);
		_lastOutput = now;
		if (reason != FrameUpdateReason::NewCapture) return {};
		if (_lastCapture != Clock::time_point{}) {
			_intervals[_samples++] = now - _lastCapture;
		}
		_lastCapture = now;
		if (_samples < _intervals.size()) return {};
		std::sort(_intervals.begin(), _intervals.end());
		OutputCadenceSnapshot result;
		result.counts = _counts;
		Clock::duration total{};
		for (auto interval : _intervals) total += interval;
		result.captureRate = std::chrono::duration<double>(total).count() > 0 ?
			double(_samples) / std::chrono::duration<double>(total).count() : 0;
		const auto milliseconds = [](auto value) { return std::chrono::duration<double, std::milli>(value).count(); };
		result.p50Ms = milliseconds(_intervals[_samples / 2]);
		result.p95Ms = milliseconds(_intervals[(_samples * 95 + 99) / 100 - 1]);
		result.p99Ms = milliseconds(_intervals[(_samples * 99 + 99) / 100 - 1]);
		result.maximumMs = milliseconds(_intervals.back());
		result.outputMaximumMs = milliseconds(_outputMaximum);
		_samples = 0; _counts = {}; _outputMaximum = {};
		return result;
	}
private:
	std::array<Clock::duration, 120> _intervals{};
	std::array<uint32_t, 5> _counts{};
	size_t _samples = 0;
	Clock::time_point _lastCapture{}, _lastOutput{};
	Clock::duration _outputMaximum{};
};

// No catch-up burst after a long stall. The frontend polls this deadline while
// continuing to service input; capacity waits remain owned by the presenter.
class FramePresentationClock {
public:
	using Clock = std::chrono::steady_clock;
	Clock::time_point Due(Clock::time_point now, std::chrono::nanoseconds interval) const noexcept {
		return _started ? _last + interval : now;
	}
	void Presented(Clock::time_point now, Clock::time_point due,
		std::chrono::nanoseconds interval) noexcept {
		_last = interval.count() > 0 && now <= due + interval ? due : now;
		_started = true;
	}
	void Reset() noexcept { *this = {}; }
private:
	Clock::time_point _last{};
	bool _started = false;
};

}
