#include "DLSSNRAdaptiveResolution.h"
#include "DLSSNRDetailParameters.h"
#include "DLSSNRPerformanceParameters.h"
#include "EffectParameterRules.h"
#include <cassert>
#include <iostream>
#include <map>
#include <limits>
#include <string>
using namespace Magpie;

using Resolution = DLSSNRAdaptiveResolution::Resolution;
using Reason = DLSSNRAdaptiveResolution::Reason;
struct Harness {
	DLSSNRAdaptiveResolution policy;
	DLSSNRAdaptiveResolution::Sample sample;
	unsigned ticks = 0, deliveryEvery = 1, publishEvery = 1, adoptEvery = 1, lateEvery = 0;
	Harness(unsigned full = 75) {
		policy.Reset({full});
		sample.explicitTarget = 60;
		sample.foregroundGpuMs = 5;
		sample.fullGpuMs = 3;
		sample.fullTurnaroundMs = sample.fullReadyMs = 6;
		sample.fullAdoptionMs = 7;
		sample.enhancementCoverage = 1;
		sample.newCapture = true;
		sample.hasHotResolution = true;
		sample.hotResolution = {full < 100 ? full+5 : full-5};
	}
	void Async(double full = 3, double ready = 6) {
		sample.asynchronous = true;
		sample.fullGpuMs = full; sample.fullReadyMs = ready;
		sample.foregroundGpuMs = 3;
	}
	Resolution Step(double interval = 1./60) {
		sample.now += interval; sample.captureTime += interval;
		++ticks; ++sample.gpuSamples; ++sample.fullGpuSamples;
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
};
int main() {
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
	for (unsigned i=0; i<600; ++i) {
		duplicateQuery.sample.gpuSamples = 0; duplicateQuery.Hold(1);
	}
	Harness invalid;
	invalid.sample.foregroundGpuMs = std::numeric_limits<double>::quiet_NaN();
	invalid.Hold(600);

	// Normal overload uses two observation windows. A severe overload can
	// break the hold interval, but one transient spike cannot.
	Harness ordinary;
	ordinary.sample.foregroundGpuMs = 16;
	assert(ordinary.UntilProposal().full == 70);
	assert(ordinary.policy.Active().full == 75);
	ordinary.policy.Commit({70}, ordinary.sample.now);
	ordinary.Hold(20);
	Harness oneSpike;
	oneSpike.Hold(100);
	oneSpike.sample.foregroundGpuMs = 50; oneSpike.Hold(1);
	oneSpike.sample.foregroundGpuMs = 5; oneSpike.Hold(200);
	Harness panic(90);
	panic.sample.foregroundGpuMs = 30; panic.sample.hotResolution = {70};
	assert(panic.UntilProposal(20) == (Resolution{70}));
	assert(panic.sample.now < .5);
	Harness coldPanic(90);
	coldPanic.sample.foregroundGpuMs = 30; coldPanic.sample.hasHotResolution = false;
	assert(coldPanic.UntilProposal(20) == (Resolution{85}));

	// Foreground inference cost scales with the full working area; shared
	// fixed work stays outside that area prediction.
	Harness foregroundOverload;
	foregroundOverload.Async(); foregroundOverload.sample.foregroundGpuMs = 19;
	assert(foregroundOverload.UntilProposal(30) == (Resolution{70}));
	assert(foregroundOverload.policy.LastReason() == Reason::ForegroundOverload);
	Harness fixedBottleneck;
	fixedBottleneck.sample.foregroundGpuMs = 1; fixedBottleneck.sample.fixedGpuMs = 20;
	fixedBottleneck.Hold(360);
	assert(fixedBottleneck.policy.LastReason() == Reason::FixedBottleneck);

	// Late results with slow full computation lower full. Reprojection rejects
	// with healthy compute timings must not masquerade as a full GPU overload.
	Harness late;
	late.Async(15,18); late.lateEvery = 5;
	assert(late.UntilProposal(180) == (Resolution{70}));
	assert(late.policy.LastReason() == Reason::FullDeadline);
	Harness localReject;
	localReject.Async(); localReject.lateEvery = 5;
	localReject.sample.enhancementCoverage = .2;
	localReject.Hold(1200);
	assert(localReject.policy.LastReason() == Reason::Quality);
	Harness fullPanic(90);
	fullPanic.Async(25,30); fullPanic.sample.hotResolution = {70};
	assert(fullPanic.UntilProposal(30) == (Resolution{70}));
	Harness backgroundPanic(90);
	backgroundPanic.Async(25,30); backgroundPanic.sample.hotResolution = {70};
	assert(backgroundPanic.UntilProposal(30) == (Resolution{70}));

	// Proactive movement needs six sustained seconds, real backend content
	// delivery and an exact hot neighbor. A cold or more distant upper tier
	// never triggers runtime creation.
	Harness headroom;
	headroom.Hold(360);
	assert(headroom.UntilProposal() == (Resolution{80}));
	Harness coldUpgrade;
	coldUpgrade.sample.hasHotResolution = false; coldUpgrade.Hold(1200);
	assert(coldUpgrade.policy.LastReason() == Reason::ColdUpgrade);
	Harness distantUpgrade;
	distantUpgrade.sample.hotResolution = {85}; distantUpgrade.Hold(1200);
	assert(distantUpgrade.policy.LastReason() == Reason::ColdUpgrade);
	Harness lostHot;
	lostHot.Hold(360);
	lostHot.sample.hasHotResolution = false; lostHot.Hold(360);
	assert(lostHot.policy.LastReason() == Reason::ColdUpgrade);
	lostHot.sample.hasHotResolution = true;
	assert(lostHot.UntilProposal() == (Resolution{80}));
	Harness slowDelivery;
	slowDelivery.deliveryEvery = 2; slowDelivery.Hold(1200);
	assert(slowDelivery.policy.LastReason() == Reason::Delivery);
	Harness sourceSlower;
	for (unsigned i=0; i<360; ++i) assert(sourceSlower.Step(1./30) == sourceSlower.policy.Active());
	assert(sourceSlower.policy.LastReason() == Reason::Delivery);
	Harness expensiveSwitch;
	expensiveSwitch.policy.Commit({80}, 0, 250);
	expensiveSwitch.sample.hotResolution = {85};
	expensiveSwitch.Hold(840);
	assert(expensiveSwitch.UntilProposal() == (Resolution{85}));
	Harness afterDowngrade(80);
	afterDowngrade.policy.Commit({75},0);
	afterDowngrade.sample.hotResolution = {80};
	afterDowngrade.Hold(700);
	assert(afterDowngrade.UntilProposal() == (Resolution{80}));
	Harness promptDowngrade(80);
	promptDowngrade.policy.Commit({80},0,250);
	promptDowngrade.sample.foregroundGpuMs = 25;
	assert(promptDowngrade.UntilProposal(30).full == 75);
	Harness flowUnknown;
	flowUnknown.sample.fixedGpuKnown = false; flowUnknown.Hold(1200);
	Harness invalidFixed;
	invalidFixed.sample.fixedGpuMs = std::numeric_limits<double>::quiet_NaN(); invalidFixed.Hold(1200);

	// Publication, adoption and useful pixel weights are independent upgrade
	// gates. Ready/adoption latency use their observation window, not the last
	// conveniently fast completion.
	Harness neverPublished;
	neverPublished.Async(); neverPublished.publishEvery = 0; neverPublished.adoptEvery = 0;
	neverPublished.Hold(1200);
	Harness unusedResults;
	unusedResults.Async(); unusedResults.adoptEvery = 2;
	unusedResults.Hold(1200);
	assert(unusedResults.policy.LastReason() == Reason::Quality);
	Harness weakResults;
	weakResults.Async(); weakResults.sample.enhancementCoverage = .6;
	weakResults.Hold(1200);
	Harness adoptionDelay;
	adoptionDelay.Async(); adoptionDelay.sample.fullAdoptionMs = 40;
	adoptionDelay.Hold(1200);
	Harness readyTail;
	readyTail.Async();
	for (unsigned i=0; i<900; ++i) {
		readyTail.sample.fullReadyMs = i%5 == 0 ? 20 : 6;
		assert(readyTail.Step() == readyTail.policy.Active());
	}
	readyTail.sample.fullReadyMs = 6;
	assert(readyTail.Step() == readyTail.policy.Active());
	Harness recovered;
	recovered.Async(); recovered.sample.enhancementCoverage = .2; recovered.Hold(300);
	recovered.sample.enhancementCoverage = 1; recovered.Hold(300);
	assert(recovered.UntilProposal() == (Resolution{80}));

	// The whole measured foreground follows full area; a fixed GPU cost is
	// added once. Upper tiers still require the existing headroom threshold.
	Harness fullPrediction(25);
	fullPrediction.Async(); fullPrediction.sample.foregroundGpuMs = 8;
	assert(fullPrediction.UntilProposal() == (Resolution{30}));
	Harness upperTooCostly(25);
	upperTooCostly.Async(); upperTooCostly.sample.foregroundGpuMs = 10.5;
	upperTooCostly.Hold(1200);
	Harness pipeline;
	pipeline.Async();
	assert(pipeline.UntilProposal() == (Resolution{80}));

	Harness failed;
	failed.sample.foregroundGpuMs = 25;
	assert(failed.UntilProposal().full == 70);
	failed.policy.Reject(failed.sample.now); failed.Hold(290);
	Harness resetHistory;
	resetHistory.Async(); resetHistory.Hold(360);
	resetHistory.sample.reset = true; resetHistory.Hold(1);
	resetHistory.sample.reset = false; resetHistory.Hold(360);
	assert(resetHistory.UntilProposal().full == 80);
	Harness edited;
	edited.Hold(360); edited.policy.InvalidateMeasurements(edited.sample.now); edited.Hold(360);
	assert(edited.UntilProposal().full == 80);
	Harness resetCounters;
	resetCounters.Async(); resetCounters.Hold(360);
	resetCounters.sample.completed = resetCounters.sample.published = resetCounters.sample.fullAdopted = 0;
	resetCounters.Hold(360);
	assert(resetCounters.UntilProposal().full == 80);
	Harness floor(25);
	floor.sample.foregroundGpuMs = 200; floor.Hold(240);
	Harness ceiling(100);
	ceiling.Hold(1200);
	Harness quantized;
	quantized.policy.Reset({77});
	assert(quantized.policy.Active() == (Resolution{75}));

	// The established source cadence survives backpressure and a history reset.
	Harness cadence;
	cadence.sample.explicitTarget = 0;
	for (unsigned i=0; i<30; ++i) cadence.Step();
	assert(std::abs(cadence.policy.Target()-60) < .001);
	for (unsigned i=0; i<120; ++i) {
		cadence.sample.now += 1./30; cadence.sample.captureTime += 1./30;
		cadence.policy.Observe(cadence.sample);
	}
	assert(std::abs(cadence.policy.Target()-60) < .001);
	cadence.sample.reset = true; cadence.Step();
	assert(std::abs(cadence.policy.Target()-60) < .001);

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
	std::cout << "DLSSNR full-resolution costs, deadline/quality windows, hot tiers and parameter rules passed\n";
}
