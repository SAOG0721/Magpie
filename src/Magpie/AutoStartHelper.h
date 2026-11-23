#pragma once

namespace Magpie {

struct AutoStartHelper {
	enum class Scope { CurrentExecutable, CurrentUser };

	static bool EnableAutoStart(bool runElevated, Scope scope = Scope::CurrentExecutable) noexcept;
	static bool DisableAutoStart() noexcept;
	static bool IsAutoStartEnabled(Scope scope = Scope::CurrentExecutable) noexcept;
	static void MigrateLegacyAutoStartTask() noexcept;
};

}
