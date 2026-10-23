#include "../src/Magpie.Core/ProfilerHistory.h"
#include <cassert>
#include <cstdio>
#include <limits>

using namespace Magpie;
using namespace std::chrono;
static const ProfilerHistory::Time base(100s);
static bool Near(double value, double expected, double tolerance = 0.001) {
    return std::abs(value - expected) < tolerance;
}

int main() {
    for (int fps : {60, 120}) {
        ProfilerHistory history;
        history.Reset(base);
        for (int i = 0; i < fps * 12; ++i) {
            const auto at = base + nanoseconds(int64_t(i) * 1000000000 / fps);
            history.Frame(at);
            history.Effects(at, 2.0);
        }
        history.Observe(base + 12s);
        const auto plot = history.Read(base + 12s);
        assert(Near(plot.points.back().fps, fps));
        assert(!plot.points.back().warming);
        const auto& before = plot.points[98];
        assert(before.interval.count && Near(before.interval.Mean(), 1000.0 / fps));
        assert(before.effects.count && Near(before.effects.Mean(), 2.0));
        const auto repeated = history.Read(base + 12s);
        assert(repeated.points[98].effects.count == before.effects.count);
    }
    ProfilerHistory history;
    history.Reset(base);
    history.Frame(base);
    history.Frame(base + 16ms);
    history.Frame(base + 216ms);
    history.Effects(base + 16ms, 1);
    history.Effects(base + 16ms, 30); // Two completions before one UI snapshot.
    history.Effects(base + 16ms, 2);
    history.Effects(base + 216ms, 0); // Confirmed reuse is a valid zero.
    history.Effects(base + 216ms, std::numeric_limits<double>::quiet_NaN());
    history.Observe(base + 500ms);
    auto plot = history.Read(base + 500ms);
    bool spike = false, delayed = false, zero = false, missing = false;
    for (const auto& point : plot.points) {
        if (point.interval.maximum == 200) spike = true;
        if (point.effects.maximum == 30) { delayed = true; assert(point.effects.count == 3); }
        if (point.effects.count && point.effects.Mean() == 0) zero = true;
        if (point.time > base + 300ms && !point.effects.count) missing = true;
    }
    assert(spike && delayed && zero && missing);
    history.Observe(base + 2s);
    plot = history.Read(base + 2s);
    assert(plot.points.back().fpsValid && plot.points.back().fps == 0);
    assert(!plot.points.back().interval.count && !plot.points.back().effects.count);
    // UI alone cannot invent observations after the producer stopped reporting.
    assert(!history.Read(base + 3s).points.back().fpsValid);
    history.Suspend(true, base + 2100ms);
    history.Observe(base + 5s);
    assert(history.Read(base + 5s).suspended);
    assert(!history.Read(base + 5s).points.back().fpsValid);
    history.Suspend(false, base + 5s);
    history.Frame(base + 5s);
    history.Frame(base + 5016ms);
    history.Observe(base + 5100ms);
    plot = history.Read(base + 5100ms);
    for (const auto& point : plot.points) assert(point.interval.maximum <= 16);
    // Same-pass-count chain/sequence reset drops delayed results from the old epoch.
    history.Reset(base + 6s);
    history.Effects(base + 5016ms, 999);
    history.Frame(base + 6s);
    history.Observe(base + 6050ms);
    plot = history.Read(base + 6050ms);
    assert(Near(plot.points.back().fps, 20) && plot.points.back().warming);
    for (const auto& point : plot.points) assert(!point.effects.count);
    // Ring wrap / long gap stays bounded and cannot resurrect old data.
    history.Frame(base + 60s);
    history.Effects(base, 500);
    history.Observe(base + 60100ms);
    plot = history.Read(base + 60100ms);
    for (const auto& point : plot.points) assert(!point.effects.count);
    assert(plot.points[98].interval.maximum > 50000); // real long gap is retained
    std::puts("Profiler history: rates, actual intervals, peaks, delayed results, missing/zero, pause, reset and wrap passed.");
}
