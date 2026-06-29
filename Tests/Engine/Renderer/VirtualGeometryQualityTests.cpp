#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
namespace
{
	std::filesystem::path MakeVirtualGeometryQualityTestRoot(const char* name)
	{
		const std::filesystem::path root = std::filesystem::temp_directory_path() / name;
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root / "Assets");
		return root;
	}

	bool NearlyEqual(float lhs, float rhs)
	{
		return std::abs(lhs - rhs) <= 0.0001f;
	}

	void RestoreRenderSettings(const std::filesystem::path& previousAssetRoot, const RenderSettings& previousSettings)
	{
		// Restore the in-memory renderer settings before returning the shared
		// ResourceManager to its earlier root. The write goes to the temporary
		// test project rather than the developer's active project settings.
		RenderSystem::Instance().SetRenderSettings(previousSettings);
		ResourceManager::Instance().SetAssetRoot(previousAssetRoot);
	}
}

	void RegisterVirtualGeometryQualityTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Quality.SanitizesUnsafeControls", [](TestContext& context) {
			VirtualGeometryQualityProfile profile = MakeDefaultVirtualGeometryQualityProfile();
			profile.lodErrorScale = std::numeric_limits<float>::infinity();
			profile.preloadBias = std::numeric_limits<float>::quiet_NaN();
			profile.maxLodDrop = 7u;
			profile.hiZOcclusionEnabled = true;
			profile.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Residency;
			profile.geometryBudgetBytes = 64ull * 1024ull * 1024ull;

			VirtualGeometryQualityProfile sanitized = SanitizeVirtualGeometryQualityProfile(profile);
			context.Expect(NearlyEqual(1.0f, sanitized.lodErrorScale), "Non-finite LOD error scale should fall back to a safe default.");
			context.Expect(NearlyEqual(1.0f, sanitized.preloadBias), "Non-finite preload bias should fall back to a safe default.");
			context.ExpectEqual(7u, sanitized.maxLodDrop, "Sanitizing should preserve authored max LOD drop.");
			context.Expect(sanitized.hiZOcclusionEnabled, "Sanitizing should preserve Hi-Z toggle state.");
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Residency),
				static_cast<std::uint32_t>(sanitized.debugVisualizationMode),
				"Sanitizing should preserve debug visualization mode.");
			context.ExpectEqual(64ull * 1024ull * 1024ull, sanitized.geometryBudgetBytes, "Sanitizing should preserve the geometry budget.");

			profile.lodErrorScale = 0.001f;
			profile.preloadBias = 128.0f;
			sanitized = SanitizeVirtualGeometryQualityProfile(profile);
			context.Expect(NearlyEqual(0.05f, sanitized.lodErrorScale), "LOD error scale should clamp to the safe lower bound.");
			context.Expect(NearlyEqual(16.0f, sanitized.preloadBias), "Preload bias should clamp to the safe upper bound.");
		});

		registry.Add("VirtualGeometry.Quality.SavesProjectRenderSettings", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualitySaveTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");

			VirtualGeometryQualityProfile profile = MakeDefaultVirtualGeometryQualityProfile();
			profile.lodErrorScale = 2.5f;
			profile.maxLodDrop = 3u;
			profile.hiZOcclusionEnabled = true;
			profile.hiZSelfOcclusionGuardEnabled = false;
			profile.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;
			profile.geometryBudgetBytes = 123456u;
			profile.preloadBias = 4.0f;
			RenderSystem::Instance().SetVirtualGeometryQualityProfile(profile);

			std::ifstream input(root / "ProjectSettings" / "RenderSettings.json");
			nlohmann::json document;
			input >> document;
			input.close();
			const nlohmann::json& virtualGeometry = document["settings"]["virtualGeometry"];

			context.Expect(!virtualGeometry.contains("enabled"), "Saved render settings should not include a virtual geometry enable toggle.");
			context.Expect(NearlyEqual(2.5f, virtualGeometry.value("lodErrorScale", 0.0f)), "Saved render settings should include LOD error scale.");
			context.ExpectEqual(3u, virtualGeometry.value("maxLodDrop", 0u), "Saved render settings should include max LOD drop.");
			context.Expect(virtualGeometry.value("hiZOcclusionEnabled", false), "Saved render settings should include the Hi-Z toggle.");
			context.Expect(!virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true), "Saved render settings should include self-occlusion guard toggle.");
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::HiZ),
				virtualGeometry.value("debugVisualizationMode", 0u),
				"Saved render settings should include debug visualization mode.");
			context.ExpectEqual(123456ull, virtualGeometry.value("geometryBudgetBytes", 0ull), "Saved render settings should include geometry budget.");
			context.Expect(NearlyEqual(4.0f, virtualGeometry.value("preloadBias", 0.0f)), "Saved render settings should include preload bias.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Quality.LoadsProjectRenderSettings", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualityLoadTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");
			std::filesystem::create_directories(root / "ProjectSettings");

			nlohmann::json document;
			document["preset"] = static_cast<std::uint32_t>(QualityPreset::Custom);
			document["settings"] = {
				{ "shadowResolution", 1024u },
				{ "shadowFilteringSize", 3u },
				{ "maxUploadedLights", 64u },
				{ "maxDynamicLights", 32u },
				{ "maxShadowLights", 4u },
				{ "maxAreaLights", 2u },
				{ "enableLTC", 1u },
				{ "enableIBL", 1u },
				{ "indirectStrength", 1.0f },
				{ "ambientOcclusionStrength", 1.0f },
				{ "ambientOcclusionQuality", 2u },
				{ "anisotropicFiltering", 4u },
				{ "variableRateShading", 0u },
				{ "virtualGeometry", {
					{ "lodErrorScale", 3.0f },
					{ "maxLodDrop", 2u },
					{ "hiZOcclusionEnabled", true },
					{ "hiZSelfOcclusionGuardEnabled", false },
					{ "debugVisualizationMode", static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Error) },
					{ "geometryBudgetBytes", 987654ull },
					{ "preloadBias", 2.0f }
				} }
			};
			std::ofstream output(root / "ProjectSettings" / "RenderSettings.json");
			output << document.dump(4);
			output.close();

			RenderSystem::Instance().LoadSettings();
			const VirtualGeometryQualityProfile loaded = RenderSystem::Instance().GetVirtualGeometryQualityProfile();
			context.Expect(NearlyEqual(3.0f, loaded.lodErrorScale), "Loaded render settings should restore LOD error scale.");
			context.ExpectEqual(2u, loaded.maxLodDrop, "Loaded render settings should restore max LOD drop.");
			context.Expect(loaded.hiZOcclusionEnabled, "Loaded render settings should restore the Hi-Z toggle.");
			context.Expect(!loaded.hiZSelfOcclusionGuardEnabled, "Loaded render settings should restore the self-occlusion guard toggle.");
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Error),
				static_cast<std::uint32_t>(loaded.debugVisualizationMode),
				"Loaded render settings should restore debug visualization mode.");
			context.ExpectEqual(987654ull, loaded.geometryBudgetBytes, "Loaded render settings should restore geometry budget.");
			context.Expect(NearlyEqual(2.0f, loaded.preloadBias), "Loaded render settings should restore preload bias.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			std::filesystem::remove_all(root);
		});
	}
}
