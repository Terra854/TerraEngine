// AnimationGraphEvaluationTests.cpp - Deterministic blend, layer, and sync evaluator contracts.

#include "TestHarness.h"

#include "Animation/Graph/BlendTreeEvaluator.h"
#include "Animation/Graph/LayerEvaluator.h"
#include "Animation/Graph/SyncGroupEvaluator.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace TerraEngine::Tests {
namespace {
using namespace TerraEngine::Animation;

bool Near(float left, float right, float tolerance = 0.0001F) {
	return std::abs(left - right) <= tolerance;
}

AnimationQuaternion RotationAroundZ(float radians, bool negate = false) {
	const float halfAngle = radians * 0.5F;
	AnimationQuaternion result{0.0F, 0.0F, std::sin(halfAngle), std::cos(halfAngle)};
	if (negate) {
		result.x = -result.x;
		result.y = -result.y;
		result.z = -result.z;
		result.w = -result.w;
	}
	return result;
}

GraphPoseContribution Pose(float value, bool includeRootMotion = true) {
	GraphPoseContribution result;
	result.compatibilityFingerprint = 7U;
	result.jointCount = 2U;
	result.jointStableIds = {11U, 22U};
	result.joints = {
		{{value, value * 2.0F, -value}, RotationAroundZ(value * 0.2F),
			{1.0F + value * 0.25F, 1.0F + value * 0.5F, 1.0F + value * 0.75F}},
		{{10.0F + value, value, value * 0.5F}, RotationAroundZ(value * -0.1F),
			{2.0F + value * 0.1F, 2.0F + value * 0.2F, 2.0F + value * 0.3F}}
	};
	result.morphs = {{101U, value}, {102U, 10.0F + value}};
	result.curves = {{201U, value * 2.0F}, {202U, 20.0F + value}};
	if (includeRootMotion) {
		AnimationRootMotionDelta rootMotion;
		rootMotion.animator = {3U, 4U};
		rootMotion.sourceBeginTick = 10;
		rootMotion.sourceEndTick = 20;
		rootMotion.localTranslation = {value, value * 2.0F, value * 3.0F};
		rootMotion.worldTranslation = {value * 4.0F, value * 5.0F, value * 6.0F};
		rootMotion.localYawRadians = value * 0.1F;
		rootMotion.worldYawRadians = value * 0.2F;
		rootMotion.accumulatedCycles = static_cast<std::int64_t>(value);
		rootMotion.sourceStateId = 100U + static_cast<std::uint64_t>(value);
		rootMotion.layerIndex = static_cast<std::uint32_t>(value);
		rootMotion.generation = 5U;
		rootMotion.policy = AnimatorRootMotionPolicy::ExtractOnly;
		result.rootMotion = rootMotion;
	}
	return result;
}

GraphBlendSample Sample(std::uint64_t stableId, float positionX, float positionY, float poseValue) {
	return {stableId, positionX, positionY, Pose(poseValue)};
}

AnimationMaskAsset Mask(
	std::vector<AnimationMaskDenseJointWeight> jointWeights,
	std::vector<std::uint64_t> morphIds,
	std::vector<std::uint64_t> curveIds,
	float rootMotionWeight) {
	AnimationMaskAsset result;
	result.assetId.value[0] = 1U;
	result.skeletonAssetId.value[0] = 2U;
	result.compatibilityFingerprint = 7U;
	result.compiledJointWeights = std::move(jointWeights);
	result.compiledMorphIds = std::move(morphIds);
	result.compiledCurveIds = std::move(curveIds);
	result.rootMotionWeight = rootMotionWeight;
	return result;
}
} // namespace

void RegisterAnimationGraphEvaluationTests(TestRegistry &registry) {
	registry.Add("Animation.GraphEvaluation.Blend1DComposesEveryChannelAndUsesExplicitDomainPolicy", [](TestContext &context) {
		GraphBlendSample low = Sample(10U, 0.0F, 0.0F, 0.0F);
		GraphBlendSample high = Sample(20U, 1.0F, 0.0F, 1.0F);
		high.contribution.joints[0].rotation = RotationAroundZ(0.2F, true);
		const GraphBlendSample samples[] = {high, low};
		GraphPoseContribution midpoint;
		GraphPoseContribution clamped;
		GraphPoseContribution extrapolated;

		const AnimationStatus midpointStatus = EvaluateBlendTree1D(
			samples, 0.5F, AnimationBlendOutOfDomainPolicy::Clamp, midpoint);
		const AnimationStatus clampStatus = EvaluateBlendTree1D(
			samples, -1.0F, AnimationBlendOutOfDomainPolicy::Clamp, clamped);
		const AnimationStatus extrapolateStatus = EvaluateBlendTree1D(
			samples, -1.0F, AnimationBlendOutOfDomainPolicy::Extrapolate, extrapolated);

		context.Expect(midpointStatus == AnimationStatus::Ok && clampStatus == AnimationStatus::Ok &&
			extrapolateStatus == AnimationStatus::Ok,
			"Valid 1D samples must evaluate under both declared out-of-domain policies.");
		context.Expect(Near(midpoint.joints[0].translation.x, 0.5F) && Near(midpoint.joints[0].scale.x, 1.125F) &&
			IsNormalized(midpoint.joints[0].rotation) && Near(midpoint.morphs[0].value, 0.5F) &&
			Near(midpoint.curves[0].value, 1.0F) && midpoint.rootMotion.has_value() &&
			Near(midpoint.rootMotion->localTranslation.z, 1.5F),
			"Translation, rotation, scale, morphs, curves, and numeric root motion must share the same stable weights.");
		context.Expect(Near(clamped.joints[0].translation.x, 0.0F) &&
			Near(extrapolated.joints[0].translation.x, -1.0F),
			"Clamp selects the authored boundary while extrapolate continues the endpoint segment.");
	});

	registry.Add("Animation.GraphEvaluation.BlendTreesRejectDuplicateSamplesAtomically", [](TestContext &context) {
		const GraphBlendSample duplicateIds[] = {
			Sample(10U, 0.0F, 0.0F, 0.0F), Sample(10U, 1.0F, 0.0F, 1.0F)};
		const GraphBlendSample duplicate1DPositions[] = {
			Sample(10U, 0.0F, 0.0F, 0.0F), Sample(20U, 0.0F, 1.0F, 1.0F)};
		const GraphBlendSample duplicate2DPositions[] = {
			Sample(10U, 0.0F, 0.0F, 0.0F), Sample(20U, 0.0F, 0.0F, 1.0F)};
		GraphPoseContribution output = Pose(9.0F);
		const GraphPoseContribution before = output;

		const AnimationStatus duplicateIdStatus = EvaluateBlendTree1D(
			duplicateIds, 0.5F, AnimationBlendOutOfDomainPolicy::Clamp, output);
		const AnimationStatus duplicate1DStatus = EvaluateBlendTree1D(
			duplicate1DPositions, 0.5F, AnimationBlendOutOfDomainPolicy::Clamp, output);
		const AnimationStatus duplicate2DStatus = EvaluateBlendTree2D(
			duplicate2DPositions, 0.5F, 0.5F, AnimationBlendOutOfDomainPolicy::Clamp, output);
		const GraphBlendSample overflowSamples[] = {
			Sample(10U, 0.0F, 0.0F, 0.0F), Sample(20U, 1.0F, 0.0F, 1.0F)};
		const AnimationStatus overflowStatus = EvaluateBlendTree1D(
			overflowSamples, std::numeric_limits<float>::max(), AnimationBlendOutOfDomainPolicy::Extrapolate, output);
		context.Expect(duplicateIdStatus == AnimationStatus::InvalidArgument &&
			duplicate1DStatus == AnimationStatus::InvalidArgument &&
			duplicate2DStatus == AnimationStatus::InvalidArgument && overflowStatus == AnimationStatus::RangeOverflow &&
			output == before,
			"Duplicate records and finite-input arithmetic overflow must preserve the caller's last good pose.");
	});

	registry.Add("Animation.GraphEvaluation.Blend2DUsesDeterministicCellsClampAndExtrapolation", [](TestContext &context) {
		std::vector<GraphBlendSample> forward{
			Sample(30U, 0.0F, 1.0F, 2.0F),
			Sample(10U, 0.0F, 0.0F, 0.0F),
			Sample(20U, 1.0F, 0.0F, 1.0F)};
		auto reverse = forward;
		std::reverse(reverse.begin(), reverse.end());
		GraphPoseContribution inside;
		GraphPoseContribution reordered;
		GraphPoseContribution clamped;
		GraphPoseContribution extrapolated;

		const AnimationStatus insideStatus = EvaluateBlendTree2D(
			forward, 0.25F, 0.25F, AnimationBlendOutOfDomainPolicy::Clamp, inside);
		const AnimationStatus reorderedStatus = EvaluateBlendTree2D(
			reverse, 0.25F, 0.25F, AnimationBlendOutOfDomainPolicy::Clamp, reordered);
		const AnimationStatus clampStatus = EvaluateBlendTree2D(
			forward, 2.0F, 2.0F, AnimationBlendOutOfDomainPolicy::Clamp, clamped);
		const AnimationStatus extrapolateStatus = EvaluateBlendTree2D(
			forward, 2.0F, 2.0F, AnimationBlendOutOfDomainPolicy::Extrapolate, extrapolated);

		context.Expect(insideStatus == AnimationStatus::Ok && reorderedStatus == AnimationStatus::Ok && inside == reordered &&
			Near(inside.joints[0].translation.x, 0.75F),
			"A stable-ID tie break must make a 2D cell independent of incoming sample order.");
		context.Expect(clampStatus == AnimationStatus::Ok && Near(clamped.joints[0].translation.x, 1.5F) &&
			extrapolateStatus == AnimationStatus::Ok && Near(extrapolated.joints[0].translation.x, 6.0F),
			"Clamp projects onto the triangle boundary while extrapolate preserves barycentric continuation.");
	});

	registry.Add("Animation.GraphEvaluation.Blend2DCollinearSamplesUseStableOneDimensionalFallback", [](TestContext &context) {
		const GraphBlendSample samples[] = {
			Sample(20U, 2.0F, 0.0F, 2.0F), Sample(10U, 0.0F, 0.0F, 0.0F)};
		GraphPoseContribution midpoint;
		GraphPoseContribution clamped;
		const AnimationStatus midpointStatus = EvaluateBlendTree2D(
			samples, 1.0F, 0.0F, AnimationBlendOutOfDomainPolicy::Clamp, midpoint);
		const AnimationStatus clampStatus = EvaluateBlendTree2D(
			samples, 3.0F, 0.0F, AnimationBlendOutOfDomainPolicy::Clamp, clamped);
		context.Expect(midpointStatus == AnimationStatus::Ok && Near(midpoint.joints[0].translation.x, 1.0F) &&
			clampStatus == AnimationStatus::Ok && Near(clamped.joints[0].translation.x, 2.0F),
			"Two-point and collinear 2D trees must remain defined without dividing by a zero triangle area.");
	});

	registry.Add("Animation.GraphEvaluation.CompiledNestedBlendTablesResolveEveryLeafKindAndRejectCyclesAtomically", [](TestContext &context) {
		EvaluationProgram program;
		program.controllerAssetId.value[0] = 1U;
		program.compatibilityFingerprint = 7U;
		program.blendNodes = {
			{10U, EvaluationOpcode::BlendTree1D, AnimationBlendOutOfDomainPolicy::Clamp, 7U,
				{{1U, 20U, 0.0F, 0.0F}, {2U, 30U, 1.0F, 0.0F}}},
			{20U, EvaluationOpcode::BlendTree1D, AnimationBlendOutOfDomainPolicy::Clamp, 7U,
				{{3U, 40U, 0.0F, 0.0F}, {4U, 50U, 1.0F, 0.0F}, {5U, 60U, 2.0F, 0.0F}}}
		};
		program.stableHash = ComputeEvaluationProgramStableHash(program);
		const GraphNodeContribution leaves[] = {
			{40U, Pose(0.0F), EvaluationOpcode::SampleClip},
			{30U, Pose(4.0F), EvaluationOpcode::CallSubgraph},
			{50U, Pose(2.0F), EvaluationOpcode::MotionMatch},
			{60U, Pose(6.0F), EvaluationOpcode::ProceduralPose}
		};
		const EvaluationProgram validProgram = program;
		GraphPoseContribution result;
		const AnimationStatus nested = EvaluateCompiledBlendNode(program, 10U, 0.5F, 0.0F, leaves, result);
		const GraphPoseContribution before = result;
		program.blendNodes[1].samples[0].childNodeId = 10U;
		program.stableHash = ComputeEvaluationProgramStableHash(program);
		const AnimationStatus cyclic = EvaluateCompiledBlendNode(program, 10U, 0.5F, 0.0F, leaves, result);
		context.Expect(nested == AnimationStatus::Ok && Near(before.joints[0].translation.x, 2.5F) &&
			cyclic == AnimationStatus::Rejected && result == before,
			"Nested tables accept clip/subgraph/motion/procedural pose packets and bounded cycle rejection is atomic.");

		const GraphNodeContribution missingLeaf[] = {leaves[1], leaves[2], leaves[3]};
		const AnimationStatus missingStatus =
			EvaluateCompiledBlendNode(validProgram, 10U, 0.5F, 0.0F, missingLeaf, result);
		auto invalidProducer = std::vector<GraphNodeContribution>(std::begin(leaves), std::end(leaves));
		invalidProducer[0].producerOpcode = EvaluationOpcode::BlendTree1D;
		const AnimationStatus producerStatus =
			EvaluateCompiledBlendNode(validProgram, 10U, 0.5F, 0.0F, invalidProducer, result);
		context.Expect(missingStatus == AnimationStatus::MissingAsset && producerStatus == AnimationStatus::InvalidArgument &&
			result == before,
			"Missing leaves and blend opcodes disguised as leaf packets must reject before changing the published result.");
	});

	registry.Add("Animation.GraphEvaluation.OverrideLayerCombinesControlsAndReusableMasksAcrossAllChannels", [](TestContext &context) {
		const GraphPoseContribution base = Pose(1.0F);
		const GraphPoseContribution source = Pose(5.0F);
		const AnimationMaskAsset mask = Mask({{11U, 0.5F}}, {101U}, {201U}, 0.25F);
		const GraphLayerInput layer{
			20U, AnimationGraphLayerMode::Override, 0.5F, 1.0F, 1.0F, &mask, &source, nullptr};
		GraphPoseContribution output;

		const AnimationStatus status = EvaluateAnimationLayers(base, {&layer, 1U}, output);
		context.Expect(status == AnimationStatus::Ok && Near(output.joints[0].translation.x, 2.0F) &&
			Near(output.joints[0].translation.y, 4.0F) && Near(output.joints[0].translation.z, -2.0F) &&
			Near(output.joints[0].scale.x, 1.5F) && Near(output.joints[0].scale.y, 2.0F) &&
			Near(output.joints[0].scale.z, 2.5F) && IsNormalized(output.joints[0].rotation) &&
			output.joints[0].rotation != base.joints[0].rotation,
			"Controller, node, layer, and dense joint-mask weights must affect every transform channel.");
		context.Expect(output.joints[1] == base.joints[1] && Near(output.morphs[0].value, 3.0F) &&
			Near(output.morphs[1].value, base.morphs[1].value) && Near(output.curves[0].value, 6.0F) &&
			Near(output.curves[1].value, base.curves[1].value),
			"Unselected joints, morphs, and curves must remain byte-for-byte unchanged.");
		context.Expect(output.rootMotion.has_value() && Near(output.rootMotion->localTranslation.x, 1.5F) &&
			Near(output.rootMotion->localTranslation.y, 3.0F) && Near(output.rootMotion->worldTranslation.x, 6.0F) &&
			Near(output.rootMotion->localYawRadians, 0.15F) && Near(output.rootMotion->worldYawRadians, 0.3F) &&
			output.rootMotion->sourceStateId == base.rootMotion->sourceStateId,
			"Root-motion numeric fields use the root mask while sub-majority discrete provenance remains stable.");
	});

	registry.Add("Animation.GraphEvaluation.AdditiveLayersUseReferenceDeltaQuaternionAndScaleRatio", [](TestContext &context) {
		const GraphPoseContribution base = Pose(2.0F);
		const GraphPoseContribution source = Pose(3.0F);
		const GraphPoseContribution reference = Pose(1.0F);
		const GraphLayerInput layer{
			10U, AnimationGraphLayerMode::Additive, 1.0F, 1.0F, 0.5F, nullptr, &source, &reference};
		GraphPoseContribution output;

		const AnimationStatus status = EvaluateAnimationLayers(base, {&layer, 1U}, output);
		context.Expect(status == AnimationStatus::Ok && Near(output.joints[0].translation.x, 3.0F) &&
			Near(output.joints[0].translation.y, 6.0F) && Near(output.joints[0].translation.z, -3.0F) &&
			Near(output.joints[0].scale.x, 1.8F) && Near(output.joints[0].scale.y, 2.6666667F) &&
			Near(output.joints[0].scale.z, 3.5714285F) && IsNormalized(output.joints[0].rotation) &&
			output.joints[0].rotation != base.joints[0].rotation,
			"Additive transforms must use translation delta, shortest-path quaternion delta, and multiplicative scale ratio.");
		context.Expect(Near(output.morphs[0].value, 3.0F) && Near(output.curves[0].value, 6.0F) &&
			output.rootMotion.has_value() && Near(output.rootMotion->localTranslation.y, 6.0F) &&
			Near(output.rootMotion->worldTranslation.z, 18.0F) && Near(output.rootMotion->localYawRadians, 0.3F) &&
			Near(output.rootMotion->worldYawRadians, 0.6F),
			"Additive reference deltas must cover morphs, curves, and every numeric root-motion field.");
	});

	registry.Add("Animation.GraphEvaluation.LayerOrderIsAuthoredOrderNotStableIdOrder", [](TestContext &context) {
		const GraphPoseContribution base = Pose(1.0F);
		const GraphPoseContribution overridePose = Pose(5.0F);
		const GraphPoseContribution additivePose = Pose(3.0F);
		const GraphPoseContribution reference = Pose(1.0F);
		const GraphLayerInput authoredOrder[] = {
			{20U, AnimationGraphLayerMode::Override, 1.0F, 1.0F, 1.0F, nullptr, &overridePose, nullptr},
			{10U, AnimationGraphLayerMode::Additive, 1.0F, 1.0F, 0.5F, nullptr, &additivePose, &reference}
		};
		const GraphLayerInput reversed[] = {authoredOrder[1], authoredOrder[0]};
		GraphPoseContribution forwardOutput;
		GraphPoseContribution reverseOutput;

		const AnimationStatus forwardStatus = EvaluateAnimationLayers(base, authoredOrder, forwardOutput);
		const AnimationStatus reverseStatus = EvaluateAnimationLayers(base, reversed, reverseOutput);
		context.Expect(forwardStatus == AnimationStatus::Ok && reverseStatus == AnimationStatus::Ok &&
			Near(forwardOutput.joints[0].translation.x, 6.0F) && Near(reverseOutput.joints[0].translation.x, 5.0F),
			"Layer stable IDs identify records, but the supplied authored sequence controls composition order.");
	});

	registry.Add("Animation.GraphEvaluation.InvalidLayersAndAsymmetricRootMotionPreserveLastGoodOutput", [](TestContext &context) {
		const GraphPoseContribution base = Pose(1.0F);
		GraphPoseContribution source = Pose(3.0F);
		const GraphPoseContribution reference = Pose(1.0F);
		GraphPoseContribution output = Pose(9.0F);
		const GraphPoseContribution before = output;
		GraphLayerInput invalid{
			10U, AnimationGraphLayerMode::Additive, 1.0F, 1.0F, 1.0F, nullptr, &source, nullptr};
		const AnimationStatus missingReference = EvaluateAnimationLayers(base, {&invalid, 1U}, output);
		invalid.mode = static_cast<AnimationGraphLayerMode>(255U);
		invalid.additiveReference = &reference;
		const AnimationStatus invalidMode = EvaluateAnimationLayers(base, {&invalid, 1U}, output);
		invalid.mode = AnimationGraphLayerMode::Override;
		invalid.additiveReference = nullptr;
		source.rootMotion.reset();
		const AnimationStatus asymmetricRoot = EvaluateAnimationLayers(base, {&invalid, 1U}, output);
		GraphLayerInput valid{
			1U, AnimationGraphLayerMode::Override, 1.0F, 1.0F, 1.0F, nullptr, &reference, nullptr};
		std::vector<GraphLayerInput> tooManyLayers(33U, valid);
		for (std::size_t index = 0U; index < tooManyLayers.size(); ++index) {
			tooManyLayers[index].stableId = index + 1U;
		}
		const AnimationStatus hardLimitStatus = EvaluateAnimationLayers(base, tooManyLayers, output);

		context.Expect(missingReference == AnimationStatus::InvalidArgument &&
			invalidMode == AnimationStatus::InvalidArgument && asymmetricRoot == AnimationStatus::InvalidArgument &&
			hardLimitStatus == AnimationStatus::HardLimitExceeded &&
			output == before,
			"Missing references, unknown modes, asymmetric roots, and unbounded layer counts must reject atomically.");
	});

	registry.Add("Animation.GraphEvaluation.LayerCompatibilityAndArithmeticOverflowRejectAtomically", [](TestContext &context) {
		const GraphPoseContribution base = Pose(1.0F);
		GraphPoseContribution source = Pose(3.0F);
		GraphPoseContribution reference = Pose(1.0F);
		GraphPoseContribution output = Pose(9.0F);
		const GraphPoseContribution before = output;
		AnimationMaskAsset incompatibleMask = Mask({{11U, 1.0F}}, {101U}, {201U}, 1.0F);
		incompatibleMask.compatibilityFingerprint = 99U;
		GraphLayerInput layer{
			10U, AnimationGraphLayerMode::Override, 1.0F, 1.0F, 1.0F, &incompatibleMask, &source, nullptr};
		const AnimationStatus maskStatus = EvaluateAnimationLayers(base, {&layer, 1U}, output);

		layer.mask = nullptr;
		layer.mode = AnimationGraphLayerMode::Additive;
		layer.additiveReference = &reference;
		source.joints[0].scale.x = std::numeric_limits<float>::max();
		reference.joints[0].scale.x = 0.0000001F;
		const AnimationStatus overflowStatus = EvaluateAnimationLayers(base, {&layer, 1U}, output);
		context.Expect(maskStatus == AnimationStatus::IncompatibleAsset && overflowStatus == AnimationStatus::RangeOverflow &&
			output == before,
			"Incompatible reusable masks and finite-input arithmetic overflow must preserve the caller's last good packet.");
	});
	registry.Add("Animation.GraphEvaluation.SyncMapsPhaseWithinNontrivialNamedIntervals", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 71U;
		const EvaluationSyncGroup group{kGroup, 10U};
		const EvaluationSyncMarker members[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U}, {kGroup, 3U, 103U, 10U, 300U},
			{kGroup, 4U, 201U, 20U, 100U}, {kGroup, 5U, 202U, 20U, 200U}, {kGroup, 6U, 203U, 20U, 300U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 2}, {200U, 6}, {300U, 9}};
		const GraphSyncRuntimeMarker followerMarkers[] = {{100U, 1}, {200U, 9}, {300U, 10}};
		const GraphSyncRuntimeState states[] = {{10U, 4, 10, false, leaderMarkers}, {20U, 0, 12, false, followerMarkers}};
		std::vector<GraphSyncCorrection> corrections;

		const AnimationStatus status = EvaluateSyncGroup(group, members, states, corrections);
		context.Expect(status == AnimationStatus::Ok && corrections.size() == 1U && corrections[0].followerStateId == 20U &&
			corrections[0].correctedTick == 5 && !corrections[0].reducedFidelity,
			"Sync maps the leader's integer interval phase to the follower interval with matching named endpoints.");
	});
	registry.Add("Animation.GraphEvaluation.SyncMapsLoopSeamIntervals", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 72U;
		const EvaluationSyncGroup group{kGroup, 10U};
		const EvaluationSyncMarker members[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 200U}, {kGroup, 4U, 202U, 20U, 100U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 2}, {200U, 7}};
		const GraphSyncRuntimeMarker followerMarkers[] = {{200U, 3}, {100U, 8}};
		const GraphSyncRuntimeState states[] = {{10U, 1, 10, true, leaderMarkers}, {20U, 0, 12, true, followerMarkers}};
		std::vector<GraphSyncCorrection> corrections;

		const AnimationStatus status = EvaluateSyncGroup(group, members, states, corrections);
		context.Expect(status == AnimationStatus::Ok && corrections.size() == 1U && corrections[0].correctedTick == 7 &&
			!corrections[0].reducedFidelity,
			"Looping sync treats the last-to-first marker interval as a wrapped named-marker seam.");
	});
	registry.Add("Animation.GraphEvaluation.SyncSelectsDeclaredLeaderStateDeterministically", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 73U;
		const EvaluationSyncGroup group{kGroup, 10U};
		// Member and state order intentionally disagree with both the leader and
		// follower IDs. Compiled leaderStateId and stable IDs remain authoritative.
		const EvaluationSyncMarker members[] = {
			{kGroup, 6U, 302U, 30U, 200U}, {kGroup, 4U, 202U, 20U, 200U},
			{kGroup, 2U, 102U, 10U, 200U}, {kGroup, 5U, 301U, 30U, 100U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 1U, 101U, 10U, 100U}};
		const EvaluationSyncMarker reorderedMembers[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 4U, 202U, 20U, 200U},
			{kGroup, 5U, 301U, 30U, 100U}, {kGroup, 6U, 302U, 30U, 200U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 2}, {200U, 6}};
		const GraphSyncRuntimeMarker follower20Markers[] = {{100U, 0}, {200U, 8}};
		const GraphSyncRuntimeMarker follower30Markers[] = {{100U, 1}, {200U, 5}};
		const GraphSyncRuntimeState states[] = {
			{30U, 0, 8, false, follower30Markers}, {20U, 0, 10, false, follower20Markers}, {10U, 4, 10, false, leaderMarkers}};
		const GraphSyncRuntimeState reorderedStates[] = {
			{10U, 4, 10, false, leaderMarkers}, {20U, 0, 10, false, follower20Markers}, {30U, 0, 8, false, follower30Markers}};
		std::vector<GraphSyncCorrection> corrections;
		std::vector<GraphSyncCorrection> reorderedCorrections;

		const AnimationStatus status = EvaluateSyncGroup(group, members, states, corrections);
		const AnimationStatus reorderedStatus = EvaluateSyncGroup(group, reorderedMembers, reorderedStates, reorderedCorrections);
		context.Expect(status == AnimationStatus::Ok && reorderedStatus == AnimationStatus::Ok &&
			corrections == reorderedCorrections && corrections.size() == 2U && corrections[0].followerStateId == 20U &&
			corrections[0].correctedTick == 4 && corrections[1].followerStateId == 30U && corrections[1].correctedTick == 3,
			"Sync follows EvaluationSyncGroup::leaderStateId and emits followers in stable ID order regardless of input order.");
	});
	registry.Add("Animation.GraphEvaluation.SyncFallsBackForIncompleteMarkerIntervals", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 74U;
		const EvaluationSyncGroup group{kGroup, 10U};
		const EvaluationSyncMarker members[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 4U, 202U, 20U, 200U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 2}, {200U, 6}};
		const GraphSyncRuntimeMarker incompleteFollowerMarkers[] = {{100U, 1}};
		const GraphSyncRuntimeState states[] = {{10U, 5, 10, false, leaderMarkers}, {20U, 0, 8, false, incompleteFollowerMarkers}};
		std::vector<GraphSyncCorrection> corrections;

		const AnimationStatus status = EvaluateSyncGroup(group, members, states, corrections);
		context.Expect(status == AnimationStatus::Ok && corrections.size() == 1U && corrections[0].correctedTick == 4 &&
			corrections[0].reducedFidelity,
			"Sync uses duration-normalized timing and reports reduced fidelity when a follower lacks an adjacent interval.");
	});
	registry.Add("Animation.GraphEvaluation.SyncRejectsDuplicateAndInvalidRecordsAtomically", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 75U;
		const EvaluationSyncGroup group{kGroup, 10U};
		const EvaluationSyncMarker duplicateNames[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 4U, 202U, 20U, 100U}};
		const EvaluationSyncMarker validMembers[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 4U, 202U, 20U, 200U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 2}, {200U, 6}};
		const GraphSyncRuntimeMarker followerMarkers[] = {{100U, 1}, {200U, 7}};
		const GraphSyncRuntimeMarker undeclaredFollowerMarkers[] = {{100U, 1}, {999U, 7}};
		const GraphSyncRuntimeState validStates[] = {{10U, 4, 10, false, leaderMarkers}, {20U, 0, 8, false, followerMarkers}};
		const GraphSyncRuntimeState duplicateStates[] = {{10U, 4, 10, false, leaderMarkers}, {10U, 0, 8, false, followerMarkers}};
		const GraphSyncRuntimeState undeclaredStates[] = {
			{10U, 4, 10, false, leaderMarkers}, {20U, 0, 8, false, undeclaredFollowerMarkers}};
		std::vector<GraphSyncCorrection> corrections{{999U, 99, true}};
		const std::vector<GraphSyncCorrection> before = corrections;

		const AnimationStatus duplicateNameStatus = EvaluateSyncGroup(group, duplicateNames, validStates, corrections);
		const AnimationStatus duplicateStateStatus = EvaluateSyncGroup(group, validMembers, duplicateStates, corrections);
		const AnimationStatus undeclaredStatus = EvaluateSyncGroup(group, validMembers, undeclaredStates, corrections);
		context.Expect(duplicateNameStatus == AnimationStatus::InvalidArgument &&
			duplicateStateStatus == AnimationStatus::InvalidArgument && undeclaredStatus == AnimationStatus::InvalidArgument &&
			corrections == before,
			"Duplicate declarations, duplicate states, and undeclared runtime markers reject before changing caller output.");
	});
	registry.Add("Animation.GraphEvaluation.SyncReportsCheckedTickOverflowAtomically", [](TestContext &context) {
		constexpr std::uint64_t kGroup = 76U;
		constexpr AnimationFixedTick kMaximumTick = std::numeric_limits<AnimationFixedTick>::max();
		const EvaluationSyncGroup group{kGroup, 10U};
		const EvaluationSyncMarker members[] = {
			{kGroup, 1U, 101U, 10U, 100U}, {kGroup, 2U, 102U, 10U, 200U},
			{kGroup, 3U, 201U, 20U, 100U}, {kGroup, 4U, 202U, 20U, 200U}};
		const GraphSyncRuntimeMarker leaderMarkers[] = {{100U, 0}, {200U, kMaximumTick - 1}};
		const GraphSyncRuntimeMarker followerMarkers[] = {{100U, 0}, {200U, kMaximumTick - 1}};
		const GraphSyncRuntimeState states[] = {
			{10U, kMaximumTick / 2, kMaximumTick, false, leaderMarkers},
			{20U, 0, kMaximumTick, false, followerMarkers}};
		std::vector<GraphSyncCorrection> corrections{{999U, 99, true}};
		const std::vector<GraphSyncCorrection> before = corrections;

		const AnimationStatus status = EvaluateSyncGroup(group, members, states, corrections);
		context.Expect(status == AnimationStatus::RangeOverflow && corrections == before,
			"Fixed-tick multiplication reports overflow and preserves the previously published correction vector.");
	});
}
} // namespace TerraEngine::Tests
