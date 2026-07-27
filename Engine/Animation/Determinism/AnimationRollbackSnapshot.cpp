// AnimationRollbackSnapshot.cpp - Canonical value-only snapshot capture and restore.

#include "Animation/Determinism/AnimationRollbackSnapshot.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <stdexcept>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

		[[nodiscard]] constexpr std::uint64_t Mix(std::uint64_t value, std::uint64_t next) noexcept
		{
			return (value ^ next) * kFnvPrime;
		}

		[[nodiscard]] bool IsFiniteWord(std::uint32_t value) noexcept
		{
			return std::isfinite(DecodeAnimationNetworkFloat(value));
		}

		template<std::size_t Count>
		[[nodiscard]] bool AreFiniteWords(const std::array<std::uint32_t, Count>& values) noexcept
		{
			return std::all_of(values.begin(), values.end(), IsFiniteWord);
		}

		[[nodiscard]] bool IsFinite(const AnimationNetworkFloat3Bits& value) noexcept
		{
			return AreFiniteWords(value.words);
		}

		[[nodiscard]] bool IsFinite(const AnimationNetworkQuaternionBits& value) noexcept
		{
			return AreFiniteWords(value.words);
		}

		[[nodiscard]] bool IsFinite(const AnimationNetworkTransformBits& value) noexcept
		{
			return IsFinite(value.translation) && IsFinite(value.rotation) && IsFinite(value.scale);
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackPlaybackState value) noexcept
		{
			return value >= AnimationRollbackPlaybackState::Stopped
				&& value <= AnimationRollbackPlaybackState::Completed;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackPlaybackDirection value) noexcept
		{
			return value == AnimationRollbackPlaybackDirection::Reverse
				|| value == AnimationRollbackPlaybackDirection::Stationary
				|| value == AnimationRollbackPlaybackDirection::Forward;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackValueType value) noexcept
		{
			return value >= AnimationRollbackValueType::Trigger
				&& value <= AnimationRollbackValueType::Transform;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackIkState value) noexcept
		{
			return value >= AnimationRollbackIkState::Solved
				&& value <= AnimationRollbackIkState::NoContribution;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackFootContactPhase value) noexcept
		{
			return value >= AnimationRollbackFootContactPhase::Released
				&& value <= AnimationRollbackFootContactPhase::Releasing;
		}

		[[nodiscard]] bool IsSupported(AnimationRollbackRagdollState value) noexcept
		{
			return value >= AnimationRollbackRagdollState::Inactive
				&& value <= AnimationRollbackRagdollState::Rejected;
		}

		[[nodiscard]] bool IsSupported(AnimationDeterministicStateKind value) noexcept
		{
			return value >= AnimationDeterministicStateKind::Controller
				&& value <= AnimationDeterministicStateKind::Physics;
		}

		[[nodiscard]] std::size_t ParameterWordCount(AnimationRollbackValueType type) noexcept
		{
			switch(type) {
			case AnimationRollbackValueType::Trigger: return 0U;
			case AnimationRollbackValueType::Boolean: return 1U;
			case AnimationRollbackValueType::Integer: return 1U;
			case AnimationRollbackValueType::Float: return 1U;
			case AnimationRollbackValueType::Vector3: return 3U;
			case AnimationRollbackValueType::Quaternion: return 4U;
			case AnimationRollbackValueType::Transform: return 10U;
			}
			return 0U;
		}

		[[nodiscard]] bool IsValid(const AnimationParameterRollbackState& value) noexcept
		{
			if(value.parameterId == 0U || !IsSupported(value.type)) {
				return false;
			}
			const std::size_t usedWords = ParameterWordCount(value.type);
			if(value.type == AnimationRollbackValueType::Boolean && value.valueWords[0] > 1U) {
				return false;
			}
			if(value.type != AnimationRollbackValueType::Integer
				&& value.type != AnimationRollbackValueType::Boolean
				&& value.type != AnimationRollbackValueType::Trigger) {
				for(std::size_t index = 0U; index < usedWords; ++index) {
					if(!IsFiniteWord(value.valueWords[index])) {
						return false;
					}
				}
			}
			// Canonical unused zero words prevent equivalent short values from
			// acquiring different deterministic hashes.
			return std::all_of(value.valueWords.begin() + usedWords, value.valueWords.end(),
				[](std::uint32_t word) { return word == 0U; });
		}

		template<typename Value, typename Less, typename Validator>
		[[nodiscard]] bool IsStrictlyOrderedAndValid(
			const std::vector<Value>& values,
			Less less,
			Validator validator) noexcept
		{
			for(std::size_t index = 0U; index < values.size(); ++index) {
				if(!validator(values[index])) {
					return false;
				}
				if(index != 0U && !less(values[index - 1U], values[index])) {
					return false;
				}
			}
			return true;
		}

		[[nodiscard]] bool GenericStateLess(
			const AnimationDeterministicStateRecord& left,
			const AnimationDeterministicStateRecord& right) noexcept
		{
			if(left.kind != right.kind) {
				return left.kind < right.kind;
			}
			if(left.stableId != right.stableId) {
				return left.stableId < right.stableId;
			}
			return left.ordinal < right.ordinal;
		}

		void Canonicalize(AnimationRollbackSnapshot& snapshot)
		{
			std::sort(snapshot.controllers.begin(), snapshot.controllers.end(),
				[](const auto& left, const auto& right) {
					return left.controllerInstanceId < right.controllerInstanceId;
				});
			std::sort(snapshot.clips.begin(), snapshot.clips.end(),
				[](const auto& left, const auto& right) { return left.clipInstanceId < right.clipInstanceId; });
			std::sort(snapshot.transitions.begin(), snapshot.transitions.end(),
				[](const auto& left, const auto& right) {
					return left.transitionStableId < right.transitionStableId;
				});
			std::sort(snapshot.layers.begin(), snapshot.layers.end(),
				[](const auto& left, const auto& right) { return left.layerStableId < right.layerStableId; });
			std::sort(snapshot.parameters.begin(), snapshot.parameters.end(),
				[](const auto& left, const auto& right) { return left.parameterId < right.parameterId; });
			std::sort(snapshot.triggers.begin(), snapshot.triggers.end(),
				[](const auto& left, const auto& right) {
					if(left.parameterId != right.parameterId) {
						return left.parameterId < right.parameterId;
					}
					return left.sequenceId < right.sequenceId;
				});
			std::sort(snapshot.events.begin(), snapshot.events.end(),
				[](const auto& left, const auto& right) {
					if(left.streamId != right.streamId) {
						return left.streamId < right.streamId;
					}
					if(left.deliveredSequence != right.deliveredSequence) {
						return left.deliveredSequence < right.deliveredSequence;
					}
					return left.eventId < right.eventId;
				});
			std::sort(snapshot.rootMotionStates.begin(), snapshot.rootMotionStates.end(),
				[](const auto& left, const auto& right) { return left.accumulationId < right.accumulationId; });
			std::sort(snapshot.ikStates.begin(), snapshot.ikStates.end(),
				[](const auto& left, const auto& right) {
					if(left.stableNodeId != right.stableNodeId) {
						return left.stableNodeId < right.stableNodeId;
					}
					return left.goalId < right.goalId;
				});
			std::sort(snapshot.retargetStates.begin(), snapshot.retargetStates.end(),
				[](const auto& left, const auto& right) { return left.stableNodeId < right.stableNodeId; });
			std::sort(snapshot.motionMatchStates.begin(), snapshot.motionMatchStates.end(),
				[](const auto& left, const auto& right) { return left.stableNodeId < right.stableNodeId; });
			std::sort(snapshot.morphStates.begin(), snapshot.morphStates.end(),
				[](const auto& left, const auto& right) { return left.targetStableId < right.targetStableId; });
			std::sort(snapshot.ragdollStates.begin(), snapshot.ragdollStates.end(),
				[](const auto& left, const auto& right) {
					if(left.ragdollInstanceId != right.ragdollInstanceId) {
						return left.ragdollInstanceId < right.ragdollInstanceId;
					}
					return left.bodyId < right.bodyId;
				});
			std::sort(snapshot.state.begin(), snapshot.state.end(), GenericStateLess);
		}

		[[nodiscard]] AnimationStatus ValidateCounts(const AnimationRollbackSnapshot& snapshot) noexcept
		{
			if(snapshot.controllers.size() > kAnimationRollbackMaximumControllerStates
				|| snapshot.clips.size() > kAnimationRollbackMaximumClipStates
				|| snapshot.transitions.size() > kAnimationRollbackMaximumTransitionStates
				|| snapshot.layers.size() > kAnimationRollbackMaximumLayerStates
				|| snapshot.parameters.size() > kAnimationRollbackMaximumParameterStates
				|| snapshot.triggers.size() > kAnimationRollbackMaximumTriggerStates
				|| snapshot.events.size() > kAnimationRollbackMaximumEventCursorStates
				|| snapshot.rootMotionStates.size() > kAnimationRollbackMaximumRootMotionStates
				|| snapshot.ikStates.size() > kAnimationRollbackMaximumIkStates
				|| snapshot.retargetStates.size() > kAnimationRollbackMaximumRetargetStates
				|| snapshot.motionMatchStates.size() > kAnimationRollbackMaximumMotionMatchStates
				|| snapshot.morphStates.size() > kAnimationRollbackMaximumMorphStates
				|| snapshot.ragdollStates.size() > kAnimationRollbackMaximumRagdollStates
				|| snapshot.state.size() > kAnimationDeterminismMaximumStateRecords) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		[[nodiscard]] bool ValidateTypedRecords(const AnimationRollbackSnapshot& snapshot) noexcept
		{
			const bool controllersValid = IsStrictlyOrderedAndValid(snapshot.controllers,
				[](const auto& left, const auto& right) {
					return left.controllerInstanceId < right.controllerInstanceId;
				},
				[](const auto& value) {
					return value.controllerInstanceId != 0U && value.activeStateId != 0U
						&& value.lastEvaluatedTick >= value.stateEnteredTick;
				});
			const bool clipsValid = IsStrictlyOrderedAndValid(snapshot.clips,
				[](const auto& left, const auto& right) { return left.clipInstanceId < right.clipInstanceId; },
				[](const auto& value) {
					return value.clipInstanceId != 0U && value.clipAssetId.IsValid()
						&& value.clipGeneration != 0U && value.clipContentFingerprint != 0U
						&& value.durationTicks >= 0 && value.resolvedTick >= 0
						&& (value.durationTicks == 0 || value.resolvedTick <= value.durationTicks)
						&& IsFiniteWord(value.speedBits)
						&& value.subTickRemainderQ16 < (1U << 16U) && IsSupported(value.runState)
						&& IsSupported(value.direction);
				});
			const bool transitionsValid = IsStrictlyOrderedAndValid(snapshot.transitions,
				[](const auto& left, const auto& right) {
					return left.transitionStableId < right.transitionStableId;
				},
				[](const auto& value) {
					return value.transitionStableId != 0U && value.sourceStateId != 0U
						&& value.destinationStateId != 0U && value.durationTicks >= 0
						&& IsFiniteWord(value.progressBits)
						&& IsFiniteWord(value.destinationOffsetBits)
						&& IsFiniteWord(value.interruptedProgressBits);
				});
			const bool layersValid = IsStrictlyOrderedAndValid(snapshot.layers,
				[](const auto& left, const auto& right) { return left.layerStableId < right.layerStableId; },
				[](const auto& value) {
					return value.layerStableId != 0U && value.activeStateId != 0U
						&& IsFiniteWord(value.weightBits);
				});
			const bool parametersValid = IsStrictlyOrderedAndValid(snapshot.parameters,
				[](const auto& left, const auto& right) { return left.parameterId < right.parameterId; },
				[](const auto& value) { return IsValid(value); });
			const bool triggersValid = IsStrictlyOrderedAndValid(snapshot.triggers,
				[](const auto& left, const auto& right) {
					if(left.parameterId != right.parameterId) {
						return left.parameterId < right.parameterId;
					}
					return left.sequenceId < right.sequenceId;
				},
				[](const auto& value) { return value.parameterId != 0U && value.sequenceId != 0U; });
			const bool eventsValid = IsStrictlyOrderedAndValid(snapshot.events,
				[](const auto& left, const auto& right) {
					if(left.streamId != right.streamId) {
						return left.streamId < right.streamId;
					}
					if(left.deliveredSequence != right.deliveredSequence) {
						return left.deliveredSequence < right.deliveredSequence;
					}
					return left.eventId < right.eventId;
				},
				[](const auto& value) {
					return value.streamId != 0U && value.stateInstanceId != 0U
						&& value.clipAssetId.IsValid() && value.eventId != 0U;
				});
			const bool rootsValid = IsStrictlyOrderedAndValid(snapshot.rootMotionStates,
				[](const auto& left, const auto& right) { return left.accumulationId < right.accumulationId; },
				[](const auto& value) {
					return value.accumulationId != 0U && value.sourceEndTick >= value.sourceBeginTick
						&& value.generation != 0U && IsFinite(value.localTranslation)
						&& IsFinite(value.worldTranslation) && IsFiniteWord(value.localYawBits)
						&& IsFiniteWord(value.worldYawBits);
				});
			const bool ikValid = IsStrictlyOrderedAndValid(snapshot.ikStates,
				[](const auto& left, const auto& right) {
					if(left.stableNodeId != right.stableNodeId) {
						return left.stableNodeId < right.stableNodeId;
					}
					return left.goalId < right.goalId;
				},
				[](const auto& value) {
					return value.stableNodeId != 0U && value.goalId != 0U
						&& value.targetStableId != 0U && IsSupported(value.solverState)
						&& IsFiniteWord(value.pelvisOffsetBits) && IsFiniteWord(value.weightBits)
						&& IsFiniteWord(value.errorBits)
						&& (value.contactTargetObjectId == EMPTY_UUID
							|| value.contactTargetObjectGeneration != 0U)
						&& IsFinite(value.lockedContactTransform)
						&& IsFinite(value.contactSurfaceNormal)
						&& IsFiniteWord(value.contactContributionBits)
						&& IsSupported(value.contactPhase);
				});
			const bool retargetValid = IsStrictlyOrderedAndValid(snapshot.retargetStates,
				[](const auto& left, const auto& right) { return left.stableNodeId < right.stableNodeId; },
				[](const auto& value) {
					return value.stableNodeId != 0U && value.profileFingerprint != 0U
						&& value.profileGeneration != 0U && value.sourceSkeletonGeneration != 0U
						&& value.targetSkeletonGeneration != 0U && value.sourcePoseGeneration != 0U
						&& value.targetPoseGeneration != 0U && value.deterministicHash != 0U
						&& IsFiniteWord(value.normalizedTimeBits);
				});
			const bool motionMatchValid = IsStrictlyOrderedAndValid(snapshot.motionMatchStates,
				[](const auto& left, const auto& right) { return left.stableNodeId < right.stableNodeId; },
				[](const auto& value) {
					return value.stableNodeId != 0U && value.databaseFingerprint != 0U
						&& value.databaseGeneration != 0U && value.querySequence != 0U
						&& (value.usedFallback || value.candidateId != 0U)
						&& value.selectionHash != 0U && IsFiniteWord(value.totalScoreBits);
				});
			const bool morphValid = IsStrictlyOrderedAndValid(snapshot.morphStates,
				[](const auto& left, const auto& right) { return left.targetStableId < right.targetStableId; },
				[](const auto& value) {
					return value.targetStableId != 0U && value.generation != 0U
						&& IsFiniteWord(value.weightBits);
				});
			const bool ragdollValid = IsStrictlyOrderedAndValid(snapshot.ragdollStates,
				[](const auto& left, const auto& right) {
					if(left.ragdollInstanceId != right.ragdollInstanceId) {
						return left.ragdollInstanceId < right.ragdollInstanceId;
					}
					return left.bodyId < right.bodyId;
				},
				[](const auto& value) {
					return value.ragdollInstanceId != 0U && value.bodyId != 0U
						&& value.stableJointId != 0U && IsSupported(value.lifecycle)
						&& value.worldGeneration != 0U && value.profileGeneration != 0U
						&& value.skeletonGeneration != 0U
						&& value.physicsPublicationGeneration != 0U && value.bodyGeneration != 0U
						&& value.fixedStep >= 0 && IsFinite(value.modelTransform)
						&& IsFinite(value.linearVelocity) && IsFinite(value.angularVelocity)
						&& IsFiniteWord(value.blendWeightBits);
				});
			const bool extensionsValid = IsStrictlyOrderedAndValid(snapshot.state,
				GenericStateLess,
				[](const auto& value) { return value.stableId != 0U && IsSupported(value.kind); });

			return controllersValid && clipsValid && transitionsValid && layersValid
				&& parametersValid && triggersValid && eventsValid && rootsValid
				&& ikValid && retargetValid && motionMatchValid && morphValid
				&& ragdollValid && extensionsValid;
		}

		[[nodiscard]] std::uint64_t MixUuid(std::uint64_t checksum, const UUID& value) noexcept
		{
			for(const std::uint8_t byte : value) {
				checksum = Mix(checksum, byte);
			}
			return checksum;
		}

		[[nodiscard]] std::uint64_t MixGuid(std::uint64_t checksum, const AnimationGuid& value) noexcept
		{
			return MixUuid(checksum, value.value);
		}

		template<std::size_t Count>
		[[nodiscard]] std::uint64_t MixWords(
			std::uint64_t checksum,
			const std::array<std::uint32_t, Count>& words) noexcept
		{
			for(const std::uint32_t word : words) {
				checksum = Mix(checksum, word);
			}
			return checksum;
		}

		[[nodiscard]] std::uint64_t MixTransform(
			std::uint64_t checksum,
			const AnimationNetworkTransformBits& value) noexcept
		{
			return MixWords(checksum, value.words());
		}

		[[nodiscard]] std::uint64_t MixFingerprint(
			std::uint64_t checksum,
			const AnimationDeterministicFingerprint& value) noexcept
		{
			checksum = Mix(checksum, value.compatibility);
			checksum = Mix(checksum, value.controllerAsset);
			checksum = Mix(checksum, value.programAsset);
			checksum = Mix(checksum, value.retargetAsset);
			checksum = Mix(checksum, value.motionDatabaseAsset);
			checksum = Mix(checksum, value.ragdollProfileAsset);
			return Mix(checksum, value.aggregateAssetHash);
		}

		[[nodiscard]] std::uint64_t MixGenerations(
			std::uint64_t checksum,
			const AnimationRollbackGenerationStamp& value) noexcept
		{
			checksum = Mix(checksum, value.objectGeneration);
			checksum = Mix(checksum, value.componentGeneration);
			checksum = Mix(checksum, value.sceneGeneration);
			checksum = Mix(checksum, value.assetGeneration);
			checksum = Mix(checksum, value.programGeneration);
			checksum = Mix(checksum, value.physicsWorldGeneration);
			return Mix(checksum, value.historyGeneration);
		}

		[[nodiscard]] std::uint64_t BeginSection(
			std::uint64_t checksum,
			AnimationDeterministicStateKind kind,
			std::size_t count) noexcept
		{
			checksum = Mix(checksum, static_cast<std::uint16_t>(kind));
			return Mix(checksum, count);
		}

		[[nodiscard]] AnimationStatus ValidateIntegrity(const AnimationRollbackSnapshot& snapshot) noexcept
		{
			if(snapshot.stateChecksum != ComputeDeterministicStateChecksum(snapshot)
				|| snapshot.poseChecksum != ComputeDeterministicSnapshotChecksum(snapshot)) {
				return AnimationStatus::ContentHashMismatch;
			}
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus ValidateRollbackSnapshot(const AnimationRollbackSnapshot& snapshot) noexcept
	{
		if(snapshot.version != kAnimationDeterminismPacketVersion || !snapshot.animator.IsValid()
			|| snapshot.tick < 0 || snapshot.publicationGeneration == 0U
			|| snapshot.frozenWorkerCount == 0U || !snapshot.fingerprint.IsValid()
			|| (!snapshot.generations.IsEmpty() && !snapshot.generations.IsValid())) {
			return AnimationStatus::InvalidArgument;
		}
		const AnimationStatus countStatus = ValidateCounts(snapshot);
		if(countStatus != AnimationStatus::Ok) {
			return countStatus;
		}
		return ValidateTypedRecords(snapshot) ? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
	}

	std::uint64_t ComputeDeterministicStateChecksum(
		std::span<const AnimationDeterministicStateRecord> state) noexcept
	{
		std::uint64_t checksum = Mix(kFnvOffset, state.size());
		for(const AnimationDeterministicStateRecord& record : state) {
			checksum = Mix(checksum, static_cast<std::uint16_t>(record.kind));
			checksum = Mix(checksum, record.flags);
			checksum = Mix(checksum, record.ordinal);
			checksum = Mix(checksum, record.stableId);
			for(const std::uint64_t value : record.values) {
				checksum = Mix(checksum, value);
			}
		}
		return checksum;
	}

	std::uint64_t ComputeDeterministicStateChecksum(const AnimationRollbackSnapshot& snapshot) noexcept
	{
		std::uint64_t checksum = kFnvOffset;
		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Controller, snapshot.controllers.size());
		for(const AnimationControllerRollbackState& value : snapshot.controllers) {
			checksum = Mix(checksum, value.controllerInstanceId);
			checksum = Mix(checksum, value.activeStateId);
			checksum = Mix(checksum, value.previousStateId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.stateEnteredTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.lastEvaluatedTick));
			checksum = Mix(checksum, value.syncGroupId);
			checksum = Mix(checksum, value.syncMarkerId);
			checksum = Mix(checksum, value.deterministicRandomState);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Clip, snapshot.clips.size());
		for(const AnimationClipRollbackState& value : snapshot.clips) {
			checksum = Mix(checksum, value.clipInstanceId);
			checksum = MixGuid(checksum, value.clipAssetId);
			checksum = Mix(checksum, value.clipGeneration);
			checksum = Mix(checksum, value.clipContentFingerprint);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.absoluteTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.resolvedTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.durationTicks));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.loopCount));
			checksum = Mix(checksum, value.speedBits);
			checksum = Mix(checksum, value.speedQ16);
			checksum = Mix(checksum, value.speedProductRemainder);
			checksum = Mix(checksum, value.subTickRemainderQ16);
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.runState));
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.direction));
			checksum = Mix(checksum, value.looping ? 1U : 0U);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Transition, snapshot.transitions.size());
		for(const AnimationTransitionRollbackState& value : snapshot.transitions) {
			checksum = Mix(checksum, value.transitionStableId);
			checksum = Mix(checksum, value.sourceStateId);
			checksum = Mix(checksum, value.destinationStateId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.startTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.durationTicks));
			checksum = Mix(checksum, value.progressBits);
			checksum = Mix(checksum, value.destinationOffsetBits);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.destinationEnteredTick));
			checksum = Mix(checksum, value.interruptedTransitionStableId);
			checksum = Mix(checksum, value.interruptedProgressBits);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Layer, snapshot.layers.size());
		for(const AnimationLayerRollbackState& value : snapshot.layers) {
			checksum = Mix(checksum, value.layerStableId);
			checksum = Mix(checksum, value.activeStateId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.stateTick));
			checksum = Mix(checksum, value.weightBits);
			checksum = Mix(checksum, value.additive ? 1U : 0U);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Parameter, snapshot.parameters.size());
		for(const AnimationParameterRollbackState& value : snapshot.parameters) {
			checksum = Mix(checksum, value.parameterId);
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.type));
			checksum = MixWords(checksum, value.valueWords);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Trigger, snapshot.triggers.size());
		for(const AnimationTriggerRollbackState& value : snapshot.triggers) {
			checksum = Mix(checksum, value.parameterId);
			checksum = Mix(checksum, value.sequenceId);
			checksum = Mix(checksum, value.raised ? 1U : 0U);
			checksum = Mix(checksum, value.consumed ? 1U : 0U);
			checksum = Mix(checksum, value.resetPending ? 1U : 0U);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Event, snapshot.events.size());
		for(const AnimationEventCursorRollbackState& value : snapshot.events) {
			checksum = Mix(checksum, value.streamId);
			checksum = Mix(checksum, value.stateInstanceId);
			checksum = MixGuid(checksum, value.clipAssetId);
			checksum = Mix(checksum, value.eventId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.sourceTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.loopCount));
			checksum = Mix(checksum, value.deliveredSequence);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.deliveredThroughTick));
			checksum = Mix(checksum, value.delivered ? 1U : 0U);
		}
		checksum = Mix(checksum, snapshot.eventWatermark.streamId);
		checksum = Mix(checksum, snapshot.eventWatermark.deliveredSequence);
		checksum = Mix(checksum, static_cast<std::uint64_t>(snapshot.eventWatermark.deliveredThroughTick));

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::RootMotion, snapshot.rootMotionStates.size());
		for(const AnimationRootMotionRollbackState& value : snapshot.rootMotionStates) {
			checksum = Mix(checksum, value.accumulationId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.sourceBeginTick));
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.sourceEndTick));
			checksum = MixWords(checksum, value.localTranslation.words);
			checksum = MixWords(checksum, value.worldTranslation.words);
			checksum = Mix(checksum, value.localYawBits);
			checksum = Mix(checksum, value.worldYawBits);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.accumulatedCycles));
			checksum = Mix(checksum, value.sourceStateId);
			checksum = Mix(checksum, value.layerIndex);
			checksum = Mix(checksum, value.generation);
			checksum = Mix(checksum, value.correlationId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.appliedThroughTick));
		}
		checksum = Mix(checksum, static_cast<std::uint64_t>(snapshot.rootMotion.sourceTick));
		checksum = MixWords(checksum, snapshot.rootMotion.translationAndYawBits);
		checksum = Mix(checksum, snapshot.rootMotion.correlationId);
		checksum = Mix(checksum, snapshot.rootMotion.requiresSceneTransaction ? 1U : 0U);
		checksum = Mix(checksum, snapshot.rootMotion.requiresPhysicsCommand ? 1U : 0U);

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Ik, snapshot.ikStates.size());
		for(const AnimationIkRollbackState& value : snapshot.ikStates) {
			checksum = Mix(checksum, value.stableNodeId);
			checksum = Mix(checksum, value.goalId);
			checksum = Mix(checksum, value.targetStableId);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.evaluatedTick));
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.solverState));
			checksum = Mix(checksum, value.pelvisOffsetBits);
			checksum = Mix(checksum, value.weightBits);
			checksum = Mix(checksum, value.errorBits);
			checksum = Mix(checksum, value.plantedContactId);
			checksum = MixUuid(checksum, value.contactTargetObjectId);
			checksum = Mix(checksum, value.contactTargetObjectGeneration);
			checksum = MixTransform(checksum, value.lockedContactTransform);
			checksum = MixWords(checksum, value.contactSurfaceNormal.words);
			checksum = Mix(checksum, value.contactContributionBits);
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.contactPhase));
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Retarget, snapshot.retargetStates.size());
		for(const AnimationRetargetRollbackState& value : snapshot.retargetStates) {
			checksum = Mix(checksum, value.stableNodeId);
			checksum = Mix(checksum, value.profileFingerprint);
			checksum = Mix(checksum, value.profileGeneration);
			checksum = Mix(checksum, value.sourceSkeletonGeneration);
			checksum = Mix(checksum, value.targetSkeletonGeneration);
			checksum = Mix(checksum, value.sourcePoseGeneration);
			checksum = Mix(checksum, value.targetPoseGeneration);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.sourceTick));
			checksum = Mix(checksum, value.normalizedTimeBits);
			checksum = Mix(checksum, value.deterministicHash);
			checksum = Mix(checksum, value.reducedFidelity ? 1U : 0U);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::MotionMatch, snapshot.motionMatchStates.size());
		for(const AnimationMotionMatchRollbackState& value : snapshot.motionMatchStates) {
			checksum = Mix(checksum, value.stableNodeId);
			checksum = Mix(checksum, value.databaseFingerprint);
			checksum = Mix(checksum, value.databaseGeneration);
			checksum = Mix(checksum, value.querySequence);
			checksum = Mix(checksum, value.candidateId);
			checksum = Mix(checksum, value.sourceClipFingerprint);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.sampleTick));
			checksum = Mix(checksum, value.totalScoreBits);
			checksum = Mix(checksum, value.selectionHash);
			checksum = Mix(checksum, value.usedFallback ? 1U : 0U);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Morph, snapshot.morphStates.size());
		for(const AnimationMorphRollbackState& value : snapshot.morphStates) {
			checksum = Mix(checksum, value.targetStableId);
			checksum = Mix(checksum, value.sourceStableId);
			checksum = Mix(checksum, value.weightBits);
			checksum = Mix(checksum, value.generation);
		}

		checksum = BeginSection(checksum, AnimationDeterministicStateKind::Ragdoll, snapshot.ragdollStates.size());
		for(const AnimationRagdollRollbackState& value : snapshot.ragdollStates) {
			checksum = Mix(checksum, value.ragdollInstanceId);
			checksum = Mix(checksum, value.bodyId);
			checksum = Mix(checksum, value.stableJointId);
			checksum = Mix(checksum, static_cast<std::uint8_t>(value.lifecycle));
			checksum = Mix(checksum, value.worldGeneration);
			checksum = Mix(checksum, value.profileGeneration);
			checksum = Mix(checksum, value.skeletonGeneration);
			checksum = Mix(checksum, value.physicsPublicationGeneration);
			checksum = Mix(checksum, value.bodyGeneration);
			checksum = Mix(checksum, static_cast<std::uint64_t>(value.fixedStep));
			checksum = MixTransform(checksum, value.modelTransform);
			checksum = MixWords(checksum, value.linearVelocity.words);
			checksum = MixWords(checksum, value.angularVelocity.words);
			checksum = Mix(checksum, value.blendWeightBits);
			checksum = Mix(checksum, value.eventSequence);
			checksum = Mix(checksum, value.sleeping ? 1U : 0U);
		}

		checksum = Mix(checksum, ComputeDeterministicStateChecksum(snapshot.state));
		return checksum;
	}

	std::uint64_t ComputeDeterministicSnapshotChecksum(const AnimationRollbackSnapshot& snapshot) noexcept
	{
		std::uint64_t checksum = ComputeDeterministicStateChecksum(snapshot);
		checksum = Mix(checksum, snapshot.version);
		checksum = Mix(checksum, snapshot.animator.slotIndex);
		checksum = Mix(checksum, snapshot.animator.generation);
		checksum = Mix(checksum, static_cast<std::uint64_t>(snapshot.tick));
		checksum = Mix(checksum, snapshot.publicationGeneration);
		// Worker count is captured as diagnostic provenance, not authority. Leaving
		// it out of the deterministic pose/state identity is what lets the frozen
		// 1/4/16/31-worker matrix compare exact hashes.
		checksum = MixFingerprint(checksum, snapshot.fingerprint);
		return MixGenerations(checksum, snapshot.generations);
	}

	AnimationStatus AnimationRollbackSnapshotCodec::Capture(
		const AnimationRollbackSnapshot& source,
		AnimationRollbackSnapshot& captured) const noexcept
	{
		const AnimationStatus countStatus = ValidateCounts(source);
		if(countStatus != AnimationStatus::Ok) {
			return countStatus;
		}
		try {
			// Build and validate a complete candidate first. Invalid input, duplicate
			// identities, and allocation pressure cannot modify the caller's last-good
			// captured output.
			AnimationRollbackSnapshot candidate = source;
			Canonicalize(candidate);
			const AnimationStatus validation = ValidateRollbackSnapshot(candidate);
			if(validation != AnimationStatus::Ok) {
				return validation;
			}
			candidate.stateChecksum = ComputeDeterministicStateChecksum(candidate);
			candidate.poseChecksum = ComputeDeterministicSnapshotChecksum(candidate);
			captured = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AnimationRollbackSnapshotCodec::Restore(
		const AnimationRollbackSnapshot& snapshot,
		const AnimationRollbackRestoreRequirements& requirements,
		AnimationRollbackSnapshot& restored) const noexcept
	{
		const AnimationStatus validation = ValidateRollbackSnapshot(snapshot);
		if(validation != AnimationStatus::Ok) {
			return validation;
		}
		if(!requirements.animator.IsValid() || !requirements.fingerprint.IsValid()
			|| requirements.publicationGeneration == 0U || !requirements.generations.IsValid()) {
			return AnimationStatus::InvalidArgument;
		}
		if(snapshot.fingerprint != requirements.fingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		if(snapshot.animator != requirements.animator
			|| snapshot.publicationGeneration != requirements.publicationGeneration
			|| snapshot.generations != requirements.generations) {
			return AnimationStatus::StaleHandle;
		}
		const AnimationStatus integrity = ValidateIntegrity(snapshot);
		if(integrity != AnimationStatus::Ok) {
			return integrity;
		}
		try {
			AnimationRollbackSnapshot candidate = snapshot;
			restored = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AnimationRollbackSnapshotCodec::Restore(
		const AnimationRollbackSnapshot& snapshot,
		const AnimationDeterministicFingerprint& expectedFingerprint,
		AnimationGeneration expectedPublicationGeneration,
		AnimationRollbackSnapshot& restored) const noexcept
	{
		const AnimationStatus validation = ValidateRollbackSnapshot(snapshot);
		if(validation != AnimationStatus::Ok) {
			return validation;
		}
		if(!expectedFingerprint.IsValid() || expectedPublicationGeneration == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(snapshot.fingerprint != expectedFingerprint) {
			return AnimationStatus::IncompatibleAsset;
		}
		if(snapshot.publicationGeneration != expectedPublicationGeneration) {
			return AnimationStatus::StaleHandle;
		}
		const AnimationStatus integrity = ValidateIntegrity(snapshot);
		if(integrity != AnimationStatus::Ok) {
			return integrity;
		}
		try {
			AnimationRollbackSnapshot candidate = snapshot;
			restored = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		} catch(const std::length_error&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}
}
