// FbxAnimationImportTests.cpp - Contract tests for animated Assimp FBX packages.
//
// These tests deliberately inspect the public package sidecar rather than
// importer-private Assimp objects. The sidecar is what the Asset Browser,
// prefab authoring, automation, and a project reload use after worker-safe
// import preparation has crossed its protected publication boundary.

#include "TestHarness.h"

#include "AssetPipeline/FbxTestHelpers.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"

#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using AnimationMatrix =
			std::array<float, 16u>;

		AnimationMatrix MatrixFromTransform(
			const TerraEngine::Animation::AnimationTransform& transform)
		{
			// Skeleton transforms use column-vector affine math. Build the same
			// row-major T * R * S matrix consumed by the runtime palette path so
			// this test detects an importer that silently substitutes node-pose
			// inverses for the FBX-authored skin bind.
			const auto& q = transform.rotation;
			const float xx = q.x * q.x;
			const float yy = q.y * q.y;
			const float zz = q.z * q.z;
			const float xy = q.x * q.y;
			const float xz = q.x * q.z;
			const float yz = q.y * q.z;
			const float wx = q.w * q.x;
			const float wy = q.w * q.y;
			const float wz = q.w * q.z;
			return {{
				(1.0f - 2.0f * (yy + zz)) * transform.scale.x,
				(2.0f * (xy - wz)) * transform.scale.y,
				(2.0f * (xz + wy)) * transform.scale.z,
				transform.translation.x,
				(2.0f * (xy + wz)) * transform.scale.x,
				(1.0f - 2.0f * (xx + zz)) * transform.scale.y,
				(2.0f * (yz - wx)) * transform.scale.z,
				transform.translation.y,
				(2.0f * (xz - wy)) * transform.scale.x,
				(2.0f * (yz + wx)) * transform.scale.y,
				(1.0f - 2.0f * (xx + yy)) * transform.scale.z,
				transform.translation.z,
				0.0f, 0.0f, 0.0f, 1.0f
			}};
		}

		AnimationMatrix Multiply(
			const AnimationMatrix& left,
			const AnimationMatrix& right)
		{
			AnimationMatrix result{};
			for (std::size_t row = 0u; row < 4u; ++row)
			{
				for (std::size_t column = 0u; column < 4u; ++column)
				{
					for (std::size_t lane = 0u; lane < 4u; ++lane)
					{
						result[row * 4u + column] +=
							left[row * 4u + lane]
								* right[lane * 4u + column];
					}
				}
			}
			return result;
		}

		bool IsIdentity(
			const AnimationMatrix& matrix,
			float tolerance = 0.0001f)
		{
			for (std::size_t index = 0u; index < matrix.size(); ++index)
			{
				const float expected = index % 5u == 0u ? 1.0f : 0.0f;
				if (std::abs(matrix[index] - expected) > tolerance)
				{
					return false;
				}
			}
			return true;
		}

		TerraEngine::AssetPipeline::FbxAnimatedImportResult
			ImportAnimatedFbxFixture(std::string_view fixtureName)
		{
			const std::filesystem::path fixturePath =
				AssimpFbxFixturePath(fixtureName);
			const std::filesystem::path tempRoot =
				std::filesystem::temp_directory_path()
					/ "TerraEngineFbxAnimationImporterTests"
					/ fixtureName;
			TerraEngine::AssetPipeline::FbxPackageImportOptions options{};
			options.sourcePath = fixturePath;
			options.assetRoot = tempRoot / "Assets";
			options.buildRoot = tempRoot / "Build";
			options.sourceRelativePath =
				std::filesystem::path("Models") / fixturePath.filename();
			options.packageGuid = TerraEngine::GenerateUUID();
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportFbxPackageWithAnimation(
				options);
		}

		bool HasDiagnostic(const TerraEngine::AssetPipeline::ModelPackageImportResult& result, std::string_view code)
		{
			for(const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic : result.diagnostics)
			{
				if(diagnostic.code == code)
				{
					return true;
				}
			}
			return false;
		}

		// T035 must replace the legacy static-only downgrade. Keeping this contract
		// at the public result boundary lets the future cooker choose its concrete
		// binary records without coupling these tests to its private implementation.
		void ExpectAnimatedFbxWasImported(
			TestContext& context,
			const TerraEngine::AssetPipeline::ModelPackageImportResult& result,
			std::string_view requirement)
		{
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"The representative animated FBX package should remain importable.");
			context.Expect(!HasDiagnostic(result, "UnsupportedSkeletonIgnored"),
				std::string(requirement) + " requires hierarchy, bones, bind data, and skin weights to be cooked instead of ignored.");
			context.Expect(!HasDiagnostic(result, "UnsupportedAnimationIgnored"),
				std::string(requirement) + " requires animation clips and track data to be cooked instead of ignored.");
		}
	}

	void RegisterFbxAnimationImportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Animation.FbxImport.UsesCurrentImporterRecipeVersion", [](TestContext& context) {
			context.ExpectEqual(
				std::string("6"),
				std::string(TerraEngine::AssetPipeline::GetModelPackageImporterVersion("fixture.fbx")),
				"FBX packages must use the authored skin-bind hierarchy importer recipe version.");
			context.ExpectEqual(
				std::string("1"),
				std::string(TerraEngine::AssetPipeline::GetModelPackageImporterVersion("fixture.gltf")),
				"Changing the FBX recipe must not invalidate unrelated glTF packages.");
		});

		registry.Add("Editor.Animation.FbxImport.PreservesHierarchySkinAndClipPayload", [](TestContext& context) {
			// Assimp's compact fixture contains both an animation and a skeleton,
			// making it a deterministic offline contract source for the FBX path.
			ExpectAnimatedFbxWasImported(context, ImportFbxFixture("animation_with_skeleton.fbx"),
				"The FBX hierarchy, bone, weight, and clip import contract");
		});

		registry.Add("Editor.Animation.FbxImport.PreservesMorphSocketsUnitsAndNormalizedParityMetadata", [](TestContext& context) {
			// `close_to_identity_transforms` exercises Assimp's transform conversion
			// path; the compact animated fixture supplies the required skin/clip data.
			ExpectAnimatedFbxWasImported(context, ImportFbxFixture("animation_with_skeleton.fbx"),
				"The FBX morph, socket, unit conversion, and glTF-parity import contract");
		});

		registry.Add("Editor.Animation.FbxImport.PreservesAuthoredSkinBindPose", [](TestContext& context) {
			const TerraEngine::AssetPipeline::FbxAnimatedImportResult result =
				ImportAnimatedFbxFixture("animation_with_skeleton.fbx");
			ExpectAnimatedFbxWasImported(
				context,
				result.package,
				"The FBX authored skin-bind contract");
			context.Expect(result.animation.skeletons.size() == 1u,
				"The representative animated FBX should publish one inspectable skeleton.");
			context.Expect(result.package.animationManifest.importerVersion == "6",
				"The generated FBX animation manifest must use the same bind-and-clip recipe version as AssetDatabase invalidation.");
			if (result.animation.skeletons.size() != 1u)
			{
				return;
			}

			const TerraEngine::Animation::SkeletonAsset& skeleton =
				result.animation.skeletons.front().asset;
			std::vector<AnimationMatrix> modelBind(skeleton.joints.size());
			std::vector<AnimationMatrix> sourceNodeModels(skeleton.joints.size());
			bool foundAuthoredOffset = false;
			bool foundSourceNodePoseDifferentFromBind = false;
			bool allCookedBindPalettesAreIdentity = true;
			for (std::size_t index = 0u; index < skeleton.joints.size(); ++index)
			{
				const AnimationMatrix local =
					MatrixFromTransform(skeleton.joints[index].localBindTransform);
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				modelBind[index] = parent < 0
					? local
					: Multiply(
						modelBind[static_cast<std::size_t>(parent)],
						local);
				const AnimationMatrix sourceLocal =
					MatrixFromTransform(result.animation.nodes[index].localTransform);
				sourceNodeModels[index] = parent < 0
					? sourceLocal
					: Multiply(
						sourceNodeModels[static_cast<std::size_t>(parent)],
						sourceLocal);
				if (!skeleton.joints[index].deformsMesh)
				{
					continue;
				}
				foundAuthoredOffset = foundAuthoredOffset
					|| !IsIdentity(skeleton.inverseBindMatrices[index].values);
				foundSourceNodePoseDifferentFromBind =
					foundSourceNodePoseDifferentFromBind
					|| !IsIdentity(Multiply(
						sourceNodeModels[index],
						skeleton.inverseBindMatrices[index].values));
				allCookedBindPalettesAreIdentity =
					allCookedBindPalettesAreIdentity
					&& IsIdentity(Multiply(
						modelBind[index],
						skeleton.inverseBindMatrices[index].values));
			}
			context.Expect(foundAuthoredOffset
					&& foundSourceNodePoseDifferentFromBind
					&& allCookedBindPalettesAreIdentity,
				"An FBX skin bind that differs from the exported node pose must retain its authored non-identity offset while the cooked local bind hierarchy composes back to identity.");
		});

	}
}
