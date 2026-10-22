#pragma once
#include <mutex>

namespace Magpie {

struct ScreenshotHelper {
	// Capture local wall-clock time at request admission, before any suspension.
	static std::wstring CaptureTimestamp();
	// Caller holds SaveMutex through writing all sides. Empty means enumeration failure.
	static std::wstring FindUnusedScreenshotStem(const std::filesystem::path& screenshotsDir,
		std::wstring_view timestamp);
	// Shared by ordinary screenshots and right-click texture exports.
	static std::mutex& SaveMutex() noexcept;
};

}
