#pragma once
#include <cstdint>
#include <string_view>

namespace Magpie {

// Owned by one capture source, rather than one WGC session: cursor restarts
// must retain a compatibility fallback until the next scaling session.
class WgcMinUpdateIntervalPolicy {
public:
	static constexpr int64_t Compatibility100ns = 10'000;

	void Configure(bool vrrEnabled, bool is25H2OrNewer, std::wstring_view overrideValue = {}) noexcept {
		_requested100ns = vrrEnabled && is25H2OrNewer ? 0 : Compatibility100ns;
		_reason = _requested100ns == 0 ? "vrr-win25h2" : "legacy-compatibility";
		_invalidOverride = false;
		if (overrideValue == L"0" || overrideValue == L"1000") {
			_requested100ns = overrideValue == L"0" ? 0 : Compatibility100ns;
			_reason = "diagnostic-override";
		} else if (!overrideValue.empty()) {
			_invalidOverride = true;
		}
		_interval100ns = _requested100ns;
	}

	int64_t Requested100ns() const noexcept { return _requested100ns; }
	int64_t Interval100ns() const noexcept { return _interval100ns; }
	const char* Reason() const noexcept { return _reason; }
	bool InvalidOverride() const noexcept { return _invalidOverride; }
	bool FellBack() const noexcept { return _interval100ns != _requested100ns; }
	void FallBackToCompatibility() noexcept { _interval100ns = Compatibility100ns; }

private:
	int64_t _requested100ns = Compatibility100ns;
	int64_t _interval100ns = Compatibility100ns;
	const char* _reason = "legacy-compatibility";
	bool _invalidOverride = false;
};

}
