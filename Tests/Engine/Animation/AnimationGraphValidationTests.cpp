// AnimationGraphValidationTests.cpp - Compiler diagnostics and immutable program contracts.

#include "TestHarness.h"

#include "Animation/Graph/AnimationGraphCompiler.h"
#include "Animation/Graph/EvaluationProgram.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace TerraEngine::Tests {
namespace {
using namespace TerraEngine::Animation;

AnimationGuid Guid(std::uint8_t seed) {
	AnimationGuid value;
	for (std::size_t index = 0U; index < value.value.size(); ++index)
		value.value[index] = static_cast<std::uint8_t>(seed + index);
	return value;
}

AnimationControllerAsset MakeValidController(std::uint8_t seed = 1U) {
	AnimationControllerAsset controller;
	controller.assetId = Guid(seed);
	controller.skeletonAssetId = Guid(50U);
	controller.compatibilityFingerprint = 0x4455U;
	controller.nodes = {{1U, AnimationControllerNodeKind::ProceduralPose, 0U, {}, 0U, 0U},
						{2U, AnimationControllerNodeKind::BlendTree1D, 0U, {}, 0U, 0U}};
	controller.states = {{10U, 1U, 0U, true, false, false}, {11U, 2U, 0U, false, false, false}};
	controller.transitions = {{20U, 10U, 11U, 0U, 1, 0, 0.0F, 1U}};
	controller.syncGroups = {{30U, "Locomotion", 10U, {{31U, "FootPlant", 77U, 10U}}}};
	return controller;
}

bool HasDiagnostic(const AnimationGraphCompilationResult &result, AnimationGraphDiagnosticCode code) {
	return std::any_of(result.diagnostics.begin(), result.diagnostics.end(),
					   [code](const AnimationGraphDiagnostic &diagnostic) { return diagnostic.code == code; });
}
} // namespace

void RegisterAnimationGraphValidationTests(TestRegistry &registry) {
	registry.Add("Animation.GraphCompiler.RejectsStructuralAndSemanticDiagnostics", [](TestContext &context) {
		EvaluationProgram prior{};
		prior.controllerAssetId = Guid(90U);
		const EvaluationProgram original = prior;
		AnimationControllerAsset invalid = MakeValidController();
		invalid.states[0].parentStateId = 10U;
		AnimationGraphCompilationResult result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(result.status == AnimationStatus::Rejected &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::StateParentCycle) && prior == original,
					   "A direct state-parent cycle must reject without replacing the caller's last-good program.");

		invalid = MakeValidController();
		invalid.states[0].parentStateId = 11U;
		invalid.states[1].parentStateId = 10U;
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::StateParentCycle),
					   "An indirect state-parent cycle must report a graph-element diagnostic.");

		invalid = MakeValidController();
		for (std::uint64_t index = 12U; index < 80U; ++index)
			invalid.states.push_back({index, 1U, index - 1U, false, false, false});
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::StateMachineDepthExceeded),
					   "Nested state-machine parent chains must stop at the frozen depth limit.");

		invalid = MakeValidController();
		invalid.states[0].entry = false;
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::MissingDefaultEntry),
					   "Missing entries remain readable authoring data but are rejected for program publication.");

		invalid = MakeValidController();
		invalid.transitions.clear();
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::UnreachableState),
					   "States disconnected from the selected entry must be diagnosed before dispatch.");

		invalid = MakeValidController();
		invalid.states.push_back({12U, 2U, 10U, false, false, false});
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::MissingDefaultEntry),
					   "Each nested state-machine scope needs its own default entry before publication.");
	});

	registry.Add("Animation.GraphCompiler.RejectsDependenciesTransitionsAndBadValues", [](TestContext &context) {
		EvaluationProgram prior{};
		AnimationControllerAsset invalid = MakeValidController();
		invalid.subgraphDependencies = {{40U, invalid.assetId, 1U, invalid.compatibilityFingerprint}};
		AnimationGraphCompilationResult result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::SubgraphCycle),
					   "Direct recursive subgraph references must be rejected.");

		AnimationControllerAsset other = MakeValidController(2U);
		invalid = MakeValidController(1U);
		invalid.subgraphDependencies = {{40U, other.assetId, 1U, invalid.compatibilityFingerprint}};
		other.subgraphDependencies = {{41U, invalid.assetId, 1U, other.compatibilityFingerprint}};
		const std::vector<AnimationControllerAsset> available{other};
		result = CompileAnimationControllerGraph(invalid, {available}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::SubgraphCycle),
					   "Indirect subgraph recursion must use the supplied immutable dependency documents.");

		std::vector<AnimationControllerAsset> deepControllers;
		for (std::uint8_t index = 2U; index < 36U; ++index)
			deepControllers.push_back(MakeValidController(index));
		invalid = MakeValidController(1U);
		invalid.subgraphDependencies = {{40U, deepControllers.front().assetId, 1U, invalid.compatibilityFingerprint}};
		for (std::size_t index = 0U; index + 1U < deepControllers.size(); ++index)
			deepControllers[index].subgraphDependencies = {
				{40U + index, deepControllers[index + 1U].assetId, 1U, deepControllers[index].compatibilityFingerprint}};
		result = CompileAnimationControllerGraph(invalid, {deepControllers}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::SubgraphDepthExceeded),
					   "Subgraph traversal must stop at the frozen recursion-depth limit.");

		invalid = MakeValidController();
		invalid.transitions.push_back({21U, 10U, 10U, 0U, 1, 0, 0.0F, 1U});
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::AmbiguousTransition),
					   "Equal-priority competing transitions must not depend on incidental container order.");

		invalid = MakeValidController();
		invalid.nodes[0] = {1U, AnimationControllerNodeKind::Clip, 0U, Guid(60U), 1U, 0x9999U};
		invalid.layers = {{70U, 10U, {}, 1U, invalid.compatibilityFingerprint, false, std::numeric_limits<float>::quiet_NaN()}};
		invalid.transitions[0].destinationOffsetNormalized = std::numeric_limits<float>::quiet_NaN();
		invalid.parameters = {{80U, "Speed", AnimationControllerParameterType::Float, true}};
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::IncompatibleAsset) &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::InvalidMask) &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::NonFiniteField) &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::InvalidParameter),
					   "Parameter, mask, dependency fingerprint, and non-finite fields each require explicit diagnostics.");

		invalid = MakeValidController();
		invalid.nodes.resize(static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerNodes).hardMaximum) + 1U);
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(result.status == AnimationStatus::HardLimitExceeded &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::HardLimitExceeded),
					   "Controller hard limits must reject before compiler graph work or program allocation.");

		invalid = MakeValidController();
		invalid.graphVariables.resize(static_cast<std::size_t>(GetAnimationRuntimeLimit(AnimationLimit::ControllerParameters).hardMaximum) +
									  1U);
		const EvaluationProgram unchanged = prior;
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(result.status == AnimationStatus::HardLimitExceeded && prior == unchanged,
					   "Auxiliary graph-variable limits must fail before allocation and preserve prior published output.");

		invalid = MakeValidController();
		invalid.graphVariables = {{80U, "Vector", AnimationControllerParameterType::Vector,
								   AnimationFloat3{1.0F, 2.0F, std::numeric_limits<float>::quiet_NaN()}}};
		invalid.reusableBindings = {{81U, 999U, 80U}};
		invalid.states[0].nodeId = 999U;
		invalid.syncGroups[0].markers[0].stateId = 999U;
		invalid.subgraphDependencies = {{82U, Guid(70U), 0U, 0U}};
		result = CompileAnimationControllerGraph(invalid, {}, prior);
		context.Expect(HasDiagnostic(result, AnimationGraphDiagnosticCode::InvalidParameter) &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::InvalidStructure) &&
						   HasDiagnostic(result, AnimationGraphDiagnosticCode::IncompatibleAsset),
					   "Variables, bindings, state nodes, sync markers, and subgraph generations must name responsible invalid elements.");
	});

	registry.Add("Animation.GraphCompiler.BuildsDeterministicImmutableProgram", [](TestContext &context) {
		AnimationControllerAsset forward = MakeValidController();
		AnimationControllerAsset reverse = forward;
		std::reverse(reverse.nodes.begin(), reverse.nodes.end());
		std::reverse(reverse.transitions.begin(), reverse.transitions.end());
		EvaluationProgram first;
		EvaluationProgram second;
		const AnimationGraphCompilationResult firstResult = CompileAnimationControllerGraph(forward, {}, first);
		const AnimationGraphCompilationResult secondResult = CompileAnimationControllerGraph(reverse, {}, second);
		EvaluationDispatchState dispatch{};
		EvaluationProgram mismatchedBytecode = first;
		mismatchedBytecode.bytecode.front() = static_cast<std::uint8_t>(EvaluationOpcode::SampleClip);
		mismatchedBytecode.stableHash = ComputeEvaluationProgramStableHash(mismatchedBytecode);
		EvaluationProgram invalidAlignment = first;
		invalidAlignment.scratchPlan.alignmentBytes = 3U;
		invalidAlignment.stableHash = ComputeEvaluationProgramStableHash(invalidAlignment);
		context.Expect(firstResult.status == AnimationStatus::Ok && secondResult.status == AnimationStatus::Ok && first == second &&
						   first.bytecode == second.bytecode && first.stableHash == ComputeEvaluationProgramStableHash(first) &&
						   first.scratchPlan.byteCount == first.instructions.size() * 64U &&
						   DispatchEvaluationProgramCpu(first, dispatch) == AnimationStatus::Ok &&
						   dispatch.dispatchedInstructionCount == first.instructions.size() &&
						   DispatchEvaluationProgramCpu(mismatchedBytecode, dispatch) == AnimationStatus::InvalidArgument &&
						   DispatchEvaluationProgramCpu(invalidAlignment, dispatch) == AnimationStatus::InvalidArgument,
					   "Stable IDs define instruction, transition, sync-marker, hash, and scratch ordering for immutable CPU dispatch.");
	});
}
} // namespace TerraEngine::Tests
