#include "pch.h"
#include "AppSettings.h"
#include "EffectHelper.h"
#include "EffectsService.h"
#include "JsonHelper.h"
#include "Logger.h"
#include "ScalingMode.h"
#include "ScalingModesService.h"
#include "StrHelper.h"

using namespace ::Magpie;
using namespace winrt;

namespace Magpie {

ScalingMode& ScalingModesService::GetScalingMode(uint32_t idx) {
	return AppSettings::Get().ScalingModes()[idx];
}

uint32_t ScalingModesService::GetScalingModeCount() {
	return (uint32_t)AppSettings::Get().ScalingModes().size();
}

void ScalingModesService::AddScalingMode(std::wstring_view name, int copyFrom) {
	assert(!name.empty());

	std::vector<ScalingMode>& scalingModes = AppSettings::Get().ScalingModes();
	if (copyFrom < 0) {
		scalingModes.emplace_back().name = name;
	} else {
		scalingModes.emplace_back(scalingModes[copyFrom]).name = name;
	}

	ScalingModeAdded.Invoke(copyFrom < 0 ? EffectAddedWay::Add : EffectAddedWay::Duplicate);

	AppSettings::Get().SaveAsync();
}

static void UpdateProfileAfterRemove(Profile& profile, int removedIdx) {
	if (profile.scalingMode == removedIdx) {
		profile.scalingMode = -1;
	} else if (profile.scalingMode > removedIdx) {
		--profile.scalingMode;
	}
}

void ScalingModesService::RemoveScalingMode(uint32_t index) {
	std::vector<ScalingMode>& scalingModes = AppSettings::Get().ScalingModes();
	scalingModes.erase(scalingModes.begin() + index);

	UpdateProfileAfterRemove(AppSettings::Get().DefaultProfile(), (int)index);
	for (Profile& profile : AppSettings::Get().Profiles()) {
		UpdateProfileAfterRemove(profile, (int)index);
	}

	ScalingModeRemoved.Invoke(index);

	AppSettings::Get().SaveAsync();
}

static void UpdateProfileAfterMove(Profile& profile, int fromIndex, int toIndex) {
	if (profile.scalingMode == fromIndex) {
		profile.scalingMode = toIndex;
	} else if (fromIndex < toIndex &&
		profile.scalingMode > fromIndex && profile.scalingMode <= toIndex) {
		--profile.scalingMode;
	} else if (toIndex < fromIndex &&
		profile.scalingMode >= toIndex && profile.scalingMode < fromIndex) {
		++profile.scalingMode;
	}
}

bool ScalingModesService::MoveScalingMode(uint32_t fromIndex, uint32_t toIndex) {
	std::vector<ScalingMode>& scalingModes = AppSettings::Get().ScalingModes();
	if (fromIndex >= scalingModes.size() || toIndex >= scalingModes.size() || fromIndex == toIndex) {
		return false;
	}

	ScalingMode movedMode = std::move(scalingModes[fromIndex]);
	scalingModes.erase(scalingModes.begin() + fromIndex);
	scalingModes.insert(scalingModes.begin() + toIndex, std::move(movedMode));

	UpdateProfileAfterMove(
		AppSettings::Get().DefaultProfile(),
		(int)fromIndex,
		(int)toIndex
	);
	for (Profile& profile : AppSettings::Get().Profiles()) {
		UpdateProfileAfterMove(profile, (int)fromIndex, (int)toIndex);
	}

	ScalingModeMoved.Invoke(fromIndex, toIndex);

	AppSettings::Get().SaveAsync();
	return true;
}

void ScalingModesService::ResetScalingModes() {
	AppSettings::Get().ResetScalingModes();
	ScalingModesReset.Invoke();
}

static void WriteScalingMode(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer, const ScalingMode& scaleMode) {
	writer.StartObject();
	writer.Key("name");
	writer.String(StrHelper::UTF16ToUTF8(scaleMode.name).c_str());
	if (!scaleMode.effects.empty()) {
		writer.Key("effects");
		writer.StartArray();
		for (const auto& effect : scaleMode.effects) {
			writer.StartObject();
			writer.Key("name");
			writer.String(StrHelper::UTF16ToUTF8(effect.name).c_str());

			if (effect.HasScale()) {
				writer.Key("scalingType");
				writer.Uint((uint32_t)effect.scalingType);
				writer.Key("scale");
				writer.StartObject();
				writer.Key("x");
				writer.Double(effect.scale.first);
				writer.Key("y");
				writer.Double(effect.scale.second);
				writer.EndObject();
			}

			if (!effect.parameters.empty()) {
				writer.Key("parameters");
				writer.StartObject();
				for (const auto& [name, value] : effect.parameters) {
					writer.Key(StrHelper::UTF16ToUTF8(name).c_str());
					writer.Double(value);
				}
				writer.EndObject();
			}

			writer.EndObject();
		}
		writer.EndArray();
	}
	writer.EndObject();
}

void ScalingModesService::Export(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer) const noexcept {
	Export(writer, AppSettings::Get().ScalingModes());
}

void ScalingModesService::Export(rapidjson::PrettyWriter<rapidjson::StringBuffer>& writer,
    const std::vector<ScalingMode>& modes) {
    writer.Key("scalingModes");
    writer.StartArray();

    for (const ScalingMode& scalingMode : modes) {
		WriteScalingMode(writer, scalingMode);
	}

	writer.EndArray();
}

static bool LoadScalingMode(
	const rapidjson::GenericObject<true, rapidjson::Value>& scalingModeObj,
	ScalingMode& scalingMode,
	bool loadingSettings
) {
	if (!JsonHelper::ReadString(scalingModeObj, "name", scalingMode.name)) {
		return false;
	}

	auto effectsNode = scalingModeObj.FindMember("effects");
	if (effectsNode == scalingModeObj.MemberEnd()) {
		return true;
	}

	if (!effectsNode->value.IsArray()) {
		return loadingSettings;
	}

	auto effectsArray = effectsNode->value.GetArray();
	scalingMode.effects.reserve(effectsArray.Size());

	for (const auto& elem : effectsArray) {
		if (!elem.IsObject()) {
			if (loadingSettings) {
				continue;
			} else {
				return false;
			}
		}

		auto elemObj = elem.GetObj();
		EffectItem& effect = scalingMode.effects.emplace_back();

		if (!JsonHelper::ReadString(elemObj, "name", effect.name)) {
			if (loadingSettings) {
				scalingMode.effects.pop_back();
				continue;
			} else {
				return false;
			}
		}
		// Frame Rate Filter used to live in the Utility folder. Keep existing
		// user scaling modes working after moving it to the root effect list.
		if (effect.name == L"Utility\\FrameRate_Filter") {
			effect.name = L"FrameRate_Filter";
		}

		if (!JsonHelper::ReadUInt(elemObj, "scalingType", (uint32_t&)effect.scalingType) && !loadingSettings) {
			return false;
		}

		auto scaleNode = elemObj.FindMember("scale");
		if (scaleNode != elemObj.MemberEnd()) {
			if (scaleNode->value.IsObject()) {
				auto scaleObj = scaleNode->value.GetObj();

				float x, y;
				if (JsonHelper::ReadFloat(scaleObj, "x", x, true)
					&& JsonHelper::ReadFloat(scaleObj, "y", y, true)
					&& x > 0 && y > 0)
				{
					effect.scale = { x,y };
				} else {
					if (!loadingSettings) {
						return false;
					}
				}
			} else {
				if (!loadingSettings) {
					return false;
				}
			}
		}

		auto parametersNode = elemObj.FindMember("parameters");
		if (parametersNode != elemObj.MemberEnd()) {
			if (parametersNode->value.IsObject()) {
				auto paramsObj = parametersNode->value.GetObj();

				effect.parameters.reserve(paramsObj.MemberCount());
				for (const auto& param : paramsObj) {
					if (!param.value.IsNumber()) {
						if (loadingSettings) {
							continue;
						} else {
							return false;
						}
					}

					std::wstring name = StrHelper::UTF8ToUTF16(param.name.GetString());
					effect.parameters[name] = param.value.GetFloat();
				}
			} else {
				if (!loadingSettings) {
					return false;
				}
			}
		}
	}

	return true;
}

struct V065NormalizationStats {
	uint32_t removedDepthParameters = 0;
	uint32_t removedLegacyParameters = 0;
	uint32_t clampedParameters = 0;
	uint32_t migratedGuidanceModes = 0;
	uint32_t removedDepthDiagnostics = 0;
	uint32_t insertedFallbacks = 0;
	uint32_t migratedMotionVectorChoices = 0;
	uint32_t normalizedOpticalFlowChoices = 0;
	uint32_t migratedXeSSMfgSettings = 0;
	uint32_t removedXeSSMfgNvidiaParameters = 0;

	bool Changed() const noexcept {
		return removedDepthParameters || removedLegacyParameters ||
			clampedParameters || migratedGuidanceModes ||
			removedDepthDiagnostics || insertedFallbacks ||
			migratedMotionVectorChoices || normalizedOpticalFlowChoices ||
			migratedXeSSMfgSettings || removedXeSSMfgNvidiaParameters;
	}
};

static V065NormalizationStats NormalizeV065ScalingModes(
	std::vector<ScalingMode>& scalingModes
) noexcept {
	V065NormalizationStats stats;
	for (ScalingMode& scalingMode : scalingModes) {
		const size_t oldEffectCount = scalingMode.effects.size();
		std::erase_if(scalingMode.effects, [](const EffectItem& effect) {
			return effect.name == L"Diagnostics\\FrameGuidance_Depth" ||
				effect.name == L"Diagnostics\\FrameGuidance_DepthResidual";
		});
		stats.removedDepthDiagnostics += static_cast<uint32_t>(
			oldEffectCount - scalingMode.effects.size());

		for (EffectItem& effect : scalingMode.effects) {

			if (effect.name == L"DLSSNR\\DLSSNR_AI_Filter") {
				auto guidanceMode = effect.parameters.find(L"guidanceMode");
				if (guidanceMode != effect.parameters.end()) {
					const int oldMode = std::clamp(
						static_cast<int>(std::lround(guidanceMode->second)), 0, 3);
					effect.parameters.try_emplace(
						L"useMotionVectors", oldMode == 0 || oldMode == 2 ? 1.0f : 0.0f);
					effect.parameters.erase(guidanceMode);
					++stats.migratedGuidanceModes;
				}
				stats.removedDepthParameters += static_cast<uint32_t>(
					effect.parameters.erase(L"depthInferenceInterval"));
				stats.removedLegacyParameters += static_cast<uint32_t>(
					effect.parameters.erase(L"nrPreset"));
				for (std::wstring_view name : {
					L"intensity", L"localToneStrength", L"localStructureStrength" }) {
					auto it = effect.parameters.find(name);
					if (it == effect.parameters.end()) continue;
					const float clamped = std::isfinite(it->second) ?
						std::clamp(it->second, 0.0f, 2.0f) : 1.0f;
					if (clamped != it->second) {
						it->second = clamped;
						++stats.clampedParameters;
					}
				}
			} else if ((effect.name == L"DLSS\\DLSS_SR" && !effect.parameters.contains(L"opticalFlowMethod")) ||
				effect.name == L"DLSSFG\\DLSS_FrameGeneration") {
				stats.removedDepthParameters += static_cast<uint32_t>(
					effect.parameters.erase(L"useEstimatedDepth"));
			}

			const bool isDlssMotionConsumer =
				(effect.name == L"DLSS\\DLSS_SR" && !effect.parameters.contains(L"opticalFlowMethod")) ||
				effect.name == L"DLSSFG\\DLSS_FrameGeneration" ||
				effect.name == L"DLSSNR\\DLSSNR_AI_Filter";
			if (isDlssMotionConsumer) {
				auto quality = effect.parameters.find(L"motionVectorQuality");
				if (quality == effect.parameters.end()) {
					auto legacy = effect.parameters.find(L"useMotionVectors");
					const float migrated = legacy != effect.parameters.end() &&
						legacy->second < 0.5f ? 0.0f : 2.0f;
					effect.parameters[L"motionVectorQuality"] = migrated;
					++stats.migratedMotionVectorChoices;
				} else {
					const int value = std::isfinite(quality->second) ?
						static_cast<int>(std::lround(quality->second)) : -1;
					if (value < 0 || value > 5 || quality->second != float(value)) {
						quality->second = 2.0f;
						++stats.normalizedOpticalFlowChoices;
					}
				}
				stats.removedLegacyParameters += static_cast<uint32_t>(
					effect.parameters.erase(L"useMotionVectors"));
			}

			// r8: collapse SR marker variants; jitter never modified captured pixels.
			for (std::wstring_view family : { L"DLSS", L"FSR2", L"FSR3", L"FSR4", L"XeSS" }) {
				const std::wstring prefix = std::wstring(family) + L"\\" + std::wstring(family);
				const bool legacyZero = effect.name == prefix + L"_ZeroMV" ||
					effect.name == prefix + L"_ZeroMV_Jitter";
				const bool legacyFlow = effect.name == prefix + L"_OpticalFlow";
				if (!legacyZero && !legacyFlow && effect.name != prefix + L"_SR") continue;
				if (legacyZero || legacyFlow) {
					effect.name = prefix + L"_SR";
					effect.parameters[L"opticalFlowMethod"] = legacyZero ? 0.0f : family == L"DLSS" ? 2.0f : 1.0f;
					if (legacyFlow) effect.parameters[family == L"DLSS" ? L"nvidiaOpticalFlowQuality" : L"amdOpticalFlowMode"] = family == L"DLSS" ? 2.0f : 1.0f;
					++stats.migratedMotionVectorChoices;
				}
				if (family == L"DLSS") {
					auto old = effect.parameters.find(L"motionVectorQuality");
					const float quality = old != effect.parameters.end() ? old->second : 2.0f;
					if (effect.parameters.try_emplace(L"opticalFlowMethod", quality == 0.0f ? 0.0f : 2.0f).second) ++stats.migratedMotionVectorChoices;
					effect.parameters.try_emplace(L"nvidiaOpticalFlowQuality", quality >= 1.0f && quality <= 5.0f ? quality : 2.0f);
				}
				for (const auto& [name, minimum, maximum, fallback] : {
					std::tuple{ L"opticalFlowMethod", 0.0f, 2.0f, family == L"DLSS" ? 2.0f : 0.0f },
					std::tuple{ L"amdOpticalFlowMode", 0.0f, 1.0f, 1.0f },
					std::tuple{ L"nvidiaOpticalFlowQuality", 1.0f, 5.0f, 2.0f } }) {
					auto [it, inserted] = effect.parameters.try_emplace(name, fallback);
					if (inserted || !std::isfinite(it->second) || it->second < minimum ||
						it->second > maximum || std::round(it->second) != it->second) {
						it->second = fallback;
						++stats.normalizedOpticalFlowChoices;
					}
				}
				for (std::wstring_view old : { L"motionVectorQuality", L"useMotionVectors", L"useEstimatedDepth", L"enableJitter" })
					stats.removedLegacyParameters += static_cast<uint32_t>(effect.parameters.erase(old));
				break;
			}

			if (effect.name == L"XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV" ||
				effect.name == L"XeSSFG\\XeSS_MultiFrameGeneration_ZeroMV") {
				auto normalizeChoice = [&](std::wstring_view name, int minValue,
					int maxValue, int defaultValue,
					uint32_t& migratedCounter) noexcept {
					auto [it, inserted] = effect.parameters.try_emplace(
						std::wstring(name), static_cast<float>(defaultValue));
					if (inserted) {
						++migratedCounter;
						return;
					}
					const int value = std::isfinite(it->second) ?
						static_cast<int>(std::lround(it->second)) : minValue - 1;
					if (value < minValue || value > maxValue ||
						it->second != static_cast<float>(value)) {
						it->second = static_cast<float>(defaultValue);
						++stats.normalizedOpticalFlowChoices;
					}
				};

				if (effect.name == L"XeSSFG\\XeSS_FrameGeneration_x2_ZeroMV") {
					normalizeChoice(L"opticalFlowMethod", 0, 2, 0,
						stats.migratedMotionVectorChoices);
					normalizeChoice(L"amdOpticalFlowMode", 0, 1, 1,
						stats.migratedMotionVectorChoices);
					normalizeChoice(L"nvidiaOpticalFlowQuality", 1, 5, 2,
						stats.migratedMotionVectorChoices);
				} else {
					// MFG method 2 used to mean NVOF. It must fall back to None,
					// never clamp to AMDOF. MFG x2 is a valid cross-vendor request.
					normalizeChoice(L"opticalFlowMethod", 0, 1, 0,
						stats.migratedXeSSMfgSettings);
					normalizeChoice(L"amdOpticalFlowMode", 0, 1, 1,
						stats.migratedMotionVectorChoices);
					normalizeChoice(L"multiplier", 2, 4, 3,
						stats.migratedXeSSMfgSettings);
					stats.removedXeSSMfgNvidiaParameters +=
						static_cast<uint32_t>(effect.parameters.erase(
							L"nvidiaOpticalFlowQuality"));
				}
			}
		}

		if (oldEffectCount != 0 && scalingMode.effects.empty()) {
			EffectItem& fallback = scalingMode.effects.emplace_back();
			fallback.name = L"Bilinear";
			fallback.scalingType = ScalingType::Fit;
			++stats.insertedFallbacks;
		}
	}
	return stats;
}

bool ScalingModesService::Import(const rapidjson::GenericObject<true, rapidjson::Value>& root, bool loadingSettings) noexcept {
	auto scalingModesNode = root.FindMember("scalingModes");
	if (scalingModesNode == root.MemberEnd()) {
		return true;
	}

	if (!scalingModesNode->value.IsArray()) {
		return loadingSettings;
	}

	const auto& scalingModesArray = scalingModesNode->value.GetArray();
	const rapidjson::SizeType size = scalingModesArray.Size();
	if (size == 0) {
		return true;
	}

	std::vector<ScalingMode> scalingModes;
	scalingModes.reserve(size);

	for (const auto& elem : scalingModesArray) {
		if (!elem.IsObject()) {
			if (loadingSettings) {
				continue;
			} else {
				return false;
			}
		}

		if (!LoadScalingMode(elem.GetObj(), scalingModes.emplace_back(), loadingSettings)) {
			if (loadingSettings) {
				scalingModes.pop_back();
			} else {
				return false;
			}
		}
	}

	if (scalingModes.empty()) {
		return true;
	}

	const V065NormalizationStats normalization =
		NormalizeV065ScalingModes(scalingModes);
	if (normalization.Changed()) {
		Logger::Get().Info(fmt::format(
			"v0.6.5 scaling-mode normalization: migratedGuidanceModes={} "
			"removedDepthParameters={} removedLegacyParameters={} "
			"clampedParameters={} removedDepthDiagnostics={} fallbacks={} "
			"migratedMotionVectorChoices={} normalizedOpticalFlowChoices={} "
			"migratedXeSSMfgSettings={} removedXeSSMfgNvidiaParameters={}",
			normalization.migratedGuidanceModes,
			normalization.removedDepthParameters,
			normalization.removedLegacyParameters,
			normalization.clampedParameters,
			normalization.removedDepthDiagnostics,
			normalization.insertedFallbacks,
			normalization.migratedMotionVectorChoices,
			normalization.normalizedOpticalFlowChoices,
			normalization.migratedXeSSMfgSettings,
			normalization.removedXeSSMfgNvidiaParameters));
	}

	std::vector<ScalingMode>& settings = AppSettings::Get().ScalingModes();
	settings.insert(
		settings.end(),
		std::make_move_iterator(scalingModes.begin()),
		std::make_move_iterator(scalingModes.end())
	);

	ScalingModeAdded.Invoke(EffectAddedWay::Import);

	if (!loadingSettings) {
		AppSettings::Get().SaveAsync();
	}

	return true;
}

}
