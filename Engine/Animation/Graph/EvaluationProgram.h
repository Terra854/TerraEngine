// EvaluationProgram.h - Immutable, deterministic CPU animation evaluation plan.

#pragma once

#include "Animation/AnimationTypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
// These explicit values are part of the compiled-program ABI. Append only.
enum class EvaluationOpcode : std::uint8_t {
	SampleClip = 0U,
	BlendTree1D = 1U,
	BlendTree2D = 2U,
	CallSubgraph = 3U,
	MotionMatch = 4U,
	ProceduralPose = 5U
};

enum class EvaluationTransitionInterruptionSource : std::uint8_t {
	None = 0U,
	Current = 1U,
	Next = 2U
};
enum class EvaluationTransitionInterruptionPolicy : std::uint8_t {
	Reject = 0U,
	Replace = 1U
};

// Trigger consumption controls whether one transition owns an edge or whether
// the same edge may remain visible to another graph consumer. Reset and failure
// behavior are stored separately on EvaluationTransition below.
enum class EvaluationTriggerConsumption : std::uint8_t {
	SingleConsumer = 0U,
	MultiConsumer = 1U
};
enum class AnimationBlendOutOfDomainPolicy : std::uint8_t { Clamp = 0U, Extrapolate = 1U };

struct EvaluationState {
	std::uint64_t stableId = 0U;
	std::uint64_t parentStateId = 0U;
	bool entry = false;
	bool exit = false;
	bool anyState = false;
	std::uint64_t nodeId = 0U;
	EvaluationOpcode nodeOpcode = EvaluationOpcode::ProceduralPose;
	auto operator<=>(const EvaluationState &) const = default;
};

struct EvaluationInstruction {
	EvaluationOpcode opcode = EvaluationOpcode::SampleClip;
	std::uint64_t sourceStableId = 0U;
	AnimationGuid dependencyAssetId{};
	AnimationGeneration dependencyGeneration = 0U;
	AnimationCompatibilityFingerprint dependencyFingerprint = 0U;
	std::uint32_t scratchOffsetBytes = 0U;
	auto operator<=>(const EvaluationInstruction &) const = default;
};

struct EvaluationTransition {
	std::uint64_t sourceStateId = 0U;
	std::uint64_t destinationStateId = 0U;
	std::uint64_t conditionParameterId = 0U;
	AnimationFixedTick durationTicks = 0;
	AnimationFixedTick exitTimeTicks = 0;
	float destinationOffsetNormalized = 0.0F;
	std::uint32_t priority = 0U;
	std::uint64_t stableId = 0U;
	bool anyState = false;
	EvaluationTransitionInterruptionSource interruptionSource = EvaluationTransitionInterruptionSource::None;
	EvaluationTransitionInterruptionPolicy interruptionPolicy = EvaluationTransitionInterruptionPolicy::Reject;
	EvaluationTriggerConsumption triggerConsumption = EvaluationTriggerConsumption::SingleConsumer;
	bool triggerResetAtEndStep = true;
	bool resetTriggerOnFailure = false;
	auto operator<=>(const EvaluationTransition &) const = default;
};

struct EvaluationSyncMarker {
	std::uint64_t groupStableId = 0U;
	std::uint64_t markerStableId = 0U;
	std::uint64_t clipEventId = 0U;
	std::uint64_t stateId = 0U;
	std::uint64_t nameStableHash = 0U;
	auto operator<=>(const EvaluationSyncMarker &) const = default;
};

struct EvaluationScratchPlan {
	std::uint32_t byteCount = 0U;
	std::uint32_t alignmentBytes = 16U;
	auto operator<=>(const EvaluationScratchPlan &) const = default;
};

struct EvaluationBlendSample {
	std::uint64_t stableId = 0U;
	std::uint64_t childNodeId = 0U;
	float positionX = 0.0F;
	float positionY = 0.0F;
	auto operator<=>(const EvaluationBlendSample &) const = default;
};
struct EvaluationBlendNode {
	std::uint64_t nodeStableId = 0U;
	EvaluationOpcode opcode = EvaluationOpcode::BlendTree1D;
	AnimationBlendOutOfDomainPolicy outOfDomainPolicy = AnimationBlendOutOfDomainPolicy::Clamp;
	AnimationCompatibilityFingerprint outputCompatibilityFingerprint = 0U;
	std::vector<EvaluationBlendSample> samples;
	auto operator<=>(const EvaluationBlendNode &) const = default;
};
struct EvaluationLayer {
	std::uint64_t stableId = 0U;
	std::uint64_t rootStateId = 0U;
	AnimationGuid maskAssetId{};
	AnimationGeneration maskGeneration = 0U;
	AnimationCompatibilityFingerprint maskFingerprint = 0U;
	bool additive = false;
	float defaultWeight = 1.0F;
	auto operator<=>(const EvaluationLayer &) const = default;
};
struct EvaluationSyncGroup {
	std::uint64_t stableId = 0U;
	std::uint64_t leaderStateId = 0U;
	auto operator<=>(const EvaluationSyncGroup &) const = default;
};

struct EvaluationProgram {
	AnimationGuid controllerAssetId{};
	AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
	std::uint64_t stableHash = 0U;
	EvaluationScratchPlan scratchPlan{};
	// One byte per instruction is the stable opcode stream consumed by CPU/GPU back ends.
	std::vector<std::uint8_t> bytecode;
	std::vector<EvaluationInstruction> instructions;
	std::vector<EvaluationState> states;
	std::vector<EvaluationTransition> transitions;
	std::vector<EvaluationSyncMarker> syncMarkers;
	std::vector<EvaluationBlendNode> blendNodes;
	std::vector<EvaluationLayer> layers;
	std::vector<EvaluationSyncGroup> syncGroups;
	auto operator<=>(const EvaluationProgram &) const = default;
};

// Later evaluators replace the opcode cases. This shell proves that published
// programs are bounded and immutable before runtime work is dispatched.
struct EvaluationDispatchState {
	std::uint32_t dispatchedInstructionCount = 0U;
};

TERRAENGINE_API std::uint64_t ComputeEvaluationProgramStableHash(const EvaluationProgram &program) noexcept;
TERRAENGINE_API AnimationStatus DispatchEvaluationProgramCpu(const EvaluationProgram &program, EvaluationDispatchState &output) noexcept;
} // namespace TerraEngine::Animation
