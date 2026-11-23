#pragma once
#include "Win32Helper.h"

namespace Magpie {

inline HMODULE LoadPackagedFidelityFxUpscaleModule(const wchar_t* name) noexcept {
	const auto path = Win32Helper::GetExePath().parent_path() / name;
	return LoadLibraryExW(path.c_str(), nullptr,
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
}

}
