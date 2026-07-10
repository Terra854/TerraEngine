// EditorPreferences.h — Editor-wide user preferences
//
// Defines the data structures for persisting editor-wide settings and the list of recently opened projects. it utilizes nlohmann/json for automated serialization of preferences such as theme selection and external script editor commands.

#pragma once

#include "EditorMcpPreferences.h"
#include "../Windows/SceneViewOverlayTypes.h"

#include <string>
#include <vector>
#include <nlohmann/json.hpp>

struct RecentProjectEntry {
	std::string name;
	std::string path;
	long long lastOpened = 0;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(RecentProjectEntry, name, path, lastOpened)
};

// struct EditorPreferences groups related state and operations so callers have one documented contract.
struct EditorPreferences {
	std::vector<RecentProjectEntry> recentProjects;
	std::string theme = "Dark";
	std::string externalScriptEditorCommand;
	bool autoBuildScripts = true;
	// The MCP bridge is an editor-wide user preference, not project state, so
	// startup can restore the user's requested local automation endpoint.
	bool editorMcpEnabled = false;
	std::string editorMcpWriteModeDefault = "read_only";
	std::string editorMcpRecentSession;
	std::uint16_t editorMcpListenPort = TerraEngine::Editor::Preferences::kDefaultEditorMcpListenPort;
	// Renderer diagnostics are editor-wide preferences because they affect the
	// local developer session and should not force project/runtime defaults.
	bool vulkanValidationLayers = false;
	bool renderDebugDiagnostics = false;
	bool rendererDiagnosticsPreferencesInitialized = false;
	TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference sceneViewOverlay;

	friend void to_json(nlohmann::json& json, const EditorPreferences& preferences)
	{
		json = nlohmann::json{
			{"recentProjects", preferences.recentProjects},
			{"theme", preferences.theme},
			{"externalScriptEditorCommand", preferences.externalScriptEditorCommand},
			{"autoBuildScripts", preferences.autoBuildScripts},
			{"editorMcpEnabled", preferences.editorMcpEnabled},
			{"editorMcpWriteModeDefault", preferences.editorMcpWriteModeDefault},
			{"editorMcpRecentSession", preferences.editorMcpRecentSession},
			{"editorMcpListenPort", preferences.editorMcpListenPort},
			{"vulkanValidationLayers", preferences.vulkanValidationLayers},
			{"renderDebugDiagnostics", preferences.renderDebugDiagnostics},
			{"rendererDiagnosticsPreferencesInitialized", preferences.rendererDiagnosticsPreferencesInitialized},
			{"sceneViewOverlay", preferences.sceneViewOverlay}
		};
	}

	friend void from_json(const nlohmann::json& json, EditorPreferences& preferences)
	{
		// Each field falls back to the struct default so older or partially
		// written preference files remain loadable after editor upgrades.
		preferences.recentProjects = json.value("recentProjects", preferences.recentProjects);
		preferences.theme = json.value("theme", preferences.theme);
		preferences.externalScriptEditorCommand = json.value("externalScriptEditorCommand", preferences.externalScriptEditorCommand);
		preferences.autoBuildScripts = json.value("autoBuildScripts", preferences.autoBuildScripts);
		preferences.editorMcpEnabled = json.value("editorMcpEnabled", json.value("editorMcpEnabledByDefault", preferences.editorMcpEnabled));
		preferences.editorMcpWriteModeDefault = json.value("editorMcpWriteModeDefault", preferences.editorMcpWriteModeDefault);
		preferences.editorMcpRecentSession = json.value("editorMcpRecentSession", preferences.editorMcpRecentSession);
		if (const auto portIt = json.find("editorMcpListenPort"); portIt != json.end())
		{
			// Preference files are user-editable, so reject negative, privileged,
			// oversized, or ephemeral values without throwing during editor startup.
			if (portIt->is_number_unsigned())
			{
				const std::uint64_t port = portIt->get<std::uint64_t>();
				if (port <= TerraEngine::Editor::Preferences::kMaximumEditorMcpListenPort
					&& TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(static_cast<std::int64_t>(port)))
				{
					preferences.editorMcpListenPort = static_cast<std::uint16_t>(port);
				}
			}
			else if (portIt->is_number_integer())
			{
				const std::int64_t port = portIt->get<std::int64_t>();
				if (TerraEngine::Editor::Preferences::IsValidEditorMcpListenPort(port))
				{
					preferences.editorMcpListenPort = static_cast<std::uint16_t>(port);
				}
			}
		}
		preferences.vulkanValidationLayers = json.value("vulkanValidationLayers", preferences.vulkanValidationLayers);
		preferences.renderDebugDiagnostics = json.value("renderDebugDiagnostics", preferences.renderDebugDiagnostics);
		preferences.rendererDiagnosticsPreferencesInitialized =
			json.value("rendererDiagnosticsPreferencesInitialized", false)
			|| json.contains("vulkanValidationLayers")
			|| json.contains("renderDebugDiagnostics");
		if (json.contains("sceneViewOverlay") && json["sceneViewOverlay"].is_object())
		{
			preferences.sceneViewOverlay = json["sceneViewOverlay"].get<TerraEngine::Editor::SceneViewOverlay::SceneOverlayPreference>();
		}
		preferences.sceneViewOverlay = TerraEngine::Editor::SceneViewOverlay::SanitizePreference(preferences.sceneViewOverlay);
	}
};
