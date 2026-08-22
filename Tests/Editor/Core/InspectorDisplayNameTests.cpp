// InspectorDisplayNameTests.cpp - regression coverage for readable Inspector labels.

#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Windows/InspectorDisplayNames.h"

namespace TerraEngine::EditorTests
{
	void RegisterInspectorDisplayNameTests(TestRegistry& registry)
	{
		registry.Add("Editor.Inspector.DisplayNamesHumanizeStableKeys", [](TestContext& context) {
			context.ExpectEqual(std::string("Play On Start"),
				TerraEngine::Editor::BuildInspectorIdentifierLabel("playOnStart"),
				"Camel-case property keys should be presented as separate title-cased words.");
			context.ExpectEqual(std::string("Clip Asset ID"),
				TerraEngine::Editor::BuildInspectorIdentifierLabel("clipAssetId"),
				"Common identifier abbreviations should remain recognizable in labels.");
			context.ExpectEqual(std::string("Audio Source"),
				TerraEngine::Editor::BuildInspectorIdentifierLabel("AudioSource"),
				"Pascal-case component names should be split into readable words.");
			context.ExpectEqual(std::string("GPU LOD 2 ID"),
				TerraEngine::Editor::BuildInspectorIdentifierLabel("gpuLOD2Id"),
				"Acronyms and numeric suffixes should retain clear word boundaries.");
		});

		registry.Add("Editor.Inspector.CorePropertiesUseAuthoringLabels", [](TestContext& context) {
			context.ExpectEqual(std::string("Audio Clip"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("AudioSourceComponent", "clipAssetId"),
				"An audio asset selector should describe the asset rather than expose its UUID field.");
			context.ExpectEqual(std::string("Mixer Group"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("AudioSourceComponent", "mixGroupId"),
				"Audio routing should use author-facing mixer terminology.");
			context.ExpectEqual(std::string("Use For Playback"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("AudioListenerComponent", "eligible"),
				"Listener eligibility should explain its effect.");
			context.ExpectEqual(std::string("Vertical FOV (Degrees)"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("CameraComponent", "verticalFovDegrees"),
				"Camera labels should expose familiar terminology and units.");
			context.ExpectEqual(std::string("Aspect Ratio"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("CameraComponent", "aspectRatio"),
				"Properties without a domain override should still use the readable fallback.");
			context.ExpectEqual(std::string("Terrain Asset"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("TerrainComponent", "terrainAssetId"),
				"Terrain references should be described as assets instead of UUID storage fields.");
			context.ExpectEqual(std::string("Cast Shadows"),
				TerraEngine::Editor::BuildStandardInspectorPropertyLabel("TerrainComponent", "castsShadows"),
				"Terrain rendering options should use the established author-facing wording.");
		});
	}
}
