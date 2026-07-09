// PlatformUtils.h — Platform-specific utility functions.
//
// This file declares utility functions that wrap OS-specific APIs for common
// tasks such as retrieving persistent data paths, opening URLs in the default
// browser, and performing string conversions (e.g., UTF-8 to wide string).

#pragma once

#include "Core/FatalErrorReport.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace TerraEngine
{
	// Keep the managed and native platform identifiers aligned so the script
	// bridge can pass the enum value through as an integer without translation
	// tables that could drift out of sync later.
	enum class RuntimePlatform : std::uint32_t
	{
		Unknown = 0,
		Windows = 1,
		Linux = 2,
		macOS = 3
	};

	namespace PlatformUtils
	{
		// Returns the host operating system TerraEngine is currently running on.
		RuntimePlatform GetRuntimePlatform();

		// Returns the absolute path to the running executable when the platform
		// exposes it. Callers should handle an empty path by falling back to a
		// context-specific root such as current_path().
		std::filesystem::path GetExecutablePath();

		// Returns the directory containing the running executable, or an empty
		// path when executable discovery is unavailable on the current platform.
		std::filesystem::path GetExecutableDirectory();

		// Resolves the per-product writable save-data directory and creates it if
		// possible so gameplay code can write files immediately.
		std::filesystem::path GetPersistentDataPath(std::string_view productName);

		// Resolves the per-product diagnostics directory. Crash dumps and fatal
		// sidecars live here instead of SaveData so game/editor data remains
		// clean and user backups do not pick up heavy diagnostic artifacts.
		std::filesystem::path GetDiagnosticsDataPath(std::string_view productName);

		// Returns the crash dump folder under Diagnostics/CrashDumps and creates
		// it when possible. Callers receive an empty path only if no safe
		// writable diagnostics location can be resolved.
		std::filesystem::path GetCrashDumpDirectory(std::string_view productName);

		// Writes a native crash dump when the host platform supports it and
		// always attempts to write a small text sidecar with the same report.
		FatalErrorDialogReport WriteCrashDumpForReport(FatalErrorDialogReport report, std::string_view productName);

		// Asks the host operating system to open the URL with the user's default
		// handler. Invalid input is rejected with a warning instead of crashing.
		bool OpenUrl(std::string_view url);
	}
}
