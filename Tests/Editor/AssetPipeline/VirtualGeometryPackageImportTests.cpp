#include "TestHarness.h"

#include "AssetImportResult.h"
#include "ModelPackageImporter.h"

#include <filesystem>
#include <fstream>
#include <iterator>
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
	}

	void RegisterVirtualGeometryPackageImportTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.PackageImport.CooksMultipleGeneratedMeshOutputs", [](TestContext& context) {
			const std::filesystem::path tempRoot = MakeVirtualGeometryPackageTempRoot("MultipleMeshes");
			nlohmann::json document = LoadBaseGltfFixture();

			// Duplicate the fixture mesh through a second source mesh and node.
			// The importer should keep both generated children while the package
			// compiler produces one shared TEMF payload for the static package.
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
			for (const TerraEngine::AssetPipeline::GeneratedModelMeshAsset& mesh : result.generatedMeshes)
			{
				context.Expect(mesh.virtualGeometryRequested, "Package mesh records should state that virtual geometry was requested.");
				context.Expect(mesh.virtualGeometryCooked, "Generated package mesh should validate a cooked virtual geometry payload.");
				context.ExpectEqual(std::string("Cooked"), mesh.virtualGeometryStatus, "Generated package mesh should report a cooked virtual geometry status.");
				context.Expect(mesh.virtualGeometryDetailLevels > 0u, "Cooked package mesh should expose a detail count.");
				context.Expect(mesh.virtualGeometryMeshlets > 0u, "Cooked package mesh should expose a meshlet count.");
				context.Expect(mesh.virtualGeometrySourceTriangles > 0u, "Cooked package mesh should expose source triangle count.");
			}

			const nlohmann::json sidecar = TerraEngine::AssetPipeline::ToJson(result);
			context.Expect(sidecar["generatedMeshes"][0]["virtualGeometry"].is_object(), "Sidecar JSON should persist virtual geometry metadata for generated meshes.");
			context.ExpectEqual(std::string("Cooked"), sidecar["generatedMeshes"][0]["virtualGeometry"].value("status", std::string()), "Sidecar JSON should expose the cooked status.");
			const nlohmann::json targets = sidecar["generatedMeshes"][0]["virtualGeometry"].value("targets", nlohmann::json::array());
			context.Expect(!targets.empty(), "Sidecar JSON should persist requested-target diagnostics for generated meshes.");
			context.Expect(targets.front().contains("desiredTriangleCount"), "Sidecar requested-target diagnostics should include desired triangle counts.");
			context.Expect(targets.front().contains("actualTriangleCount"), "Sidecar requested-target diagnostics should include actual generated or candidate triangle counts.");

			const std::filesystem::path sourceAssetPath = tempRoot / "Assets/Models/virtual-multi-mesh.gltf";
			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(sourceAssetPath, result), "Virtual geometry package sidecar should save.");
			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(sourceAssetPath, loaded), "Virtual geometry package sidecar should reload.");
			context.ExpectEqual(std::string("Cooked"), loaded.generatedMeshes.front().virtualGeometryStatus, "Virtual geometry status should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryCooked, "Virtual geometry cooked flag should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryClusterSummary.clusterCount > 0u, "Virtual geometry cluster summary should round-trip through the sidecar loader.");
			context.Expect(!loaded.generatedMeshes.front().virtualGeometryTargets.empty(), "Requested-target diagnostics should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryTargets.front().desiredTriangleCount > 0u, "Desired target counts should round-trip through the sidecar loader.");
			context.Expect(loaded.generatedMeshes.front().virtualGeometryTargets.front().actualTriangleCount > 0u, "Actual target counts should round-trip through the sidecar loader.");
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
				"UnsupportedAnimationIgnored",
				"UnsupportedSkinIgnored",
				"UnsupportedCameraIgnored",
				"UnsupportedLightIgnored",
				"UnsupportedConstraintIgnored"
			};
			for (const std::string& code : expectedCodes)
			{
				context.Expect(HasDiagnosticCode(result, code), "Unsupported package content should be reported through diagnostics.");
			}
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
	}
}
