#pragma once
#include "EffectDesc.h"
#include "DLSSNRParameters.h"
#include "CommonSharedConstants.h"
#include <winrt/Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.ApplicationModel.Resources.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <fmt/format.h>
#include <algorithm>

namespace Magpie {

struct EffectParameterLocalization {
	static std::string Resource(std::wstring_view key, std::string_view fallback = {}) {
		try {
			const auto value = winrt::Windows::ApplicationModel::Resources::ResourceLoader::GetForViewIndependentUse(
				CommonSharedConstants::APP_RESOURCE_MAP_ID).GetString(winrt::hstring(key));
			if (!value.empty()) return winrt::to_string(value);
		} catch (...) {}
		return std::string(fallback);
	}

	static std::string HdrPairSource(size_t index) {
		return fmt::format(fmt::runtime(Resource(L"HdrParameter_PairSource",
			"Restores settings from HDR -> SDR at effect #{}.")), index + 1);
	}

	template<class GetValue>
	static std::string DLSSNRChoiceReason(std::string_view /*parameter*/, GetValue&& /*getValue*/) {
		return Resource(L"EffectParam_DLSSNR_DLSSNR_AI_Filter_FlowRequired",
			"Select AMDOF or NVOF to use this option.");
	}

	// Display-only help shared by XAML and the runtime overlay. DLSSNR names
	// show purpose only; other effects retain their descriptor-based numeric help.
	static std::string Tooltip(std::string_view effect, const EffectParameterDesc& parameter,
		bool /*enabled*/ = true) {
		const std::string_view base = effect == "DLSSNR\\DLSSNR_AI_Filter"
			? DLSSNRBaseParameter(parameter.name) : std::string_view(parameter.name);
		std::string text = Resource(L"EffectParam_" + KeyPart(effect) + L"_" + KeyPart(base) + L"_Description");
		if (text.empty()) {
			const auto newline = parameter.label.find('\n');
			if (newline != std::string::npos) text = parameter.label.substr(newline + 1);
		}
		if (effect == "DLSSNR\\DLSSNR_AI_Filter") return text;
		auto append = [&](std::string value) {
			if (!text.empty()) text += '\n';
			text += value;
		};
		if (!parameter.choices.empty()) {
			const int value = std::get<1>(parameter.constant).defaultValue;
			const auto choice = std::find_if(parameter.choices.begin(), parameter.choices.end(),
				[&](const auto& item) { return item.value == value; });
			append(Resource(L"EffectParameter_Help_Default", "Default") + ": " +
				(choice == parameter.choices.end() ? std::to_string(value) : choice->label));
		} else {
			std::visit([&](const auto& constant) {
				const std::string step=parameter.segmentedSteps ? "5 / 10 / 50" : fmt::format("{:.7g}",
					parameter.inputStep > 0 ? double(parameter.sliderStep) : double(constant.step));
				append(fmt::format("{} {:.7g}–{:.7g} · {} {:.7g} · {} {}",
					Resource(L"EffectParameter_Help_Range", "Range"), parameter.inputStep > 0 ? std::max(double(constant.minValue), double(parameter.editMinimum)) : double(constant.minValue), double(constant.maxValue),
					Resource(L"EffectParameter_Help_Default", "Default"), double(constant.defaultValue),
					Resource(L"EffectParameter_Help_Step", "Step"), step));
			}, parameter.constant);
			if (parameter.inputStep > 0) append(Resource(L"HdrParameter_PreciseInputHelp",
				"Type an exact value; Shift + arrow or wheel uses the coarse step."));
		}
		return text;
	}

	static std::wstring KeyPart(std::string_view value) {
		std::wstring result;
		for (char c : value) {
			result.push_back((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				(c >= '0' && c <= '9') ? wchar_t(c) : L'_');
		}
		return result;
	}

	// Translate a display-only copy. Effect IDs, shader metadata, compilation
	// caches, parameter names and saved numeric values remain language-neutral.
	static void Localize(std::string_view effect, std::vector<EffectParameterDesc>& parameters) noexcept {
		try {
			using namespace winrt::Windows::ApplicationModel::Resources;
			const auto language = Core::ResourceContext::GetForViewIndependentUse()
				.QualifierValues().Lookup(L"Language");
			const std::wstring_view tag(language);
			if (!tag.starts_with(L"zh") && !tag.starts_with(L"ZH")) return;
			const auto loader = ResourceLoader::GetForViewIndependentUse(
				CommonSharedConstants::APP_RESOURCE_MAP_ID);
			const std::wstring prefix = L"EffectParam_" + KeyPart(effect) + L"_";
			auto translate = [&](const std::wstring& key, std::string& text) {
				try {
					const auto value = loader.GetString(key);
					if (!value.empty()) text = winrt::to_string(value);
				} catch (...) { /* Missing third-party translations keep their source text. */ }
			};
			for (auto& parameter : parameters) {
				const std::wstring key = prefix + KeyPart(effect == "DLSSNR\\DLSSNR_AI_Filter"
					? DLSSNRBaseParameter(parameter.name) : std::string_view(parameter.name));
				translate(key + L"_Label", parameter.label);
				if (!parameter.group.empty()) {
					translate(prefix + L"Group_" + KeyPart(parameter.group), parameter.group);
				}
				for (auto& choice : parameter.choices) {
					translate(key + L"_Option_" + std::to_wstring(choice.value), choice.label);
				}
			}
		} catch (...) { /* Resource failures must not prevent editing parameters. */ }
	}
};

}
