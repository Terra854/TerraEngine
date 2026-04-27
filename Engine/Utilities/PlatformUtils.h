// PlatformUtils.h — Platform-specific utility functions.
//
// This file declares utility functions that wrap OS-specific APIs for common
// tasks such as retrieving persistent data paths, opening URLs in the default
// browser, and performing string conversions (e.g., UTF-8 to wide string).

#pragma once

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

		// Resolves the per-product writable save-data directory and creates it if
		// possible so gameplay code can write files immediately.
		std::filesystem::path GetPersistentDataPath(std::string_view productName);

		// Asks the host operating system to open the URL with the user's default
		// handler. Invalid input is rejected with a warning instead of crashing.
		bool OpenUrl(std::string_view url);
	}
}
