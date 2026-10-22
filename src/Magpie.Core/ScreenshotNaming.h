#pragma once
#include <cstdint>
#include <limits>
#include <string_view>
#include <optional>
#include <string>

namespace Magpie {

constexpr bool ScreenshotNameEquals(std::wstring_view left, std::wstring_view right) noexcept {
	if (left.size() != right.size()) return false;
	for (size_t i = 0; i < left.size(); ++i) {
		auto lower = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? c + (L'a' - L'A') : c; };
		if (lower(left[i]) != lower(right[i])) return false;
	}
	return true;
}

// A timestamp reserves one request stem across all sides and export purposes.
// Legacy numbered files do not match this prefix. Directories also reserve names.
inline std::optional<uint32_t> ParseScreenshotCollision(std::wstring_view name,
	std::wstring_view timestamp) {
	const std::wstring prefix = L"Magpie_" + std::wstring(timestamp);
	if (name.size() <= prefix.size() || !ScreenshotNameEquals(name.substr(0, prefix.size()), prefix)) return {};
	const size_t dot = name.find_last_of(L'.');
	if (dot == std::wstring_view::npos || dot < prefix.size()) return {};
	const auto extension = name.substr(dot);
	if (!ScreenshotNameEquals(extension, L".png") && !ScreenshotNameEquals(extension, L".dds")) return {};
	auto tail = name.substr(prefix.size(), dot - prefix.size());
	if (tail.empty()) return 0;
	if (tail.front() != L'_') return {};
	tail.remove_prefix(1);
	if (tail.empty()) return {};
	if (tail.front() < L'0' || tail.front() > L'9') return 0;
	const size_t separator = tail.find(L'_');
	if (separator == std::wstring_view::npos || separator + 1 == tail.size()) return {};
	uint32_t value = 0;
	for (wchar_t digit : tail.substr(0, separator)) {
		if (digit < L'0' || digit > L'9') return {};
		const uint32_t d = static_cast<uint32_t>(digit - L'0');
		if (value > (std::numeric_limits<uint32_t>::max() - d) / 10) return {};
		value = value * 10 + d;
	}
	return value;
}

inline std::wstring ScreenshotStem(std::wstring_view timestamp, uint32_t collision) {
	std::wstring result = L"Magpie_" + std::wstring(timestamp);
	if (collision) result += (collision < 10 ? L"_0" : L"_") + std::to_wstring(collision);
	return result;
}

}
