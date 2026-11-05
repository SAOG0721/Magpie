#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace Magpie {

enum class DlssNrState : uint8_t { Disabled, Starting, Running, Skipped, Stopped };
enum class DlssNrStage : uint8_t { Initialize, LoadLibrary, InitializeLibrary, CreateFeature, Evaluate, Resize };
enum class DlssNrReason : uint8_t {
	Unknown, UnsupportedAdapter, LibraryMissing, LibraryLoadFailed, MissingExports,
	Compatibility, DriverOrLibraryOutOfDate, OutOfMemory, AccessDenied, RuntimeFault, BuildDisabled
};
enum class DlssNrCodeKind : uint8_t { None, Win32, HResult, Ngx, Seh };

// A cause is recorded at the failing API boundary, never inferred from log text.
// The first failure survives cleanup and subsequent pass-through frames.
struct DlssNrFailure {
	DlssNrStage stage = DlssNrStage::Initialize;
	DlssNrReason reason = DlssNrReason::Unknown;
	DlssNrCodeKind codeKind = DlssNrCodeKind::None;
	uint32_t code = 0;
	std::string operation;
	bool operator==(const DlssNrFailure&) const = default;
};

inline DlssNrFailure DlssNrApiFailure(DlssNrStage stage, DlssNrCodeKind kind,
	uint32_t code, std::string operation) {
	DlssNrReason reason = DlssNrReason::Unknown;
	if (kind == DlssNrCodeKind::Seh) reason = DlssNrReason::RuntimeFault;
	else if (kind == DlssNrCodeKind::Ngx) {
		// nvsdk_ngx_defs.h: these are broader than a GPU or driver verdict.
		switch (code) {
		case 0xBAD00001: reason = DlssNrReason::Compatibility; break;
		case 0xBAD0000C: reason = DlssNrReason::DriverOrLibraryOutOfDate; break;
		case 0xBAD0000D: reason = DlssNrReason::OutOfMemory; break;
		case 0xBAD0000F: reason = DlssNrReason::AccessDenied; break;
		}
	} else if (kind == DlssNrCodeKind::HResult) {
		if (code == 0x8007000E) reason = DlssNrReason::OutOfMemory;
		else if (code == 0x80070005 || code == 0x887A002B) reason = DlssNrReason::AccessDenied;
	} else if (kind == DlssNrCodeKind::Win32 && code == 5) reason = DlssNrReason::AccessDenied;
	return { stage, reason, kind, code, std::move(operation) };
}

struct DlssNrStatus {
	DlssNrState state = DlssNrState::Disabled;
	DlssNrFailure failure;
	bool restartRequired = false;
	std::string environment;
	uint32_t activeInputResolutionPercent = 0;
	bool operator==(const DlssNrStatus&) const = default;
	bool Failed() const noexcept { return state == DlssNrState::Skipped || state == DlssNrState::Stopped; }
};

// One user session, with a new generation for every renderer rebuild. Readers
// copy a snapshot; the render thread never mutates UI-owned parameter metadata.
class DlssNrSessionState {
public:
	struct Snapshot {
		uint32_t runId = 0;
		uint64_t revision = 0;
		std::vector<DlssNrStatus> effects;
	};
	void Begin(uint32_t runId, const std::vector<bool>& enabled) {
		std::scoped_lock lock(_mutex);
		_snapshot = { runId, _snapshot.revision + 1, std::vector<DlssNrStatus>(enabled.size()) };
		_ended = false;
		for (size_t i = 0; i < enabled.size(); ++i)
			if (enabled[i]) _snapshot.effects[i].state = DlssNrState::Starting;
	}
	bool Update(uint32_t runId, size_t index, DlssNrStatus value) {
		std::scoped_lock lock(_mutex);
		if (_ended || runId != _snapshot.runId || index >= _snapshot.effects.size() || _snapshot.effects[index] == value) return false;
		_snapshot.effects[index] = std::move(value);
		++_snapshot.revision;
		return true;
	}
	bool End(uint32_t runId) {
		std::scoped_lock lock(_mutex);
		if (_ended || runId != _snapshot.runId) return false;
		_ended = true;
		for (auto& status : _snapshot.effects) {
			if (status.state == DlssNrState::Starting || status.state == DlssNrState::Running) status.state = DlssNrState::Disabled;
			else if (status.state == DlssNrState::Skipped) status.state = DlssNrState::Stopped;
		}
		++_snapshot.revision;
		return true;
	}
	Snapshot Read() const { std::scoped_lock lock(_mutex); return _snapshot; }
private:
	mutable std::mutex _mutex;
	Snapshot _snapshot;
	bool _ended = true;
};

constexpr const wchar_t* DlssNrStateKey(DlssNrState state) noexcept {
	switch (state) {
	case DlssNrState::Starting: return L"DlssNr_State_Starting";
	case DlssNrState::Running: return L"DlssNr_State_Running";
	case DlssNrState::Skipped: return L"DlssNr_State_Skipped";
	case DlssNrState::Stopped: return L"DlssNr_State_Stopped";
	default: return L"DlssNr_State_Disabled";
	}
}

constexpr const wchar_t* DlssNrReasonKey(DlssNrReason reason) noexcept {
	switch (reason) {
	case DlssNrReason::UnsupportedAdapter: return L"DlssNr_Reason_Adapter";
	case DlssNrReason::LibraryMissing: return L"DlssNr_Reason_Missing";
	case DlssNrReason::LibraryLoadFailed: return L"DlssNr_Reason_Load";
	case DlssNrReason::MissingExports: return L"DlssNr_Reason_Exports";
	case DlssNrReason::Compatibility: return L"DlssNr_Reason_Compatibility";
	case DlssNrReason::DriverOrLibraryOutOfDate: return L"DlssNr_Reason_Version";
	case DlssNrReason::OutOfMemory: return L"DlssNr_Reason_Memory";
	case DlssNrReason::AccessDenied: return L"DlssNr_Reason_Access";
	case DlssNrReason::RuntimeFault: return L"DlssNr_Reason_Fault";
	case DlssNrReason::BuildDisabled: return L"DlssNr_Reason_Build";
	default: return L"DlssNr_Reason_Unknown";
	}
}

constexpr const wchar_t* DlssNrAdviceKey(const DlssNrStatus& status) noexcept {
	if (status.restartRequired) return L"DlssNr_Advice_Restart";
	switch (status.failure.reason) {
	case DlssNrReason::LibraryMissing:
	case DlssNrReason::LibraryLoadFailed:
	case DlssNrReason::MissingExports:
	case DlssNrReason::BuildDisabled: return L"DlssNr_Advice_Files";
	case DlssNrReason::UnsupportedAdapter: return L"DlssNr_Advice_Adapter";
	case DlssNrReason::DriverOrLibraryOutOfDate: return L"DlssNr_Advice_Version";
	case DlssNrReason::OutOfMemory: return L"DlssNr_Advice_Memory";
	case DlssNrReason::AccessDenied: return L"DlssNr_Advice_Access";
	default: return L"DlssNr_Advice_Unknown";
	}
}

}
