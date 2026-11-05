#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <map>
#include <limits>
#include <string>
#include "DLSSNRParameters.h"
#include "EffectParameterRestart.h"

namespace Magpie {
struct MotionVectorRequest { int method = 0; };
struct DlssnrExperimentProtocol { bool enabled = false; float scale = 1; };
struct EffectOption { std::map<std::string, float> parameters; };
inline MotionVectorRequest ParseDlssOpticalFlowRequest(const EffectOption& option) {
	const auto it = option.parameters.find("opticalFlowMethod");
	return {it == option.parameters.end() ? 0 : static_cast<int>(it->second)};
}
#include "DLSSNRSettingsUnderTest.h"
}

int main() {
	using namespace Magpie;
	// Raw runtime options already use current values. Stored maps are migrated
	// once at the persistence boundary, with the marker surviving every copy.
	static_assert(int(DLSSNRProcessingMode::EveryFrame) == 0);
	static_assert(int(DLSSNRProcessingMode::Alternating) == 1);
	static_assert(int(DLSSNRProcessingMode::Pipeline) == 2);
	static_assert(int(DLSSNRProcessingMode::SparseEconomy) == 3);
	static_assert(DLSSNR_MODE_VERSION == 2.f);
	assert(ParseDLSSNRSettings({}).sparseUpdateInterval == 2);
	constexpr std::array legacyModes{0.f,1.f,0.f,0.f,0.f,1.f,0.f};
	constexpr std::array versionedModes{0.f,1.f,0.f,1.f};
	for (int version : {0,1,2}) for (float mode : {-1.f,0.f,1.f,2.f,3.f,4.f,5.f,6.f,9.f,1.5f,
		std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()})
		for (int present : {0,1}) for (float interval : {2.f,3.f,4.f,0.f,2.5f,5.f,
			std::numeric_limits<float>::infinity(),std::numeric_limits<float>::quiet_NaN()}) {
		std::map<std::wstring,float> saved{{L"processingMode",mode},
			{L"modelPreset",2.f},{L"pass2_intensity",1.5f},{L"pass3_style",2.f},
			{L"multiPass",3.f},{L"inputResolutionPercent",75.f},{L"resolutionControl",1.f},
			{L"residualTransfer",1.f},{L"opticalFlowMethod",2.f},
			{L"nvidiaOpticalFlowQuality",5.f},{L"antiFlicker",4.f},{L"antiFlickerTimeMs",95.f},
			{L"residualChromaProtection",.75f},
			{L"recoveryPolicy",2.f},{L"bufferedDelayFrames",1.f},{L"asyncResidualComposition",0.f}};
		if (version) saved[L"__dlssnrModeVersion"] = float(version);
		if (present) saved[L"sparseUpdateInterval"] = interval;
		const bool inRange = std::isfinite(mode) && mode >= 0 && mode == std::floor(mode) &&
			mode < float(version ? versionedModes.size() : legacyModes.size());
		const float expectedMode = !inRange ? 0.f : version ?
			versionedModes[size_t(mode)] : legacyModes[size_t(mode)];
		const float expectedInterval = !version && mode == 1.f ? 2.f :
			(present && (interval == 2.f || interval == 3.f || interval == 4.f) ? interval : 2.f);
		assert(NormalizeDLSSNRDetailParameters(saved));
		assert(saved.at(L"processingMode") == expectedMode && saved.at(L"sparseUpdateInterval") == expectedInterval);
		assert(saved.at(L"__dlssnrModeVersion") == DLSSNR_MODE_VERSION);
		for (const auto key : {L"recoveryPolicy",L"bufferedDelayFrames",L"asyncResidualComposition"})
			assert(!saved.contains(key));
		assert(saved.at(L"modelPreset") == 2 && saved.at(L"pass2_intensity") == 1.5f && saved.at(L"pass3_style") == 2);
		assert(saved.at(L"inputResolutionPercent") == 75 && saved.at(L"multiPass") == 3);
		assert(saved.at(L"opticalFlowMethod") == 2 && saved.at(L"nvidiaOpticalFlowQuality") == 5);
		assert(saved.at(L"antiFlicker") == 4 && saved.at(L"antiFlickerTimeMs") == 95);
		assert(saved.at(L"residualTransfer") == 1 && saved.at(L"residualChromaProtection") == .75f);
		const auto roundTrip = saved;
		assert(!NormalizeDLSSNRDetailParameters(saved) && saved == roundTrip);
		auto copied = saved;
		assert(!NormalizeDLSSNRDetailParameters(copied) && copied == saved);
		EffectOption imported;
		for (const auto& [name,value] : copied) {
			std::string key;
			for (wchar_t character : name) key.push_back(static_cast<char>(character));
			imported.parameters.emplace(std::move(key),value);
		}
		std::map<std::wstring,float> exported;
		for (const auto& [name,value] : imported.parameters)
			exported.emplace(std::wstring(name.begin(),name.end()),value);
		assert(!NormalizeDLSSNRDetailParameters(exported) && exported == saved);
		auto restarted = exported;
		assert(!NormalizeDLSSNRDetailParameters(restarted) && restarted == saved);
		assert(DLSSNRPassCount([&](std::string_view name,float fallback) {
			const auto it=imported.parameters.find(std::string(name));
			return it==imported.parameters.end()?fallback:it->second;
		}) == 3);
		for (int pass : {1,2,3}) {
			const auto option = DLSSNRPassOption(imported,pass);
			const auto parsed = ParseDLSSNRSettings(option);
			assert(int(parsed.processingMode) == int(expectedMode));
			assert(parsed.sparseUpdateInterval == unsigned(expectedInterval));
			assert(parsed.modelPreset == 2 && parsed.inputResolutionPercent == 75);
			assert(option.parameters.at("__dlssnrModeVersion") == DLSSNR_MODE_VERSION);
		}
	}
	for (float invalidVersion : {-1.f,0.f,.5f,3.f,std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN()}) {
		std::map<std::wstring,float> saved{{L"__dlssnrModeVersion",invalidVersion},{L"processingMode",2.f}};
		assert(NormalizeDLSSNRDetailParameters(saved));
		assert(saved.at(L"__dlssnrModeVersion") == DLSSNR_MODE_VERSION && saved.at(L"processingMode") == 0);
		assert(!NormalizeDLSSNRDetailParameters(saved));
	}
	for (float mode : {0.f,1.f,2.f,3.f}) {
		EffectOption raw{{{"processingMode",mode},{"opticalFlowMethod",2.f},{"residualTransfer",1.f}}};
		const auto clean = raw;
		const int expectedMode = mode == 1.f || mode == 3.f ? 1 : 0;
		assert(int(ParseDLSSNRSettings(raw).processingMode) == expectedMode);
		for (const auto key : {"recoveryPolicy","bufferedDelayFrames","asyncResidualComposition"})
			raw.parameters[key] = std::numeric_limits<float>::quiet_NaN();
		assert(SameDLSSNRPerformanceControls(raw,clean));
		assert(IsDLSSNRPerformanceConfigurationValid([&](std::string_view name,float fallback) {
			const auto it=raw.parameters.find(std::string(name));
			return it==raw.parameters.end()?fallback:it->second;
		}) == (mode == 0.f || mode == 1.f));
		const auto parsed = ParseDLSSNRSettings(raw);
		assert(int(parsed.processingMode) == expectedMode && parsed.sparseUpdateInterval == 2);
	}
	// Upgrade, import and save all share the actual migration helper.
	for (int legacy : {-1, 0, 1}) for (int percent : {-1, 25, 75, 100}) {
		std::map<std::wstring,float> saved{{L"intensity",.6f},{L"pass2_useAutoMask",1.f}};
		if (legacy >= 0) saved[L"enableInputResolutionScaling"] = float(legacy);
		if (percent >= 0) saved[L"inputResolutionPercent"] = float(percent);
		NormalizeDLSSNRDetailParameters(saved);
		assert(saved.at(L"enableInputResolutionScaling") == 1);
		assert(saved.at(L"inputResolutionPercent") == (percent < 0 ? 100.f : float(percent)));
		assert(saved.at(L"intensity") == .6f && saved.at(L"pass2_useAutoMask") == 1);
		const auto roundTrip = saved;
		assert(!NormalizeDLSSNRDetailParameters(saved) && saved == roundTrip);
	}
	for (float value : {-100.f, 1000.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
		std::map<std::wstring,float> saved{{L"enableInputResolutionScaling",value},{L"inputResolutionPercent",value}};
		NormalizeDLSSNRDetailParameters(saved);
		assert(saved.at(L"enableInputResolutionScaling") == 1);
		assert(saved.at(L"inputResolutionPercent") >= 25 && saved.at(L"inputResolutionPercent") <= 100);
	}
	for (float value : {0.f, 1.f, 2.f, -1.f, std::numeric_limits<float>::quiet_NaN()})
		for (float processingMode : {0.f, 1.f, 2.f, 3.f}) {
		std::map<std::wstring,float> saved{{L"opticalFlowScaleOrder",value},
			{L"resolutionControl",1.f},{L"inputResolutionPercent",75.f},
			{L"modelPreset",2.f},{L"processingMode",processingMode},{L"__dlssnrModeVersion",1.f},{L"residualTransfer",1.f},
			{L"opticalFlowMethod",2.f},{L"nvidiaOpticalFlowQuality",5.f}};
		NormalizeDLSSNRDetailParameters(saved);
		assert(!saved.contains(L"opticalFlowScaleOrder"));
		const float expectedMode = processingMode == 1.f || processingMode == 3.f ? 1.f : 0.f;
		assert(saved.at(L"modelPreset") == 2 && saved.at(L"processingMode") == expectedMode);
		assert(saved.at(L"resolutionControl") == 1 && saved.at(L"inputResolutionPercent") == 75);
		assert(saved.at(L"residualTransfer") == 1 && saved.at(L"opticalFlowMethod") == 2 &&
			saved.at(L"nvidiaOpticalFlowQuality") == 5);
		const auto roundTrip = saved;
		assert(!NormalizeDLSSNRDetailParameters(saved) && saved == roundTrip);
		EffectOption option;
		for (const auto& [name, parameter] : saved) {
			std::string key;
			for (wchar_t character : name) key.push_back(static_cast<char>(character));
			option.parameters.emplace(key, parameter);
		}
		assert(IsDLSSNRPerformanceConfigurationValid([&](std::string_view name, float fallback) {
			const auto it = option.parameters.find(std::string(name));
			return it == option.parameters.end() ? fallback : it->second;
		}));
		const auto parsed = ParseDLSSNRSettings(option);
		assert(int(parsed.processingMode) == int(expectedMode));
		const auto cleanOption = option;
		option.parameters["opticalFlowScaleOrder"] = value;
		assert(SameDLSSNRPerformanceControls(option, cleanOption));
		const auto legacy = ParseDLSSNRSettings(option);
		assert(legacy.motionRequest.method == parsed.motionRequest.method &&
			legacy.inputResolutionPercent == parsed.inputResolutionPercent && legacy.modelPreset == parsed.modelPreset);
	}
	for (float model : {0.f, 1.f, 2.f}) {
		EffectOption shared{{{"modelPreset",model},{"enableInputResolutionScaling",0.f},{"inputResolutionPercent",75.f},
			{"residualChromaProtection",.75f}}};
		for (int pass : {1, 2, 3}) {
			const auto settings = ParseDLSSNRSettings(DLSSNRPassOption(shared,pass));
			assert(settings.modelPreset == int(model) && settings.enableInputResolutionScaling && settings.inputResolutionPercent == 75);
			assert(settings.residualChromaProtection == .75f);
		}
	}
	EffectOption option{{{"style", 2.f}, {"intensity", 0.5f}, {"localToneStrength", 0.25f},
		{"localStructureStrength", 0.75f}, {"skinStructureStrength", 2.f},
		{"useAutoMask", 1.f}, {"uiCorrection", 1.f}, {"opticalFlowMethod", 2.f},
		{"enableInputResolutionScaling", 1.f}, {"inputResolutionPercent", 75.f}}};
	const DLSSNRSettings defaults;
	for (int pass : {2, 3}) {
		const auto fresh = ParseDLSSNRSettings(DLSSNRPassOption(option, pass), true);
		assert(fresh.style == defaults.style && fresh.intensity == defaults.intensity);
		assert(fresh.localToneStrength == defaults.localToneStrength);
		assert(fresh.localStructureStrength == defaults.localStructureStrength);
		assert(fresh.skinStructureStrength == defaults.skinStructureStrength);
		assert(fresh.useAutoMask == defaults.useAutoMask && fresh.uiCorrection == defaults.uiCorrection);
		assert(fresh.motionRequest.method == 2 && fresh.experimentalHdr.enabled);
		assert(fresh.enableInputResolutionScaling && fresh.inputResolutionPercent == 75);
	}
	option.parameters["pass2_style"] = 1;
	option.parameters["pass2_intensity"] = 1.75f;
	option.parameters["pass3_skinStructureStrength"] = 1.5f;
	const auto first = ParseDLSSNRSettings(DLSSNRPassOption(option, 1));
	const auto second = ParseDLSSNRSettings(DLSSNRPassOption(option, 2));
	const auto third = ParseDLSSNRSettings(DLSSNRPassOption(option, 3));
	assert(first.style == 2 && first.intensity == 0.5f && first.skinStructureStrength == 2);
	assert(second.style == 1 && second.intensity == 1.75f && second.skinStructureStrength == 0);
	assert(third.style == 0 && third.intensity == 1 && third.skinStructureStrength == 1.5f);
	// Restoring one parameter cannot change the other two passes.
	option.parameters["pass2_intensity"] = defaults.intensity;
	assert(ParseDLSSNRSettings(DLSSNRPassOption(option, 2)).intensity == 1);
	assert(ParseDLSSNRSettings(DLSSNRPassOption(option, 1)).intensity == 0.5f);
	assert(ParseDLSSNRSettings(DLSSNRPassOption(option, 3)).skinStructureStrength == 1.5f);
	// Old mode selectors are ignored; numeric controls keep their semantics.
	option.parameters["residualColorMode"] = 1;
	option.parameters["residualMultiplier"] = 0;
	option.parameters["residualHueProtection"] = 2;
	option.parameters["residualDarkProtection"] = std::numeric_limits<float>::quiet_NaN();
	option.parameters["residualLowFrequencyGain"] = -1;
	option.parameters["residualChromaTemporalStrength"] = 1.f;
	auto detail = ParseDLSSNRSettings(option);
	assert(detail.residualMultiplier == 0);
	assert(detail.residualHueProtection == 1 && detail.residualDarkProtection == 0);
	assert(detail.residualLowFrequencyGain == 0 && detail.residualDetailGain == 1);
	assert(detail.residualChromaTemporalStrength == 0 && detail.residualDebugView == 0);
	option.parameters["residualColorMode"] = 0.5f;
	for (float obsoleteMode : {0.f, 1.f, .5f, -3.f, std::numeric_limits<float>::quiet_NaN()}) {
		option.parameters["residualColorMode"] = obsoleteMode;
		const auto migrated = ParseDLSSNRSettings(option);
		assert(migrated.residualMultiplier == detail.residualMultiplier);
		assert(migrated.residualSaturation == detail.residualSaturation);
		assert(migrated.residualHueProtection == detail.residualHueProtection);
		assert(migrated.style == detail.style && migrated.intensity == detail.intensity);
	}
	// Alternating and the retired raw value's fallback retain the chosen interval
	// in every pass, while manual percent and pass-local settings remain intact.
	for (int mode : {1,3}) for (float interval : {2.f,3.f,4.f}) for (float quality : {1.f,5.f}) {
		EffectOption route{{{"processingMode",float(mode)},{"resolutionControl",1.f},
			{"residualTransfer",1.f},{"opticalFlowMethod",2.f},{"nvidiaOpticalFlowQuality",quality},
			{"sparseUpdateInterval",interval},{"inputResolutionPercent",75.f}}};
		const auto preserved=route.parameters;
		assert(IsDLSSNRPerformanceConfigurationValid([&](std::string_view name,float fallback) {
			const auto it=route.parameters.find(std::string(name));
			return it==route.parameters.end()?fallback:it->second;
		}) == (mode == 1));
		for (int pass : {1,2,3}) {
			const auto parsed=ParseDLSSNRSettings(DLSSNRPassOption(route,pass));
			assert(parsed.processingMode==DLSSNRProcessingMode::Alternating && parsed.inputResolutionPercent==75);
			assert(parsed.sparseUpdateInterval==uint32_t(interval) && parsed.motionRequest.method==2);
		}
		assert(route.parameters==preserved);
	}
	for (int mode : {1,3}) {
		EffectOption route{{{"processingMode",float(mode)}}};
		const auto parsed=ParseDLSSNRSettings(route);
		assert(parsed.sparseUpdateInterval==2 && route.parameters.size()==1);
	}
	EffectParameterRestartQueue queue;
	const auto now = EffectParameterRestartQueue::Clock::now();
	assert(queue.Update(0, "multiPass", 2, 1, now));
	assert(!queue.ReadyToStop(now, false));
	assert(queue.ReadyToStop(now + std::chrono::milliseconds(300), false));
	assert(!queue.ReadyToStop(now + std::chrono::milliseconds(300), true));
	const auto changes = queue.TakeChanges();
	assert(changes.size() == 1 && changes.front().parameter == "multiPass");
	queue.WaitAfterStop(now);
	assert(!queue.ReadyToStart(now + std::chrono::milliseconds(499)));
	assert(queue.ReadyToStart(now + std::chrono::milliseconds(500)));
	std::cout << "DLSSNR settings: historical/v1/two-mode migration, idempotent boundaries, runtime fallbacks, preserved controls, HDR and restart debounce passed.\n";
}
