// Bake-only validation companion. The runtime consumer loads these real cooked
// assets through ResourceManager and authors the normal Animator/Mesh components.
#pragma once
#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Assets/AnimationClipAsset.h"
#include <filesystem>
#include <string>

namespace TerraEngine::Tests::Review {
	struct AnimatedConsumerCompanion {
		Animation::SkeletonAsset skeleton;
		Animation::AnimationClipAsset clip;
		// Relative to outputRoot; stable names are suitable for hash-pinned install.
		std::filesystem::path skeletonPath;
		std::filesystem::path clipPath;
	};
	// All source/import/generated files remain inside outputRoot. Output is
	// replaced only after actual import, clip compilation and codec readback pass.
	Animation::AnimationStatus BakeAnimatedConsumerCompanion(
		const std::filesystem::path& outputRoot,
		AnimatedConsumerCompanion& output,
		std::string& diagnostic);
}
