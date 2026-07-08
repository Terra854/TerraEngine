#include "TestHarness.h"

#include "AssetImportResult.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	void RegisterModelPackageImportResultTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.ModelPackageImportResult.RoundTripsFbxSidecar", [](TestContext& context) {
			const std::filesystem::path tempSource = std::filesystem::temp_directory_path() / "TerraEngineModelPackageResultTests" / "Models" / "RoundTrip.fbx";

			TerraEngine::AssetPipeline::ModelPackageImportResult result{};
			result.status = TerraEngine::AssetPipeline::ModelImportStatus::SucceededWithWarnings;
			result.packageGuid = TerraEngine::GenerateUUID();
			result.package.sourcePath = "Models/RoundTrip.fbx";
			result.package.packageKind = TerraEngine::AssetPipeline::ModelPackageKind::Fbx;
			result.package.sourceHashHex = "abc123";
			result.package.importRoot = "Models/RoundTrip_Generated";

			TerraEngine::AssetPipeline::GeneratedModelMeshAsset mesh{};
			mesh.guid = result.packageGuid;
			mesh.sourceMeshIndex = 1u;
			mesh.sourcePrimitiveIndex = 2u;
			mesh.sourceNodePath = "Root/Node";
			mesh.sourcePath = result.package.sourcePath;
			mesh.compiledPath = "Models/RoundTrip.temf";
			mesh.displayName = "RoundTrip";
			mesh.localBoundsMin = { -1.0f, -2.0f, -3.0f };
			mesh.localBoundsMax = { 1.0f, 2.0f, 3.0f };
			result.generatedMeshes.push_back(mesh);

			TerraEngine::AssetPipeline::GeneratedModelPrefabAsset prefab{};
			prefab.guid = TerraEngine::GenerateUUID();
			prefab.sourcePath = "Models/RoundTrip_Generated/RoundTrip.teprefab";
			prefab.displayName = "RoundTrip";
			prefab.meshGuids.push_back(mesh.guid);
			result.generatedPrefabs.push_back(prefab);

			TerraEngine::AssetPipeline::ModelImportDiagnostic diagnostic{};
			diagnostic.severity = TerraEngine::AssetPipeline::ModelDiagnosticSeverity::Warning;
			diagnostic.code = "UnsupportedAnimationIgnored";
			diagnostic.message = "Animation ignored.";
			result.diagnostics.push_back(diagnostic);

			context.Expect(TerraEngine::AssetPipeline::SaveModelPackageImportResult(tempSource, result), "Sidecar save should succeed.");

			TerraEngine::AssetPipeline::ModelPackageImportResult loaded;
			context.Expect(TerraEngine::AssetPipeline::LoadModelPackageImportResult(tempSource, loaded), "Sidecar load should succeed.");
			context.Expect(loaded.package.packageKind == TerraEngine::AssetPipeline::ModelPackageKind::Fbx, "Package kind should round-trip as FBX.");
			context.ExpectEqual(std::string("Root/Node"), loaded.generatedMeshes.front().sourceNodePath, "Mesh node path should round-trip.");
			context.ExpectEqual(static_cast<std::size_t>(1), loaded.generatedPrefabs.size(), "Generated prefab records should round-trip.");
			context.ExpectEqual(std::string("Models/RoundTrip_Generated/RoundTrip.teprefab"), loaded.generatedPrefabs.front().sourcePath.generic_string(), "Generated prefab source path should round-trip.");
			context.ExpectEqual(static_cast<std::size_t>(1), loaded.generatedPrefabs.front().meshGuids.size(), "Generated prefab mesh references should round-trip.");
			context.ExpectEqual(static_cast<std::size_t>(1), loaded.diagnostics.size(), "Diagnostics should round-trip.");
		});
	}
}
