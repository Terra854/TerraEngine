#pragma once

#include "Animation/AnimationSystem.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace TerraEngine::Runtime {

struct AnimationParityValidationOptions final {
	std::uint32_t tickCount = 120U;
	float fixedDeltaSeconds = 1.0F / 60.0F;
	std::string mode = "headless-cpu-fallback";
	// Rendered-runtime validation requires one real backend readback after the
	// bounded tick sequence. Headless CPU fallback deliberately leaves this
	// false because it has no renderer and is compared through pose/state hashes.
	bool requireRenderedVisualCapture = false;
};

struct AnimationParityReport final {
	bool succeeded = false;
	bool orderlyShutdown = false;
	std::uint32_t ticksEvaluated = 0U;
	std::uint32_t animatorCount = 0U;
	std::uint32_t requestedTicks = 0U;
	float fixedDeltaSeconds = 0.0F;
	std::string mode;
	std::string diagnostic;
	nlohmann::json animators = nlohmann::json::array();
	nlohmann::json ticks = nlohmann::json::array();
	// The rendered lane embeds its source-free PNG so the invoking validation
	// script can preserve and inspect the exact bytes emitted by the installed
	// runtime. The headless lane records an explicit non-required capture.
	nlohmann::json visualCapture = nlohmann::json::object();

	[[nodiscard]] nlohmann::json ToJson() const;
};

struct AnimationParityAnimatorDigest final {
	std::string stateHash;
	std::string poseHash;
};

[[nodiscard]] AnimationParityAnimatorDigest ComputeAnimationParityAnimatorDigest(
	const Animation::PublishedAnimatorPose& pose);

// Stable FNV-1a helper used by tests and canonical report payloads.
[[nodiscard]] std::string CanonicalAnimationParityHash(std::string_view bytes);

// Capture one published animation frame. The caller owns the fixed-tick loop.
[[nodiscard]] bool CaptureAnimationParityTick(
	const Animation::AnimationSystem& animation, std::uint32_t tickIndex, AnimationParityReport& report);

[[nodiscard]] AnimationParityReport ValidatePublishedAnimationParity(
	const Animation::AnimationSystem& animation,
	const AnimationParityValidationOptions& options = {});

} // namespace TerraEngine::Runtime
