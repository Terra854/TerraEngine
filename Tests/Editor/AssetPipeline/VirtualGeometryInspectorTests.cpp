#include "TestHarness.h"

#include "AssetImportResult.h"
#include "AssetDatabase.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "ModelPackageImporter.h"
#include "Utilities/UUID.h"

#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path MakeVirtualGeometryInspectorProjectRoot()
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryInspectorTests";
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root / "Assets");
			std::filesystem::create_directories(root / "Build" / "Assets");
			return root;
		}

		void WriteMeshHeader(const std::filesystem::path& path, bool cookedVirtualGeometry)
		{
			MeshHeader header{};
			std::memcpy(header.magic, "TEMF", 4);
			header.version = kMeshCurrentVersion;
			header.headerSize = sizeof(MeshHeader);
			header.meshletCount = 12u;
			if (cookedVirtualGeometry)
			{
				header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
				header.virtualGeometryHeaderOffset = sizeof(MeshHeader);
				header.virtualGeometryHeaderSize = sizeof(VirtualGeometryHeader);
				header.virtualGeometryPayloadGeneration = kVirtualGeometryPayloadGeneration;
			}
			else
			{
				header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
				header.virtualGeometryHeaderOffset = 0u;
				header.virtualGeometryHeaderSize = 0u;
				header.virtualGeometryPayloadGeneration = 0u;
			}

			std::ofstream output(path, std::ios::binary);
			output.write(reinterpret_cast<const char*>(&header), sizeof(header));
			if (cookedVirtualGeometry)
			{
				VirtualGeometryHeader virtualHeader{};
				virtualHeader.payloadGeneration = kVirtualGeometryPayloadGeneration;
				virtualHeader.headerSize = sizeof(VirtualGeometryHeader);
				virtualHeader.detailLevelCount = 3u;
				virtualHeader.cullableGroupCount = 12u;
				virtualHeader.clusterCount = 12u;
				virtualHeader.coneSafetyCount = 12u;
				virtualHeader.coneSafetyTableOffset = sizeof(MeshHeader) + sizeof(VirtualGeometryHeader);
				virtualHeader.flags = kVirtualGeometryValidationFlagConeSafetyMetadataValidated;
				virtualHeader.sourceTriangleCount = 2048u;
				output.write(reinterpret_cast<const char*>(&virtualHeader), sizeof(virtualHeader));
				for (std::uint32_t clusterIndex = 0u; clusterIndex < virtualHeader.coneSafetyCount; ++clusterIndex)
				{
					VirtualGeometryConeSafetyRecord coneSafety{};
					coneSafety.clusterId = clusterIndex;
					coneSafety.groupOffset = clusterIndex;
					coneSafety.groupCount = 1u;
					coneSafety.materialSidedness = 0u;
					coneSafety.windingState = 0u;
					coneSafety.alphaRisk = 0u;
					coneSafety.thinShellRisk = 0u;
					coneSafety.flags = kVirtualGeometryConeSafetyFlagConeValid | kVirtualGeometryConeSafetyFlagEligible;
					output.write(reinterpret_cast<const char*>(&coneSafety), sizeof(coneSafety));
				}
			}
		}

		std::string ReadBinaryFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return std::string(
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>());
		}

		void WriteTinyObj(const std::filesystem::path& path)
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::trunc);
			output << "v 0 0 0\n";
			output << "v 1 0 0\n";
			output << "v 0 1 0\n";
			output << "vt 0 0\n";
			output << "vt 1 0\n";
			output << "vt 0 1\n";
			output << "f 1/1 2/2 3/3\n";
		}

		void WriteMaterialAsset(const std::filesystem::path& path, std::string_view name)
		{
			std::filesystem::create_directories(path.parent_path());
			nlohmann::json material;
			material["name"] = std::string(name);
			material["shader"] = "";
			material["textures"] = nlohmann::json::object();
			std::ofstream output(path, std::ios::trunc);
			output << material.dump(4);
		}

		void WriteAssetMetadata(
			const std::filesystem::path& assetRoot,
			const std::filesystem::path& sourcePath,
			const std::filesystem::path& compiledPath,
			const TerraEngine::UUID& guid,
			const std::string& importerName)
		{
			const std::filesystem::path fullSourcePath = assetRoot / sourcePath;
			std::filesystem::create_directories(fullSourcePath.parent_path());

			nlohmann::json metadata;
			metadata["guid"] = TerraEngine::ToHexString(guid);
			metadata["importerName"] = importerName;
			metadata["lastModified"] = std::filesystem::last_write_time(fullSourcePath).time_since_epoch().count();
			metadata["sourcePath"] = sourcePath.generic_string();
			metadata["compiledPath"] = compiledPath.generic_string();
			metadata["settings"] = nlohmann::json::object();

			// Tests write the same metadata shape the editor persists so the
			// startup scan exercises the public asset-database path.
			std::ofstream output(fullSourcePath.string() + ".meta", std::ios::trunc);
			output << metadata.dump(4);
		}
	}

	void RegisterVirtualGeometryInspectorTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.AssetDatabase.ReloadsFromStartupIndexWithoutMetaScan", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path sourcePath = assetRoot / "CachedMaterial.tmat";
			const std::filesystem::path compiledPath = buildRoot / "CachedMaterial.tmat";
			const TerraEngine::UUID guid = TerraEngine::GenerateUUID();

			WriteMaterialAsset(sourcePath, "CachedMaterial");
			WriteMaterialAsset(compiledPath, "CachedMaterialCompiled");
			WriteAssetMetadata(assetRoot, "CachedMaterial.tmat", "CachedMaterial.tmat", guid, "MaterialImporter");

			AssetDatabase::Instance().Initialize(root);
			context.Expect(std::filesystem::exists(root / "Library" / "AssetIndex.json"), "Initial scan should persist a startup asset index.");
			AssetDatabase::Instance().Shutdown();

			std::filesystem::remove(sourcePath.string() + ".meta");
			AssetDatabase::Instance().Initialize(root);
			const AssetMetadata* indexedMetadata = AssetDatabase::Instance().FindAssetByGuid(guid);
			context.Expect(indexedMetadata != nullptr, "Startup should restore known assets from AssetIndex.json before the reconcile scan runs.");
			context.ExpectEqual(std::filesystem::path("CachedMaterial.tmat"), indexedMetadata->sourcePath, "Indexed metadata should preserve the original source path.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.AssetDatabase.BackgroundReconcileFindsNewFilesAfterIndexedStartup", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const TerraEngine::UUID indexedGuid = TerraEngine::GenerateUUID();

			WriteMaterialAsset(assetRoot / "Indexed.tmat", "Indexed");
			WriteMaterialAsset(buildRoot / "Indexed.tmat", "IndexedCompiled");
			WriteAssetMetadata(assetRoot, "Indexed.tmat", "Indexed.tmat", indexedGuid, "MaterialImporter");
			AssetDatabase::Instance().Initialize(root);
			AssetDatabase::Instance().Shutdown();

			WriteMaterialAsset(assetRoot / "DiscoveredLater.tmat", "DiscoveredLater");
			context.Expect(std::filesystem::exists(assetRoot / "DiscoveredLater.tmat"), "New material source should exist before indexed startup reconcile.");
			AssetDatabase::Instance().Initialize(root);

			bool discovered = false;
			for (int attempt = 0; attempt < 80 && !discovered; ++attempt) {
				AssetDatabase::Instance().Update();
				for (const auto& [path, metadata] : AssetDatabase::Instance().GetAssets()) {
					if (metadata.sourcePath == std::filesystem::path("DiscoveredLater.tmat")) {
						discovered = true;
						break;
					}
				}
				if (!discovered) {
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
			}

			context.Expect(discovered, "Background reconcile should import new files that were not present in the startup index.");
			context.Expect(AssetDatabase::Instance().FindAssetByGuid(indexedGuid) != nullptr, "Indexed assets should remain registered while reconcile catches up.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.ReadsCookedMetadata", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "mesh.temf";
			WriteMeshHeader(compiledPath, true);

			AssetMetadata metadata{};
			metadata.compiledPath = "mesh.temf";
			VirtualGeometryAssetMetadata virtualGeometry{};
			const bool readable = AssetDatabase::Instance().TryGetVirtualGeometryMetadata(metadata, virtualGeometry);

			context.Expect(readable, "Inspector metadata accessor should read a valid compiled TEMF header.");
			context.Expect(virtualGeometry.hasVirtualGeometry, "Cooked virtual geometry should be reported as present.");
			context.Expect(!virtualGeometry.needsRebuild, "Current payload generation should not request a rebuild.");
			context.ExpectEqual(3u, virtualGeometry.detailLevelCount, "Inspector metadata should expose the cooked detail count.");
			context.ExpectEqual(2048u, virtualGeometry.sourceTriangleCount, "Inspector metadata should expose source triangle count.");
			context.Expect(virtualGeometry.warnings.empty(), "Current cooked metadata should not surface warning rows.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.SurfacesRebuildWarnings", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "stale.temf";
			WriteMeshHeader(compiledPath, false);

			AssetMetadata metadata{};
			metadata.compiledPath = "stale.temf";
			VirtualGeometryAssetMetadata virtualGeometry{};
			const bool readable = AssetDatabase::Instance().TryGetVirtualGeometryMetadata(metadata, virtualGeometry);

			context.Expect(readable, "Stale virtual geometry headers should remain readable for inspector guidance.");
			context.Expect(virtualGeometry.hasVirtualGeometry, "Stale virtual geometry flags should still be visible to the inspector.");
			context.Expect(virtualGeometry.needsRebuild, "Incomplete virtual geometry metadata should request a rebuild.");
			context.Expect(!virtualGeometry.warnings.empty(), "Stale metadata should surface a visible warning.");
			context.Expect(virtualGeometry.rebuildGuidance.find("Rebuild") != std::string::npos, "Stale metadata should include explicit rebuild guidance for the inspector.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.AssetDatabase.PreservesCompiledMeshOnFailedRebuild", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path sourcePath = AssetDatabase::Instance().GetAssetRoot() / "broken.obj";
			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "broken.temf";
			std::ofstream source(sourcePath, std::ios::trunc);
			source << "this is not a valid obj mesh";
			source.close();
			std::ofstream compiled(compiledPath, std::ios::binary | std::ios::trunc);
			compiled << "previous usable compiled mesh";
			compiled.close();
			const std::string before = ReadBinaryFile(compiledPath);

			AssetDatabase::Instance().ImportAsset(sourcePath, true);
			const std::string after = ReadBinaryFile(compiledPath);

			context.ExpectEqual(before, after, "Failed mesh rebuilds should leave the previous compiled artifact untouched.");
			context.Expect(!std::filesystem::exists(compiledPath.string() + ".previous"), "Successful rollback should not leave a publish backup beside the compiled mesh.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.SavesLoadsAndResetsLooseMeshOverrides", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path sourcePath = AssetDatabase::Instance().GetAssetRoot() / "override.obj";
			WriteTinyObj(sourcePath);
			AssetDatabase::Instance().ImportAsset(sourcePath, true);

			const auto& assets = AssetDatabase::Instance().GetAssets();
			const auto assetIt = assets.find(std::filesystem::absolute(sourcePath));
			context.Expect(assetIt != assets.end(), "Imported mesh should be registered for inspector override tests.");

			VirtualGeometryAdaptiveSettingsMetadata settings = AssetDatabase::Instance().GetDefaultVirtualGeometryAdaptiveSettings();
			settings.targetRatios = { 1.0f, 0.5f, 0.25f };
			settings.minimumSourceTriangles = 24u;
			settings.minimumGeneratedTriangles = 8u;
			std::string error;
			context.Expect(AssetDatabase::Instance().SaveVirtualGeometryPerMeshOverride(assetIt->second.guid, settings, &error), error.empty() ? "Per-mesh override should save." : error);

			VirtualGeometryAdaptiveSettingsMetadata loaded;
			context.Expect(AssetDatabase::Instance().TryGetVirtualGeometryAdaptiveSettings(assetIt->second, loaded), "Saved mesh override should resolve.");
			context.Expect(loaded.overridePresent, "Resolved settings should report a per-mesh override.");
			context.ExpectEqual(static_cast<std::size_t>(3u), loaded.targetRatios.size(), "Override target ratios should round-trip in memory.");
			context.ExpectEqual(24u, loaded.minimumSourceTriangles, "Override minimum source triangles should round-trip in memory.");

			AssetDatabase::Instance().Shutdown();
			AssetDatabase::Instance().Initialize(root);
			const auto& reloadedAssets = AssetDatabase::Instance().GetAssets();
			const auto reloadedIt = reloadedAssets.find(std::filesystem::absolute(sourcePath));
			context.Expect(reloadedIt != reloadedAssets.end(), "Mesh metadata should reload from disk.");
			context.Expect(AssetDatabase::Instance().HasVirtualGeometryPerMeshOverride(reloadedIt->second), "Override should persist in the .meta file.");

			context.Expect(AssetDatabase::Instance().ClearVirtualGeometryPerMeshOverride(reloadedIt->second.guid, &error), error.empty() ? "Override should reset." : error);
			VirtualGeometryAdaptiveSettingsMetadata resetSettings;
			context.Expect(AssetDatabase::Instance().TryGetVirtualGeometryAdaptiveSettings(reloadedIt->second, resetSettings), "Reset mesh settings should resolve to defaults.");
			context.Expect(!AssetDatabase::Instance().HasVirtualGeometryPerMeshOverride(reloadedIt->second), "Reset should remove the .meta override block.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.ShowsOverrideControlsOnlyForMeshAssets", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			AssetMetadata meshMetadata{};
			meshMetadata.importerName = "MeshImporter";
			AssetMetadata packageMetadata{};
			packageMetadata.importerName = "GltfPackageImporter";
			AssetMetadata textureMetadata{};
			textureMetadata.importerName = "TextureImporter";

			context.Expect(AssetDatabase::Instance().CanEditVirtualGeometryPerMeshOverride(meshMetadata), "Loose imported meshes should expose Inspector override controls.");
			context.Expect(!AssetDatabase::Instance().CanEditVirtualGeometryPerMeshOverride(packageMetadata), "Package source assets should not expose per-mesh controls directly.");
			context.Expect(!AssetDatabase::Instance().CanEditVirtualGeometryPerMeshOverride(textureMetadata), "Non-mesh assets should not expose virtual geometry override controls.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.ReadsRequestedTargetCookDiagnostics", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path sourcePath = AssetDatabase::Instance().GetAssetRoot() / "diagnostics.obj";
			WriteTinyObj(sourcePath);
			AssetDatabase::Instance().ImportAsset(sourcePath, true);
			const auto& assets = AssetDatabase::Instance().GetAssets();
			const auto assetIt = assets.find(std::filesystem::absolute(sourcePath));
			context.Expect(assetIt != assets.end(), "Imported mesh should be registered before reading cook diagnostics.");

			std::vector<VirtualGeometryDetailDiagnosticMetadata> diagnostics;
			context.Expect(AssetDatabase::Instance().TryGetVirtualGeometryDetailDiagnostics(assetIt->second, diagnostics), "Cook diagnostics should be queryable for mesh assets.");
			context.Expect(!diagnostics.empty(), "Cook diagnostics should include at least one requested target row.");
			context.ExpectEqual(0u, diagnostics.front().targetIndex, "First cook diagnostic should describe LOD0/source target.");
			context.ExpectEqual(std::string("Generated"), diagnostics.front().status, "LOD0/source target should be reported as generated.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.RebuildsWithCurrentSettingsAndResetsToDefaults", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path sourcePath = AssetDatabase::Instance().GetAssetRoot() / "rebuild.obj";
			WriteTinyObj(sourcePath);
			AssetDatabase::Instance().ImportAsset(sourcePath, true);
			const auto& assets = AssetDatabase::Instance().GetAssets();
			const auto assetIt = assets.find(std::filesystem::absolute(sourcePath));
			context.Expect(assetIt != assets.end(), "Imported mesh should be registered before rebuild.");

			VirtualGeometryAdaptiveSettingsMetadata settings = AssetDatabase::Instance().GetDefaultVirtualGeometryAdaptiveSettings();
			settings.targetRatios = { 1.0f, 0.5f };
			settings.minimumSourceTriangles = 16u;
			std::string error;
			context.Expect(AssetDatabase::Instance().SaveVirtualGeometryPerMeshOverride(assetIt->second.guid, settings, &error), error.empty() ? "Override should save before rebuild." : error);
			context.Expect(AssetDatabase::Instance().RebuildVirtualGeometryAsset(assetIt->second.guid, &error), error.empty() ? "Rebuild should route through AssetDatabase import." : error);
			context.Expect(AssetDatabase::Instance().HasVirtualGeometryPerMeshOverride(assetIt->second), "Rebuild should preserve the current override metadata.");
			context.Expect(AssetDatabase::Instance().ClearVirtualGeometryPerMeshOverride(assetIt->second.guid, &error), error.empty() ? "Reset should clear override after rebuild." : error);
			context.Expect(!AssetDatabase::Instance().HasVirtualGeometryPerMeshOverride(assetIt->second), "Reset-to-defaults should remove override metadata.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.GeneratedMeshOverridesRoundTripThroughSidecar", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path packagePath = AssetDatabase::Instance().GetAssetRoot() / "package.glb";
			std::ofstream(packagePath, std::ios::binary | std::ios::trunc).put('\0');
			TerraEngine::AssetPipeline::ModelPackageImportResult result{};
			result.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			result.packageGuid = TerraEngine::GenerateUUID();
			result.package.sourcePath = "package.glb";
			result.package.importRoot = "package_Generated";
			TerraEngine::AssetPipeline::GeneratedModelMeshAsset mesh{};
			mesh.guid = TerraEngine::GenerateUUID();
			mesh.displayName = "GeneratedMesh";
			mesh.sourcePath = "package.glb";
			mesh.compiledPath = "package.temf";
			result.generatedMeshes.push_back(mesh);
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(packagePath, result), "Fixture package sidecar should save.");

			VirtualGeometryAdaptiveSettingsMetadata settings = AssetDatabase::Instance().GetDefaultVirtualGeometryAdaptiveSettings();
			settings.targetRatios = { 1.0f, 0.70710677f, 0.5f };
			settings.minimumGeneratedTriangles = 32u;
			std::string error;
			context.Expect(AssetDatabase::Instance().SaveGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, mesh.guid, settings, &error), error.empty() ? "Generated mesh override should save." : error);

			VirtualGeometryAdaptiveSettingsMetadata loaded;
			context.Expect(AssetDatabase::Instance().TryGetGeneratedMeshVirtualGeometryAdaptiveSettings(packagePath, mesh.guid, loaded), "Generated mesh override should load.");
			context.Expect(loaded.overridePresent, "Generated mesh settings should report a sidecar override.");
			context.ExpectEqual(static_cast<std::size_t>(3u), loaded.targetRatios.size(), "Generated mesh override ratios should round-trip.");
			context.ExpectEqual(32u, loaded.minimumGeneratedTriangles, "Generated mesh threshold should round-trip.");

			context.Expect(AssetDatabase::Instance().ClearGeneratedMeshVirtualGeometryPerMeshOverride(packagePath, mesh.guid, &error), error.empty() ? "Generated mesh reset should save." : error);
			VirtualGeometryAdaptiveSettingsMetadata resetSettings;
			context.Expect(AssetDatabase::Instance().TryGetGeneratedMeshVirtualGeometryAdaptiveSettings(packagePath, mesh.guid, resetSettings), "Generated mesh default settings should load after reset.");
			context.Expect(!resetSettings.overridePresent, "Generated mesh reset should remove the sidecar override.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.DeleteRemovesGeneratedOutputs", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path generatedRoot = "package_Generated";
			const std::filesystem::path packageSource = "package.gltf";
			const std::filesystem::path packageCompiled = "package.temf";
			const std::filesystem::path materialSource = generatedRoot / "GeneratedMaterial.tmat";
			const std::filesystem::path textureSource = generatedRoot / "GeneratedTexture.png";
			const std::filesystem::path materialCompiled = materialSource;
			const std::filesystem::path textureCompiled = generatedRoot / "GeneratedTexture.tetf";

			std::filesystem::create_directories(assetRoot / generatedRoot);
			std::filesystem::create_directories(buildRoot / generatedRoot);
			std::ofstream(assetRoot / packageSource, std::ios::trunc) << "{}";
			std::ofstream(buildRoot / packageCompiled, std::ios::binary | std::ios::trunc).put('\0');
			std::ofstream(assetRoot / materialSource, std::ios::trunc) << "{}";
			std::ofstream(buildRoot / materialCompiled, std::ios::trunc) << "{}";
			std::ofstream(assetRoot / textureSource, std::ios::binary | std::ios::trunc).put('\0');
			std::ofstream(buildRoot / textureCompiled, std::ios::binary | std::ios::trunc).put('\0');

			TerraEngine::AssetPipeline::ModelPackageImportResult result{};
			result.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			result.packageGuid = TerraEngine::GenerateUUID();
			result.package.sourcePath = packageSource;
			result.package.importRoot = generatedRoot;

			TerraEngine::AssetPipeline::GeneratedModelMeshAsset mesh{};
			mesh.guid = TerraEngine::GenerateUUID();
			mesh.displayName = "GeneratedMesh";
			mesh.sourcePath = packageSource;
			mesh.compiledPath = packageCompiled;
			result.generatedMeshes.push_back(mesh);

			TerraEngine::AssetPipeline::GeneratedModelMaterialAsset material{};
			material.guid = TerraEngine::GenerateUUID();
			material.displayName = "GeneratedMaterial";
			material.sourcePath = materialSource;
			material.compiledPath = materialCompiled;
			result.generatedMaterials.push_back(material);

			TerraEngine::AssetPipeline::GeneratedModelTextureAsset texture{};
			texture.guid = TerraEngine::GenerateUUID();
			texture.displayName = "GeneratedTexture";
			texture.sourcePath = textureSource;
			texture.compiledPath = textureCompiled;
			result.generatedTextures.push_back(texture);

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(assetRoot / packageSource, result), "Fixture package sidecar should save before delete.");
			WriteAssetMetadata(assetRoot, packageSource, packageCompiled, result.packageGuid, TerraEngine::AssetPipeline::GetModelPackageImporterName(assetRoot / packageSource));
			WriteAssetMetadata(assetRoot, materialSource, materialCompiled, material.guid, "MaterialImporter");
			WriteAssetMetadata(assetRoot, textureSource, textureCompiled, texture.guid, "TextureImporter");

			AssetDatabase::Instance().Initialize(root);
			context.Expect(AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(assetRoot / packageSource)), "Package should be registered before delete.");
			context.Expect(AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(assetRoot / materialSource)), "Generated material should be registered before delete.");
			context.Expect(AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(assetRoot / textureSource)), "Generated texture should be registered before delete.");

			AssetDatabase::Instance().DeleteAsset(assetRoot / packageSource);

			context.Expect(!std::filesystem::exists(assetRoot / packageSource), "Deleting the package should remove the source glTF.");
			context.Expect(!std::filesystem::exists((assetRoot / packageSource).string() + ".meta"), "Deleting the package should remove the package metadata.");
			context.Expect(!std::filesystem::exists(TerraEngine::AssetPipeline::GetImportResultPath(assetRoot / packageSource)), "Deleting the package should remove the package import sidecar.");
			context.Expect(!std::filesystem::exists(buildRoot / packageCompiled), "Deleting the package should remove the compiled package mesh.");
			context.Expect(!std::filesystem::exists(assetRoot / generatedRoot), "Deleting the package should remove generated asset source files.");
			context.Expect(!std::filesystem::exists(buildRoot / generatedRoot), "Deleting the package should remove generated compiled files.");
			context.Expect(!AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(assetRoot / materialSource)), "Generated material metadata should leave the live database.");
			context.Expect(!AssetDatabase::Instance().GetAssets().contains(std::filesystem::absolute(assetRoot / textureSource)), "Generated texture metadata should leave the live database.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});
	}
}
