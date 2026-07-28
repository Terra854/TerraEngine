#include "TestHarness.h"

#include "FbxTestHelpers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <assimp/Importer.hpp>
#include <assimp/config.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using Matrix4 = std::array<float, 16u>;

		Matrix4 IdentityMatrix()
		{
			return {{
				1.0f, 0.0f, 0.0f, 0.0f,
				0.0f, 1.0f, 0.0f, 0.0f,
				0.0f, 0.0f, 1.0f, 0.0f,
				0.0f, 0.0f, 0.0f, 1.0f
			}};
		}

		Matrix4 ToMatrix(const aiMatrix4x4& source)
		{
			return {{
				source.a1, source.a2, source.a3, source.a4,
				source.b1, source.b2, source.b3, source.b4,
				source.c1, source.c2, source.c3, source.c4,
				source.d1, source.d2, source.d3, source.d4
			}};
		}

		Matrix4 Multiply(const Matrix4& left, const Matrix4& right)
		{
			Matrix4 result{};
			for (std::size_t row = 0u; row < 4u; ++row)
			{
				for (std::size_t column = 0u; column < 4u; ++column)
				{
					for (std::size_t lane = 0u; lane < 4u; ++lane)
					{
						result[row * 4u + column] += left[row * 4u + lane] * right[lane * 4u + column];
					}
				}
			}
			return result;
		}

		std::array<float, 3u> TransformPoint(const Matrix4& transform, const aiVector3D& point)
		{
			return {{
				transform[0u] * point.x + transform[1u] * point.y + transform[2u] * point.z + transform[3u],
				transform[4u] * point.x + transform[5u] * point.y + transform[6u] * point.z + transform[7u],
				transform[8u] * point.x + transform[9u] * point.y + transform[10u] * point.z + transform[11u]
			}};
		}

		std::array<float, 3u> TransformPoint(const Matrix4& transform, const std::array<float, 3u>& point)
		{
			return {{
				transform[0u] * point[0u] + transform[1u] * point[1u] + transform[2u] * point[2u] + transform[3u],
				transform[4u] * point[0u] + transform[5u] * point[1u] + transform[6u] * point[2u] + transform[7u],
				transform[8u] * point[0u] + transform[9u] * point[1u] + transform[10u] * point[2u] + transform[11u]
			}};
		}

		Matrix4 ToMatrix(const TerraEngine::Animation::AnimationTransform& transform)
		{
			// Match the CPU palette's T * R * S convention so this importer test
			// proves the exact bind-pose matrix operation used by both render modes.
			const float x = transform.rotation.x;
			const float y = transform.rotation.y;
			const float z = transform.rotation.z;
			const float w = transform.rotation.w;
			const float xx = x * x;
			const float yy = y * y;
			const float zz = z * z;
			const float xy = x * y;
			const float xz = x * z;
			const float yz = y * z;
			const float wx = w * x;
			const float wy = w * y;
			const float wz = w * z;
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

		void CollectSourceMeshPlacements(
			const aiScene& scene,
			const aiNode& node,
			const Matrix4& parent,
			std::unordered_map<unsigned int, std::vector<Matrix4>>& placements)
		{
			const Matrix4 global = Multiply(parent, ToMatrix(node.mTransformation));
			for (unsigned int index = 0u; index < node.mNumMeshes; ++index)
			{
				if (node.mMeshes[index] < scene.mNumMeshes)
				{
					placements[node.mMeshes[index]].push_back(global);
				}
			}
			for (unsigned int index = 0u; index < node.mNumChildren; ++index)
			{
				if (node.mChildren[index] != nullptr)
				{
					CollectSourceMeshPlacements(scene, *node.mChildren[index], global, placements);
				}
			}
		}

		float Distance(const std::array<float, 3u>& left, const std::array<float, 3u>& right)
		{
			const float x = left[0u] - right[0u];
			const float y = left[1u] - right[1u];
			const float z = left[2u] - right[2u];
			return std::sqrt(x * x + y * y + z * z);
		}

		bool HasDiagnostic(
			const TerraEngine::AssetPipeline::ModelPackageImportResult& result,
			std::string_view code)
		{
			return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [code](const auto& diagnostic) {
				return diagnostic.code == code;
			});
		}

		TerraEngine::AssetPipeline::FbxAnimatedImportResult ImportQuaterniusBaseCharacter(
			const TerraEngine::UUID& packageGuid)
		{
			// The Spec 043 validation corpus retains the exact CC0 Quaternius source
			// used to reproduce this importer bug. Keep the path outside committed
			// fixtures because it is a 2 MiB validation input, while the test pins the
			// behavior whenever that reproducible corpus is provisioned.
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "out/validation/animation-model-sources/quaternius/BaseCharacter.fbx";
			TerraEngine::AssetPipeline::FbxPackageImportOptions options{};
			options.sourcePath = sourcePath;
			options.assetRoot = std::filesystem::temp_directory_path() / "TerraEngineFbxImporterTests" / "QuaterniusBaseCharacter" / "Assets";
			options.buildRoot = std::filesystem::temp_directory_path() / "TerraEngineFbxImporterTests" / "QuaterniusBaseCharacter" / "Build";
			options.sourceRelativePath = "Models/Quaternius/BaseCharacter.fbx";
			options.packageGuid = packageGuid;
			options.forceReimport = true;
			return TerraEngine::AssetPipeline::ImportFbxPackageWithAnimation(options);
		}
	}

	void RegisterFbxImportDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("Editor.AssetPipeline.FbxImportDiagnostics.ReportsImportedAnimationAndSkeletonSummary", [](TestContext& context) {
			const auto result = ImportFbxFixture("animation_with_skeleton.fbx");
			context.Expect(result.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"Animated FBX should publish its supported static and animation data.");
			const bool hasAnimationWarning = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code == "UnsupportedAnimationIgnored";
			});
			const bool hasSkeletonWarning = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code == "UnsupportedSkeletonIgnored";
			});
			const bool hasAnimationSummary = std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [](const TerraEngine::AssetPipeline::ModelImportDiagnostic& diagnostic) {
				return diagnostic.code == "FbxAnimationImportSummary";
			});
			const bool hasSkeletonChild = std::any_of(result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& child) {
				return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::Skeleton;
			});
			const bool hasClipChild = std::any_of(result.animationManifest.subAssets.begin(), result.animationManifest.subAssets.end(), [](const auto& child) {
				return child.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::AnimationClip;
			});
			context.Expect(!hasAnimationWarning && !hasSkeletonWarning,
				"Supported FBX skeleton and animation data must no longer use legacy ignored-feature warnings.");
			context.Expect(hasAnimationSummary && hasSkeletonChild && hasClipChild,
				"Importer diagnostics and manifest must expose the cooked skeleton/clip result.");
		});

		registry.Add("Editor.AssetPipeline.FbxImportDiagnostics.QuaterniusMultiMaterialSkinSectionsRemainDistinct", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "out/validation/animation-model-sources/quaternius/BaseCharacter.fbx";
			if (!std::filesystem::exists(sourcePath))
			{
				// The pinned real-model corpus is acquired only during final validation.
				// Clean checkouts still run the committed FBX fixture coverage; when the
				// corpus exists, this named regression adds the exact external case.
				context.Expect(true,
					"The optional Quaternius regression awaits the hash-verified final-validation corpus.");
				return;
			}

			const auto result = ImportQuaterniusBaseCharacter(TerraEngine::GenerateUUID());
			context.Expect(result.package.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed,
				"Assimp-split BaseCharacter skin sections should still cook as one publishable FBX package.");
			context.Expect(!HasDiagnostic(result.package, "FbxAmbiguousSkinnedMeshIdentity"),
				"Same-named skinned mesh material sections must not be rejected as one ambiguous mesh identity.");
			context.ExpectEqual(static_cast<std::size_t>(2u), result.animation.skinnedMeshes.size(),
				"BaseCharacter's two Assimp material sections must both remain represented for deformable cooking.");

			std::set<std::string> sectionKeys;
			for (const auto& skin : result.animation.skinnedMeshes)
			{
				sectionKeys.insert(skin.semanticKey);
			}
			context.ExpectEqual(result.animation.skinnedMeshes.size(), sectionKeys.size(),
				"Each cooked skin section must retain a distinct deterministic semantic key.");
			const bool hasDeformableMesh = std::any_of(result.package.animationManifest.subAssets.begin(), result.package.animationManifest.subAssets.end(), [](const auto& asset) {
				return asset.kind == TerraEngine::AssetPipeline::AnimationImportSubAssetKind::DeformableMesh;
			});
			context.Expect(hasDeformableMesh,
				"The preserved skinned sections must contribute to the generated deformable mesh subasset.");
		});

		registry.Add("Editor.AssetPipeline.FbxImportDiagnostics.QuaterniusDuplicateBodyTargetSelectsDeformingNode", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "out/validation/animation-model-sources/quaternius/BaseCharacter.fbx";
			if (!std::filesystem::exists(sourcePath))
			{
				// See the paired material-section regression above: acquisition is not a
				// build prerequisite, but a provisioned final corpus must exercise this.
				context.Expect(true,
					"The optional Quaternius regression awaits the hash-verified final-validation corpus.");
				return;
			}

			const auto result = ImportQuaterniusBaseCharacter(TerraEngine::GenerateUUID());
			context.Expect(!HasDiagnostic(result.package, "FbxAnimationTargetMissing"),
				"A duplicate Body name must resolve to its unique deforming node instead of rejecting the clip.");
			context.Expect(!result.animation.clips.empty(),
				"BaseCharacter animation must remain cooked after resolving its duplicate Body target.");
			std::set<std::uint64_t> deformingJointIds;
			for (const auto& skin : result.animation.skinnedMeshes)
			{
				for (const auto& influence : skin.influences)
				{
					deformingJointIds.insert(influence.jointStableId);
				}
			}
			const bool targetsDeformingBody = std::any_of(result.animation.clips.begin(), result.animation.clips.end(), [&deformingJointIds](const auto& clip) {
				return std::any_of(clip.nodeChannels.begin(), clip.nodeChannels.end(), [&deformingJointIds](const auto& channel) {
					return channel.targetSemanticKey.find("body") != std::string::npos
						&& deformingJointIds.contains(channel.targetStableId);
				});
			});
			context.Expect(targetsDeformingBody,
				"The cooked Body channel must target the skin's deforming hierarchy node, not its render-mesh namesake.");
		});

		registry.Add("Editor.AssetPipeline.FbxImportDiagnostics.QuaterniusBindPosePreservesBakedMultiMeshVertices", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "out/validation/animation-model-sources/quaternius/BaseCharacter.fbx";
			if (!std::filesystem::exists(sourcePath))
			{
				context.Expect(true,
					"The optional Quaternius bind-pose regression awaits the hash-verified final-validation corpus.");
				return;
			}

			const auto result = ImportQuaterniusBaseCharacter(TerraEngine::GenerateUUID());
			context.Expect(result.package.status != TerraEngine::AssetPipeline::ModelImportStatus::Failed
				&& result.animation.skeletons.size() == 1u
				&& result.animation.skinnedMeshes.size() == 2u,
				"The BaseCharacter regression requires its two cooked skin sections and one skeleton.");
			if (result.animation.skeletons.size() != 1u)
			{
				return;
			}

			Assimp::Importer importer;
			importer.SetPropertyBool(AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS, false);
			const aiScene* scene = importer.ReadFile(sourcePath.string(),
				aiProcess_Triangulate | aiProcess_SortByPType | aiProcess_JoinIdenticalVertices
				| aiProcess_FindInvalidData | aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace
				| aiProcess_ImproveCacheLocality | aiProcess_GlobalScale | aiProcess_PopulateArmatureData);
			context.Expect(scene != nullptr && scene->mRootNode != nullptr,
				"The independent source read must reproduce the importer normalization before checking bind deformation.");
			if (scene == nullptr || scene->mRootNode == nullptr)
			{
				return;
			}

			std::unordered_map<unsigned int, std::vector<Matrix4>> meshPlacements;
			CollectSourceMeshPlacements(*scene, *scene->mRootNode, IdentityMatrix(), meshPlacements);
			const auto& skeleton = result.animation.skeletons.front().asset;
			std::vector<Matrix4> bindModels(skeleton.joints.size(), IdentityMatrix());
			std::unordered_map<std::uint64_t, std::size_t> jointIndicesByStableId;
			for (std::size_t index = 0u; index < skeleton.joints.size(); ++index)
			{
				const auto& joint = skeleton.joints[index];
				bindModels[index] = ToMatrix(joint.localBindTransform);
				if (joint.parentIndex >= 0)
				{
					bindModels[index] = Multiply(bindModels[static_cast<std::size_t>(joint.parentIndex)], bindModels[index]);
				}
				jointIndicesByStableId.emplace(joint.stableId, index);
			}

			std::size_t verifiedMeshCount = 0u;
			std::size_t verifiedVertexCount = 0u;
			float largestBindError = 0.0f;
			std::vector<std::pair<std::size_t, float>> meshBindErrors;
			for (const auto& skin : result.animation.skinnedMeshes)
			{
				context.Expect(skin.sourceMeshIndex < scene->mNumMeshes
					&& meshPlacements[static_cast<unsigned int>(skin.sourceMeshIndex)].size() == 1u,
					"Each source skin section must retain one unambiguous mesh placement for package-space baking.");
				if (skin.sourceMeshIndex >= scene->mNumMeshes
					|| meshPlacements[static_cast<unsigned int>(skin.sourceMeshIndex)].size() != 1u)
				{
					continue;
				}

				const aiMesh& mesh = *scene->mMeshes[skin.sourceMeshIndex];
				const Matrix4& placement = meshPlacements[static_cast<unsigned int>(skin.sourceMeshIndex)].front();
				float meshBindError = 0.0f;
				std::vector<std::vector<std::pair<std::size_t, float>>> influences(mesh.mNumVertices);
				for (const auto& sourceInfluence : skin.influences)
				{
					const auto joint = jointIndicesByStableId.find(sourceInfluence.jointStableId);
					context.Expect(sourceInfluence.vertexIndex < mesh.mNumVertices && joint != jointIndicesByStableId.end(),
						"Every imported influence must target one cooked skeleton joint and one source vertex.");
					if (sourceInfluence.vertexIndex >= mesh.mNumVertices || joint == jointIndicesByStableId.end())
					{
						continue;
					}
					influences[sourceInfluence.vertexIndex].push_back({ joint->second, sourceInfluence.weight });
				}

				for (unsigned int vertexIndex = 0u; vertexIndex < mesh.mNumVertices; ++vertexIndex)
				{
					const auto& vertexInfluences = influences[vertexIndex];
					if (vertexInfluences.empty())
					{
						continue;
					}
					float weightSum = 0.0f;
					for (const auto& influence : vertexInfluences) weightSum += influence.second;
					const std::array<float, 3u> cookedVertex = TransformPoint(placement, mesh.mVertices[vertexIndex]);
					std::array<float, 3u> deformedVertex{};
					for (const auto& influence : vertexInfluences)
					{
						const Matrix4 skinMatrix = Multiply(
							bindModels[influence.first],
							skeleton.inverseBindMatrices[influence.first].values);
						const std::array<float, 3u> transformed = TransformPoint(skinMatrix, cookedVertex);
						const float normalizedWeight = influence.second / weightSum;
						for (std::size_t component = 0u; component < deformedVertex.size(); ++component)
						{
							deformedVertex[component] += transformed[component] * normalizedWeight;
						}
					}
					const float bindError = Distance(cookedVertex, deformedVertex);
					meshBindError = std::max(meshBindError, bindError);
					largestBindError = std::max(largestBindError, bindError);
					++verifiedVertexCount;
				}
				meshBindErrors.push_back({ skin.sourceMeshIndex, meshBindError });
				++verifiedMeshCount;
			}

			// Keep the measured counts and maximum displacement in the failure so a
			// future real-model regression identifies transform error versus missing
			// corpus coverage without requiring another instrumented build.
			std::string sectionErrors;
			for (const auto& [sourceMeshIndex, error] : meshBindErrors)
			{
				sectionErrors += " mesh[" + std::to_string(sourceMeshIndex)
					+ "]=" + std::to_string(error);
			}
			context.Expect(verifiedMeshCount == 2u && verifiedVertexCount > 0u && largestBindError < 0.002f,
				"At bind pose, every weighted package-space vertex must remain fixed after the cooked global-bone/inverse-bind palette is applied. "
				"Verified meshes=" + std::to_string(verifiedMeshCount)
				+ ", vertices=" + std::to_string(verifiedVertexCount)
				+ ", cooked maximum error=" + std::to_string(largestBindError)
				+ ", section errors:" + sectionErrors + ".");
		});
	}
}
