// EditorPreferencesStorage.cpp - Single owner for editor preference paths.

#include "EditorPreferencesStorage.h"

#include <cstdlib>

namespace
{
	constexpr char kEditorPreferencesFileName[] = ".terra_editor_prefs.json";

	std::filesystem::path AppendEditorPreferenceFile(const char* basePath)
	{
		if (basePath == nullptr || basePath[0] == '\0')
		{
			return {};
		}
		return std::filesystem::path(basePath)
			/ "TerraEngine" / "Editor" / kEditorPreferencesFileName;
	}
}

namespace TerraEngine::Editor
{
	std::filesystem::path ResolveGlobalEditorPreferencesPath()
	{
		// Keep platform fallback policy here so early renderer startup and the
		// long-lived ProjectManager can never disagree about the global source.
#ifdef _WIN32
		return AppendEditorPreferenceFile(std::getenv("APPDATA"));
#elif defined(__APPLE__)
		if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
		{
			return std::filesystem::path(home) / "Library" / "Application Support"
				/ "TerraEngine" / "Editor" / kEditorPreferencesFileName;
		}
		return {};
#else
		if (const char* config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && config[0] != '\0')
		{
			return AppendEditorPreferenceFile(config);
		}
		if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0')
		{
			return std::filesystem::path(home) / ".config"
				/ "TerraEngine" / "Editor" / kEditorPreferencesFileName;
		}
		return {};
#endif
	}

	EditorPreferencesStorageLocation ResolveEditorPreferencesStorageLocation()
	{
		if (const char* sessionPath = std::getenv(kEditorPreferencesSessionPathEnvironment);
			sessionPath != nullptr && sessionPath[0] != '\0')
		{
			// The override is process-local and is intentionally reported as such so
			// diagnostics and tests cannot present it as another global preference file.
			return {std::filesystem::path(sessionPath), true};
		}
		return {ResolveGlobalEditorPreferencesPath(), false};
	}
}
