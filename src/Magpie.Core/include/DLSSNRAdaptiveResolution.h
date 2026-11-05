#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Magpie {

// Resolution policy only. A proposal neither creates resources nor changes the
// active tier; the chain validates the hot entry again before committing it.
class DLSSNRAdaptiveResolution {
public:
	struct Resolution {
		uint32_t full = 100;
		bool operator==(const Resolution&) const = default;
	};
	struct Sample {
		double now = 0, captureTime = 0, explicitTarget = 0;
		double foregroundGpuMs = 0, fixedGpuMs = 0;
		double fullTurnaroundMs = 0;
		double enhancementCoverage = -1;
		uint64_t gpuSamples = 0, completed = 0, late = 0, published = 0;
		// Backend new-capture publications, never same-source refreshes or FG.
		uint64_t contentDelivered = 0;
		bool newCapture = false, reset = false, asynchronous = false;
		bool fixedGpuKnown = true;
		double fullGpuMs = -1;
		double fullReadyMs = -1, fullAdoptionMs = -1;
		uint64_t fullGpuSamples = 0, fullReadySamples = 0;
		uint64_t fullAdoptionSamples = 0, fullAdopted = 0, coverageSamples = 0;
		bool hasHotResolution = false;
		Resolution hotResolution{};
	};
	enum class Reason {
		Sampling, Cooldown, FixedBottleneck, ForegroundOverload,
		FullDeadline, Delivery, Quality, Headroom, ColdUpgrade, HotUpgrade
	};

	void Reset(Resolution initial, double now = 0) {
		*this = {};
		_active = Quantize(initial);
		_cooldownUntil = now + 1;
	}
	Resolution Active() const noexcept { return _active; }
	double Target() const noexcept { return _target; }
	Reason LastReason() const noexcept { return _reason; }
	static const char* ReasonName(Reason reason) noexcept {
		switch (reason) {
		case Reason::Sampling: return "sampling";
		case Reason::Cooldown: return "cooldown";
		case Reason::FixedBottleneck: return "fixed-bottleneck";
		case Reason::ForegroundOverload: return "foreground-overload";
		case Reason::FullDeadline: return "full-deadline";
		case Reason::Delivery: return "content-delivery";
		case Reason::Quality: return "enhancement-quality";
		case Reason::Headroom: return "headroom";
		case Reason::ColdUpgrade: return "cold-upgrade";
		case Reason::HotUpgrade: return "hot-upgrade";
		}
		return "sampling";
	}

	// Live inference edits invalidate cost/quality evidence without turning a
	// temporary delivery slowdown into a new, lower capture target.
	void InvalidateMeasurements(double now) {
		ClearWindows(); _cadence.clear(); _counters.clear();
		_goodSince = 0; _overBudgetWindows = _panicSamples = _panicFullSamples = 0;
		_cooldownUntil = now + 1; _lastEvaluation = now;
	}

	Resolution Observe(const Sample& sample) {
		if (!sample.newCapture) return _active;
		const double now = sample.now;
		if (sample.reset || (_lastCapture > 0 && sample.captureTime - _lastCapture > .5)) {
			InvalidateMeasurements(now);
			_lastCapture = sample.captureTime;
			return _active;
		}
		if (sample.explicitTarget > 0 && std::isfinite(sample.explicitTarget))
			_target = std::clamp(sample.explicitTarget, 1., 1000.);
		if (sample.captureTime > _lastCapture) {
			if (_lastCapture > 0) {
				const double dt = sample.captureTime - _lastCapture;
				if (dt >= .001 && dt <= .2) {
					_cadence.push_back(dt);
					if (_cadence.size() > 120) _cadence.erase(_cadence.begin());
				}
			}
			_lastCapture = sample.captureTime;
			if (_target == 0 && _cadence.size() >= 24) {
				auto values = _cadence; std::ranges::sort(values);
				const double median = values[values.size()/2];
				if (values[values.size()*9/10] < median*1.2)
					_target = std::clamp(1./median, 1., 1000.);
			}
		}
		const double budget = _target > 0 ? 1000./_target : 0;
		const double fixed = Valid(sample.fixedGpuMs) ? sample.fixedGpuMs : 0;
		if (_foreground.Add(sample.gpuSamples, now, sample.foregroundGpuMs)) {
			_fixed.Add(sample.gpuSamples, now, fixed);
			_panicSamples = budget > 0 && sample.foregroundGpuMs + fixed > budget*1.1
				? _panicSamples+1 : 0;
		}
		if (_fullGpu.Add(sample.fullGpuSamples, now, sample.fullGpuMs))
			_panicFullSamples = budget > 0 && sample.fullGpuMs > budget*1.1 ? _panicFullSamples+1 : 0;
		// The legacy turnaround remains a fallback for older diagnostic producers.
		_ready.Add(sample.fullReadySamples ? sample.fullReadySamples : sample.completed,
			now, sample.fullReadyMs >= 0 ? sample.fullReadyMs : sample.fullTurnaroundMs);
		_adoption.Add(sample.fullAdoptionSamples, now, sample.fullAdoptionMs);
		_coverage.Add(sample.coverageSamples, now, sample.enhancementCoverage);
		RecordCounters(sample);
		for (auto* window : {&_foreground, &_fixed, &_fullGpu, &_ready, &_adoption, &_coverage})
			window->Prune(now);
		const bool emergency = _panicSamples >= 3 || (sample.asynchronous && _panicFullSamples >= 3);
		if (budget == 0 || _foreground.Size() < 8) { _reason = Reason::Sampling; return _active; }
		if (now < _rejectUntil || (!emergency && now < _cooldownUntil)) {
			_reason = Reason::Cooldown; return _active;
		}
		if (!emergency && now-_lastEvaluation < .25) return _active;
		_lastEvaluation = now;
		const double foreground = _foreground.Percentile(.95), fixedCost = _fixed.Percentile(.95);
		const double frontCost = foreground + fixedCost;
		auto predictForeground = [&](Resolution tier) {
			return fixedCost + foreground*Area(tier.full, _active.full);
		};
		const auto& first = _counters.front();
		const uint64_t completed = sample.completed-first.completed, late = sample.late-first.late;
		const uint64_t published = sample.published-first.published, adopted = sample.fullAdopted-first.adopted;
		const double seconds = now-first.now;
		const bool fullKnown = _fullGpu.Size() >= 8 && _ready.Size() >= 8;
		const bool computeSlow = fullKnown && (_fullGpu.Percentile(.95) > budget*.8 ||
			(_ready.Percentile(.95) > budget*.9 && _fullGpu.Percentile(.95) >= budget*.5));
		const bool missedDeadline = completed >= 8 && double(late)/completed > .05;
		const bool fixedBottleneck = fixedCost >= budget*.9 && foreground < budget*.15;
		_overBudgetWindows = frontCost > budget*.9 && !fixedBottleneck ? _overBudgetWindows+1 : 0;
		if (sample.asynchronous && computeSlow && (missedDeadline || _panicFullSamples >= 3)) {
			_goodSince = 0; _reason = Reason::FullDeadline;
			return Downgrade(sample, emergency, [&](Resolution tier) {
				return _fullGpu.Percentile(.95)*Area(tier.full, _active.full);
			});
		}
		if (_overBudgetWindows >= 2 || _panicSamples >= 3) {
			_goodSince = 0;
			if (fixedBottleneck) { _reason = Reason::FixedBottleneck; return _active; }
			_reason = Reason::ForegroundOverload;
			return Downgrade(sample, emergency, predictForeground);
		}
		if (fixedBottleneck) { _goodSince = 0; _reason = Reason::FixedBottleneck; return _active; }
		if (frontCost >= budget*.65 || !sample.fixedGpuKnown || !Valid(sample.fixedGpuMs)) {
			_goodSince = 0; _reason = Reason::Headroom; return _active;
		}
		if (seconds <= 0 || double(sample.contentDelivered-first.delivered)/seconds < _target*.95) {
			_goodSince = 0; _reason = Reason::Delivery; return _active;
		}
		// These are deliberately separate: publication alone neither proves that
		// a result was selected nor that its pixels contributed useful enhancement.
		const bool quality = !sample.asynchronous || (completed >= 8 && published >= 8 &&
			double(published)/completed >= .95 && !missedDeadline &&
			double(adopted)/published >= .9 && _adoption.Size() >= 8 &&
			_adoption.Percentile(.95) < budget*2 && _coverage.Size() >= 8 &&
			_coverage.Percentile(.1) >= .8 && fullKnown);
		if (!quality) { _goodSince = 0; _reason = Reason::Quality; return _active; }
		if (_goodSince == 0) _goodSince = now;
		if (now-_goodSince < 6 || now < _upgradeAfter) { _reason = Reason::Cooldown; return _active; }
		Resolution next = _active;
		if (_active.full < 100) {
			next.full += 5;
			if (predictForeground(next) >= budget*.8 || (sample.asynchronous &&
				_ready.Percentile(.95)*Area(next.full, _active.full) >= budget*.8)) next = _active;
		}
		if (next == _active) { _reason = Reason::Headroom; return _active; }
		if (!sample.hasHotResolution || sample.hotResolution != next) {
			_reason = Reason::ColdUpgrade; return _active;
		}
		_reason = Reason::HotUpgrade;
		return next;
	}

	void Commit(Resolution value, double now, double preparationMs = 0) {
		value = Quantize(value);
		const bool downgrade = value.full < _active.full;
		const double costDelay = std::isfinite(preparationMs) ? std::max(0., preparationMs)*.06 : 0;
		_upgradeAfter = now+std::max(downgrade ? 12. : 8., costDelay);
		_active = value; _cooldownUntil = now+(downgrade ? .5 : 1.);
		_goodSince = 0; _overBudgetWindows = _panicSamples = _panicFullSamples = 0;
		ClearWindows(); _counters.clear();
	}
	void Reject(double now) {
		_rejectUntil = now+5; _goodSince = 0;
		ClearWindows(); _counters.clear();
	}
private:
	struct Window {
		struct Value { double now, value; };
		std::vector<Value> values;
		uint64_t serial = 0;
		bool Add(uint64_t next, double now, double value) {
			if (!next || next == serial || !Valid(value)) return false;
			if (next < serial) values.clear();
			serial = next; values.push_back({now, value});
			if (values.size() > 120) values.erase(values.begin());
			return true;
		}
		void Prune(double now) {
			std::erase_if(values, [&](const Value& value) { return now-value.now > 4; });
		}
		size_t Size() const noexcept { return values.size(); }
		double Percentile(double fraction) const {
			if (values.empty()) return 0;
			std::vector<double> sorted; sorted.reserve(values.size());
			for (const auto& value : values) sorted.push_back(value.value);
			std::ranges::sort(sorted);
			return sorted[std::min(sorted.size()-1, size_t(std::ceil(sorted.size()*fraction)-1))];
		}
	};
	struct Counters { double now; uint64_t completed, late, published, adopted, delivered; };
	void RecordCounters(const Sample& sample) {
		if (!_counters.empty()) {
			const auto& last = _counters.back();
			if (sample.completed < last.completed || sample.late < last.late || sample.published < last.published ||
				sample.fullAdopted < last.adopted || sample.contentDelivered < last.delivered) {
				_counters.clear(); _goodSince = 0;
			}
		}
		_counters.push_back({sample.now, sample.completed, sample.late, sample.published, sample.fullAdopted, sample.contentDelivered});
		std::erase_if(_counters, [&](const Counters& value) { return sample.now-value.now > 4; });
		if (_counters.size() > 120) _counters.erase(_counters.begin());
	}
	static bool Valid(double value) noexcept { return std::isfinite(value) && value >= 0; }
	static double Area(uint32_t value, uint32_t previous) noexcept {
		const double ratio = double(value)/previous; return ratio*ratio;
	}
	static Resolution Quantize(Resolution value) noexcept {
		value.full = (std::clamp(value.full, 25u, 100u)+2)/5*5;
		return value;
	}
	template<typename Prediction>
	Resolution Downgrade(const Sample& sample, bool emergency, Prediction&& predict) const {
		Resolution next = _active;
		if (next.full > 25) next.full -= 5;
		if (next == _active || !sample.hasHotResolution || Quantize(sample.hotResolution) != sample.hotResolution) return next;
		const auto hot = sample.hotResolution;
		const bool matching = hot.full < _active.full;
		if (matching && (emergency || hot == next) && predict(hot) < predict(_active)) return hot;
		return next;
	}
	void ClearWindows() {
		_foreground = {}; _fixed = {};
		_fullGpu = {}; _ready = {}; _adoption = {}; _coverage = {};
	}
	Resolution _active;
	Reason _reason = Reason::Sampling;
	std::vector<double> _cadence;
	std::vector<Counters> _counters;
	Window _foreground, _fixed, _fullGpu, _ready, _adoption, _coverage;
	double _target = 0, _lastCapture = 0, _lastEvaluation = 0;
	double _cooldownUntil = 0, _upgradeAfter = 0, _rejectUntil = 0, _goodSince = 0;
	unsigned _overBudgetWindows = 0, _panicSamples = 0, _panicFullSamples = 0;
};

}
