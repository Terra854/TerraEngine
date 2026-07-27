#include "TestHarness.h"

#include "AssetImportResult.h"
#include "FbxTestHelpers.h"
#include "ModelPackageImporter.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path MakeVirtualGeometryPackageTempRoot(std::string_view testName)
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path() / "TerraEngineVirtualGeometryPackageImportTests" / std::string(testName);
			std::filesystem::remove_all(root);
			std::filesystem::create_directories(root);
			return root;
		}

		nlohmann::json LoadBaseGltfFixture()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path fixturePath = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			std::ifstream input(fixturePath);
			return nlohmann::json::parse(input);
		}

		std::filesystem::path WriteGltfFixture(const std::filesystem::path& tempRoot, std::string_view fileName, const nlohmann::json& document)
		{
			const std::filesystem::path fixturePath = tempRoot / std::string(fileName);
			std::ofstream output(fixturePath);
			output << document.dump(2);
			return fixturePath;
		}

		TerraEngine::AssetPipeline::ModelPackageImportOptions BuildPackageOptions(
			const std::filesystem::path& tempRoot,
			const std::filesystem::path& fixturePath,
			const std::filesystem::path& sourceRelativePath,
			const TerraEngine::UUID& packageGuid)
		{
			TerraEngine::AssetPipeline::ModelPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath = sourceRelativePath;
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			return options;
		}

		bool HasDiagnosticCode(const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view code)
		{
			for (const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if (diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		std::string ReadTextFile(const std::filesystem::path& path)
		{
			std::ifstream input(path);
			return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
		}

		nlohmann::json LoadHardeningFixtureManifest()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			std::ifstream input(sourceRoot / "Tests/Fixtures/VirtualGeometry/hardening-fixtures.json");
			return nlohmann::json::parse(input);
		}

		bool HasCoverageTag(const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh, std::string_view expectedTag)
		{
			return std::find(mesh.virtualGeometryCoverageTags.begin(), mesh.virtualGeometryCoverageTags.end(), std::string(expectedTag)) != mesh.virtualGeometryCoverageTags.end();
		}
	}

	void RegisterVirtualGeometryPackageImportTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.PackageImport.HardeningFixtureManifestCoversRequiredCategories", [](TestContext& context) {
			const nlohmann::json manifest = LoadHardeningFixtureManifest();
			context.ExpectEqual(1, manifest.value("schemaVersion", 0), "Hardening fixture manifest should use schema version 1.");

			std::set<std::string> categories;
			std::set<std::string> packageFormats;
			for (const nlohmann::json& format : manifest.value("packageFormats", nlohmann::json::array()))
			{
				packageFormats.insert(format.get<std::string>());
			}
			for (const nlohmann::json& entry : manifest.value("requiredCategories", nlohmann::json::array()))
			{
				context.Expect(entry.contains("id") && entry["id"].is_string(), "Every hardening fixture entry should have a stable id.");
				context.Expect(entry.contains("category") && entry["category"].is_string(), "Every hardening fixture entry should have a category.");
				context.Expect(entry.value("requiredForCleanCheckout", false), "Required hardening fixtures should be clean-checkout safe.");
				context.Expect(!entry.value("expectedDiagnostics", nlohmann::json::array()).empty(), "Each hardening fixture should name expected diagnostics.");
				context.Expect(!entry.value("expectedRenderOutcomes", nlohmann::json::array()).empty(), "Each hardening fixture should name expected render outcomes.");
				context.Expect(!entry.value("coverageTags", nlohmann::json::array()).empty(), "Each hardening fixture should name coverage tags consumed by import/export diagnostics.");
				categories.insert(entry["category"].get<std::string>());
			}

			const std::vector<std::string> requiredCategories = {
				"tiny",
				"dense",
				"thin",
				"curved",
				"hard-surface",
				"multi-material-package",
				"streaming-pressure",
				"multi-view",
				"export"
			};
			for (const std::string& category : requiredCategories)
			{
				context.Expect(categories.contains(category), "The hardening fixture manifest should cover every broad virtual geometry validation category.");
			}
			context.Expect(packageFormats.contains("gltf"), "Package coverage should include text glTF fixtures.");
			context.Expect(packageFormats.contains("glb"), "Package coverage should include binary GLB fixtures.");
			context.Expect(packageFormats.contains("fbx"), "Package coverage should include FBX fixtures.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.CooksMultipleGeneratedMeshOutputs", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("MultipleMeshes");
			nlohmann::json document = LoadBaseGltfFixture();

			// Duplicate the fixture mesh through a second source mesh and node.
			// Each generated child must now own a matching compiled TEMF so a
			// component that references one child cannot accidentally select or
			// render virtual-geometry clusters from a sibling mesh.
			nlohmann::json secondMesh = document["meshes"][0];
			document["meshes"][0]["name"] = "Triangle_A";
			secondMesh["name"] = "Triangle_B";
			document["meshes"].push_back(secondMesh);
			document["nodes"] = nlohmann::json::array({
				{ { "name", "Node_A" }, { "mesh", 0 } },
				{ { "name", "Node_B" }, { "mesh", 1 } }
			});
			document["scenes"][0]["nodes"] = nlohmann::json::array({ 0, 1 });

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "virtual-multi-mesh.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/virtual-multi-mesh.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Multi-mesh package import should produce usable static geometry.");
			context.ExpectEqual(static_cast<std::size_t>(2), result.generatedMeshes.size(), "Each source mesh should remain visible as a generated package mesh.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.generatedPrefabs.size(), "Package import should generate one configured prefab for the imported object.");
			context.Expect(result.generatedMeshes[0].compiledPath != result.generatedMeshes[1].compiledPath, "Generated mesh children should not share one compiled package TEMF.");
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				context.Expect(mesh.compiledPath.parent_path().filename().string().find("_Generated") != std::string::npos, "Generated child TEMFs should live beside other package child artifacts.");
				context.Expect(mesh.virtualGeometryRequested, "Package mesh records should state that virtual geometry was requested.");
				context.Expect(mesh.virtualGeometryCooked, "Generated package mesh should validate a cooked virtual geometry payload.");
				context.ExpectEqual(std::string("Cooked"), mesh.virtualGeometryStatus, "Generated package mesh should report a cooked virtual geometry status.");
				context.Expect(mesh.virtualGeometryDetailLevels > 0u, "Cooked package mesh should expose a detail count.");
				context.Expect(mesh.virtualGeometryMeshlets > 0u, "Cooked package mesh should expose a meshlet count.");
				context.Expect(mesh.virtualGeometrySourceTriangles > 0u, "Cooked package mesh should expose source triangle count.");
				context.Expect(mesh.virtualGeometryConeSafetySummary.metadataValid, "Package mesh should preserve cone safety metadata from the compiled TEMF.");
				context.Expect(mesh.virtualGeometryConeSafetySummary.disabledClusterCount > 0u, "Merged package mesh cone culling should stay disabled until per-cluster material proof exists.");
			}
			const std::filesystem::path generatedPrefabPath = options.assetRoot / result.generatedPrefabs.front().sourcePath;
			context.Expect(std::filesystem::exists(generatedPrefabPath), "Generated package prefab should be written beside package child assets.");
			const nlohmann::json prefabJson = nlohmann::json::parse(ReadTextFile(generatedPrefabPath));
			const nlohmann::json prefabObjects = prefabJson.value("objects", nlohmann::json::array());
			context.ExpectEqual(static_cast<std::size_t>(1), prefabObjects.size(), "Generated prefab should contain one import root object.");
			const nlohmann::json prefabChildren = prefabObjects.empty() ? nlohmann::json::array() : prefabObjects.front().value("children", nlohmann::json::array());
			context.ExpectEqual(result.generatedMeshes.size(), prefabChildren.size(), "Generated prefab should create one render child per generated mesh.");
			if (!prefabChildren.empty())
			{
				const nlohmann::json meshRender = prefabChildren.front().value("components", nlohmann::json::object()).value("MeshRenderComponent", nlohmann::json::object());
				context.ExpectEqual(TerraEngine::ToHexString(result.generatedMeshes.front().guid), meshRender.value("meshId", std::string()), "Generated prefab child should reference the generated mesh GUID.");
				context.Expect(!meshRender.value("materialId", std::string()).empty(), "Generated prefab child should preserve available material assignment data.");
			}

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);
			context.ExpectEqual(static_cast<std::size_t>(1), sidecar.value("generatedPrefabs", nlohmann::json::array()).size(), "Sidecar JSON should include generated prefab records.");
			context.Expect(sidecar["generatedMeshes"][0]["virtualGeometry"].is_object(), "Sidecar JSON should persist virtual geometry metadata for generated meshes.");
			context.ExpectEqual(std::string("Cooked"), sidecar["generatedMeshes"][0]["virtualGeometry"].value("status", std::string()), "Sidecar JSON should expose the cooked status.");
			const nlohmann::json targets = sidecar["generatedMeshes"][0]["virtualGeometry"].value("targets", nlohmann::json::array());
			context.Expect(!targets.empty(), "Sidecar JSON should persist requested-target diagnostics for generated meshes.");
			context.Expect(targets.front().contains("desiredTriangleCount"), "Sidecar requested-target diagnostics should include desired triangle counts.");
			context.Expect(targets.front().contains("actualTriangleCount"), "Sidecar requested-target diagnostics should include actual generated or candidate triangle counts.");
			const nlohmann::json coneSafety = sidecar["generatedMeshes"][0]["virtualGeometry"].value("coneSafety", nlohmann::json::object());
			context.Expect(coneSafety.value("metadataValid", false), "Sidecar JSON should persist cone safety validity.");
			context.Expect(coneSafety.value("disabledClusterCount", 0u) > 0u, "Sidecar JSON should persist cone safety disabled counts.");
			context.Expect(!coneSafety.value("disabledReasons", nlohmann::json::array()).empty(), "Sidecar JSON should persist cone safety disabled reasons.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/virtual-multi-mesh.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Virtual geometry package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Virtual geometry package sidecar should reload.");
			context.ExpectEqual(static_cast<std::size_t>(1), loaded.generatedPrefabs.size(), "Generated prefab records should round-trip through the sidecar loader.");
			context.ExpectEqual(std::string("Cooked"), loaded.generatedMeshes.front().virtualGeometryStatus, "Virtual geometry status should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryCooked, "Virtual geometry cooked flag should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryClusterSummary.clusterCount > 0u, "Virtual geometry cluster summary should round-trip through the sidecar loader.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryTargets.empty(), "Requested-target diagnostics should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryTargets.front().desiredTriangleCount > 0u, "Desired target counts should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryTargets.front().actualTriangleCount > 0u, "Actual target counts should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryConeSafetySummary.metadataValid, "Cone safety metadata validity should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryConeSafetySummary.disabledClusterCount > 0u, "Cone safety disabled counts should round-trip through the sidecar loader.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PreservesStableGeneratedIdentifiers", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("StableIdentifiers");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "stable.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/stable.gltf", packageGuid);

			const auto first = TerraEngine::AssetPipeline::ImportModelPackage(options);
			const auto second = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(first.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "First package import should succeed.");
			context.Expect(second.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Second package import should succeed.");
			context.Expect(!first.generatedMeshes.empty() && !second.generatedMeshes.empty(), "Both imports should produce generated mesh records.");
			context.Expect(!first.generatedMaterials.empty() && !second.generatedMaterials.empty(), "Both imports should produce generated material records.");
			context.Expect(!first.materialAssignments.empty() && !second.materialAssignments.empty(), "Both imports should preserve material assignments.");

			const std::string expectedMeshGuid = TerraEngine::ToHexString(TerraEngine::AssetPipeline::BuildStableGeneratedModelUuid(packageGuid, "gltf-mesh", 0u));
			context.ExpectEqual(expectedMeshGuid, TerraEngine::ToHexString(first.generatedMeshes.front().guid), "Generated mesh GUID should match the stable package child ID.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMeshes.front().guid), TerraEngine::ToHexString(second.generatedMeshes.front().guid), "Generated mesh GUID should remain stable across reimport.");
			context.ExpectEqual(TerraEngine::ToHexString(first.generatedMaterials.front().guid), TerraEngine::ToHexString(second.generatedMaterials.front().guid), "Generated material GUID should remain stable across reimport.");
			context.ExpectEqual(TerraEngine::ToHexString(first.materialAssignments.front().meshGuid), TerraEngine::ToHexString(second.materialAssignments.front().meshGuid), "Material slot mesh reference should remain stable across reimport.");
			context.Expect(TerraEngine::ToHexString(first.generatedMeshes.front().guid) != TerraEngine::ToHexString(packageGuid), "Generated mesh should keep a package-child ID instead of being overwritten by the package GUID.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.GeneratedChildOwnsCurrentGpuBytes", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("GeneratedChildGpuBytes");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "child-gpu-bytes.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/child-gpu-bytes.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Package import should produce generated children for residency accounting.");
			context.Expect(!result.generatedMeshes.empty(), "Package import should expose the generated mesh child id.");
			if (result.generatedMeshes.empty())
			{
				return;
			}

			ResourceManager& resourceManager = ResourceManager::Instance();
			resourceManager.SetStreamingDiagnosticsEnabled(true);
			const TerraEngine::UUID childMeshId = result.generatedMeshes.front().guid;
			const std::filesystem::path parentPath = tempRoot / "Assets/Models/child-gpu-bytes.gltf";
			const std::filesystem::path childPath = tempRoot / "Build" / result.generatedMeshes.front().compiledPath;
			resourceManager.RegisterStreamableAsset(packageGuid, AssetType::Mesh, parentPath);
			resourceManager.RegisterStreamableAsset(childMeshId, AssetType::Mesh, childPath);
			resourceManager.MarkResourceUploadedToGpu(childMeshId, AssetType::Mesh, 77u, 4096u, "generated package child upload");

			const TerraEngine::Resource::StreamingDiagnosticsSummary summary = resourceManager.GetStreamingDiagnosticsSummary();
			const auto parentRecord = std::find_if(summary.records.begin(), summary.records.end(), [packageGuid](const TerraEngine::Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == packageGuid;
			});
			const auto childRecord = std::find_if(summary.records.begin(), summary.records.end(), [childMeshId](const TerraEngine::Resource::StreamingDiagnosticRecord& record) {
				return record.assetId == childMeshId;
			});

			context.Expect(parentRecord != summary.records.end(), "The package parent should exist only as a logical diagnostics row before UI filtering.");
			context.Expect(childRecord != summary.records.end(), "The generated mesh child should own the current GPU-byte row.");
			if (parentRecord != summary.records.end())
			{
				context.Expect(parentRecord->exactGpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::ZeroByState, "Package parents must not borrow generated child GPU bytes.");
				context.ExpectEqual(0ull, parentRecord->exactGpuBytes.bytes, "Package parent current GPU bytes should stay zero unless it owns an allocation.");
			}
			if (childRecord != summary.records.end())
			{
				context.Expect(childRecord->exactGpuBytes.status == TerraEngine::Resource::ExactResidencyByteStatus::Exact, "Generated child current GPU bytes should be exact.");
				context.ExpectEqual(4096ull, childRecord->exactGpuBytes.bytes, "Generated child current GPU bytes should match the backend upload.");
			}

			resourceManager.DeleteResource(packageGuid, AssetType::Mesh);
			resourceManager.DeleteResource(childMeshId, AssetType::Mesh);
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.ReportsMultiMaterialClusterCoverage", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("MultiMaterialCoverage");
			nlohmann::json document = LoadBaseGltfFixture();
			nlohmann::json secondMaterial = document["materials"][0];
			secondMaterial["name"] = "Mat_Blue";
			secondMaterial["pbrMetallicRoughness"]["baseColorFactor"] = nlohmann::json::array({ 0.05f, 0.15f, 0.9f, 1.0f });
			document["materials"].push_back(secondMaterial);
			nlohmann::json secondPrimitive = document["meshes"][0]["primitives"][0];
			secondPrimitive["material"] = 1;
			document["meshes"][0]["primitives"].push_back(secondPrimitive);

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "multi-material.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/multi-material.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Multi-material package import should still produce static geometry.");
			context.ExpectEqual(static_cast<std::size_t>(1u), result.generatedMeshes.size(), "One source mesh should produce one generated mesh record.");
			context.ExpectEqual(static_cast<std::size_t>(2u), result.materialAssignments.size(), "Both authored primitives should produce material assignments.");
			const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh = result.generatedMeshes.front();
			context.Expect(mesh.virtualGeometryCooked, "Multi-material fixture should still cook virtual geometry.");
			context.ExpectEqual(2u, mesh.virtualGeometryClusterSummary.sourceMaterialAssignmentCount, "Cluster summary should remember the authored material assignment count.");
			context.Expect(mesh.virtualGeometryClusterSummary.materialSectionCount >= 1u, "Cluster summary should report cooked virtual geometry material sections.");
			context.Expect(mesh.virtualGeometryClusterSummary.clusterMaterialSectionCount >= 1u, "Cluster summary should report cluster material ownership diagnostics.");
			context.Expect(!mesh.virtualGeometryClusterSummary.materialCoverageValid, "Current TEMF v1 package cook should flag multi-material cluster coverage for review instead of silently claiming full section coverage.");
			context.Expect(std::any_of(mesh.virtualGeometryWarnings.begin(), mesh.virtualGeometryWarnings.end(), [](const std::string& warning) {
				return warning.find("material coverage") != std::string::npos;
			}), "Multi-material package diagnostics should explain material coverage review.");

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);
			const nlohmann::json clusterSummary = sidecar["generatedMeshes"][0]["virtualGeometry"]["clusterSummary"];
			context.ExpectEqual(2u, clusterSummary.value("sourceMaterialAssignmentCount", 0u), "Sidecar JSON should persist source material assignment counts.");
			context.Expect(!clusterSummary.value("materialCoverageValid", true), "Sidecar JSON should persist material coverage review state.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/multi-material.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Multi-material package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Multi-material package sidecar should reload.");
			context.ExpectEqual(2u, loaded.generatedMeshes.front().virtualGeometryClusterSummary.sourceMaterialAssignmentCount, "Material assignment count should round-trip through the sidecar loader.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryClusterSummary.materialCoverageValid, "Material coverage state should round-trip through the sidecar loader.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PreservesGeneratedMeshOverrideSettings", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("GeneratedOverrideSettings");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "override-settings.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/override-settings.gltf", packageGuid);
			options.hasVirtualGeometrySettings = true;
			options.virtualGeometrySettings.hasOverride = true;
			options.virtualGeometrySettings.settingsSource = "PerMeshOverride";
			options.virtualGeometrySettings.targetRatios = { 1.0f, 0.5f };
			options.virtualGeometrySettings.minimumGeneratedTriangles = 32u;

			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Package import with generated mesh override settings should succeed.");
			context.Expect(!result.generatedMeshes.empty(), "Package import should produce a generated mesh record.");
			context.Expect(result.generatedMeshes.front().virtualGeometrySettings.hasOverride, "Generated mesh record should preserve the override flag.");
			context.ExpectEqual(static_cast<std::size_t>(2u), result.generatedMeshes.front().virtualGeometrySettings.targetRatios.size(), "Generated mesh override ratios should be stored in the import result.");
			context.ExpectEqual(32u, result.generatedMeshes.front().virtualGeometrySettings.minimumGeneratedTriangles, "Generated mesh override thresholds should be stored in the import result.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/override-settings.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Override package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Override package sidecar should reload.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometrySettings.hasOverride, "Generated mesh override flag should round-trip through the sidecar.");
			context.ExpectEqual(static_cast<std::size_t>(2u), loaded.generatedMeshes.front().virtualGeometrySettings.targetRatios.size(), "Generated mesh override ratios should round-trip through the sidecar.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.UnsupportedContentStaysNonBlocking", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("UnsupportedContent");
			nlohmann::json document = LoadBaseGltfFixture();
			document["extras"] = {
				{ "animations", true },
				{ "skins", true },
				{ "cameras", true },
				{ "lights", true },
				{ "constraints", true }
			};

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "unsupported.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/unsupported.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			context.Expect(result.status == TerraEngine::AssetPipeline::ModelImportStatus::SucceededWithWarnings, "Unsupported authoring data should warn without blocking static mesh output.");
			context.Expect(!result.generatedMeshes.empty(), "Usable static geometry should still be generated when unsupported authoring data is ignored.");
			const std::vector<std::string> expectedCodes = {
				"UnsupportedCameraIgnored",
				"UnsupportedLightIgnored",
				"UnsupportedConstraintIgnored"
			};
			for (const std::string& code : expectedCodes)
			{
				context.Expect(HasDiagnosticCode(result, code), "Unsupported package content should be reported through diagnostics.");
			}
			context.Expect(!HasDiagnosticCode(result, "UnsupportedAnimationIgnored")
				&& !HasDiagnosticCode(result, "UnsupportedSkinIgnored"),
				"Animation and skin records are supported package content and must not be downgraded by the format-neutral dispatcher.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.RejectsCorruptSidecarWithoutMutatingLoadedResult", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("RejectedSidecar");
			const nlohmann::json document = LoadBaseGltfFixture();
			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "atomicity.gltf", document);
			const TerraEngine::UUID packageGuid = TerraEngine::GenerateUUID();
			const auto options = BuildPackageOptions(tempRoot, fixturePath, "Models/atomicity.gltf", packageGuid);
			const auto result = TerraEngine::AssetPipeline::ImportModelPackage(options);

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/atomicity.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Initial package sidecar should publish through the atomic writer.");
			const std::filesystem::path sidecarPath = TerraEngine::AssetPipeline::GetImportResultPath(sourceAssetPath);
			const std::string goodSidecar = ReadTextFile(sidecarPath);

			{
				std::ofstream corrupt(sidecarPath, std::ios::trunc);
				corrupt << "{ this is not valid package JSON";
			}

			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			loaded.status = TerraEngine::AssetPipeline::ModelImportStatus::Succeeded;
			loaded.packageGuid = packageGuid;
			context.Expect(!TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Corrupt package sidecar should be rejected.");
			context.Expect(loaded.status == TerraEngine::AssetPipeline::ModelImportStatus::Succeeded, "Rejected sidecar reads should not partially mutate the caller's previous result.");
			context.ExpectEqual(TerraEngine::ToHexString(packageGuid), TerraEngine::ToHexString(loaded.packageGuid), "Rejected sidecar reads should preserve the previous package GUID.");

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Atomic writer should recover from a corrupt sidecar by publishing a complete replacement.");
			context.ExpectEqual(goodSidecar, ReadTextFile(sidecarPath), "Recovered sidecar should match the last complete import result.");
			context.Expect(!std::filesystem::exists(sidecarPath.string() + ".tmp"), "Atomic sidecar writer should not leave staged files after success.");
		});

		registry.Add("VirtualGeometry.Editor.PackageImport.PublishesCoverageTagsForGltfAndFbxPackages", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("CoverageTags");
			nlohmann::json document = LoadBaseGltfFixture();
			nlohmann::json secondMaterial = document["materials"][0];
			secondMaterial["name"] = "Coverage_Mat_B";
			document["materials"].push_back(secondMaterial);
			nlohmann::json secondPrimitive = document["meshes"][0]["primitives"][0];
			secondPrimitive["material"] = 1;
			document["meshes"][0]["primitives"].push_back(secondPrimitive);

			const std::filesystem::path fixturePath = WriteGltfFixture(tempRoot, "coverage-tags.gltf", document);
			const TerraEngine::UUID gltfPackageGuid = TerraEngine::GenerateUUID();
			const auto gltfOptions = BuildPackageOptions(tempRoot, fixturePath, "Models/coverage-tags.gltf", gltfPackageGuid);
			const auto gltfResult = TerraEngine::AssetPipeline::ImportModelPackage(gltfOptions);
			context.Expect(gltfResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Coverage-tag glTF package import should succeed.");
			context.Expect(!gltfResult.generatedMeshes.empty(), "Coverage-tag glTF package import should produce a generated mesh.");
			const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& gltfMesh = gltfResult.generatedMeshes.front();
			context.Expect(HasCoverageTag(gltfMesh, "package-gltf"), "glTF package meshes should publish the package-gltf coverage tag.");
			context.Expect(HasCoverageTag(gltfMesh, "package-static-geometry"), "Package meshes should publish the static geometry coverage tag.");
			context.Expect(HasCoverageTag(gltfMesh, "multi-material-package"), "Multi-material package meshes should publish the multi-material coverage tag.");
			context.Expect(HasCoverageTag(gltfMesh, "adaptive-targets"), "Cooked package meshes should publish adaptive target coverage.");
			context.Expect(HasCoverageTag(gltfMesh, "fallback-coverage"), "Cooked package meshes should publish fallback coverage.");
			context.Expect(HasCoverageTag(gltfMesh, "page-links"), "Cooked package meshes should publish page-link coverage.");
			context.Expect(HasCoverageTag(gltfMesh, "cone-safety-metadata"), "glTF package meshes should publish cone safety metadata coverage.");
			context.Expect(HasCoverageTag(gltfMesh, "cone-culling-disabled"), "glTF package meshes should state that cone culling remains disabled without per-cluster material proof.");

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(gltfResult);
			const nlohmann::json tags = sidecar["generatedMeshes"][0]["virtualGeometry"].value("coverageTags", nlohmann::json::array());
			bool sidecarHasMultiMaterialTag = false;
			for (const nlohmann::json& tag : tags)
			{
				sidecarHasMultiMaterialTag = sidecarHasMultiMaterialTag || (tag.is_string() && tag.get<std::string>() == "multi-material-package");
			}
			context.Expect(sidecarHasMultiMaterialTag, "Coverage tags should persist into generated package sidecar JSON.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/coverage-tags.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, gltfResult), "Coverage-tag glTF package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loadedGltf;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loadedGltf), "Coverage-tag glTF package sidecar should reload.");
			context.Expect(HasCoverageTag(loadedGltf.generatedMeshes.front(), "package-gltf"), "Coverage tags should round-trip through the package sidecar loader.");

			const auto fbxResult = ImportFbxFixture("box.fbx");
			context.Expect(fbxResult.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed, "Coverage-tag FBX package import should succeed.");
			context.Expect(!fbxResult.generatedMeshes.empty(), "Coverage-tag FBX package import should produce a generated mesh.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "package-fbx"), "FBX package meshes should publish the package-fbx coverage tag.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "package-static-geometry"), "FBX package meshes should publish the static geometry coverage tag.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "adaptive-targets"), "FBX package meshes should publish adaptive target coverage.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "cone-safety-metadata"), "FBX package meshes should publish cone safety metadata coverage.");
			context.Expect(HasCoverageTag(fbxResult.generatedMeshes.front(), "cone-culling-disabled"), "FBX package meshes should keep cone culling disabled until per-cluster material proof exists.");
		});
	}
}
