// ProceduralNodeEvaluator.h - Ordered procedural graph evaluation over copied poses.

#pragma once

#include "Animation/Diagnostics/ProceduralDiagnostics.h"
#include "Animation/Graph/LayerEvaluator.h"
#include "Animation/Procedural/AnimationConstraints.h"
#include "Animation/Procedural/ChainIk.h"
#include "Animation/Procedural/FootPlacement.h"
#include "Animation/Procedural/TwoBoneIk.h"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace TerraEngine::Animation {
	// The CPU evaluator is also the reference implementation used to validate a
	// visual GPU pass. Visual-only output must never satisfy gameplay, recording,
	// rollback, networking, physics, or another authoritative consumer.
	enum class ProceduralGraphAuthority : std::uint8_t {
		VisualOnly = 0U,
		CpuAuthoritative = 1U
	};

	struct ProceduralTwoBoneNodeInput {
		std::uint32_t rootJointIndex = kInvalidProceduralJointIndex;
		std::uint32_t middleJointIndex = kInvalidProceduralJointIndex;
		std::uint32_t endJointIndex = kInvalidProceduralJointIndex;
		AnimationFloat3 targetPosition{};
		AnimationFloat3 polePosition{0.0F, 1.0F, 0.0F};
		ProceduralJointLimit middleLimit{};
		ProceduralCoordinateSpace space = ProceduralCoordinateSpace::Model;
		std::uint64_t goalId = 0U;
		std::uint64_t producerOrder = 0U;
		std::int32_t priority = 0;
		float solverWeight = 1.0F;
		float maximumStretchScale = 1.0F;
		float twistRadians = 0.0F;
		float toleranceMeters = kProceduralPositionToleranceMeters;
		bool allowStretch = false;
	};

	// Chain storage is owned by the immutable graph packet. The evaluator builds
	// temporary solver spans only after copying the current complete pose.
	struct ProceduralChainNodeInput {
		std::vector<std::uint32_t> jointIndices;
		std::vector<ProceduralJointLimit> jointLimits;
		ProceduralGoalSnapshot goals;
		std::uint32_t maximumIterations = kChainIkIterationDefault;
		float toleranceMeters = kProceduralPositionToleranceMeters;
	};

	struct ProceduralAimNodeInput {
		std::uint32_t jointIndex = kInvalidProceduralJointIndex;
		ProceduralGoalSnapshot goals;
		AnimationFloat3 localAimAxis{0.0F, 0.0F, 1.0F};
		AnimationFloat3 localUpAxis{0.0F, 1.0F, 0.0F};
		AnimationFloat3 worldUp{0.0F, 1.0F, 0.0F};
		float maximumAngleRadians = 3.14159265358979323846F;
		float dampingRate = 0.0F;
		float deltaSeconds = 0.0F;
		float solverWeight = 1.0F;
	};

	struct ProceduralLookNodeInput {
		ProceduralAimNodeInput aim;
		float maximumYawRadians = 3.14159265358979323846F;
		float maximumPitchRadians = 1.57079632679489661923F;
	};

	struct ProceduralCopyTransformNodeInput {
		std::uint32_t jointIndex = kInvalidProceduralJointIndex;
		std::uint32_t parentJointIndex = kInvalidProceduralJointIndex;
		AnimationTransform targetTransform{};
		ProceduralCoordinateSpace targetSpace = ProceduralCoordinateSpace::Parent;
		float translationWeight = 1.0F;
		float rotationWeight = 1.0F;
		float scaleWeight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	struct ProceduralJointConstraintNodeInput {
		std::uint32_t jointIndex = kInvalidProceduralJointIndex;
		AnimationQuaternion referenceRotation{};
		AnimationFloat3 localTwistAxis{1.0F, 0.0F, 0.0F};
		ProceduralJointLimit limit{};
		float solverWeight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	struct ProceduralTwistNodeInput {
		std::vector<std::uint32_t> jointIndices;
		std::vector<float> distributionWeights;
		AnimationFloat3 localTwistAxis{1.0F, 0.0F, 0.0F};
		float twistRadians = 0.0F;
		float solverWeight = 1.0F;
		std::uint64_t goalId = 0U;
	};

	struct ProceduralFootBinding {
		std::uint32_t jointIndex = kInvalidProceduralJointIndex;
		FootPlacementFootInput input;
	};

	struct ProceduralFootPlacementNodeInput {
		std::uint32_t pelvisJointIndex = kInvalidProceduralJointIndex;
		ProceduralQuerySnapshot queries;
		std::vector<ProceduralFootBinding> feet;
		float previousPelvisOffsetMeters = 0.0F;
		float maximumPelvisDropMeters = 0.35F;
		float maximumPelvisRaiseMeters = 0.20F;
		float pelvisWeight = 1.0F;
		float deltaSeconds = 0.0F;
		float releaseSmoothingSeconds = 0.10F;
		float pelvisSmoothingSeconds = 0.08F;
		AnimationFixedTick maximumQueryAgeTicks = 1;
	};

	using ProceduralGraphNodePayload = std::variant<
		ProceduralTwoBoneNodeInput,
		ProceduralChainNodeInput,
		ProceduralAimNodeInput,
		ProceduralLookNodeInput,
		ProceduralCopyTransformNodeInput,
		ProceduralJointConstraintNodeInput,
		ProceduralTwistNodeInput,
		ProceduralFootPlacementNodeInput>;

	// layerOrder and nodeOrder are explicit authored keys. stableId breaks ties
	// deterministically and is also the durable identity recorded in snapshots.
	struct ProceduralGraphNodeInput {
		std::uint64_t stableId = 0U;
		std::uint32_t layerOrder = 0U;
		std::uint32_t nodeOrder = 0U;
		AnimationGraphLayerMode layerMode = AnimationGraphLayerMode::Override;
		float controllerWeight = 1.0F;
		float nodeWeight = 1.0F;
		float layerWeight = 1.0F;
		const AnimationMaskAsset* mask = nullptr;
		const GraphPoseContribution* additiveReference = nullptr;
		ProceduralGraphNodePayload payload;
	};

	// Stateful operations currently consist of planted-foot contacts and pelvis
	// smoothing. Capturing them beside the pose makes rollback complete without
	// retaining pointers into a physics or scene-query provider.
	struct ProceduralNodeStateSnapshot {
		std::uint64_t stableNodeId = 0U;
		float pelvisOffsetMeters = 0.0F;
		std::vector<FootContactState> footContacts;
		auto operator<=>(const ProceduralNodeStateSnapshot&) const = default;
	};

	struct ProceduralPoseSnapshot {
		std::uint32_t schemaVersion = 1U;
		ProceduralGenerationStamp generation;
		AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		GraphPoseContribution pose;
		std::vector<std::uint64_t> orderedNodeIds;
		std::vector<ProceduralNodeStateSnapshot> nodeStates;
	};

	struct ProceduralGraphEvaluationRequest {
		ProceduralGenerationStamp generation;
		ProceduralGenerationStamp expectedGeneration;
		const GraphPoseContribution* inputPose = nullptr;
		std::span<const ProceduralGraphNodeInput> nodes;
		ProceduralGraphAuthority authority = ProceduralGraphAuthority::CpuAuthoritative;
		bool authoritativeConsumer = false;
		bool recordingRequested = false;
		bool rollbackRequested = false;
	};

	struct ProceduralGraphEvaluationResult {
		ProceduralResultHeader header;
		GraphPoseContribution pose;
		std::vector<std::uint64_t> orderedNodeIds;
		std::vector<ProceduralDiagnosticRecord> diagnostics;
		std::vector<ProceduralNodeStateSnapshot> nodeStates;
		std::optional<ProceduralPoseSnapshot> recordingSnapshot;
		std::optional<ProceduralPoseSnapshot> rollbackSnapshot;
		bool cpuAuthoritative = false;
	};

	// The caller's result is replaced only after all nodes, layers, diagnostics,
	// and optional snapshots have staged successfully. A stale or malformed goal
	// therefore cannot expose a partially procedural pose.
	TERRAENGINE_API AnimationStatus EvaluateProceduralGraphNodes(
		const ProceduralGraphEvaluationRequest& request,
		ProceduralGraphEvaluationResult& output) noexcept;

	// Rollback validation copies both pose and state into temporary storage before
	// replacing either output, preserving the previous publication on rejection.
	TERRAENGINE_API AnimationStatus RestoreProceduralPoseSnapshot(
		const ProceduralPoseSnapshot& snapshot,
		const ProceduralGenerationStamp& expectedGeneration,
		AnimationCompatibilityFingerprint expectedCompatibilityFingerprint,
		GraphPoseContribution& outputPose,
		std::vector<ProceduralNodeStateSnapshot>& outputNodeStates) noexcept;
}
