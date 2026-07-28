#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/RenderSystem.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
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

	VirtualGeometryAssetRuntime MakeMixedDistanceQualityAsset()
	{
		VirtualGeometryAssetRuntime asset{};
		asset.assetId = TerraEngine::GenerateUUID();
		asset.clusterCoverageValidated = true;
		asset.detailLevels = {
			{ 0u, 0u, 8u, 512u, 0.0f, 1.0f, glm::vec3(-2.0f), glm::vec3(2.0f) },
			{ 1u, 8u, 8u, 256u, 0.04f, 0.5f, glm::vec3(-2.0f), glm::vec3(2.0f) }
		};
		asset.cullableGroups.resize(16u);

		GeometryPageRuntime residentPage{};
		residentPage.pageId = 0u;
		residentPage.firstLodIndex = 0u;
		residentPage.lodCount = 2u;
		residentPage.groupOffset = 0u;
		residentPage.groupCount = 16u;
		residentPage.state = GeometryPageState::Resident;
		residentPage.fallbackPageId = 0u;
		residentPage.fallbackPage = true;
		asset.pages = { residentPage };

		VirtualGeometryClusterRuntime nearRoot{};
		nearRoot.clusterId = 0u;
		nearRoot.parentClusterId = kVirtualGeometryInvalidIndex;
		nearRoot.firstChildClusterId = 2u;
		nearRoot.childCount = 1u;
		nearRoot.lodIndex = 1u;
		nearRoot.firstGroupIndex = 8u;
		nearRoot.groupCount = 4u;
		nearRoot.pageId = 0u;
		nearRoot.fallbackClusterId = 0u;
		nearRoot.geometricError = 0.04f;
		nearRoot.boundsCenter = glm::vec3(-1.0f, 0.0f, -5.0f);
		nearRoot.boundsRadius = 1.0f;

		VirtualGeometryClusterRuntime farRoot = nearRoot;
		farRoot.clusterId = 1u;
		farRoot.firstChildClusterId = 3u;
		farRoot.firstGroupIndex = 12u;
		farRoot.boundsCenter = glm::vec3(1.0f, 0.0f, -120.0f);

		VirtualGeometryClusterRuntime nearChild{};
		nearChild.clusterId = 2u;
		nearChild.parentClusterId = 0u;
		nearChild.firstChildClusterId = kVirtualGeometryInvalidIndex;
		nearChild.childCount = 0u;
		nearChild.lodIndex = 0u;
		nearChild.firstGroupIndex = 0u;
		nearChild.groupCount = 4u;
		nearChild.materialSectionId = 0u;
		nearChild.pageId = 0u;
		nearChild.fallbackClusterId = 0u;
		nearChild.boundsCenter = nearRoot.boundsCenter;
		nearChild.boundsRadius = 1.0f;

		VirtualGeometryClusterRuntime farChild = nearChild;
		farChild.clusterId = 3u;
		farChild.parentClusterId = 1u;
		farChild.firstGroupIndex = 4u;
		farChild.boundsCenter = farRoot.boundsCenter;

		asset.clusters = { nearRoot, farRoot, nearChild, farChild };
		return asset;
	}

	VirtualGeometryAssetRuntime MakeThresholdTransitionAsset()
	{
		VirtualGeometryAssetRuntime asset{};
		asset.assetId = TerraEngine::GenerateUUID();
		asset.clusterCoverageValidated = true;
		asset.detailLevels = {
			{ 0u, 0u, 4u, 256u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
			{ 1u, 4u, 4u, 128u, 0.015f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
		};
		asset.cullableGroups.resize(8u);

		GeometryPageRuntime page{};
		page.pageId = 0u;
		page.firstLodIndex = 0u;
		page.lodCount = 2u;
		page.groupCount = 8u;
		page.state = GeometryPageState::Resident;
		page.fallbackPageId = 0u;
		page.fallbackPage = true;
		asset.pages = { page };

		VirtualGeometryClusterRuntime root{};
		root.clusterId = 0u;
		root.parentClusterId = kVirtualGeometryInvalidIndex;
		root.firstChildClusterId = 1u;
		root.childCount = 1u;
		root.lodIndex = 1u;
		root.firstGroupIndex = 4u;
		root.groupCount = 4u;
		root.pageId = 0u;
		root.fallbackClusterId = 0u;
		root.geometricError = 0.01f;
		root.boundsRadius = 1.0f;

		VirtualGeometryClusterRuntime child{};
		child.clusterId = 1u;
		child.parentClusterId = 0u;
		child.lodIndex = 0u;
		child.firstGroupIndex = 0u;
		child.groupCount = 4u;
		child.pageId = 0u;
		child.fallbackClusterId = 0u;
		child.boundsRadius = 1.0f;
		asset.clusters = { root, child };
		return asset;
	}

	bool SelectionContainsCluster(const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult& result, std::uint32_t clusterId)
	{
		return std::any_of(result.selectedClusters.begin(), result.selectedClusters.end(), [clusterId](const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& record) {
			return record.clusterId == clusterId;
		});
	}

	void RestoreRenderSettings(const std::filesystem::path& previousAssetRoot, const RenderSettings& previousSettings)
	{
		// Restore the in-memory renderer settings before returning the shared
		// ResourceManager to its earlier root. The write goes to the temporary
		// test project rather than the developer's active project settings.
		RenderSystem::Instance().ClearTransientRenderDiagnosticsOverride();
		RenderSystem::Instance().SetRenderSettings(previousSettings);
		ResourceManager::Instance().SetAssetRoot(previousAssetRoot);
	}
}

	void RegisterVirtualGeometryQualityTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Quality.DefaultsRetainLegacyHiZMetadata", [](TestContext& context) {
			// Hi-Z is now resource-gated by the renderer. These fields remain so
			// older project settings can round-trip without reintroducing a
			// runtime occlusion disable switch.
			const VirtualGeometryQualityProfile profile = MakeDefaultVirtualGeometryQualityProfile();

			context.Expect(profile.hiZOcclusionEnabled, "Default virtual geometry quality should preserve the legacy Hi-Z metadata value.");
			context.Expect(profile.hiZSelfOcclusionGuardEnabled, "Default virtual geometry quality should preserve legacy self-occlusion metadata.");
			context.Expect(profile.hiZNearPlaneGuardEnabled, "Default virtual geometry quality should preserve legacy near-plane metadata.");
			context.Expect(profile.hiZThinGeometryGuardEnabled, "Default virtual geometry quality should preserve legacy thin-geometry metadata.");
		});

		registry.Add("VirtualGeometry.Quality.SanitizesUnsafeControls", [](TestContext& context) {
			VirtualGeometryQualityProfile profile = MakeDefaultVirtualGeometryQualityProfile();
			profile.lodErrorScale = std::numeric_limits<float>::infinity();
			profile.preloadBias = std::numeric_limits<float>::quiet_NaN();
			profile.maxLodDrop = 7u;
			profile.hiZOcclusionEnabled = true;
			profile.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Residency;
			profile.geometryBudgetBytes = 64ull * 1024ull * 1024ull;
			profile.lodTransitionHysteresisPixels = std::numeric_limits<float>::quiet_NaN();
			profile.lodTransitionStableFrames = 0u;

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
			context.Expect(NearlyEqual(0.35f, sanitized.lodTransitionHysteresisPixels), "Non-finite transition hysteresis should fall back to the safe default.");
			context.ExpectEqual(1u, sanitized.lodTransitionStableFrames, "Transition stable-frame count should clamp to at least one frame.");

			profile.lodErrorScale = 0.001f;
			profile.preloadBias = 128.0f;
			profile.lodTransitionHysteresisPixels = 64.0f;
			profile.lodTransitionStableFrames = 128u;
			profile.debugVisualizationMode = static_cast<VirtualGeometryDebugVisualizationMode>(12u);
			sanitized = SanitizeVirtualGeometryQualityProfile(profile);
			context.Expect(NearlyEqual(0.05f, sanitized.lodErrorScale), "LOD error scale should clamp to the safe lower bound.");
			context.Expect(NearlyEqual(16.0f, sanitized.preloadBias), "Preload bias should clamp to the safe upper bound.");
			context.Expect(NearlyEqual(8.0f, sanitized.lodTransitionHysteresisPixels), "Transition hysteresis should clamp to the safe upper bound.");
			context.ExpectEqual(16u, sanitized.lodTransitionStableFrames, "Transition stable-frame count should clamp to the safe upper bound.");
			context.Expect(sanitized.debugVisualizationMode == VirtualGeometryDebugVisualizationMode::None, "Unknown debug visualization values should sanitize to Off.");
		});

		registry.Add("VirtualGeometry.Quality.LegacyHiZMetadataDoesNotAffectVisibilitySignature", [](TestContext& context) {
			VirtualGeometryQualityProfile baseProfile = MakeDefaultVirtualGeometryQualityProfile();
			const std::uint64_t baseSignature = BuildVirtualGeometryQualityProfileSignature(baseProfile);

			VirtualGeometryQualityProfile legacyChanged = baseProfile;
			legacyChanged.hiZOcclusionEnabled = false;
			legacyChanged.hiZSelfOcclusionGuardEnabled = false;
			legacyChanged.hiZNearPlaneGuardEnabled = false;
			legacyChanged.hiZThinGeometryGuardEnabled = false;

			context.ExpectEqual(baseSignature, BuildVirtualGeometryQualityProfileSignature(legacyChanged), "Legacy Hi-Z metadata must not rebuild visibility commands as if it were a runtime disable switch.");
		});

		registry.Add("VirtualGeometry.Quality.SavesProjectRenderSettings", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualitySaveTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");

			RenderSettings diagnosticSettings = RenderSystem::Instance().GetRenderSettings();
			diagnosticSettings.validationEnabled = 1u;
			diagnosticSettings.debugEnabled = 1u;
			RenderSystem::Instance().SetRenderSettings(diagnosticSettings);

			VirtualGeometryQualityProfile profile = MakeDefaultVirtualGeometryQualityProfile();
			profile.lodErrorScale = 2.5f;
			profile.maxLodDrop = 3u;
			profile.hiZOcclusionEnabled = true;
			profile.hiZSelfOcclusionGuardEnabled = false;
			profile.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;
			profile.geometryBudgetBytes = 123456u;
			profile.preloadBias = 4.0f;
			profile.lodTransitionHysteresisPixels = 0.5f;
			profile.lodTransitionStableFrames = 4u;
			RenderSystem::Instance().SetVirtualGeometryQualityProfile(profile);

			std::ifstream input(root / "ProjectSettings" / "RenderSettings.json");
			nlohmann::json document;
			input >> document;
			input.close();
			const nlohmann::json& virtualGeometry = document["settings"]["virtualGeometry"];

			context.Expect(document.value("validationEnabled", false), "Saved render settings should expose Vulkan validation as a top-level startup flag.");
			context.Expect(document.value("debugEnabled", false), "Saved render settings should expose render debug diagnostics as a top-level startup flag.");
			context.Expect(!virtualGeometry.contains("enabled"), "Saved render settings should not include a virtual geometry enable toggle.");
			context.Expect(NearlyEqual(2.5f, virtualGeometry.value("lodErrorScale", 0.0f)), "Saved render settings should include LOD error scale.");
			context.ExpectEqual(3u, virtualGeometry.value("maxLodDrop", 0u), "Saved render settings should include max LOD drop.");
			context.Expect(virtualGeometry.value("hiZOcclusionEnabled", false), "Saved render settings should preserve legacy Hi-Z metadata.");
			context.Expect(!virtualGeometry.value("hiZSelfOcclusionGuardEnabled", true), "Saved render settings should preserve legacy self-occlusion metadata.");
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::HiZ),
				virtualGeometry.value("debugVisualizationMode", 0u),
				"Saved render settings should include debug visualization mode.");
			context.ExpectEqual(123456ull, virtualGeometry.value("geometryBudgetBytes", 0ull), "Saved render settings should include geometry budget.");
			context.Expect(NearlyEqual(4.0f, virtualGeometry.value("preloadBias", 0.0f)), "Saved render settings should include preload bias.");
			context.Expect(NearlyEqual(0.5f, virtualGeometry.value("lodTransitionHysteresisPixels", 0.0f)), "Saved render settings should include transition hysteresis.");
			context.ExpectEqual(4u, virtualGeometry.value("lodTransitionStableFrames", 0u), "Saved render settings should include transition stable frames.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Quality.TransientRenderDiagnosticsDoNotSaveProjectSettings", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const bool previousDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualityTransientDiagnosticsTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");
			RenderSystem::Instance().ClearTransientRenderDiagnosticsOverride();

			RenderSettings savedSettings = previousSettings;
			savedSettings.validationEnabled = 0u;
			savedSettings.debugEnabled = 0u;
			RenderSystem::Instance().SetRenderSettings(savedSettings);
			RenderSystem::Instance().ApplyTransientRenderDiagnostics(true, true);

			const std::filesystem::path settingsPath = root / "ProjectSettings" / "RenderSettings.json";
			std::ifstream transientInput(settingsPath);
			nlohmann::json transientDocument;
			transientInput >> transientDocument;
			transientInput.close();

			context.Expect(ResourceManager::Instance().IsRenderValidationEnabled(), "Transient editor validation should update the live validation request.");
			context.Expect(ResourceManager::Instance().IsRenderDebugEnabled(), "Transient editor diagnostics should update the live debug flag.");
			context.Expect(TerraEngine::Renderer::RenderDiagnosticsRequested(), "Transient editor diagnostics should request render diagnostics immediately.");
			context.Expect(!transientDocument.value("validationEnabled", true), "Transient editor validation must not rewrite project RenderSettings.json.");
			context.Expect(!transientDocument.value("debugEnabled", true), "Transient editor diagnostics must not rewrite project RenderSettings.json.");

			savedSettings.shadowResolution += 128u;
			RenderSystem::Instance().SetRenderSettings(savedSettings);
			std::ifstream projectSaveInput(settingsPath);
			nlohmann::json projectSaveDocument;
			projectSaveInput >> projectSaveDocument;
			projectSaveInput.close();
			context.Expect(!projectSaveDocument.value("validationEnabled", true), "Later project graphics saves should not inherit transient validation preferences.");
			context.Expect(!projectSaveDocument.value("debugEnabled", true), "Later project graphics saves should not inherit transient debug preferences.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(previousDiagnosticsRequested);
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Quality.RenderDebugDiagnosticsApplyLive", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const bool previousDiagnosticsRequested = TerraEngine::Renderer::RenderDiagnosticsRequested();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualityLiveDebugTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");

			// The debug flag is user-authored project state, but the editor also
			// needs the running renderer to honor it immediately. These checks
			// prove both the saved setting and temporary diagnostics windows feed
			// the same runtime request without overriding each other.
			RenderSettings settings = previousSettings;
			settings.debugEnabled = 0u;
			RenderSystem::Instance().SetRenderSettings(settings);
			RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
			context.Expect(!ResourceManager::Instance().IsRenderDebugEnabled(), "Disabling render debug diagnostics should update ResourceManager immediately.");
			context.Expect(!TerraEngine::Renderer::RenderDiagnosticsRequested(), "Disabling render debug diagnostics should clear the runtime request when no editor window needs it.");

			settings.debugEnabled = 1u;
			RenderSystem::Instance().SetRenderSettings(settings);
			context.Expect(ResourceManager::Instance().IsRenderDebugEnabled(), "Enabling render debug diagnostics should update ResourceManager immediately.");
			context.Expect(TerraEngine::Renderer::RenderDiagnosticsRequested(), "Enabling render debug diagnostics should request detailed render diagnostics without restarting.");

			settings.debugEnabled = 0u;
			RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(true);
			RenderSystem::Instance().SetRenderSettings(settings);
			context.Expect(!ResourceManager::Instance().IsRenderDebugEnabled(), "Project debug state should still turn off while a diagnostics window is open.");
			context.Expect(TerraEngine::Renderer::RenderDiagnosticsRequested(), "Temporary diagnostics windows should keep diagnostics active after the project toggle is disabled.");

			RenderSystem::Instance().SetRenderGraphDiagnosticsRequested(false);
			context.Expect(!TerraEngine::Renderer::RenderDiagnosticsRequested(), "Closing the last diagnostics window should clear diagnostics when the project toggle is off.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			TerraEngine::Renderer::SetRenderDiagnosticsRequested(previousDiagnosticsRequested);
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
			document["validationEnabled"] = true;
			document["debugEnabled"] = true;
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
					{ "preloadBias", 2.0f },
					{ "lodTransitionHysteresisPixels", 0.65f },
					{ "lodTransitionStableFrames", 5u }
				} }
			};
			std::ofstream output(root / "ProjectSettings" / "RenderSettings.json");
			output << document.dump(4);
			output.close();

			RenderSystem::Instance().LoadSettings();
			const RenderSettings loadedSettings = RenderSystem::Instance().GetRenderSettings();
			const VirtualGeometryQualityProfile loaded = RenderSystem::Instance().GetVirtualGeometryQualityProfile();
			context.ExpectEqual(1u, loadedSettings.validationEnabled, "Loaded render settings should restore Vulkan validation startup flag.");
			context.ExpectEqual(1u, loadedSettings.debugEnabled, "Loaded render settings should restore render debug startup flag.");
			context.Expect(ResourceManager::Instance().IsRenderDebugEnabled(), "Loaded render debug diagnostics should apply to the live ResourceManager flag.");
			context.Expect(TerraEngine::Renderer::RenderDiagnosticsRequested(), "Loaded render debug diagnostics should request detailed diagnostics without requiring another restart.");
			context.Expect(NearlyEqual(3.0f, loaded.lodErrorScale), "Loaded render settings should restore LOD error scale.");
			context.ExpectEqual(2u, loaded.maxLodDrop, "Loaded render settings should restore max LOD drop.");
			context.Expect(loaded.hiZOcclusionEnabled, "Loaded render settings should restore legacy Hi-Z metadata.");
			context.Expect(!loaded.hiZSelfOcclusionGuardEnabled, "Loaded render settings should restore legacy self-occlusion metadata.");
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::Error),
				static_cast<std::uint32_t>(loaded.debugVisualizationMode),
				"Loaded render settings should restore debug visualization mode.");
			context.ExpectEqual(987654ull, loaded.geometryBudgetBytes, "Loaded render settings should restore geometry budget.");
			context.Expect(NearlyEqual(2.0f, loaded.preloadBias), "Loaded render settings should restore preload bias.");
			context.Expect(NearlyEqual(0.65f, loaded.lodTransitionHysteresisPixels), "Loaded render settings should restore transition hysteresis.");
			context.ExpectEqual(5u, loaded.lodTransitionStableFrames, "Loaded render settings should restore transition stable frames.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Quality.LoadsMissingLegacyHiZMetadataAsDefaultOn", [](TestContext& context) {
			const std::filesystem::path previousAssetRoot = ResourceManager::Instance().GetAssetRoot();
			const RenderSettings previousSettings = RenderSystem::Instance().GetRenderSettings();
			const std::filesystem::path root = MakeVirtualGeometryQualityTestRoot("TerraEngineVirtualGeometryQualityMissingHiZTests");
			ResourceManager::Instance().SetAssetRoot(root / "Assets");
			std::filesystem::create_directories(root / "ProjectSettings");

			// This shape matches an older RenderSettings file that already had
			// the virtualGeometry block but not the compatibility Hi-Z metadata.
			nlohmann::json document;
			document["preset"] = static_cast<std::uint32_t>(QualityPreset::Custom);
			document["settings"] = {
				{ "virtualGeometry", {
					{ "lodErrorScale", 1.0f },
					{ "maxLodDrop", 0u },
					{ "debugVisualizationMode", static_cast<std::uint32_t>(VirtualGeometryDebugVisualizationMode::None) }
				} }
			};
			std::ofstream output(root / "ProjectSettings" / "RenderSettings.json");
			output << document.dump(4);
			output.close();

			RenderSystem::Instance().LoadSettings();
			const VirtualGeometryQualityProfile loaded = RenderSystem::Instance().GetVirtualGeometryQualityProfile();

			context.Expect(loaded.hiZOcclusionEnabled, "Missing legacy Hi-Z metadata should inherit the default value.");

			RestoreRenderSettings(previousAssetRoot, previousSettings);
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Quality.SelectsMixedDetailForNearAndFarClusters", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeMixedDistanceQualityAsset();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.lodErrorScale = 1.0f;

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					TerraEngine::GenerateUUID(),
					TerraEngine::GenerateUUID(),
					quality,
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(1000u, 1000u),
					true,
					true);

			context.ExpectEqual(static_cast<std::size_t>(2u), result.selectedClusters.size(), "Mixed-distance fixture should select one cluster for each source region.");
			context.Expect(SelectionContainsCluster(result, 2u), "Nearby region should refine to the higher-detail child cluster.");
			context.Expect(SelectionContainsCluster(result, 1u), "Farther region should remain on its coarser root cluster.");
			context.Expect(!SelectionContainsCluster(result, 0u), "Refined near region should not also dispatch the overlapping parent cluster.");
			context.Expect(!SelectionContainsCluster(result, 3u), "Far region should avoid an unnecessary high-detail child when its screen error is below the refinement threshold.");
		});

		registry.Add("VirtualGeometry.Quality.DoesNotSubstituteWholeObjectLodForMixedClusterCut", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeMixedDistanceQualityAsset();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.lodErrorScale = 1.0f;

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					TerraEngine::GenerateUUID(),
					TerraEngine::GenerateUUID(),
					quality,
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(1000u, 1000u),
					true,
					true);

			bool sawHighDetailCluster = false;
			bool sawCoarseCluster = false;
			for (const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& cluster : result.selectedClusters)
			{
				sawHighDetailCluster = sawHighDetailCluster || cluster.lodIndex == 0u;
				sawCoarseCluster = sawCoarseCluster || cluster.lodIndex == 1u;
			}

			context.ExpectEqual(static_cast<std::size_t>(2u), result.selectedClusters.size(), "Mixed-detail quality selection should remain sparse instead of collapsing to one whole-object range.");
			context.Expect(sawHighDetailCluster && sawCoarseCluster, "The selected cut should contain both near high-detail and far coarse clusters.");
			context.Expect(result.range.selectedClusterCount == 2u, "Draw range metadata should point at the sparse selected-cluster payload.");
			context.Expect(SelectionContainsCluster(result, 2u), "Sparse dispatch should include the nearby high-detail cluster.");
			context.Expect(SelectionContainsCluster(result, 1u), "Sparse dispatch should include the far coarse cluster.");
			context.Expect(!SelectionContainsCluster(result, 0u), "Sparse dispatch should not also include the whole nearby parent cluster.");
			context.Expect(!SelectionContainsCluster(result, 3u), "Sparse dispatch should not force the far region into its high-detail child.");
		});

		registry.Add("VirtualGeometry.Quality.HoldsStableCutNearStationaryThreshold", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeThresholdTransitionAsset();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.lodTransitionHysteresisPixels = 0.35f;
			quality.lodTransitionStableFrames = 3u;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::VirtualGeometryClusterSelectionResult previous =
				RenderSystem::SelectVirtualGeometryStableClusterCut(
					asset,
					meshId,
					objectId,
					quality,
					146.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(0u),
					true,
					true,
					nullptr,
					RenderViewType::Game,
					2u);

			std::uint32_t selectionSwitches = 0u;
			std::uint32_t heldFrames = 0u;
			std::uint32_t previousClusterId = previous.selectedClusters.empty() ? 0xffffffffu : previous.selectedClusters.front().clusterId;
			for (std::uint32_t frame = 0u; frame < 300u; ++frame)
			{
				const float projectedRadius = (frame % 2u) == 0u ? 154.0f : 146.0f;
				TerraEngine::Renderer::VirtualGeometryClusterSelectionResult current =
					RenderSystem::SelectVirtualGeometryStableClusterCut(
						asset,
						meshId,
						objectId,
						quality,
						projectedRadius,
						glm::mat4(1.0f),
						glm::vec3(0.0f),
						glm::uvec2(0u),
						true,
						true,
						&previous,
						RenderViewType::Game,
						2u);
				const std::uint32_t currentClusterId = current.selectedClusters.empty() ? 0xffffffffu : current.selectedClusters.front().clusterId;
				if (currentClusterId != previousClusterId)
				{
					++selectionSwitches;
				}
				if (current.transitionState.heldPreviousSelection)
				{
					++heldFrames;
				}
				previousClusterId = currentClusterId;
				previous = current;
			}

			context.ExpectEqual(0u, selectionSwitches, "Alternating threshold jitter should not flip selected cluster cuts while inside the hysteresis band.");
			context.Expect(heldFrames > 0u, "Near-threshold candidates should report held transition frames for diagnostics.");
			context.ExpectEqual(0u, previousClusterId, "The coarse root should remain the stable cut during repeated threshold jitter.");
		});
	}
}
