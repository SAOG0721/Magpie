#include "ConfigLocations.h"
#include "ConfigPersistence.h"
#include "ConfigRecovery.h"
#include <cassert>
#include <iostream>

static void Put(const std::filesystem::path& path, std::string_view text = "{}") {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << text;
	assert(file.good());
}

int wmain(int argc, wchar_t** argv) {
	assert(argc == 2);
	const auto root = std::filesystem::absolute(argv[1]) / (L"locations-" + std::to_wstring(GetCurrentProcessId()));
	const auto exe = root / L"app";
	const auto user = root / L"local";
	const auto enhanced = user / L"Magpie" / L"config" / L"v4e" / L"config.json";
	const auto legacy = user / L"Magpie" / L"config" / L"v4" / L"config.json";
	const auto portable = exe / L"config" / L"v4e" / L"config.json";
	using Magpie::ConfigLocations::Select;
	auto selected = Select(exe, user);
	assert(selected.source.empty() && selected.destination == enhanced && !selected.error);
	Put(legacy, R"({"original":4})");
	const std::string upstreamBytes = "{\r\n  \"original\":4\r\n}\r\n";
	Put(legacy, upstreamBytes);
	std::string loaded;
	uint32_t readError = 0;
	assert(Magpie::ConfigPersistence::ReadFileBytes(legacy, loaded, &readError));
	assert(loaded == upstreamBytes && !readError);
	selected = Select(exe, user);
	assert(selected.source == legacy && selected.destination == enhanced && !selected.portable);
	Magpie::ConfigSaveState state;
	assert(Magpie::ConfigPersistence::WriteAtomic(selected.destination, R"({"enhanced":true})", 1, state));
	assert(Magpie::ConfigPersistence::Read(legacy) == upstreamBytes);
	const auto preserved = Magpie::ConfigRecovery::FilesFor(enhanced, loaded);
	assert(Magpie::ConfigRecovery::Preserve(legacy, loaded, preserved));
	assert(Magpie::ConfigPersistence::Read(preserved.original) == upstreamBytes);
	const auto oversized = root / L"oversized.json";
	{
		std::ofstream file(oversized, std::ios::binary);
		file.seekp(Magpie::ConfigPersistence::MaxFileBytes);
		file.put(' ');
		assert(file.good());
	}
	assert(!Magpie::ConfigPersistence::ReadFileBytes(oversized, loaded, &readError));
	assert(readError == ERROR_FILE_TOO_LARGE && loaded.empty());
	assert(Magpie::ConfigPersistence::Read(oversized).empty());
	assert(std::filesystem::file_size(oversized) == Magpie::ConfigPersistence::MaxFileBytes + 1);
	int backupReads = 0;
	const auto readBackup = [&] { ++backupReads; return upstreamBytes; };
	assert(Magpie::ConfigRecovery::PrepareWithBackup("{}", readBackup, "Recovered").kind !=
		Magpie::ConfigRecovery::Kind::Backup);
	assert(backupReads == 0);
	assert(Magpie::ConfigRecovery::PrepareWithBackup("{broken", readBackup, "Recovered").kind ==
		Magpie::ConfigRecovery::Kind::Backup);
	assert(backupReads == 1);
	const std::string oversizedOutput(Magpie::ConfigPersistence::MaxFileBytes + 1, ' ');
	assert(!Magpie::ConfigPersistence::WriteAtomic(enhanced, oversizedOutput, 2, state));
	assert(GetLastError() == ERROR_FILE_TOO_LARGE);
	assert(Magpie::ConfigPersistence::Read(enhanced) == R"({"enhanced":true})");
	assert(Select(exe, user).source == enhanced);
	Put(enhanced, "{broken");
	assert(Select(exe, user).source == enhanced); // Existing v4e damage never selects v4.
	std::filesystem::remove(enhanced);
	Put(enhanced.native() + L".bak");
	assert(Select(exe, user).source == enhanced.native() + L".bak");
	std::filesystem::remove(enhanced.native() + L".bak");
	Put(exe / L"config" / L"config.json", R"({"portableOriginal":true})");
	selected = Select(exe, user);
	assert(selected.portable && selected.source == exe / L"config" / L"config.json" && selected.destination == portable);
	Put(enhanced);
	assert(Select(exe, user).source == enhanced); // A migrated user copy beats stale portable legacy data.
	Put(portable);
	assert(Select(exe, user).source == portable);
	// Switching away first commits a newer destination revision. An older
	// async save arriving after old-file removal must not resurrect portability.
	Magpie::ConfigSaveState switched;
	assert(Magpie::ConfigPersistence::WriteAtomic(enhanced, R"({"latest":true})", 2, switched));
	std::filesystem::remove(portable);
	assert(Magpie::ConfigPersistence::WriteAtomic(portable, R"({"stale":true})", 1, switched));
	assert(!std::filesystem::exists(portable) && Select(exe, user).source == enhanced);
	std::filesystem::create_directory(portable);
	selected = Select(exe, user);
	assert(selected.error == ERROR_DIRECTORY && selected.source == portable); // Wrong type is not absence.
	std::cout << "Config locations: v4e precedence, read-only v4 import, backup, portable isolation and invalid-path handling passed.\n";
}
