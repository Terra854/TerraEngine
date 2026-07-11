// EditorStartupRendererConfiguration.cpp - Deterministic startup resolver.

#include "EditorStartupRendererConfiguration.h"
#include "../Project/EditorPreferences.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>

namespace
{
	std::filesystem::path ResolvePreferencesPath()
	{
		if (const char* overridePath = std::getenv("TERRAENGINE_EDITOR_PREFS_PATH"); overridePath && overridePath[0]) {
			return overridePath;
		}
#ifdef _WIN32
		if (const char* appData = std::getenv("APPDATA"); appData && appData[0]) {
			return std::filesystem::path(appData) / "TerraEngine" / "Editor" / ".terra_editor_prefs.json";
		}
#else
		if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && config[0]) {
			return std::filesystem::path(config) / "TerraEngine" / "Editor" / ".terra_editor_prefs.json";
		}
#endif
		return {};
	}

	std::filesystem::path NormalizeProjectPath(const std::filesystem::path& path)
	{
		std::error_code error;
		auto normalized = std::filesystem::weakly_canonical(path, error);
		if (!error) return normalized;
		normalized = std::filesystem::absolute(path, error);
		return error ? path.lexically_normal() : normalized.lexically_normal();
	}

	void ReadProjectSettings(const std::filesystem::path& root,
		TerraEngine::Editor::EditorStartupRendererConfiguration& configuration)
	{
		const std::filesystem::path path = root / "ProjectSettings" / "RenderSettings.json";
		if (!std::filesystem::exists(path)) {
			configuration.diagnostics.push_back("Project render settings are missing; using engine defaults.");
			return;
		}
		try {
			std::ifstream stream(path);
			nlohmann::json document;
			stream >> document;
			const nlohmann::json& settings = document.contains("settings") && document["settings"].is_object()
				? document["settings"] : document;
			RenderSettings& output = configuration.renderSettings;
			output.shadowResolution = std::clamp(settings.value("shadowResolution", output.shadowResolution), 128u, 16384u);
			output.shadowFilteringSize = std::clamp(settings.value("shadowFilteringSize", output.shadowFilteringSize), 1u, 15u);
			output.maxUploadedLights = std::clamp(settings.value("maxUploadedLights", output.maxUploadedLights), 1u, 4096u);
			output.maxDynamicLights = std::min(settings.value("maxDynamicLights", output.maxDynamicLights), output.maxUploadedLights);
			output.maxShadowLights = std::clamp(settings.value("maxShadowLights", output.maxShadowLights), 0u, kMaxShadowResourceSlots);
			output.maxAreaLights = std::min(settings.value("maxAreaLights", output.maxAreaLights), output.maxDynamicLights);
			output.enableLTC = settings.value("enableLTC", output.enableLTC) ? 1u : 0u;
			output.enableIBL = settings.value("enableIBL", output.enableIBL) ? 1u : 0u;
			output.indirectStrength = std::clamp(settings.value("indirectStrength", output.indirectStrength), 0.0f, 16.0f);
			output.ambientOcclusionStrength = std::clamp(settings.value("ambientOcclusionStrength", output.ambientOcclusionStrength), 0.0f, 4.0f);
			output.ambientOcclusionQuality = std::clamp(settings.value("ambientOcclusionQuality", output.ambientOcclusionQuality), 0u, 3u);
			output.anisotropicFiltering = std::clamp(settings.value("anisotropicFiltering", output.anisotropicFiltering), 1u, 16u);
			output.variableRateShading = settings.value("variableRateShading", output.variableRateShading) ? 1u : 0u;
			configuration.validationEnabled = document.value("validationEnabled", settings.value("validationEnabled", false));
			configuration.debugEnabled = document.value("debugEnabled", settings.value("debugEnabled", false));
			output.validationEnabled = configuration.validationEnabled ? 1u : 0u;
			output.debugEnabled = configuration.debugEnabled ? 1u : 0u;
			if (settings.contains("virtualGeometry") && settings["virtualGeometry"].is_object()) {
				const auto& virtualGeometry = settings["virtualGeometry"];
				output.virtualGeometry.lodErrorScale = std::clamp(virtualGeometry.value("lodErrorScale", output.virtualGeometry.lodErrorScale), 0.05f, 64.0f);
				output.virtualGeometry.maxLodDrop = virtualGeometry.value("maxLodDrop", output.virtualGeometry.maxLodDrop);
				output.virtualGeometry.hiZOcclusionEnabled = virtualGeometry.value("hiZOcclusionEnabled", output.virtualGeometry.hiZOcclusionEnabled);
				output.virtualGeometry.hiZSelfOcclusionGuardEnabled = virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true);
				output.virtualGeometry.hiZNearPlaneGuardEnabled = virtualGeometry.value("hiZNearPlaneGuardEnabled", true);
				output.virtualGeometry.hiZThinGeometryGuardEnabled = virtualGeometry.value("hiZThinGeometryGuardEnabled", true);
				output.virtualGeometry.geometryBudgetBytes = virtualGeometry.value("geometryBudgetBytes", output.virtualGeometry.geometryBudgetBytes);
				output.virtualGeometry.preloadBias = std::clamp(virtualGeometry.value("preloadBias", output.virtualGeometry.preloadBias), 0.0f, 8.0f);
			}
		} catch (const std::exception& exception) {
			configuration.diagnostics.push_back(std::string("Project render settings are malformed; using safe defaults: ") + exception.what());
			configuration.renderSettings = {};
			configuration.validationEnabled = false;
			configuration.debugEnabled = false;
		}
	}

	void ApplyUserPreferences(TerraEngine::Editor::EditorStartupRendererConfiguration& configuration)
	{
		const std::filesystem::path path = ResolvePreferencesPath();
		if (path.empty() || !std::filesystem::exists(path)) return;
		try {
			std::ifstream stream(path);
			nlohmann::json document;
			stream >> document;
			const EditorPreferences preferences = document.get<EditorPreferences>();
			if (!preferences.rendererDiagnosticsPreferencesInitialized) return;
			configuration.validationEnabled = preferences.vulkanValidationLayers;
			configuration.debugEnabled = preferences.renderDebugDiagnostics;
			configuration.renderSettings.validationEnabled = configuration.validationEnabled ? 1u : 0u;
			configuration.renderSettings.debugEnabled = configuration.debugEnabled ? 1u : 0u;
			configuration.preferenceOrigin = TerraEngine::Editor::StartupPreferenceOrigin::UserPreferences;
		} catch (const std::exception& exception) {
			configuration.diagnostics.push_back(std::string("Editor diagnostic preferences are malformed; project/default flags remain active: ") + exception.what());
		}
	}
}

namespace TerraEngine::Editor
{
	EditorStartupRendererConfiguration ResolveEditorStartupRendererConfiguration(
		const std::optional<std::filesystem::path>& projectPath)
	{
		EditorStartupRendererConfiguration configuration;
		if (projectPath) {
			configuration.origin = StartupRendererOrigin::DirectProject;
			configuration.preferenceOrigin = StartupPreferenceOrigin::MigratedProjectDiagnostics;
			configuration.projectRoot = NormalizeProjectPath(*projectPath);
			ReadProjectSettings(*configuration.projectRoot, configuration);
		} else {
			configuration.origin = StartupRendererOrigin::HubDefaults;
			configuration.diagnostics.push_back("Project hub launch uses explicit renderer defaults; current working directory discovery is disabled.");
		}
		ApplyUserPreferences(configuration);
		return configuration;
	}
}
