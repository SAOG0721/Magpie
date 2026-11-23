#include "pch.h"
#include "RtssService.h"
#include "Logger.h"
#include "Win32Helper.h"
#include "StrHelper.h"
#include <utility>

namespace Magpie {
namespace {

struct TargetIdentity {
	HWND source = nullptr, processWindow = nullptr;
	wil::unique_handle process;
	DWORD processId = 0;
	FILETIME creation{};
	std::string profile;
	std::wstring exe;
	bool Valid() const {
		if (!process || !IsWindow(source) || !IsWindow(processWindow)) return false;
		if (source != processWindow && !IsChild(source, processWindow)) return false;
		DWORD pid = 0, exitCode = 0;
		if (!QueryWindowThreadProcessId(processWindow, pid)) return false;
		FILETIME created{}, exited{}, kernel{}, user{};
		return pid == processId && GetExitCodeProcess(process.get(), &exitCode) && exitCode == STILL_ACTIVE &&
			GetProcessTimes(process.get(), &created, &exited, &kernel, &user) &&
			created.dwHighDateTime == creation.dwHighDateTime && created.dwLowDateTime == creation.dwLowDateTime;
	}
	bool SameProcess(const TargetIdentity& other) const noexcept {
		return processId == other.processId && creation.dwHighDateTime == other.creation.dwHighDateTime &&
			creation.dwLowDateTime == other.creation.dwLowDateTime && _stricmp(profile.c_str(), other.profile.c_str()) == 0;
	}
};

std::string ProfileName(const std::wstring& name) { return RtssProfileAccess::ProfileName(name); }

void SetAccessStatus(const RtssProfileAccess& access, RtssStatus& status) {
	const bool sameInstallation = !status.rtss.executable.empty() &&
		_wcsicmp(access.Directory().c_str(), status.rtss.executable.parent_path().c_str()) == 0;
	status.readReady = sameInstallation && access.CanRead();
	status.applyReady = sameInstallation && access.CanApply();
	status.initializeReady = sameInstallation && access.CanInitializeUnlimited();
}

void SetRateStatus(const RtssProfileSnapshot& snapshot, RtssStatus& status) {
	status.timeLimit = snapshot.rateKnown[2] && snapshot.rate[2] > 0;
	status.rateKnown = snapshot.rateKnown[2] && (status.timeLimit || (snapshot.rateKnown[0] &&
		(!snapshot.rate[0] || (snapshot.rateKnown[1] && snapshot.rate[1]))));
	status.rateUnavailable = !status.rateKnown;
	status.currentRate = status.rateKnown && snapshot.rate[0] && snapshot.rateKnown[1] ? double(snapshot.rate[0]) / snapshot.rate[1] : 0;
	status.detectionDisabled = snapshot.detectionKnown && !snapshot.detectionLevel;
}

std::shared_ptr<TargetIdentity> ResolveTarget(HWND source) {
	if (!IsWindow(source)) return {};
	HWND actual = source;
	const auto sourceName = Win32Helper::GetWindowExeName(source);
	if (_wcsicmp(sourceName.c_str(), L"ApplicationFrameHost.exe") == 0) {
		struct Children { HWND actual = nullptr; DWORD pid = 0; bool ambiguous = false; } children;
		EnumChildWindows(source, [](HWND child, LPARAM context) -> BOOL {
			auto& found = *reinterpret_cast<Children*>(context);
			if (Win32Helper::GetWindowClassName(child) != L"Windows.UI.Core.CoreWindow") return TRUE;
			DWORD pid = 0;
			if (!QueryWindowThreadProcessId(child, pid) || pid == GetCurrentProcessId()) return TRUE;
			if (found.pid && found.pid != pid) found.ambiguous = true;
			found.pid = pid; found.actual = child;
			return TRUE;
		}, reinterpret_cast<LPARAM>(&children));
		if (!children.actual || children.ambiguous) return {};
		actual = children.actual;
	}
	auto result = std::make_shared<TargetIdentity>();
	result->source = source; result->processWindow = actual;
	if (!QueryWindowThreadProcessId(actual, result->processId) || result->processId == GetCurrentProcessId()) return {};
	result->process.reset(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, result->processId));
	FILETIME exited{}, kernel{}, user{};
	if (!result->process || !GetProcessTimes(result->process.get(), &result->creation, &exited, &kernel, &user)) return {};
	result->exe = Win32Helper::GetWindowExeName(actual);
	if (_wcsicmp(result->exe.c_str(), L"ApplicationFrameHost.exe") == 0) return {};
	result->profile = ProfileName(result->exe);
	return result->profile.empty() || !result->Valid() ? nullptr : result;
}

bool Incompatible(const std::wstring& error) {
	return error == L"UnsupportedInterface" || error == L"UnsupportedArchitecture";
}

void LogFailure(const char* stage, const std::wstring& error) {
	Logger::Get().Warn(fmt::format("RTSS {}: {}", stage, StrHelper::UTF16ToUTF8(error)));
}

const wchar_t* ProfileFailureText(const RtssProfileFailure& failure, const std::wstring& error, bool restoring) {
	if (failure.nativeFault || error == L"InterfaceFault") return L"Rtss_Failure_Interface";
	if (failure.fileError == ERROR_ACCESS_DENIED || failure.fileError == ERROR_PRIVILEGE_NOT_HELD) return L"Rtss_Failure_Permission";
	if (error == L"TimeModeUnsupported") return L"Rtss_Failure_TimeMode";
	if (error == L"MissingInterface" || error == L"UnsupportedInterface") return L"Rtss_Failure_MissingInterface";
	if (error == L"ReadPropertyFailed") return L"Rtss_Failure_Property";
	if (error == L"UnsupportedArchitecture") return L"Rtss_Failure_Architecture";
	if (error == L"InvalidSelfProfile" || error == L"InvalidProfileName") return L"Rtss_Failure_ProfileObject";
	if (error == L"SelfProfileConflict") return L"Rtss_Failure_SelfProfileConflict";
	if (error == L"InstallationChanged") return L"Rtss_Failure_InstallationChanged";
	if (error == L"UnsupportedProfileFormat" || error == L"UnsupportedProfileEncoding" ||
		error == L"DuplicateProfileField" || error == L"InvalidRateExpression") return L"Rtss_Failure_ProfileFormat";
	switch (failure.stage) {
	case RtssFailureStage::Read: return L"Rtss_Failure_Read";
	case RtssFailureStage::Setting: return L"Rtss_Failure_Set";
	case RtssFailureStage::Save: return L"Rtss_Failure_Write";
	case RtssFailureStage::Verify: return L"Rtss_Failure_Verify";
	case RtssFailureStage::Reload: return L"Rtss_Failure_Reload";
	default: return restoring ? L"Rtss_Failure_Restore" : L"Rtss_Failure_Write";
	}
}

void LogProfileFailure(const char* operation, const std::wstring& error, const RtssProfileFailure& failure) {
	Logger::Get().Warn(fmt::format(
		"RTSS {}: {} stage={} profile={} version={} property={} requested={} observed={} observedKnown={} "
		"saveAttempted={} readbackComplete={} nativeFault={} fileError={} recoveryFile={}",
		operation, StrHelper::UTF16ToUTF8(error), int(failure.stage), failure.profile.empty() ? "Global" : failure.profile,
		StrHelper::UTF16ToUTF8(failure.version), failure.property, failure.requested, failure.observed, failure.observedKnown,
		failure.saveAttempted, failure.readbackComplete, failure.nativeFault, failure.fileError, StrHelper::UTF16ToUTF8(failure.recoveryFile)));
}

}

struct RtssService::Session {
	uint64_t generation = 0;
	RtssSettings settings;
	std::shared_ptr<TargetIdentity> initialTarget;
	std::atomic<std::shared_ptr<TargetIdentity>> target;
	std::weak_ptr<const uint8_t> identity;
	std::atomic<bool> cancelled = false, updateQueued = false, healthQueued = false;
	std::atomic<uint32_t> desiredRate = 0;
	std::atomic<bool> contentConstrained = false;
	std::string profile;
	std::filesystem::path installation;
	RtssProfileSnapshot original, lastWritten;
	bool modified = false, created = false, suspended = false, finished = false;
	bool prepared = false, reloadPending = false, selfReloadPending = false;
};

RtssService& RtssService::Get() {
	// Process lifetime: a stalled third-party DLL must not force UI shutdown to
	// wait forever or outlive a freed service after the bounded cleanup deadline.
	static auto* instance = new RtssService;
	return *instance;
}
RtssService::~RtssService() { Uninitialize(); }

void RtssService::Initialize(winrt::DispatcherQueue dispatcher) {
	if (_thread.joinable()) return;
	_dispatcher = std::move(dispatcher);
	_closing = false; _notify = true;
	_thread = std::thread([this] { _Worker(); });
	Recheck(false);
}

void RtssService::Uninitialize() {
	if (!_thread.joinable()) return;
	_notify = false;
	if (_launchTimer) _launchTimer.Stop();
	End();
	{
		std::lock_guard lock(_mutex);
		_closing = true;
	}
	_wake.notify_one();
	// Give one cleanup attempt a bounded exit window. Its state remains alive
	// if the DLL stalls; process exit can abandon it without blocking shutdown.
	std::unique_lock lock(_mutex);
	const bool completed = _done.wait_for(lock, std::chrono::milliseconds(750), [this] { return _exited; });
	lock.unlock();
	if (completed) _thread.join(); else _thread.detach();
}

void RtssService::_Queue(std::function<void()> task) {
	{
		std::lock_guard lock(_mutex);
		if (_closing) return;
		_tasks.push_back(std::move(task));
	}
	_wake.notify_one();
}

void RtssService::_Worker() {
	winrt::init_apartment(winrt::apartment_type::multi_threaded);
	while (true) {
		std::function<void()> task;
		{
			std::unique_lock lock(_mutex);
			_wake.wait(lock, [this] { return _closing || !_tasks.empty(); });
			if (_tasks.empty()) break;
			task = std::move(_tasks.front()); _tasks.pop_front();
		}
		try { task(); }
		catch (...) {
			Logger::Get().Error("RTSS operation failed with an exception");
			auto status = Status(); status.state = RtssState::Failed;
			status.failureStage = L"Rtss_Failure_Access";
			if (_workerSession) {
				_workerSession->suspended = true;
				_PublishSession(_workerSession, std::move(status));
			} else _Publish(std::move(status));
		}
	}
	winrt::uninit_apartment();
	{
		std::lock_guard lock(_mutex); _exited = true;
	}
	_done.notify_one();
}

RtssStatus RtssService::Status() const {
	std::lock_guard lock(_mutex); return _status;
}

RtssStatus RtssService::InventoryStatus() const {
	std::lock_guard lock(_mutex); return _inventoryStatus;
}

RtssStatus RtssService::SessionStatus() const {
	std::lock_guard lock(_mutex); return _sessionStatus;
}
RtssStatus RtssService::ManagementStatus() const {
	std::lock_guard lock(_mutex); return _managementStatus;
}
bool RtssService::IsLatestStatus(const RtssStatus& status, std::weak_ptr<const uint8_t> identity) const noexcept {
	const auto owner = identity.lock();
	return owner && status.sessionGeneration == _generation && status.profileIdentity.lock() == owner;
}
RtssStatus RtssService::CurrentSessionStatus(std::weak_ptr<const uint8_t> identity) const {
	auto status = SessionStatus();
	if (!HasSession(identity) || IsCurrentStatus(status, identity)) return status;
	// Begin has queued work but has not yet published. Do not show the previous
	// session or project pending UI edits onto the captured running policy.
	status = {};
	status.sessionGeneration = _uiSession->generation;
	status.profileIdentity = _uiSession->identity;
	status.scope = _uiSession->settings.scope;
	return status;
}

void RtssService::_Publish(RtssStatus status) {
	SetAccessStatus(_access, status);
	{
		std::lock_guard lock(_mutex);
		if (status.inventoryKnown) _inventoryStatus = status;
		if (status.sessionGeneration && !status.manualOperation && !status.selfInitialization) _sessionStatus = status;
		if (status.manualOperation || status.selfInitialization) _managementStatus = status;
		_status = std::move(status);
	}
	if (_notify) _dispatcher.TryEnqueue([this] { if (_notify) StatusChanged.Invoke(); });
}

bool RtssService::_Prepare(RtssStatus& status) {
	status.readReady = status.applyReady = status.initializeReady = false;
	status.rtss = RtssProfileAccess::DetectTool(false);
	status.afterburner = RtssProfileAccess::DetectTool(true);
	status.inventoryKnown = true;
	if (status.rtss.executable.empty()) { status.state = RtssState::NotInstalled; return false; }
	std::wstring error;
	if (!_access.Initialize(status.rtss, error)) {
		status.state = Incompatible(error) ? RtssState::Incompatible : RtssState::Failed;
		status.failureStage = error == L"InterfaceFault" ? L"Rtss_Failure_Interface" : error == L"LibraryMissing" ?
			L"Rtss_Failure_LibraryMissing" : error == L"AccessDenied" ? L"Rtss_Failure_Permission" :
			error == L"UntrustedInstallation" ? L"Rtss_Failure_UntrustedInstallation" :
			error == L"UnsupportedArchitecture" ? L"Rtss_Failure_Architecture" : L"Rtss_Failure_Access";
		LogProfileFailure("check", error, _access.Failure()); return false;
	}
	SetAccessStatus(_access, status);
	status.state = status.readReady ? (status.rtss.running ? RtssState::WaitingTarget : RtssState::NotRunning) : RtssState::Incompatible;
	status.failureStage = status.readReady ? L"" : L"Rtss_Failure_MissingInterface";
	return true;
}

uint64_t RtssService::Begin(HWND target, std::weak_ptr<const uint8_t> identity, RtssSettings settings) {
	End();
	if (!settings.IsValid() || settings.scope == RtssScope::None) return ++_generation;
	auto session = std::make_shared<Session>();
	session->generation = ++_generation; session->identity = std::move(identity);
	session->settings = settings.IsValid() ? settings : RtssSettings{};
	session->initialTarget = ResolveTarget(target);
	session->target = session->initialTarget;
	if (settings.scope == RtssScope::Target && session->initialTarget) session->profile = session->initialTarget->profile;
	if (settings.rateMode == RtssRateMode::Custom) session->desiredRate = settings.customRate;
	_uiSession = session;
	_StartSessionWatch(session);
	_Queue([this, session] { _workerSession = session; _TryApply(session); });
	return session->generation;
}

void RtssService::ContentTargetChanged(uint64_t sessionId, HWND source, bool fixed, double baseRate, bool constrained) {
	const auto session = _uiSession;
	if (!session || session->generation != sessionId || session->cancelled) return;
	const auto previousTarget = session->target.load();
	const bool targetChanged = !previousTarget || previousTarget->source != source || !previousTarget->Valid();
	if (targetChanged) {
		const auto rebound = ResolveTarget(source);
		if (!rebound || !session->initialTarget || !rebound->SameProcess(*session->initialTarget)) {
			End();
			_Queue([this, session] {
				auto status = SessionStatus(); status.profileIdentity = session->identity;
				status.sessionGeneration = session->generation; status.scope = session->settings.scope;
				status.selfInitialization = status.manualOperation = false;
				status.reloaded = status.profileSaved = status.rateUnavailable = false;
				status.profile.clear(); status.rateKnown = false;
				status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Target";
				_Publish(std::move(status));
			});
			return;
		}
		// A new HWND in the same process is an internal rebuild, not a new baseline.
		session->target = rebound;
	}
	if (session->settings.rateMode == RtssRateMode::ContentSync) {
		const auto rate = session->settings.ResolveRate(fixed, baseRate).value_or(0);
		const bool rateChanged = session->desiredRate.exchange(rate) != rate;
		if (rateChanged && _notify) {
			_dispatcher.TryEnqueue([this] { if (_notify) StatusChanged.Invoke(); });
		}
		session->contentConstrained = constrained;
		if (!rateChanged && !targetChanged) return;
	} else if (!targetChanged) return;
	if (session->updateQueued.exchange(true)) return;
	_Queue([this, session] {
		session->updateQueued = false;
		_TryApply(session);
	});
}

void RtssService::End() {
	if (_sessionTimer) { _sessionTimer.Stop(); _sessionTimer = nullptr; }
	if (!_uiSession) return;
	auto session = std::exchange(_uiSession, nullptr);
	session->cancelled = true;
	// Clear the display even if nothing was written and _Finish publishes no result.
	if (_notify) _dispatcher.TryEnqueue([this] { if (_notify) StatusChanged.Invoke(); });
	_Queue([this, session] {
		_Finish(session);
		if (_workerSession == session) _workerSession.reset();
	});
}

uint32_t RtssService::ContentSyncRate(std::weak_ptr<const uint8_t> identity) const noexcept {
	const auto session = _uiSession;
	const auto profile = identity.lock();
	if (!session || session->cancelled || !profile || profile != session->identity.lock() ||
		session->settings.rateMode != RtssRateMode::ContentSync) return 0;
	return session->desiredRate.load();
}

void RtssService::ReleaseProfile(std::weak_ptr<const uint8_t> identity) {
	if (_uiSession && identity.lock() == _uiSession->identity.lock()) End();
}

void RtssService::_StartSessionWatch(const std::shared_ptr<Session>& session) {
	_sessionTimer = _dispatcher.CreateTimer();
	_sessionTimer.Interval(std::chrono::seconds(2));
	_sessionTimer.Tick([this, session](winrt::DispatcherQueueTimer const&, winrt::IInspectable const&) {
		if (session->cancelled || session->healthQueued.exchange(true)) return;
		_Queue([this, session] {
			session->healthQueued = false;
			_CheckSession(session);
		});
	});
	_sessionTimer.Start();
}

void RtssService::_CheckSession(const std::shared_ptr<Session>& session) {
	if (session->cancelled || session->finished || session->suspended || !session->prepared || !session->modified || _workerSession != session) return;
	const auto target = session->target.load();
	auto status = SessionStatus();
	status.afterburner = InventoryStatus().afterburner;
	status.rtss = RtssProfileAccess::DetectTool(false);
	if (!target || !target->Valid()) {
		status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Target";
	} else if (status.rtss.executable.empty() || !status.rtss.running) {
		status.state = RtssState::NotRunning; status.failureStage = L"Rtss_Failure_ProcessExited";
	} else if (_wcsicmp(session->installation.c_str(), status.rtss.executable.parent_path().c_str()) != 0) {
		status.state = RtssState::Conflict; status.failureStage = L"Rtss_Failure_InstallationChanged";
	} else return;
	// Read-only health checks never relaunch tools or rewrite a cap automatically.
	session->suspended = true; session->prepared = false;
	status.reloaded = false;
	_PublishSession(session, std::move(status));
}

void RtssService::_TryApply(const std::shared_ptr<Session>& session) {
	if (session->cancelled || session->suspended || session->finished || session->settings.scope == RtssScope::None) return;
	RtssStatus status;
	auto target = session->target.load();
	status.profileIdentity = session->identity;
	status.profile = session->settings.scope == RtssScope::Global ? L"Global" : target ? target->exe : std::wstring{};
	status.desiredRate = session->desiredRate.load();
	status.contentConstrained = session->contentConstrained;
	if (!status.desiredRate) {
		status.state = RtssState::WaitingContent;
		_PublishSession(session, std::move(status)); return;
	}
	if (!_PrepareSession(session, status)) return;
	if (session->modified && _wcsicmp(session->installation.c_str(), status.rtss.executable.parent_path().c_str()) != 0) {
		session->suspended = true; status.state = RtssState::Conflict;
		status.failureStage = L"Rtss_Failure_InstallationChanged"; _PublishSession(session, std::move(status)); return;
	}
	if (!status.rtss.running) { session->suspended = true; status.state = RtssState::NotRunning; _PublishSession(session, std::move(status)); return; }
	if (target && !target->Valid()) {
		const auto rebound = ResolveTarget(target->source);
		if (rebound && session->initialTarget && rebound->SameProcess(*session->initialTarget)) {
			target = rebound; session->target = rebound;
		}
	}
	if (!target || !target->Valid()) {
		session->suspended = true; status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Target"; _PublishSession(session, std::move(status)); return;
	}
	if (_stricmp(target->profile.c_str(), _access.SelfProfile().c_str()) == 0) {
		session->suspended = true; status.state = RtssState::Failed;
		status.failureStage = L"Rtss_Failure_SelfProfileConflict"; _PublishSession(session, std::move(status)); return;
	}
	if (!status.desiredRate) { status.state = RtssState::WaitingContent; _PublishSession(session, std::move(status)); return; }
	RtssProfileSnapshot before;
	std::wstring error;
	status.state = RtssState::Applying; _PublishSession(session, status);
	if (!_access.Read(session->profile, before, error)) {
		session->suspended = true; LogProfileFailure("read", error, _access.Failure());
		status.state = RtssState::Failed; status.failureStage = ProfileFailureText(_access.Failure(), error, false);
		_PublishSession(session, std::move(status)); return;
	}
	if (session->modified && (!RtssProfileAccess::SameRate(before, session->lastWritten) ||
		(session->created && (before.contents != session->lastWritten.contents || before.detectionLevel != session->lastWritten.detectionLevel)))) {
		session->suspended = true; status.state = RtssState::Conflict; _PublishSession(session, std::move(status)); return;
	}
	if (session->modified && before.rate[0] == status.desiredRate && before.rate[1] == 1 && before.rate[2] == 0) {
		if (session->reloadPending && !_access.ReloadProfiles(error, [session, target] { return !session->cancelled && target->Valid(); })) {
			session->suspended = true; status.state = RtssState::Failed;
			status.profileSaved = true; SetRateStatus(before, status);
			status.failureStage = ProfileFailureText(_access.Failure(), error, false);
			_PublishSession(session, std::move(status)); return;
		}
		session->reloadPending = false;
		status.state = RtssState::Written; status.currentRate = status.desiredRate;
		status.rateKnown = true; status.reloaded = true;
		status.detectionDisabled = before.detectionKnown && !before.detectionLevel; _PublishSession(session, std::move(status)); return;
	}
	if (session->cancelled || !target->Valid()) return;
	if (!session->modified) { session->original = before; session->created = !before.exists; session->installation = status.rtss.executable.parent_path(); }
	RtssProfileSnapshot written;
	bool saveAttempted = false, readbackComplete = false;
	bool success = false;
	try {
		success = _access.Apply(session->profile, status.desiredRate, before, written, saveAttempted, readbackComplete, error,
			[session, target, rate = status.desiredRate] { return !session->cancelled && target->Valid() && session->desiredRate == rate; });
	} catch (...) { error = L"WriteException"; }
	if (written.exists && written.rate[0] == status.desiredRate && written.rate[1] == 1 && written.rate[2] == 0) {
		session->modified = true; session->lastWritten = written;
	} else if (saveAttempted && !(readbackComplete && RtssProfileAccess::SameRate(before, written))) {
		// Save is void: even failed readback may follow a real external write.
		// Retain the original and try guarded cleanup once. A new profile with
		// unverified content cannot satisfy deletion ownership checks.
		session->modified = true;
		session->lastWritten = before;
		if (!before.exists) session->lastWritten.contents.clear();
		session->lastWritten.exists = true;
		session->lastWritten.rate = { status.desiredRate, 1, 0, before.rate[3] };
	}
	if (!success) {
		if (error == L"Cancelled") return;
		status.profileSaved = _access.Failure().stage == RtssFailureStage::Reload && readbackComplete;
		if (status.profileSaved) { SetRateStatus(written, status); session->reloadPending = true; }
		LogProfileFailure("write", error, _access.Failure()); status.state = error == L"Conflict" ? RtssState::Conflict : RtssState::Failed;
		status.failureStage = error == L"Conflict" ? L"" : ProfileFailureText(_access.Failure(), error, false);
		session->suspended = true; _PublishSession(session, std::move(status)); return;
	}
	status.state = RtssState::Written; status.currentRate = status.desiredRate;
	status.rateKnown = true;
	status.detectionDisabled = written.detectionKnown && !written.detectionLevel;
	status.reloaded = status.rtss.running;
	session->reloadPending = false;
	if (session->profile.empty()) {
		RtssProfileSnapshot targetProfile;
		if (_access.Read(target->profile, targetProfile, error)) status.targetOverridesGlobal = targetProfile.exists;
	}
	_PublishSession(session, std::move(status));
}

void RtssService::_Finish(const std::shared_ptr<Session>& session) {
	if (session->finished) return;
	session->finished = true;
	if (!session->modified) return;
	auto status = SessionStatus();
	status.selfInitialization = status.manualOperation = false;
	status.sessionGeneration = session->generation;
	status.scope = session->settings.scope;
	status.rateKnown = false;
	status.timeLimit = false;
	status.targetOverridesGlobal = false;
	status.failureStage.clear();
	status.reloaded = status.profileSaved = status.rateUnavailable = false;
	status.profileIdentity = session->identity;
	status.profile = session->profile.empty() ? L"Global" : session->initialTarget->exe;
	if (!_Prepare(status) || _wcsicmp(session->installation.c_str(), status.rtss.executable.parent_path().c_str()) != 0) {
		status.state = session->settings.exitPolicy == RtssExitPolicy::KeepChanges ? RtssState::Kept : RtssState::RestoreFailed;
		if (_wcsicmp(session->installation.c_str(), status.rtss.executable.parent_path().c_str()) != 0) status.failureStage = L"Rtss_Failure_InstallationChanged";
		_Publish(std::move(status)); return;
	}
	if (session->settings.exitPolicy == RtssExitPolicy::KeepChanges) {
		RtssProfileSnapshot current;
		std::wstring error;
		if (_access.Read(session->profile, current, error)) {
			if (!RtssProfileAccess::SameRate(current, session->lastWritten)) {
				status.state = RtssState::Conflict; _Publish(std::move(status)); return;
			}
			SetRateStatus(current, status);
		}
		status.state = RtssState::Kept; _Publish(std::move(status)); return;
	}
	std::wstring error;
	bool conflict = false;
	bool restored = false;
	try { restored = _access.Restore(session->profile, session->original, session->lastWritten, session->created, conflict, error); }
	catch (...) { error = L"RestoreException"; }
	if (!restored) {
		status.profileSaved = _access.Failure().stage == RtssFailureStage::Reload && _access.Failure().readbackComplete;
		LogProfileFailure("restore", error, _access.Failure()); status.state = conflict ? RtssState::Conflict : RtssState::RestoreFailed;
		status.failureStage = conflict ? L"" : ProfileFailureText(_access.Failure(), error, true);
	} else { status.state = RtssState::Restored; status.reloaded = status.rtss.running; }
	_Publish(std::move(status));
}

bool RtssService::HasSession(std::weak_ptr<const uint8_t> identity) const noexcept {
	const auto owner = identity.lock();
	return _uiSession && !_uiSession->cancelled && owner && owner == _uiSession->identity.lock();
}

bool RtssService::IsCurrentStatus(const RtssStatus& status, std::weak_ptr<const uint8_t> identity) const noexcept {
	return HasSession(identity) && status.sessionGeneration == _uiSession->generation && status.profileIdentity.lock() == identity.lock();
}

void RtssService::Retry(std::weak_ptr<const uint8_t> identity) {
	if (!HasSession(identity) || _autoPreparing) return;
	const auto session = _uiSession;
	_Queue([this, session] {
		if (session->cancelled || _workerSession != session || session->finished || SessionStatus().state == RtssState::Conflict) return;
		session->suspended = false; session->prepared = false;
		_TryApply(session);
	});
}

void RtssService::Recheck(bool retry) {
	if (retry && _uiSession) { Retry(_uiSession->identity); return; }
	if (_recheckPending || !_notify) return;
	_recheckPending = true;
	StatusChanged.Invoke();
	_Queue([this] {
		// Inventory only: never launch, initialize or apply a target profile.
		RtssStatus status;
		try {
			_Prepare(status);
			_RefreshMagpieInitialization(status);
			_Publish(std::move(status));
			if (_workerSession) _CheckSession(_workerSession);
		} catch (...) {
			status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Access";
			_Publish(std::move(status));
		}
		if (_notify) _dispatcher.TryEnqueue([this] {
			_recheckPending = false;
			if (_notify) StatusChanged.Invoke();
		});
	});
}

void RtssService::_PublishSession(const std::shared_ptr<Session>& session, RtssStatus status) {
	if (session->cancelled || session->finished || session->identity.expired() || _workerSession != session) return;
	status.profileIdentity = session->identity;
	status.sessionGeneration = session->generation;
	status.scope = session->settings.scope;
	status.selfInitialization = status.manualOperation = false;
	_Publish(std::move(status));
}

bool RtssService::_PrepareSession(const std::shared_ptr<Session>& session, RtssStatus& status) {
	const auto valid = [session] {
		const auto target = session->target.load();
		return !session->cancelled && !session->identity.expired() && target && target->Valid();
	};
	if (!valid()) {
		if (!session->cancelled) {
			session->suspended = true; status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Target";
			_PublishSession(session, std::move(status));
		}
		return false;
	}
	_autoPreparing = true;
	auto clearPreparing = wil::scope_exit([this] {
		_autoPreparing = false;
		if (_notify) _dispatcher.TryEnqueue([this] { if (_notify) StatusChanged.Invoke(); });
	});
	const auto fail = [&] {
		session->suspended = true; session->prepared = false;
		_PublishSession(session, status); return false;
	};
	status.state = RtssState::Checking; _PublishSession(session, status);
	if (!_Prepare(status)) return fail();
	if (!valid()) return false;
	if (!status.rtss.running) {
		status.state = RtssState::StartingTool; _PublishSession(session, status);
		std::wstring error;
		if (!RtssProfileAccess::LaunchTool(status.rtss, error)) {
			status.state = RtssState::ToolStartFailed;
			status.failureStage = error == L"UntrustedLaunchDirectory" ? L"Rtss_Failure_ProtectedLaunch" :
				error == L"UntrustedInstallation" ? L"Rtss_Failure_UntrustedInstallation" : L"Rtss_Failure_Launch";
			LogFailure("automatic launch", error); return fail();
		}
		// Bounded readiness check on the worker. Stop/new-session cancellation is
		// checked every 250 ms; the rendering and UI threads continue independently.
		for (int attempt = 0; attempt < 40 && valid(); ++attempt) {
			status.rtss = RtssProfileAccess::DetectTool(false);
			if (status.rtss.running) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
		}
		if (!valid()) return false;
		if (!status.rtss.running) {
			status.state = RtssState::ToolStartFailed; status.failureStage = L"Rtss_Failure_StartTimeout"; return fail();
		}
		if (!_Prepare(status)) return fail();
	}
	if (!valid()) return false;
	if (session->modified && _wcsicmp(session->installation.c_str(), status.rtss.executable.parent_path().c_str()) != 0) {
		status.state = RtssState::Conflict; status.failureStage = L"Rtss_Failure_InstallationChanged"; return fail();
	}
	if (!session->prepared) {
		const auto target = session->target.load();
		if (_stricmp(target->profile.c_str(), _access.SelfProfile().c_str()) == 0) {
			status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_SelfProfileConflict"; return fail();
		}
		status.state = RtssState::InitializingMagpie; _PublishSession(session, status);
		RtssProfileSnapshot written;
		bool alreadyUnlimited = false;
		std::wstring error;
		if (!_access.InitializeUnlimited(_access.SelfProfile(), written, alreadyUnlimited, error, valid)) {
			if (error == L"Cancelled") return false;
			status.state = error == L"Conflict" ? RtssState::Conflict : RtssState::Failed;
			status.failureStage = ProfileFailureText(_access.Failure(), error, false);
			status.profileSaved = _access.Failure().stage == RtssFailureStage::Reload && _access.Failure().readbackComplete;
			session->selfReloadPending = status.profileSaved;
			// This partial write belongs to Magpie, not to the target cap.
			status.profile = Win32Helper::GetExePath().filename().native();
			LogProfileFailure("automatic initialize Magpie", error, _access.Failure()); return fail();
		}
		if (session->selfReloadPending && !_access.ReloadProfiles(error, valid)) {
			status.state = RtssState::Failed; status.profileSaved = true;
			status.profile = Win32Helper::GetExePath().filename().native();
			status.failureStage = ProfileFailureText(_access.Failure(), error, false); return fail();
		}
		session->selfReloadPending = false;
		_magpieInitialization = RtssInitializationState::Initialized;
		session->prepared = true;
	}
	return valid();
}

void RtssService::Launch(bool afterburner) {
	if (_launchPending || _initializationPending || _autoPreparing) return;
	_launchPending = true;
	_Queue([this, afterburner] {
		auto status = InventoryStatus(); status.state = RtssState::StartingTool;
		status.selfInitialization = false; status.manualOperation = true; status.rateKnown = false;
		status.reloaded = status.profileSaved = status.rateUnavailable = false;
		status.detectionDisabled = status.targetOverridesGlobal = false;
		status.profileIdentity.reset(); status.sessionGeneration = 0;
		status.profile = afterburner ? L"MSI Afterburner" : L"RTSS"; status.failureStage.clear(); _Publish(status);
		std::wstring error;
		const auto tool = RtssProfileAccess::DetectTool(afterburner);
		const bool success = RtssProfileAccess::LaunchTool(tool, error);
		if (!success) {
			LogFailure("launch", error);
			status.state = RtssState::ToolStartFailed;
			status.failureStage = error == L"UntrustedLaunchDirectory" ? L"Rtss_Failure_ProtectedLaunch" :
				error == L"UntrustedInstallation" ? L"Rtss_Failure_UntrustedInstallation" : L"Rtss_Failure_Launch";
			_Publish(status);
		}
		if (_notify) _dispatcher.TryEnqueue([this, success, afterburner] {
			if (!success || !_notify) { _launchPending = false; return; }
			// One bounded readiness window after an explicit user launch. Checking
			// every 500 ms for 10 s only queries running state; it never writes per frame.
			_launchTimer = _dispatcher.CreateTimer();
			_launchTimer.Interval(std::chrono::milliseconds(500));
			_launchTimer.Tick([this, afterburner, attempts = 0](winrt::DispatcherQueueTimer const& timer, winrt::IInspectable const&) mutable {
				const auto s = Status();
				const bool selectedRunning = afterburner ? s.afterburner.running : s.rtss.running;
				const bool timeout = ++attempts >= 20;
				if (!_notify || selectedRunning || timeout) {
					timer.Stop(); _launchPending = false;
					StatusChanged.Invoke();
				}
				if (_notify && selectedRunning) {
					_Queue([this, afterburner] {
						RtssStatus status;
						_Prepare(status);
						status.manualOperation = true;
						status.profile = afterburner ? L"MSI Afterburner" : L"RTSS";
						status.state = RtssState::ToolStarted;
						status.failureStage.clear();
						_Publish(std::move(status));
					});
					return;
				}
				if (_notify && timeout && !selectedRunning) {
					_Queue([this, afterburner] {
						const auto tool = RtssProfileAccess::DetectTool(afterburner);
						auto status = InventoryStatus();
						status.state = tool.running ? RtssState::ToolStarted : RtssState::ToolStartFailed;
						if (afterburner) status.afterburner = tool; else status.rtss = tool;
						status.selfInitialization = false; status.manualOperation = true; status.rateKnown = false;
						status.reloaded = status.profileSaved = status.rateUnavailable = false;
						status.detectionDisabled = status.targetOverridesGlobal = false;
						status.profileIdentity.reset(); status.sessionGeneration = 0;
						status.profile = afterburner ? L"MSI Afterburner" : L"RTSS";
						status.failureStage = tool.running ? L"" : L"Rtss_Failure_StartTimeout";
						_Publish(std::move(status));
					});
					return;
				}
				if (_notify) Recheck(false);
			});
			_launchTimer.Start();
		});
	});
}

void RtssService::InitializeMagpie() {
	if (_initializationPending || _launchPending || _autoPreparing || !_notify) return;
	_initializationPending = true;
	StatusChanged.Invoke();
	_Queue([this] {
		_InitializeMagpie();
		if (_notify) _dispatcher.TryEnqueue([this] {
			_initializationPending = false;
			if (_notify) StatusChanged.Invoke();
		});
	});
}

void RtssService::_InitializeMagpie() {
	RtssStatus status;
	status.selfInitialization = true;
	status.profile = Win32Helper::GetExePath().filename().native();
	std::wstring error;
	try {
		if (_Prepare(status)) {
			if (_workerSession && !_workerSession->cancelled && !_workerSession->finished) {
				const auto target = _workerSession->target.load();
				if (target && target->Valid() && _stricmp(target->profile.c_str(), _access.SelfProfile().c_str()) == 0) {
					status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_SelfProfileConflict";
					_Publish(std::move(status)); return;
				}
			}
			status.state = RtssState::InitializingMagpie; _Publish(status);
			RtssProfileSnapshot written;
			bool alreadyUnlimited = false;
			const auto name = ProfileName(status.profile);
			if (_access.InitializeUnlimited(name, written, alreadyUnlimited, error, [this] { return _notify.load(); })) {
				status.state = alreadyUnlimited ? RtssState::MagpieAlreadyUnlimited : RtssState::MagpieInitialized;
				_magpieInitialization = RtssInitializationState::Initialized;
				status.currentRate = 0; status.rateKnown = true;
				status.reloaded = !alreadyUnlimited && status.rtss.running;
			} else {
				status.state = error == L"Conflict" ? RtssState::Conflict : RtssState::Failed;
				status.failureStage = error == L"Conflict" ? L"" : ProfileFailureText(_access.Failure(), error, false);
				status.profileSaved = _access.Failure().stage == RtssFailureStage::Reload && _access.Failure().readbackComplete;
				if (status.profileSaved) { status.currentRate = 0; status.rateKnown = true;
					_magpieInitialization = RtssInitializationState::Initialized; }
				LogProfileFailure("initialize Magpie", error, _access.Failure());
			}
		}
	} catch (...) {
		status.state = RtssState::Failed; status.failureStage = L"Rtss_Failure_Write";
		LogProfileFailure("initialize Magpie", L"WriteException", _access.Failure());
	}
	_RefreshMagpieInitialization(status);
	_Publish(std::move(status));
}

void RtssService::_RefreshMagpieInitialization(const RtssStatus& status) {
	// This is inventory work, performed on page preview/recheck rather than the
	// content-rate update path. Never confuse Global or the game with our profile.
	auto state = RtssInitializationState::Unknown;
	if (status.readReady && _access.CanRead() && !_access.SelfProfile().empty()) {
		RtssProfileSnapshot snapshot;
		std::wstring error;
		if (_access.Read(_access.SelfProfile(), snapshot, error)) {
			if (!snapshot.exists) state = RtssInitializationState::Uninitialized;
			else if (snapshot.rateKnown[0] && snapshot.rateKnown[2])
				state = !snapshot.rate[0] && !snapshot.rate[2] ? RtssInitializationState::Initialized
					: RtssInitializationState::Uninitialized;
		}
	}
	_magpieInitialization = state;
}

void RtssService::Preview(RtssSettings settings, std::wstring executable, std::function<void(RtssStatus)> completed) {
	_Queue([this, settings, executable = std::move(executable), completed = std::move(completed)] {
		RtssStatus status;
		if (_Prepare(status)) {
			_RefreshMagpieInitialization(status);
			std::string profile;
			if (settings.scope == RtssScope::Target) profile = ProfileName(std::filesystem::path(executable).filename().native());
			if (settings.scope == RtssScope::Target && profile.empty()) status.state = RtssState::WaitingTarget;
			else {
				RtssProfileSnapshot snapshot;
				std::wstring error;
				status.profile = profile.empty() ? L"Global" : std::filesystem::path(executable).filename().native();
				if (_access.Read(profile, snapshot, error)) {
					status.state = snapshot.exists ? RtssState::Read : RtssState::MissingProfile;
					SetRateStatus(snapshot, status);
				} else {
					status.state = RtssState::Failed; status.failureStage = ProfileFailureText(_access.Failure(), error, false);
					LogProfileFailure("preview", error, _access.Failure());
				}
			}
		}
		if (!status.readReady) _magpieInitialization = RtssInitializationState::Unknown;
		SetAccessStatus(_access, status);
		{
			std::lock_guard lock(_mutex); _inventoryStatus = status;
		}
		if (_notify) _dispatcher.TryEnqueue([this, status = std::move(status), completed] {
			if (_notify) completed(status);
		});
	});
}

}
