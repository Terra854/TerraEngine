// EvaluationProgram.cpp - Stable hashing and bounded CPU dispatch shell.

#include "Animation/Graph/EvaluationProgram.h"

#include <bit>
#include <limits>
#include <type_traits>

namespace TerraEngine::Animation {
namespace {
constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
constexpr std::uint32_t kScratchBytesPerInstruction = 64U;
constexpr std::uint32_t kMaximumProgramScratchBytes = 64U * 1024U * 1024U;

// Serialize integral fields in little-endian order before hashing. This avoids
// letting host endianness, padding, or vector boundaries change a program hash.
template <typename T> void HashValue(std::uint64_t &hash, T value) noexcept {
	static_assert(std::is_integral_v<T>);
	using Unsigned = std::make_unsigned_t<T>;
	const Unsigned bits = static_cast<Unsigned>(value);
	for (std::size_t index = 0U; index < sizeof(Unsigned); ++index) {
		hash ^= static_cast<std::uint8_t>((bits >> (index * 8U)) & 0xffU);
		hash *= kFnvPrime;
	}
}

void HashGuid(std::uint64_t &hash, const AnimationGuid &value) noexcept {
	for (const std::uint8_t byte : value.value) {
		hash ^= byte;
		hash *= kFnvPrime;
	}
}
} // namespace

std::uint64_t ComputeEvaluationProgramStableHash(const EvaluationProgram &program) noexcept {
	std::uint64_t hash = kFnvOffset;
	HashGuid(hash, program.controllerAssetId);
	HashValue(hash, program.compatibilityFingerprint);
	HashValue(hash, program.scratchPlan.byteCount);
	HashValue(hash, program.scratchPlan.alignmentBytes);
	HashValue(hash, static_cast<std::uint64_t>(program.bytecode.size()));
	for (const std::uint8_t opcode : program.bytecode) {
		HashValue(hash, opcode);
	}
	HashValue(hash, static_cast<std::uint64_t>(program.instructions.size()));
	for (const EvaluationInstruction &instruction : program.instructions) {
		HashValue(hash, static_cast<std::uint8_t>(instruction.opcode));
		HashValue(hash, instruction.sourceStableId);
		HashGuid(hash, instruction.dependencyAssetId);
		HashValue(hash, instruction.dependencyGeneration);
		HashValue(hash, instruction.dependencyFingerprint);
		HashValue(hash, instruction.scratchOffsetBytes);
	}
	HashValue(hash, static_cast<std::uint64_t>(program.states.size()));
	for (const EvaluationState &state : program.states) {
		HashValue(hash, state.stableId);
		HashValue(hash, state.parentStateId);
		HashValue(hash, static_cast<std::uint8_t>(state.entry));
		HashValue(hash, static_cast<std::uint8_t>(state.exit));
		HashValue(hash, static_cast<std::uint8_t>(state.anyState));
		HashValue(hash, state.nodeId);
		HashValue(hash, static_cast<std::uint8_t>(state.nodeOpcode));
	}
	HashValue(hash, static_cast<std::uint64_t>(program.transitions.size()));
	for (const EvaluationTransition &transition : program.transitions) {
		HashValue(hash, transition.sourceStateId);
		HashValue(hash, transition.destinationStateId);
		HashValue(hash, transition.conditionParameterId);
		HashValue(hash, static_cast<std::uint64_t>(transition.durationTicks));
		HashValue(hash, static_cast<std::uint64_t>(transition.exitTimeTicks));
		HashValue(hash, std::bit_cast<std::uint32_t>(transition.destinationOffsetNormalized));
		HashValue(hash, transition.priority);
		HashValue(hash, transition.stableId);
		HashValue(hash, static_cast<std::uint8_t>(transition.anyState));
		HashValue(hash, static_cast<std::uint8_t>(transition.interruptionSource));
		HashValue(hash, static_cast<std::uint8_t>(transition.interruptionPolicy));
		HashValue(hash, static_cast<std::uint8_t>(transition.triggerConsumption));
		HashValue(hash, static_cast<std::uint8_t>(transition.triggerResetAtEndStep));
		HashValue(hash, static_cast<std::uint8_t>(transition.resetTriggerOnFailure));
	}
	HashValue(hash, static_cast<std::uint64_t>(program.syncMarkers.size()));
	for (const EvaluationSyncMarker &marker : program.syncMarkers) {
		HashValue(hash, marker.groupStableId);
		HashValue(hash, marker.markerStableId);
		HashValue(hash, marker.clipEventId);
		HashValue(hash, marker.stateId);
		HashValue(hash, marker.nameStableHash);
	}
	HashValue(hash, static_cast<std::uint64_t>(program.blendNodes.size()));
	for (const EvaluationBlendNode &node : program.blendNodes) {
		HashValue(hash, node.nodeStableId);
		HashValue(hash, static_cast<std::uint8_t>(node.opcode));
		HashValue(hash, static_cast<std::uint8_t>(node.outOfDomainPolicy));
		HashValue(hash, node.outputCompatibilityFingerprint);
		HashValue(hash, static_cast<std::uint64_t>(node.samples.size()));
		for (const EvaluationBlendSample &sample : node.samples) {
			HashValue(hash, sample.stableId); HashValue(hash, sample.childNodeId);
			HashValue(hash, std::bit_cast<std::uint32_t>(sample.positionX)); HashValue(hash, std::bit_cast<std::uint32_t>(sample.positionY));
		}
	}
	HashValue(hash, static_cast<std::uint64_t>(program.layers.size()));
	for (const EvaluationLayer &layer : program.layers) { HashValue(hash,layer.stableId);HashValue(hash,layer.rootStateId);HashGuid(hash,layer.maskAssetId);HashValue(hash,layer.maskGeneration);HashValue(hash,layer.maskFingerprint);HashValue(hash,static_cast<std::uint8_t>(layer.additive));HashValue(hash,std::bit_cast<std::uint32_t>(layer.defaultWeight)); }
	HashValue(hash, static_cast<std::uint64_t>(program.syncGroups.size()));
	for (const EvaluationSyncGroup &group : program.syncGroups) { HashValue(hash,group.stableId);HashValue(hash,group.leaderStateId); }
	return hash;
}

AnimationStatus DispatchEvaluationProgramCpu(const EvaluationProgram &program, EvaluationDispatchState &output) noexcept {
	if (!program.controllerAssetId.IsValid() || program.compatibilityFingerprint == 0U ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, program.instructions.size()) != AnimationStatus::Ok ||
		ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, program.blendNodes.size()) != AnimationStatus::Ok ||
		program.bytecode.size() != program.instructions.size() || program.scratchPlan.alignmentBytes == 0U ||
		(program.scratchPlan.alignmentBytes & (program.scratchPlan.alignmentBytes - 1U)) != 0U ||
		program.scratchPlan.byteCount % program.scratchPlan.alignmentBytes != 0U ||
		program.scratchPlan.byteCount != program.instructions.size() * kScratchBytesPerInstruction ||
		program.scratchPlan.byteCount > kMaximumProgramScratchBytes || program.stableHash != ComputeEvaluationProgramStableHash(program)) {
		return AnimationStatus::InvalidArgument;
	}

	// This intentionally performs no pose work yet. It is the checked dispatch
	// seam that later ImplementationCheck-ImplementationCheck evaluators fill without mutating the program.
	EvaluationDispatchState staged{};
	for (std::size_t index = 0U; index < program.instructions.size(); ++index) {
		const EvaluationInstruction &instruction = program.instructions[index];
		if (static_cast<std::uint8_t>(instruction.opcode) > static_cast<std::uint8_t>(EvaluationOpcode::ProceduralPose) ||
			program.bytecode[index] != static_cast<std::uint8_t>(instruction.opcode) ||
			instruction.scratchOffsetBytes != index * kScratchBytesPerInstruction ||
			instruction.scratchOffsetBytes % program.scratchPlan.alignmentBytes != 0U ||
			instruction.scratchOffsetBytes > program.scratchPlan.byteCount ||
			program.scratchPlan.byteCount - instruction.scratchOffsetBytes < kScratchBytesPerInstruction) {
			return AnimationStatus::InvalidArgument;
		}
		++staged.dispatchedInstructionCount;
	}
	for (const EvaluationBlendNode &blend : program.blendNodes) {
		if ((blend.opcode != EvaluationOpcode::BlendTree1D && blend.opcode != EvaluationOpcode::BlendTree2D) ||
			blend.outOfDomainPolicy > AnimationBlendOutOfDomainPolicy::Extrapolate ||
			blend.outputCompatibilityFingerprint != program.compatibilityFingerprint ||
			ValidateAnimationHardLimit(AnimationLimit::ControllerNodes, blend.samples.size()) != AnimationStatus::Ok)
			return AnimationStatus::InvalidArgument;
		for (const EvaluationBlendSample &sample : blend.samples)
			if (sample.stableId == 0U || sample.childNodeId == 0U || !std::isfinite(sample.positionX) || !std::isfinite(sample.positionY))
				return AnimationStatus::InvalidArgument;
	}
	for (const EvaluationLayer &layer : program.layers)
		if (layer.stableId == 0U || layer.rootStateId == 0U || !std::isfinite(layer.defaultWeight) || layer.defaultWeight < 0.0F || layer.defaultWeight > 1.0F)
			return AnimationStatus::InvalidArgument;
	output = staged;
	return AnimationStatus::Ok;
}
} // namespace TerraEngine::Animation
