#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace Magpie {

// Owns the cadence and quality decision. The caller owns GPU resources and
// confirms a switch only after the proposed tier was actually committed.
class DLSSNRAdaptiveResolution {
public:
	struct Resolution {
		uint32_t full = 100;
		bool operator==(const Resolution&) const = default;
	};
	struct Sample {
		double now = 0, captureTime = 0, explicitTarget = 0, captureLimit = 0;
		double foregroundGpuMs = 0, fixedGpuMs = 0;
		double fullTurnaroundMs = 0, enhancementCoverage = -1;
		uint64_t gpuSamples = 0, completed = 0, late = 0, published = 0;
		// New-capture publications only. The latency starts at capture admission.
		uint64_t contentDelivered = 0;
		double publicationMs = -1;
		bool newCapture = false, reset = false, asynchronous = false;
		bool fixedGpuKnown = true, captureQueued = false, fullInference = true;
		unsigned updateInterval = 1;
		double fullGpuMs = -1;
		double fullReadyMs = -1, fullAdoptionMs = -1;
		uint64_t fullGpuSamples = 0, fullReadySamples = 0;
		uint64_t fullAdoptionSamples = 0, fullAdopted = 0, coverageSamples = 0;
		bool hasHotResolution = false, hasRecoveryResolution = false;
		Resolution hotResolution{}, recoveryResolution{};
	};

	enum class Reason {
		Sampling, Cooldown, FixedBottleneck, ForegroundOverload, FullDeadline,
		Delivery, Quality, Headroom, ColdUpgrade, HotUpgrade, RetryBlocked,
		TrialAccepted, RestoreResolution
	};

	void Reset(Resolution initial, double now = 0) {
		*this = {};
		_active = Quantize(initial);
		_settleUntil = now + SWITCH_SETTLE_SECONDS;
	}
	Resolution Active() const noexcept { return _active; }
	double Target() const noexcept { return _target; }
	Reason LastReason() const noexcept { return _reason; }
	bool RequiresHotResolution() const noexcept { return _proposal.hotOnly; }
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
			case Reason::RetryBlocked: return "retry-blocked";
			case Reason::TrialAccepted: return "trial-accepted";
			case Reason::RestoreResolution: return "restore-resolution";
		}
		return "sampling";
	}

	// Explicit inference changes invalidate the cost history for the old setup.
	void InvalidateMeasurements(double now) {
		_failedUp = {}; _failedDown = {};
		_gpuReliefNeedsDelivery = false;
		ResetObservation(now);
	}

	Resolution Observe(const Sample& sample) {
		if (!sample.newCapture) return _active;
		if (_proposal.kind != Change::None) return _proposal.to;
		const double configured = Positive(sample.explicitTarget);
		const double limit = Positive(sample.captureLimit);
		if (std::abs(configured - _configured) > .001 || std::abs(limit - _captureLimit) > .001) {
			_configured = configured;
			_captureLimit = limit;
			_target = Limited(configured);
			_gpuReliefNeedsDelivery = false;
			ResetObservation(sample.now);
		}
		if (sample.reset || (_lastCapture > 0 && sample.captureTime - _lastCapture > .5)) {
			// Interruption invalidates a comparison, not the record of a failed tier.
			ResetObservation(sample.now, true);
		}
		_lastCapture = sample.captureTime;

		const bool newGpu = NewSample(sample.gpuSamples, _gpuSerial);
		const bool newFull = NewSample(sample.fullGpuSamples, _fullSerial);
		const bool newDelivery = NewSample(sample.contentDelivered, _deliverySerial);
		const bool newReady = NewSample(sample.fullReadySamples ? sample.fullReadySamples : sample.completed, _readySerial);
		const bool newAdoption = NewSample(sample.fullAdoptionSamples, _adoptionSerial);
		const bool newCoverage = NewSample(sample.coverageSamples, _coverageSerial);
		if (sample.now < _settleUntil || sample.now < _rejectUntil) {
			_reason = Reason::Cooldown;
			return _active;
		}

		if (_window.captures == 0) _window.Start(sample);
		else {
			++_window.captures;
			_window.queued += sample.captureQueued;
			_window.captureEnd = sample.captureTime;
		}
		if (newGpu) {
			Append(sample.fullInference || sample.asynchronous ? _window.foreground : _window.reuse,
				sample.foregroundGpuMs);
			if (sample.fixedGpuKnown) Append(_window.fixed, sample.fixedGpuMs);
		}
		if (newFull) Append(_window.inference, sample.fullGpuMs);
		if (newDelivery) Append(_window.latency, sample.publicationMs);
		if (sample.asynchronous) {
			if (newReady) Append(_window.ready, sample.fullReadyMs >= 0 ? sample.fullReadyMs : sample.fullTurnaroundMs);
			if (newAdoption) Append(_window.adoption, sample.fullAdoptionMs);
			if (newCoverage) Append(_window.coverage, sample.enhancementCoverage);
		}

		const double elapsed = sample.now - _window.start;
		const bool alternating = !sample.asynchronous && sample.updateInterval > 1;
		const size_t minPhaseSamples = alternating ? 4 : 8;
		if (elapsed < WINDOW_SECONDS || _window.foreground.size() < minPhaseSamples ||
			(alternating && _window.reuse.size() < minPhaseSamples)) {
			_reason = Reason::Sampling;
			if (elapsed >= 3) _window = {};
			return _active;
		}
		const Metrics measured = _window.Measure(sample, alternating);
		_window = {};
		if (!(measured.source > 0)) return _active;
		if (!_trial) UpdateTarget(measured);
		if (!(_target > 0)) return _active;
		const double budget = 1000. / _target;
		RecordObservation(measured, budget);
		return _trial ? AssessTrial(measured, budget, sample) : SelectResolution(measured, budget, sample);
	}

	void Commit(Resolution value, double now, double preparationMs = 0) {
		value = Quantize(value);
		if (_proposal.to == value && (_proposal.kind == Change::Up || _proposal.kind == Change::Down)) {
			_trial = _proposal;
		} else {
			_trial.reset();
		}
		_proposal = {};
		_active = value;
		_anchor = 0;
		ResetWindows(now, std::max(SWITCH_SETTLE_SECONDS, preparationMs / 1000.));
	}

	void Reject(double now) {
		_proposal = {};
		_rejectUntil = now + 5;
		ResetWindows(now, SWITCH_SETTLE_SECONDS);
	}

	// A sparse status snapshot for the runtime log. Updated once per window.
	struct Observation {
		uint64_t sequence = 0;
		double sourceFps = 0, deliveredFps = 0, gpuMs = 0, scaledMs = 0;
		double budgetMs = 0, gpuShare = 0, queuedFraction = 0, lateFraction = 0, publicationP90 = -1;
	};
	const Observation& LastObservation() const noexcept { return _observation; }

private:
	static constexpr double WINDOW_SECONDS = 1.;
	static constexpr double SWITCH_SETTLE_SECONDS = .25;
	static constexpr double DOWN_PRESSURE = .93;
	static constexpr double UP_PRESSURE = .81;
	static constexpr double RETRY_PRESSURE_CHANGE = .12;
	static constexpr double SATURATED_CAPTURE_GPU_SHARE = .92;
	static constexpr unsigned CONFIRM_WINDOWS = 2;
	static constexpr unsigned UPGRADE_WINDOWS = 4;
	static constexpr unsigned TIER_COUNT = 16;

	struct Metrics {
		double source = 0, delivered = 0, delivery = 0, queued = 0;
		double work = 0, fixed = 0, full = 0, scalable = 0, fullGpu = 0, workSpread = 0;
		double readyP95 = 0, adoptionP95 = 0, coverageP10 = 0;
		double publicationP90 = -1;
		bool fixedKnown = false, scalableKnown = false, fullKnown = false, adoptionKnown = false;
		uint64_t completed = 0, overdue = 0, published = 0, adopted = 0;
		std::vector<double> latency;

		double LateFraction(double budget) const {
			if (latency.empty()) return 0;
			const auto late = std::ranges::count_if(latency, [budget](double value) { return value > budget; });
			return double(late) / double(latency.size());
		}
		double CaptureGpuShare() const {
			return (work - workSpread) * std::min(source, delivered) / 1000.;
		}
	};
	struct Window {
		double start = 0, captureStart = 0, captureEnd = 0;
		uint64_t delivered = 0, completed = 0, overdue = 0, published = 0, adopted = 0;
		unsigned captures = 0, queued = 0;
		std::vector<double> foreground, reuse, fixed, inference, latency, ready, adoption, coverage;

		void Start(const Sample& sample) {
			start = sample.now;
			captureStart = captureEnd = sample.captureTime;
			delivered = sample.contentDelivered;
			completed = sample.completed;
			overdue = sample.late;
			published = sample.published;
			adopted = sample.fullAdopted;
			captures = 1;
			queued = unsigned(sample.captureQueued);
		}
		Metrics Measure(const Sample& sample, bool alternating) {
			const double elapsed = sample.now - start;
			const double sourceElapsed = captureEnd > captureStart ? captureEnd - captureStart : elapsed;
			const uint64_t publications = Difference(sample.contentDelivered, delivered);
			const double full = Percentile(foreground, .9);
			const double fixedMs = Percentile(fixed, .9);
			const auto perCapture = [alternating, interval = sample.updateInterval](double fullMs, double reuseMs) {
				return alternating ? (fullMs + reuseMs*(interval-1))/interval : fullMs;
			};
			const double work = perCapture(full, Percentile(reuse, .9));
			const double typical = perCapture(Percentile(foreground, .5), Percentile(reuse, .5)) + Percentile(fixed, .5);
			const double fullGpu = Percentile(inference, .9);
			const bool estimatesFull = inference.size() >= 3;
			const double scalable = estimatesFull ?
				std::min(fullGpu / (alternating ? sample.updateInterval : 1), work) : work;
			return {
				.source = double(captures-1) / sourceElapsed,
				.delivered = double(publications) / elapsed,
				.delivery = double(publications) / double(captures-1),
				.queued = double(queued) / double(captures),
				.work = work + fixedMs, .fixed = fixedMs, .full = full + fixedMs,
				.scalable = scalable, .fullGpu = fullGpu, .workSpread = std::max(0., work + fixedMs - typical),
				.readyP95 = Percentile(ready, .95), .adoptionP95 = Percentile(adoption, .95),
				.coverageP10 = Percentile(coverage, .1),
				.publicationP90 = latency.size() >= 8 ? Percentile(latency, .9) : -1,
				.fixedKnown = sample.fixedGpuKnown && !fixed.empty(),
				.scalableKnown = estimatesFull,
				.fullKnown = estimatesFull && ready.size() >= minAsyncMeasurements,
				.adoptionKnown = adoption.size() >= minAsyncMeasurements && !coverage.empty(),
				.completed = Difference(sample.completed, completed),
				.overdue = Difference(sample.late, overdue),
				.published = Difference(sample.published, published),
				.adopted = Difference(sample.fullAdopted, adopted),
				.latency = std::move(latency)
			};
		}
		static constexpr size_t minAsyncMeasurements = 5;
	};
	enum class Change { None, Up, Down, Restore };
	enum class DescentBenefit { None, GpuRelief, Delivery };
	struct Transition {
		Change kind = Change::None;
		Resolution from{}, to{};
		double pressure = 0, forecast = 0, scalablePressure = 0, noise = 0;
		double delivered = 0, publicationP90 = -1, gpuShare = 0;
		double worstUpper = 0;
		unsigned windows = 0, goodWindows = 0, badWindows = 0, deliveryWindows = 0;
		bool scalableKnown = false, hotOnly = false;
	};
	struct FailedChange {
		double referencePressure = 0, upperPressure = 0, noise = 0;
		bool active = false;
		double RequiredChange(double currentNoise) const {
			return std::max({referencePressure*RETRY_PRESSURE_CHANGE, noise, currentNoise});
		}
	};

	static bool NewSample(uint64_t serial, uint64_t& prior) {
		return serial && std::exchange(prior, serial) != serial;
	}
	static uint64_t Difference(uint64_t now, uint64_t before) { return now >= before ? now-before : 0; }
	static double Positive(double value) {
		return std::isfinite(value) && value > 0 ? std::clamp(value, 1., 1000.) : 0;
	}
	static void Append(std::vector<double>& values, double value) {
		if (std::isfinite(value) && value >= 0 && values.size() < 1024) values.push_back(value);
	}
	static double Percentile(std::vector<double>& values, double fraction) {
		if (values.empty()) return 0;
		const auto nth = values.begin() + (size_t(std::ceil(double(values.size())*fraction))-1);
		std::ranges::nth_element(values, nth);
		return *nth;
	}
	static Resolution Quantize(Resolution value) {
		return {(std::clamp(value.full, 25u, 100u)+2)/5*5};
	}
	static size_t Tier(Resolution value) { return (value.full - 25) / 5; }
	double Limited(double rate) const {
		const double ceiling = _configured > 0 && _captureLimit > 0 ?
			std::min(_configured, _captureLimit) : std::max(_configured, _captureLimit);
		return ceiling && rate > 0 ? std::min(ceiling, rate) : rate;
	}
	double Predict(const Metrics& measured, Resolution next) const {
		const double ratio = double(next.full) / _active.full;
		return measured.work + measured.scalable * (ratio*ratio - 1);
	}
	void ResetWindows(double now, double settle) {
		_window = {};
		_settleUntil = now + settle;
		_cadenceCandidate = 0;
		_cadenceWindows = _goodWindows = _badWindows = 0;
	}
	void ResetObservation(double now, bool retainSourceAnchor = false) {
		_proposal = {};
		_trial.reset();
		if (!retainSourceAnchor) _anchor = 0;
		ResetWindows(now, SWITCH_SETTLE_SECONDS);
	}
	void UpdateTarget(const Metrics& measured) {
		const double seen = Limited(measured.source);
		if (!_target) _target = seen;
		if (!_anchor) {
			// The first measurements after a switch establish that tier's
			// baseline. Faster delivery from the switch is not a new target.
			_anchor = seen;
			return;
		}
		const bool healthy = HealthyAtSourceRate(measured);
		const bool slower = seen < _target*.90 && healthy;
		const bool faster = seen > _target*1.05 &&
			((_configured > 0 && healthy) || (seen > _anchor*1.07 && (healthy || measured.queued >= .12)));
		if (!slower && !faster) { _cadenceWindows = 0; return; }
		if (!_cadenceWindows || std::abs(seen - _cadenceCandidate) > _cadenceCandidate*.08) {
			_cadenceCandidate = seen;
			_cadenceWindows = 1;
			return;
		}
		if (++_cadenceWindows >= CONFIRM_WINDOWS) {
			_target = std::min(seen, _cadenceCandidate);
			_anchor = seen;
			_cadenceWindows = _goodWindows = _badWindows = 0;
		}
	}
	bool HealthyAtSourceRate(const Metrics& measured) const {
		const double supplyBudget = 1000. / Limited(measured.source);
		const double workLimit = supplyBudget * (measured.latency.size() >= 8 ? .94 : .86);
		return measured.delivery >= .94 && measured.work < workLimit &&
			measured.queued < .10 && measured.LateFraction(supplyBudget) < .10;
	}
	bool Overloaded(const Metrics& measured, double budget) const {
		const bool deliveryMiss = measured.delivered < _target*.96 &&
			(measured.queued >= .12 || measured.source >= _target*.97);
		const bool locallyLate = measured.LateFraction(budget) >= .12;
		// CPU publication can remain fast while GPU work fills each actual
		// capture interval. Only use this evidence against an explicit goal.
		const bool saturated = _configured > 0 && measured.delivered < _target*.9 &&
			measured.scalableKnown && measured.scalable >= budget*.12 &&
			measured.CaptureGpuShare() >= SATURATED_CAPTURE_GPU_SHARE;
		return (measured.work > budget*DOWN_PRESSURE && (deliveryMiss || locallyLate)) ||
			(measured.full > budget && locallyLate) || saturated;
	}
	bool HasHot(Resolution resolution, const Sample& sample) const {
		return (sample.hasHotResolution && sample.hotResolution == resolution) ||
			(sample.hasRecoveryResolution && sample.recoveryResolution == resolution);
	}
	Resolution Propose(Change kind, Resolution next, const Metrics& measured, double budget, bool hotOnly) {
		_proposal = {
			.kind = kind, .from = _active, .to = next,
			.pressure = measured.work/budget, .forecast = Predict(measured, next)/budget,
			.scalablePressure = measured.scalable/budget, .noise = measured.workSpread/budget,
			.delivered = measured.delivered, .publicationP90 = measured.publicationP90, .gpuShare = measured.CaptureGpuShare(),
			.scalableKnown = measured.scalableKnown, .hotOnly = hotOnly
		};
		return next;
	}
	Resolution SelectResolution(const Metrics& measured, double budget, const Sample& sample) {
		const double scalable = measured.scalable;
		if (measured.fixed >= budget*.9 && scalable < budget*.15) {
			_goodWindows = _badWindows = 0;
			_reason = Reason::FixedBottleneck;
			return _active;
		}
		const bool asyncMiss = sample.asynchronous && measured.fullKnown && measured.completed >= 8 &&
			double(measured.overdue)/double(measured.completed) > .05 &&
			(measured.fullGpu > budget*.8 || (measured.readyP95 > budget*.9 && measured.fullGpu >= budget*.5));
		const double sourceRate = Limited(measured.source);
		const bool sourceHealthy = HealthyAtSourceRate(measured);
		const bool sourceLimited = sourceRate < _target*.90 && sourceHealthy;
		const bool overload = (!sourceLimited && Overloaded(measured, budget)) || asyncMiss;
		const Resolution lower{_active.full > 25 ? _active.full-5 : _active.full};
		// Without clear attribution, only probe a single already warm tier.
		const bool hotProbe = !sourceLimited && _configured == 0 && measured.work > budget*1.03 &&
			measured.queued < .12 && HasHot(lower, sample);
		const bool wantsDown = (overload || hotProbe) && lower != _active && scalable >= budget*.12;
		const auto& failedDown = _failedDown[Tier(lower)];
		// Deadline counts fluctuate when latency straddles the budget. Retrying
		// a rejected descent requires two fresh windows of worse scalable load.
		const bool retryBlocked = wantsDown && failedDown.active &&
			scalable/budget - failedDown.referencePressure < failedDown.RequiredChange(measured.workSpread/budget);
		_badWindows = wantsDown && !retryBlocked ? _badWindows + 1 : 0;
		if (_badWindows) _goodWindows = 0;
		if (retryBlocked) {
			_goodWindows = 0;
			_reason = Reason::RetryBlocked;
			return _active;
		}
		if (_badWindows >= CONFIRM_WINDOWS) {
			_reason = asyncMiss ? Reason::FullDeadline : Reason::ForegroundOverload;
			return Propose(Change::Down, lower, measured, budget, !overload);
		}

		// A modest healthy source slowdown can leave the working budget
		// unchanged. Judge delivery against the captures actually available.
		const double deliveryGoal = _configured == 0 && sourceHealthy ? std::min(sourceRate, _target) : _target;
		const bool goodDelivery = measured.delivery >= .94 && measured.delivered >= deliveryGoal*.95 &&
			measured.LateFraction(budget) < .08;
		const bool quality = !sample.asynchronous || (measured.fullKnown && measured.adoptionKnown &&
			measured.completed >= 8 && measured.published >= 8 &&
			double(measured.published)/double(measured.completed) >= .95 &&
			double(measured.adopted)/double(measured.published) >= .9 &&
			double(measured.overdue)/double(measured.completed) <= .05 &&
			measured.adoptionP95 < budget*2 && measured.coverageP10 >= .8);
		if (overload || !goodDelivery || !quality || !measured.fixedKnown || measured.work >= budget*.72) {
			_goodWindows = 0;
			_reason = !quality ? Reason::Quality : !goodDelivery ? Reason::Delivery : Reason::Headroom;
			return _active;
		}
		if (_active.full == 100) { _reason = Reason::Headroom; return _active; }
		Resolution next = _active;
		bool blocked = false, warmUpgradeAvailable = false;
		for (const auto candidate : {sample.recoveryResolution, sample.hotResolution}) {
			if (candidate.full <= next.full || !HasHot(candidate, sample)) continue;
			warmUpgradeAvailable = true;
			const double forecast = Predict(measured, candidate)/budget;
			const double ratio = double(candidate.full)/_active.full;
			if (forecast > UP_PRESSURE || (sample.asynchronous && measured.readyP95*ratio*ratio >= budget*.8)) continue;
			const auto& failure = _failedUp[Tier(candidate)];
			if (failure.active && (failure.referencePressure - forecast < failure.RequiredChange(measured.workSpread/budget) ||
				(failure.referencePressure > 0 && failure.upperPressure*forecast/failure.referencePressure > .89))) {
				blocked = true;
				continue;
			}
			next = candidate;
		}
		if (next == _active) {
			_goodWindows = 0;
			_reason = blocked ? Reason::RetryBlocked :
				warmUpgradeAvailable ? Reason::Headroom : Reason::ColdUpgrade;
			return _active;
		}
		if (_goodTier != next) _goodWindows = 0;
		_goodTier = next;
		if (++_goodWindows < UPGRADE_WINDOWS) { _reason = Reason::Cooldown; return _active; }
		_reason = Reason::HotUpgrade;
		return Propose(Change::Up, next, measured, budget, true);
	}
	DescentBenefit EvaluateDescent(const Transition& trial, const Metrics& measured, double budget) const {
		const double workSaved = trial.pressure - measured.work/budget;
		const double noise = std::max(trial.noise, measured.workSpread/budget);
		if (workSaved < std::max(.02, noise) || measured.delivered < trial.delivered*.98)
			return DescentBenefit::None;

		// Compare processing latency in milliseconds. Crossing the budget by
		// a fraction of a millisecond can flip the entire late-frame count.
		const bool compareLatency = trial.publicationP90 >= 0 && measured.publicationP90 >= 0;
		const double latencySaved = trial.publicationP90 - measured.publicationP90;
		if (compareLatency && latencySaved < -budget*.25) return DescentBenefit::None;
		const double deliveredGain = std::min(measured.delivered, _target) - std::min(trial.delivered, _target);
		if (deliveredGain >= _target*.025 ||
			(compareLatency && trial.publicationP90 > budget && latencySaved >= budget*.12)) return DescentBenefit::Delivery;

		// Permit one attributable GPU saving at a saturated fixed target.
		// Further reductions need a delivery or latency gain to be accepted.
		const double scalableSaved = trial.scalablePressure - measured.scalable/budget;
		const bool gpuRelief = !_gpuReliefNeedsDelivery && _configured > 0 &&
			trial.gpuShare >= SATURATED_CAPTURE_GPU_SHARE && trial.scalableKnown && measured.scalableKnown &&
			scalableSaved >= std::max(.06, trial.scalablePressure*.03);
		return gpuRelief ? DescentBenefit::GpuRelief : DescentBenefit::None;
	}
	Resolution AssessTrial(const Metrics& measured, double budget, const Sample& sample) {
		auto& trial = *_trial;
		const double pressure = measured.work/budget;
		const bool up = trial.kind == Change::Up;
		const auto benefit = up ? DescentBenefit::None : EvaluateDescent(trial, measured, budget);
		const bool passed = up ? !Overloaded(measured, budget) &&
			measured.delivered >= std::min(trial.delivered, _target)*.95 : benefit != DescentBenefit::None;
		const bool failed = !passed;
		++trial.windows;
		trial.goodWindows = passed ? trial.goodWindows + 1 : 0;
		trial.badWindows = failed ? trial.badWindows + 1 : 0;
		trial.deliveryWindows = benefit == DescentBenefit::Delivery ? trial.deliveryWindows + 1 : 0;
		trial.worstUpper = std::max(trial.worstUpper, pressure);
		if (trial.goodWindows >= CONFIRM_WINDOWS) {
			_gpuReliefNeedsDelivery = !up && trial.deliveryWindows < CONFIRM_WINDOWS;
			if (!up) {
				_failedUp[Tier(trial.from)] = {
					.referencePressure = Predict(measured, trial.from)/budget,
					.upperPressure = trial.pressure, .noise = measured.workSpread/budget, .active = true
				};
			}
			_trial.reset();
			_reason = Reason::TrialAccepted;
			return _active;
		}
		if (trial.badWindows >= CONFIRM_WINDOWS || trial.windows >= 5) {
			// Only live windows can count against a tier. A creation failure or
			// a capture interruption cancels the comparison separately.
			(up ? _failedUp : _failedDown)[Tier(trial.to)] = {
				.referencePressure = up ? trial.forecast : trial.scalablePressure,
				.upperPressure = trial.worstUpper, .noise = trial.noise, .active = true
			};
			if (HasHot(trial.from, sample)) {
				_reason = Reason::RestoreResolution;
				return Propose(Change::Restore, trial.from, measured, budget, true);
			}
			_trial.reset();
			_reason = Reason::ColdUpgrade;
			return _active;
		}
		_reason = Reason::Cooldown;
		return _active;
	}
	void RecordObservation(const Metrics& measured, double budget) {
		_observation = {
			.sequence = _observation.sequence + 1, .sourceFps = measured.source,
			.deliveredFps = measured.delivered, .gpuMs = measured.work,
			.scaledMs = measured.scalable, .budgetMs = budget, .gpuShare = measured.CaptureGpuShare(),
			.queuedFraction = measured.queued, .lateFraction = measured.LateFraction(budget),
			.publicationP90 = measured.publicationP90
		};
	}

	Resolution _active, _goodTier;
	Reason _reason = Reason::Sampling;
	Window _window;
	Observation _observation;
	Transition _proposal;
	std::optional<Transition> _trial;
	std::array<FailedChange, TIER_COUNT> _failedUp{}, _failedDown{};
	bool _gpuReliefNeedsDelivery = false;
	double _target = 0, _configured = 0, _captureLimit = 0, _lastCapture = 0;
	double _anchor = 0, _cadenceCandidate = 0;
	double _settleUntil = 0, _rejectUntil = 0;
	unsigned _cadenceWindows = 0, _goodWindows = 0, _badWindows = 0;
	uint64_t _gpuSerial = 0, _fullSerial = 0, _deliverySerial = 0;
	uint64_t _readySerial = 0, _adoptionSerial = 0, _coverageSerial = 0;
};

}
