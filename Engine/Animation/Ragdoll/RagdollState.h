// RagdollState.h - Bounded copied state for recording, rollback, and replication.

#pragma once

#include "Animation/Determinism/AnimationNetworkPackets.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	enum class RagdollLifecycleState : std::uint8_t {
		Inactive,
		Creating,
		Active,
		Recovering,
		TearingDown,
		Rejected
	};

	struct RagdollStateSnapshot {
		RagdollLifecycleState lifecycle = RagdollLifecycleState::Inactive;
		RagdollGenerationStamp generation{};
		PhysicsPoseSnapshot physicsPose{};
		std::vector<float> blendWeights;
		std::uint64_t eventSequence = 0U;
		std::uint64_t recordingSequence = 0U;
		std::uint64_t motionMatchCandidateId = 0U;
		AnimationGeneration motionDatabaseGeneration = 0U;
		AnimationGeneration retargetGeneration = 0U;
		AnimationFloat3 accumulatedRootTranslation{};
		float accumulatedRootYawRadians = 0.0F;
		std::uint64_t contactMask = 0U;
		std::uint64_t recoveryCandidateId = 0U;
		AnimationFixedTick recoveryTransitionTick = 0;
		AnimationGeneration rollbackGeneration = 0U;
		std::uint64_t replaySequence = 0U;
		std::uint64_t networkSequence = 0U;
		bool predicted = false;
		std::uint64_t deterministicHash = 0U;
	};

	class TERRAENGINE_API RagdollState final {
	public:
		// Publication validates the complete integration record and lifecycle edge
		// before replacing Current(). Restore is the only API that may move backward
		// in fixed time or lower an event watermark.
		AnimationStatus Publish(RagdollStateSnapshot snapshot) noexcept;
		AnimationStatus Restore(
			AnimationFixedTick tick,
			const RagdollGenerationStamp& expected) noexcept;

		AnimationStatus CaptureRollback(
			std::uint64_t ragdollInstanceId,
			std::vector<AnimationRagdollRollbackState>& output) const noexcept;
		AnimationStatus RestoreRollback(
			std::span<const AnimationRagdollRollbackState> records,
			const RagdollGenerationStamp& expected) noexcept;

		const RagdollStateSnapshot& Current() const noexcept { return current_; }
		std::size_t HistorySize() const noexcept { return history_.size(); }

	private:
		RagdollStateSnapshot current_{};
		std::deque<RagdollStateSnapshot> history_;
	};
}
