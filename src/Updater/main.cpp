// Copyright (c) Xu
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "pch.h"
#include "CommonSharedConstants.h"
#include "Logger.h"
#include "PackageFiles.h"
#include "DlssgSm86Package.h"
#include "UpdateTransaction.h"
#include "StrHelper.h"
#include "Version.h"
#include <shellapi.h>

static void InitializeLogger() noexcept {
	// 日志文件创建在 Temp 目录中
	std::wstring logPath(MAX_PATH + 1, L'\0');
	const DWORD len = GetTempPath(MAX_PATH + 2, logPath.data());
	if (len <= 0) {
		return;
	}

	logPath.resize(len);
	if (!logPath.ends_with(L'\\')) {
		logPath.push_back(L'\\');
	}

	logPath.append(CommonSharedConstants::UPDATER_LOG_NAME);

	Logger::Get().Initialize(
		spdlog::level::info,
		std::move(logPath),
		CommonSharedConstants::LOG_MAX_SIZE,
		1
	);
}

// 将当前目录设为程序所在目录
static bool SetWorkingDir() noexcept {
	std::wstring exePath;
	HRESULT hr = wil::GetModuleFileNameW(NULL, exePath);
	if (FAILED(hr)) {
		Logger::Get().ComError("GetModuleFileNameW 失败", hr);
		return false;
	}

	std::filesystem::path parentPath =
		std::filesystem::path(std::move(exePath)).parent_path();

	if (!SetCurrentDirectory(parentPath.c_str())) {
		Logger::Get().Win32Error("SetCurrentDirectory 失败");
		return false;
	}

	return true;
}

static bool FileExists(const wchar_t* fileName) noexcept {
	DWORD attrs = GetFileAttributes(fileName);
	// 排除文件夹
	return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

static bool WaitForMagpieToExit() noexcept {
	{
		wil::unique_mutex_nothrow hSingleInstanceMutex;
		if (hSingleInstanceMutex.try_create(CommonSharedConstants::SINGLE_INSTANCE_MUTEX_NAME)) {
			if (wil::handle_wait(hSingleInstanceMutex.get(), 10000)) {
				hSingleInstanceMutex.ReleaseMutex();
			}
		}
	}

	// 即使 mutex 已被释放，Magpie.exe 仍有可能正在后台执行清理工作
	// Wait for exclusive access without destroying the rollback baseline.
	for (int i = 0; i < 1000; ++i) {
		wil::unique_handle file(CreateFileW(L"Magpie.exe", GENERIC_READ | DELETE,
			0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
		if (file) {
			return true;
		}

		Sleep(10);
	}

	// 超时
	return false;
}

static bool VerifyCommunityPackage(const std::filesystem::path& root) {
#ifdef MP_ENABLE_DLSS_FRAME_GENERATION
	const bool required = true;
#else
	const bool required = std::filesystem::exists(root / Magpie::DlssgSm86Package::Directory);
#endif
	if (!required) return true;
	if (!Magpie::DlssgSm86Package::Verify(root)) return false;
	for (const auto* name : { L"version.dll", L"winmm.dll", L"dbghelp.dll", L"dinput8.dll", L"nvngx_dlssg.dll", L"dlssg_sm86.ini" }) {
		if (std::filesystem::exists(root / name)) return false;
	}
	return true;
}

int APIENTRY wWinMain(
	_In_ HINSTANCE /*hInstance*/,
	_In_opt_ HINSTANCE /*hPrevInstance*/,
	_In_ wchar_t* lpCmdLine,
	_In_ int /*nCmdShow*/)
{
	if (!lpCmdLine || lpCmdLine[0] == 0) {
		// 参数为空则什么事都不做，以防止用户手动启动 Updater.exe
		return 0;
	}

	InitializeLogger();

	Logger::Get().Info("程序启动");

	if (!SetWorkingDir()) {
		Logger::Get().Critical("SetWorkingDir 失败");
		return 1;
	}

	Version oldVersion;
	if (!oldVersion.Parse(StrHelper::UTF16ToUTF8(lpCmdLine))) {
		Logger::Get().Critical(StrHelper::Concat(
			"解析版本号失败: ", StrHelper::UTF16ToUTF8(lpCmdLine)));
		return 1;
	}

	Logger::Get().Info(StrHelper::Concat("旧版本号: ", oldVersion.ToString<char>()));

	std::optional<PackageFiles> oldFiles = PackageFiles::Get(oldVersion);
	if (!oldFiles) {
		Logger::Get().Critical(StrHelper::Concat("未找到版本 ", oldVersion.ToString<char>()));
		return 1;
	}

	// 检查 Updater.exe 所处环境
	if (!FileExists(L"Magpie.exe") || !FileExists(L"update\\Magpie.exe")) {
		Logger::Get().Critical("未找到 Magpie.exe 或 update\\Magpie.exe");
		return 1;
	}
	
	// Verify before touching the existing install. Updater is the new package's
	// executable (UpdateService has already replaced it), so its pins match it.
	UpdateTransaction transaction;
	bool installed = false;
	try {
		if (!VerifyCommunityPackage(CommonSharedConstants::UPDATE_DIR)) {
			Logger::Get().Critical("Update DLSSFG package integrity check failed; downloaded files retained");
			return 1;
		}
		if (!WaitForMagpieToExit()) {
			Logger::Get().Critical("等待 Magpie.exe 退出超时");
			return 1;
		}
		std::vector<std::filesystem::path> obsolete;
		for (const wchar_t* file : oldFiles->files) obsolete.emplace_back(file);
		if (std::filesystem::exists(std::filesystem::path(CommonSharedConstants::UPDATE_DIR) /
			Magpie::DlssgSm86Package::Directory)) {
			// Exact old ownership only; UpdateTransaction retains rollback copies.
			for (const auto& file : Magpie::DlssgSm86Package::LegacyFiles) {
				if (Magpie::PinnedFile::Verify(file.name, file.size, file.sha256))
					obsolete.emplace_back(file.name);
			}
		}
		if (!transaction.Prepare(CommonSharedConstants::UPDATE_DIR, L".", obsolete)) {
			Logger::Get().Critical("Update rollback snapshot failed; existing install retained");
			return 1;
		}
		installed = transaction.Install() && VerifyCommunityPackage(L".");
	} catch (...) {
		Logger::Get().Critical("Update file transaction raised an exception");
	}
	if (!installed) {
		const bool restored = transaction.Rollback();
		Logger::Get().Error(StrHelper::Concat("Update transaction last file: ",
			StrHelper::UTF16ToUTF8(transaction.CurrentFile().wstring()),
			"; captured Win32 error: ", std::to_string(transaction.FailureCode()),
			"; rollback directory: ", StrHelper::UTF16ToUTF8(transaction.Backup().wstring())));
		Logger::Get().Critical(restored ?
			"Update failed; rollback completed. Update and backup retained; Magpie will not launch" :
			"Update failed and rollback incomplete. Update and backup retained; Magpie will not launch");
		return 1;
	}
	Logger::Get().Info(StrHelper::Concat("Update verified; rollback copy retained at ",
		StrHelper::UTF16ToUTF8(transaction.Backup().wstring())));

	// 删除 update 文件夹
	HRESULT hr = wil::RemoveDirectoryRecursiveNoThrow(CommonSharedConstants::UPDATE_DIR);
	if (SUCCEEDED(hr)) {
		Logger::Get().Info("已删除 update 文件夹");
	} else {
		Logger::Get().ComError("RemoveDirectoryRecursiveNoThrow 失败", hr);
	}

	// 启动 Magpie
	SHELLEXECUTEINFO execInfo{
		.cbSize = sizeof(execInfo),
		.fMask = SEE_MASK_NOASYNC,
		.lpVerb = L"open",
		.lpFile = L"Magpie.exe",
		.nShow = SW_SHOWNORMAL
	};
	if (ShellExecuteEx(&execInfo)) {
		Logger::Get().Info("已启动 Magpie.exe");
	} else {
		Logger::Get().Win32Error("ShellExecuteEx 失败");
	}

	Logger::Get().Info("程序退出");
	Logger::Get().Flush();
	return 0;
}
