// FbxPackageImporter.h - Assimp-backed FBX package import entry point.
//
// FBX is imported as a package so static meshes, generated materials, extracted
// or referenced textures, assignments, and diagnostics travel through the same
// editor contract as glTF instead of becoming a single loose mesh compile.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "ModelPackageImporter.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace TerraEngine::AssetPipeline
{
	using FbxPackageImportOptions = ModelPackageImportOptions;

	// Assimp exposes FBX node transforms and animation channels only while its
	// aiScene is alive. These owned records preserve the normalized source data
	// until ImplementationCheck compresses clips and ImplementationCheck publishes the common package manifest.
	struct FbxAnimatedNode
	{
		std::uint64_t stableId = 0u;
		std::int32_t parentIndex = -1;
		std::string semanticKey;
		std::string displayName;
		TerraEngine::Animation::AnimationTransform localTransform{};
		bool socketCandidate = false;
	};

	struct FbxBoneInfluence
	{
		std::uint32_t vertexIndex = 0u;
		std::uint64_t jointStableId = 0u;
		float weight = 0.0f;
	};

	struct FbxImportedSkinnedMesh
	{
		std::size_t sourceMeshIndex = 0u;
		std::string semanticKey;
		TerraEngine::Animation::AnimationGuid skeletonAssetId{};
		std::vector<FbxBoneInfluence> influences;
	};

	struct FbxVectorKey
	{
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		TerraEngine::Animation::AnimationFloat3 value{};
	};

	struct FbxQuaternionKey
	{
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		TerraEngine::Animation::AnimationQuaternion value{};
	};

	struct FbxNodeAnimationChannel
	{
		std::uint64_t targetStableId = 0u;
		std::string targetSemanticKey;
		// Assimp exposes FBX curves as chronological sampled keys. Retaining the
		// effective interpolation beside each component makes the normalized seam
		// explicit even when an exporter baked a source cubic curve to linear keys.
		TerraEngine::Animation::AnimationInterpolation translationInterpolation = TerraEngine::Animation::AnimationInterpolation::Linear;
		TerraEngine::Animation::AnimationInterpolation rotationInterpolation = TerraEngine::Animation::AnimationInterpolation::Linear;
		TerraEngine::Animation::AnimationInterpolation scaleInterpolation = TerraEngine::Animation::AnimationInterpolation::Linear;
		std::vector<FbxVectorKey> translations;
		std::vector<FbxQuaternionKey> rotations;
		std::vector<FbxVectorKey> scales;
	};

	struct FbxMorphWeightKey
	{
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		std::vector<std::uint32_t> targetIndices;
		std::vector<float> weights;
	};

	struct FbxMorphAnimationChannel
	{
		std::string meshSemanticKey;
		std::vector<FbxMorphWeightKey> keys;
	};

	struct FbxGeneratedSkeleton
	{
		std::string semanticKey;
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		TerraEngine::Animation::SkeletonAsset asset;
	};

	struct FbxGeneratedAnimationClip
	{
		std::string semanticKey;
		std::string displayName;
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		double sourceTicksPerSecond = 0.0;
		TerraEngine::Animation::AnimationClipAsset asset;
		std::vector<FbxNodeAnimationChannel> nodeChannels;
		std::vector<FbxMorphAnimationChannel> morphChannels;
	};

	struct FbxGeneratedMorphTarget
	{
		std::uint64_t stableId = 0u;
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::size_t sourceMeshIndex = 0u;
		std::size_t sourceTargetIndex = 0u;
		std::string semanticKey;
		std::string displayName;
		std::vector<TerraEngine::Animation::AnimationFloat3> positionDeltas;
		std::vector<TerraEngine::Animation::AnimationFloat3> normalDeltas;
		// Assimp exposes tangent replacements on animated meshes when the source
		// format authored them. An empty channel deliberately means "not authored".
		std::vector<TerraEngine::Animation::AnimationFloat3> tangentDeltas;
	};

	struct FbxCoordinateConversion
	{
		float unitScaleMeters = 1.0f;
		std::int32_t upAxis = 1;
		std::int32_t upAxisSign = 1;
		std::int32_t frontAxis = 2;
		std::int32_t frontAxisSign = 1;
		std::int32_t coordinateAxis = 0;
		std::int32_t coordinateAxisSign = 1;
		bool assimpOutputNormalizedToMeters = true;
	};

	struct FbxAnimatedPackageData
	{
		FbxCoordinateConversion coordinateConversion;
		std::vector<FbxAnimatedNode> nodes;
		std::vector<FbxGeneratedSkeleton> skeletons;
		std::vector<FbxImportedSkinnedMesh> skinnedMeshes;
		std::vector<FbxGeneratedAnimationClip> clips;
		std::vector<FbxGeneratedMorphTarget> morphTargets;
		std::uint64_t parityFingerprint = 0u;
	};

	// The common ModelPackageImportResult cannot own animation children until
	// ImplementationCheck lands. This result keeps the fully prepared data beside today's package
	// result so the later manifest integration can publish it without reparsing FBX.
	struct FbxAnimatedImportResult
	{
		ModelPackageImportResult package;
		FbxAnimatedPackageData animation;
	};

	[[nodiscard]] FbxAnimatedImportResult ImportFbxPackageWithAnimation(const FbxPackageImportOptions& options);
	[[nodiscard]] ModelPackageImportResult ImportFbxPackage(const FbxPackageImportOptions& options);
}
