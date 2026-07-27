// BlendTreeEvaluator.h - Deterministic 1D and 2D copied-pose blend helpers.

#pragma once

#include "Animation/Graph/GraphEvaluationTypes.h"
#include "Animation/Graph/EvaluationProgram.h"

#include <span>

namespace TerraEngine::Animation {
// A standalone sample owns its copied child result. This entry point is useful
// for CPU reference tests and for evaluators that have already resolved a node.
struct GraphBlendSample {
	std::uint64_t stableId = 0U;
	float positionX = 0.0F;
	float positionY = 0.0F;
	GraphPoseContribution contribution;
	auto operator<=>(const GraphBlendSample &) const = default;
};

// Compiled trees resolve leaf packets by stable node ID. The producer opcode
// proves that a leaf came from one of the compatible pose-producing node kinds;
// nested blend opcodes must instead be resolved through EvaluationProgram.
struct GraphNodeContribution {
	std::uint64_t nodeId = 0U;
	GraphPoseContribution contribution;
	EvaluationOpcode producerOpcode = EvaluationOpcode::SampleClip;
	auto operator<=>(const GraphNodeContribution &) const = default;
};

TERRAENGINE_API AnimationStatus EvaluateBlendTree1D(std::span<const GraphBlendSample> samples, float parameter,
	AnimationBlendOutOfDomainPolicy policy, GraphPoseContribution &output);
TERRAENGINE_API AnimationStatus EvaluateBlendTree2D(std::span<const GraphBlendSample> samples, float parameterX, float parameterY,
	AnimationBlendOutOfDomainPolicy policy, GraphPoseContribution &output);
// Resolve immutable childNodeIds through compiled blend tables. Leaf outputs
// are copied packets from clip, subgraph, procedural, or motion evaluators.
TERRAENGINE_API AnimationStatus EvaluateCompiledBlendNode(const EvaluationProgram &program, std::uint64_t nodeId,
	float parameterX, float parameterY, std::span<const GraphNodeContribution> leafOutputs, GraphPoseContribution &output);
} // namespace TerraEngine::Animation
