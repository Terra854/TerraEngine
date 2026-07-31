// AnimationNetworkPackets.h - Versioned copied values for deterministic animation transport.
//
// Network reliability, fragmentation, and encryption belong to the host game.
// Animation owns only bounded semantic values. The records below intentionally
// contain no pointer, renderer handle, transient resource index, or backend type.

#pragma once

#include "Animation/AnimationTypes.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"

#include <array>
#include <bit>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace TerraEngine::Animation {
	inline constexpr std::uint16_t kAnimationDeterminismPacketVersion = 1U;
	inline constexpr std::size_t kAnimationDeterminismMaximumStateRecords = 256U;
	inline constexpr std::size_t kAnimationDeterminismMaximumPacketRecords = 256U;
	inline constexpr std::size_t kAnimationDeterminismMaximumPacketBytes = 1024U * 1024U;
	inline constexpr std::size_t kAnimationDeterminismDefaultHistorySnapshots =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::RollbackHistoryTicks).operationalDefault);
	inline constexpr std::size_t kAnimationDeterminismMaximumHistorySnapshots =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::RollbackHistoryTicks).hardMaximum);

	// Each domain has an explicit independent bound. A malformed snapshot is
	// rejected from counts alone before copying or sorting any variable payload.
	inline constexpr std::size_t kAnimationRollbackMaximumControllerStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumClipStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::PackageClips).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumTransitionStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::TransitionsPerState).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumLayerStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerLayers).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumParameterStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerParameters).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumTriggerStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerParameters).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumEventCursorStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumRootMotionStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerLayers).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumIkStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumRetargetStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::AnimatorProceduralGoals).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumMotionMatchStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::MotionQueryTopK).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumMorphStates =
		static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::MeshMorphTargets).hardMaximum);
	inline constexpr std::size_t kAnimationRollbackMaximumRagdollStates = 256U;

	// Stable float bits prevent an ABI serializer from depending on native
	// structure padding. Codecs write each word explicitly in transport byte order.
	[[nodiscard]] constexpr std::uint32_t EncodeAnimationNetworkFloat(float value) noexcept
	{
		return std::bit_cast<std::uint32_t>(value);
	}

	[[nodiscard]] constexpr float DecodeAnimationNetworkFloat(std::uint32_t value) noexcept
	{
		return std::bit_cast<float>(value);
	}

	struct AnimationNetworkFloat3Bits {
		std::array<std::uint32_t, 3U> words{};
		auto operator<=>(const AnimationNetworkFloat3Bits&) const = default;
	};

	struct AnimationNetworkQuaternionBits {
		std::array<std::uint32_t, 4U> words{};
		auto operator<=>(const AnimationNetworkQuaternionBits&) const = default;
	};

	struct AnimationNetworkTransformBits {
		AnimationNetworkFloat3Bits translation{};
		AnimationNetworkQuaternionBits rotation{};
		AnimationNetworkFloat3Bits scale{};

		[[nodiscard]] constexpr std::array<std::uint32_t, 10U> words() const noexcept
		{
			return {
				translation.words[0], translation.words[1], translation.words[2],
				rotation.words[0], rotation.words[1], rotation.words[2], rotation.words[3],
				scale.words[0], scale.words[1], scale.words[2]};
		}

		auto operator<=>(const AnimationNetworkTransformBits&) const = default;
	};

	// Tests apply this concept to every transport leaf. Owning packets may use
	// vectors, but each element is a fixed-width copied standard-layout value.
	template<typename Type>
	concept AnimationTransportValueRecord = std::is_trivially_copyable_v<Type>
		&& std::is_standard_layout_v<Type>;

	// Every restore compares the content/program identity before any copied state
	// becomes observable. Generation alone is insufficient after reimporting the
	// same stable authored asset with different bytes.
	struct AnimationDeterministicFingerprint {
		AnimationCompatibilityFingerprint compatibility = 0U;
		std::uint64_t controllerAsset = 0U;
		std::uint64_t programAsset = 0U;
		std::uint64_t retargetAsset = 0U;
		std::uint64_t motionDatabaseAsset = 0U;
		std::uint64_t ragdollProfileAsset = 0U;
		std::uint64_t aggregateAssetHash = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return compatibility != 0U && controllerAsset != 0U
				&& programAsset != 0U && aggregateAssetHash != 0U;
		}

		auto operator<=>(const AnimationDeterministicFingerprint&) const = default;
	};

	// FunctionalContract requires more than the animator slot generation. This stamp names
	// every live authority that can make an otherwise valid snapshot stale.
	struct AnimationRollbackGenerationStamp {
		AnimationGeneration objectGeneration = 0U;
		AnimationGeneration componentGeneration = 0U;
		AnimationGeneration sceneGeneration = 0U;
		AnimationGeneration assetGeneration = 0U;
		AnimationGeneration programGeneration = 0U;
		AnimationGeneration physicsWorldGeneration = 0U;
		AnimationGeneration historyGeneration = 0U;

		[[nodiscard]] constexpr bool IsEmpty() const noexcept
		{
			return objectGeneration == 0U && componentGeneration == 0U
				&& sceneGeneration == 0U && assetGeneration == 0U
				&& programGeneration == 0U && physicsWorldGeneration == 0U
				&& historyGeneration == 0U;
		}

		[[nodiscard]] constexpr bool IsValid() const noexcept
		{
			return objectGeneration != 0U && componentGeneration != 0U
				&& sceneGeneration != 0U && assetGeneration != 0U
				&& programGeneration != 0U && physicsWorldGeneration != 0U
				&& historyGeneration != 0U;
		}

		auto operator<=>(const AnimationRollbackGenerationStamp&) const = default;
	};

	enum class AnimationRollbackPlaybackState : std::uint8_t {
		Stopped = 0U,
		Playing = 1U,
		Paused = 2U,
		Completed = 3U
	};

	enum class AnimationRollbackPlaybackDirection : std::int8_t {
		Reverse = -1,
		Stationary = 0,
		Forward = 1
	};

	// This enum mirrors the copied runtime parameter shapes without importing a
	// variant or a mutable command queue into the transport contract.
	enum class AnimationRollbackValueType : std::uint8_t {
		Trigger = 0U,
		Boolean = 1U,
		Integer = 2U,
		Float = 3U,
		Vector3 = 4U,
		Quaternion = 5U,
		Transform = 6U
	};

	enum class AnimationRollbackIkState : std::uint8_t {
		Solved = 0U,
		Converged = 1U,
		MaxIterations = 2U,
		Unreachable = 3U,
		Singular = 4U,
		Clamped = 5U,
		StaleInput = 6U,
		NoContribution = 7U
	};

	enum class AnimationRollbackFootContactPhase : std::uint8_t {
		Released = 0U,
		Locked = 1U,
		Releasing = 2U
	};

	enum class AnimationRollbackRagdollState : std::uint8_t {
		Inactive = 0U,
		Creating = 1U,
		Active = 2U,
		Recovering = 3U,
		TearingDown = 4U,
		Rejected = 5U
	};

	struct AnimationControllerRollbackState {
		std::uint64_t controllerInstanceId = 0U;
		std::uint64_t activeStateId = 0U;
		std::uint64_t previousStateId = 0U;
		AnimationFixedTick stateEnteredTick = 0;
		AnimationFixedTick lastEvaluatedTick = 0;
		std::uint64_t syncGroupId = 0U;
		std::uint64_t syncMarkerId = 0U;
		std::uint64_t deterministicRandomState = 0U;
		auto operator<=>(const AnimationControllerRollbackState&) const = default;
	};

	struct AnimationClipRollbackState {
		std::uint64_t clipInstanceId = 0U;
		AnimationGuid clipAssetId{};
		AnimationGeneration clipGeneration = 0U;
		std::uint64_t clipContentFingerprint = 0U;
		AnimationFixedTick absoluteTick = 0;
		AnimationFixedTick resolvedTick = 0;
		AnimationFixedTick durationTicks = 0;
		std::int64_t loopCount = 0;
		std::uint32_t speedBits = EncodeAnimationNetworkFloat(1.0F);
		std::uint32_t speedQ16 = 1U << 16U;
		std::uint64_t speedProductRemainder = 0U;
		std::uint32_t subTickRemainderQ16 = 0U;
		AnimationRollbackPlaybackState runState = AnimationRollbackPlaybackState::Stopped;
		AnimationRollbackPlaybackDirection direction = AnimationRollbackPlaybackDirection::Stationary;
		bool looping = false;
		auto operator<=>(const AnimationClipRollbackState&) const = default;
	};

	struct AnimationTransitionRollbackState {
		std::uint64_t transitionStableId = 0U;
		std::uint64_t sourceStateId = 0U;
		std::uint64_t destinationStateId = 0U;
		AnimationFixedTick startTick = 0;
		AnimationFixedTick durationTicks = 0;
		std::uint32_t progressBits = 0U;
		std::uint32_t destinationOffsetBits = 0U;
		AnimationFixedTick destinationEnteredTick = 0;
		std::uint64_t interruptedTransitionStableId = 0U;
		std::uint32_t interruptedProgressBits = 0U;
		auto operator<=>(const AnimationTransitionRollbackState&) const = default;
	};

	struct AnimationLayerRollbackState {
		std::uint64_t layerStableId = 0U;
		std::uint64_t activeStateId = 0U;
		AnimationFixedTick stateTick = 0;
		std::uint32_t weightBits = EncodeAnimationNetworkFloat(1.0F);
		bool additive = false;
		auto operator<=>(const AnimationLayerRollbackState&) const = default;
	};

	// Ten words fit the largest v1 parameter value: translation, quaternion, and
	// scale. Smaller types must leave unused words zero so one value has one hash.
	struct AnimationParameterRollbackState {
		std::uint64_t parameterId = 0U;
		AnimationRollbackValueType type = AnimationRollbackValueType::Float;
		std::array<std::uint32_t, 10U> valueWords{};
		auto operator<=>(const AnimationParameterRollbackState&) const = default;
	};

	struct AnimationTriggerRollbackState {
		std::uint64_t parameterId = 0U;
		std::uint64_t sequenceId = 0U;
		bool raised = false;
		bool consumed = false;
		bool resetPending = false;
		auto operator<=>(const AnimationTriggerRollbackState&) const = default;
	};

	struct AnimationEventCursorRollbackState {
		std::uint64_t streamId = 0U;
		std::uint64_t stateInstanceId = 0U;
		AnimationGuid clipAssetId{};
		std::uint64_t eventId = 0U;
		AnimationFixedTick sourceTick = 0;
		std::int64_t loopCount = 0;
		std::uint64_t deliveredSequence = 0U;
		AnimationFixedTick deliveredThroughTick = 0;
		bool delivered = false;
		auto operator<=>(const AnimationEventCursorRollbackState&) const = default;
	};

	struct AnimationRootMotionRollbackState {
		std::uint64_t accumulationId = 0U;
		AnimationFixedTick sourceBeginTick = 0;
		AnimationFixedTick sourceEndTick = 0;
		AnimationNetworkFloat3Bits localTranslation{};
		AnimationNetworkFloat3Bits worldTranslation{};
		std::uint32_t localYawBits = 0U;
		std::uint32_t worldYawBits = 0U;
		std::int64_t accumulatedCycles = 0;
		std::uint64_t sourceStateId = 0U;
		std::uint32_t layerIndex = 0U;
		AnimationGeneration generation = 0U;
		std::uint64_t correlationId = 0U;
		AnimationFixedTick appliedThroughTick = 0;
		auto operator<=>(const AnimationRootMotionRollbackState&) const = default;
	};

	struct AnimationIkRollbackState {
		std::uint64_t stableNodeId = 0U;
		std::uint64_t goalId = 0U;
		std::uint64_t targetStableId = 0U;
		AnimationFixedTick evaluatedTick = 0;
		AnimationRollbackIkState solverState = AnimationRollbackIkState::NoContribution;
		std::uint32_t pelvisOffsetBits = 0U;
		std::uint32_t weightBits = 0U;
		std::uint32_t errorBits = 0U;
		std::uint64_t plantedContactId = 0U;
		UUID contactTargetObjectId = EMPTY_UUID;
		AnimationGeneration contactTargetObjectGeneration = 0U;
		AnimationNetworkTransformBits lockedContactTransform{};
		AnimationNetworkFloat3Bits contactSurfaceNormal{};
		std::uint32_t contactContributionBits = 0U;
		AnimationRollbackFootContactPhase contactPhase = AnimationRollbackFootContactPhase::Released;
		auto operator<=>(const AnimationIkRollbackState&) const = default;
	};

	struct AnimationRetargetRollbackState {
		std::uint64_t stableNodeId = 0U;
		std::uint64_t profileFingerprint = 0U;
		AnimationGeneration profileGeneration = 0U;
		AnimationGeneration sourceSkeletonGeneration = 0U;
		AnimationGeneration targetSkeletonGeneration = 0U;
		AnimationGeneration sourcePoseGeneration = 0U;
		AnimationGeneration targetPoseGeneration = 0U;
		AnimationFixedTick sourceTick = 0;
		std::uint32_t normalizedTimeBits = 0U;
		std::uint64_t deterministicHash = 0U;
		bool reducedFidelity = false;
		auto operator<=>(const AnimationRetargetRollbackState&) const = default;
	};

	struct AnimationMotionMatchRollbackState {
		std::uint64_t stableNodeId = 0U;
		std::uint64_t databaseFingerprint = 0U;
		AnimationGeneration databaseGeneration = 0U;
		std::uint64_t querySequence = 0U;
		std::uint64_t candidateId = 0U;
		std::uint64_t sourceClipFingerprint = 0U;
		AnimationFixedTick sampleTick = 0;
		std::uint32_t totalScoreBits = 0U;
		std::uint64_t selectionHash = 0U;
		bool usedFallback = false;
		auto operator<=>(const AnimationMotionMatchRollbackState&) const = default;
	};

	struct AnimationMorphRollbackState {
		std::uint64_t targetStableId = 0U;
		std::uint64_t sourceStableId = 0U;
		std::uint32_t weightBits = 0U;
		AnimationGeneration generation = 0U;
		auto operator<=>(const AnimationMorphRollbackState&) const = default;
	};

	struct AnimationRagdollRollbackState {
		std::uint64_t ragdollInstanceId = 0U;
		std::uint64_t bodyId = 0U;
		std::uint64_t stableJointId = 0U;
		AnimationRollbackRagdollState lifecycle = AnimationRollbackRagdollState::Inactive;
		AnimationGeneration worldGeneration = 0U;
		AnimationGeneration profileGeneration = 0U;
		AnimationGeneration skeletonGeneration = 0U;
		AnimationGeneration physicsPublicationGeneration = 0U;
		AnimationGeneration bodyGeneration = 0U;
		AnimationFixedTick fixedStep = 0;
		AnimationNetworkTransformBits modelTransform{};
		AnimationNetworkFloat3Bits linearVelocity{};
		AnimationNetworkFloat3Bits angularVelocity{};
		std::uint32_t blendWeightBits = 0U;
		std::uint64_t eventSequence = 0U;
		bool sleeping = false;
		auto operator<=>(const AnimationRagdollRollbackState&) const = default;
	};

	static_assert(AnimationTransportValueRecord<AnimationControllerRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationClipRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationTransitionRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationLayerRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationParameterRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationTriggerRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationEventCursorRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationRootMotionRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationIkRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationRetargetRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationMotionMatchRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationMorphRollbackState>);
	static_assert(AnimationTransportValueRecord<AnimationRagdollRollbackState>);

	// Generic records remain the append-only extension seam used by the later
	// resimulation and replication tasks. Complete v1 domains use the typed records
	// above so transform-sized state is never truncated into four opaque words.
	enum class AnimationDeterministicStateKind : std::uint16_t {
		Controller = 1U,
		Clip = 2U,
		Transition = 3U,
		Layer = 4U,
		Parameter = 5U,
		Trigger = 6U,
		Event = 7U,
		RootMotion = 8U,
		Ik = 9U,
		Retarget = 10U,
		MotionMatch = 11U,
		Morph = 12U,
		Ragdoll = 13U,
		Attachment = 14U,
		Physics = 15U
	};

	struct AnimationDeterministicStateRecord {
		AnimationDeterministicStateKind kind = AnimationDeterministicStateKind::Controller;
		std::uint16_t flags = 0U;
		std::uint32_t ordinal = 0U;
		std::uint64_t stableId = 0U;
		std::array<std::uint64_t, 4U> values{};
		auto operator<=>(const AnimationDeterministicStateRecord&) const = default;
	};

	struct AnimationEventWatermark {
		std::uint64_t streamId = 0U;
		std::uint64_t deliveredSequence = 0U;
		AnimationFixedTick deliveredThroughTick = 0;
		auto operator<=>(const AnimationEventWatermark&) const = default;
	};

	struct AnimationRootMotionCorrection {
		AnimationFixedTick sourceTick = 0;
		std::array<std::uint32_t, 4U> translationAndYawBits{};
		std::uint64_t correlationId = 0U;
		bool requiresSceneTransaction = false;
		bool requiresPhysicsCommand = false;
		auto operator<=>(const AnimationRootMotionCorrection&) const = default;
	};

	// This is an owning immutable-by-publication value. Capture canonicalizes and
	// validates a staged copy, while History retains only complete captured values.
	struct AnimationRollbackSnapshot {
		std::uint16_t version = kAnimationDeterminismPacketVersion;
		AnimatorInstanceHandle animator{};
		AnimationFixedTick tick = 0;
		AnimationGeneration publicationGeneration = 0U;
		std::uint32_t frozenWorkerCount = 1U;
		AnimationDeterministicFingerprint fingerprint{};
		AnimationRollbackGenerationStamp generations{};
		std::uint64_t poseChecksum = 0U;
		std::uint64_t stateChecksum = 0U;
		AnimationEventWatermark eventWatermark{};
		AnimationRootMotionCorrection rootMotion{};
		std::vector<AnimationControllerRollbackState> controllers;
		std::vector<AnimationClipRollbackState> clips;
		std::vector<AnimationTransitionRollbackState> transitions;
		std::vector<AnimationLayerRollbackState> layers;
		std::vector<AnimationParameterRollbackState> parameters;
		std::vector<AnimationTriggerRollbackState> triggers;
		std::vector<AnimationEventCursorRollbackState> events;
		std::vector<AnimationRootMotionRollbackState> rootMotionStates;
		std::vector<AnimationIkRollbackState> ikStates;
		std::vector<AnimationRetargetRollbackState> retargetStates;
		std::vector<AnimationMotionMatchRollbackState> motionMatchStates;
		std::vector<AnimationMorphRollbackState> morphStates;
		std::vector<AnimationRagdollRollbackState> ragdollStates;
		std::vector<AnimationDeterministicStateRecord> state;
		auto operator<=>(const AnimationRollbackSnapshot&) const = default;
	};

	// Packet kinds stay transport-neutral. Networking owns delivery behavior;
	// animation owns the versioned meaning and hard bounds of copied records.
	enum class AnimationNetworkPacketKind : std::uint16_t {
		AuthoritativeState = 1U,
		PredictedInput = 2U,
		LateJoinSnapshot = 3U,
		ReplaySeek = 4U,
		DivergenceReport = 5U
	};

	struct AnimationNetworkPacketHeader {
		std::uint16_t version = kAnimationDeterminismPacketVersion;
		AnimationNetworkPacketKind kind = AnimationNetworkPacketKind::AuthoritativeState;
		std::uint32_t byteCount = 0U;
		std::uint64_t sequence = 0U;
		AnimatorInstanceHandle animator{};
		AnimationFixedTick tick = 0;
		AnimationGeneration publicationGeneration = 0U;
		AnimationDeterministicFingerprint fingerprint{};
		// The explicit codec hashes only the bounded semantic payload. Transport
		// framing, retransmission, encryption, and authentication remain host-owned.
		std::uint64_t payloadChecksum = 0U;
		auto operator<=>(const AnimationNetworkPacketHeader&) const = default;
	};

	// Commands are deliberately semantic rather than ABI handles. Their copied
	// payload words are interpreted by the immutable evaluation program named by
	// the packet fingerprint after the complete envelope has passed validation.
	enum class AnimationReplicatedCommandKind : std::uint16_t {
		Play = 0U,
		CrossFade = 1U,
		Seek = 2U,
		SetLayerWeight = 3U,
		SetIkTarget = 4U,
		ConsumeRootMotion = 5U,
		SetRagdollState = 6U,
		ResetTrigger = 7U
	};

	struct AnimationReplicatedCommandRecord {
		std::uint64_t commandId = 0U;
		std::uint64_t targetStableId = 0U;
		std::uint64_t correlationId = 0U;
		AnimationFixedTick effectiveTick = 0;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		AnimationReplicatedCommandKind operation = AnimationReplicatedCommandKind::Play;
		std::uint8_t payloadWordCount = 0U;
		std::array<std::uint32_t, 10U> payloadWords{};
		bool predicted = false;
		auto operator<=>(const AnimationReplicatedCommandRecord&) const = default;
	};

	struct AnimationReplicatedParameterRecord {
		AnimationParameterRollbackState parameter{};
		AnimationFixedTick effectiveTick = 0;
		std::uint32_t producerId = 0U;
		std::uint64_t producerSequence = 0U;
		bool predicted = false;
		auto operator<=>(const AnimationReplicatedParameterRecord&) const = default;
	};

	struct AnimationReplicationRecord {
		AnimationDeterministicStateRecord state;
		bool predicted = false;
		auto operator<=>(const AnimationReplicationRecord&) const = default;
	};

	struct AnimationReplicatedRootMotionRecord {
		AnimationRootMotionRollbackState state{};
		bool acknowledged = false;
		bool predicted = false;
		auto operator<=>(const AnimationReplicatedRootMotionRecord&) const = default;
	};

	struct AnimationReplicatedMotionMatchRecord {
		AnimationMotionMatchRollbackState state{};
		bool predicted = false;
		auto operator<=>(const AnimationReplicatedMotionMatchRecord&) const = default;
	};

	struct AnimationReplicatedRagdollRecord {
		AnimationRagdollRollbackState state{};
		bool predicted = false;
		auto operator<=>(const AnimationReplicatedRagdollRecord&) const = default;
	};

	static_assert(AnimationTransportValueRecord<AnimationReplicatedCommandRecord>);
	static_assert(AnimationTransportValueRecord<AnimationReplicatedParameterRecord>);
	static_assert(AnimationTransportValueRecord<AnimationReplicationRecord>);
	static_assert(AnimationTransportValueRecord<AnimationReplicatedRootMotionRecord>);
	static_assert(AnimationTransportValueRecord<AnimationReplicatedMotionMatchRecord>);
	static_assert(AnimationTransportValueRecord<AnimationReplicatedRagdollRecord>);

	struct AnimationReplicationPacket {
		AnimationNetworkPacketHeader header{};
		std::vector<AnimationReplicatedCommandRecord> commands;
		std::vector<AnimationReplicatedParameterRecord> parameters;
		std::vector<AnimationReplicationRecord> records;
		std::vector<AnimationReplicatedRootMotionRecord> rootMotion;
		std::vector<AnimationReplicatedMotionMatchRecord> motionMatches;
		std::vector<AnimationReplicatedRagdollRecord> ragdolls;
		auto operator<=>(const AnimationReplicationPacket&) const = default;
	};

	struct AnimationRollbackSnapshotPacket {
		AnimationNetworkPacketHeader header{};
		AnimationRollbackSnapshot snapshot{};
		auto operator<=>(const AnimationRollbackSnapshotPacket&) const = default;
	};

	// A field reason separates an asset mismatch from a graph, event, root, or
	// motion decision even when all of them ultimately alter the pose checksum.
	enum class AnimationDivergenceField : std::uint8_t {
		None = 0U,
		MissingSnapshot = 1U,
		AnimatorIdentity = 2U,
		Tick = 3U,
		AssetFingerprint = 4U,
		AssetGeneration = 5U,
		GraphElement = 6U,
		PoseElement = 7U,
		PoseChecksum = 8U,
		Event = 9U,
		RootMotion = 10U,
		MotionMatch = 11U,
		SnapshotIntegrity = 12U
	};

	struct AnimationDivergencePoint {
		AnimatorInstanceHandle animator{};
		AnimationFixedTick tick = 0;
		AnimationDivergenceField field = AnimationDivergenceField::None;
		AnimationDeterministicStateKind category = AnimationDeterministicStateKind::Controller;
		std::uint64_t stableId = 0U;
		std::uint32_t ordinal = 0U;
		std::uint64_t expectedChecksum = 0U;
		std::uint64_t actualChecksum = 0U;
		AnimationGeneration expectedGeneration = 0U;
		AnimationGeneration actualGeneration = 0U;
		AnimationDeterministicFingerprint expectedFingerprint{};
		AnimationDeterministicFingerprint actualFingerprint{};
		bool expectedPresent = true;
		bool actualPresent = true;
		bool found = false;
		auto operator<=>(const AnimationDivergencePoint&) const = default;
	};

	[[nodiscard]] TERRAENGINE_API AnimationStatus ValidateRollbackSnapshot(
		const AnimationRollbackSnapshot& snapshot) noexcept;
	[[nodiscard]] TERRAENGINE_API std::uint64_t ComputeDeterministicStateChecksum(
		std::span<const AnimationDeterministicStateRecord> state) noexcept;
	[[nodiscard]] TERRAENGINE_API std::uint64_t ComputeDeterministicStateChecksum(
		const AnimationRollbackSnapshot& snapshot) noexcept;
	[[nodiscard]] TERRAENGINE_API std::uint64_t ComputeDeterministicSnapshotChecksum(
		const AnimationRollbackSnapshot& snapshot) noexcept;
}
