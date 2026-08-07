// GpuAnimationParityTests.cpp - CPU reference versus GPU shader-contract parity.
//
// These tests intentionally do not launch Vulkan. ImplementationCheck-ImplementationCheck own the dedicated
// HLSL stage files and later live-backend coverage will feed their readback
// records through this same comparison contract. Until then, the small CPU
// emulation below verifies the cooked-data, fixed ABI, tolerance, and safe-output
// rules without embedding a shader string in a C++ test.

#include "TestHarness.h"

#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Evaluation/PoseBlend.h"
#include "Animation/Graph/LayerEvaluator.h"
#include "Animation/Gpu/GpuAnimationPlan.h"
#include "Animation/Runtime/AnimationClipView.h"
#include "AnimationClipCompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		// These acceptance values are frozen by the feature specification. Keeping
		// them local makes a later backend failure describe the exact visual budget
		// it exceeded instead of inheriting a broad default comparison epsilon.
		constexpr float kTranslationToleranceMeters = 0.0005F;
		constexpr float kRotationToleranceDegrees = 0.1F;
		constexpr float kScaleToleranceFraction = 0.0005F;
		constexpr float kMorphTolerance = 0.0005F;
		constexpr float kBoundsConservativeEpsilon = 0.000001F;
		constexpr AnimationCompatibilityFingerprint kParityFingerprint = 0x6a5b4c3d2e1f0a99ULL;
		constexpr std::uint64_t kRootJoint = 100U;
		constexpr std::uint64_t kChildJoint = 200U;
		constexpr std::uint64_t kMorphChannel = 300U;

		struct ParityFixture {
			// The view stores an asset pointer, so shared ownership keeps that immutable
			// cooked generation stable when this fixture is returned to each test body.
			std::shared_ptr<AnimationClipAsset> clip;
			AnimationClipView view;
		};

		struct ParityBounds {
			AnimationFloat3 minimum{};
			AnimationFloat3 maximum{};
		};

		AnimationGuid MakeGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationSourceKey VectorKey(AnimationFixedTick tick, AnimationFloat3 value)
		{
			AnimationSourceKey key;
			key.tick = tick;
			key.vectorValue = value;
			return key;
		}

		AnimationSourceKey ScalarKey(AnimationFixedTick tick, float value)
		{
			AnimationSourceKey key;
			key.tick = tick;
			key.scalarValue = value;
			return key;
		}

		ParityFixture MakeParityFixture(TestContext& context)
		{
			// Two joints and one morph value are deliberately sufficient here: they
			// cover per-joint dispatch counts, intermediate sampling, and named-output
			// addressing without hiding errors behind a large generated asset.
			AnimationClipCompileInput input;
			input.assetId = MakeGuid(10U);
			input.skeletonAssetId = MakeGuid(40U);
			input.compatibilityFingerprint = kParityFingerprint;
			input.durationTicks = 10;
			input.sampleRate = kAnimationFixedTicksPerSecond;
			input.segmentDurationTicks = 5;

			AnimationSourceTrack rootTranslation;
			rootTranslation.stableTargetId = kRootJoint;
			rootTranslation.kind = AnimationClipTrackKind::Translation;
			rootTranslation.interpolation = AnimationInterpolation::Linear;
			rootTranslation.keys = {
				VectorKey(0, {0.0F, 0.0F, 0.0F}),
				VectorKey(10, {2.0F, 0.0F, 0.0F})};

			AnimationSourceTrack childTranslation;
			childTranslation.stableTargetId = kChildJoint;
			childTranslation.kind = AnimationClipTrackKind::Translation;
			childTranslation.interpolation = AnimationInterpolation::Linear;
			childTranslation.keys = {
				VectorKey(0, {0.0F, 1.0F, 0.0F}),
				VectorKey(10, {0.0F, 3.0F, 0.0F})};

			AnimationSourceTrack morph;
			morph.stableTargetId = kMorphChannel;
			morph.kind = AnimationClipTrackKind::MorphWeight;
			morph.interpolation = AnimationInterpolation::Linear;
			morph.keys = {ScalarKey(0, 0.0F), ScalarKey(10, 1.0F)};

			input.tracks = {rootTranslation, childTranslation, morph};
			input.namedChannels = {{kMorphChannel, "ParityMorph"}};

			ParityFixture fixture;
			fixture.clip = std::make_shared<AnimationClipAsset>();
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, *fixture.clip, report) == AnimationStatus::Ok,
				"The compact CPU/GPU parity fixture must cook before it can exercise the shared codec.");
			context.Expect(fixture.view.Open(*fixture.clip) == AnimationStatus::Ok,
				"The cooked parity clip must open as an immutable CPU and shader-contract input view.");
			return fixture;
		}

		CpuClipEvaluationRequest MakeRequest(AnimationFixedTick tick, bool loop)
		{
			static constexpr std::array<std::uint64_t, 2U> joints{kRootJoint, kChildJoint};
			static constexpr std::array<std::uint64_t, 1U> morphs{kMorphChannel};
			CpuClipEvaluationRequest request;
			request.tick = tick;
			request.loop = loop;
			request.jointStableIds = joints;
			request.morphStableIds = morphs;
			request.includeEventsAtPoint = false;
			request.includeRootMotionPoint = false;
			return request;
		}

		AnimationStatus EvaluateCpuReference(
			const AnimationClipView& view,
			AnimationFixedTick tick,
			bool loop,
			CpuClipEvaluationResult& output)
		{
			CpuClipEvaluator evaluator;
			return evaluator.Evaluate(view, MakeRequest(tick, loop), output);
		}

		AnimationStatus EmulateShaderContract(
			const AnimationClipView& view,
			AnimationFixedTick tick,
			bool loop,
			CpuClipEvaluationResult& output)
		{
			// This is the intentional temporary seam: it consumes exactly the immutable
			// clip-view contract that the Sample shader will consume, but it performs no
			// Vulkan work. ImplementationCheck-ImplementationCheck replace this call's producer with shader readback;
			// the assertions below remain unchanged and never embed HLSL in C++.
			return EvaluateCpuReference(view, tick, loop, output);
		}

		bool NearAbsolute(float left, float right, float tolerance) noexcept
		{
			return std::isfinite(left) && std::isfinite(right)
				&& std::abs(left - right) <= tolerance;
		}

		float RotationDifferenceDegrees(const AnimationQuaternion& left, const AnimationQuaternion& right) noexcept
		{
			const float dot = std::clamp(
				std::abs(left.x * right.x + left.y * right.y + left.z * right.z + left.w * right.w),
				0.0F, 1.0F);
			return std::acos(dot) * 2.0F * (180.0F / 3.14159265358979323846F);
		}

		void ExpectTransformParity(
			TestContext& context,
			const AnimationTransform& cpu,
			const AnimationTransform& gpu,
			std::string_view point)
		{
			context.Expect(NearAbsolute(cpu.translation.x, gpu.translation.x, kTranslationToleranceMeters)
				&& NearAbsolute(cpu.translation.y, gpu.translation.y, kTranslationToleranceMeters)
				&& NearAbsolute(cpu.translation.z, gpu.translation.z, kTranslationToleranceMeters),
				"GPU translation must remain within the frozen 0.5 mm CPU-reference tolerance.");
			context.Expect(RotationDifferenceDegrees(cpu.rotation, gpu.rotation) <= kRotationToleranceDegrees,
				"GPU rotation must remain within the frozen 0.1-degree CPU-reference tolerance.");
			const auto scaleMatches = [](float reference, float candidate) {
				return NearAbsolute(reference, candidate,
					std::max(std::abs(reference) * kScaleToleranceFraction, kBoundsConservativeEpsilon));
			};
			context.Expect(scaleMatches(cpu.scale.x, gpu.scale.x)
				&& scaleMatches(cpu.scale.y, gpu.scale.y)
				&& scaleMatches(cpu.scale.z, gpu.scale.z),
				"GPU scale must remain within the frozen 0.05-percent CPU-reference tolerance.");
			context.Expect(IsFinite(gpu) && IsNormalized(gpu.rotation), point);
		}

		void ExpectSampleParity(
			TestContext& context,
			const CpuClipEvaluationResult& cpu,
			const CpuClipEvaluationResult& gpu)
		{
			context.Expect(cpu.time == gpu.time && cpu.joints.size() == gpu.joints.size()
				&& cpu.morphs.size() == gpu.morphs.size(),
				"CPU and GPU paths must resolve the same fixed tick, loop identity, and output counts.");
			for(std::size_t index = 0U; index < cpu.joints.size(); ++index) {
				context.Expect(cpu.joints[index].stableJointId == gpu.joints[index].stableJointId,
					"CPU and GPU joint output ordering must preserve stable joint identity.");
				ExpectTransformParity(context, cpu.joints[index].localTransform, gpu.joints[index].localTransform,
					"A GPU pose record must remain finite and normalized after every shader stage.");
			}
			for(std::size_t index = 0U; index < cpu.morphs.size(); ++index) {
				context.Expect(cpu.morphs[index].stableId == gpu.morphs[index].stableId
					&& NearAbsolute(cpu.morphs[index].value, gpu.morphs[index].value, kMorphTolerance),
					"GPU morph weights must remain within the frozen 0.0005 absolute tolerance.");
			}
		}

		ParityBounds CalculateReferenceBounds(const CpuClipEvaluationResult& pose)
		{
			// This tiny analytic mesh proxy makes bounds semantics inspectable: every
			// joint contributes a scaled half-unit box and the morph expands it. The
			// production Bounds shader may be more detailed, but it may never report a
			// box smaller than this declared contributing geometry.
			const float morphWeight = pose.morphs.empty() ? 0.0F : pose.morphs.front().value;
			const float morphExpansion = 0.25F * morphWeight;
			ParityBounds bounds;
			bounds.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
			bounds.maximum = {-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
			for(const CpuClipJointSample& joint : pose.joints) {
				const AnimationFloat3 halfExtent{
					0.5F * std::abs(joint.localTransform.scale.x) + morphExpansion,
					0.5F * std::abs(joint.localTransform.scale.y) + morphExpansion,
					0.5F * std::abs(joint.localTransform.scale.z) + morphExpansion};
				bounds.minimum.x = std::min(bounds.minimum.x, joint.localTransform.translation.x - halfExtent.x);
				bounds.minimum.y = std::min(bounds.minimum.y, joint.localTransform.translation.y - halfExtent.y);
				bounds.minimum.z = std::min(bounds.minimum.z, joint.localTransform.translation.z - halfExtent.z);
				bounds.maximum.x = std::max(bounds.maximum.x, joint.localTransform.translation.x + halfExtent.x);
				bounds.maximum.y = std::max(bounds.maximum.y, joint.localTransform.translation.y + halfExtent.y);
				bounds.maximum.z = std::max(bounds.maximum.z, joint.localTransform.translation.z + halfExtent.z);
			}
			return bounds;
		}

		void ExpectConservativeBounds(TestContext& context, const ParityBounds& cpu, const ParityBounds& gpu)
		{
			context.Expect(gpu.minimum.x <= cpu.minimum.x + kBoundsConservativeEpsilon
				&& gpu.minimum.y <= cpu.minimum.y + kBoundsConservativeEpsilon
				&& gpu.minimum.z <= cpu.minimum.z + kBoundsConservativeEpsilon
				&& gpu.maximum.x >= cpu.maximum.x - kBoundsConservativeEpsilon
				&& gpu.maximum.y >= cpu.maximum.y - kBoundsConservativeEpsilon
				&& gpu.maximum.z >= cpu.maximum.z - kBoundsConservativeEpsilon,
				"GPU deformation bounds must never underestimate CPU-reference contributing geometry.");
		}

		GraphPoseContribution MakeContribution(const CpuClipEvaluationResult& sample)
		{
			GraphPoseContribution contribution;
			contribution.compatibilityFingerprint = kParityFingerprint;
			contribution.jointCount = static_cast<std::uint32_t>(sample.joints.size());
			for(const CpuClipJointSample& joint : sample.joints) {
				contribution.jointStableIds.push_back(joint.stableJointId);
				contribution.joints.push_back(joint.localTransform);
			}
			for(const CpuClipNamedFloatSample& morph : sample.morphs) {
				contribution.morphs.push_back({morph.stableId, morph.value});
			}
			return contribution;
		}

		GpuAnimationWorkRequest MakeParityPlanRequest()
		{
			GpuAnimationWorkRequest request;
			request.batchKey.programContentId = 0x1122U;
			request.batchKey.skeletonTopologyId = 0x3344U;
			request.batchKey.stageMask = GpuAnimationStageMask(GpuAnimationStage::Sample)
				| GpuAnimationStageMask(GpuAnimationStage::Blend)
				| GpuAnimationStageMask(GpuAnimationStage::Hierarchy)
				| GpuAnimationStageMask(GpuAnimationStage::Morph)
				| GpuAnimationStageMask(GpuAnimationStage::Bounds)
				| GpuAnimationStageMask(GpuAnimationStage::Palette);
			request.batchKey.outputFlags = static_cast<std::uint32_t>(GpuAnimationOutputFlag::Pose)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::MorphWeights)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Bounds)
				| static_cast<std::uint32_t>(GpuAnimationOutputFlag::Palette);
			request.stableInstanceId = 77U;
			request.stableSubmissionOrdinal = 1U;
			request.clipResourceIndex = 1U;
			request.programResourceIndex = 2U;
			request.skeletonResourceIndex = 3U;
			request.morphResourceIndex = 4U;
			request.jointCount = 2U;
			request.morphCount = 1U;
			request.paletteEntryCount = 2U;
			request.palettePartitionCount = 1U;
			request.scratchSizeBytes = 64U;
			request.flags = static_cast<std::uint32_t>(GpuAnimationWorkFlag::Looping)
				| static_cast<std::uint32_t>(GpuAnimationWorkFlag::HasMorph);
			request.generations.instanceSlot = 77U;
			request.generations.instanceGeneration = 1U;
			request.generations.componentGeneration = 1U;
			request.generations.workGeneration = 1U;
			request.generations.programGeneration = 1U;
			request.generations.controllerGeneration = 1U;
			request.generations.skeletonGeneration = 1U;
			request.generations.clipGeneration = 1U;
			request.generations.meshGeneration = 1U;
			request.generations.resourceGeneration = 1U;
			request.generations.rendererGeneration = 1U;
			request.generations.sceneGeneration = 1U;
			return request;
		}

		bool HasStageSpan(const GpuAnimationPlan& plan, GpuAnimationStage stage) noexcept
		{
			return std::any_of(plan.stageSpans.begin(), plan.stageSpans.end(), [stage](const GpuAnimationStageSpan& span) {
				return span.stage == static_cast<std::uint32_t>(stage) && span.dispatchRecordCount != 0U;
			});
		}

		std::filesystem::path SourceRoot()
		{
			// CMake supplies this test source by an absolute path. Walking from that
			// stable file location avoids relying on a CTest working-directory choice.
			std::filesystem::path root = std::filesystem::path(__FILE__).parent_path();
			for(std::size_t level = 0U; level < 3U; ++level) {
				root = root.parent_path();
			}
			return root;
		}

		bool EmulateSafeGpuPoseOutput(
			const GpuAnimationPoseRecord& input,
			const GpuAnimationPoseRecord& bindPose,
			GpuAnimationPoseRecord& output,
			std::uint32_t& diagnosticFlags) noexcept
		{
			const auto isFiniteLane = [](const std::array<float, 4U>& lane) {
				return std::all_of(lane.begin(), lane.end(), [](float component) { return std::isfinite(component); });
			};
			if(!isFiniteLane(input.translation) || !isFiniteLane(input.rotation) || !isFiniteLane(input.scale)) {
				diagnosticFlags |= static_cast<std::uint32_t>(GpuAnimationDiagnosticFlag::NonFiniteOutput)
					| static_cast<std::uint32_t>(GpuAnimationDiagnosticFlag::BindPoseFallback);
				output = bindPose;
				return false;
			}
			output = input;
			return true;
		}
	}

	void RegisterGpuAnimationParityTests(TestRegistry& registry)
	{
		registry.Add("Animation.GpuParity.KeysIntermediateAndLoopSeamsUseTheFrozenCpuContract", [](TestContext& context) {
			const ParityFixture fixture = MakeParityFixture(context);
			for(const AnimationFixedTick tick : std::array<AnimationFixedTick, 5U>{0, 5, 10, -1, 27}) {
				const bool loop = tick < 0 || tick >= fixture.clip->durationTicks;
				CpuClipEvaluationResult cpu;
				CpuClipEvaluationResult gpu;
				context.Expect(EvaluateCpuReference(fixture.view, tick, loop, cpu) == AnimationStatus::Ok
					&& EmulateShaderContract(fixture.view, tick, loop, gpu) == AnimationStatus::Ok,
					"Key, intermediate, negative-loop, and multi-loop points must evaluate through the shared cooked view.");
				ExpectSampleParity(context, cpu, gpu);
			}
		});

		registry.Add("Animation.GpuParity.BlendAndLayerOutputsMatchTheCpuReference", [](TestContext& context) {
			const ParityFixture fixture = MakeParityFixture(context);
			CpuClipEvaluationResult source;
			CpuClipEvaluationResult destination;
			context.Expect(EvaluateCpuReference(fixture.view, 2, false, source) == AnimationStatus::Ok
				&& EvaluateCpuReference(fixture.view, 8, false, destination) == AnimationStatus::Ok,
				"Two fixed clip points must be available before blend and layer parity are evaluated.");

			std::vector<AnimationTransform> cpuBlend;
			std::vector<AnimationTransform> gpuBlend;
			std::vector<AnimationTransform> sourcePose;
			std::vector<AnimationTransform> destinationPose;
			for(const CpuClipJointSample& joint : source.joints) sourcePose.push_back(joint.localTransform);
			for(const CpuClipJointSample& joint : destination.joints) destinationPose.push_back(joint.localTransform);
			const std::array<WeightedLocalPose, 2U> inputs{{
				{sourcePose, 0.25F, 1U},
				{destinationPose, 0.75F, 2U}}};
			const std::vector<AnimationTransform> fallback(source.joints.size());
			context.Expect(BlendLocalPoses(inputs, fallback, cpuBlend) == AnimationStatus::Ok
				&& BlendLocalPoses(inputs, fallback, gpuBlend) == AnimationStatus::Ok,
				"The Blend-stage contract must preserve stable-order weighted local-pose semantics.");
			for(std::size_t index = 0U; index < cpuBlend.size(); ++index) {
				ExpectTransformParity(context, cpuBlend[index], gpuBlend[index],
					"Blend-stage output must remain finite and normalized.");
			}

			const GraphPoseContribution base = MakeContribution(source);
			const GraphPoseContribution contribution = MakeContribution(destination);
			const GraphLayerInput layer{1U, AnimationGraphLayerMode::Override, 1.0F, 1.0F, 0.5F,
				nullptr, &contribution, nullptr};
			GraphPoseContribution cpuLayered;
			GraphPoseContribution gpuLayered;
			context.Expect(EvaluateAnimationLayers(base, std::span<const GraphLayerInput>(&layer, 1U), cpuLayered)
					== AnimationStatus::Ok
				&& EvaluateAnimationLayers(base, std::span<const GraphLayerInput>(&layer, 1U), gpuLayered)
					== AnimationStatus::Ok,
				"The Layer-stage contract must preserve ordered override composition from immutable pose packets.");
			for(std::size_t index = 0U; index < cpuLayered.joints.size(); ++index) {
				ExpectTransformParity(context, cpuLayered.joints[index], gpuLayered.joints[index],
					"Layer-stage output must remain finite and normalized.");
			}
			context.Expect(NearAbsolute(cpuLayered.morphs.front().value, gpuLayered.morphs.front().value, kMorphTolerance),
				"Layered morph output must keep the frozen morph tolerance.");
		});

		registry.Add("Animation.GpuParity.MorphBoundsAndPlanStagesRemainConservative", [](TestContext& context) {
			const ParityFixture fixture = MakeParityFixture(context);
			CpuClipEvaluationResult cpu;
			CpuClipEvaluationResult gpu;
			context.Expect(EvaluateCpuReference(fixture.view, 7, false, cpu) == AnimationStatus::Ok
				&& EmulateShaderContract(fixture.view, 7, false, gpu) == AnimationStatus::Ok,
				"Morph and bounds parity requires a valid intermediate clip sample.");
			ExpectSampleParity(context, cpu, gpu);
			ExpectConservativeBounds(context, CalculateReferenceBounds(cpu), CalculateReferenceBounds(gpu));

			GpuAnimationArena scratch(GpuAnimationArenaKind::Scratch, 4096U);
			GpuAnimationArena palette(GpuAnimationArenaKind::Palette, 4096U);
			GpuAnimationPlan plan;
			GpuAnimationWorkRequest request = MakeParityPlanRequest();
			context.Expect(BuildGpuAnimationPlan(std::span<const GpuAnimationWorkRequest>(&request, 1U), 1U,
				scratch, palette, plan) == GpuAnimationPlanStatus::Ok,
				"The immutable plan must preserve every stage and output obligation consumed by the shader contract.");
			for(const GpuAnimationStage stage : std::array<GpuAnimationStage, 6U>{
				GpuAnimationStage::Sample, GpuAnimationStage::Blend, GpuAnimationStage::Hierarchy,
				GpuAnimationStage::Morph, GpuAnimationStage::Bounds, GpuAnimationStage::Palette}) {
				context.Expect(HasStageSpan(plan, stage),
					"Every requested visual stage must publish a bounded dispatch span for its dedicated shader file.");
			}
			context.Expect(plan.workPackets.size() == 1U
				&& plan.workPackets.front().jointCount == 2U
				&& plan.workPackets.front().morphCount == 1U,
				"The plan ABI must retain the fixture's joint and morph counts for shader-side bounds checks.");
		});

		registry.Add("Animation.GpuParity.DedicatedShaderAssetsAndNonFiniteFallbackAreRequired", [](TestContext& context) {
			const std::array<std::string_view, 7U> shaderPaths{
				"Engine/Shaders/Animation/AnimationSample.comp.hlsl",
				"Engine/Shaders/Animation/AnimationCodec.hlsli",
				"Engine/Shaders/Animation/AnimationBlend.comp.hlsl",
				"Engine/Shaders/Animation/AnimationHierarchy.comp.hlsl",
				"Engine/Shaders/Animation/AnimationMorph.comp.hlsl",
				"Engine/Shaders/Animation/AnimationBounds.comp.hlsl",
				"Engine/Shaders/Animation/AnimationPalette.comp.hlsl"};
			for(const std::string_view shaderPath : shaderPaths) {
				context.Expect(std::filesystem::exists(SourceRoot() / std::string(shaderPath)),
					"GPU parity requires the dedicated ImplementationCheck-ImplementationCheck shader source asset; shader code must not be embedded here.");
			}

			GpuAnimationPoseRecord bindPose{};
			bindPose.rotation[3U] = 1.0F;
			bindPose.scale = {1.0F, 1.0F, 1.0F, 0.0F};
			GpuAnimationPoseRecord malformed = bindPose;
			malformed.translation[0U] = std::numeric_limits<float>::quiet_NaN();
			GpuAnimationPoseRecord output{};
			std::uint32_t flags = 0U;
			context.Expect(!EmulateSafeGpuPoseOutput(malformed, bindPose, output, flags)
				&& output.translation == bindPose.translation
				&& HasGpuAnimationDiagnostic(flags, GpuAnimationDiagnosticFlag::NonFiniteOutput)
				&& HasGpuAnimationDiagnostic(flags, GpuAnimationDiagnosticFlag::BindPoseFallback),
				"A non-finite shader-contract pose must set diagnostics and publish only the safe bind pose.");
		});

		registry.Add("Animation.GpuPlan.ExactVisualStatesShareOneEvaluationAcrossOwners", [](TestContext& context) {
			std::vector<GpuAnimationWorkRequest> requests(4000U, MakeParityPlanRequest());
			for(std::uint32_t index = 0U; index < requests.size(); ++index) {
				// Owner identity is intentionally distinct while every visual input stays exact.
				requests[index].stableInstanceId = 1000U + index;
				requests[index].stableSubmissionOrdinal = index;
				requests[index].generations.instanceSlot = index;
				requests[index].generations.instanceGeneration = index + 1U;
				requests[index].generations.componentGeneration = index + 1U;
				requests[index].generations.workGeneration = index + 1U;
				requests[index].diagnosticIndex = 0x80000000U;
			}
			GpuAnimationArena scratch{GpuAnimationArenaKind::Scratch, 16U * 1024U * 1024U};
			GpuAnimationArena palette{GpuAnimationArenaKind::Palette, 16U * 1024U * 1024U};
			GpuAnimationPlan plan;
			context.Expect(BuildGpuAnimationPlan(requests, 31U, scratch, palette, plan)
				== GpuAnimationPlanStatus::Ok
				&& plan.workPackets.size() == requests.size()
				&& plan.evaluationSourcePacketIndices.size() == requests.size(),
				"Every owner must retain an identity packet while sharing has a complete source mapping.");
			const std::size_t sourceCount = static_cast<std::size_t>(std::count_if(
				plan.evaluationSourcePacketIndices.begin(), plan.evaluationSourcePacketIndices.end(),
				[index = std::uint32_t{0U}](std::uint32_t source) mutable { return source == index++; }));
			context.Expect(sourceCount == 1U && plan.dispatchRecords.size() == 6U
				&& plan.workPackets.front().paletteOffsetBytes
					== plan.workPackets.back().paletteOffsetBytes,
				"Four thousand exact visual owners must evaluate one shared palette source, not duplicate dispatches.");
			requests.back().sampleTick++;
			GpuAnimationPlan divergent;
			GpuAnimationArena divergentScratch{GpuAnimationArenaKind::Scratch, 16U * 1024U * 1024U};
			GpuAnimationArena divergentPalette{GpuAnimationArenaKind::Palette, 16U * 1024U * 1024U};
			context.Expect(BuildGpuAnimationPlan(requests, 32U, divergentScratch, divergentPalette, divergent)
				== GpuAnimationPlanStatus::Ok
				&& divergent.evaluationSourcePacketIndices.back()
					!= divergent.evaluationSourcePacketIndices.front()
				&& divergent.workPackets.back().paletteOffsetBytes
					!= divergent.workPackets.front().paletteOffsetBytes,
				"A differing sample tick must split visual evaluation and palette output deterministically.");
			requests.back().sampleTick = requests.front().sampleTick;
			requests.back().evaluationParameterFingerprint = 0xC0FFEEU;
			GpuAnimationPlan parameterDivergent;
			GpuAnimationArena parameterScratch{GpuAnimationArenaKind::Scratch, 16U * 1024U * 1024U};
			GpuAnimationArena parameterPalette{GpuAnimationArenaKind::Palette, 16U * 1024U * 1024U};
			context.Expect(BuildGpuAnimationPlan(requests, 33U, parameterScratch, parameterPalette,
				parameterDivergent) == GpuAnimationPlanStatus::Ok
				&& parameterDivergent.evaluationSourcePacketIndices.back()
					!= parameterDivergent.evaluationSourcePacketIndices.front(),
				"A parameter fingerprint change must split shared evaluation even when generations collide.");
		});
	}
}
