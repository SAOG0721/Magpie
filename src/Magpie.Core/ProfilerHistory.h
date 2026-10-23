#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>

namespace Magpie {

// Backend-owned, fixed-size history. Copies are safe to inspect on the frontend.
// Time is supplied by the caller so presentation/UI activity cannot add frames.
class ProfilerHistory {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr size_t PointCount = 100;
    static constexpr auto BucketDuration = std::chrono::milliseconds(100);

    struct Values {
        double total = 0;
        double maximum = 0;
        uint32_t count = 0;
        void Add(double value) noexcept {
            total += value;
            maximum = std::max(maximum, value);
            ++count;
        }
        double Mean() const noexcept { return count ? total / count : 0; }
    };
    struct Point {
        Time time{};
        float fps = 0;
        bool fpsValid = false;
        bool warming = false;
        Values interval;
        Values effects;
    };
    struct Plot {
        std::array<Point, PointCount> points{};
        bool suspended = false;
    };

    void Reset(Time now) noexcept {
        *this = {};
        _started = now;
        _observedThrough = now;
        _active = true;
        _latestTick = Tick(now);
    }

    void Suspend(bool suspended, Time now) noexcept {
        if (!_active || suspended == _suspended) return;
        if (!suspended) {
            // Restart the observation window after an explicit capture pause.
            Reset(now);
            return;
        }
        Advance(now);
        _suspended = true;
        _suspendedAt = now;
        _lastFrame.reset();
    }

    void Frame(Time now) noexcept {
        if (!_active || _suspended) return;
        Advance(now);
        Bucket& bucket = At(Tick(now));
        ++bucket.frames;
        if (_lastFrame && now > *_lastFrame) {
            bucket.interval.Add(std::chrono::duration<double, std::milli>(now - *_lastFrame).count());
        }
        _lastFrame = now;
    }

    void Observe(Time now) noexcept {
        if (_active) Advance(now);
    }

    // A completed GPU result belongs to its original CPU frame anchor, not to
    // the time GetData happens to finish. Late results cannot overwrite newer bins.
    void Effects(Time frameTime, double milliseconds) noexcept {
        if (!_active || frameTime < _started || !std::isfinite(milliseconds) || milliseconds < 0) return;
        const int64_t tick = Tick(frameTime);
        if (tick > _latestTick || _latestTick - tick >= int64_t(Capacity)) return;
        Bucket& bucket = At(tick);
        bucket.effects.Add(milliseconds);
    }

    Plot Read(Time now) const noexcept {
        Plot result;
        result.suspended = _suspended;
        if (!_active || now < _started) return result;
        const int64_t lastTick = Tick(now);
        for (size_t i = 0; i < PointCount; ++i) {
            const int64_t tick = lastTick - int64_t(PointCount - 1 - i);
            auto& point = result.points[i];
            const Time end = std::min(now, End(tick));
            point.time = end;
            if (end <= _started) continue;
            if (const auto* bucket = Find(tick)) {
                point.interval = bucket->interval;
                point.effects = bucket->effects;
            }
            if (_suspended && end >= _suspendedAt) continue;
            if (end > _observedThrough) continue;
            const Time begin = std::max(_started, End(tick - 10));
            const double elapsed = std::chrono::duration<double>(end - begin).count();
            if (elapsed <= 0) continue;
            uint64_t frames = 0;
            for (int64_t sample = tick - 9; sample <= tick; ++sample) {
                if (const auto* bucket = Find(sample)) frames += bucket->frames;
            }
            point.fps = float(double(frames) / elapsed);
            point.fpsValid = true;
            point.warming = elapsed < 0.999;
        }
        return result;
    }

    bool Active() const noexcept { return _active; }
    bool Suspended() const noexcept { return _suspended; }

private:
    // Ten extra bins preserve the FPS window preceding the oldest visible point.
    static constexpr size_t Capacity = PointCount + 12;
    struct Bucket {
        int64_t tick = INT64_MIN;
        uint32_t frames = 0;
        Values interval;
        Values effects;
    };
    static int64_t Tick(Time now) noexcept {
        return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() / 100;
    }
    static Time End(int64_t tick) noexcept {
        return Time(std::chrono::duration_cast<Clock::duration>(BucketDuration * (tick + 1)));
    }
    static size_t Index(int64_t tick) noexcept {
        return size_t((tick % int64_t(Capacity) + int64_t(Capacity)) % int64_t(Capacity));
    }
    Bucket& At(int64_t tick) noexcept {
        auto& bucket = _buckets[Index(tick)];
        if (bucket.tick != tick) bucket = Bucket{ .tick = tick };
        return bucket;
    }
    const Bucket* Find(int64_t tick) const noexcept {
        const auto& bucket = _buckets[Index(tick)];
        return bucket.tick == tick ? &bucket : nullptr;
    }
    void Advance(Time now) noexcept {
        _observedThrough = std::max(_observedThrough, now);
        const int64_t tick = Tick(now);
        if (tick <= _latestTick) return;
        // Bound work even after a long content gap. Zero-frame bins are real
        // observations; absent GPU/interval samples remain absent.
        for (int64_t i = std::max(_latestTick + 1, tick - int64_t(Capacity) + 1); i <= tick; ++i) At(i);
        _latestTick = tick;
    }
    std::array<Bucket, Capacity> _buckets{};
    Time _started{}, _suspendedAt{}, _observedThrough{};
    std::optional<Time> _lastFrame;
    int64_t _latestTick = 0;
    bool _active = false, _suspended = false;
};

}
