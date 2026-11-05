#include "DlssOpticalFlowParameters.h"
#include "EffectParameterRules.h"
#include "MotionVectorRequest.h"
#include "OpticalFlowDefaults.h"
#include <vector>
#include <cassert>
#include <iostream>
#include <limits>
#include <map>
#include <algorithm>
#include <span>

namespace Magpie {
struct EffectOption { std::map<std::string, float, std::less<>> parameters; };
struct MotionSettings {
	MotionVectorRequest motionRequest;
	float gain = 0.08f;
	uint32_t inputResolutionPercent = 100;
};
enum class EffectParameterRestartReason { FrameGuidance, ResourceRecreation, NativeBackend };
enum class EffectParameterApplyMode { Live, RestartRequired };
struct DLSSNRFilter {
	struct Impl { bool disabled = false; };
	Impl* _impl = nullptr;
	MotionSettings _settings;
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept;
	EffectParameterRestartReason GetParameterRestartReason(std::string_view) const noexcept;
};
struct DLSSFrameGenerator {
	MotionSettings _requestedSettings;
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept;
};
struct FrameGuidanceDiagnostics {
	MotionSettings _settings;
	FrameGuidanceRequirements GetFrameGuidanceRequirements() const noexcept;
	EffectParameterApplyMode GetParameterApplyMode(std::string_view) const noexcept;
	EffectParameterRestartReason GetParameterRestartReason(std::string_view) const noexcept;
	bool ApplyLiveParameters(const EffectOption&, std::span<const std::string>) noexcept;
};
}
#include "dlss_optical_flow_production.h"

struct StoredEffect {
	std::wstring name;
	std::map<std::wstring, float, std::less<>> parameters;
	int scalingType = 3;
	float scale = 1.5f;
};

int main() {
	using namespace Magpie;
	const auto nvBalanced = MotionVectorRequest::Nvidia(NvidiaOpticalFlowQuality::Balanced);
	for (auto id : {L"DLSSNR\\DLSSNR_AI_Filter", L"DLSSFG\\DLSS_FrameGeneration"}) {
		for (int quality = 0; quality <= 5; ++quality) {
			StoredEffect effect{id, {{L"motionVectorQuality", float(quality)}, {L"other", 0.25f}}};
			assert(MigrateDlssOpticalFlowParameters(effect));
			assert(effect.parameters.at(L"opticalFlowMethod") == (quality ? 2 : 0));
			assert(effect.parameters.at(L"nvidiaOpticalFlowQuality") == (quality ? quality : 2));
			assert(!effect.parameters.contains(L"motionVectorQuality"));
			assert(effect.parameters.at(L"other") == 0.25f && effect.scalingType == 3 && effect.scale == 1.5f);
			assert(!MigrateDlssOpticalFlowParameters(effect));
			EffectOption runtime;
			for (const auto& [name, value] : effect.parameters) {
				std::string key;
				for (wchar_t c : name) key.push_back(static_cast<char>(c));
				runtime.parameters[key] = value;
			}
			assert(ParseDlssOpticalFlowRequest(runtime) == MotionVectorRequest::Nvidia(
				static_cast<NvidiaOpticalFlowQuality>(quality)));
		}
		for (float enabled : {0.0f, 1.0f}) {
			StoredEffect legacy{id, {{L"useMotionVectors", enabled}}};
			assert(MigrateDlssOpticalFlowParameters(legacy));
			assert(legacy.parameters.at(L"opticalFlowMethod") == (enabled ? 2 : 0));
			assert(!legacy.parameters.contains(L"useMotionVectors"));
		}
		StoredEffect custom{std::wstring(id) + L"_Custom", {{L"motionVectorQuality", 4.0f}}};
		assert(!MigrateDlssOpticalFlowParameters(custom) && custom.parameters.size() == 1);
		StoredEffect fresh{id};
		assert(MigrateDlssOpticalFlowParameters(fresh));
		assert(fresh.parameters.at(L"opticalFlowMethod") == 0);
		StoredEffect selected{id, {{L"opticalFlowMethod", 1.0f}, {L"amdOpticalFlowMode", 0.0f},
			{L"motionVectorQuality", 0.0f}, {L"nvidiaOpticalFlowQuality", 5.0f}}};
		assert(MigrateDlssOpticalFlowParameters(selected));
		assert(selected.parameters.at(L"opticalFlowMethod") == 1 && selected.parameters.at(L"amdOpticalFlowMode") == 0);
		assert(selected.parameters.at(L"nvidiaOpticalFlowQuality") == 5);
		assert(!MigrateDlssOpticalFlowParameters(selected));
	}
	assert(ParseDlssOpticalFlowRequest({}) == MotionVectorRequest{});
	for (float invalid : {-1.0f, 9.0f, 1.5f, std::numeric_limits<float>::infinity(),
		std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max()}) {
		assert(ParseDlssOpticalFlowRequest({{{"motionVectorQuality", invalid}}}) == MotionVectorRequest{});
		assert(ParseDlssOpticalFlowRequest({{{"motionVectorQuality", invalid}, {"useMotionVectors", 0.0f}}}) == MotionVectorRequest{});
		assert(ParseDlssOpticalFlowRequest({{{"opticalFlowMethod", invalid}}}) == MotionVectorRequest{});
		assert(ParseDlssOpticalFlowRequest({{{"opticalFlowMethod", 2.0f}, {"nvidiaOpticalFlowQuality", invalid}}}) == nvBalanced);
		assert(ParseDlssOpticalFlowRequest({{{"motionVectorQuality", 5.0f}, {"nvidiaOpticalFlowQuality", invalid}}}) == nvBalanced);
		assert(ParseDlssOpticalFlowRequest({{{"opticalFlowMethod", 1.0f}, {"amdOpticalFlowMode", invalid}}}) ==
			MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality));
	}
	DLSSNRFilter::Impl impl;
	DLSSNRFilter nr{&impl};
	DLSSFrameGenerator fg;
	FrameGuidanceDiagnostics diagnostic;
	assert(ParseOpticalFlowRequest({}) == MotionVectorRequest{});
	for (int method = 0; method <= 2; ++method) {
		for (int quality = (method == 2 ? 1 : 0); quality <= (method == 2 ? 5 : 1); ++quality) {
			const auto request = ParseDlssOpticalFlowRequest({{{"opticalFlowMethod", float(method)},
				{"amdOpticalFlowMode", float(quality)}, {"nvidiaOpticalFlowQuality", float(quality)}}});
			assert(request.method == static_cast<OpticalFlowMethod>(method));
			nr._settings.motionRequest = fg._requestedSettings.motionRequest = request;
			diagnostic._settings.motionRequest = request;
			const auto nrReq = nr.GetFrameGuidanceRequirements();
			const auto fgReq = fg.GetFrameGuidanceRequirements();
			assert(nrReq == fgReq && nrReq.zero && nrReq.HasMotion() == (method != 0));
			assert(diagnostic.GetFrameGuidanceRequirements() == nrReq);
			if (method) assert(nrReq.Contains(request) && request.quality == quality);
			FrameGuidanceRequirements combined = nrReq;
			combined.Merge(fgReq);
			combined.Merge(diagnostic.GetFrameGuidanceRequirements());
			int providers = 0;
			combined.Resolved().ForEachMotion([&](auto) { ++providers; });
			assert(providers == (method != 0));
			const EffectOption gainUpdate{{{"opticalFlowMethod", float(method)},
				{"amdOpticalFlowMode", float(quality)}, {"nvidiaOpticalFlowQuality", float(quality)}, {"gain", 0.5f}}};
			const std::string gainName[]{"gain"};
			assert(diagnostic.ApplyLiveParameters(gainUpdate, gainName));
			assert(diagnostic._settings.gain == 0.5f);
			auto changedProvider = gainUpdate;
			changedProvider.parameters["opticalFlowMethod"] = float((method + 1) % 3);
			changedProvider.parameters["gain"] = 0.75f;
			assert(!diagnostic.ApplyLiveParameters(changedProvider, gainName));
			assert(diagnostic._settings.gain == 0.5f);
			const std::string providerName[]{"opticalFlowMethod"};
			assert(!diagnostic.ApplyLiveParameters(gainUpdate, providerName));
		}
	}
	impl.disabled = true;
	assert(!nr.GetFrameGuidanceRequirements().Any());
	FrameGuidanceRequirements mixed{.zero = true};
	mixed.Add(MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality));
	mixed.Add(nvBalanced);
	assert(mixed.PreferredMotion() == nvBalanced);
	mixed.Add(MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance));
	mixed.Add(nvBalanced);
	const auto resolved = mixed.Resolved();
	unsigned providers = 0;
	resolved.ForEachMotion([&](auto request) { ++providers; assert(request == nvBalanced); });
	assert(providers == 1 && resolved.Contains(nvBalanced));
	assert(!resolved.Contains(MotionVectorRequest::Amd(AmdOpticalFlowMode::Quality)));
	assert(FrameGuidanceRequirements::ResolveConsumer(
		MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance), nvBalanced) == nvBalanced);
	impl.disabled = false;
	nr._settings.motionRequest = MotionVectorRequest::Amd(AmdOpticalFlowMode::Performance);
	for (unsigned percent : {25u, 75u, 100u}) {
		nr._settings.inputResolutionPercent = percent;
		assert(nr.GetFrameGuidanceRequirements().FirstMotion() == nr._settings.motionRequest);
	}
	assert(FrameGuidanceRequirements::ResolveConsumer({}, nvBalanced) == MotionVectorRequest{});
	for (auto name : {"opticalFlowMethod", "amdOpticalFlowMode", "nvidiaOpticalFlowQuality"}) {
		assert(nr.GetParameterRestartReason(name) == EffectParameterRestartReason::FrameGuidance);
		assert(diagnostic.GetParameterRestartReason(name) == EffectParameterRestartReason::FrameGuidance);
		assert(diagnostic.GetParameterApplyMode(name) == EffectParameterApplyMode::RestartRequired);
	}
	assert(diagnostic.GetParameterApplyMode("gain") == EffectParameterApplyMode::Live);
	for (auto id : {"DLSSNR\\DLSSNR_AI_Filter", "DLSSFG\\DLSS_FrameGeneration", "Diagnostics\\FrameGuidance_Motion", "Diagnostics\\FrameGuidance_Confidence"}) {
		for (int method = 0; method <= 2; ++method) {
			auto get = [&](auto name, float fallback) {
				if (std::string_view(name) == "performanceShowAdvanced") return 1.f;
				return std::string_view(name) == "opticalFlowMethod" ? float(method) : fallback;
			};
			assert(IsEffectParameterVisible(id, "amdOpticalFlowMode", get) == (method == 1));
			assert(IsEffectParameterVisible(id, "nvidiaOpticalFlowQuality", get) == (method == 2));
			assert(IsEffectParameterVisible(id, "opticalFlowMethod", get));
		}
	}
	assert(IsEffectParameterVisible("Custom", "amdOpticalFlowMode", [](auto, float fallback) { return fallback; }));
	struct StoredMode { std::vector<StoredEffect> effects; };
	std::vector<StoredMode> modes{{{{L"DLSSNR\\DLSSNR_AI_Filter", {{L"opticalFlowMethod", 2.0f}}},
		{L"Diagnostics\\FrameGuidance_Confidence", {{L"opticalFlowMethod", 1.0f}}},
		{L"Custom", {{L"opticalFlowMethod", 2.0f}}}}}};
	uint32_t defaultsVersion = 0;
	assert(ApplyOpticalFlowDefaultsMigration(modes, defaultsVersion));
	assert(defaultsVersion == 1 && modes[0].effects[0].parameters.at(L"opticalFlowMethod") == 0);
	assert(modes[0].effects[1].parameters.at(L"opticalFlowMethod") == 0);
	assert(modes[0].effects[2].parameters.at(L"opticalFlowMethod") == 2);
	modes[0].effects[0].parameters[L"opticalFlowMethod"] = 1;
	assert(!ApplyOpticalFlowDefaultsMigration(modes, defaultsVersion));
	assert(modes[0].effects[0].parameters.at(L"opticalFlowMethod") == 1);
	std::cout << "DLSS and motion diagnostic optical flow: migration, defaults, invalid values, AMD/NVIDIA/None routing, shared requests, visibility and live gain isolation passed.\n";
}
