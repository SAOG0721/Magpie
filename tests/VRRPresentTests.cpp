// Filled with the production helper by prepare_vrr_present_test.py.
#include "VRRPresentationPolicy.h"
#include "ForegroundCompatibilityState.h"
#include "VRRPerformance.h"
#include <string_view>
#include <iostream>
#include <stdexcept>
#include <vector>

using namespace Magpie;
struct FakeSwapChain {
	std::vector<HRESULT> results;
	std::vector<UINT> flags;
	HWND window = HWND(uintptr_t(0x111));
	UINT creationFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
	HRESULT descResult = S_OK, windowResult = S_OK, fallbackResult = S_OK;
	HRESULT GetDesc1(DXGI_SWAP_CHAIN_DESC1* desc) {
		desc->Flags = creationFlags; desc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; return descResult;
	}
	HRESULT GetHwnd(HWND* value) { *value = window; return windowResult; }
	HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* desc) { desc->OutputWindow = window; return fallbackResult; }
	HRESULT Present(UINT interval, UINT presentFlags) {
		if (interval != 0) throw std::runtime_error("VRR sync interval must be zero");
		flags.push_back(presentFlags);
		if (flags.size() > results.size()) throw std::runtime_error("Unexpected extra Present retry");
		return results[flags.size() - 1];
	}
};
struct Logger {
	static Logger& Get() { static Logger logger; return logger; }
	void ComWarn(const char*, HRESULT) {}
	void Info(std::string_view) {}
};
namespace fmt { template<class... T> const char* format(const char* message, T...) { return message; } }
struct PresenterBase {
	VRRPresentationPolicy _vrr;
	unsigned probes = 0, transitions = 0, firstPresents = 0;
	bool _firstPresentComplete = false;
	bool _vrrInitializationComplete = false;
	HWND _vrrWindow = HWND(uintptr_t(0x111));
	void _RefreshVRREnvironment() { ++probes; }
	UINT _PresentSyncInterval(UINT) const noexcept { return 0; }
	void _UpdateForegroundCompatibility() noexcept;
	void _BindVRRSwapChain(FakeSwapChain*) noexcept;
	void _VRRPathAvailable(bool available) { _vrr.PathAvailable(available); }
	void _LogVRRState(std::string_view event) { if (event == "first-present") ++firstPresents; else ++transitions; }
	HRESULT _PresentWithVRR(FakeSwapChain*, bool) noexcept;
	PresenterBase() {
		_vrr.Configure(true, true);
		_vrr.PathAvailable(true); _vrr.Environment(VRRUnavailableReason::None);
		_vrr.Created(_vrr.CreationFlags()); _vrr.AltEnterDisabled(true);
	}
};
struct NvidiaForegroundCompatibility {
	static inline bool potential = false, present = false;
	static inline bool allowBind = true, rejected = false;
	static bool BindPresentationWindow(HWND) { return allowBind; }
	static void PresentationState(bool canCreate, bool canPresent) { potential = canCreate; present = canPresent; }
	static void Phase(ForegroundQueryPhase) {} static void Reject(const char*) { rejected = true; }
};
/* PRODUCTION_PRESENT */
static void Check(bool condition, const char* message) {
	if (!condition) throw std::runtime_error(message);
}
int main() {
	for (bool fallback : {false, true}) {
		PresenterBase endpoint; FakeSwapChain chain;
		if (fallback) chain.windowResult = E_NOTIMPL;
		endpoint._BindVRRSwapChain(&chain);
		Check(endpoint._vrr.PresentFlags() != 0, "Actual HWND/proxy descriptor binding lost valid tearing");
	}
	for (int failure = 0; failure < 4; ++failure) {
		PresenterBase endpoint; FakeSwapChain chain;
		NvidiaForegroundCompatibility::rejected = false;
		if (failure == 0) chain.window = HWND(uintptr_t(0x222));
		if (failure == 1) chain.descResult = E_FAIL;
		if (failure == 2) { chain.windowResult = E_NOTIMPL; chain.fallbackResult = E_FAIL; }
		if (failure == 3) NvidiaForegroundCompatibility::allowBind = false;
		endpoint._BindVRRSwapChain(&chain);
		Check(!endpoint._vrr.PresentFlags() && NvidiaForegroundCompatibility::rejected,
			"Unbound final endpoint kept presentation eligibility");
		if (failure == 1) Check(endpoint._vrr.ResizeFlags() == DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING,
			"Failed descriptor probing lost known immutable resize flags");
		NvidiaForegroundCompatibility::allowBind = true;
	}
	PresenterBase proxy; FakeSwapChain withoutTearing; withoutTearing.creationFlags = 0;
	proxy._BindVRRSwapChain(&withoutTearing);
	Check(!proxy._vrr.PresentFlags() && !NvidiaForegroundCompatibility::potential,
		"Actual proxy missing immutable tearing flags stayed eligible");
	PresenterBase binding;
	binding._vrr.Configure(true, true); binding._vrr.PathAvailable(true);
	binding._vrr.Environment(VRRUnavailableReason::None);
	binding._UpdateForegroundCompatibility();
	Check(NvidiaForegroundCompatibility::potential && !NvidiaForegroundCompatibility::present,
		"Valid creation phase did not admit driver initialization queries");
	binding._vrr.Created(0); binding._vrr.AltEnterDisabled(true);
	binding._UpdateForegroundCompatibility();
	Check(!NvidiaForegroundCompatibility::potential && !NvidiaForegroundCompatibility::present,
		"Actual proxy without tearing stayed armed under creation intent");
	binding._vrr.Created(DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
	binding._UpdateForegroundCompatibility();
	Check(NvidiaForegroundCompatibility::potential && NvidiaForegroundCompatibility::present,
		"Bound tearing endpoint did not restore presentation eligibility");
	PresenterBase ordinary;
	FakeSwapChain rejected{{DXGI_ERROR_INVALID_CALL, S_OK, S_OK}, {}};
	Check(ordinary._PresentWithVRR(&rejected, true) == S_OK, "Ordinary fallback did not submit once without tearing");
	Check(rejected.flags == std::vector<UINT>{DXGI_PRESENT_ALLOW_TEARING, 0}, "Ordinary fallback flags/order wrong");
	Check(ordinary.transitions == 1 && !ordinary._vrr.PresentFlags(), "Rejection didn't latch for this session");
	Check(ordinary._PresentWithVRR(&rejected, true) == S_OK && rejected.flags.back() == 0,
		"Next frame retried rejected tearing contract");
	PresenterBase sdk;
	FakeSwapChain sdkRejected{{DXGI_ERROR_INVALID_CALL, S_OK}, {}};
	Check(sdk._PresentWithVRR(&sdkRejected, false) == DXGI_ERROR_INVALID_CALL && sdkRejected.flags.size() == 1,
		"SDK proxy double-submitted tagged FG input");
	Check(sdk._PresentWithVRR(&sdkRejected, false) == S_OK && sdkRejected.flags.back() == 0,
		"SDK next-frame fallback failed");
	for (HRESULT failure : {DXGI_ERROR_DEVICE_REMOVED, DXGI_ERROR_DEVICE_RESET, E_OUTOFMEMORY}) {
		PresenterBase device;
		FakeSwapChain lost{{failure}, {}};
		Check(device._PresentWithVRR(&lost, true) == failure && lost.flags.size() == 1 && device.transitions == 0,
			"Non-contract failure incorrectly retried or downgraded as VRR rejection");
	}
	PresenterBase off; off._vrr.Configure(false, true);
	FakeSwapChain noTearing{{DXGI_ERROR_INVALID_CALL}, {}};
	Check(off._PresentWithVRR(&noTearing, true) == DXGI_ERROR_INVALID_CALL && noTearing.flags.size() == 1,
		"Non-tearing contract error was retried");
	PresenterBase occluded;
	FakeSwapChain hidden{{DXGI_STATUS_OCCLUDED}, {}};
	Check(occluded._PresentWithVRR(&hidden, true) == DXGI_STATUS_OCCLUDED && occluded.transitions == 0,
		"Hidden first-frame status was treated as VRR contract failure");
	std::cout << "Production VRR Present helper: bounded fallback, SDK single submission, device-loss and occlusion cases passed\n";
}
