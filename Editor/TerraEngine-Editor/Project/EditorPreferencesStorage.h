// EditorPreferencesStorage.h - Canonical editor preference storage resolution.
//
// Normal editor runs always use one user-level file. Tests and installed-tool
// runs may opt into a separate session file without changing that global file.

#pragma once

#include <filesystem>

namespace TerraEngine::Editor
{
	inline constexpr char kEditorPreferencesSessionPathEnvironment[] =
		"TERRAENGINE_EDITOR_PREFS_SESSION_PATH";

	struct EditorPreferencesStorageLocation
	{
		std::filesystem::path path;
		bool isolatedSession = false;
	};

	// Resolve the only editor-wide preference document for the current user.
	// An empty result means the platform did not expose a suitable user config
	// directory; callers should use defaults instead of writing beside the app.
	[[nodiscard]] std::filesystem::path ResolveGlobalEditorPreferencesPath();

	// Resolve the store used by this process. The environment variable above is
	// deliberately session-specific: it gives tests and automation an isolated
	// writable document but never changes where global preferences live.
	[[nodiscard]] EditorPreferencesStorageLocation ResolveEditorPreferencesStorageLocation();
}
