// AnimationRollback.h - Deterministic restore and fixed-tick resimulation.

#pragma once

#include "Animation/Determinism/AnimationHistory.h"

#include <functional>

namespace TerraEngine::Animation {
	inline constexpr std::size_t kAnimationRollbackMaximumExternalInputs =
		kAnimationRollbackMaximumEventCursorStates;
	inline constexpr std::size_t kAnimationRollbackMaximumDeferredMutations =
		kAnimationRollbackMaximumEventCursorStates;

	struct AnimationRollbackExternalInput {
		AnimationFixedTick tick = 0;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		AnimationDeterministicStateRecord value{};
		auto operator<=>(const AnimationRollbackExternalInput&) const = default;
	};

	// Worker resimulation produces copied requests only. The scene and physics
	// owners apply the complete batch later at their declared protected boundary.
	enum class AnimationRollbackMutationDomain : std::uint8_t {
		Scene = 0U,
		Character = 1U,
		Physics = 2U,
		Attachment = 3U,
		Ragdoll = 4U
	};

	enum class AnimationRollbackMutationOperation : std::uint8_t {
		ApplyRootMotion = 0U,
		SynchronizeState = 1U
	};

	struct AnimationRollbackDeferredMutation {
		AnimationRollbackMutationDomain domain = AnimationRollbackMutationDomain::Scene;
		AnimationRollbackMutationOperation operation = AnimationRollbackMutationOperation::ApplyRootMotion;
		AnimatorInstanceHandle animator{};
		AnimationFixedTick tick = 0;
		AnimationGeneration expectedSceneGeneration = 0U;
		AnimationGeneration expectedPhysicsWorldGeneration = 0U;
		std::uint64_t correlationId = 0U;
		AnimationRootMotionCorrection rootMotion{};
		AnimationDeterministicStateRecord state{};
		auto operator<=>(const AnimationRollbackDeferredMutation&) const = default;
	};

	static_assert(AnimationTransportValueRecord<AnimationRollbackDeferredMutation>);

	struct AnimationRollbackMutationBatch {
		std::uint16_t version = kAnimationDeterminismPacketVersion;
		AnimatorInstanceHandle animator{};
		AnimationFixedTick restoredTick = 0;
		AnimationFixedTick throughTick = 0;
		AnimationRollbackGenerationStamp generations{};
		std::vector<AnimationRollbackDeferredMutation> commands;
		std::uint64_t checksum = 0U;
		bool requiresProtectedSceneFlush = false;
		bool requiresPhysicsBoundary = false;
		auto operator<=>(const AnimationRollbackMutationBatch&) const = default;
	};

	struct AnimationRollbackResult {
		AnimationStatus status = AnimationStatus::InvalidArgument;
		AnimationRollbackSnapshot snapshot{};
		std::vector<AnimationDeterministicStateRecord> appliedInputs;
		AnimationEventWatermark eventWatermark{};
		AnimationRootMotionCorrection rootMotionCorrection{};
		std::vector<AnimationEventCursorRollbackState> eventsToPublish;
		std::size_t suppressedHistoricalEventCount = 0U;
		std::vector<AnimationRootMotionCorrection> rootMotionCorrections;
		AnimationRollbackMutationBatch deferredMutations{};
		auto operator<=>(const AnimationRollbackResult&) const = default;
	};

	[[nodiscard]] TERRAENGINE_API std::uint64_t ComputeAnimationRollbackMutationChecksum(
		const AnimationRollbackMutationBatch& batch) noexcept;

	using AnimationRollbackStep = std::function<AnimationStatus(AnimationFixedTick,
		std::span<const AnimationDeterministicStateRecord>, AnimationRollbackSnapshot&)>;

	class TERRAENGINE_API AnimationRollback final {
	public:
		[[nodiscard]] AnimationStatus Resimulate(const AnimationRollbackSnapshot& restorePoint,
			AnimationFixedTick targetTick, std::span<const AnimationRollbackExternalInput> inputs,
			const AnimationRollbackStep& step, AnimationRollbackResult& output) const noexcept;
	};
}
