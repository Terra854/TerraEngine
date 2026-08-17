// TerrainAssetDatabaseTests.cpp — Asset Database ownership checks for terrain bundles.

#include "TestHarness.h"

#include "AssetDatabase.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainAssetDatabaseTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Foundation.ManagedDataSubtreeHasOneParentAsset", [](TestContext& context) {
			const std::filesystem::path root = std::filesystem::temp_directory_path() /
				("TerraEngineTerrainAssetDatabase_" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			const std::filesystem::path terrainRoot = root / "Assets" / "Terrain" / "Landscape.teterrain";
			const std::filesystem::path privateBlob = std::filesystem::path(terrainRoot.string() + ".data") / "blobs" / "first.tearea";
			std::error_code error;
			std::filesystem::remove_all(root, error);
			std::filesystem::create_directories(privateBlob.parent_path(), error);
			std::ofstream(terrainRoot, std::ios::binary | std::ios::trunc) << "{}";
			std::ofstream(privateBlob, std::ios::binary | std::ios::trunc) << "private terrain area";

			AssetDatabase::Instance().Shutdown();
			AssetDatabase::Instance().Initialize(root);
			AssetDatabase::Instance().ScanProject();
			const auto& assets = AssetDatabase::Instance().GetAssets();
			context.Expect(assets.contains(std::filesystem::absolute(terrainRoot)),
				"The .teterrain root must be discovered as the one editable project asset.");
			context.Expect(!assets.contains(std::filesystem::absolute(privateBlob)),
				"Managed terrain blobs must never receive independent Asset Database identities.");
			context.Expect(!std::filesystem::exists(privateBlob.string() + ".meta"),
				"Scanning a terrain data subtree must not create metadata beside private blobs.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root, error);
		});

		registry.Add("Terrain.Layers.MaterialChangesPreserveWeightIdentityAndReportFallbackEvents", [](TestContext& context) {
			const std::filesystem::path root = std::filesystem::temp_directory_path() /
				("TerraEngineTerrainMaterialJournal_" + std::to_string(
					std::chrono::steady_clock::now().time_since_epoch().count()));
			const std::filesystem::path original = root / "Assets" / "Materials" / "Rock.tmat";
			const std::filesystem::path renamed = root / "Assets" / "Materials" / "CliffRock.tmat";
			std::error_code error;
			std::filesystem::remove_all(root, error);
			std::filesystem::create_directories(original.parent_path(), error);
			std::ofstream(original, std::ios::binary | std::ios::trunc) << "{}";

			AssetDatabase::Instance().Shutdown();
			AssetDatabase::Instance().Initialize(root);
			AssetDatabase::Instance().ImportAsset(original);
			const auto first = AssetDatabase::Instance().GetTerrainMaterialChangesSince(0u);
			context.Expect(!first.changes.empty() && first.changes.back().kind == TerrainMaterialChangeKind::Imported,
				"Initial material registration must publish an observable terrain material generation.");
			const auto originalMetadata = AssetDatabase::Instance().FindAssetByGuid(first.changes.back().materialAssetId);
			const TerraEngine::UUID authoredLayerMaterial = originalMetadata ? originalMetadata->guid : TerraEngine::EMPTY_UUID;

			AssetDatabase::Instance().ImportAsset(original, true);
			const auto reimported = AssetDatabase::Instance().GetTerrainMaterialChangesSince(first.generation);
			context.Expect(!reimported.changes.empty() && reimported.changes.back().kind == TerrainMaterialChangeKind::Reimported,
				"Reimport must publish a new generation without changing the authored material UUID.");

			std::filesystem::rename(original, renamed, error);
			std::filesystem::rename(original.string() + ".meta", renamed.string() + ".meta", error);
			AssetDatabase::Instance().ImportAsset(renamed, true);
			const auto renamedEvents = AssetDatabase::Instance().GetTerrainMaterialChangesSince(reimported.generation);
			context.Expect(!renamedEvents.changes.empty() && renamedEvents.changes.back().kind == TerrainMaterialChangeKind::Renamed
				&& renamedEvents.changes.back().materialAssetId == authoredLayerMaterial,
				"Rename must retain the UUID referenced by authored terrain weights and report a new observation generation.");

			AssetDatabase::Instance().DeleteAsset(renamed);
			const auto deleted = AssetDatabase::Instance().GetTerrainMaterialChangesSince(renamedEvents.generation);
			context.Expect(!deleted.changes.empty() && deleted.changes.back().kind == TerrainMaterialChangeKind::Deleted
				&& deleted.changes.back().materialAssetId == authoredLayerMaterial,
				"Deletion must emit fallback diagnostics rather than rewriting terrain-authored material identities.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root, error);
		});
	}
}
