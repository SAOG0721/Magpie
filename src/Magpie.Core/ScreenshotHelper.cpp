#include "pch.h"
#include "ScreenshotHelper.h"
#include "ScreenshotNaming.h"
#include "Logger.h"

namespace Magpie {

std::mutex& ScreenshotHelper::SaveMutex() noexcept {
	static std::mutex mutex;
	return mutex;
}

std::wstring ScreenshotHelper::CaptureTimestamp() {
	SYSTEMTIME time{};
	GetLocalTime(&time);
	return fmt::format(L"{:04}{:02}{:02}_{:02}{:02}{:02}_{:03}", time.wYear, time.wMonth, time.wDay,
		time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
}

std::wstring ScreenshotHelper::FindUnusedScreenshotStem(const std::filesystem::path& directory,
	std::wstring_view timestamp) {
	WIN32_FIND_DATA data{};
	const std::wstring pattern = directory.native() + L"\\Magpie_" + std::wstring(timestamp) + L"*";
	wil::unique_hfind find(FindFirstFileEx(pattern.c_str(), FindExInfoBasic,
		&data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
	if (!find) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) return ScreenshotStem(timestamp, 0);
		Logger::Get().Win32Error("Enumerate screenshot directory failed");
		return {};
	}
	std::vector<uint32_t> collisions;
	do {
		if (const auto collision = ParseScreenshotCollision(data.cFileName, timestamp)) collisions.push_back(*collision);
	} while (FindNextFile(find.get(), &data));
	if (GetLastError() != ERROR_NO_MORE_FILES) {
		Logger::Get().Win32Error("FindNextFile failed while finding screenshot timestamp");
		return {};
	}
	std::sort(collisions.begin(), collisions.end());
	uint32_t next = 0;
	for (uint32_t collision : collisions) {
		if (collision < next) continue;
		if (collision > next) break;
		if (next == std::numeric_limits<uint32_t>::max()) return {};
		++next;
	}
	return ScreenshotStem(timestamp, next);
}

}
