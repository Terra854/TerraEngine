// AnimationCpuDeterminismTests.cpp - Fixed-tick repeatability and worker parity.

#include "TestHarness.h"

#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Evaluation/CpuDeterministicEvaluator.h"
#include "Animation/Runtime/AnimationEventStream.h"
#include "Animation/Runtime/AnimatorPlayback.h"
#include "Animation/Runtime/RootMotion.h"
#include "AnimationClipCompiler.h"

#include <array>
#include <bit>
#include <cstdint>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		using namespace TerraEngine::AssetPipeline;

		constexpr std::uint64_t kDeterministicJoint = 44U;
		constexpr std::uint64_t kDeterministicRoot = 1U;

		AnimationGuid MakeDeterministicGuid(std::uint8_t seed)
		{
			AnimationGuid guid;
			for(std::size_t index = 0U; index < guid.value.size(); ++index) {
				guid.value[index] = static_cast<std::uint8_t>(seed + index);
			}
			return guid;
		}

		AnimationSourceKey VectorKey(AnimationFixedTick tick, float x)
		{
			AnimationSourceKey key;
			key.tick = tick;
			key.vectorValue = {x, 0.0F, 0.0F};
			return key;
		}

		AnimationSourceKey ScalarKey(AnimationFixedTick tick, float value)
		{
			AnimationSourceKey key;
			key.tick = tick;
			key.scalarValue = value;
			return key;
		}

		AnimationClipAsset MakeDeterministicClip(TestContext& context)
		{
			AnimationClipCompileInput input;
			input.assetId = MakeDeterministicGuid(10U);
			input.skeletonAssetId = MakeDeterministicGuid(40U);
			input.compatibilityFingerprint = 0x1122334455667788ULL;
			input.durationTicks = 8;
			input.sampleRate = 60U;
			input.segmentDurationTicks = 4;
			AnimationSourceTrack translation;
			translation.stableTargetId = kDeterministicJoint;
			translation.kind = AnimationClipTrackKind::Translation;
			translation.interpolation = AnimationInterpolation::Linear;
			translation.keys = {VectorKey(0, 0.0F), VectorKey(8, 8.0F)};
			AnimationSourceTrack rootTranslation = translation;
			rootTranslation.stableTargetId = kDeterministicRoot;
			rootTranslation.kind = AnimationClipTrackKind::RootTranslation;
			AnimationSourceTrack rootYaw;
			rootYaw.stableTargetId = kDeterministicRoot;
			rootYaw.kind = AnimationClipTrackKind::RootYaw;
			rootYaw.interpolation = AnimationInterpolation::Linear;
			rootYaw.keys = {ScalarKey(0, 0.0F), ScalarKey(8, 0.8F)};
			input.tracks = {translation, rootTranslation, rootYaw};
			input.events = {{70U, 3, AnimationEventPayloadType::Integer, "70", true, true}};

			AnimationClipAsset clip;
			AnimationClipCompileReport report;
			context.Expect(CompileAnimationClip(input, {}, clip, report) == AnimationStatus::Ok,
				"The deterministic fixture must compile once before the repeated runtime checks.");
			return clip;
		}

		GraphPoseContribution MakeAdvancedPose(float translation)
		{
			// The advanced evaluator receives complete, copied poses just like a
			// scheduler worker would; no test object or renderer state is retained.
			GraphPoseContribution pose;
			pose.compatibilityFingerprint = 0xabcU;
			pose.jointCount = 1U;
			pose.jointStableIds = {91U};
			AnimationTransform joint;
			joint.translation.x = translation;
			pose.joints = {joint};
			pose.morphs = {{7U, translation}};
			return pose;
		}

		ProceduralGenerationStamp MakeAdvancedGeneration()
		{
			ProceduralGenerationStamp generation;
			generation.animator = {8U, 1U};
			generation.componentGeneration = 1U;
			generation.skeletonGeneration = 1U;
			generation.programGeneration = 1U;
			generation.poseGeneration = 1U;
			generation.publicationGeneration = 1U;
			generation.tick = 12;
			return generation;
		}

		MotionDatabaseAsset MakeAdvancedMotionDatabase()
		{
			MotionDatabaseAsset database;
			database.assetId = MakeDeterministicGuid(90U);
			database.runtimeGeneration = 1U;
			database.compatibilityFingerprint = 0xabcU;
			database.sourceFingerprint = 0xdefU;
			database.quantizationScales.fill(1.0F);
			MotionDatabaseSample sample;
			sample.stableSampleId = 200U;
			sample.sourceClipId = 300U;
			database.samples = {sample};
			database.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(database);
			return database;
		}

		std::uint64_t Mix(std::uint64_t hash, std::uint64_t value) noexcept
		{
			// FNV-1a over fixed-width semantic values is sufficient here: equality,
			// rather than cryptographic collision resistance, is the regression goal.
			hash ^= value;
			return hash * 1099511628211ULL;
		}

		std::uint64_t FloatBits(float value) noexcept
		{
			return static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value));
		}

		struct DeterministicResult {
			AnimationStatus status = AnimationStatus::InternalError;
			std::uint64_t hash = 0U;
		};

		DeterministicResult EvaluateDeterministicRecord(
			const AnimationClipAsset& clip,
			const AnimationClipView& view,
			std::size_t recordIndex) noexcept
		{
			const AnimationFixedTick startTick = static_cast<AnimationFixedTick>(recordIndex % 8U);
			const AnimationFixedTick deltaTicks = static_cast<AnimationFixedTick>((recordIndex % 17U) + 1U);
			AnimatorPlayback playback;
			AnimatorPlaybackConfiguration configuration;
			configuration.durationTicks = clip.durationTicks;
			configuration.loop = true;
			configuration.timePolicy = AnimatorTimePolicy::Fixed;
			if(playback.Configure(configuration) != AnimationStatus::Ok
				|| playback.Seek(startTick) != AnimationStatus::Ok
				|| playback.Play() != AnimationStatus::Ok) return {};
			AnimatorPlaybackAdvanceResult advance;
			if(playback.Advance({0.0, 0.0, deltaTicks}, advance) != AnimationStatus::Ok) return {};

			CpuClipEvaluator evaluator;
			const std::array<std::uint64_t, 1U> joints{kDeterministicJoint};
			CpuClipEvaluationRequest request;
			request.tick = playback.AbsoluteTick();
			request.loop = true;
			request.jointStableIds = joints;
			CpuClipEvaluationResult pose;
			if(evaluator.Evaluate(view, request, pose) != AnimationStatus::Ok
				|| pose.joints.size() != 1U) return {};

			AnimationEventStream eventStream;
			const AnimationAssetHandle clipHandle{clip.assetId, 1U};
			if(eventStream.Configure(clipHandle, clip.durationTicks, clip.events) != AnimationStatus::Ok) return {};
			AnimationEventTraversalRequest traversal;
			traversal.animator = {static_cast<std::uint32_t>(recordIndex + 1U), 1U};
			traversal.stateInstanceId = 1U;
			traversal.sourceStateId = 1U;
			traversal.previousAbsoluteTick = advance.previousAbsoluteTick;
			traversal.currentAbsoluteTick = advance.currentAbsoluteTick;
			traversal.producerOrderBase = 1U;
			AnimationEventTraversalResult events;
			const AnimationStatus eventStatus = eventStream.Traverse(traversal, events);
			if(eventStatus != AnimationStatus::Ok) return {};

			AnimationResolvedClipTime beginTime;
			AnimationResolvedClipTime endTime;
			CpuClipRootMotionPoint beginPoint;
			CpuClipRootMotionPoint endPoint;
			if(view.ResolveTime(advance.previousAbsoluteTick, true, beginTime) != AnimationStatus::Ok
				|| view.ResolveTime(advance.currentAbsoluteTick, true, endTime) != AnimationStatus::Ok
				|| evaluator.SampleRootMotionPoint(view, beginTime, beginPoint) != AnimationStatus::Ok
				|| evaluator.SampleRootMotionPoint(view, endTime, endPoint) != AnimationStatus::Ok) return {};
			AnimationRootMotionExtractionRequest extraction;
			extraction.animator = traversal.animator;
			extraction.sourceBeginTick = advance.previousAbsoluteTick;
			extraction.sourceEndTick = advance.currentAbsoluteTick;
			extraction.begin = {beginPoint.translation, beginPoint.yawRadians, beginTime.loopIndex};
			extraction.end = {endPoint.translation, endPoint.yawRadians, endTime.loopIndex};
			extraction.accumulatedCycleTranslation = clip.rootMotion.accumulatedCycleTranslation;
			extraction.accumulatedCycleYawRadians = clip.rootMotion.accumulatedCycleYawRadians;
			extraction.sourceStateId = 1U;
			extraction.generation = 1U;
		AnimationRootMotionDelta rootMotion;
			if(ExtractRootMotion(extraction, rootMotion) != AnimationStatus::Ok) return {};

			std::uint64_t hash = 1469598103934665603ULL;
			hash = Mix(hash, static_cast<std::uint64_t>(advance.currentAbsoluteTick));
			hash = Mix(hash, static_cast<std::uint64_t>(pose.time.resolvedTick));
			hash = Mix(hash, static_cast<std::uint64_t>(pose.time.loopIndex));
			hash = Mix(hash, FloatBits(pose.joints[0].localTransform.translation.x));
			for(const AnimationEventRecord& event : events.events) {
				hash = Mix(hash, event.identity.eventId);
				hash = Mix(hash, static_cast<std::uint64_t>(event.identity.sourceTick));
				hash = Mix(hash, static_cast<std::uint64_t>(event.identity.loopCount));
			}
			hash = Mix(hash, FloatBits(rootMotion.localTranslation.x));
			hash = Mix(hash, FloatBits(rootMotion.localYawRadians));
			hash = Mix(hash, static_cast<std::uint64_t>(rootMotion.accumulatedCycles));
			return {AnimationStatus::Ok, hash};
		}

		std::vector<DeterministicResult> EvaluateWithWorkerCount(
			const AnimationClipAsset& clip,
			const AnimationClipView& view,
			std::size_t recordCount,
			std::size_t workerCount)
		{
			std::vector<DeterministicResult> output(recordCount);
			std::vector<std::thread> workers;
			workers.reserve(workerCount);
			for(std::size_t worker = 0U; worker < workerCount; ++worker) {
				const std::size_t begin = (recordCount * worker) / workerCount;
				const std::size_t end = (recordCount * (worker + 1U)) / workerCount;
				workers.emplace_back([&clip, &view, &output, begin, end]() {
					// Every worker owns a disjoint output range and reads only the same
					// immutable clip view, mirroring AnimationSystem's scheduler packets.
					for(std::size_t index = begin; index < end; ++index) {
						output[index] = EvaluateDeterministicRecord(clip, view, index);
					}
				});
			}
			for(std::thread& worker : workers) worker.join();
			return output;
		}
	}

	void RegisterAnimationCpuDeterminismTests(TestRegistry& registry)
	{
		registry.Add("Animation.CpuDeterminism.FixedPoseEventRootMotionRepeats10000Runs", [](TestContext& context) {
			const AnimationClipAsset clip = MakeDeterministicClip(context);
			AnimationClipView view;
			context.Expect(view.Open(clip) == AnimationStatus::Ok,
				"The immutable deterministic clip view must be valid before repetition.");
			const DeterministicResult expected = EvaluateDeterministicRecord(clip, view, 4U);
			context.Expect(expected.status == AnimationStatus::Ok,
				"The reference fixed-tick pose/event/root-motion record must evaluate successfully.");
			for(std::size_t run = 0U; run < 10000U; ++run) {
				const DeterministicResult actual = EvaluateDeterministicRecord(clip, view, 4U);
				context.Expect(actual.status == AnimationStatus::Ok && actual.hash == expected.hash,
					"Every one of 10,000 independent fixed-tick evaluations must be bit-identical.");
			}
		});

		registry.Add("Animation.CpuDeterminism.WorkerCountsProduceIdenticalDisjointOutputs", [](TestContext& context) {
			const AnimationClipAsset clip = MakeDeterministicClip(context);
			AnimationClipView view;
			view.Open(clip);
			const std::vector<DeterministicResult> reference =
				EvaluateWithWorkerCount(clip, view, 257U, 1U);
			for(const std::size_t workerCount : std::array<std::size_t, 3U>{4U, 16U, 31U}) {
				const std::vector<DeterministicResult> candidate =
					EvaluateWithWorkerCount(clip, view, 257U, workerCount);
				context.Expect(candidate.size() == reference.size(),
					"Every worker matrix lane must cover the complete deterministic record range.");
				for(std::size_t index = 0U; index < reference.size(); ++index) {
					context.Expect(reference[index].status == AnimationStatus::Ok
						&& candidate[index].status == AnimationStatus::Ok
						&& candidate[index].hash == reference[index].hash,
						"Worker partition count must not change pose, event, root-motion, or state output.");
				}
			}
		});

		registry.Add("Animation.CpuDeterminism.AdvancedStagesAreCpuOnlyCanonicalAndFallbackSafely", [](TestContext& context) {
			const GraphPoseContribution input = MakeAdvancedPose(0.11F);
			const GraphPoseContribution selected = MakeAdvancedPose(1.25F);
			const MotionDatabaseAsset database = MakeAdvancedMotionDatabase();
			MotionMatchQuery query;
			query.database = {database.assetId, database.runtimeGeneration};
			query.expectedSchemaFingerprint = database.featureSchemaFingerprint;
			query.featureWeights.fill(1U);
			MotionMatchGpuReadback misleadingGpu;
			misleadingGpu.generation = {{database.assetId, database.runtimeGeneration},
				database.featureSchemaFingerprint, 1U};
			misleadingGpu.status = AnimationStatus::Ok;
			misleadingGpu.candidates = {{999U, 999U, 0, {}}};
			MotionMatchGraphNodeRequest motion;
			motion.database = &database;
			motion.query = &query;
			motion.gpuReadback = &misleadingGpu;
			motion.resolvedPose = {{200U, 300U, 0, {}}, &selected};
			motion.transitionWeight = 1.0F;
			motion.evaluationTick = 12;

			ProceduralCopyTransformNodeInput copy;
			copy.jointIndex = 0U;
			copy.targetTransform.translation.x = 2.0F;
			copy.goalId = 400U;
			ProceduralGraphNodeInput proceduralNode;
			proceduralNode.stableId = 400U;
			proceduralNode.payload = copy;
			const std::array proceduralNodes{proceduralNode};
			const ProceduralGenerationStamp proceduralGeneration = MakeAdvancedGeneration();
			ProceduralGraphEvaluationRequest procedural;
			procedural.generation = proceduralGeneration;
			procedural.expectedGeneration = proceduralGeneration;
			procedural.nodes = proceduralNodes;
			// The deterministic evaluator must override this visual request before
			// calling the procedural node, because rollback is authoritative CPU work.
			procedural.authority = ProceduralGraphAuthority::VisualOnly;

			AnimationTransform physicsTransform;
			physicsTransform.translation.x = 4.0F;
			const RagdollGenerationStamp ragdollGeneration{1U, 2U, 3U, 4U, 5};
			const PhysicsPoseSnapshot physics{ragdollGeneration,
				{{10U, 91U, physicsTransform, {}, {}, false}}, true};
			CpuDeterministicRagdollRequest ragdoll;
			ragdoll.physicsPose = &physics;
			ragdoll.expectedGeneration = ragdollGeneration;
			ragdoll.jointInputs = {{91U, 1.0F, false, RagdollBoundaryPolicy::MatchAnimation}};
			ragdoll.timeWeight = 0.5F;

			CpuDeterministicEvaluationRequest request;
			request.inputPose = &input;
			request.motionMatch = &motion;
			request.procedural = &procedural;
			request.ragdoll = &ragdoll;
			CpuDeterministicEvaluationResult result;
			context.Expect(EvaluateCpuDeterministicAdvancedPose(request, result) == AnimationStatus::Ok
				&& result.status == AnimationStatus::Ok && !result.usedGpuResult
				&& result.evaluatedStages == std::vector<CpuDeterministicAdvancedStage>{
					CpuDeterministicAdvancedStage::MotionMatch,
					CpuDeterministicAdvancedStage::Procedural,
					CpuDeterministicAdvancedStage::Ragdoll}
				&& result.pose.joints.front().translation.x == 3.0F,
				"Motion selection, procedural work, and ragdoll blending must use the canonical CPU-only order and a fixed-point result.");

			const RagdollGenerationStamp staleGeneration{1U, 2U, 3U, 4U, 6};
			ragdoll.expectedGeneration = staleGeneration;
			const GraphPoseContribution fallback = MakeAdvancedPose(-1.0F);
			request.motionMatch = nullptr;
			request.procedural = nullptr;
			request.fallbackPose = &fallback;
			context.Expect(EvaluateCpuDeterministicAdvancedPose(request, result) == AnimationStatus::Ok
				&& result.usedFallback && result.failureStatus == AnimationStatus::StaleHandle
				&& result.failedStage == CpuDeterministicAdvancedStage::Ragdoll
				&& result.pose == fallback,
				"A stale physics pose must publish the complete authored fallback with an explicit deterministic failure reason.");
		});
	}
}
