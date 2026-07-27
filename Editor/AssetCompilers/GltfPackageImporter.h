// GltfPackageImporter.h - Package-level glTF/GLB import entry point.
//
// Unlike the legacy mesh compiler path, this importer treats glTF as a package
// that owns meshes, materials, textures, and assignments. It returns a durable
// result contract that the AssetDatabase and MCP bridge can consume.

#pragma once

#include "Animation/Assets/AnimationClipAsset.h"
#include "Animation/Assets/SkeletonAsset.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "GltfImportTypes.h"
#include "ModelPackageImporter.h"

namespace TerraEngine::AssetPipeline
{
	using GltfPackageImportOptions = ModelPackageImportOptions;

	// glTF is right-handed, Y-up, and meter-based by specification. Import tools
	// may nevertheless attach explicit source metadata when converting legacy
	// authoring packages to glTF; retaining it makes a one-time conversion visible.
	enum class GltfSourceHandedness : std::uint8_t
	{
		Right = 0U,
		Left = 1U
	};

	struct GltfCoordinateConversion
	{
		float sourceUnitScaleMeters = 1.0F;
		Animation::SkeletonSourceAxis sourceUpAxis = Animation::SkeletonSourceAxis::PositiveY;
		Animation::SkeletonSourceAxis sourceForwardAxis = Animation::SkeletonSourceAxis::PositiveZ;
		GltfSourceHandedness sourceHandedness = GltfSourceHandedness::Right;
		bool appliedExactlyOnce = true;
	};

	// Nodes are stored separately from a skeleton because one glTF package can
	// contain multiple skins that share source nodes. normalizedSemanticPath is
	// the stable source key; source array indices are diagnostic metadata only.
	struct GltfImportedNode
	{
		std::size_t sourceNodeIndex = 0U;
		std::optional<std::size_t> parentNodeIndex;
		std::uint64_t stableId = 0U;
		std::string displayName;
		std::string normalizedSemanticPath;
		std::string jointRole;
		Animation::AnimationTransform localTransform{};
		bool namedSocket = false;
		std::string socketRole;
	};

	struct GltfImportedMorphTarget
	{
		std::size_t sourceMeshIndex = 0U;
		std::size_t sourcePrimitiveIndex = 0U;
		std::size_t sourceTargetIndex = 0U;
		std::uint64_t stableId = 0U;
		std::string displayName;
		std::string semanticKey;
		float defaultWeight = 0.0F;
	};

	// Raw source samples are kept until AnimationClipCompiler performs segmented
	// reduction and quantization. values is key-major and componentCount states
	// how many consecutive floats form each value (including cubic tangents).
	struct GltfImportedAnimationTrack
	{
		std::size_t sourceChannelIndex = 0U;
		std::size_t sourceNodeIndex = 0U;
		std::uint64_t stableTargetId = 0U;
		Animation::AnimationClipTrackKind kind = Animation::AnimationClipTrackKind::Translation;
		Animation::AnimationInterpolation interpolation = Animation::AnimationInterpolation::Linear;
		std::uint32_t componentCount = 0U;
		std::vector<float> timesSeconds;
		std::vector<float> values;
	};

	struct GltfGeneratedSkeleton
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		std::size_t sourceSkinIndex = 0U;
		std::string displayName;
		std::string semanticKey;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		Animation::SkeletonAsset asset;
	};

	struct GltfGeneratedAnimationClip
	{
		TerraEngine::UUID guid = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID skeletonGuid = TerraEngine::EMPTY_UUID;
		std::size_t sourceAnimationIndex = 0U;
		std::string displayName;
		std::string semanticKey;
		std::filesystem::path sourcePath;
		std::filesystem::path compiledPath;
		Animation::AnimationClipAsset asset;
		std::vector<GltfImportedAnimationTrack> sourceTracks;
	};

	// T037 will merge these records into the format-neutral manifest. Keeping an
	// importer-owned aggregate now lets T034 preserve complete source semantics
	// without racing edits to the shared ModelPackageImportResult contract.
	struct GltfAnimatedPackageData
	{
		GltfCoordinateConversion coordinateConversion;
		std::vector<GltfImportedNode> nodes;
		std::vector<GltfImportedMorphTarget> morphTargets;
		std::vector<GltfGeneratedSkeleton> skeletons;
		std::vector<GltfGeneratedAnimationClip> animationClips;
		std::filesystem::path publishedAnimationRoot;
		bool publishedAtomically = false;
	};

	struct GltfAnimatedImportResult
	{
		GltfImportResult package;
		GltfAnimatedPackageData animation;
	};

	[[nodiscard]] bool IsGltfPackageSourceExtension(const std::filesystem::path& filePath);
	[[nodiscard]] GltfAnimatedImportResult ImportGltfPackageWithAnimation(const GltfPackageImportOptions& options);
	[[nodiscard]] GltfImportResult ImportGltfPackage(const GltfPackageImportOptions& options);
}
