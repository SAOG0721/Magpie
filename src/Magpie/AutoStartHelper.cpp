#include "pch.h"
#include "AutoStartHelper.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include "StrHelper.h"
#include "Win32Helper.h"
#include <Lmcons.h>
#include <sddl.h>
#include <ShlObj.h>
#include <taskschd.h>

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 实现开机启动
//
// 首先尝试使用任务计划程序，此方案的优点是以管理员身份启动时不会显示 UAC。
// 如果创建任务失败，则回落到在当前用户的启动文件夹中创建快捷方式。
//
// 任务计划程序的使用参考自
// https://github.com/microsoft/PowerToys/blob/3d54cb838504c12f59516afaf1a00fde2dd5d01b/src/runner/auto_start_helper.cpp
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

using namespace winrt;

namespace Magpie {

static constexpr auto TASK_FOLDER = L"\\Magpie";

struct TaskAccount {
	std::wstring sid;
	std::wstring qualifiedName;
	std::wstring legacyName;
};

static std::wstring SidString(PSID sid) noexcept {
	LPWSTR value = nullptr;
	if (!ConvertSidToStringSidW(sid, &value)) return {};
	const std::unique_ptr<void, decltype(&LocalFree)> owner(value, LocalFree);
	return value;
}

static TaskAccount CurrentTaskAccount() noexcept {
	wil::unique_handle token;
	alignas(TOKEN_USER) BYTE tokenUser[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]{};
	DWORD size = 0;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, token.put()) ||
		!GetTokenInformation(token.get(), TokenUser, tokenUser, sizeof(tokenUser), &size)) {
		Logger::Get().Win32Error("获取当前用户 SID 失败");
		return {};
	}
	const auto sid = reinterpret_cast<const TOKEN_USER*>(tokenUser)->User.Sid;
	TaskAccount result;
	result.sid = SidString(sid);
	WCHAR name[UNLEN + 1]{}, domain[256]{};
	DWORD nameLength = ARRAYSIZE(name), domainLength = ARRAYSIZE(domain);
	SID_NAME_USE use{};
	if (LookupAccountSidW(nullptr, sid, name, &nameLength, domain, &domainLength, &use)) {
		result.qualifiedName = *domain ? StrHelper::Concat(domain, L"\\", name) : name;
		result.legacyName = StrHelper::Concat(L"Autorun for ", name);
	}
	return result;
}


static std::wstring GetTaskName(const TaskAccount& account) noexcept {
	return StrHelper::Concat(L"Autorun for ", account.sid);
}

static bool IsTaskAccount(BSTR userId, const TaskAccount& account) noexcept {
	if (!userId || !*userId) return false;
	if (_wcsicmp(userId, account.sid.c_str()) == 0 ||
		_wcsicmp(userId, account.qualifiedName.c_str()) == 0) return true;
	alignas(DWORD) BYTE sid[SECURITY_MAX_SID_SIZE]{};
	WCHAR domain[256]{};
	DWORD size = sizeof(sid), domainSize = ARRAYSIZE(domain);
	SID_NAME_USE use{};
	return LookupAccountNameW(nullptr, userId, sid, &size, domain, &domainSize, &use) &&
		SidString(sid) == account.sid;
}

static std::wstring TaskSecurityDescriptor(const TaskAccount& account) noexcept {
	return StrHelper::Concat(L"O:", account.sid, L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;", account.sid, L")");
}

static bool HasRestrictedTaskAcl(IRegisteredTask* task, const TaskAccount& account) noexcept {
	wil::unique_bstr sddl;
	if (FAILED(task->GetSecurityDescriptor(OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, sddl.put()))) {
		return false;
	}
	PSECURITY_DESCRIPTOR raw = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.get(), SDDL_REVISION_1, &raw, nullptr)) return false;
	const std::unique_ptr<void, decltype(&LocalFree)> descriptor(raw, LocalFree);
	PSID owner = nullptr;
	PACL acl = nullptr;
	BOOL present = FALSE, defaulted = FALSE;
	SECURITY_DESCRIPTOR_CONTROL control{};
	DWORD revision = 0;
	if (!GetSecurityDescriptorOwner(raw, &owner, &defaulted) || !owner || SidString(owner) != account.sid ||
		!GetSecurityDescriptorControl(raw, &control, &revision) || !(control & SE_DACL_PROTECTED) ||
		!GetSecurityDescriptorDacl(raw, &present, &acl, &defaulted) || !present || !acl || acl->AceCount != 3) return false;
	bool system = false, admins = false, user = false;
	for (DWORD i = 0; i < acl->AceCount; ++i) {
		void* rawAce = nullptr;
		if (!GetAce(acl, i, &rawAce)) return false;
		const auto ace = static_cast<ACCESS_ALLOWED_ACE*>(rawAce);
		if (ace->Header.AceType != ACCESS_ALLOWED_ACE_TYPE || ace->Header.AceFlags || ace->Mask != FILE_ALL_ACCESS) return false;
		PSID sid = &ace->SidStart;
		if (IsWellKnownSid(sid, WinLocalSystemSid)) system = true;
		else if (IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) admins = true;
		else if (SidString(sid) == account.sid) user = true;
		else return false;
	}
	return system && admins && user;
}

static com_ptr<ITaskService> CreateTaskService() noexcept {
	com_ptr<ITaskService> taskService = try_create_instance<ITaskService>(CLSID_TaskScheduler);
	if (!taskService) {
		Logger::Get().Error("创建 TaskService 失败");
		return nullptr;
	}

	HRESULT hr = taskService->Connect(Win32Helper::Variant(), Win32Helper::Variant(),
		Win32Helper::Variant(), Win32Helper::Variant());
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskService::Connect 失败", hr);
		return nullptr;
	}

	return taskService;
}

static com_ptr<ITaskFolder> OpenTaskFolder(ITaskService* service, bool create) noexcept {
	com_ptr<ITaskFolder> folder;
	const auto name = wil::make_bstr_nothrow(TASK_FOLDER);
	if (SUCCEEDED(service->GetFolder(name.get(), folder.put())) || !create) return folder;
	com_ptr<ITaskFolder> root;
	HRESULT hr = service->GetFolder(wil::make_bstr_nothrow(L"\\").get(), root.put());
	if (SUCCEEDED(hr)) hr = root->CreateFolder(name.get(), Win32Helper::Variant(), folder.put());
	if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS)) hr = service->GetFolder(name.get(), folder.put());
	if (FAILED(hr)) Logger::Get().ComError("打开或创建 Magpie 任务文件夹失败", hr);
	return folder;
}

struct ExistingAutoStartTask {
	com_ptr<IRegisteredTask> task;
	TASK_RUNLEVEL_TYPE runLevel{};
	bool enabled = false;

	explicit operator bool() const noexcept { return bool(task); }
};

static bool MatchesAutoStartExecutable(std::wstring_view path, AutoStartHelper::Scope scope) noexcept {
	const std::filesystem::path executable(path);
	if (!executable.is_absolute()) return false;
	if (_wcsicmp(executable.c_str(), Win32Helper::GetExePath().c_str()) == 0) return true;
	return scope == AutoStartHelper::Scope::CurrentUser &&
		_wcsicmp(executable.filename().c_str(), L"Magpie.exe") == 0;
}

static ExistingAutoStartTask FindAutoStartTask(
	ITaskFolder* folder, const TaskAccount& account, bool legacy, AutoStartHelper::Scope scope
) noexcept {
	const auto name = legacy ? account.legacyName : GetTaskName(account);
	if (name.empty()) return {};
	ExistingAutoStartTask result;
	if (FAILED(folder->GetTask(wil::make_bstr_nothrow(name.c_str()).get(), result.task.put()))) return {};
	com_ptr<ITaskDefinition> definition;
	com_ptr<IPrincipal> principal;
	wil::unique_bstr principalUser;
	TASK_LOGON_TYPE logonType{};
	VARIANT_BOOL enabled{};
	if (FAILED(result.task->get_Definition(definition.put())) ||
		FAILED(result.task->get_Enabled(&enabled)) ||
		FAILED(definition->get_Principal(principal.put())) ||
		FAILED(principal->get_UserId(principalUser.put())) || !IsTaskAccount(principalUser.get(), account) ||
		FAILED(principal->get_LogonType(&logonType)) || logonType != TASK_LOGON_INTERACTIVE_TOKEN ||
		FAILED(principal->get_RunLevel(&result.runLevel))) return {};
	com_ptr<ITriggerCollection> triggers;
	com_ptr<ITrigger> trigger;
	LONG count = 0;
	if (FAILED(definition->get_Triggers(triggers.put())) || FAILED(triggers->get_Count(&count)) || count != 1 ||
		FAILED(triggers->get_Item(1, trigger.put()))) return {};
	const auto logon = trigger.try_as<ILogonTrigger>();
	wil::unique_bstr logonUser;
	if (!logon || FAILED(logon->get_UserId(logonUser.put())) || !IsTaskAccount(logonUser.get(), account)) return {};
	com_ptr<IActionCollection> actions;
	com_ptr<IAction> action;
	if (FAILED(definition->get_Actions(actions.put())) || FAILED(actions->get_Count(&count)) || count != 1 ||
		FAILED(actions->get_Item(1, action.put()))) return {};
	const auto exec = action.try_as<IExecAction>();
	wil::unique_bstr arguments, path;
	if (!exec || FAILED(exec->get_Path(path.put())) || !path || !MatchesAutoStartExecutable(path.get(), scope) ||
		FAILED(exec->get_Arguments(arguments.put())) || !arguments ||
		wcscmp(arguments.get(), CommonSharedConstants::OPTION_LAUNCH_WITHOUT_WINDOW) != 0) return {};
	if (!legacy && !HasRestrictedTaskAcl(result.task.get(), account)) return {};
	result.enabled = enabled == VARIANT_TRUE;
	return result;
}

static bool RegisterAutoStartTask(
	ITaskService* taskService, ITaskFolder* taskFolder, const TaskAccount& account,
	const std::wstring& name, bool update, bool runElevated, bool enabled
) noexcept {
	const auto accountSid = wil::make_bstr_nothrow(account.sid.c_str());

	// Create the task builder object to create the task.
	com_ptr<ITaskDefinition> task;
	HRESULT hr = taskService->NewTask(0, task.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("创建 ITaskDefinition 失败", hr);
		return false;
	}

	// ------------------------------------------------------
	// Get the registration info for setting the identification.
	com_ptr<IRegistrationInfo> regInfo;
	hr = task->get_RegistrationInfo(regInfo.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 IRegistrationInfo 失败", hr);
		return false;
	}

	const auto& author = account.qualifiedName.empty() ? account.sid : account.qualifiedName;
	hr = regInfo->put_Author(wil::make_bstr_nothrow(author.c_str()).get());
	if (FAILED(hr)) {
		Logger::Get().ComError("IRegistrationInfo::put_Author 失败", hr);
		return false;
	}

	 // ------------------------------------------------------
	// Create the settings for the task
	com_ptr<ITaskSettings> taskSettings;
	hr = task->get_Settings(taskSettings.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 ITaskSettings 失败", hr);
		return false;
	}

	hr = taskSettings->put_Enabled(enabled ? VARIANT_TRUE : VARIANT_FALSE);
	if (SUCCEEDED(hr)) hr = taskSettings->put_StartWhenAvailable(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_StartWhenAvailable 失败", hr);
		return false;
	}
	hr = taskSettings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_StopIfGoingOnBatteries 失败", hr);
		return false;
	}
	// 不限制执行时间
	hr = taskSettings->put_ExecutionTimeLimit(wil::make_bstr_nothrow(L"PT0S").get());
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_ExecutionTimeLimit 失败", hr);
		return false;
	}
	hr = taskSettings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
	if (FAILED(hr)) {
		Logger::Get().ComError("ITaskSettings::put_DisallowStartIfOnBatteries 失败", hr);
		return false;
	}

	// ------------------------------------------------------
	// Get the trigger collection to insert the logon trigger.
	com_ptr<ITriggerCollection> triggerCollection;
	hr = task->get_Triggers(triggerCollection.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取 ITriggerCollection 失败", hr);
		return false;
	}

	// Add the logon trigger to the task.
	{
		com_ptr<ITrigger> trigger;
		hr = triggerCollection->Create(TASK_TRIGGER_LOGON, trigger.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 ITrigger 失败", hr);
			return false;
		}

		com_ptr<ILogonTrigger> logonTrigger = trigger.try_as<ILogonTrigger>();
		if (!logonTrigger) {
			Logger::Get().Error("获取 ILogonTrigger 失败");
			return false;
		}

		logonTrigger->put_Id(wil::make_bstr_nothrow(L"Trigger1").get());

		// Timing issues may make explorer not be started when the task runs.
		// Add a little delay to mitigate this.
		logonTrigger->put_Delay(wil::make_bstr_nothrow(L"PT03S").get());

		// Define the user. The task will execute when the user logs on.
		// The specified user must be a user on this computer.
		hr = logonTrigger->put_UserId(accountSid.get());
		if (FAILED(hr)) {
			Logger::Get().ComError("ILogonTrigger::put_UserId 失败", hr);
			return false;
		}
	}

	// ------------------------------------------------------
	// Add an Action to the task. This task will execute the path passed to this custom action.
	{
		com_ptr<IActionCollection> actionCollection;

		// Get the task action collection pointer.
		hr = task->get_Actions(actionCollection.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("获取 IActionCollection 失败", hr);
			return false;
		}

		// Create the action, specifying that it is an executable action.
		com_ptr<IAction> action;
		hr = actionCollection->Create(TASK_ACTION_EXEC, action.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("创建 IAction 失败", hr);
			return false;
		}

		// QI for the executable task pointer.
		com_ptr<IExecAction> execAction = action.try_as<IExecAction>();
		if (!execAction) {
			Logger::Get().Error("获取 IExecAction 失败");
			return false;
		}

		// Set the path of the executable to Magpie (passed as CustomActionData).
		const std::filesystem::path& exePath = Win32Helper::GetExePath();
		hr = execAction->put_Path(wil::make_bstr_nothrow(exePath.c_str()).get());
		if (FAILED(hr)) {
			Logger::Get().ComError("设置可执行文件路径失败", hr);
			return false;
		}

		hr = execAction->put_Arguments(wil::make_bstr_nothrow(
			CommonSharedConstants::OPTION_LAUNCH_WITHOUT_WINDOW).get());
		if (FAILED(hr)) return false;
	}

	// ------------------------------------------------------
	// Create the principal for the task
	{
		com_ptr<IPrincipal> principal;
		hr = task->get_Principal(principal.put());
		if (FAILED(hr)) {
			Logger::Get().ComError("获取 IPrincipal 失败", hr);
			return false;
		}

		// Set up principal information:
		principal->put_Id(wil::make_bstr_nothrow(L"Principal1").get());
		hr = principal->put_UserId(accountSid.get());
		if (SUCCEEDED(hr)) hr = principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
		if (FAILED(hr)) return false;

		if (runElevated) {
			hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
		} else {
			hr = principal->put_RunLevel(TASK_RUNLEVEL_LUA);
		}

		if (FAILED(hr)) {
			Logger::Get().ComError("IPrincipal::put_RunLevel 失败", hr);
			return false;
		}
	}

	// ------------------------------------------------------
	//  Save the task in the Magpie folder.
	{
		com_ptr<IRegisteredTask> registeredTask;
		const auto sddl = TaskSecurityDescriptor(account);
		const LONG flags = (update ? TASK_UPDATE : TASK_CREATE) |
			TASK_DONT_ADD_PRINCIPAL_ACE | TASK_IGNORE_REGISTRATION_TRIGGERS;
		hr = taskFolder->RegisterTaskDefinition(
			wil::make_bstr_nothrow(name.c_str()).get(),
			task.get(),
			flags,
			Win32Helper::Variant(account.sid.c_str()),
			Win32Helper::Variant(),
			TASK_LOGON_INTERACTIVE_TOKEN,
			Win32Helper::Variant(sddl.c_str()),
			registeredTask.put()
		);
		if (FAILED(hr)) {
			Logger::Get().ComError("注册任务失败", hr);
			return false;
		}

	}

	return true;
}

static bool CreateAutoStartTask(bool runElevated, AutoStartHelper::Scope scope) noexcept {
	const auto account = CurrentTaskAccount();
	if (account.sid.empty()) return false;
	const auto service = CreateTaskService();
	if (!service) return false;
	const auto folder = OpenTaskFolder(service.get(), true);
	if (!folder) return false;
	const auto current = FindAutoStartTask(folder.get(), account, false, scope);
	const bool legacy = !current && bool(FindAutoStartTask(folder.get(), account, true, scope));
	const auto name = legacy ? account.legacyName : GetTaskName(account);
	return RegisterAutoStartTask(service.get(), folder.get(), account, name, current || legacy, runElevated, true);
}

static bool DeleteAutoStartTask() noexcept {
	const auto account = CurrentTaskAccount();
	if (account.sid.empty()) return false;
	const auto service = CreateTaskService();
	if (!service) return false;
	com_ptr<ITaskFolder> folder;
	const HRESULT openResult = service->GetFolder(wil::make_bstr_nothrow(TASK_FOLDER).get(), folder.put());
	if (FAILED(openResult)) {
		return openResult == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || openResult == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
	}
	bool success = true;
	for (const bool legacy : {false, true}) {
		if (!FindAutoStartTask(folder.get(), account, legacy, AutoStartHelper::Scope::CurrentUser)) continue;
		const auto name = legacy ? account.legacyName : GetTaskName(account);
		const HRESULT hr = folder->DeleteTask(wil::make_bstr_nothrow(name.c_str()).get(), 0);
		if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
			Logger::Get().ComError("删除任务失败", hr);
			success = false;
		}
	}
	return success;
}

static bool IsAutoStartTaskActive(AutoStartHelper::Scope scope) noexcept {
	const auto account = CurrentTaskAccount();
	if (account.sid.empty()) return false;
	const auto service = CreateTaskService();
	if (!service) return false;
	const auto folder = OpenTaskFolder(service.get(), false);
	return folder && (FindAutoStartTask(folder.get(), account, false, scope).enabled ||
		FindAutoStartTask(folder.get(), account, true, scope).enabled);
}

static std::wstring GetShortcutPath() noexcept {
	// 获取用户的启动文件夹路径
	wil::unique_cotaskmem_string startupDir;
	HRESULT hr = SHGetKnownFolderPath(FOLDERID_Startup, 0, NULL, startupDir.put());
	if (FAILED(hr)) {
		Logger::Get().ComError("获取启动文件夹失败", hr);
		return {};
	}

	return StrHelper::Concat(startupDir.get(), L"\\Magpie.lnk");
}

static bool IsAutoStartShortcutFor(const std::wstring& path, AutoStartHelper::Scope scope) noexcept {
	if (path.empty() || !Win32Helper::FileExists(path.c_str())) return false;
	const auto link = try_create_instance<IShellLink>(CLSID_ShellLink);
	if (!link) return false;
	const auto file = link.try_as<IPersistFile>();
	if (!file || FAILED(file->Load(path.c_str(), STGM_READ))) return false;
	WCHAR executable[32768]{}, arguments[256]{};
	return SUCCEEDED(link->GetPath(executable, ARRAYSIZE(executable), nullptr, SLGP_RAWPATH)) &&
		SUCCEEDED(link->GetArguments(arguments, ARRAYSIZE(arguments))) &&
		wcscmp(arguments, CommonSharedConstants::OPTION_LAUNCH_WITHOUT_WINDOW) == 0 &&
		MatchesAutoStartExecutable(executable, scope);
}

static bool CreateAutoStartShortcut(AutoStartHelper::Scope scope) noexcept {
	const auto path = GetShortcutPath();
	if (path.empty() || (Win32Helper::FileExists(path.c_str()) && !IsAutoStartShortcutFor(path, scope))) return false;
	com_ptr<IShellLink> shellLink = try_create_instance<IShellLink>(CLSID_ShellLink);
	if (!shellLink) {
		Logger::Get().Error("创建 ShellLink 失败");
		return false;
	}

	shellLink->SetPath(Win32Helper::GetExePath().c_str());
	shellLink->SetArguments(CommonSharedConstants::OPTION_LAUNCH_WITHOUT_WINDOW);

	com_ptr<IPersistFile> persistFile = shellLink.try_as<IPersistFile>();
	if (!persistFile) {
		Logger::Get().Error("获取 IPersistFile 失败");
		return false;
	}

	HRESULT hr = persistFile->Save(path.c_str(), TRUE);
	if (FAILED(hr)) {
		Logger::Get().ComError("保存快捷方式失败", hr);
		return false;
	}

	return true;
}

static bool DeleteAutoStartShortcut(AutoStartHelper::Scope scope) noexcept {
	std::wstring shortcutPath = GetShortcutPath();
	if (shortcutPath.empty()) {
		return false;
	}
	if (!IsAutoStartShortcutFor(shortcutPath, scope)) return true;

	if (!DeleteFile(shortcutPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
		Logger::Get().Win32Error("删除快捷方式失败");
		return false;
	}

	return true;
}

bool AutoStartHelper::EnableAutoStart(bool runElevated, Scope scope) noexcept {
	if (CreateAutoStartTask(runElevated, scope)) {
		DeleteAutoStartShortcut(scope);
		return true;
	}

	return CreateAutoStartShortcut(scope);
}

bool AutoStartHelper::DisableAutoStart() noexcept {
	// 避免或运算符的短路，确保两者都被删除
	bool result1 = DeleteAutoStartTask();
	bool result2 = DeleteAutoStartShortcut(Scope::CurrentUser);
	return result1 && result2;
}

bool AutoStartHelper::IsAutoStartEnabled(Scope scope) noexcept {
	return IsAutoStartTaskActive(scope) || IsAutoStartShortcutFor(GetShortcutPath(), scope);
}

void AutoStartHelper::MigrateLegacyAutoStartTask() noexcept {
	const auto account = CurrentTaskAccount();
	if (account.sid.empty() || account.legacyName.empty()) return;
	const auto service = CreateTaskService();
	if (!service) return;
	const auto folder = OpenTaskFolder(service.get(), false);
	if (!folder) return;
	const auto old = FindAutoStartTask(folder.get(), account, true, Scope::CurrentExecutable);
	if (!old || HasRestrictedTaskAcl(old.task.get(), account)) return;
	const bool runElevated = old.runLevel == TASK_RUNLEVEL_HIGHEST;
	if (runElevated && !Win32Helper::IsProcessElevated()) return;

	// Atomically replace the verified legacy definition and permissions in place.
	// This preserves its enabled state and leaves it usable if registration fails.
	if (!RegisterAutoStartTask(service.get(), folder.get(), account, account.legacyName, true, runElevated, old.enabled)) {
		Logger::Get().Warn("Could not secure the existing Magpie auto-start task");
	}
}

}
