#include "WgcMinUpdateIntervalPolicy.h"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace std::chrono_literals;

namespace winrt {
using TimeSpan = std::chrono::duration<int64_t, std::ratio<1, 10'000'000>>;
struct hresult_error {
	int32_t code() const noexcept { return static_cast<int32_t>(0x80004005u); }
};
struct GraphicsCaptureSession {};
template<typename T> std::wstring_view name_of() { return L"GraphicsCaptureSession"; }
struct ApiInformation {
	static inline bool available = true;
	static bool IsPropertyPresent(std::wstring_view, std::wstring_view) { return available; }
};
}

namespace fmt {
template<typename... Args>
std::string format(std::string_view message, const Args&...) { return std::string(message); }
}

namespace Magpie {
struct Logger {
	static Logger& Get() { static Logger instance; return instance; }
	void Info(std::string_view) {}
	void Warn(std::string_view message) { warnings.emplace_back(message); }
	std::vector<std::string> warnings;
};

struct FakeCaptureSession {
	bool zeroSetterFails = false, zeroGetterFails = false;
	bool compatibilitySetterFails = false, compatibilityGetterFails = false;
	int64_t zeroReadback = 0, compatibilityReadback = 10'000;
	std::vector<int64_t> setters;
	unsigned getters = 0;
	int64_t lastValue = 10'000;
	void MinUpdateInterval(winrt::TimeSpan value) {
		setters.push_back(value.count());
		if (value.count() == 0 ? zeroSetterFails : compatibilitySetterFails) throw winrt::hresult_error{};
		lastValue = value.count();
	}
	winrt::TimeSpan MinUpdateInterval() {
		++getters;
		if (lastValue == 0 ? zeroGetterFails : compatibilityGetterFails) throw winrt::hresult_error{};
		return winrt::TimeSpan(lastValue == 0 ? zeroReadback : compatibilityReadback);
	}
};

struct GraphicsCaptureFrameSource {
	WgcMinUpdateIntervalPolicy _minUpdateIntervalPolicy;
	FakeCaptureSession _captureSession;
	uint64_t _captureSessionGeneration = 1;
	void _ApplyMinUpdateInterval();
};

#include "WgcApplyMinUpdateInterval.inc"
}

int main() {
	using namespace Magpie;
	for (bool vrr : {false, true}) for (bool is25H2OrNewer : {false, true}) {
		WgcMinUpdateIntervalPolicy policy;
		policy.Configure(vrr, is25H2OrNewer);
		assert(policy.Interval100ns() == (vrr && is25H2OrNewer ? 0 : 10'000));
		assert(!policy.FellBack() && !policy.InvalidOverride());
		policy.Configure(vrr, is25H2OrNewer, L"0");
		assert(policy.Interval100ns() == 0);
		policy.Configure(vrr, is25H2OrNewer, L"1000");
		assert(policy.Interval100ns() == 10'000);
		for (auto invalid : {L"1", L"2", L"2000", L"-1", L"00", L"0 ", L" 1000", L"1ms", L"100000000"}) {
			policy.Configure(vrr, is25H2OrNewer, invalid);
			assert(policy.InvalidOverride());
			assert(policy.Interval100ns() == (vrr && is25H2OrNewer ? 0 : 10'000));
		}
	}
	{
		GraphicsCaptureFrameSource source;
		source._minUpdateIntervalPolicy.Configure(true, true);
		winrt::ApiInformation::available = false;
		source._ApplyMinUpdateInterval();
		assert(source._captureSession.setters.empty() && source._captureSession.getters == 0);
		winrt::ApiInformation::available = true;
	}
	{
		GraphicsCaptureFrameSource source;
		source._minUpdateIntervalPolicy.Configure(true, true);
		source._ApplyMinUpdateInterval();
		source._captureSession = {};
		++source._captureSessionGeneration;
		source._ApplyMinUpdateInterval();
		assert(source._captureSession.setters == std::vector<int64_t>{0});
		assert(source._captureSession.getters == 1 && !source._minUpdateIntervalPolicy.FellBack());
	}
	for (int failure = 0; failure < 3; ++failure) {
		GraphicsCaptureFrameSource source;
		source._minUpdateIntervalPolicy.Configure(true, true);
		source._captureSession.zeroSetterFails = failure == 0;
		source._captureSession.zeroGetterFails = failure == 1;
		source._captureSession.zeroReadback = failure == 2 ? 1 : 0;
		source._ApplyMinUpdateInterval();
		assert((source._captureSession.setters == std::vector<int64_t>{0, 10'000}));
		assert(source._minUpdateIntervalPolicy.Requested100ns() == 0 && source._minUpdateIntervalPolicy.FellBack());
		// Recreating the WGC session must not retry the rejected zero interval.
		source._captureSession = {};
		++source._captureSessionGeneration;
		source._ApplyMinUpdateInterval();
		assert(source._captureSession.setters == std::vector<int64_t>{10'000});
		assert(source._captureSession.getters == 1 && source._minUpdateIntervalPolicy.FellBack());
		// A new scaling session selects its own policy again.
		source._minUpdateIntervalPolicy.Configure(true, true);
		assert(source._minUpdateIntervalPolicy.Interval100ns() == 0 && !source._minUpdateIntervalPolicy.FellBack());
	}
	for (bool fallback : {false, true}) {
		GraphicsCaptureFrameSource source;
		source._minUpdateIntervalPolicy.Configure(fallback, true);
		source._captureSession.zeroGetterFails = fallback;
		source._captureSession.compatibilitySetterFails = true;
		bool threw = false;
		try { source._ApplyMinUpdateInterval(); } catch (const winrt::hresult_error&) { threw = true; }
		assert(threw); // The existing _StartCapture catch still owns setter failures.
	}
	for (bool fallback : {false, true}) for (bool getterFails : {false, true}) {
		GraphicsCaptureFrameSource source;
		source._minUpdateIntervalPolicy.Configure(fallback, true);
		source._captureSession.zeroReadback = 1;
		source._captureSession.compatibilityGetterFails = getterFails;
		source._captureSession.compatibilityReadback = 0;
		const auto warnings = Logger::Get().warnings.size();
		source._ApplyMinUpdateInterval();
		assert(Logger::Get().warnings.size() > warnings);
		assert(source._minUpdateIntervalPolicy.Interval100ns() == 10'000);
		assert(source._minUpdateIntervalPolicy.FellBack() == fallback);
	}
	std::cout << "PASS WGC interval selection, exact overrides, API availability, zero setter/getter/readback fallback, compatibility failures and restart persistence\n";
}
