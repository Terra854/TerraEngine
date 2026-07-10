#include "TestHarness.h"

#include "AssetImportResult.h"
#include "AssetDatabase.h"
#include "Engine/File Formats/TerraEngineMeshFile.h"
#include "ModelPackageImporter.h"
#include "Resource/ResourceManager.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

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
			std::filesystem::create_directories(path.parent_path());

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

		void WriteSmallMeshSkippedHeader(const std::filesystem::path& path)
		{
			MeshHeader header{};
			std::memcpy(header.magic, "TEMF", 4);
			header.version = kMeshCurrentVersion;
			header.headerSize = sizeof(MeshHeader);
			header.meshletCount = 1u;
			header.meshletFlags = kMeshletHeaderFlagHasVirtualGeometry;
			header.virtualGeometryHeaderOffset = sizeof(MeshHeader);
			header.virtualGeometryHeaderSize = sizeof(VirtualGeometryHeader);
			header.virtualGeometryPayloadGeneration = kVirtualGeometryPayloadGeneration;

			const std::string message = "Mesh is too small for useful page streaming.";
			VirtualGeometryHeader virtualHeader{};
			virtualHeader.payloadGeneration = kVirtualGeometryPayloadGeneration;
			virtualHeader.headerSize = sizeof(VirtualGeometryHeader);
			virtualHeader.detailLevelCount = 1u;
			virtualHeader.cullableGroupCount = 1u;
			virtualHeader.clusterCount = 1u;
			virtualHeader.sourceTriangleCount = 1u;
			virtualHeader.skippedDetailCount = 1u;
			virtualHeader.skippedDetailTableOffset = sizeof(MeshHeader) + sizeof(VirtualGeometryHeader);
			virtualHeader.diagnosticStringOffset = virtualHeader.skippedDetailTableOffset + sizeof(VirtualGeometrySkippedDetailRecord);
			virtualHeader.diagnosticStringSize = static_cast<std::uint32_t>(message.size());

			VirtualGeometrySkippedDetailRecord skipped{};
			skipped.targetIndex = 1u;
			skipped.status = static_cast<std::uint32_t>(VirtualGeometryDetailTargetRecordStatus::Skipped);
			skipped.reason = static_cast<std::uint32_t>(VirtualGeometrySkippedDetailRecordReason::MeshTooSmall);
			skipped.messageStringSize = static_cast<std::uint32_t>(message.size());

			std::ofstream output(path, std::ios::binary);
			output.write(reinterpret_cast<const char*>(&header), sizeof(header));
			output.write(reinterpret_cast<const char*>(&virtualHeader), sizeof(virtualHeader));
			output.write(reinterpret_cast<const char*>(&skipped), sizeof(skipped));
			output.write(message.data(), static_cast<std::streamsize>(message.size()));
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

		template <typename Predicate>
		bool PumpAssetDatabaseUntil(Predicate predicate, int attempts = 160)
		{
			for (int attempt = 0; attempt < attempts; ++attempt)
			{
				AssetDatabase::Instance().Update();
				if (predicate())
				{
					return true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
			return false;
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
			const bool indexCreated = PumpAssetDatabaseUntil([&]() {
				return std::filesystem::exists(root / "Library" / "AssetIndex.json");
			});
			context.Expect(indexCreated, "Background startup reconcile should persist a startup asset index.");
			AssetDatabase::Instance().Shutdown();

			std::filesystem::remove(sourcePath.string() + ".meta");
			AssetDatabase::Instance().Initialize(root);
			const AssetMetadata* indexedMetadata = AssetDatabase::Instance().FindAssetByGuid(guid);
			context.Expect(indexedMetadata != nullptr, "Startup should restore known assets from AssetIndex.json before the reconcile scan runs.");
			context.ExpectEqual(std::filesystem::path("CachedMaterial.tmat"), indexedMetadata->sourcePath, "Indexed metadata should preserve the original source path.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.AssetDatabase.StartupPackageChildrenPublishOneCompleteIndex", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path packageSource = "camera.glb";
			const std::filesystem::path materialSource = "camera_Generated/camera.tmat";
			const std::filesystem::path prefabSource = "camera_Generated/camera.teprefab";
			const std::filesystem::path trailingSource = "Trailing.tmat";
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID prefabGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID trailingGuid = TerraEngine::GenerateUUID();

			std::ofstream(assetRoot / packageSource, std::ios::binary | std::ios::trunc).put('\0');
			WriteMaterialAsset(assetRoot / materialSource, "GeneratedCameraMaterial");
			WriteMaterialAsset(buildRoot / materialSource, "GeneratedCameraMaterialCompiled");
			std::ofstream(assetRoot / prefabSource, std::ios::trunc) << "{}";
			WriteMaterialAsset(assetRoot / trailingSource, "Trailing");
			WriteMaterialAsset(buildRoot / trailingSource, "TrailingCompiled");

			TerraEngine::AssetPipeline::ModelPackageImportResult importResult{};
			importResult.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			importResult.packageGuid = packageGuid;
			importResult.package.sourcePath = packageSource;
			importResult.package.importRoot = "camera_Generated";
			TerraEngine::AssetPipeline::GeneratedModelMaterialAsset material{};
			material.guid = materialGuid;
			material.sourcePath = materialSource;
			material.compiledPath = materialSource;
			material.displayName = "GeneratedCameraMaterial";
			importResult.generatedMaterials.push_back(material);
			TerraEngine::AssetPipeline::GeneratedModelPrefabAsset prefab{};
			prefab.guid = prefabGuid;
			prefab.sourcePath = prefabSource;
			prefab.displayName = "GeneratedCameraPrefab";
			importResult.generatedPrefabs.push_back(prefab);
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(assetRoot / packageSource, importResult), "Package sidecar should save generated startup children.");

			const auto BuildIndexEntry = [&](const std::filesystem::path& sourcePath, const std::filesystem::path& compiledPath, const TerraEngine::UUID& guid, const std::string& importerName) {
				nlohmann::json entry;
				entry["guid"] = TerraEngine::ToHexString(guid);
				entry["importerName"] = importerName;
				entry["lastModified"] = std::filesystem::last_write_time(assetRoot / sourcePath).time_since_epoch().count();
				entry["sourcePath"] = sourcePath.generic_string();
				entry["compiledPath"] = compiledPath.generic_string();
				entry["settings"] = nlohmann::json::object();
				return entry;
			};

			nlohmann::json startupIndex;
			startupIndex["version"] = 1;
			startupIndex["assets"] = nlohmann::json::array({
				BuildIndexEntry(packageSource, "camera.temf", packageGuid, TerraEngine::AssetPipeline::GetModelPackageImporterName(assetRoot / packageSource)),
				BuildIndexEntry(trailingSource, trailingSource, trailingGuid, "MaterialImporter")
			});
			std::filesystem::create_directories(root / "Library");
			std::ofstream(root / "Library" / "AssetIndex.json", std::ios::trunc) << startupIndex.dump(4);

			AssetDatabase::Instance().Initialize(root);
			nlohmann::json publishedIndex;
			std::ifstream(root / "Library" / "AssetIndex.json") >> publishedIndex;
			std::vector<std::string> publishedSources;
			for (const nlohmann::json& entry : publishedIndex["assets"]) {
				publishedSources.push_back(entry.value("sourcePath", std::string()));
			}

			const auto HasPublishedSource = [&](const std::filesystem::path& sourcePath) {
				return std::find(publishedSources.begin(), publishedSources.end(), sourcePath.generic_string()) != publishedSources.end();
			};
			context.Expect(HasPublishedSource(packageSource), "Consolidated startup publication should retain the package parent.");
			context.Expect(HasPublishedSource(materialSource), "Consolidated startup publication should add the generated material.");
			context.Expect(HasPublishedSource(prefabSource), "Consolidated startup publication should add the generated prefab.");
			context.Expect(HasPublishedSource(trailingSource), "Consolidated startup publication should retain entries loaded after the package parent.");
			context.Expect(!std::filesystem::exists(root / "Library" / "AssetIndex.json.tmp"), "Consolidated startup publication should not leave a shared temporary index file.");

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
			context.Expect(PumpAssetDatabaseUntil([&]() {
				return std::filesystem::exists(root / "Library" / "AssetIndex.json");
			}), "Initial background reconcile should publish an asset index before the indexed startup pass.");
			AssetDatabase::Instance().Shutdown();

			WriteMaterialAsset(assetRoot / "DiscoveredLater.tmat", "DiscoveredLater");
			context.Expect(std::filesystem::exists(assetRoot / "DiscoveredLater.tmat"), "New material source should exist before indexed startup reconcile.");
			AssetDatabase::Instance().Initialize(root);

			bool discovered = false;
			discovered = PumpAssetDatabaseUntil([&]() {
				for (const auto& [path, metadata] : AssetDatabase::Instance().GetAssets()) {
					if (metadata.sourcePath == std::filesystem::path("DiscoveredLater.tmat")) {
						return true;
					}
				}
				return false;
			});

			context.Expect(discovered, "Background reconcile should import new files that were not present in the startup index.");
			context.Expect(AssetDatabase::Instance().FindAssetByGuid(indexedGuid) != nullptr, "Indexed assets should remain registered while reconcile catches up.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.AssetDatabase.ModelPackageReconcileUsesSidecarChildrenInsteadOfParentTemf", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path packageSource = "camera.glb";
			const std::filesystem::path obsoleteParentCompiled = "camera.temf";
			const std::filesystem::path generatedMeshCompiled = "camera_Generated/camera_uv.temf";
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID meshGuid = TerraEngine::GenerateUUID();

			std::ofstream(assetRoot / packageSource, std::ios::binary | std::ios::trunc).put('\0');
			WriteMeshHeader(buildRoot / generatedMeshCompiled, true);
			WriteAssetMetadata(
				assetRoot,
				packageSource,
				obsoleteParentCompiled,
				packageGuid,
				TerraEngine::AssetPipeline::GetModelPackageImporterName(assetRoot / packageSource));

			TerraEngine::AssetPipeline::ModelPackageImportResult result{};
			result.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			result.packageGuid = packageGuid;
			result.package.sourcePath = packageSource;
			result.package.importRoot = "camera_Generated";

			TerraEngine::AssetPipeline::GeneratedModelMeshAsset mesh{};
			mesh.guid = meshGuid;
			mesh.displayName = "camera_uv";
			mesh.sourcePath = packageSource;
			mesh.compiledPath = generatedMeshCompiled;
			result.generatedMeshes.push_back(mesh);

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(assetRoot / packageSource, result), "Fixture package sidecar should save generated child outputs.");

			ResourceManager::Instance().ClearCaches();
			AssetDatabase::Instance().Initialize(root);
			const bool packageRegistered = PumpAssetDatabaseUntil([&]() {
				return AssetDatabase::Instance().FindAssetByGuid(packageGuid) != nullptr;
			});

			context.Expect(packageRegistered, "Package reconcile should register the package from metadata and sidecar data.");
			context.Expect(!AssetDatabase::Instance().IsCompiling(), "Unchanged package with valid sidecar children should not launch a compiler job.");
			context.Expect(!std::filesystem::exists(buildRoot / obsoleteParentCompiled), "Missing obsolete parent package TEMF should not make the package dirty.");
			const TerraEngine::UUID resolvedMesh = ResourceManager::Instance().ResolveAssetReference((buildRoot / generatedMeshCompiled).generic_string(), AssetType::Mesh);
			context.ExpectEqual(TerraEngine::ToHexString(meshGuid), TerraEngine::ToHexString(resolvedMesh), "Generated child mesh should stay registered as the renderable package output.");

			AssetDatabase::Instance().Shutdown();
			ResourceManager::Instance().ClearCaches();
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
			context.Expect(!virtualGeometry.pageStreamingReady, "Cooked metadata without page tables should not be reported as page-streaming ready.");
			context.Expect(!virtualGeometry.pageIntegrityErrors.empty(), "Inspector metadata should expose page integrity issues without turning them into rebuild warnings.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.US7.ReportsSmallMeshSkippedStreamingReason", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			const std::filesystem::path compiledPath = AssetDatabase::Instance().GetBuildRoot() / "tiny.temf";
			WriteSmallMeshSkippedHeader(compiledPath);

			AssetMetadata metadata{};
			metadata.importerName = "MeshImporter";
			metadata.compiledPath = "tiny.temf";
			VirtualGeometryAssetMetadata virtualGeometry{};
			const bool readable = AssetDatabase::Instance().TryGetVirtualGeometryMetadata(metadata, virtualGeometry);
			VirtualGeometryPageSummaryMetadata pageSummary{};
			const bool pageSummaryReadable = AssetDatabase::Instance().TryGetVirtualGeometryPageSummary(metadata, pageSummary);

			context.Expect(readable, "Tiny cooked mesh metadata should remain readable.");
			context.Expect(virtualGeometry.smallMeshStreamingSkipped, "Tiny meshes should explicitly report that page streaming was skipped.");
			context.Expect(!virtualGeometry.pageStreamingReady, "Tiny skipped meshes should not claim page streaming readiness.");
			context.Expect(virtualGeometry.skippedStreamingReason.find("too small") != std::string::npos, "Tiny skipped meshes should carry the cook explanation.");
			context.Expect(pageSummaryReadable, "Mesh page summaries should be readable for tiny meshes.");
			context.Expect(pageSummary.smallMeshStreamingSkipped, "Page summaries should carry the tiny-mesh skipped flag.");
			context.Expect(pageSummary.integrityErrors.empty(), "Intentional tiny-mesh skips should not look like corrupt page metadata.");

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

		registry.Add("VirtualGeometry.Editor.AssetDatabase.GeneratedPackageChildNamesResolveFromSidecar", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			const std::filesystem::path assetRoot = root / "Assets";
			const std::filesystem::path buildRoot = root / "Build" / "Assets";
			const std::filesystem::path packageSource = "camera.glb";
			const std::filesystem::path packageCompiled = "camera.temf";
			const std::filesystem::path namedMeshCompiled = "camera_Generated/camera_uv.temf";
			const std::filesystem::path fallbackMeshCompiled = "camera_Generated/tripod_head.temf";
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();

			std::ofstream(assetRoot / packageSource, std::ios::binary | std::ios::trunc).put('\0');
			WriteMeshHeader(buildRoot / packageCompiled, false);
			WriteMeshHeader(buildRoot / namedMeshCompiled, true);
			WriteMeshHeader(buildRoot / fallbackMeshCompiled, true);
			WriteAssetMetadata(
				assetRoot,
				packageSource,
				packageCompiled,
				packageGuid,
				TerraEngine::AssetPipeline::GetModelPackageImporterName(assetRoot / packageSource));

			TerraEngine::AssetPipeline::ModelPackageImportResult result{};
			result.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			result.packageGuid = packageGuid;
			result.package.sourcePath = packageSource;
			result.package.importRoot = "camera_Generated";

			TerraEngine::AssetPipeline::GeneratedModelMeshAsset namedMesh{};
			namedMesh.guid = TerraEngine::GenerateUUID();
			namedMesh.displayName = "camera_uv";
			namedMesh.sourceNodePath = "Scene/Camera/camera_uv";
			namedMesh.sourcePath = packageSource;
			namedMesh.compiledPath = namedMeshCompiled;
			result.generatedMeshes.push_back(namedMesh);

			TerraEngine::AssetPipeline::GeneratedModelMeshAsset fallbackMesh{};
			fallbackMesh.guid = TerraEngine::GenerateUUID();
			fallbackMesh.sourceNodePath = "Scene/Tripod/tripod_head";
			fallbackMesh.sourcePath = packageSource;
			fallbackMesh.compiledPath = fallbackMeshCompiled;
			result.generatedMeshes.push_back(fallbackMesh);

			TerraEngine::AssetPipeline::GeneratedModelMaterialAsset material{};
			material.guid = TerraEngine::GenerateUUID();
			material.displayName = "brass_trim";
			material.sourcePath = "camera_Generated/brass_trim.tmat";
			material.compiledPath = material.sourcePath;
			result.generatedMaterials.push_back(material);

			TerraEngine::AssetPipeline::GeneratedModelTextureAsset texture{};
			texture.guid = TerraEngine::GenerateUUID();
			texture.displayName = "lens_baseColor";
			texture.sourcePath = "camera_Generated/lens_baseColor.png";
			texture.compiledPath = "camera_Generated/lens_baseColor.tetf";
			result.generatedTextures.push_back(texture);

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(assetRoot / packageSource, result), "Fixture package sidecar should save child display names.");

			ResourceManager::Instance().ClearCaches();
			AssetDatabase::Instance().Initialize(root);
			context.Expect(PumpAssetDatabaseUntil([&]() {
				return std::filesystem::exists(root / "Library" / "AssetIndex.json");
			}), "Initial package reconcile should persist a startup asset index.");
			AssetDatabase::Instance().Shutdown();

			std::filesystem::remove(buildRoot / packageCompiled);
			ResourceManager::Instance().ClearCaches();
			AssetDatabase::Instance().Initialize(root);

			std::string resolvedName;
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(namedMesh.guid, GeneratedModelPackageChildKind::Mesh, resolvedName), "Generated mesh child should resolve from the package sidecar.");
			context.ExpectEqual(std::string("camera_uv"), resolvedName, "Generated mesh component labels should use the imported child mesh name, not the package parent name.");

			resolvedName.clear();
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(fallbackMesh.guid, GeneratedModelPackageChildKind::Mesh, resolvedName), "Generated mesh child should still resolve when the importer only supplied a node path.");
			context.ExpectEqual(std::string("tripod_head"), resolvedName, "Generated mesh fallback labels should use the source node leaf.");

			resolvedName.clear();
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(material.guid, GeneratedModelPackageChildKind::Material, resolvedName), "Generated material child should resolve from the package sidecar.");
			context.ExpectEqual(std::string("brass_trim"), resolvedName, "Generated material component labels should use the child material name.");

			resolvedName.clear();
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(texture.guid, GeneratedModelPackageChildKind::Texture, resolvedName), "Generated texture child should resolve from the package sidecar.");
			context.ExpectEqual(std::string("lens_baseColor"), resolvedName, "Generated texture component labels should use the child texture name.");

			resolvedName.clear();
			context.Expect(!AssetDatabase::Instance().TryGetGeneratedModelPackageChildDisplayName(namedMesh.guid, GeneratedModelPackageChildKind::Texture, resolvedName), "A mesh GUID should not resolve through the texture child lookup.");

			std::vector<TerraEngine::UUID> childIds;
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildIds(packageGuid, GeneratedModelPackageChildKind::Mesh, childIds), "Generated mesh child IDs should resolve from the package parent GUID.");
			context.ExpectEqual(static_cast<std::size_t>(2u), childIds.size(), "Package parent rows should be able to enumerate all generated child meshes.");
			context.Expect(std::find(childIds.begin(), childIds.end(), namedMesh.guid) != childIds.end(), "Package child ID lookup should include the named mesh child.");
			context.Expect(std::find(childIds.begin(), childIds.end(), fallbackMesh.guid) != childIds.end(), "Package child ID lookup should include the fallback-named mesh child.");

			childIds.clear();
			context.Expect(AssetDatabase::Instance().TryGetGeneratedModelPackageChildIds(packageGuid, GeneratedModelPackageChildKind::Texture, childIds), "Generated texture child IDs should resolve from the package parent GUID.");
			context.ExpectEqual(static_cast<std::size_t>(1u), childIds.size(), "Package texture child lookup should return the generated texture record.");
			context.ExpectEqual(TerraEngine::ToHexString(texture.guid), TerraEngine::ToHexString(childIds.front()), "Package texture child lookup should return the sidecar texture GUID.");

			const TerraEngine::UUID resolvedNamedMesh = ResourceManager::Instance().ResolveAssetReference((buildRoot / namedMeshCompiled).generic_string(), AssetType::Mesh);
			context.ExpectEqual(TerraEngine::ToHexString(namedMesh.guid), TerraEngine::ToHexString(resolvedNamedMesh), "Fast startup should register generated mesh child TEMFs even when the obsolete parent package TEMF is missing.");
			const TerraEngine::UUID resolvedPackageParent = ResourceManager::Instance().ResolveAssetReference((buildRoot / packageCompiled).generic_string(), AssetType::Mesh);
			context.Expect(resolvedPackageParent != packageGuid, "Package parents should not be registered as renderable mesh resources; generated children own the runtime TEMFs.");

			AssetDatabase::Instance().Shutdown();
			ResourceManager::Instance().ClearCaches();
			std::filesystem::remove_all(root);
		});

		registry.Add("VirtualGeometry.Editor.Inspector.US7.HidesPageStreamingReadinessForNonMeshAssets", [](TestContext& context) {
			const std::filesystem::path root = MakeVirtualGeometryInspectorProjectRoot();
			AssetDatabase::Instance().Initialize(root);

			AssetMetadata textureMetadata{};
			textureMetadata.importerName = "TextureImporter";
			textureMetadata.compiledPath = "texture.tetf";
			VirtualGeometryPageSummaryMetadata pageSummary{};
			VirtualGeometryAdaptiveSettingsMetadata settings{};

			context.Expect(!AssetDatabase::Instance().IsMeshAsset(textureMetadata), "Texture assets should not be treated as mesh assets.");
			context.Expect(!AssetDatabase::Instance().CanEditVirtualGeometryPerMeshOverride(textureMetadata), "Non-mesh assets should not expose VG override controls.");
			context.Expect(!AssetDatabase::Instance().TryGetVirtualGeometryAdaptiveSettings(textureMetadata, settings), "Non-mesh assets should not expose VG settings controls.");
			context.Expect(!AssetDatabase::Instance().TryGetVirtualGeometryPageSummary(textureMetadata, pageSummary), "Non-mesh assets should not expose page-streaming readiness summaries.");

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
			const bool generatedAssetsRegistered = PumpAssetDatabaseUntil([&]() {
				const auto& assets = AssetDatabase::Instance().GetAssets();
				return assets.contains(std::filesystem::absolute(assetRoot / packageSource))
					&& assets.contains(std::filesystem::absolute(assetRoot / materialSource))
					&& assets.contains(std::filesystem::absolute(assetRoot / textureSource));
			});
			context.Expect(generatedAssetsRegistered, "Package and generated child assets should be registered before delete.");

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
