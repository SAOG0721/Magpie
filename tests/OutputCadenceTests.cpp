#include "FramePresentationTiming.h"
#include <cassert>
#include <iostream>

using namespace Magpie;
using namespace std::chrono_literals;

int main() {
	using Clock = CaptureIdleClock::Clock;
	const auto start = Clock::time_point(1s);
	for (int rate : {30, 60, 90, 120}) {
		CaptureIdleClock idle;
		const auto interval = std::chrono::nanoseconds(1'000'000'000 / rate);
		auto now = start;
		int64_t timestamp = 10'000'000;
		idle.Observe(timestamp, now);
		assert(idle.Remaining(now + 100ms) > 0ns);
		for (int frame = 1; frame <= 360; ++frame) {
			const auto jitter = frame % 3 == 0 ? -1ms : frame % 3 == 1 ? 1ms : 0ms;
			const auto delta = interval + jitter;
			// Even a slow 30 Hz source with a 120 Hz output target keeps its
			// next source opportunity ahead of any final quality refresh.
			assert(idle.Remaining(now + delta + 1ms) > 0ns);
			now += delta;
			timestamp += delta.count() / 100;
			idle.Observe(timestamp, now);
		}
		const auto threshold = idle.IdleInterval();
		assert(threshold > interval * 2);
		assert(idle.Remaining(now + threshold - 1ns) > 0ns);
		assert(idle.Remaining(now + threshold) == 0ns);
		idle.Observe(timestamp + 20'000'000, now + 2s);
		assert(idle.IdleInterval() == threshold);
		assert(idle.Remaining(now + 2s + interval) > 0ns);
		idle.Reset();
		idle.Observe(timestamp + 20'000'000, now + 2s);
		assert(idle.IdleInterval() == 250ms);
	}
	for (int rate : {60, 90, 120}) {
		const auto period = std::chrono::nanoseconds(1'000'000'000 / rate);
		FrontEdgeSyncClock content;
		FramePresentationClock display;
		content.SetInterval(period);
		content.Submitted(start);
		display.Presented(start, start, period);
		const auto nextContent = content.Due(start);
		for (const auto reason : {FrameUpdateReason::QualityRefresh, FrameUpdateReason::ParameterUpdate}) {
			assert(!IsContentUpdate(reason));
			const auto refresh = start + period * 9 / 4;
			display.Presented(refresh, refresh, period);
			assert(content.Due(refresh) == nextContent);
			assert(display.Due(refresh, period) == refresh + period);
		}
		OutputCadenceStatistics stats;
		assert(!stats.Submitted(FrameUpdateReason::NewCapture, start));
		std::optional<OutputCadenceSnapshot> sample;
		for (int frame = 1; frame <= 120; ++frame) {
			assert(!stats.Submitted(FrameUpdateReason::QualityRefresh, start + period * frame - period / 2));
			sample = stats.Submitted(FrameUpdateReason::NewCapture, start + period * frame);
		}
		assert(sample && sample->counts[0] == 121 && sample->counts[1] == 120);
		assert(sample->p50Ms == sample->maximumMs);
		assert(sample->captureRate > rate - 0.01 && sample->captureRate < rate + 0.01);
	}
	{
		// A genuine source-rate drop is different from the output target. Learn
		// the first slower native interval rather than retaining a 120 Hz guess.
		CaptureIdleClock idle;
		idle.Observe(10'000'000, start);
		idle.Observe(10'083'333, start + 8'333'300ns);
		idle.Observe(10'166'666, start + 16'666'600ns);
		idle.Observe(10'499'999, start + 49'999'900ns);
		assert(idle.IdleInterval() > 66ms);
		assert(idle.Remaining(start + 83'333'200ns) > 0ns);
	}
	std::cout << "PASS source idle detection at 30/60/90/120 Hz with jitter, static/resume/reset; "
		"separate content/display clocks and successful Present interval statistics\n";
}
