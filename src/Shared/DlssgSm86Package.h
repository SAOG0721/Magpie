#pragma once
#include "PinnedFile.h"

namespace Magpie::DlssgSm86Package {

inline constexpr wchar_t Directory[] = L"runtimes/dlssg_sm86";
inline constexpr char Revision[] = "6f047bdc885e861575e8b049413ca31dc171a924";
inline constexpr char Version[] = "0.4.0";
inline constexpr char RuntimeVersion[] = "310.9.16.0";

struct File {
	const wchar_t* name;
	uint64_t size;
	const char* sha256;
};

// Complete immutable package; relative paths are also used by the upstream proxy.
inline constexpr uint64_t ArchiveSize = 80551324;
inline constexpr char ArchiveSHA256[] = "5865360c3295038f0ff979d303bb7eb9437eec09fbc08686967d3b5d41d9c019";
inline constexpr char ArchiveURL[] = "https://github.com/sdli1995/dlssg_for_sm86/releases/download/0.4.0/dlssg-release-0.4.0-x64.zip";
inline constexpr File Files[]{
	{ L"version.dll", 853280, "7b3f90435be4f94348bf8d128c84871c7f904c704f7690601a263277c327a7b2" },
	{ L"native.ini", 486, "d390834bea14c1c0f72331e90707f935918a0f136cff223b592b66ddc2e4b344" },
	{ L"forward.ini", 485, "282d23df4c96a72f706dd7b2c926df9534c993f414b92be7aed8965c44171c8a" },
	{ L"THIRD_PARTY_NOTICES.txt", 8139, "c8f2449237f0c556ab7b494ae131e534b39614b5d0f1e56e39d95353426ff2f7" },
	{ L"LICENSE.md", 1065, "7537220f50c6430d953eda8bff01f0817014e604cb3607f3b62c786d070d8570" },
	{ L"dlssg_sm86/backend/sm86_backend.dll", 2109728, "30ffc6e09a347ccde87ecc6fa54fd7818f5ed8a9353f5c31c2a90835c7f0ca9b" },
	{ L"dlssg_sm86/kernels/fg_kernels.dll", 18318624, "1394c2d5d9cc8938ba3e52b043048d20d6d4c13a019c161b11a3dfd16343ec24" },
	{ L"dlssg_sm86/runtime/nvngx_dlssg.dll", 7506544, "cadee1aa6db3907babdc3285da2aebc4553d92b89c38ba3eae8abd8bba1bea72" }
};
inline constexpr File Backend = Files[5];
inline constexpr File Kernels = Files[6];
inline constexpr File Runtime = Files[7];
// Only these exact former component files are eligible for cleanup/rollback.
inline constexpr File LegacyFiles[]{
 { L"nvngx_dlssg.dll", 7460976, "ff6e90eb78b827927dff5b4ecc6b1c870c2e9bca29ed9f48c7d348cc9e170b82" },
 { L"version.dll", 30021920, "c3934a09399f022504227c72df0bf8c0de55f9a08880dddde898c5262cefa838" },
 { L"dlssg_sm86.ini", 463, "38b42e4325b43da3f451539a24c711f7b1c8bfac590c8548ec92d543eab02923" },
 { L"dlssg_sm86.ini", 452, "acd899417ffbf3a9d73725e3526932465d21835d92b52192e2e9a4e22ce23a6d" }
};

inline bool Verify(const std::filesystem::path& root) {
	for (const auto& file : Files) {
		if (!PinnedFile::Verify(root / Directory / file.name, file.size, file.sha256)) return false;
	}
	return true;
}

}
