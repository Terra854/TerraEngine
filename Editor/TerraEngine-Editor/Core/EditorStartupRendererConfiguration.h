// EditorStartupRendererConfiguration.h - Read-only pre-Vulkan editor profile.

#pragma once

#include "Engine/Renderer/RenderTypes.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Editor
{
	enum class StartupRendererOrigin { DirectProject, HubDefaults };
	enum class StartupPreferenceOrigin { EngineDefaults, UserPreferences, MigratedProjectDiagnostics };

	struct EditorStartupRendererConfiguration
	{
		StartupRendererOrigin origin = StartupRendererOrigin::HubDefaults;
		StartupPreferenceOrigin preferenceOrigin = StartupPreferenceOrigin::EngineDefaults;
		std::optional<std::filesystem::path> projectRoot;
		RenderSettings renderSettings{};
		bool validationEnabled = false;
		bool debugEnabled = false;
		std::vector<std::string> diagnostics;
	};

	[[nodiscard]] EditorStartupRendererConfiguration ResolveEditorStartupRendererConfiguration(
		const std::optional<std::filesystem::path>& projectPath);
}
