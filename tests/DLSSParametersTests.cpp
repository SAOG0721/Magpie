#include <windows.h>
#include <winrt/base.h>
#include <array>
#include <ranges>
#include <string>
#include <vector>
#include <map>
#include <cassert>
#include <iostream>
#include "DLSSSRParameters.h"
#include "EffectParameterRules.h"
#include "EffectParameterValue.h"
#include "EffectFrameState.h"
#include "dlss_parameter_descriptors.h"

using namespace Magpie;
struct Option { std::map<std::string, float> parameters; };

int main() {
	Option option;
	assert(ParseDLSSSRPreset(option) == 11);
	auto preset = SrPresetDescriptor();
	for (int value : {0,1,2,3,4,5,6,10,11,12,13}) {
		option.parameters["renderPreset"] = float(value);
		assert(ParseDLSSSRPreset(option) == value);
		assert(NormalizeEffectParameterValue(preset, float(value)) == value);
	}
	for (float bad : {-1.f, 7.f, 8.f, 9.f, 14.f, 15.f, 1.5f, 10.5f, 1e30f,
		std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
		option.parameters["renderPreset"] = bad;
		assert(ParseDLSSSRPreset(option) == 11);
		assert(NormalizeEffectParameterValue(preset, bad) == 11);
	}
	// Display order is not storage identity.
	std::ranges::reverse(preset.choices);
	assert(NormalizeEffectParameterValue(preset, 10) == 10);
	assert(DLSSSRPresetName(0) == "Auto" && DLSSSRPresetName(11) == "K");
	auto time = NrTimeDescriptor();
	for (int value = -20; value <= 2300; ++value) {
		const float ms = value / 10.f;
		assert(NormalizeDLSSNRAntiFlickerTimeMs(ms) == NormalizeEffectParameterValue(time, ms));
	}
	for (float bad : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
		assert(NormalizeDLSSNRAntiFlickerTimeMs(bad) == 80);
		assert(NormalizeEffectParameterValue(time, bad) == 80);
	}
	option.parameters.clear();
	auto get = [&](std::string_view key, float fallback) {
		auto it = option.parameters.find(std::string(key));
		return it == option.parameters.end() ? fallback : it->second;
	};
	assert(DLSSNRAntiFlickerTimeMs(get) == 80);
	for (int mode = 0; mode <= 4; ++mode) {
		option.parameters["antiFlicker"] = float(mode);
		assert(IsEffectParameterVisible("DLSSNR\\DLSSNR_AI_Filter", "antiFlickerTimeMs", get) == (mode != 0));
		assert(IsEffectParameterVisible("Custom", "antiFlickerTimeMs", get));
	}
	// Both parameter editors enumerate these shipped choices. Retired modes
	// cannot appear as choices or survive descriptor normalization.
	const auto nrMode = NrModeDescriptor();
	assert(nrMode.choices.size() == 2);
	assert(nrMode.choices[0].value == 0 && nrMode.choices[1].value == 1);
	for (float mode : {0.f, 1.f}) assert(NormalizeEffectParameterValue(nrMode, mode) == mode);
	for (float retired : {2.f, 3.f}) assert(NormalizeEffectParameterValue(nrMode, retired) == 0);
	const auto interval = NrIntervalDescriptor();
	assert(interval.choices.size() == 3);
	for (size_t i = 0; i < interval.choices.size(); ++i) {
		assert(interval.choices[i].value == int(i) + 2);
		assert(NormalizeEffectParameterValue(interval, float(i) + 2) == float(i) + 2);
	}
	constexpr auto nrEffect = "DLSSNR\\DLSSNR_AI_Filter";
	for (float mode : {0.f, 1.f}) {
		option.parameters = {{"processingMode", mode}, {"performanceShowAdvanced", 1.f},
			{"resolutionControl", 0.f}, {"opticalFlowMethod", 0.f}, {"antiFlicker", 2.f}};
		assert(IsEffectParameterVisible(nrEffect, "processingMode", get));
		assert(IsEffectParameterVisible(nrEffect, "sparseUpdateInterval", get) == (mode == 1));
		assert(IsEffectParameterVisible(nrEffect, "residualTransfer", get) == (mode == 1));
		assert(IsEffectParameterVisible(nrEffect, "antiFlickerTimeMs", get));
		assert(IsDLSSNRChoiceEnabled("resolutionControl", 0, get));
		assert(IsDLSSNRChoiceEnabled("resolutionControl", 1, get));
		assert(IsDLSSNRChoiceEnabled("residualTransfer", 0, get));
		assert(!IsDLSSNRChoiceEnabled("residualTransfer", 1, get));
		option.parameters["opticalFlowMethod"] = 1;
		assert(IsDLSSNRChoiceEnabled("residualTransfer", 1, get));
		option.parameters["performanceShowAdvanced"] = 0;
		assert(!IsEffectParameterVisible(nrEffect, "processingMode", get));
		assert(!IsEffectParameterVisible(nrEffect, "sparseUpdateInterval", get));
		assert(IsEffectParameterVisible("Custom", "sparseUpdateInterval", get));
	}
	option.parameters = {{"processingMode", 1.f}, {"resolutionControl", 0.f}};
	NormalizeDLSSNRParameterEdit("processingMode", get, [&](std::string_view name, float value) {
		option.parameters[std::string(name)] = value;
	});
	assert(option.parameters.at("resolutionControl") == 0); // Alternating retains adaptive resolution.
	// Time-only edits leave the renderer cache/epoch untouched on a duplicate.
	EffectFrameState frame;
	EffectFrameKey key{.frameId=1, .inputRevision=1, .captureSequence=1, .resourceGeneration=1};
	frame.Commit(key, true);
	const auto epoch = frame.OutputHistoryRevision();
	if (DLSSNRParameterChangesCurrentOutput("antiFlickerTimeMs", 3)) frame.ParametersChanged();
	key.parameterRevision = frame.ParameterRevision();
	assert(!frame.NeedsDraw(key, false) && frame.OutputHistoryRevision() == epoch);
	++key.frameId; ++key.inputRevision;
	assert(frame.NeedsDraw(key, false) && !frame.RequiresHistoryReset(key));
	assert(DLSSNRParameterChangesCurrentOutput("intensity", 1));
	assert(!DLSSNRParameterChangesCurrentOutput("pass3_intensity", 1));
	std::cout << "DLSS preset identities/defaults, shipped two-mode NR choices, interval visibility, independent resolution/flow controls, UI/backend normalization and duplicate-frame history preservation passed.\n";
}
