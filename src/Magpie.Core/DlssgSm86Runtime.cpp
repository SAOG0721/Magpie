#include "pch.h"
#include "DlssgSm86Runtime.h"
#include "DlssgSm86Policy.h"
#include "DlssgSm86Package.h"
#include "Logger.h"
#include "Win32Helper.h"
#include "StrHelper.h"
#include <mutex>
#include <shlobj.h>
#include <psapi.h>

namespace Magpie::DlssgSm86Runtime {

#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
namespace {

std::mutex runtimeMutex;
enum class Stage { Unprepared, Preparing, ProxyMapped, BundleVerified, Failed };
Stage stage = Stage::Unprepared;
std::string failure;
HMODULE proxy = nullptr;
HMODULE runtime = nullptr;
HMODULE backend = nullptr;
std::filesystem::path featureDirectory;
std::filesystem::path packageDirectory;
std::vector<HANDLE> packageFiles; // Immutable tree retained until process exit.
LUID physicalAdapter{};
// Intentionally retained until process exit. Unloading the proxy is not safe.
HANDLE proxyFile = INVALID_HANDLE_VALUE;
HANDLE configFile = INVALID_HANDLE_VALUE;
HANDLE runtimeFile = INVALID_HANDLE_VALUE;
HANDLE backendFile = INVALID_HANDLE_VALUE;

bool Fail(std::string_view reason) {
	stage = Stage::Failed;
	failure = reason;
	Logger::Get().Error(fmt::format("DLSSFG preparation failed: {}; restart Magpie after correcting this condition. No native fallback", reason));
	return false;
}

std::filesystem::path ModulePath(HMODULE module) {
	std::wstring path(32768, L'\0');
	const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
	if (!length || length >= path.size()) return {};
	path.resize(length);
	return path;
}

bool SamePath(const std::filesystem::path& a, const std::filesystem::path& b) {
	// Cache constants contain forward slashes; Win32 module paths use backslashes.
	return !a.empty() && !b.empty() && _wcsicmp(a.lexically_normal().make_preferred().c_str(),
		b.lexically_normal().make_preferred().c_str()) == 0;
}

// Must only be called with runtimeMutex held. Both modules must come from the
// returned runtime's bundle, with exactly one FG runtime and one backend mapped.
bool VerifyPairImpl() {
	const auto actualRuntime = ModulePath(runtime);
	if (actualRuntime.empty() || !SamePath(actualRuntime, packageDirectory / DlssgSm86Package::Runtime.name))
		return Fail("redirected runtime mapping changed");
	std::array<HMODULE, 1024> modules{};
	DWORD needed = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(),
		static_cast<DWORD>(sizeof(modules)), &needed) || needed > sizeof(modules))
		return Fail("cannot enumerate the runtime/backend mappings");
	bool foundRuntime = false, foundBackend = false;
	for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
		const auto path = ModulePath(modules[i]);
		if (path.empty()) return Fail("cannot identify a loaded module");
		const auto name = path.filename().wstring();
		const bool isRuntime = _wcsnicmp(name.c_str(), L"nvngx_dlssg", 11) == 0;
		const bool isBackend = _wcsnicmp(name.c_str(), L"sm86_backend", 12) == 0;
		if (!isRuntime && !isBackend) continue;
		const auto& expected = isRuntime ? DlssgSm86Package::Runtime : DlssgSm86Package::Backend;
		if (!SamePath(path, packageDirectory / expected.name) ||
			(isRuntime && (modules[i] != runtime || foundRuntime)) ||
			(isBackend && (foundBackend || (backend && backend != modules[i]))))
			return Fail("unexpected or duplicate FG runtime/backend mapping");
		HANDLE& file = isRuntime ? runtimeFile : backendFile;
		if (file == INVALID_HANDLE_VALUE) file = PinnedFile::Open(path);
		if (!PinnedFile::Verify(file, expected.size, expected.sha256))
			return Fail("bundled runtime/backend cannot be locked or its checksum is incorrect");
		if (isRuntime) {
			foundRuntime = true;
		} else {
			// Pin the backend without calling any undocumented upstream ABI.
			if (!backend && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(modules[i]), &backend))
				return Fail("cannot retain the bundled backend mapping");
			foundBackend = true;
		}
	}
	if (!foundRuntime || !foundBackend) return Fail("bundled runtime/backend pair is not loaded");
	return true;
}

bool QueryArchitecture(const LUID& wanted, int& major, int& minor, int& driver) {
	// Load only the installed driver. No CUDA context, SDK runtime or toolkit
	// deployment is required for these stable CUDA Driver API entry points.
	wil::unique_hmodule cuda(LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
	if (!cuda) return false;
	using Init = int(WINAPI*)(unsigned int);
	using GetCount = int(WINAPI*)(int*);
	using GetDevice = int(WINAPI*)(int*, int);
	using GetLuid = int(WINAPI*)(char*, unsigned int*, int);
	using GetAttribute = int(WINAPI*)(int*, int, int);
	auto init = reinterpret_cast<Init>(GetProcAddress(cuda.get(), "cuInit"));
	auto count = reinterpret_cast<GetCount>(GetProcAddress(cuda.get(), "cuDeviceGetCount"));
	auto device = reinterpret_cast<GetDevice>(GetProcAddress(cuda.get(), "cuDeviceGet"));
	auto luid = reinterpret_cast<GetLuid>(GetProcAddress(cuda.get(), "cuDeviceGetLuid"));
	auto attribute = reinterpret_cast<GetAttribute>(GetProcAddress(cuda.get(), "cuDeviceGetAttribute"));
	auto driverVersion = reinterpret_cast<GetCount>(GetProcAddress(cuda.get(), "cuDriverGetVersion"));
	if (!init || !count || !device || !luid || !attribute || !driverVersion || init(0) != 0) return false;
	int n = 0;
	if (count(&n) != 0) return false;
	for (int ordinal = 0; ordinal < n; ++ordinal) {
		int dev = 0;
		LUID candidate{};
		unsigned int mask = 0;
		if (device(&dev, ordinal) != 0 ||
			luid(reinterpret_cast<char*>(&candidate), &mask, dev) != 0 ||
			!mask || candidate.HighPart != wanted.HighPart || candidate.LowPart != wanted.LowPart) continue;
		// CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR/MINOR (75/76).
		return attribute(&major, 75, dev) == 0 && attribute(&minor, 76, dev) == 0 && driverVersion(&driver) == 0;
	}
	return false;
}

bool CacheFile(const std::filesystem::path& source, const std::filesystem::path& destination,
	const DlssgSm86Package::File& expected) {
	if (PinnedFile::Verify(destination, expected.size, expected.sha256)) return true;
	// Per-process temporary + atomic rename. Never overwrite an in-use proxy.
	auto temporary = destination;
	temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
	if (!CopyFileW(source.c_str(), temporary.c_str(), FALSE)) return false;
	const bool ok = PinnedFile::Verify(temporary, expected.size, expected.sha256) &&
		MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
	DeleteFileW(temporary.c_str());
	// Another instance can have installed the same immutable pair meanwhile.
	return ok || PinnedFile::Verify(destination, expected.size, expected.sha256);
}

bool PrepareImpl(IDXGIAdapter* adapter) {
	DXGI_ADAPTER_DESC desc{};
	if (!adapter || FAILED(adapter->GetDesc(&desc)) || desc.VendorId != 0x10de) {
		Logger::Get().Error("DLSSFG requires an identifiable NVIDIA rendering adapter");
		return false;
	}
	std::lock_guard lock(runtimeMutex);
	if (stage != Stage::Unprepared) {
		if (stage != Stage::BundleVerified) {
			Logger::Get().Error(fmt::format("DLSSFG preparation previously failed: {}; restart Magpie to retry", failure));
			return false;
		}
		if (desc.AdapterLuid.HighPart == physicalAdapter.HighPart &&
			desc.AdapterLuid.LowPart == physicalAdapter.LowPart) return true;
		Logger::Get().Error("DLSSFG rendering adapter changed after process-wide proxy load; restart Magpie before using DLSSFG");
		return false;
	}
	int major = 0, minor = 0, driver = 0;
	if (!QueryArchitecture(desc.AdapterLuid, major, minor, driver)) {
		Logger::Get().Error("DLSSFG cannot match rendering adapter LUID to a CUDA device; community FG unavailable");
		return false;
	}
	const auto route = DlssgSm86Policy::SelectRoute(major, minor);
	if (route == DlssgSm86Policy::Route::Unsupported) {
		Logger::Get().Warn(fmt::format("DLSSFG has no validated routing policy for physical SM {}.{}", major, minor));
		return false;
	}
	stage = Stage::Preparing;
	physicalAdapter = desc.AdapterLuid;
	std::array<HMODULE, 1024> loaded{};
	DWORD needed = 0;
	if (!K32EnumProcessModules(GetCurrentProcess(), loaded.data(), static_cast<DWORD>(sizeof(loaded)), &needed) ||
		needed > sizeof(loaded)) return Fail("cannot inspect existing modules before proxy load");
	for (size_t i = 0; i < needed / sizeof(HMODULE); ++i) {
		const auto path = ModulePath(loaded[i]);
		if (path.empty()) return Fail("cannot identify an existing module before proxy load");
		const auto name = path.filename().wstring();
		if (GetProcAddress(loaded[i], "DlssgProxy_Name") ||
			_wcsnicmp(name.c_str(), L"nvngx_dlssg", 11) == 0 ||
			_wcsnicmp(name.c_str(), L"sm86_backend", 12) == 0)
			return Fail("another FG proxy/runtime/backend was loaded before managed preparation");
	}
	const auto root = Win32Helper::GetExePath().parent_path();
	if (!DlssgSm86Package::Verify(root)) {
		return Fail("community package missing or checksum mismatch; reinstall the complete matching package");
	}
	wil::unique_cotaskmem_string local;
	const HRESULT localResult = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, local.put());
	if (FAILED(localResult)) {
		Logger::Get().ComError("DLSSFG cache directory unavailable", localResult);
		return Fail("local application cache directory unavailable");
	}
	const bool forward = route == DlssgSm86Policy::Route::NewerPtx;
	// Include the complete release archive + managed configuration identities. An update never edits loaded
	// code/configuration, and a read-only installation remains supported.
	const auto& ini = DlssgSm86Package::Files[forward ? 2 : 1];
	const auto& binary = DlssgSm86Package::Files[0];
	const auto cache = std::filesystem::path(local.get()) / L"Magpie/DlssgSm86" /
		DlssgSm86Package::ArchiveSHA256 / ini.sha256;
	std::filesystem::create_directories(cache);
	const auto source = root / DlssgSm86Package::Directory;
	packageDirectory = cache;
	for (const auto& file : DlssgSm86Package::Files) {
		const auto destination = cache / file.name;
		std::filesystem::create_directories(destination.parent_path());
		if (!CacheFile(source / file.name, destination, file))
			return Fail("complete package cache copy/integrity check failed");
		HANDLE held = PinnedFile::Open(destination);
		if (held == INVALID_HANDLE_VALUE) return Fail("cannot lock complete cached package");
		packageFiles.push_back(held);
		if (!PinnedFile::Verify(held, file.size, file.sha256))
			return Fail("cached package changed before it could be locked");
	}
	if (!CacheFile(source / ini.name, cache / L"dlssg_sm86.ini", ini))
		return Fail("managed configuration cache copy failed");
	proxyFile = PinnedFile::Open(cache / binary.name);
	configFile = PinnedFile::Open(cache / L"dlssg_sm86.ini");
	if (!PinnedFile::Verify(proxyFile, binary.size, binary.sha256) ||
		!PinnedFile::Verify(configFile, ini.size, ini.sha256)) {
		return Fail("cache cannot be locked and verified before loading");
	}
	// An empty private directory provides an explicit load request, not an on-disk
	// fallback. CreateDirectory must create it (an existing directory is rejected).
	const auto triggerDirectory = cache / (L"request-" + std::to_wstring(GetCurrentProcessId()) +
		L"-" + std::to_wstring(GetTickCount64()));
	if (!CreateDirectoryW(triggerDirectory.c_str(), nullptr)) return Fail("cannot create private FG load-request directory");
	const auto trigger = triggerDirectory / std::filesystem::path(DlssgSm86Package::Runtime.name).filename();
	WIN32_FILE_ATTRIBUTE_DATA attributes{};
	if (GetFileAttributesExW(trigger.c_str(), GetFileExInfoStandard, &attributes) || GetLastError() != ERROR_FILE_NOT_FOUND)
		return Fail("private FG load-request path is occupied or inaccessible");
	proxy = LoadLibraryExW((cache / binary.name).c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!proxy) {
		Logger::Get().Win32Error("DLSSFG community proxy load failed; no native FG fallback");
		return Fail("community proxy LoadLibraryEx failed");
	}
	stage = Stage::ProxyMapped;
	if (!SamePath(ModulePath(proxy), cache / binary.name) || !GetProcAddress(proxy, "DlssgProxy_Name"))
		return Fail("proxy mapped from an unexpected path or lacks its identity export");
	Logger::Get().Info(fmt::format(
		"DLSSFG proxy loaded: package={} revision={} sha256={} PCI={:04x}:{:04x} "
		"LUID={:08x}:{:08x} physicalSM={}.{} cudaDriverApi={} policy={} "
		"Optimized=1 maxGenerated=3 SpoofArchToGame=0 path={} "
		"(kernel execution not yet verified; inspect loader/backend logs)",
		DlssgSm86Package::Version, DlssgSm86Package::Revision, binary.sha256,
		desc.VendorId, desc.DeviceId, static_cast<uint32_t>(desc.AdapterLuid.HighPart), desc.AdapterLuid.LowPart,
		major, minor, driver, forward ? "newer-GPU-PTX-no-arch-bridge" : "native-family",
		StrHelper::UTF16ToUTF8(cache.wstring())));
	// The pinned proxy is lazy: mapping version.dll alone does not activate its
	// adjacent FG runtime. This standard Windows request triggers Bundled mode.
	runtime = LoadLibraryExW(trigger.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!runtime) {
		Logger::Get().Win32Error("DLSSFG explicit bundled runtime request failed");
		return Fail("bundled runtime request did not return a module");
	}
	const auto actualRuntime = ModulePath(runtime);
	if (actualRuntime.empty() || SamePath(actualRuntime.parent_path(), triggerDirectory) ||
		!SamePath(actualRuntime, cache / DlssgSm86Package::Runtime.name))
		return Fail("FG load did not redirect to a named bundled runtime");
	if (GetFileAttributesExW(trigger.c_str(), GetFileExInfoStandard, &attributes) || GetLastError() != ERROR_FILE_NOT_FOUND)
		return Fail("private FG request path changed during load; refusing ambiguous fallback");
	featureDirectory = actualRuntime.parent_path();
	if (!VerifyPairImpl()) return false;
	stage = Stage::BundleVerified;
	// Empty and owned by this invocation; never recursively delete this path.
	RemoveDirectoryW(triggerDirectory.c_str());
	Logger::Get().Info(fmt::format(
		"DLSSFG stage=BundleVerified runtime={} runtimeSha256={} backendSha256={}; "
		"NGX discovery, capability, Feature creation and kernel execution remain separate stages",
		StrHelper::UTF16ToUTF8(actualRuntime.wstring()), DlssgSm86Package::Runtime.sha256,
		DlssgSm86Package::Backend.sha256));
	return true;
}

} // namespace

bool Prepare(IDXGIAdapter* adapter) noexcept {
	try { return PrepareImpl(adapter); }
	catch (...) {
		std::lock_guard lock(runtimeMutex);
		return Fail("exception during community runtime preparation");
	}
}

bool GetFeatureDirectory(std::filesystem::path& directory) noexcept {
	try {
		std::lock_guard lock(runtimeMutex);
		directory.clear();
		if (stage != Stage::BundleVerified) return false;
		directory = featureDirectory;
		return true;
	} catch (...) { return false; }
}

bool VerifyLoadedPair() noexcept {
	try {
		std::lock_guard lock(runtimeMutex);
		return stage == Stage::BundleVerified && VerifyPairImpl();
	} catch (...) {
		std::lock_guard lock(runtimeMutex);
		return Fail("exception while verifying loaded runtime/backend");
	}
}
#else
bool Prepare(IDXGIAdapter*) noexcept { return false; }
bool GetFeatureDirectory(std::filesystem::path& directory) noexcept { directory.clear(); return false; }
bool VerifyLoadedPair() noexcept { return false; }
#endif

}
