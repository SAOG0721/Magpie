#include "pch.h"
#include "EffectCatalog.h"
#include "LocalizationService.h"
#include "Logger.h"
#include "StrHelper.h"
#include <rapidjson/document.h>

namespace Magpie {

static std::wstring ToTraditionalChinese(std::wstring text) {
	if (text.empty()) return text;

	const int length = static_cast<int>(text.size());
	const int mappedLength = LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE,
		text.data(), length, nullptr, 0, nullptr, nullptr, 0);
	if (!mappedLength) return text;

	std::wstring mapped(static_cast<size_t>(mappedLength), L'\0');
	if (LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, text.data(), length,
		mapped.data(), mappedLength, nullptr, nullptr, 0) != mappedLength) return text;
	return mapped;
}

const EffectCatalog& EffectCatalog::Get() {
	static const EffectCatalog catalog;
	return catalog;
}

const EffectPickerEntry* EffectCatalog::Find(std::wstring_view id) const {
	const auto it = std::ranges::find(_entries, id, &EffectPickerEntry::id);
	return it == _entries.end() ? nullptr : &*it;
}

EffectCatalog::EffectCatalog() {
	// Descriptions are embedded with the executable, never read from user settings.
	const HMODULE module = GetModuleHandle(nullptr);
	const std::wstring_view language = LocalizationService::Get().Language();
	const bool chinese = language.starts_with(L"zh");
	const bool traditional = language == L"zh-hant";
	HRSRC resource = FindResource(module,
		chinese ? L"EFFECT_CATALOG_ZH_HANS" : L"EFFECT_CATALOG_EN_US", RT_RCDATA);
	// Keep the picker usable if a localized resource is accidentally omitted.
	if (!resource && !chinese) {
		resource = FindResource(module, L"EFFECT_CATALOG_ZH_HANS", RT_RCDATA);
	}
	const HGLOBAL loaded = resource ? LoadResource(module, resource) : nullptr;
	const auto bytes = loaded ? static_cast<const char*>(LockResource(loaded)) : nullptr;
	if (!bytes) {
		Logger::Get().Warn("Effect catalog unavailable; using effect names and custom category");
		return;
	}
	rapidjson::Document document;
	document.Parse(bytes, SizeofResource(module, resource));
	if (document.HasParseError() || !document.IsObject()) return;
	auto text = [](const rapidjson::Value& value, const char* key) -> std::wstring {
		if (!value.IsObject()) return {};
		const auto it = value.FindMember(key);
		return it != value.MemberEnd() && it->value.IsString()
			? StrHelper::UTF8ToUTF16({ it->value.GetString(), it->value.GetStringLength() }) : std::wstring{};
	};
	const auto displayText = [&](const rapidjson::Value& value, const char* key) {
		auto result = text(value, key);
		if (traditional) return ToTraditionalChinese(std::move(result));
		return result;
	};
	const auto categories = document.FindMember("categories");
	if (categories != document.MemberEnd() && categories->value.IsArray()) {
		for (const auto& value : categories->value.GetArray()) {
			EffectPickerCategory category{ text(value, "id"), displayText(value, "name"), displayText(value, "description") };
			if (category.id.empty() || category.name.empty()) continue;
			const auto subs = value.FindMember("subcategories");
			if (subs != value.MemberEnd() && subs->value.IsArray()) {
				for (const auto& sub : subs->value.GetArray()) {
					category.subcategories.emplace_back(displayText(sub, "name"), displayText(sub, "description"));
				}
			}
			_categories.push_back(std::move(category));
		}
	}
	const auto effects = document.FindMember("effects");
	if (effects == document.MemberEnd() || !effects->value.IsArray()) return;
	for (const auto& value : effects->value.GetArray()) {
		EffectPickerEntry entry;
		entry.id = text(value, "id");
		entry.name = displayText(value, "name");
		if (entry.id.empty() || entry.name.empty()) continue;
		entry.category = text(value, "category");
		entry.subcategory = displayText(value, "subcategory");
		entry.summary = displayText(value, "summary");
		entry.details = displayText(value, "details");
		entry.recommendation = displayText(value, "recommendation");
		entry.firstTry = text(value, "level") == L"first_try";
		auto search = text(value, "search");
		if (traditional) {
			const auto converted = ToTraditionalChinese(search);
			search += L' ';
			search += converted;
		}
		entry.searchText = NormalizeEffectSearch(search);
		auto readFamily = [&](const char* key) {
			const auto family = value.FindMember(key);
			if (family == value.MemberEnd() || !family->value.IsObject()) return EffectPickerFamily{};
			return EffectPickerFamily{text(family->value, "id"), displayText(family->value, "name"),
				displayText(family->value, "summary")};
		};
		entry.family = readFamily("family");
		entry.subfamily = readFamily("subfamily");
		const auto purposes = value.FindMember("purposes");
		if (purposes != value.MemberEnd() && purposes->value.IsArray()) {
			for (const auto& purpose : purposes->value.GetArray()) {
				if (purpose.IsString()) entry.purposes.push_back(StrHelper::UTF8ToUTF16(purpose.GetString()));
			}
		}
		_entries.push_back(std::move(entry));
	}
}

}
