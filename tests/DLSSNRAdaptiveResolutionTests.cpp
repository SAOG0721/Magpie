#include "DLSSNRAdaptiveResolution.h"
#include "DLSSNRDetailParameters.h"
#include "DLSSNRPerformanceParameters.h"
#include "EffectParameterRules.h"
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>
#include <limits>
#include <source_location>
#include <string>
using namespace Magpie;

using Resolution = DLSSNRAdaptiveResolution::Resolution;
using Reason = DLSSNRAdaptiveResolution::Reason;
struct Harness {
	DLSSNRAdaptiveResolution policy;
	DLSSNRAdaptiveResolution::Sample sample;
	double interval = 1./60;
	unsigned ticks = 0, switches = 0;
	unsigned deliveryEvery = 1, publishEvery = 1, adoptEvery = 1, lateEvery = 0;
	bool freshGpu = true;
	Harness(unsigned full = 75) {
		policy.Reset({full});
		sample.explicitTarget = 60;
		sample.foregroundGpuMs = 5;
		sample.fullGpuMs = 3;
		sample.fullTurnaroundMs = sample.fullReadyMs = 6;
		sample.fullAdoptionMs = 7;
		sample.enhancementCoverage = 1;
		sample.publicationMs = 6;
		sample.newCapture = true;
		sample.hasHotResolution = true;
		sample.hotResolution = {full < 100 ? full+5 : full-5};
	}
	void Async(double full = 3, double ready = 6) {
		sample.asynchronous = true;
		sample.fullGpuMs = full; sample.fullReadyMs = ready;
		sample.foregroundGpuMs = 3;
	}
	void Serve(double fps) { interval = 1./fps; }
	void ExpectTarget(double fps, double tolerance = .001,
		std::source_location caller = std::source_location::current()) const {
		if (std::abs(policy.Target() - fps) >= tolerance)
			std::cerr << "Line " << caller.line() << ": expected " << fps << " FPS, got " << policy.Target() << '\n';
		assert(std::abs(policy.Target() - fps) < tolerance);
	}
	void Commit(Resolution next, double preparationMs = 0) {
		assert(next != policy.Active());
		policy.Commit(next, sample.now, preparationMs);
		++switches;
	}
	Resolution Step() { return Step(interval); }
	Resolution Step(double elapsed) {
		sample.now += elapsed; sample.captureTime += elapsed;
		++ticks;
		if (freshGpu) ++sample.gpuSamples;
		if (sample.fullInference || sample.asynchronous) ++sample.fullGpuSamples;
		++sample.completed; ++sample.fullReadySamples;
		if (publishEvery && ticks%publishEvery == 0) ++sample.published;
		if (adoptEvery && ticks%adoptEvery == 0) { ++sample.fullAdopted; ++sample.fullAdoptionSamples; }
		if (lateEvery && ticks%lateEvery == 0) ++sample.late;
		if (ticks%8 == 0) ++sample.coverageSamples;
		if (deliveryEvery && ticks%deliveryEvery == 0) ++sample.contentDelivered;
		return policy.Observe(sample);
	}
	Resolution UntilProposal(unsigned frames = 1200) {
		for (unsigned i = 0; i < frames; ++i) {
			const auto result = Step();
			if (result != policy.Active()) return result;
		}
		return policy.Active();
	}
	void Hold(unsigned frames) {
		for (unsigned i = 0; i < frames; ++i) assert(Step() == policy.Active());
	}
	void HoldWindows(unsigned windows) {
		const auto end = policy.LastObservation().sequence + windows;
		const double deadline = sample.now + 4 * windows;
		while (policy.LastObservation().sequence < end && sample.now < deadline) Hold(1);
		assert(policy.LastObservation().sequence == end);
	}
};

static void CheckRequiredEvidence() {
	// Repeated display updates, sparse queries and invalid samples never create
	// a valid GPU control window.
	Harness redraw; redraw.sample.newCapture = false; redraw.sample.foregroundGpuMs = 100;
	redraw.Hold(1000);
	Harness sparse;
	for (unsigned i=0; i<600; ++i) {
		sparse.sample.now += 1./60; sparse.sample.captureTime += 1./60;
		assert(sparse.policy.Observe(sparse.sample) == sparse.policy.Active());
	}
	Harness duplicateQuery;
	duplicateQuery.sample.foregroundGpuMs = 100;
	duplicateQuery.sample.publicationMs = 100;
	duplicateQuery.sample.gpuSamples = 1;
	duplicateQuery.freshGpu = false;
	duplicateQuery.Hold(600);
	Harness invalid;
	invalid.sample.foregroundGpuMs = std::numeric_limits<double>::quiet_NaN();
	invalid.Hold(600);

	// Sustained GPU pressure and late publication must span two observations.
	Harness ordinary;
	ordinary.sample.foregroundGpuMs = 16;
	ordinary.sample.publicationMs = 18;
	ordinary.HoldWindows(1);
	assert(ordinary.UntilProposal().full == 70);
	assert(ordinary.policy.LastObservation().sequence == 2);
	assert(ordinary.policy.Active().full == 75);
	ordinary.Commit({70});
	ordinary.Hold(20);
	Harness oneSpike(100);
	oneSpike.Hold(100);
	oneSpike.sample.foregroundGpuMs = oneSpike.sample.publicationMs = 50; oneSpike.Hold(1);
	oneSpike.sample.foregroundGpuMs = 5;
	oneSpike.sample.publicationMs = 6;
	oneSpike.Hold(200);
	Harness timely(90);
	timely.sample.foregroundGpuMs = 30; timely.sample.hotResolution = {85};
	timely.HoldWindows(6);

	Harness foregroundOverload;
	foregroundOverload.Async(); foregroundOverload.sample.foregroundGpuMs = 19;
	foregroundOverload.sample.publicationMs = 20;
	foregroundOverload.HoldWindows(1);
	assert(foregroundOverload.UntilProposal() == (Resolution{70}));
	assert(foregroundOverload.policy.LastReason() == Reason::ForegroundOverload);
	Harness fixedBottleneck;
	fixedBottleneck.sample.foregroundGpuMs = 1; fixedBottleneck.sample.fixedGpuMs = 20;
	fixedBottleneck.HoldWindows(3);
	assert(fixedBottleneck.policy.LastReason() == Reason::FixedBottleneck);
	Harness slowDelivery;
	slowDelivery.deliveryEvery = 2;
	slowDelivery.HoldWindows(6);
	assert(slowDelivery.policy.LastReason() == Reason::Delivery);

	Harness flowUnknown;
	flowUnknown.sample.fixedGpuKnown = false; flowUnknown.HoldWindows(6);
	Harness invalidFixed;
	invalidFixed.sample.fixedGpuMs = std::numeric_limits<double>::quiet_NaN(); invalidFixed.HoldWindows(6);
}

static void CheckPipelineWindows() {
	// Late results with slow full computation lower full. Reprojection rejects
	// with healthy compute timings must not masquerade as a full GPU overload.
	Harness late;
	late.Async(15,18); late.lateEvery = 5;
	assert(late.UntilProposal(180) == (Resolution{70}));
	assert(late.policy.LastReason() == Reason::FullDeadline);
	Harness localReject;
	localReject.Async(); localReject.lateEvery = 5;
	localReject.sample.enhancementCoverage = .2;
	localReject.HoldWindows(6);
	assert(localReject.policy.LastReason() == Reason::Quality);

	// Publication, adoption and useful pixel weights are independent upgrade
	// gates. Ready/adoption latency use their observation window, not the last
	// conveniently fast completion.
	Harness neverPublished;
	neverPublished.Async(); neverPublished.publishEvery = 0; neverPublished.adoptEvery = 0;
	neverPublished.HoldWindows(6);
	Harness unusedResults;
	unusedResults.Async(); unusedResults.adoptEvery = 2;
	unusedResults.HoldWindows(6);
	assert(unusedResults.policy.LastReason() == Reason::Quality);
	Harness weakResults;
	weakResults.Async(); weakResults.sample.enhancementCoverage = .6;
	weakResults.HoldWindows(6);
	Harness adoptionDelay;
	adoptionDelay.Async(); adoptionDelay.sample.fullAdoptionMs = 40;
	adoptionDelay.HoldWindows(6);
	Harness readyTail;
	readyTail.Async();
	for (unsigned i=0; i<600; ++i) {
		readyTail.sample.fullReadyMs = i%5 == 0 ? 20 : 6;
		assert(readyTail.Step() == readyTail.policy.Active());
	}
	readyTail.sample.fullReadyMs = 6;
	assert(readyTail.Step() == readyTail.policy.Active());
	Harness recovered;
	recovered.Async(); recovered.sample.enhancementCoverage = .2; recovered.HoldWindows(6);
	recovered.sample.enhancementCoverage = 1; recovered.HoldWindows(3);
	assert(recovered.UntilProposal() == (Resolution{80}));

	Harness pipeline;
	pipeline.Async();
	assert(pipeline.UntilProposal() == (Resolution{80}));
}

static void CheckWarmTiersAndResets() {
	Harness headroom;
	headroom.HoldWindows(3);
	assert(headroom.UntilProposal() == (Resolution{80}));
	assert(headroom.policy.RequiresHotResolution());
	Harness coldUpgrade;
	coldUpgrade.sample.hasHotResolution = false; coldUpgrade.HoldWindows(6);
	assert(coldUpgrade.policy.LastReason() == Reason::ColdUpgrade);
	Harness lostHot;
	lostHot.HoldWindows(3);
	lostHot.sample.hasHotResolution = false; lostHot.HoldWindows(2);
	assert(lostHot.policy.LastReason() == Reason::ColdUpgrade);
	lostHot.sample.hasHotResolution = true;
	lostHot.HoldWindows(3);
	assert(lostHot.UntilProposal() == (Resolution{80}));

	// The inference component scales with area. Fixed work is counted once.
	Harness fixedCost(25);
	fixedCost.sample.foregroundGpuMs = 7;
	fixedCost.sample.fixedGpuMs = 3;
	assert(fixedCost.UntilProposal() == (Resolution{30}));
	Harness upperTooCostly(25);
	upperTooCostly.sample.foregroundGpuMs = upperTooCostly.sample.fullGpuMs = 10.5;
	upperTooCostly.HoldWindows(6);

	Harness failed;
	failed.sample.foregroundGpuMs = failed.sample.publicationMs = 25;
	assert(failed.UntilProposal().full == 70);
	failed.policy.Reject(failed.sample.now); failed.Hold(240);
	assert(failed.UntilProposal().full == 70);
	Harness resetHistory;
	resetHistory.Async(); resetHistory.HoldWindows(3);
	resetHistory.sample.reset = true; resetHistory.Hold(1);
	resetHistory.sample.reset = false; resetHistory.HoldWindows(3);
	assert(resetHistory.UntilProposal().full == 80);
	Harness edited;
	edited.HoldWindows(3);
	edited.policy.InvalidateMeasurements(edited.sample.now);
	edited.HoldWindows(3);
	assert(edited.UntilProposal().full == 80);
	Harness resetCounters;
	resetCounters.Async(); resetCounters.HoldWindows(2); resetCounters.Hold(15);
	resetCounters.sample.completed = resetCounters.sample.published = resetCounters.sample.fullAdopted = 0;
	resetCounters.HoldWindows(4);
	assert(resetCounters.UntilProposal().full == 80);
	Harness floor(25);
	floor.sample.foregroundGpuMs = floor.sample.publicationMs = 200; floor.HoldWindows(4);
	Harness ceiling(100);
	ceiling.HoldWindows(6);
	Harness quantized;
	quantized.policy.Reset({77});
	assert(quantized.policy.Active() == (Resolution{75}));
}

static void CheckFollowSourceCadence() {
	// Mixed short and long intervals can carry a healthy 120 FPS stream.
	// Set a budget from their effective cadence without treating the short
	// gaps as a faster sustained target.
	Harness variable(100);
	variable.sample.explicitTarget = 0;
	const double ticksAt300Hz[]{1, 2, 4, 3, 2.5};
	for (unsigned i = 0; i < 1200; ++i)
		assert(variable.Step(ticksAt300Hz[i % 5] / 300) == variable.policy.Active());
	assert(std::abs(variable.policy.Target() - 120) < 2);
	Harness irregularTail(100);
	irregularTail.sample.explicitTarget = 0;
	irregularTail.sample.foregroundGpuMs = 7.8;
	for (unsigned i = 0; i < 1200; ++i)
		assert(irregularTail.Step((i % 5 == 4 ? 1.5 : 1.) / 120) == irregularTail.policy.Active());
	assert(std::abs(irregularTail.policy.Target() - 120 / 1.1) < 1);

	// A sustained change takes two cadence windows.
	Harness warming(100);
	warming.sample.explicitTarget = 0;
	warming.HoldWindows(2);
	warming.ExpectTarget(60);
	warming.Serve(120);
	warming.HoldWindows(1);
	warming.ExpectTarget(60);
	warming.HoldWindows(1);
	warming.ExpectTarget(120);

	// A slightly fast first window cannot suppress an upgrade indefinitely
	// when new captures keep arriving and the GPU has plenty of spare time.
	Harness modestSlowdown(55);
	modestSlowdown.sample.explicitTarget = 0;
	modestSlowdown.sample.foregroundGpuMs = 2.3;
	modestSlowdown.sample.fullGpuMs = 1.5;
	modestSlowdown.Serve(118.5);
	modestSlowdown.HoldWindows(1);
	modestSlowdown.Serve(110);
	const auto up = modestSlowdown.UntilProposal();
	assert(up.full == 60 && modestSlowdown.policy.RequiresHotResolution());
	modestSlowdown.ExpectTarget(118.5);
	modestSlowdown.Commit(up);
	modestSlowdown.sample.hotResolution = {55};
	modestSlowdown.HoldWindows(2);
	assert(modestSlowdown.policy.LastReason() == Reason::TrialAccepted);
}

static void CheckFailedUpgrade() {
	Harness h(80);
	h.Serve(120);
	h.sample.explicitTarget = h.sample.captureLimit = 120;
	constexpr std::array noise{-.2, 0., .2};
	auto stepAt80 = [&](double mean) {
		h.sample.foregroundGpuMs = mean + noise[h.ticks % noise.size()];
		h.sample.fullGpuMs = h.sample.foregroundGpuMs - .5;
		h.sample.publicationMs = h.sample.foregroundGpuMs + .6;
		return h.Step();
	};
	auto tryUpgrade = [&](double mean) {
		for (unsigned i = 0; i < 720; ++i) {
			const auto next = stepAt80(mean);
			if (next != h.policy.Active()) {
				assert(next.full == 85 && h.policy.RequiresHotResolution());
				h.Commit(next);
				return;
			}
		}
		assert(false && "Healthy 80 percent tier did not propose the warm 85 percent tier");
	};
	tryUpgrade(5.6);
	h.sample.hotResolution = {80};
	h.sample.foregroundGpuMs = 9.4;
	h.sample.fullGpuMs = 8.7;
	h.sample.publicationMs = 10.1;
	h.HoldWindows(1);
	const auto restore = h.UntilProposal();
	assert(restore.full == 80 && h.policy.LastReason() == Reason::RestoreResolution);
	assert(h.policy.RequiresHotResolution());
	h.Commit(restore);

	h.sample.hotResolution = {85};
	for (unsigned i = 0; i < 120 * 120; ++i) assert(stepAt80(5.6).full == 80);
	assert(h.switches == 2 && h.policy.Active().full == 80);
	h.ExpectTarget(120);

	tryUpgrade(3.7);
	h.sample.hotResolution = {80};
	h.sample.foregroundGpuMs = 6.1;
	h.sample.fullGpuMs = 5.6;
	h.sample.publicationMs = 7.1;
	h.HoldWindows(2);
	assert(h.policy.LastReason() == Reason::TrialAccepted);
	h.HoldWindows(8);
	assert(h.switches == 3 && h.policy.Active().full == 85);
	h.ExpectTarget(120);

	// Slow captures can hide an upgrade's throughput loss from the late and
	// coalescing checks. Compare it against the delivery before the trial.
	Harness deliveryLoss(80);
	deliveryLoss.Serve(120);
	deliveryLoss.sample.explicitTarget = 120;
	const auto upper = deliveryLoss.UntilProposal();
	assert(upper.full == 85);
	deliveryLoss.Commit(upper);
	deliveryLoss.sample.hotResolution = {80};
	deliveryLoss.Serve(96);
	deliveryLoss.HoldWindows(1);
	assert(deliveryLoss.UntilProposal().full == 80);
	assert(deliveryLoss.policy.LastReason() == Reason::RestoreResolution);
}

static void CheckFailedDownProbe() {
	Harness h(85);
	h.Serve(120);
	h.sample.explicitTarget = 0;
	h.sample.hotResolution = {80};
	h.sample.foregroundGpuMs = 9.1;
	h.sample.fullGpuMs = 8.5;
	h.sample.publicationMs = 7.5;
	h.HoldWindows(1);
	const auto probe = h.UntilProposal();
	assert(probe.full == 80 && h.policy.RequiresHotResolution());
	h.Commit(probe);

	h.sample.hotResolution = {85};
	h.sample.foregroundGpuMs = 8.65;
	h.sample.fullGpuMs = 8;
	h.HoldWindows(1);
	const auto restore = h.UntilProposal();
	assert(restore.full == 85 && h.policy.LastReason() == Reason::RestoreResolution);
	h.Commit(restore);

	h.sample.hotResolution = {80};
	h.sample.foregroundGpuMs = 9.1;
	h.sample.fullGpuMs = 8.5;
	h.Hold(120 * 90);
	h.HoldWindows(1);
	assert(h.switches == 2 && h.policy.LastReason() == Reason::RetryBlocked);

	h.sample.foregroundGpuMs = 10.8;
	h.sample.fullGpuMs = 10.1;
	h.sample.publicationMs = 11.2;
	const auto down = h.UntilProposal();
	assert(down.full == 80 && !h.policy.RequiresHotResolution());
	h.Commit(down);

	h.sample.hotResolution = {85};
	h.sample.foregroundGpuMs = 6.4;
	h.sample.fullGpuMs = 5.8;
	h.sample.publicationMs = 7.1;
	h.HoldWindows(2);
	assert(h.policy.LastReason() == Reason::TrialAccepted);
	h.HoldWindows(8);
	assert(h.switches == 3 && h.policy.Active().full == 80);
}

static void CheckFixedTargetBoundary() {
	Harness loaded(100);
	loaded.sample.explicitTarget = loaded.sample.captureLimit = 120;
	auto setTier = [&](double fps, double work, double scalable) {
		loaded.Serve(fps);
		loaded.sample.foregroundGpuMs = work;
		loaded.sample.fullGpuMs = scalable;
	};
	auto holdObservation = [&](unsigned latePercent, double lateMs, Resolution expected,
		std::source_location caller = std::source_location::current()) {
		const auto sequence = loaded.policy.LastObservation().sequence;
		Resolution next;
		do {
			loaded.sample.publicationMs = loaded.ticks * 37 % 100 < latePercent ? lateMs : 7.5;
			next = loaded.Step();
		} while (loaded.policy.LastObservation().sequence == sequence);
		if (next != expected)
			std::cerr << "Line " << caller.line() << ": expected " << expected.full << "%, got " << next.full
				<< "% (" << DLSSNRAdaptiveResolution::ReasonName(loaded.policy.LastReason()) << ")\n";
		assert(next == expected);
	};

	// The real two-pass capture improved GPU cost from 100% to 95% before
	// throughput improved. At 90%, more frames arrived even when almost
	// every publication sat just past the 8.33 ms deadline.
	setTier(58.4, 16.87, 13.18);
	holdObservation(0, 10.0, {100});
	holdObservation(0, 10.1, {95});
	assert(loaded.policy.LastReason() == Reason::ForegroundOverload);
	loaded.Commit({95});
	loaded.sample.hotResolution = {100};
	setTier(59.1, 16.47, 12.63);
	holdObservation(0, 10.0, {95});
	holdObservation(0, 10.1, {95});
	assert(loaded.policy.LastReason() == Reason::TrialAccepted);
	setTier(60.6, 16.43, 12.63);
	holdObservation(60, 10.0, {95});
	holdObservation(50, 10.2, {90});
	loaded.Commit({90}, 188);
	loaded.sample.hotResolution = {95};
	setTier(65.3, 15.03, 11.21);
	holdObservation(100, 10.6, {90});
	holdObservation(100, 10.6, {90});
	assert(loaded.policy.LastReason() == Reason::TrialAccepted);
	loaded.ExpectTarget(120);

	// A down trial that saves little at unchanged throughput may roll back
	// once. Deadline classifications alone cannot unlock the same boundary.
	loaded.policy.Reset({80}, loaded.sample.now);
	loaded.sample.hotResolution = {75};
	setTier(73.6, 13.14, 9.03);
	holdObservation(25, 9.9, {80});
	holdObservation(20, 9.9, {75});
	loaded.Commit({75}, 149);
	loaded.sample.hotResolution = {80};
	setTier(74.8, 12.72, 8.65);
	holdObservation(20, 9.9, {75});
	holdObservation(25, 9.9, {80});
	assert(loaded.policy.LastReason() == Reason::RestoreResolution);
	loaded.Commit({80});
	loaded.sample.hotResolution = {75};
	setTier(73.6, 13.14, 9.03);
	const auto steadyStart = loaded.sample.now;
	while (loaded.sample.now - steadyStart < 120) {
		const unsigned late = loaded.policy.LastObservation().sequence % 5 < 2 ? 100 : 20;
		holdObservation(late, 10.1, {80});
	}
	assert(loaded.policy.LastReason() == Reason::RetryBlocked);

	// A single change in scaled GPU load is insufficient to try again.
	setTier(73.6, 14.50, 10.50);
	holdObservation(90, 10.8, {80});
	setTier(73.6, 13.17, 9.07);
	holdObservation(90, 10.8, {80});
	setTier(73.6, 14.50, 10.50);
	holdObservation(90, 10.8, {80});
	holdObservation(90, 10.8, {75});
	assert(loaded.policy.LastReason() == Reason::ForegroundOverload);

	// Savings in GPU time alone buy at most one confirmed step. With the
	// same throughput and processing latency, the next trial must roll back.
	loaded.policy.Reset({100}, loaded.sample.now);
	loaded.sample.hotResolution = {95};
	setTier(59, 16.8, 13.20);
	holdObservation(0, 10.1, {100});
	holdObservation(0, 10.1, {95});
	loaded.Commit({95});
	loaded.sample.hotResolution = {100};
	setTier(59, 16.4, 12.60);
	holdObservation(0, 10.1, {95});
	holdObservation(0, 10.1, {95});
	assert(loaded.policy.LastReason() == Reason::TrialAccepted);
	holdObservation(0, 10.1, {95});
	holdObservation(0, 10.1, {90});
	loaded.Commit({90});
	loaded.sample.hotResolution = {95};
	setTier(59, 15.6, 11.65);
	holdObservation(0, 10.1, {90});
	holdObservation(0, 10.1, {95});
	assert(loaded.policy.LastReason() == Reason::RestoreResolution);
}

static void CheckChangingSourceRate() {
	Harness slower(100);
	slower.sample.explicitTarget = 0;
	slower.Serve(144);
	slower.HoldWindows(2);
	slower.ExpectTarget(144);
	slower.Serve(60);
	slower.sample.foregroundGpuMs = 10;
	slower.sample.publicationMs = 12;
	slower.HoldWindows(1);
	slower.ExpectTarget(144);
	slower.HoldWindows(6);
	slower.ExpectTarget(60);
	assert(slower.policy.Active().full == 100);

	Harness warmed(80);
	warmed.sample.explicitTarget = 0;
	warmed.sample.hotResolution = {100};
	warmed.sample.foregroundGpuMs = 5.5;
	warmed.sample.fullGpuMs = 5;
	warmed.Serve(144);
	warmed.HoldWindows(4);
	warmed.ExpectTarget(144);
	warmed.Serve(60);
	const auto upgrade = warmed.UntilProposal();
	assert(upgrade.full == 100 && warmed.policy.RequiresHotResolution());
	warmed.ExpectTarget(60);
	warmed.Commit(upgrade);
	warmed.HoldWindows(2);
	assert(warmed.policy.LastReason() == Reason::TrialAccepted);

	Harness configured(80);
	configured.sample.explicitTarget = configured.sample.captureLimit = 120;
	configured.sample.foregroundGpuMs = 6;
	configured.sample.publicationMs = 7.3;
	configured.Serve(60);
	const auto lowerRateUpgrade = configured.UntilProposal();
	assert(lowerRateUpgrade.full == 85);
	configured.ExpectTarget(60);
	configured.Commit(lowerRateUpgrade);
	configured.sample.hotResolution = {80};
	configured.HoldWindows(3);
	configured.Serve(120);
	configured.HoldWindows(1);
	configured.ExpectTarget(60);
	configured.HoldWindows(3);
	configured.ExpectTarget(120);
	assert(configured.switches == 1);
}

static void CheckAlternatingDelivery() {
	Harness h(100);
	h.sample.explicitTarget = 0;
	h.sample.updateInterval = 3;
	h.sample.hotResolution = {95};
	h.Serve(120);
	for (unsigned i = 0; i < 120 * 30; ++i) {
		h.sample.fullInference = i % 3 == 1;
		h.sample.foregroundGpuMs = h.sample.fullInference ? 14.3 : 2.4;
		h.sample.fullGpuMs = 12.3;
		h.sample.publicationMs = i % 157 == 14 ? 10 : 6.2;
		const double elapsed = i % 421 == 320 ? .65 : i % 137 == 126 ? .04 : h.interval;
		assert(h.Step(elapsed).full == 100);
	}
	assert(h.policy.LastObservation().sequence >= 12);
	assert(std::abs(h.policy.LastObservation().gpuMs - (14.3 + 2 * 2.4) / 3) < .001);
	h.ExpectTarget(120, 4);
}

static void CheckSourcePauseAndCandidateWindows() {
	for (bool explicitGoal : {false, true}) {
		Harness h(100);
		h.sample.explicitTarget = explicitGoal ? 120. : 0.;
		h.Serve(60);
		h.HoldWindows(4);
		h.ExpectTarget(60);
		assert(h.Step(.65).full == 100);
		h.Serve(120);
		h.HoldWindows(4);
		h.ExpectTarget(120);
	}

	// The new 60 FPS source is healthy even though its local work would miss
	// the old 144 FPS budget and has little spare time at the actual cadence.
	Harness slower(100);
	slower.sample.explicitTarget = 0;
	slower.sample.foregroundGpuMs = slower.sample.publicationMs = 5;
	slower.Serve(144);
	slower.HoldWindows(3);
	slower.ExpectTarget(144);
	slower.sample.foregroundGpuMs = slower.sample.publicationMs = 14.5;
	slower.Serve(60);
	slower.HoldWindows(5);
	slower.ExpectTarget(60);

	// Three good windows at 85% are not three good predictions for 100%.
	Harness tiers(80);
	tiers.sample.hasRecoveryResolution = true;
	tiers.sample.recoveryResolution = {100};
	tiers.sample.foregroundGpuMs = 11.4;
	tiers.sample.fullGpuMs = 4;
	tiers.HoldWindows(3);
	tiers.sample.foregroundGpuMs = 11.0;
	tiers.HoldWindows(1);
	assert(tiers.UntilProposal().full == 100);
}

static void CheckSuccessfulDescentAndRecovery() {
	Harness h(100);
	h.sample.explicitTarget = 0;
	h.sample.hasHotResolution = false;
	h.sample.foregroundGpuMs = 6.2;
	h.Serve(120);
	h.HoldWindows(3);
	h.ExpectTarget(120);

	// The accepted FPS drops with expensive work. Coalesced captures identify
	// the queue pressure while admitted publications still meet the budget.
	h.Serve(100);
	h.sample.foregroundGpuMs = 9.6;
	h.sample.fullGpuMs = 8.5;
	h.sample.publicationMs = 8;
	h.HoldWindows(2);
	h.ExpectTarget(120);
	h.sample.captureQueued = true;
	h.HoldWindows(1);
	const auto firstDown = h.UntilProposal();
	assert(firstDown.full == 95 && h.policy.LastReason() == Reason::ForegroundOverload);
	assert(!h.policy.RequiresHotResolution());
	h.Commit(firstDown);

	h.Serve(144);
	h.sample.captureQueued = false;
	h.sample.foregroundGpuMs = 6.2;
	h.sample.fullGpuMs = 5.5;
	h.sample.publicationMs = 7.9;
	h.sample.hasRecoveryResolution = true;
	h.sample.recoveryResolution = {100};
	h.HoldWindows(2);
	assert(h.policy.LastReason() == Reason::TrialAccepted);
	h.HoldWindows(12);
	h.ExpectTarget(120);
	assert(h.switches == 1);

	h.Serve(96);
	h.sample.captureQueued = true;
	h.sample.foregroundGpuMs = 9.8;
	h.sample.fullGpuMs = 8.7;
	h.sample.publicationMs = 10.1;
	const auto secondDown = h.UntilProposal();
	assert(secondDown.full == 90);
	h.Commit(secondDown);
	h.Serve(144);
	h.sample.captureQueued = false;
	h.sample.foregroundGpuMs = 5.6;
	h.sample.fullGpuMs = 4.8;
	h.sample.publicationMs = 6.6;
	h.sample.hasHotResolution = true;
	h.sample.hotResolution = {95};
	h.HoldWindows(2);
	assert(h.policy.LastReason() == Reason::TrialAccepted);
	h.HoldWindows(6);
	h.ExpectTarget(120);

	h.sample.foregroundGpuMs = 3.5;
	h.sample.fullGpuMs = 3;
	h.sample.publicationMs = 4.7;
	const auto recover = h.UntilProposal();
	assert(recover.full == 100 && h.policy.RequiresHotResolution());
	h.Commit(recover);
	h.sample.foregroundGpuMs = 5.4;
	h.sample.fullGpuMs = 4.8;
	h.sample.publicationMs = 6.9;
	h.HoldWindows(2);
	assert(h.policy.LastReason() == Reason::TrialAccepted);
	h.HoldWindows(6);
	h.ExpectTarget(120);
	assert(h.switches == 3 && h.policy.Active().full == 100);
}

static void CheckParameterRules() {
	std::map<std::wstring, float> old{{L"inputResolutionPercent", 65.f}};
	assert(NormalizeDLSSNRDetailParameters(old));
	assert(old.at(L"resolutionControl") == 1.f && old.at(L"inputResolutionPercent") == 65.f);
	std::map<std::wstring, float> added{{L"resolutionControl", 0.f}};
	NormalizeDLSSNRDetailParameters(added);
	assert(added.at(L"resolutionControl") == 0.f);
	assert(!NormalizeDLSSNRDetailParameters(added));
	std::map<std::string, float> values{{"performanceShowAdvanced", 1.f}, {"resolutionControl", 0.f}};
	auto get = [&](std::string_view name, float fallback) {
		auto it = values.find(std::string(name)); return it == values.end() ? fallback : it->second;
	};
	constexpr auto effect = "DLSSNR\\DLSSNR_AI_Filter";
	assert(!IsEffectParameterEnabled(effect, "inputResolutionPercent", false, get));
	values["resolutionControl"] = 1;
	assert(IsEffectParameterEnabled(effect, "inputResolutionPercent", false, get));
	values["performanceShowAdvanced"] = 0;
	assert(!IsEffectParameterVisible(effect, "inputResolutionPercent", get));
	assert(!IsEffectParameterVisible(effect, "resolutionControl", get));
}

int main() {
	CheckRequiredEvidence();
	CheckPipelineWindows();
	CheckWarmTiersAndResets();
	CheckFollowSourceCadence();
	CheckFailedUpgrade();
	CheckFailedDownProbe();
	CheckFixedTargetBoundary();
	CheckChangingSourceRate();
	CheckAlternatingDelivery();
	CheckSourcePauseAndCandidateWindows();
	CheckSuccessfulDescentAndRecovery();
	CheckParameterRules();
	std::cout << "DLSSNR adaptive cadence, measured trial outcomes, phase costs and parameter rules passed\n";
}
