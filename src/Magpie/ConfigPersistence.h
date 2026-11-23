#pragma once
#include <windows.h>
#include <rapidjson/document.h>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>
#include <cctype>

namespace Magpie {

// Shared by pending saves so a background operation never accesses AppSettings
// after destruction. Revisions are assigned when taking the UI-thread snapshot.
struct ConfigSaveState {
	std::mutex mutex;
	std::atomic<uint64_t> nextRevision{ 0 };
	uint64_t savedRevision = 0;
};

namespace ConfigPersistence {

inline constexpr size_t MaxFileBytes = 16 * 1024 * 1024;

inline std::string_view JsonText(std::string_view text) noexcept {
	if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
	return text;
}

inline bool IsValid(std::string_view json) {
	json = JsonText(json);
	if (json.empty()) return false;
	rapidjson::Document doc;
	doc.Parse<rapidjson::kParseValidateEncodingFlag>(json.data(), json.size());
	return !doc.HasParseError() && doc.IsObject();
}

// Keep complete top-level members, and complete objects in a top-level array
// (scalingModes/profiles). Never manufacture a partially written effect/profile.
enum class IncompleteChain { None, ExistingGroup, NewGroup };

inline std::string RecoverPrefix(std::string_view damaged, IncompleteChain* incomplete = nullptr) {
	std::vector<char> stack;
	bool inString = false, escaped = false;
	size_t boundary = 0;
	size_t stringStart = 0, boundaryDepth = 0;
	std::string_view rootMember;
	bool completeEffectsArray = false;
	std::string closing;
	auto record = [&](size_t end, bool completeArray = false) {
		boundary = end;
		boundaryDepth = stack.size();
		completeEffectsArray = completeArray;
		closing.clear();
		for (auto it = stack.rbegin(); it != stack.rend(); ++it) closing += *it == '{' ? '}' : ']';
	};
	if (incomplete) *incomplete = IncompleteChain::None;
	for (size_t i = 0; i < damaged.size(); ++i) {
		const char c = damaged[i];
		if (inString) {
			if (escaped) escaped = false;
			else if (c == '\\') escaped = true;
			else if (c == '"') {
				inString = false;
				if (stack.size() == 1) {
					size_t next = i + 1;
					while (next < damaged.size() && std::isspace(static_cast<unsigned char>(damaged[next]))) ++next;
					if (next < damaged.size() && damaged[next] == ':') rootMember = damaged.substr(stringStart, i - stringStart);
				}
			}
			continue;
		}
		if (c == '"') { inString = true; stringStart = i + 1; continue; }
		if (c == '{' || c == '[') {
			stack.push_back(c);
		} else if (c == '}' || c == ']') {
			if (stack.empty() || stack.back() != (c == '}' ? '{' : '[')) break;
			stack.pop_back();
			if (stack.size() == 1 ||
				(stack.size() == 2 && stack[0] == '{' && stack[1] == '[') ||
				(incomplete && rootMember == "scalingModes" && (stack.size() == 3 || stack.size() == 4))) {
				record(i + 1, c == ']' && stack.size() == 3);
			}
		} else if (c == ',' && (stack.size() == 1 ||
			(incomplete && rootMember == "scalingModes" && (stack.size() == 2 || stack.size() == 3)))) {
			record(i);
		}
	}
	if (!boundary) {
		if (incomplete && rootMember == "scalingModes" && stack.size() >= 2) {
			*incomplete = IncompleteChain::NewGroup;
			return R"({"scalingModes":[]})";
		}
		return {};
	}
	std::string result(damaged.substr(0, boundary));
	result += closing;
	if (!IsValid(result)) return {};
	if (incomplete && rootMember == "scalingModes" && stack.size() >= 2 &&
		(inString || stack.size() > 2 || damaged.substr(boundary).find(',') != std::string_view::npos) && !completeEffectsArray) {
		*incomplete = boundaryDepth >= 3 ? IncompleteChain::ExistingGroup : IncompleteChain::NewGroup;
	}
	return result;
}

// Preserve exact bytes for recovery fingerprints and immutable originals.
// Text-mode CRT reads would translate CRLF from upstream's configuration.
inline bool ReadFileBytes(const std::filesystem::path& path, std::string& result, uint32_t* systemError = nullptr) {
	result.clear();
	if (systemError) *systemError = 0;
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		if (systemError) *systemError = GetLastError();
		return false;
	}
	DWORD error = ERROR_SUCCESS;
	LARGE_INTEGER size{};
	if (!GetFileSizeEx(file, &size)) error = GetLastError();
	else if (size.QuadPart < 0 || static_cast<uint64_t>(size.QuadPart) > MaxFileBytes) error = ERROR_FILE_TOO_LARGE;
	if (!error) {
		try {
			result.resize(static_cast<size_t>(size.QuadPart));
			for (size_t offset = 0; offset < result.size();) {
				DWORD read = 0;
				const DWORD bytes = static_cast<DWORD>((std::min)(result.size() - offset, size_t(MAXDWORD)));
				if (!ReadFile(file, result.data() + offset, bytes, &read, nullptr)) { error = GetLastError(); break; }
				if (!read) { error = ERROR_HANDLE_EOF; break; }
				offset += read;
			}
		} catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
	}
	if (!CloseHandle(file) && !error) error = GetLastError();
	if (systemError) *systemError = error;
	if (error) result.clear();
	return error == ERROR_SUCCESS;
}

inline std::string Read(const std::filesystem::path& path) {
	std::string result;
	ReadFileBytes(path, result);
	return result;
}

inline bool WriteAtomic(const std::filesystem::path& path, std::string_view json,
	uint64_t revision, ConfigSaveState& state) {
	if (json.size() > MaxFileBytes) { SetLastError(ERROR_FILE_TOO_LARGE); return false; }
	if (!IsValid(json)) { SetLastError(ERROR_INVALID_DATA); return false; }
	std::lock_guard lock(state.mutex);
	if (revision < state.savedRevision) return true;
	// Reject a read-only destination before copying its attributes into the backup.
	// Fixing that destination must be sufficient for the user to retry saving.
	const DWORD attributes = GetFileAttributesW(path.c_str());
	if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY)) {
		SetLastError(ERROR_ACCESS_DENIED);
		return false;
	}
	std::error_code ec;
	std::filesystem::create_directories(path.parent_path(), ec);
	if (ec) { SetLastError(static_cast<DWORD>(ec.value())); return false; }
	const auto temp = std::filesystem::path(path.native() + L".tmp");
	const auto backup = std::filesystem::path(path.native() + L".bak");
	HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) return false;
	bool ok = true;
	size_t offset = 0;
	while (offset < json.size()) {
		DWORD written = 0;
		const DWORD length = static_cast<DWORD>((std::min)(json.size() - offset, size_t(MAXDWORD)));
		if (!WriteFile(file, json.data() + offset, length, &written, nullptr) || !written) {
			ok = false;
			break;
		}
		offset += written;
	}
	if (ok) ok = FlushFileBuffers(file) != FALSE;
	DWORD error = GetLastError();
	if (!CloseHandle(file) && ok) { ok = false; error = GetLastError(); }
	if (ok) {
		// Do not replace a usable backup with the damaged input being recovered.
		const std::string previous = Read(path);
		if (IsValid(previous) && !CopyFileW(path.c_str(), backup.c_str(), FALSE)) {
			ok = false;
			error = GetLastError();
		}
	}
	if (ok) {
		ok = MoveFileExW(temp.c_str(), path.c_str(),
			MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
		error = GetLastError();
	}
	if (!ok) {
		DeleteFileW(temp.c_str());
		SetLastError(error);
		return false;
	}
	state.savedRevision = revision;
	return true;
}

} // namespace ConfigPersistence
} // namespace Magpie
