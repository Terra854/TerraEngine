// RagdollState.cpp - Complete bounded lifecycle, rollback, and transport state.

#include "Animation/Ragdoll/RagdollState.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <new>
#include <set>

namespace TerraEngine::Animation {
	namespace {
		bool SupportedLifecycle(RagdollLifecycleState value) noexcept {
			return value >= RagdollLifecycleState::Inactive
				&& value <= RagdollLifecycleState::Rejected;
		}

		bool ValidTransition(RagdollLifecycleState from, RagdollLifecycleState to) noexcept {
			if(from == to) return true;
			switch(from) {
			case RagdollLifecycleState::Inactive:
				return to == RagdollLifecycleState::Creating;
			case RagdollLifecycleState::Creating:
				return to == RagdollLifecycleState::Active || to == RagdollLifecycleState::Rejected
					|| to == RagdollLifecycleState::TearingDown;
			case RagdollLifecycleState::Active:
				return to == RagdollLifecycleState::Recovering || to == RagdollLifecycleState::TearingDown
					|| to == RagdollLifecycleState::Rejected;
			case RagdollLifecycleState::Recovering:
				return to == RagdollLifecycleState::Inactive || to == RagdollLifecycleState::Active
					|| to == RagdollLifecycleState::TearingDown || to == RagdollLifecycleState::Rejected;
			case RagdollLifecycleState::TearingDown:
				return to == RagdollLifecycleState::Inactive || to == RagdollLifecycleState::Rejected;
			case RagdollLifecycleState::Rejected:
				return to == RagdollLifecycleState::Inactive || to == RagdollLifecycleState::Creating;
			}
			return false;
		}

		AnimationRollbackRagdollState ToRollbackLifecycle(RagdollLifecycleState value) noexcept {
			return static_cast<AnimationRollbackRagdollState>(value);
		}

		RagdollLifecycleState FromRollbackLifecycle(AnimationRollbackRagdollState value) noexcept {
			return static_cast<RagdollLifecycleState>(value);
		}

		AnimationNetworkFloat3Bits Float3Bits(const AnimationFloat3& value) noexcept {
			return {{{std::bit_cast<std::uint32_t>(value.x), std::bit_cast<std::uint32_t>(value.y),
				std::bit_cast<std::uint32_t>(value.z)}}};
		}

		AnimationFloat3 FromFloat3Bits(const AnimationNetworkFloat3Bits& value) noexcept {
			return {std::bit_cast<float>(value.words[0U]), std::bit_cast<float>(value.words[1U]),
				std::bit_cast<float>(value.words[2U])};
		}

		AnimationNetworkQuaternionBits QuaternionBits(const AnimationQuaternion& value) noexcept {
			return {{{std::bit_cast<std::uint32_t>(value.x), std::bit_cast<std::uint32_t>(value.y),
				std::bit_cast<std::uint32_t>(value.z), std::bit_cast<std::uint32_t>(value.w)}}};
		}

		AnimationQuaternion FromQuaternionBits(const AnimationNetworkQuaternionBits& value) noexcept {
			return {std::bit_cast<float>(value.words[0U]), std::bit_cast<float>(value.words[1U]),
				std::bit_cast<float>(value.words[2U]), std::bit_cast<float>(value.words[3U])};
		}

		AnimationNetworkTransformBits TransformBits(const AnimationTransform& value) noexcept {
			return {Float3Bits(value.translation), QuaternionBits(value.rotation), Float3Bits(value.scale)};
		}

		AnimationTransform FromTransformBits(const AnimationNetworkTransformBits& value) noexcept {
			return {FromFloat3Bits(value.translation), FromQuaternionBits(value.rotation),
				FromFloat3Bits(value.scale)};
		}

		std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept {
			hash ^= value;
			hash *= 1099511628211ULL;
			return hash;
		}

		std::uint64_t HashSnapshot(const RagdollStateSnapshot& snapshot) noexcept {
			std::uint64_t hash = 14695981039346656037ULL;
			hash = Mix(hash, static_cast<std::uint64_t>(snapshot.lifecycle));
			hash = Mix(hash, snapshot.generation.worldGeneration);
			hash = Mix(hash, snapshot.generation.profileGeneration);
			hash = Mix(hash, snapshot.generation.skeletonGeneration);
			hash = Mix(hash, snapshot.generation.publicationGeneration);
			hash = Mix(hash, static_cast<std::uint64_t>(snapshot.generation.fixedStep));
			hash = Mix(hash, snapshot.eventSequence);
			hash = Mix(hash, snapshot.recordingSequence);
			hash = Mix(hash, snapshot.motionMatchCandidateId);
			hash = Mix(hash, snapshot.motionDatabaseGeneration);
			hash = Mix(hash, snapshot.retargetGeneration);
			hash = Mix(hash, snapshot.contactMask);
			hash = Mix(hash, snapshot.recoveryCandidateId);
			hash = Mix(hash, static_cast<std::uint64_t>(snapshot.recoveryTransitionTick));
			hash = Mix(hash, snapshot.rollbackGeneration);
			hash = Mix(hash, snapshot.replaySequence);
			hash = Mix(hash, snapshot.networkSequence);
			hash = Mix(hash, snapshot.predicted ? 1U : 0U);
			hash = Mix(hash, std::bit_cast<std::uint32_t>(snapshot.accumulatedRootTranslation.x));
			hash = Mix(hash, std::bit_cast<std::uint32_t>(snapshot.accumulatedRootTranslation.y));
			hash = Mix(hash, std::bit_cast<std::uint32_t>(snapshot.accumulatedRootTranslation.z));
			hash = Mix(hash, std::bit_cast<std::uint32_t>(snapshot.accumulatedRootYawRadians));
			for(const float weight : snapshot.blendWeights) hash = Mix(hash, std::bit_cast<std::uint32_t>(weight));
			for(const PhysicsPoseBodySnapshot& body : snapshot.physicsPose.bodies) {
				hash = Mix(hash, body.bodyId);
				hash = Mix(hash, body.stableJointId);
				for(const std::uint32_t word : TransformBits(body.modelPose).words()) hash = Mix(hash, word);
				for(const std::uint32_t word : Float3Bits(body.linearVelocity).words) hash = Mix(hash, word);
				for(const std::uint32_t word : Float3Bits(body.angularVelocity).words) hash = Mix(hash, word);
				hash = Mix(hash, body.bodyGeneration);
				hash = Mix(hash, body.sleeping ? 1U : 0U);
			}
			return hash == 0U ? 1U : hash;
		}

		bool ValidateIntegrationFields(const RagdollStateSnapshot& snapshot) noexcept {
			if(!SupportedLifecycle(snapshot.lifecycle) || !snapshot.generation.IsValid()
				|| snapshot.blendWeights.size() > kRagdollSnapshotMaximumBodies
				|| !IsFinite(snapshot.accumulatedRootTranslation)
				|| !std::isfinite(snapshot.accumulatedRootYawRadians)) return false;
			for(const float weight : snapshot.blendWeights) {
				if(!std::isfinite(weight) || weight < 0.0F || weight > 1.0F) return false;
			}
			const bool requiresPose = snapshot.lifecycle == RagdollLifecycleState::Active
				|| snapshot.lifecycle == RagdollLifecycleState::Recovering;
			return !requiresPose
				|| (ValidatePhysicsPoseSnapshot(snapshot.physicsPose, snapshot.generation) == AnimationStatus::Ok
					&& (snapshot.blendWeights.empty()
						|| snapshot.blendWeights.size() == snapshot.physicsPose.bodies.size()));
		}
	}

	AnimationStatus RagdollState::Publish(RagdollStateSnapshot snapshot) noexcept {
		if(!ValidateIntegrationFields(snapshot)) return AnimationStatus::Rejected;
		if(current_.generation.IsValid()) {
			if(snapshot.generation.fixedStep < current_.generation.fixedStep
				|| snapshot.eventSequence < current_.eventSequence
				|| !ValidTransition(current_.lifecycle, snapshot.lifecycle)) {
				return AnimationStatus::Rejected;
			}
			if(snapshot.generation.fixedStep == current_.generation.fixedStep
				&& snapshot.generation != current_.generation) return AnimationStatus::StaleHandle;
		}
		const std::uint64_t hash = HashSnapshot(snapshot);
		if(snapshot.deterministicHash != 0U && snapshot.deterministicHash != hash) {
			return AnimationStatus::ContentHashMismatch;
		}
		snapshot.deterministicHash = hash;
		try {
			if(!history_.empty() && history_.back().generation.fixedStep == snapshot.generation.fixedStep) {
				// Several protected lifecycle edges may complete in one fixed step. The
				// final generation-compatible value replaces that tick atomically, while
				// an exact retry remains naturally idempotent.
				history_.back() = snapshot;
				current_ = std::move(snapshot);
				return AnimationStatus::Ok;
			}
			history_.push_back(snapshot);
			const std::size_t maximum = static_cast<std::size_t>(
				GetAnimationRuntimeLimit(AnimationLimit::RollbackHistoryTicks).hardMaximum);
			while(history_.size() > maximum) history_.pop_front();
			current_ = std::move(snapshot);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus RagdollState::Restore(
		AnimationFixedTick tick,
		const RagdollGenerationStamp& expected) noexcept {
		for(auto iterator = history_.rbegin(); iterator != history_.rend(); ++iterator) {
			if(iterator->generation.fixedStep == tick && iterator->generation == expected) {
				current_ = *iterator;
				return AnimationStatus::Ok;
			}
		}
		return AnimationStatus::StaleHandle;
	}

	AnimationStatus RagdollState::CaptureRollback(
		std::uint64_t ragdollInstanceId,
		std::vector<AnimationRagdollRollbackState>& output) const noexcept {
		if(ragdollInstanceId == 0U || !ValidateIntegrationFields(current_)
			|| current_.physicsPose.bodies.size() > kAnimationRollbackMaximumRagdollStates) {
			return AnimationStatus::InvalidArgument;
		}
		try {
			std::vector<AnimationRagdollRollbackState> staged;
			staged.reserve(current_.physicsPose.bodies.size());
			for(std::size_t index = 0U; index < current_.physicsPose.bodies.size(); ++index) {
				const PhysicsPoseBodySnapshot& body = current_.physicsPose.bodies[index];
				AnimationRagdollRollbackState record;
				record.ragdollInstanceId = ragdollInstanceId;
				record.bodyId = body.bodyId;
				record.stableJointId = body.stableJointId;
				record.lifecycle = ToRollbackLifecycle(current_.lifecycle);
				record.worldGeneration = current_.generation.worldGeneration;
				record.profileGeneration = current_.generation.profileGeneration;
				record.skeletonGeneration = current_.generation.skeletonGeneration;
				record.physicsPublicationGeneration = current_.generation.publicationGeneration;
				record.bodyGeneration = body.bodyGeneration;
				record.fixedStep = current_.generation.fixedStep;
				record.modelTransform = TransformBits(body.modelPose);
				record.linearVelocity = Float3Bits(body.linearVelocity);
				record.angularVelocity = Float3Bits(body.angularVelocity);
				record.blendWeightBits = std::bit_cast<std::uint32_t>(
					current_.blendWeights.empty() ? 1.0F : current_.blendWeights[index]);
				record.eventSequence = current_.eventSequence;
				record.sleeping = body.sleeping;
				staged.push_back(record);
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus RagdollState::RestoreRollback(
		std::span<const AnimationRagdollRollbackState> records,
		const RagdollGenerationStamp& expected) noexcept {
		if(records.empty() || records.size() > kAnimationRollbackMaximumRagdollStates
			|| !expected.IsValid()) return AnimationStatus::InvalidArgument;
		try {
			const AnimationRagdollRollbackState& first = records.front();
			RagdollStateSnapshot staged;
			staged.lifecycle = FromRollbackLifecycle(first.lifecycle);
			staged.generation = expected;
			staged.eventSequence = first.eventSequence;
			staged.physicsPose.generation = expected;
			staged.physicsPose.completeCurrentStep = true;
			staged.physicsPose.bodies.reserve(records.size());
			staged.blendWeights.reserve(records.size());
			std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
			for(const AnimationRagdollRollbackState& record : records) {
				if(record.ragdollInstanceId == 0U || record.ragdollInstanceId != first.ragdollInstanceId
					|| record.lifecycle != first.lifecycle || record.eventSequence != first.eventSequence
					|| record.worldGeneration != expected.worldGeneration
					|| record.profileGeneration != expected.profileGeneration
					|| record.skeletonGeneration != expected.skeletonGeneration
					|| record.physicsPublicationGeneration != expected.publicationGeneration
					|| record.fixedStep != expected.fixedStep || record.bodyGeneration == 0U
					|| !identities.emplace(record.bodyId, record.stableJointId).second) {
					return AnimationStatus::StaleHandle;
				}
				PhysicsPoseBodySnapshot body;
				body.bodyId = record.bodyId;
				body.stableJointId = record.stableJointId;
				body.modelPose = FromTransformBits(record.modelTransform);
				body.linearVelocity = FromFloat3Bits(record.linearVelocity);
				body.angularVelocity = FromFloat3Bits(record.angularVelocity);
				body.bodyGeneration = record.bodyGeneration;
				body.sleeping = record.sleeping;
				const float weight = std::bit_cast<float>(record.blendWeightBits);
				if(!IsFinite(body.modelPose) || !IsFinite(body.linearVelocity)
					|| !IsFinite(body.angularVelocity) || !std::isfinite(weight)
					|| weight < 0.0F || weight > 1.0F) return AnimationStatus::InvalidArgument;
				staged.physicsPose.bodies.push_back(body);
				staged.blendWeights.push_back(weight);
			}
			if(!ValidateIntegrationFields(staged)) return AnimationStatus::Rejected;
			staged.deterministicHash = HashSnapshot(staged);
			current_ = std::move(staged);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}
}
