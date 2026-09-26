// LocomotionAnimationBindings.h - bounded, copied presentation parameter mapping.
#pragma once

#include "LocomotionTypes.h"
#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Runtime/AnimationCommandQueue.h"
#include "Animation/Runtime/RootMotion.h"

#include <array>
#include <span>
#include <string>

struct CharacterLocomotionComponent;

namespace TerraEngine::Locomotion {
	inline constexpr std::size_t kMotionParameterCount = 5U;
	using MotionParameterNames = std::array<std::string, kMotionParameterCount>;
	struct TERRAENGINE_API MotionParameterMapping {
		Animation::AnimatorInstanceHandle animator{};
		std::uint64_t controllerGeneration = 0U;
		std::array<std::uint32_t, kMotionParameterCount> ids{};
		MotionParameterField failedField = MotionParameterField::None;
		MotionParameterError error = MotionParameterError::None;
	};
	struct TERRAENGINE_API PreparedMotionParameters {
		std::array<Animation::AnimationCommand, kMotionParameterCount> commands{};
		std::size_t count = 0U;
		MotionParameterField failedField = MotionParameterField::None;
		MotionParameterError error = MotionParameterError::None;
		std::uint64_t physicalTick = 0U;
	};
	// Name/type lookup happens only when the copied controller generation or
	// authored mapping changes. Empty optional names intentionally emit nothing.
	[[nodiscard]] TERRAENGINE_API MotionParameterMapping ResolveMotionParameters(
		Animation::AnimatorInstanceHandle animator, std::uint64_t controllerGeneration,
		const MotionParameterNames& names, std::span<const Animation::AnimationControllerParameter> parameters);
	// No live state is accessed here. The publisher submits this complete bounded
	// array through AnimationSystem::SubmitCommandBatch, never individual writes.
	[[nodiscard]] TERRAENGINE_API PreparedMotionParameters PrepareMotionParameters(
		const MotionParameterMapping& mapping, const LocomotionObservation& observation,
		std::uint32_t producerId, std::uint64_t sequence) noexcept;
	// Owned by the protected coordinator, never a component or a worker. Asset
	// handles key the mapping cache so ordinary pose publications do not reload it.
	struct TERRAENGINE_API CapturedLocomotionAnimator {
		Animation::AnimatorInstanceHandle animator{};
		UUID objectId = EMPTY_UUID;
		UUID authoredControllerId = EMPTY_UUID;
		UUID authoredClipId = EMPTY_UUID;
		std::uint64_t objectGeneration = 0U;
		std::uint64_t componentGeneration = 0U;
		Animation::AnimationAssetHandle controller{};
		Animation::AnimationAssetHandle clip{};
		MotionParameterNames names{};
		MotionParameterMapping mapping{};
		Animation::AnimationQuaternion relativeRotation{};
		Animation::AnimationFloat3 relativeScale{1.0F, 1.0F, 1.0F};
		Animation::AnimationFloat3 relativeTranslation{};
		std::shared_ptr<const Physics::CharacterMotionValidity> validity;
		std::shared_ptr<const Physics::CharacterMotionValidity> bindingValidity;
		LocomotionReason reason = LocomotionReason::AnimationUnavailable;
		bool rootReady = false;
		bool clipHasRootMotion = false;
	};
	// Resolve only Self or a verified live same-scene descendant. The optional
	// previous record avoids copying asset metadata when its identity is unchanged.
	[[nodiscard]] TERRAENGINE_API CapturedLocomotionAnimator CaptureLocomotionAnimator(
		LocomotionHandle owner, const CharacterLocomotionComponent& authored,
		const CapturedLocomotionAnimator* previous = nullptr);
}
