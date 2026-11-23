#include "pch.h"
#include "RtssProfileAccess.h"
#include "RtssProfileExpression.h"
#include "Win32Helper.h"
#include <charconv>
#include <sstream>
#include <memory>
#include <tlhelp32.h>
#include <shellapi.h>
#include <ShlObj.h>
#include <Aclapi.h>
#include <Softpub.h>
#include <WinTrust.h>
#include <WinCrypt.h>
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "WinTrust.lib")

namespace Magpie {
namespace {

constexpr std::array<const char*, 4> RATE_KEYS{
	"framerate/limit", "framerate/limitdenominator", "framerate/limittime", "framerate/limittimedenominator" };

// Keep SEH boundaries in POD-only functions, separate from C++ transactions.
bool GuardProfile(void (*function)(LPCSTR), LPCSTR profile, bool& faulted) noexcept {
	__try { function(profile); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; return false; }
}

BOOL GuardProperty(BOOL (*function)(LPCSTR, LPBYTE, DWORD), LPCSTR name,
	LPBYTE value, DWORD size, bool& faulted) noexcept {
	__try { return function(name, value, size); }
	__except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; return FALSE; }
}

bool GuardReload(void (*function)(), bool& faulted) noexcept {
	__try { function(); return true; }
	__except (EXCEPTION_EXECUTE_HANDLER) { faulted = true; return false; }
}

std::string Lower(std::string value) {
	for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
	return value;
}

std::string Trim(std::string value) {
	const auto first = value.find_first_not_of(" \t\r\n");
	if (first == std::string::npos) return {};
	return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}

bool NonRateEqual(const RtssProfileSnapshot& a, const RtssProfileSnapshot& b) {
	auto stripped = [](auto fields) {
		for (const auto key : RATE_KEYS) fields.erase(key);
		fields.erase("info/timestamp");
		return fields;
	};
	return stripped(a.fields) == stripped(b.fields);
}

bool ReadDiskFile(const std::filesystem::path& path, std::string& bytes,
	std::array<uint64_t, 2>& identity, DWORD& error) {
	const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (raw == INVALID_HANDLE_VALUE) { error = GetLastError(); return false; }
	const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, CloseHandle);
	BY_HANDLE_FILE_INFORMATION info{};
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(raw, &size) || !GetFileInformationByHandle(raw, &info)) { error = GetLastError(); return false; }
	if (size.QuadPart < 0 || size.QuadPart > 1024 * 1024) { error = ERROR_FILE_TOO_LARGE; return false; }
	bytes.assign(size_t(size.QuadPart), '\0');
	DWORD count = 0;
	if (!ReadFile(raw, bytes.data(), DWORD(bytes.size()), &count, nullptr)) { error = GetLastError(); return false; }
	LARGE_INTEGER after{};
	if (!GetFileSizeEx(raw, &after)) { error = GetLastError(); return false; }
	if (count != bytes.size() || after.QuadPart != size.QuadPart) { error = ERROR_RETRY; return false; }
	identity = { info.dwVolumeSerialNumber, (uint64_t(info.nFileIndexHigh) << 32) | info.nFileIndexLow };
	return true;
}

struct VerifiedToolFile {
	std::filesystem::path path;
	std::unique_ptr<void, decltype(&CloseHandle)> file{ nullptr, CloseHandle };
};

bool IsLocalExecutablePath(const std::filesystem::path& path) {
	return path.is_absolute() && path.root_name().native().size() == 2 &&
		path.root_name().native()[1] == L':';
}

bool IsMachineTrustedSigner(CRYPT_PROVIDER_SGNR* signer, const char* usage, LPCSTR policy) {
	const auto certificate = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
	if (!certificate || !certificate->pCert || signer->dwError != ERROR_SUCCESS) return false;
	CERT_CHAIN_PARA parameters{};
	parameters.cbSize = sizeof(parameters);
	char* usageOid = const_cast<char*>(usage);
	parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
	parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &usageOid;
	PCCERT_CHAIN_CONTEXT chain = nullptr;
	if (!CertGetCertificateChain(HCCE_LOCAL_MACHINE, certificate->pCert, &signer->sftVerifyAsOf,
		certificate->pCert->hCertStore, &parameters, CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL, nullptr, &chain)) return false;
	const std::unique_ptr<const CERT_CHAIN_CONTEXT, decltype(&CertFreeCertificateChain)> chainOwner(chain, CertFreeCertificateChain);
	CERT_CHAIN_POLICY_PARA policyParameters{};
	policyParameters.cbSize = sizeof(policyParameters);
	CERT_CHAIN_POLICY_STATUS result{};
	result.cbSize = sizeof(result);
	return CertVerifyCertificateChainPolicy(policy, chain, &policyParameters, &result) && result.dwError == ERROR_SUCCESS;
}

bool IsMsiSignedToolFile(const std::filesystem::path& path, HANDLE file) {
	WINTRUST_FILE_INFO info{ .cbStruct = sizeof(info), .pcwszFilePath = path.c_str(), .hFile = file };
	WINTRUST_DATA trust{};
	trust.cbStruct = sizeof(trust);
	trust.dwUIChoice = WTD_UI_NONE;
	trust.fdwRevocationChecks = WTD_REVOKE_NONE;
	trust.dwUnionChoice = WTD_CHOICE_FILE;
	trust.pFile = &info;
	trust.dwStateAction = WTD_STATEACTION_VERIFY;
	trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
	GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
	const LONG status = WinVerifyTrust(nullptr, &action, &trust);
	bool expectedPublisher = false;
	if (status == ERROR_SUCCESS) {
		const auto provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
		const auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
		const auto certificate = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
		wchar_t publisher[128]{};
		char organization[] = szOID_ORGANIZATION_NAME;
		if (certificate && certificate->pCert &&
			CertGetNameStringW(certificate->pCert, CERT_NAME_ATTR_TYPE, 0,
				organization, publisher, ARRAYSIZE(publisher)) > 1 &&
			_wcsicmp(publisher, L"MICRO-STAR INTERNATIONAL CO., LTD.") == 0) {
			// Authenticode verifies the file bytes and timestamp binding. Build
			// both chains with machine trust, at the verified signing times.
			expectedPublisher = IsMachineTrustedSigner(signer, szOID_PKIX_KP_CODE_SIGNING, CERT_CHAIN_POLICY_AUTHENTICODE);
			for (DWORD i = 0; expectedPublisher && i < signer->csCounterSigners; ++i) {
				const auto timestamp = WTHelperGetProvSignerFromChain(provider, 0, TRUE, i);
				expectedPublisher = IsMachineTrustedSigner(timestamp, szOID_PKIX_KP_TIMESTAMP_SIGNING,
					CERT_CHAIN_POLICY_AUTHENTICODE_TS);
			}
		}
	}
	trust.dwStateAction = WTD_STATEACTION_CLOSE;
	WinVerifyTrust(nullptr, &action, &trust);
	return expectedPublisher;
}

std::filesystem::path LocalToolPath(HANDLE file) {
	if (GetFileType(file) != FILE_TYPE_DISK) return {};
	std::wstring resolved(32768, L'\0');
	const DWORD length = GetFinalPathNameByHandleW(file, resolved.data(),
		static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	if (length < 7 || length >= resolved.size() || !resolved.starts_with(L"\\\\?\\")) return {};
	resolved.resize(length);
	std::filesystem::path path = resolved.substr(4);
	if (!IsLocalExecutablePath(path)) return {};
	const UINT drive = GetDriveTypeW(path.root_path().c_str());
	return drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE ? path : std::filesystem::path{};
}

VerifiedToolFile VerifyToolFile(const std::filesystem::path& path, const wchar_t* expectedName) {
	if (!IsLocalExecutablePath(path) || _wcsicmp(path.filename().c_str(), expectedName) != 0) return {};
	const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (raw == INVALID_HANDLE_VALUE) return {};
	VerifiedToolFile result{ LocalToolPath(raw), {raw, CloseHandle} };
	if (result.path.empty() || _wcsicmp(result.path.filename().c_str(), expectedName) != 0 ||
		!IsMsiSignedToolFile(result.path, raw)) return {};
	return result; // Deny replacement while the caller loads or launches the verified file.
}

bool IsRunning(const std::filesystem::path& executable) {
	if (executable.empty()) return false;
	wil::unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (!snapshot || snapshot.get() == INVALID_HANDLE_VALUE) return false;
	PROCESSENTRY32W entry{ .dwSize = sizeof(entry) };
	if (!Process32FirstW(snapshot.get(), &entry)) return false;
	do {
		if (_wcsicmp(entry.szExeFile, executable.filename().c_str()) != 0) continue;
		wil::unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
		std::wstring path(32768, L'\0');
		DWORD size = DWORD(path.size());
		if (process && QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) {
			path.resize(size);
			if (_wcsicmp(path.c_str(), executable.c_str()) == 0) return true;
		}
	} while (Process32NextW(snapshot.get(), &entry));
	return false;
}

bool IsSystemOrAdministratorSid(PSID sid) {
	if (!sid) return false;
	if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
	struct InstallerSid {
		alignas(DWORD) BYTE bytes[SECURITY_MAX_SID_SIZE]{};
		bool valid = false;
	};
	static const InstallerSid installer = [] {
		InstallerSid result;
		DWORD sidSize = sizeof(result.bytes);
		wchar_t domain[64]{};
		DWORD domainSize = ARRAYSIZE(domain);
		SID_NAME_USE use{};
		result.valid = LookupAccountNameW(nullptr, L"NT SERVICE\\TrustedInstaller",
			result.bytes, &sidSize, domain, &domainSize, &use) != FALSE;
		return result;
	}();
	return installer.valid && EqualSid(sid, const_cast<BYTE*>(installer.bytes));
}

bool IsAdministratorControlledPath(const std::filesystem::path& path, ACCESS_MASK unsafeRights) {
	wil::unique_hfile file(CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
	BY_HANDLE_FILE_INFORMATION info{};
	if (!file || !GetFileInformationByHandle(file.get(), &info) ||
		(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
	PSID owner = nullptr;
	PACL acl = nullptr;
	PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
	const DWORD status = GetSecurityInfo(file.get(), SE_FILE_OBJECT,
		OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, &owner, nullptr, &acl, nullptr, &rawDescriptor);
	const std::unique_ptr<void, decltype(&LocalFree)> descriptor(rawDescriptor, LocalFree);
	if (status != ERROR_SUCCESS || !acl || !IsSystemOrAdministratorSid(owner)) return false;
	GENERIC_MAPPING rights{ FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
	for (DWORD i = 0; i < acl->AceCount; ++i) {
		void* entry = nullptr;
		if (!GetAce(acl, i, &entry)) return false;
		const auto header = static_cast<const ACE_HEADER*>(entry);
		if (header->AceFlags & INHERIT_ONLY_ACE) continue;
		if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
		if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) return false;
		const auto allowed = static_cast<ACCESS_ALLOWED_ACE*>(entry);
		ACCESS_MASK access = allowed->Mask;
		MapGenericMask(&access, &rights);
		if ((access & unsafeRights) && !IsSystemOrAdministratorSid(&allowed->SidStart)) return false;
	}
	return true;
}

bool IsProtectedToolLaunchDirectory(const std::filesystem::path& directory) {
	constexpr ACCESS_MASK replace = DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
	constexpr ACCESS_MASK write = replace | FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
	if (!IsAdministratorControlledPath(directory, write)) return false;
	for (auto ancestor = directory.parent_path(); !ancestor.empty(); ancestor = ancestor.parent_path()) {
		if (!IsAdministratorControlledPath(ancestor, replace)) return false;
		if (ancestor == ancestor.root_path()) break;
	}
	// The vendor executables load private unsigned DLLs beside them at startup.
	std::error_code enumerationError;
	for (std::filesystem::directory_iterator files(directory, enumerationError), end;
		!enumerationError && files != end; files.increment(enumerationError)) {
		const auto extension = files->path().extension();
		if ((_wcsicmp(extension.c_str(), L".dll") == 0 || _wcsicmp(extension.c_str(), L".exe") == 0) &&
			!IsAdministratorControlledPath(files->path(), write)) return false;
	}
	return !enumerationError;
}

std::wstring ReadToolVersion(const std::filesystem::path& path) {
	DWORD ignored = 0;
	const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
	if (!size || size > 1024 * 1024) return {};
	std::vector<uint8_t> bytes(size);
	if (!GetFileVersionInfoW(path.c_str(), 0, size, bytes.data())) return {};
	VS_FIXEDFILEINFO* info = nullptr;
	UINT infoSize = 0;
	if (!VerQueryValueW(bytes.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
		infoSize < sizeof(VS_FIXEDFILEINFO)) return {};
	return fmt::format(L"{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
		HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

std::filesystem::path RegistryInstall(bool afterburner) {
	// Search both registry views, including per-user installations.
	const auto key = afterburner ? L"SOFTWARE\\Unwinder\\MSI Afterburner" : L"SOFTWARE\\Unwinder\\RTSS";
	const auto executable = afterburner ? L"MSIAfterburner.exe" : L"RTSS.exe";
	for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
		for (DWORD view : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
			wil::unique_hkey handle;
			if (RegOpenKeyExW(root, key, 0, KEY_QUERY_VALUE | view, handle.put()) != ERROR_SUCCESS) continue;
			for (const auto name : { L"InstallPath", L"InstallDir" }) {
				wchar_t value[32768]{};
				DWORD size = sizeof(value), type = 0;
				if (RegQueryValueExW(handle.get(), name, nullptr, &type, reinterpret_cast<BYTE*>(value), &size) != ERROR_SUCCESS ||
					(type != REG_SZ && type != REG_EXPAND_SZ) || size >= sizeof(value)) continue;
				std::wstring resolved(value);
				if (type == REG_EXPAND_SZ) {
					std::wstring expanded(32768, L'\0');
					const DWORD count = ExpandEnvironmentStringsW(value, expanded.data(), DWORD(expanded.size()));
					if (!count || count > expanded.size()) continue;
					expanded.resize(count - 1); resolved = std::move(expanded);
				}
				std::filesystem::path path(resolved);
				if (_wcsicmp(path.extension().c_str(), L".exe") != 0) path /= executable;
				if (IsLocalExecutablePath(path) && _wcsicmp(path.filename().c_str(), executable) == 0 &&
					Win32Helper::FileExists(path.c_str())) return path;
			}
		}
	}
	return {};
}

}

std::string RtssProfileAccess::ProfileName(const std::wstring& name) {
	if (name.empty() || name.find_first_of(L"\\/:,\r\n*?\t\"<>|") != std::wstring::npos ||
		_wcsicmp(std::filesystem::path(name).extension().c_str(), L".exe") != 0) return {};
	BOOL lossy = FALSE;
	const bool utf8 = GetACP() == CP_UTF8;
	const DWORD flags = utf8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
	BOOL* const loss = utf8 ? nullptr : &lossy;
	const int size = WideCharToMultiByte(CP_ACP, flags, name.c_str(), int(name.size()), nullptr, 0, nullptr, loss);
	if (!size || lossy) return {};
	std::string result(size_t(size), '\0');
	if (!WideCharToMultiByte(CP_ACP, flags, name.c_str(), int(name.size()), result.data(), size, nullptr, loss) || lossy) return {};
	return result;
}

RtssToolInfo RtssProfileAccess::DetectTool(bool afterburner) {
	RtssToolInfo result;
	result.executable = RegistryInstall(afterburner);
	if (result.executable.empty()) {
		for (const auto folder : { FOLDERID_ProgramFilesX86, FOLDERID_ProgramFiles }) {
			wil::unique_cotaskmem_string path;
			if (FAILED(SHGetKnownFolderPath(folder, 0, nullptr, path.put()))) continue;
			const auto candidate = std::filesystem::path(path.get()) /
				(afterburner ? L"MSI Afterburner\\MSIAfterburner.exe" : L"RivaTuner Statistics Server\\RTSS.exe");
			if (Win32Helper::FileExists(candidate.c_str())) { result.executable = candidate; break; }
		}
	}
	if (!result.executable.empty()) {
		const wil::unique_hfile file(CreateFileW(result.executable.c_str(), FILE_READ_ATTRIBUTES,
			FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file) result.executable = LocalToolPath(file.get());
	}
	if (!result.executable.empty()) {
		result.version = ReadToolVersion(result.executable);
		result.running = IsRunning(result.executable);
	}
	return result;
}

bool RtssProfileAccess::LaunchTool(const RtssToolInfo& tool, std::wstring& error) {
	if (tool.executable.empty()) { error = L"ToolNotInstalled"; return false; }
	const auto name = _wcsicmp(tool.executable.filename().c_str(), L"MSIAfterburner.exe") == 0
		? L"MSIAfterburner.exe" : L"RTSS.exe";
	const auto verified = VerifyToolFile(tool.executable, name);
	if (!verified.file) { error = L"UntrustedInstallation"; return false; }
	if (IsRunning(verified.path)) return true;
	const auto folder = verified.path.parent_path();
	if (!IsProtectedToolLaunchDirectory(folder)) { error = L"UntrustedLaunchDirectory"; return false; }
	SHELLEXECUTEINFOW info{ .cbSize = sizeof(info), .fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC,
		.lpVerb = L"open", .lpFile = verified.path.c_str(), .lpDirectory = folder.c_str(), .nShow = SW_SHOWMINNOACTIVE };
	if (!ShellExecuteExW(&info)) { error = std::to_wstring(GetLastError()); return false; }
	if (info.hProcess) CloseHandle(info.hProcess);
	return true;
}

RtssProfileAccess::~RtssProfileAccess() { _Unload(); }

void RtssProfileAccess::_Unload() {
	// Quarantine a faulted module until process exit rather than invoke its DllMain again.
	if (_module && !_faulted) FreeLibrary(_module);
	_module = nullptr;
	_load = nullptr; _save = nullptr; _delete = nullptr;
	_get = nullptr; _set = nullptr; _update = nullptr;
	_directory.clear();
}

bool RtssProfileAccess::Initialize(const RtssToolInfo& tool, std::wstring& error) {
	_version = tool.version;
	_running = tool.running;
	_selfProfile = ProfileName(Win32Helper::GetExePath().filename().native());
	_ResetFailure("");
	if (_faulted) { error = L"InterfaceFault"; return false; }
	if (tool.executable.empty()) { error = L"NotInstalled"; return false; }
	if (_module && _directory == tool.executable.parent_path()) return true;
	_Unload();
#if defined(_M_ARM64)
	error = L"UnsupportedArchitecture";
	return false;
#else
	const auto executable = VerifyToolFile(tool.executable, L"RTSS.exe");
	if (!executable.file) { error = L"UntrustedInstallation"; return false; }
	const auto directory = executable.path.parent_path();
#if defined(_WIN64)
	const auto dllName = L"RTSSHooks64.dll";
#else
	const auto dllName = L"RTSSHooks.dll";
#endif
	const auto dll = directory / dllName;
	if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
		const DWORD code = GetLastError();
		error = code == ERROR_ACCESS_DENIED ? L"AccessDenied" :
			(code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? L"LibraryMissing" : L"LoadFailed: " + std::to_wstring(code);
		_Unload(); return false;
	}
	const auto verified = VerifyToolFile(dll, dllName);
	if (!verified.file || _wcsicmp(verified.path.parent_path().c_str(), directory.c_str()) != 0) {
		error = L"UntrustedInstallation";
		return false;
	}
	_module = LoadLibraryExW(verified.path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!_module) {
		const DWORD code = GetLastError();
		error = code == ERROR_BAD_EXE_FORMAT ? L"UnsupportedArchitecture" : code == ERROR_ACCESS_DENIED ?
			L"AccessDenied" : L"LoadFailed: " + std::to_wstring(code);
		_Unload(); return false;
	}
	_directory = directory;
	_load = reinterpret_cast<ProfileFn>(GetProcAddress(_module, "LoadProfile"));
	_save = reinterpret_cast<ProfileFn>(GetProcAddress(_module, "SaveProfile"));
	_delete = reinterpret_cast<ProfileFn>(GetProcAddress(_module, "DeleteProfile"));
	_get = reinterpret_cast<PropertyFn>(GetProcAddress(_module, "GetProfileProperty"));
	_set = reinterpret_cast<PropertyFn>(GetProcAddress(_module, "SetProfileProperty"));
	_update = reinterpret_cast<void (*)()>(GetProcAddress(_module, "UpdateProfiles"));
	// Loading a module is not a promise that every operation is available.
	// Each transaction checks only its own exports and properties.
	return true;
#endif
}

bool RtssProfileAccess::_RequireInterface(bool available, RtssFailureStage stage, const char* name, std::wstring& error) {
	if (available && !_faulted) return true;
	_failure.property = name;
	return _Fail(stage, L"MissingInterface", error);
}

bool RtssProfileAccess::_Get(const char* property, uint32_t& value) {
	const bool result = _get && !_faulted && GuardProperty(_get, property, reinterpret_cast<BYTE*>(&value), sizeof(value), _faulted) != FALSE;
	_failure.nativeFault = _faulted;
	if (!result) { _failure.property = property; _failure.observedKnown = false; }
	return result;
}

bool RtssProfileAccess::_Set(const char* property, uint32_t value) {
	return _set && !_faulted && GuardProperty(_set, property, reinterpret_cast<BYTE*>(&value), sizeof(value), _faulted) != FALSE;
}

void RtssProfileAccess::_ResetFailure(const std::string& profile) {
	_failure = {};
	_failure.profile = profile;
	_failure.version = _version;
	_failure.nativeFault = _faulted;
}

bool RtssProfileAccess::_Fail(RtssFailureStage stage, const wchar_t* reason, std::wstring& error) {
	_failure.stage = stage;
	_failure.nativeFault = _faulted;
	error = _faulted ? L"InterfaceFault" : reason;
	return false;
}

bool RtssProfileAccess::_SetIfChanged(const char* property, uint32_t value, std::wstring& error, const uint32_t* knownValue) {
	_failure.property = property;
	_failure.requested = value;
	_failure.observedKnown = _Get(property, _failure.observed);
	if (!_failure.observedKnown) {
		if (_faulted) return _Fail(RtssFailureStage::Setting, L"ReadPropertyFailed", error);
		// A known snapshot can avoid an unnecessary setter. Otherwise the
		// authorized setter must accept the value, and Save must prove it on disk.
		if (knownValue) { _failure.observed = *knownValue; _failure.observedKnown = true; }
	}
	// Get can return values the setter rejects, including FrametimeLimit=0 in 7.3.5.
	if (!_failure.observedKnown || _failure.observed != value) {
		if (!_Set(property, value)) return _Fail(RtssFailureStage::Setting, L"SetPropertyFailed", error);
		_failure.observedKnown = _Get(property, _failure.observed);
		if (_faulted || (_failure.observedKnown && _failure.observed != value)) {
			return _Fail(RtssFailureStage::Setting, L"SetReadbackFailed", error);
		}
		// Getter absence is not setter rejection. Every caller verifies the
		// saved fields; an omitted/unreadable result still fails that verification.
	}
	_failure.property.clear();
	_failure.observedKnown = false;
	_failure.requested = _failure.observed = 0;
	return true;
}

bool RtssProfileAccess::_VerifyProperty(const char* property, uint32_t expected, uint32_t actual, std::wstring& error) {
	if (expected == actual) return true;
	_failure.property = property;
	_failure.requested = expected;
	_failure.observed = actual;
	_failure.observedKnown = true;
	return _Fail(RtssFailureStage::Verify, L"VerifyFailed", error);
}

bool RtssProfileAccess::_CallProfile(ProfileFn function, const char* profile) {
	const bool result = function && !_faulted && GuardProfile(function, profile, _faulted);
	_failure.nativeFault = _faulted;
	return result;
}

bool RtssProfileAccess::_Reload() {
	return !_faulted && (!_running || (_update && GuardReload(_update, _faulted)));
}

std::filesystem::path RtssProfileAccess::_ProfilePath(const std::string& profile) const {
	// RTSS profile names are ANSI. The caller rejects lossy process-name conversion.
	const int count = MultiByteToWideChar(CP_ACP, 0, profile.c_str(), -1, nullptr, 0);
	if (!count) return {};
	std::wstring name(size_t(count), L'\0');
	MultiByteToWideChar(CP_ACP, 0, profile.c_str(), -1, name.data(), count);
	name.resize(size_t(count) - 1);
	return _directory / L"Profiles" / (profile.empty() ? L"Global" : name + L".cfg");
}

bool RtssProfileAccess::_Exists(const std::string& profile, bool& exists, std::wstring& error) {
	// Do not call EnumProfiles: a read-only probe of this installation's x64
	// export raised an access violation. RTSS stores both presets and user
	// profiles in Profiles/<exe>.cfg; existence is distinct from Load fallback.
	const auto path = _ProfilePath(profile);
	if (path.empty()) { error = L"InvalidProfileName"; return false; }
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES) {
		if (attributes & FILE_ATTRIBUTE_DIRECTORY) { error = L"InvalidProfileFile"; return false; }
		exists = true; return true;
	}
	const DWORD code = GetLastError();
	if (code == ERROR_FILE_NOT_FOUND) { exists = false; return true; }
	_failure.fileError = code; error = L"ReadFailed: " + std::to_wstring(code);
	return false;
}

bool RtssProfileAccess::_ReadFile(const std::string& profile, RtssProfileSnapshot& snapshot, std::wstring& error) {
	const auto path = _ProfilePath(profile);
	if (path.empty()) { error = L"InvalidProfileName"; return false; }
	std::string bytes;
	if (!ReadDiskFile(path, bytes, snapshot.fileIdentity, _failure.fileError)) {
		error = L"ReadFailed: " + std::to_wstring(_failure.fileError); return false;
	}
	if (bytes.find('\0') != bytes.npos || bytes.starts_with("\xFF\xFE") || bytes.starts_with("\xFE\xFF")) {
		error = L"UnsupportedProfileEncoding"; return false;
	}
	snapshot.contents = bytes;
	std::stringstream lines(bytes.starts_with("\xEF\xBB\xBF") ? bytes.substr(3) : bytes);
	std::string line, section;
	snapshot.fields.clear();
	while (std::getline(lines, line)) {
		line = Trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;
		if (line.front() == '[' && line.back() == ']') { section = Lower(Trim(line.substr(1, line.size() - 2))); continue; }
		const auto split = line.find('=');
		if (split == std::string::npos) continue;
		const auto key = section + '/' + Lower(Trim(line.substr(0, split)));
		if (!snapshot.fields.emplace(key, Trim(line.substr(split + 1))).second) {
			error = L"DuplicateProfileField"; return false;
		}
	}
	snapshot.rate = { 0, 1, 0, 1 };
	snapshot.rateKnown = {};
	for (size_t i = 0; i < RATE_KEYS.size(); ++i) {
		const auto it = snapshot.fields.find(RATE_KEYS[i]);
		if (it == snapshot.fields.end()) continue;
		uint32_t value = 0;
		const auto& text = it->second;
		const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), value);
		if (result != std::errc{} || end != text.data() + text.size() || ((i == 1 || i == 3) && !value)) {
			error = L"InvalidRateExpression"; return false;
		}
		snapshot.rate[i] = value;
		snapshot.rateKnown[i] = true;
	}
	if (const auto it = snapshot.fields.find("hooking/detection"); it != snapshot.fields.end()) {
		const auto& text = it->second;
		const auto [end, result] = std::from_chars(text.data(), text.data() + text.size(), snapshot.detectionLevel);
		snapshot.detectionKnown = result == std::errc{} && end == text.data() + text.size();
	}
	return true;
}

bool RtssProfileAccess::Read(const std::string& profile, RtssProfileSnapshot& snapshot, std::wstring& error) {
	_ResetFailure(profile);
	_failure.stage = RtssFailureStage::Read;
	const bool success = _Read(profile, snapshot, error);
	if (success) _failure.stage = RtssFailureStage::None;
	return success;
}

bool RtssProfileAccess::_Read(const std::string& profile, RtssProfileSnapshot& snapshot, std::wstring& error) {
	snapshot = {};
	if (!_module) { error = L"NotInitialized"; return false; }
	if (!_RequireInterface(_load != nullptr, RtssFailureStage::Read, "LoadProfile", error) ||
		!_RequireInterface(_get != nullptr, RtssFailureStage::Read, "GetProfileProperty", error)) return false;
	if (!_Exists(profile, snapshot.exists, error)) return false;
	if (profile.empty() && !snapshot.exists) { error = L"GlobalProfileMissing"; return false; }
	if (!_CallProfile(_load, snapshot.exists ? profile.c_str() : "")) { error = L"InterfaceFault"; return false; }
	if (!_ReadFile(snapshot.exists ? profile : "", snapshot, error)) return false;
	constexpr std::array<const char*, 3> properties{ "FramerateLimit", "FramerateLimitDenominator", "FrametimeLimit" };
	for (size_t i = 0; i < properties.size(); ++i) {
		uint32_t value = 0;
		if (!_Get(properties[i], value)) {
			if (_faulted) return _Fail(RtssFailureStage::Read, L"InterfaceFault", error);
			// An explicit disk field remains readable; an omitted field is unknown.
			// Required getters are checked again by the setter transaction.
			continue;
		}
		if (i == 1 && !value) return _Fail(RtssFailureStage::Read, L"InvalidRateExpression", error);
		if (snapshot.rateKnown[i] && snapshot.rate[i] != value) return _Fail(RtssFailureStage::Read, L"ProfileChangedWhileReading", error);
		snapshot.rate[i] = value; snapshot.rateKnown[i] = true;
	}
	uint32_t detection = 0;
	if (_Get("AppDetectionLevel", detection)) { snapshot.detectionLevel = detection; snapshot.detectionKnown = true; }
	else if (_faulted) return _Fail(RtssFailureStage::Read, L"InterfaceFault", error);
	_failure.property.clear(); _failure.observedKnown = false;
	return true;
}

bool RtssProfileAccess::_CheckWriteAccess(const std::string& profile, bool exists, std::wstring& error) {
	// Open only existing objects: this checks the caller's token without changing
	// a profile or creating a test file in the user's RTSS directory.
	const auto path = exists ? _ProfilePath(profile) : _directory / L"Profiles";
	const HANDLE handle = CreateFileW(path.c_str(), exists ? GENERIC_WRITE : FILE_ADD_FILE,
		FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
		exists ? FILE_ATTRIBUTE_NORMAL : FILE_FLAG_BACKUP_SEMANTICS, nullptr);
	if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); return true; }
	_failure.fileError = GetLastError();
	return _Fail(RtssFailureStage::Save, L"WriteAccessFailed", error);
}

bool RtssProfileAccess::ReloadProfiles(std::wstring& error, const std::function<bool()>& stillValid) {
	_ResetFailure({});
	if (!stillValid()) return _Fail(RtssFailureStage::Cancelled, L"Cancelled", error);
	if (!_RequireInterface(_module && !_faulted && _running && _update, RtssFailureStage::Reload, "UpdateProfiles", error)) return false;
	if (!_Reload()) return _Fail(RtssFailureStage::Reload, L"ReloadFailed", error);
	return true;
}

bool RtssProfileAccess::InitializeUnlimited(const std::string& profile, RtssProfileSnapshot& written,
	bool& alreadyUnlimited, std::wstring& error, const std::function<bool()>& stillValid) {
	_ResetFailure(profile);
	written = {}; alreadyUnlimited = false;
	_failure.stage = RtssFailureStage::Read;
	// This explicit command cannot address Global, paths or arbitrary executables.
	if (_selfProfile.empty() || _stricmp(profile.c_str(), _selfProfile.c_str()) != 0) return _Fail(RtssFailureStage::Setting, L"InvalidSelfProfile", error);
	RtssProfileSnapshot before;
	if (!_Read(profile, before, error)) return false;
	if (!stillValid()) return _Fail(RtssFailureStage::Cancelled, L"Cancelled", error);
	if (before.exists && before.rateKnown[0] && before.rateKnown[2] && !before.rate[0] && !before.rate[2]) {
		written = before; alreadyUnlimited = true; _failure.stage = RtssFailureStage::None; return true;
	}
	if (!_RequireInterface(!_running || _update != nullptr, RtssFailureStage::Reload, "UpdateProfiles", error)) return false;
	if (!_CheckWriteAccess(profile, before.exists, error)) return false;
	std::string contents;
	if (!ClearRtssRateExpression(before.contents, contents)) return _Fail(RtssFailureStage::Setting, L"UnsupportedProfileFormat", error);
	return _WriteUnlimited(profile, before, contents, written, error, stillValid);
}

bool RtssProfileAccess::_WriteUnlimited(const std::string& profile, const RtssProfileSnapshot& before,
	const std::string& contents, RtssProfileSnapshot& written, std::wstring& error,
	const std::function<bool()>& stillValid) {
	const auto path = _ProfilePath(profile);
	wchar_t tempName[MAX_PATH]{}, backupName[MAX_PATH]{};
	const auto directory = path.parent_path();
	if (!GetTempFileNameW(directory.c_str(), L"mpi", 0, tempName)) {
		_failure.fileError = GetLastError(); return _Fail(RtssFailureStage::Save, L"CreateCandidateFailed", error);
	}
	struct TemporaryFiles {
		const wchar_t* candidate;
		const wchar_t* backup;
		bool retainBackup = false;
		~TemporaryFiles() { DeleteFileW(candidate); if (!retainBackup && *backup) DeleteFileW(backup); }
	} temporary{ tempName, backupName };
	{
		const HANDLE raw = CreateFileW(tempName, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (raw == INVALID_HANDLE_VALUE) { _failure.fileError = GetLastError(); return _Fail(RtssFailureStage::Save, L"CreateCandidateFailed", error); }
		const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, CloseHandle);
		DWORD count = 0;
		if (!WriteFile(raw, contents.data(), DWORD(contents.size()), &count, nullptr) || !FlushFileBuffers(raw)) {
			_failure.fileError = GetLastError(); return _Fail(RtssFailureStage::Save, L"WriteCandidateFailed", error);
		}
		if (count != contents.size()) { _failure.fileError = ERROR_WRITE_FAULT; return _Fail(RtssFailureStage::Save, L"WriteCandidateFailed", error); }
	}
	if (before.exists) {
		if (!GetTempFileNameW(directory.c_str(), L"mpb", 0, backupName) || !DeleteFileW(backupName)) {
			_failure.fileError = GetLastError(); return _Fail(RtssFailureStage::Save, L"CreateBackupFailed", error);
		}
	}
	// Recheck both the effective input and its physical identity immediately before
	// committing. ReplaceFile still has a race window; its actual backup is checked below.
	std::unique_ptr<void, decltype(&CloseHandle)> sourceLock(nullptr, CloseHandle);
	if (!before.exists) {
		// There is no replaced target to compare with for a new profile. Keep its
		// inherited Global source stable through creation instead.
		const HANDLE raw = CreateFileW(_ProfilePath("").c_str(), GENERIC_READ, FILE_SHARE_READ,
			nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (raw == INVALID_HANDLE_VALUE) { _failure.fileError = GetLastError(); return _Fail(RtssFailureStage::Save, L"LockSourceFailed", error); }
		sourceLock.reset(raw);
	}
	RtssProfileSnapshot confirmed;
	if (!_Read(profile, confirmed, error)) return false;
	if (!SameInput(confirmed, before)) return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
	if (!stillValid()) return _Fail(RtssFailureStage::Cancelled, L"Cancelled", error);
	_failure.saveAttempted = true;
	const bool committed = before.exists ? ReplaceFileW(path.c_str(), tempName, backupName, 0, nullptr, nullptr) != FALSE :
		MoveFileExW(tempName, path.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE; // No replace of a concurrently created profile.
	if (!committed) {
		_failure.fileError = GetLastError();
		temporary.retainBackup = true;
		_failure.recoveryFile = backupName;
		// ReplaceFile may move the old file to backup even on failure. Recover only
		// a missing destination, using a non-replacing rename.
		if (before.exists && GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES && GetLastError() == ERROR_FILE_NOT_FOUND &&
			MoveFileExW(backupName, path.c_str(), MOVEFILE_WRITE_THROUGH)) {
			temporary.retainBackup = false; _failure.recoveryFile.clear();
		}
		_failure.readbackComplete = _Read(profile, written, error);
		return _Fail(RtssFailureStage::Save, L"ReplaceProfileFailed", error);
	}
	if (before.exists) {
		std::string replaced;
		std::array<uint64_t, 2> identity{};
		if (!ReadDiskFile(backupName, replaced, identity, _failure.fileError) || replaced != before.contents || identity != before.fileIdentity) {
			temporary.retainBackup = true;
			_failure.recoveryFile = backupName;
			// Lock out writers and renames before testing and rolling back our bytes.
			// An external change after the commit is left intact, with the backup retained.
			const HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (raw != INVALID_HANDLE_VALUE) {
				const std::unique_ptr<void, decltype(&CloseHandle)> file(raw, CloseHandle);
				LARGE_INTEGER size{};
				DWORD count = 0;
				std::string actual(contents.size(), '\0');
				if (!replaced.empty() && GetFileSizeEx(raw, &size) && size.QuadPart == LONGLONG(contents.size()) &&
					ReadFile(raw, actual.data(), DWORD(actual.size()), &count, nullptr) && count == actual.size() && actual == contents) {
					LARGE_INTEGER start{};
					if (SetFilePointerEx(raw, start, nullptr, FILE_BEGIN) &&
						WriteFile(raw, replaced.data(), DWORD(replaced.size()), &count, nullptr) && count == replaced.size() &&
						SetEndOfFile(raw) && FlushFileBuffers(raw)) temporary.retainBackup = false;
				}
			}
			if (!temporary.retainBackup) _failure.recoveryFile.clear();
			_failure.readbackComplete = _Read(profile, written, error);
			return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
		}
	}
	_failure.stage = RtssFailureStage::Verify;
	if (!_Read(profile, written, error)) return false;
	_failure.readbackComplete = true;
	if (!written.exists || written.contents != contents || written.rate[0] || written.rate[2] ||
		(before.rateKnown[1] && written.rateKnown[1] && written.rate[1] != before.rate[1]) ||
		(before.rateKnown[3] && written.rateKnown[3] && written.rate[3] != before.rate[3]) ||
		(before.detectionKnown && written.detectionKnown && written.detectionLevel != before.detectionLevel) || !NonRateEqual(before, written)) {
		return _Fail(RtssFailureStage::Verify, L"VerifyFailed", error);
	}
	if (!_Reload()) return _Fail(RtssFailureStage::Reload, L"ReloadFailed", error);
	_failure.stage = RtssFailureStage::None;
	return true;
}

bool RtssProfileAccess::Apply(const std::string& profile, uint32_t rate, const RtssProfileSnapshot& before,
	RtssProfileSnapshot& written, bool& saveAttempted, bool& readbackComplete,
	std::wstring& error, const std::function<bool()>& stillValid) {
	_ResetFailure(profile);
	written = {};
	saveAttempted = false; readbackComplete = false;
	_failure.stage = RtssFailureStage::Read;
	RtssProfileSnapshot current;
	if (!_Read(profile, current, error)) return false;
	if (!SameInput(current, before)) return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
	if (!rate || rate > 1000) return _Fail(RtssFailureStage::Setting, L"InvalidRate", error);
	if (!_RequireInterface(_set != nullptr, RtssFailureStage::Setting, "SetProfileProperty", error) ||
		!_RequireInterface(_save != nullptr, RtssFailureStage::Save, "SaveProfile", error) ||
		!_RequireInterface(_update != nullptr, RtssFailureStage::Reload, "UpdateProfiles", error)) return false;
	if (!_running) return _Fail(RtssFailureStage::Setting, L"NotRunning", error);
	if (!_CheckWriteAccess(profile, before.exists, error)) return false;
	if (!_SetIfChanged("FrametimeLimit", 0, error, current.rateKnown[2] ? &current.rate[2] : nullptr)) {
		// A positive FPS property does not prove that RTSS has left time mode.
		if (error == L"SetPropertyFailed" && current.rate[2]) error = L"TimeModeUnsupported";
		return false;
	}
	if (!_SetIfChanged("FramerateLimitDenominator", 1, error, current.rateKnown[1] ? &current.rate[1] : nullptr) ||
		!_SetIfChanged("FramerateLimit", rate, error, current.rateKnown[0] ? &current.rate[0] : nullptr) ||
		(!before.exists && !_SetIfChanged("AppDetectionLevel", 1, error, current.detectionKnown ? &current.detectionLevel : nullptr))) return false;
	_failure.stage = RtssFailureStage::Read;
	RtssProfileSnapshot disk;
	if (!_ReadFile(before.exists ? profile : "", disk, error)) return false;
	if (disk.contents != before.contents) return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
	if (!before.exists) {
		bool exists = false;
		if (!_Exists(profile, exists, error)) return false;
		if (exists) return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
	}
	if (!stillValid()) return _Fail(RtssFailureStage::Cancelled, L"Cancelled", error);
	saveAttempted = _failure.saveAttempted = true;
	if (!_CallProfile(_save, profile.c_str())) return _Fail(RtssFailureStage::Save, L"SaveFailed", error);
	// SaveProfile is void. Capture the actual state even when verification fails.
	_failure.stage = RtssFailureStage::Verify;
	if (!_Read(profile, written, error)) { _failure.nativeFault = _faulted; return false; }
	readbackComplete = _failure.readbackComplete = true;
	if (!written.exists) return _Fail(RtssFailureStage::Save, L"SaveFailed", error);
	if (written == before && (before.rate[0] != rate || before.rate[1] != 1 || before.rate[2] != 0)) {
		return _Fail(RtssFailureStage::Save, L"SaveFailed", error);
	}
	if (!written.rateKnown[0] || !written.rateKnown[1] || !written.rateKnown[2])
		return _Fail(RtssFailureStage::Verify, L"ReadPropertyFailed", error);
	if (!_VerifyProperty("FramerateLimit", rate, written.rate[0], error) ||
		!_VerifyProperty("FramerateLimitDenominator", 1, written.rate[1], error) ||
		!_VerifyProperty("FrametimeLimit", 0, written.rate[2], error)) return false;
	if (!before.exists && !written.detectionKnown) return _Fail(RtssFailureStage::Verify, L"ReadPropertyFailed", error);
	if ((!before.exists || (before.detectionKnown && written.detectionKnown)) &&
		!_VerifyProperty("AppDetectionLevel", before.exists ? before.detectionLevel : 1, written.detectionLevel, error)) return false;
	if (before.exists && !NonRateEqual(before, written)) {
		return _Fail(RtssFailureStage::Verify, L"VerifyFailed", error);
	}
	if (!_Reload()) return _Fail(RtssFailureStage::Reload, L"ReloadFailed", error);
	_failure.stage = RtssFailureStage::None;
	return true;
}

bool RtssProfileAccess::Restore(const std::string& profile, const RtssProfileSnapshot& original,
	const RtssProfileSnapshot& lastWritten, bool created, bool& conflict, std::wstring& error) {
	_ResetFailure(profile);
	_failure.stage = RtssFailureStage::Restore;
	conflict = false;
	RtssProfileSnapshot current;
	if (!_Read(profile, current, error)) { _failure.nativeFault = _faulted; return false; }
	if (created && !current.exists) { _failure.stage = RtssFailureStage::None; return true; }
	if (!current.rateKnown[0] || !current.rateKnown[1] || !current.rateKnown[2])
		return _Fail(RtssFailureStage::Restore, L"ReadPropertyFailed", error);
	if (!SameRate(current, lastWritten) || (created && (current.contents != lastWritten.contents || !NonRateEqual(current, lastWritten) ||
		current.detectionLevel != lastWritten.detectionLevel))) {
		conflict = true; return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
	}
	if (!_RequireInterface(!_running || _update != nullptr, RtssFailureStage::Reload, "UpdateProfiles", error)) return false;
	if (created) {
		if (!_RequireInterface(_delete != nullptr, RtssFailureStage::Restore, "DeleteProfile", error)) return false;
		RtssProfileSnapshot confirmed;
		if (!_Read(profile, confirmed, error)) return false;
		if (!confirmed.exists || confirmed.contents != lastWritten.contents) {
			conflict = true; return _Fail(RtssFailureStage::Conflict, L"Conflict", error);
		}
		_failure.saveAttempted = true;
		if (!_CallProfile(_delete, profile.c_str())) return _Fail(RtssFailureStage::Restore, L"DeleteFailed", error);
		RtssProfileSnapshot after;
		_failure.stage = RtssFailureStage::Verify;
		if (!_Read(profile, after, error)) return false;
		_failure.readbackComplete = true;
		if (after.exists) return _Fail(RtssFailureStage::Restore, L"DeleteFailed", error);
		if (!_Reload()) return _Fail(RtssFailureStage::Reload, L"ReloadFailed", error);
		_failure.stage = RtssFailureStage::None;
		return true;
	}
	if (!_RequireInterface(_set != nullptr, RtssFailureStage::Setting, "SetProfileProperty", error) ||
		!_RequireInterface(_save != nullptr, RtssFailureStage::Restore, "SaveProfile", error)) return false;
	// Missing original information cannot be restored by guessing defaults.
	// This is a stop-time requirement, never a guarantee required before Apply.
	constexpr std::array<const char*, 4> properties{
		"FramerateLimit", "FramerateLimitDenominator", "FrametimeLimit", "LimitTimeDenominator" };
	for (size_t i = 0; i < properties.size(); ++i) {
		if (!original.rateKnown[i] && (i < 3 || original.rate[2])) {
			_failure.property = properties[i]; return _Fail(RtssFailureStage::Restore, L"ReadPropertyFailed", error);
		}
	}
	// Preserve current non-rate fields. No silent file fallback for unsupported expressions.
	if (original.rateKnown[3] && (!current.rateKnown[3] || current.rate[3] != original.rate[3])) return _Fail(RtssFailureStage::Restore, L"TimeDenominatorChanged", error);
	if (!_CheckWriteAccess(profile, true, error)) return false;
	if (!_SetIfChanged("FramerateLimitDenominator", original.rate[1], error, current.rateKnown[1] ? &current.rate[1] : nullptr) ||
		!_SetIfChanged("FrametimeLimit", original.rate[2], error, current.rateKnown[2] ? &current.rate[2] : nullptr) ||
		!_SetIfChanged("FramerateLimit", original.rate[0], error, current.rateKnown[0] ? &current.rate[0] : nullptr)) return false;
	_failure.stage = RtssFailureStage::Restore;
	RtssProfileSnapshot disk;
	if (!_ReadFile(profile, disk, error)) return false;
	if (disk.contents != current.contents) { conflict = true; return _Fail(RtssFailureStage::Conflict, L"Conflict", error); }
	_failure.saveAttempted = true;
	if (!_CallProfile(_save, profile.c_str())) return _Fail(RtssFailureStage::Save, L"RestoreSaveFailed", error);
	RtssProfileSnapshot after;
	_failure.stage = RtssFailureStage::Verify;
	if (!_Read(profile, after, error)) return false;
	_failure.readbackComplete = true;
	if (!after.exists || !NonRateEqual(current, after)) {
		return _Fail(RtssFailureStage::Verify, L"RestoreVerifyFailed", error);
	}
	for (size_t i = 0; i < properties.size(); ++i) {
		if (!original.rateKnown[i]) continue;
		if (!after.rateKnown[i]) return _Fail(RtssFailureStage::Verify, L"ReadPropertyFailed", error);
		if (!_VerifyProperty(properties[i], original.rate[i], after.rate[i], error)) return false;
	}
	if (current.detectionKnown && after.detectionKnown && !_VerifyProperty("AppDetectionLevel", current.detectionLevel, after.detectionLevel, error)) return false;
	for (const auto key : RATE_KEYS) {
		if (original.fields.contains(key) != after.fields.contains(key)) {
			return _Fail(RtssFailureStage::Verify, L"RestoreExpressionChanged", error);
		}
	}
	if (!_Reload()) return _Fail(RtssFailureStage::Reload, L"ReloadFailed", error);
	_failure.stage = RtssFailureStage::None;
	return true;
}

}
