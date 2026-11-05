#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <vector>
#include "DLSSNRParameters.h"
#include "DLSSNRSparseState.h"

namespace Magpie {
struct MotionVectorRequest { int method = 0; bool operator==(const MotionVectorRequest&) const = default; };
struct DlssnrExperimentProtocol { bool enabled = false; float scale = 1; };
struct EffectOption { std::map<std::string, float> parameters; };
inline MotionVectorRequest ParseDlssOpticalFlowRequest(const EffectOption& option) {
	const auto it = option.parameters.find("opticalFlowMethod");
	return {it == option.parameters.end() ? 0 : static_cast<int>(it->second)};
}
#include "DLSSNRSettingsUnderTest.h"
enum class EffectParameterApplyMode { Live, RestartRequired, Unavailable, Restricted };
struct DLSSNRFilter {
	struct Impl {
		std::vector<std::unique_ptr<Impl>> laterPasses;
		uint64_t evaluateParameterRevision = 0;
		bool resetHistory = false, residualParametersDirty = false, disabled = false;
	};
	std::unique_ptr<Impl> _impl = std::make_unique<Impl>();
	struct Sparse {
		struct Result { DLSSNRSparseObservationClock::Result observation; };
		std::array<Result,4> results;
		bool invalidated = false;
		uint64_t compositionRevision = 1;
	};
	std::unique_ptr<Sparse> _sparse;
	std::vector<DLSSNRSettings> _passSettings;
	DLSSNRSettings _settings;
	unsigned hotInvalidations = 0;
	std::vector<bool> submissionWaits;
	EffectParameterApplyMode GetParameterApplyMode(std::string_view) const noexcept;
	bool ApplyLiveParameters(const EffectOption&, std::span<const std::string>) noexcept;
	bool ServiceInferenceSubmission(bool wait = false) noexcept { submissionWaits.push_back(wait); return true; }
	void UpdatePerformanceParameters(std::span<const std::string>, bool) noexcept {}
	void UpdateSparseParameters(bool, bool) noexcept;
	void InvalidateHotResolution() noexcept { ++hotInvalidations; }
};
#include "DLSSNRLiveUnderTest.h"
}

int main() {
	using namespace Magpie;
	DLSSNRFilter filter;
	EffectOption option{{{"enableInputResolutionScaling", 1.f}, {"multiPass", 3.f}, {"pass2_intensity", .7f}}};
	for (int i = 1; i <= 3; ++i) filter._passSettings.push_back(ParseDLSSNRSettings(DLSSNRPassOption(option, i)));
	filter._settings = filter._passSettings.front();
	for (int i = 0; i < 2; ++i) filter._impl->laterPasses.push_back(std::make_unique<DLSSNRFilter::Impl>());
	auto& first = *filter._impl;
	auto& second = *first.laterPasses[0];
	auto& third = *first.laterPasses[1];
	assert(filter.GetParameterApplyMode("modelPreset") == EffectParameterApplyMode::RestartRequired);
	option.parameters["modelPreset"] = 1;
	const std::vector<std::string> modelNames{"modelPreset"};
	assert(!filter.ApplyLiveParameters(option,modelNames));
	assert(first.evaluateParameterRevision == 0 && filter._settings.modelPreset == 0);
	option.parameters["modelPreset"] = 0;
	std::vector<std::string> names{"residualSaturation", "residualLightness"};
	option.parameters["residualSaturation"] = .3f;
	option.parameters["residualLightness"] = 1.2f;
	assert(filter.ApplyLiveParameters(option, names));
	assert(first.residualParametersDirty && !first.resetHistory && !second.resetHistory && !third.resetHistory);
	assert(first.evaluateParameterRevision == 0 && second.evaluateParameterRevision == 0 && third.evaluateParameterRevision == 0);
	assert(filter._settings.residualSaturation == .3f && filter._settings.residualLightness == 1.2f);
	assert(!filter.submissionWaits.empty() && !filter.submissionWaits.back());
	for (const auto name : DLSSNR_RESIDUAL_PARAMETERS) {
		names = {std::string(name)};
		option.parameters[std::string(name)] = 1;
		assert(filter.ApplyLiveParameters(option,names));
		assert(first.evaluateParameterRevision == 0 && second.evaluateParameterRevision == 0 && third.evaluateParameterRevision == 0);
	}
	names = {"pass3_intensity"}; option.parameters["pass3_intensity"] = .4f;
	assert(filter.ApplyLiveParameters(option, names));
	assert(first.evaluateParameterRevision == 0 && second.evaluateParameterRevision == 0 && third.evaluateParameterRevision == 1);
	assert(filter.submissionWaits.back());
	assert(!first.resetHistory && !second.resetHistory && third.resetHistory);
	names = {"pass2_intensity"}; option.parameters["pass2_intensity"] = .6f;
	assert(filter.ApplyLiveParameters(option, names));
	assert(!first.resetHistory && second.resetHistory && third.resetHistory);
	assert(filter._passSettings[0].intensity == 1 && filter._passSettings[1].intensity == .6f && filter._passSettings[2].intensity == .4f);
	// Reject the complete transaction before touching revisions, flags or values.
	names = {"intensity", "inputResolutionPercent"}; option.parameters["intensity"] = .2f;
	const auto revision = first.evaluateParameterRevision;
	assert(!filter.ApplyLiveParameters(option, names));
	assert(first.evaluateParameterRevision == revision && filter._passSettings[0].intensity == 1);
	names = {"intensity"}; option.parameters["inputResolutionPercent"] = 50;
	assert(!filter.ApplyLiveParameters(option, names));
	assert(first.evaluateParameterRevision == revision);
	option.parameters["inputResolutionPercent"] = 100;
	assert(filter.ApplyLiveParameters(option, names));
	assert(first.resetHistory && first.evaluateParameterRevision == revision + 1);
	assert(filter.ApplyLiveParameters(option, names));
	assert(first.evaluateParameterRevision == revision + 1); // no effective change
	// Hidden settings are saved without dirtying active passes.
	filter._passSettings.resize(1); first.laterPasses.clear();
	names = {"pass3_intensity"};
	const auto hiddenRevision = first.evaluateParameterRevision;
	assert(filter.ApplyLiveParameters(option, names));
	assert(first.evaluateParameterRevision == hiddenRevision);
	filter._settings.experimentalHdr.enabled = true;
	assert(filter.GetParameterApplyMode("residualSaturation") == EffectParameterApplyMode::Restricted);
	first.disabled = true;
	assert(filter.GetParameterApplyMode("intensity") == EffectParameterApplyMode::Unavailable);
	{
		// Legacy internal state is explicit; public options use supported modes.
		DLSSNRFilter sparse;
		sparse._sparse = std::make_unique<DLSSNRFilter::Sparse>();
		EffectOption route{{{"processingMode",1.f},{"resolutionControl",1.f},
			{"residualTransfer",1.f},{"opticalFlowMethod",2.f},
			{"sparseUpdateInterval",3.f}}};
		sparse._settings = ParseDLSSNRSettings(route);
		sparse._passSettings = {sparse._settings};
		const std::vector<std::string> edit{"intensity"};
		for (const auto key : {"sparseUpdateInterval"}) {
			auto changedTiming = route;
			changedTiming.parameters[key] = 4.f;
			changedTiming.parameters["intensity"] = .3f;
			assert(!sparse.ApplyLiveParameters(changedTiming,edit));
			assert(!sparse._sparse->invalidated && sparse._sparse->compositionRevision == 1 && sparse._impl->evaluateParameterRevision == 0);
			assert(sparse._settings.intensity == 1 && sparse._settings.sparseUpdateInterval == 3);
		}
		route.parameters["intensity"] = .3f;
		assert(sparse.ApplyLiveParameters(route,edit));
		assert(sparse._sparse->invalidated && sparse._impl->evaluateParameterRevision == 1 && sparse._settings.intensity == .3f);
		sparse._sparse->invalidated = false;
		auto& unobserved = sparse._sparse->results[0].observation;
		auto& observed = sparse._sparse->results[1].observation;
		assert(unobserved.Begin(3,1,1,4,5));
		assert(observed.Begin(2,1,2,3,4)); observed.observed = true;
		route.parameters["residualSaturation"] = .3f;
		const std::vector<std::string> residualEdit{"residualSaturation"};
		assert(sparse.ApplyLiveParameters(route,residualEdit));
		assert(!sparse._sparse->invalidated && sparse._sparse->compositionRevision == 2);
		assert(sparse._impl->evaluateParameterRevision == 1 && !sparse.submissionWaits.back());
		assert(!unobserved.ready && !unobserved.target && observed.observed && !observed.ready);
		assert(unobserved.Begin(3,1,1,4,6)); // the same target can confirm its new controls.
		assert(sparse.ApplyLiveParameters(route,residualEdit)); // same effective value
		assert(!sparse._sparse->invalidated && sparse._sparse->compositionRevision == 2 && unobserved.ready == 6);
		route.parameters["residualShowAdvanced"] = 1;
		assert(sparse.ApplyLiveParameters(route,std::vector<std::string>{"residualShowAdvanced"}));
		route.parameters["pass3_intensity"] = .5f;
		assert(sparse.ApplyLiveParameters(route,std::vector<std::string>{"pass3_intensity"}));
		assert(!sparse._sparse->invalidated && sparse._sparse->compositionRevision == 2);
		route.parameters["intensity"] = .5f;
		assert(sparse.ApplyLiveParameters(route,edit));
		assert(sparse._sparse->invalidated && sparse._impl->evaluateParameterRevision == 2 && sparse._settings.intensity == .5f);
	}
	for (int mode = 0; mode <= 1; ++mode) {
		DLSSNRFilter recovery;
		EffectOption route{{{"processingMode",float(mode)},{"resolutionControl",1.f},
			{"residualTransfer",1.f},{"opticalFlowMethod",2.f}}};
		recovery._settings = ParseDLSSNRSettings(route);
		recovery._passSettings = {recovery._settings};
		// Retired serialized controls cannot alter the effective live settings.
		route.parameters["recoveryPolicy"] = 2;
		route.parameters["bufferedDelayFrames"] = 2;
		route.parameters["asyncResidualComposition"] = 0;
		route.parameters["intensity"] = .3f;
		const std::vector<std::string> liveEdit{"intensity"};
		assert(recovery.ApplyLiveParameters(route,liveEdit));
		assert(recovery._impl->evaluateParameterRevision == 1);
		assert(recovery._settings.intensity == .3f);
	}
	std::cout << "Production live transaction: NR isolation, dependent history resets, post-only reuse, rejection and hidden settings passed.\n";
}
