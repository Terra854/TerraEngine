// GltfAnimationImportTests.cpp - Contract-first coverage for animated glTF packages.
//
// These tests deliberately describe the import result required by animation production validation before
// ImplementationCheck extends the production importer.  The legacy importer reports that it
// discarded animation and skin data, so each test currently fails for that
// explicit, actionable reason rather than silently accepting a static fallback.

#include "TestHarness.h"

#include "GltfPackageImporter.h"
#include "ModelPackageImporter.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
		using TerraEngine::AssetPipeline::GltfImportDiagnostic;
		using TerraEngine::AssetPipeline::GltfImportResult;
		using TerraEngine::AssetPipeline::GltfPackageImportOptions;
		using TerraEngine::AssetPipeline::ModelPackageImportOptions;
		using TerraEngine::AssetPipeline::ModelPackageImportResult;

		// Load the small committed triangle so every contract fixture remains
		// deterministic, self-contained, and independent of downloaded models.
		nlohmann::json LoadBaseDocument()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			std::ifstream input(sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf");
			return nlohmann::json::parse(input);
		}

		// Add validly-shaped package metadata for the capabilities that ImplementationCheck must
		// preserve.  The current importer intentionally ignores it, creating the
		// red state that protects the later implementation from static regressions.
		void AddAnimatedSkinContractData(nlohmann::json& document, std::string_view interpolation, bool includeSkin = true)
		{
			document["nodes"][0]["name"] = "AnimatedCharacter";
			document["nodes"].push_back({
				{ "name", "HandSocket" },
				{ "translation", { 0.0, 1.0, 0.0 } },
				{ "extras", { { "terraEngineSocket", "RightHand" } } }
			});
			document["nodes"][0]["children"] = { 1 };
			if(includeSkin)
			{
				document["nodes"][0]["skin"] = 0;
				document["skins"] = nlohmann::json::array({ {
					{ "name", "HeroSkin" },
					{ "joints", { 1 } },
					// The accessor reference is intentionally part of the test input even
					// though the base fixture does not need its values to demonstrate the
					// present importer omission.  ImplementationCheck will validate and consume it.
					{ "inverseBindMatrices", 0 }
				} });
			}
			document["animations"] = nlohmann::json::array({ {
				{ "name", std::string("ContractClip_") + std::string(interpolation) },
				{ "samplers", nlohmann::json::array() },
				{ "channels", nlohmann::json::array() },
				{ "extras", { { "requiredInterpolation", interpolation } } }
			} });
		}

		// Write each fixture beneath a unique temporary directory so package roots
		// cannot alias and the multi-package test exercises actual independent I/O.
		std::filesystem::path WriteFixture(std::string_view name, const nlohmann::json& document)
		{
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineEditorAnimationTests" / "GltfAnimationImport" / std::string(name);
			std::filesystem::create_directories(root);
			const std::filesystem::path fixturePath = root / "animated-contract.gltf";
			std::ofstream output(fixturePath, std::ios::binary | std::ios::trunc);
			output << document.dump(2);
			return fixturePath;
		}

		// Import via the production package entry point.  Tests own only the
		// temporary asset/build roots; production code remains responsible for
		// serialization and package publication.
		GltfImportResult ImportContractFixture(std::string_view name, const nlohmann::json& document)
		{
			const std::filesystem::path fixturePath = WriteFixture(name, document);
			const std::filesystem::path root = fixturePath.parent_path();
			GltfPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = root / "Assets";
			options.buildRoot = root / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / fixturePath.filename();
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportGltfPackage(options);
		}

		bool HasDiagnostic(const GltfImportResult& result, std::string_view code)
		{
			for(const GltfImportDiagnostic& diagnostic : result.diagnostics)
			{
				if(diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		// Read a published binary without depending on the source package. This
		// verifies that the runtime assets retain the node transform after the .gltf
		// and its external buffer are no longer available to the importer.
		std::vector<std::uint8_t> ReadBinaryFile(const std::filesystem::path& path)
		{
			std::ifstream input(path, std::ios::binary);
			return {
				std::istreambuf_iterator<char>(input),
				std::istreambuf_iterator<char>()
			};
		}

		ModelPackageImportResult ImportAnimatedMorphCubeFixture()
		{
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path committedFixture = sourceRoot
				/ "External/assimp/test/models/glTF2/glTF-Sample-Models/AnimatedMorphCube-glTF";
			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineEditorAnimationTests" / "AnimatedMorphCubeNodeTransform";
			std::filesystem::create_directories(root);
			// AnimatedMorphCube uses an external binary buffer. Copy both files into
			// the disposable fixture so import output cannot accidentally read from the
			// committed third-party sample after publication.
			std::filesystem::copy_file(committedFixture / "AnimatedMorphCube.gltf",
				root / "AnimatedMorphCube.gltf", std::filesystem::copy_options::overwrite_existing);
			std::filesystem::copy_file(committedFixture / "AnimatedMorphCube.bin",
				root / "AnimatedMorphCube.bin", std::filesystem::copy_options::overwrite_existing);

			ModelPackageImportOptions options{};
			options.sourcePath = root / "AnimatedMorphCube.gltf";
			options.assetRoot = root / "Assets";
			options.buildRoot = root / "Build";
			options.sourceRelativePath = std::filesystem::path("Models") / options.sourcePath.filename();
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportModelPackage(options);
		}

		bool NearlyEqual(float left, float right, float tolerance = 0.0001F)
		{
			return std::abs(left - right) <= tolerance;
		}

		// A package that contains animated skin data must not be downgraded to
		// static geometry.  This single contract is intentionally shared by the
		// focused cases below, while their inputs document the distinct features.
		void ExpectAnimatedDataWasImported(TestContext& context, const GltfImportResult& result, std::string_view requirement)
		{
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed,
				"The representative animated package should remain importable.");
			context.Expect(!HasDiagnostic(result, "UnsupportedSkinIgnored"),
				std::string(requirement) + " requires skin and inverse-bind data to be cooked instead of ignored.");
			context.Expect(!HasDiagnostic(result, "UnsupportedAnimationIgnored"),
				std::string(requirement) + " requires animation data to be cooked instead of ignored.");
		}
	}

	void RegisterGltfAnimationImportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.GltfImport.SkinAndInverseBindsAreCooked", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR");
			ExpectAnimatedDataWasImported(context, ImportContractFixture("skin-inverse-binds", document),
				"The glTF skin/inverse-bind import contract");
		});

		registry.Add("Editor.Animation.GltfImport.PreservesAnimationChannels", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR", false);
			document["animations"][0]["extras"]["requiredChannels"] = { "translation", "rotation", "scale" };
			ExpectAnimatedDataWasImported(context, ImportContractFixture("animation-channels", document),
				"The glTF animation-channel import contract");
		});

		registry.Add("Editor.Animation.GltfImport.PreservesInterpolationModes", [](TestContext& context) {
			for(const std::string_view interpolation : { "STEP", "LINEAR", "CUBICSPLINE" })
			{
				nlohmann::json document = LoadBaseDocument();
				AddAnimatedSkinContractData(document, interpolation, false);
				ExpectAnimatedDataWasImported(context, ImportContractFixture(std::string("interpolation-") + std::string(interpolation), document),
					"The glTF interpolation import contract");
			}
		});

		registry.Add("Editor.Animation.GltfImport.PreservesMorphTargets", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR");
			document["meshes"][0]["weights"] = { 0.0 };
			document["meshes"][0]["primitives"][0]["targets"] = nlohmann::json::array({ { { "POSITION", 0 } } });
			ExpectAnimatedDataWasImported(context, ImportContractFixture("morph-target", document),
				"The glTF morph-target import contract");
		});

		registry.Add("Editor.Animation.GltfImport.PreservesNamedSockets", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR");
			document["nodes"][1]["extras"]["terraEngineSocket"] = "WeaponSocket";
			ExpectAnimatedDataWasImported(context, ImportContractFixture("named-socket", document),
				"The glTF socket and nested-transform import contract");
		});

		registry.Add("Editor.Animation.GltfImport.DisambiguatesRepeatedInferredJointRoles", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			// Two morph-only mesh nodes with the same display name are both included
			// in the synthetic scene skeleton. Their stable paths remain distinct,
			// and import must turn those paths into unique serializable joint roles.
			document["meshes"][0]["weights"] = { 0.0 };
			document["meshes"][0]["primitives"][0]["targets"] = nlohmann::json::array({ { { "POSITION", 0 } } });
			document["nodes"][0]["name"] = "RepeatedNode";
			document["nodes"].push_back({ { "name", "RepeatedNode" }, { "mesh", 0 } });
			document["scenes"][0]["nodes"] = { 0, 1 };
			const GltfImportResult result = ImportContractFixture("repeated-inferred-joint-role", document);
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed
				&& !HasDiagnostic(result, "SkeletonSerializeFailed"),
				"Repeated inferred node roles must receive deterministic stable-id suffixes instead of rejecting the package.");
		});

		registry.Add("Editor.Animation.GltfImport.CoalescesIdenticalSkinRecords", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR");
			// Some exporters repeat one unnamed skin for multiple mesh nodes. The
			// structural joint signature is identical, so both source indices should
			// resolve through one skeleton and one set of animation subassets.
			document["skins"].push_back(document["skins"][0]);
			const GltfImportResult result = ImportContractFixture("identical-skin-records", document);
			context.Expect(result.status != TerraEngine::AssetPipeline::GltfImportStatus::Failed
				&& !HasDiagnostic(result, "AnimationImportManifestInvalid"),
				"Identical source skins must share one canonical skeleton identity instead of publishing duplicate manifest children.");
		});

		registry.Add("Editor.Animation.GltfImport.NormalizesCoordinateConventionsOnce", [](TestContext& context) {
			nlohmann::json document = LoadBaseDocument();
			AddAnimatedSkinContractData(document, "LINEAR");
			document["asset"]["extras"] = {
				{ "sourceHandedness", "Left" },
				{ "sourceUpAxis", "Z" },
				{ "sourceUnitScale", 0.01 }
			};
			ExpectAnimatedDataWasImported(context, ImportContractFixture("coordinate-conversion", document),
				"The glTF coordinate, axis, and unit conversion contract");
		});

		registry.Add("Editor.Animation.GltfImport.KeepsMultiplePackagesIndependent", [](TestContext& context) {
			nlohmann::json firstDocument = LoadBaseDocument();
			AddAnimatedSkinContractData(firstDocument, "STEP");
			nlohmann::json secondDocument = LoadBaseDocument();
			AddAnimatedSkinContractData(secondDocument, "CUBICSPLINE");

			const GltfImportResult first = ImportContractFixture("multi-package-first", firstDocument);
			const GltfImportResult second = ImportContractFixture("multi-package-second", secondDocument);
			ExpectAnimatedDataWasImported(context, first, "The first independent animated glTF package");
			ExpectAnimatedDataWasImported(context, second, "The second independent animated glTF package");
			context.Expect(first.packageGuid != second.packageGuid,
				"Separate source packages must retain distinct package identities.");
		});

		registry.Add("Editor.Animation.GltfImport.BindsMorphOnlyNodeTransformThroughPalette", [](TestContext& context) {
			const ModelPackageImportResult result = ImportAnimatedMorphCubeFixture();
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"The committed AnimatedMorphCube package should import as a complete model package.");
			const auto skeletonRecord = std::find_if(result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& asset) {
				return asset.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Skeleton;
			});
			const auto deformableRecord = std::find_if(result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& asset) {
				return asset.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::DeformableMesh;
			});
			context.Expect(skeletonRecord != result.animationManifest.subAssets.end()
				&& deformableRecord != result.animationManifest.subAssets.end()
				&& result.generatedMeshes.size() == 1U && !result.generatedPrefabs.empty(),
				"Morph-only import must publish one skeleton-owned deformable mesh instead of an identity-bound fallback.");
			if(skeletonRecord == result.animationManifest.subAssets.end()
				|| deformableRecord == result.animationManifest.subAssets.end()
				|| result.generatedMeshes.empty() || result.generatedPrefabs.empty()) return;

			const std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineEditorAnimationTests" / "AnimatedMorphCubeNodeTransform";
			const std::vector<std::uint8_t> skeletonBytes = ReadBinaryFile(root / "Build" / skeletonRecord->compiledPath);
			TerraEngine::Animation::SkeletonAsset skeleton;
			context.Expect(TerraEngine::Animation::DeserializeSkeletonAsset(
				skeletonBytes, skeletonRecord->compatibilityFingerprint, skeleton)
				== TerraEngine::Animation::AnimationStatus::Ok,
				"The published synthetic skeleton must remain loadable without the source glTF.");
			if(skeleton.joints.empty()) return;
			const auto joint = std::find_if(skeleton.joints.begin(), skeleton.joints.end(), [](const auto& candidate) {
				return candidate.displayName == "AnimatedMorphCube";
			});
			// Unit quaternions q and -q encode the same rotation. Matrix-to-quaternion
			// reconstruction may choose either representative for this exact 180-degree
			// source rotation, so compare the absolute quaternion dot product.
			const float sourceRotationDot = joint == skeleton.joints.end()
				? 0.0F
				: joint->localBindTransform.rotation.y * 0.7071067F
					+ joint->localBindTransform.rotation.z * -0.7071068F;
			context.Expect(joint != skeleton.joints.end()
				&& NearlyEqual(std::abs(sourceRotationDot), 1.0F)
				&& NearlyEqual(joint->localBindTransform.scale.x, 100.0F)
				&& NearlyEqual(joint->localBindTransform.scale.y, 100.0F)
				&& NearlyEqual(joint->localBindTransform.scale.z, 100.0F),
				"The source node quaternion and 100x scale must survive in the skeleton bind pose exactly once.");
			if(joint == skeleton.joints.end()) return;

			const AnimationResourceValidationResult canonicalMeshValidation =
				ResourceManager::Instance()
				.ValidateDeformableMeshFile(root / "Build" / result.generatedMeshes.front().compiledPath,
					static_cast<std::uint32_t>(skeleton.joints.size()), skeleton.compatibilityFingerprint);
			context.Expect(canonicalMeshValidation.status == TerraEngine::Animation::AnimationStatus::Ok,
				"The imported mesh must decode through canonical TEMFPRD1 deformable streams.");

			std::ifstream prefabFile(root / "Assets" / result.generatedPrefabs.front().sourcePath);
			nlohmann::json prefab = nlohmann::json::parse(prefabFile);
			const nlohmann::json& prefabRoot = prefab["objects"][0U];
			const nlohmann::json& meshObject = prefabRoot["children"][0U];
			context.Expect(prefabRoot["components"]["AnimatorComponent"]["skeletonOverrideAssetId"]
				== TerraEngine::ToHexString(skeletonRecord->guid)
				&& meshObject["components"].count("AnimatorComponent") == 0U
				&& meshObject["components"]["Transform"]["Scale"] == nlohmann::json::array({ 1.0F, 1.0F, 1.0F }),
				"The prefab root must own the palette animator while its mesh child stays identity-transformed, preventing a double node scale.");

			std::filesystem::remove(root / "AnimatedMorphCube.gltf");
			std::filesystem::remove(root / "AnimatedMorphCube.bin");
			context.Expect(std::filesystem::is_regular_file(root / "Build" / skeletonRecord->compiledPath)
				&& std::filesystem::is_regular_file(root / "Build" / result.generatedMeshes.front().compiledPath),
				"The generated skeleton and deformable mesh must remain source-free runtime outputs after their fixture files are removed.");
		});
	}
}
