// AnimationGraphCompiler.h - Validation and publication boundary for controller graphs.

#pragma once

#include "Animation/Assets/AnimationControllerAsset.h"
#include "Animation/Graph/EvaluationProgram.h"

#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
enum class AnimationGraphDiagnosticCode : std::uint8_t {
	StateParentCycle = 0U,
	StateMachineDepthExceeded = 1U,
	SubgraphCycle = 2U,
	SubgraphDepthExceeded = 3U,
	MissingDefaultEntry = 4U,
	UnreachableState = 5U,
	AmbiguousTransition = 6U,
	InvalidParameter = 7U,
	InvalidMask = 8U,
	IncompatibleAsset = 9U,
	HardLimitExceeded = 10U,
	NonFiniteField = 11U,
	InvalidStructure = 12U
};

struct AnimationGraphDiagnostic {
	AnimationGraphDiagnosticCode code = AnimationGraphDiagnosticCode::InvalidStructure;
	std::uint64_t elementStableId = 0U;
	auto operator<=>(const AnimationGraphDiagnostic &) const = default;
};

struct AnimationGraphCompilationOptions {
	// Optional controller documents resolve indirect subgraph cycles at compile time.
	std::span<const AnimationControllerAsset> availableControllers{};
};

struct AnimationGraphCompilationResult {
	AnimationStatus status = AnimationStatus::Rejected;
	std::vector<AnimationGraphDiagnostic> diagnostics;
};

TERRAENGINE_API AnimationGraphCompilationResult CompileAnimationControllerGraph(const AnimationControllerAsset &controller,
																				const AnimationGraphCompilationOptions &options,
																				EvaluationProgram &output);
} // namespace TerraEngine::Animation
