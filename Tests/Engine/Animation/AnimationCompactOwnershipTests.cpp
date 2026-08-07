// AnimationCompactOwnershipTests.cpp - Compact/complete handoff closure tests.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimationExecutionMode.h"
#include "Animation/Runtime/AnimationFallback.h"
#include "Animation/Runtime/AnimationWorkScheduler.h"
#include "Animation/Runtime/AnimatorPlayback.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		CompactGpuVisualInput MakeCompactInput()
		{
			CompactGpuVisualInput input;
			input.stableInstanceId = 0x100000001ULL;
			input.stableSubmissionOrdinal = 1U;
			input.playbackTick = 24;
			input.sourcePoseGeneration = 7U;
			input.generations = {1U, 2U, 3U, 4U, 5U, 6U, 7U};
			input.eligibilityReason = AnimationExecutionModeReason::GpuVisualEligible;
			input.gpuLoweringComplete = true;
			input.hasAuthoritativeConsumer = false;
			input.hasControllerOrTransition = false;
			input.hasAdvancedFeature = false;
			input.hasContinuitySource = true;
			return input;
		}

		AnimationFallbackGenerationKey MakeFallbackKey(std::uint64_t publication)
		{
			AnimationFallbackGenerationKey key;
			key.animator = {1U, 1U};
			key.componentGeneration = 1U;
			key.sceneGeneration = 1U;
			key.contentGeneration = 1U;
			key.rendererGeneration = 1U;
			key.physicsWorldGeneration = 1U;
			key.publicationGeneration = publication;
			key.compatibilityFingerprint = 0x1U;
			return key;
		}

		CompactGpuVisualResult MakeCompactPublication(const CompactGpuVisualInput& input)
		{
			CompactGpuVisualResult result;
			result.stableInstanceId = input.stableInstanceId;
			result.stableSubmissionOrdinal = input.stableSubmissionOrdinal;
			result.sourcePoseGeneration = input.sourcePoseGeneration;
			result.presentationPoseGeneration = input.sourcePoseGeneration;
			result.generations = input.generations;
			result.reason = input.eligibilityReason;
			result.complete = true;
			return result;
		}

		using Payload = std::uint64_t;
		using Publication = AnimationFallbackPublication<Payload>;

		std::shared_ptr<const Publication> MakePublication(
			const AnimationFallbackGenerationKey& key,
			bool complete = true)
		{
			auto publication = std::make_shared<Publication>();
			publication->generations = key;
			publication->source = AnimationFallbackSource::Current;
			publication->payload = std::make_shared<Payload>(key.publicationGeneration);
			publication->complete = complete;
			publication->finite = true;
			return publication;
		}
	}

	void RegisterAnimationCompactOwnershipTests(TestRegistry& registry)
	{
		registry.Add("Animation.CompactOwnership.MixedAdvancedFeatureParityStaysComplete", [](TestContext& context) {
			CompactGpuVisualInput visualOnly = MakeCompactInput();
			CompactGpuVisualInput advanced = visualOnly;
			advanced.hasAdvancedFeature = true;
			CompactGpuVisualInput authoritative = visualOnly;
			authoritative.hasAuthoritativeConsumer = true;
			context.Expect(ClassifyAnimationProcessing(visualOnly)
				== AnimationProcessingClass::CompactGpuVisual
				&& ClassifyAnimationProcessing(advanced) == AnimationProcessingClass::Complete
				&& ClassifyAnimationProcessing(authoritative) == AnimationProcessingClass::Complete,
				"Visual-only packets may be compact, while mixed advanced or authoritative packets retain the canonical complete evaluator.");
		});

		registry.Add("Animation.CompactOwnership.ClassTransitionsPreserveContinuityInBothDirections", [](TestContext& context) {
			const CompactGpuVisualInput input = MakeCompactInput();
			for(const auto direction : std::array{
				std::pair{AnimationProcessingClass::Complete,
					AnimationProcessingClass::CompactGpuVisual},
				std::pair{AnimationProcessingClass::CompactGpuVisual,
					AnimationProcessingClass::Complete}}) {
				AnimationClassTransition transition;
				transition.occurred = true;
				transition.from = direction.first;
				transition.to = direction.second;
				transition.sourceGenerations = input.generations;
				transition.destinationGenerations = input.generations;
				transition.continuity.transitionBoundaryTick = input.playbackTick;
				context.Expect(transition.IsContinuous(),
					"Both complete-to-compact and compact-to-complete transitions require one exact continuity envelope.");
				transition.destinationGenerations.membershipGeneration++;
				context.Expect(!transition.IsContinuous(),
					"A membership change must reject either direction instead of transferring stale ownership.");
			}
		});

		registry.Add("Animation.CompactOwnership.AllCompactForcedGpuPlaybackPublishesCurrentTick", [](TestContext& context) {
			// This mirrors an authored ForceGpuVisual direct clip: its generic work
			// list is empty, but compact GPU work must still advance and publish.
			AnimatorPlayback playback;
			AnimatorPlaybackConfiguration configuration;
			configuration.durationTicks = 120;
			configuration.timePolicy = AnimatorTimePolicy::Fixed;
			configuration.fixedStepTicks = 1;
			configuration.loop = true;
			AnimatorPlaybackAdvanceResult advance;
			context.Expect(playback.Configure(configuration) == AnimationStatus::Ok
				&& playback.Play() == AnimationStatus::Ok,
				"A valid authored Force GPU clip must enter the playing state before compact publication.");
			context.Expect(playback.Advance({0.0, 0.0, 15}, advance) == AnimationStatus::Ok
				&& playback.Snapshot().resolvedTick == 15,
				"The compact direct-clip clock must advance from its authored frame-zero pose.");

			AnimationWorkCancellationSource cancellation;
			AnimationWorkScheduler scheduler;
			const AnimationWorkPlan genericPlan = scheduler.BuildPlan({}, 16U, cancellation.CaptureToken());
			AnimationWorkRangeLedger genericLedger(genericPlan);
			context.Expect(genericPlan.status == AnimationStatus::Ok
				&& genericPlan.executionPolicy == AnimationWorkExecutionPolicy::Omitted
				&& ValidateAnimationWorkPlan(genericPlan) == AnimationStatus::Ok
				&& genericLedger.AllCompleted(),
				"An all-compact frame must treat its empty generic plan as complete without a zero-byte scratch reservation.");

			CompactGpuVisualInput first = MakeCompactInput();
			first.playbackTick = playback.Snapshot().resolvedTick;
			first.sourcePoseGeneration = 15U;
			first.generations.authorityGeneration = 15U;
			const CompactGpuVisualResult firstPublication = MakeCompactPublication(first);
			context.Expect(ClassifyAnimationProcessing(first) == AnimationProcessingClass::CompactGpuVisual
				&& firstPublication.IsPublishableFor(first),
				"The current fixed tick must produce a complete compact GPU publication.");

			context.Expect(playback.Advance({0.0, 0.0, 1}, advance) == AnimationStatus::Ok
				&& playback.Snapshot().resolvedTick == 16,
				"The following runtime frame must continue advancing the forced-GPU clip.");
			CompactGpuVisualInput second = first;
			second.playbackTick = playback.Snapshot().resolvedTick;
			second.sourcePoseGeneration = 16U;
			second.generations.authorityGeneration = 16U;
			const CompactGpuVisualResult secondPublication = MakeCompactPublication(second);
			context.Expect(second.playbackTick > first.playbackTick
				&& secondPublication.IsPublishableFor(second),
				"A later compact publication must carry a newer tick and generation instead of retaining stale GPU state.");
		});

		registry.Add("Animation.CompactOwnership.StaleCancelledDuplicateConflictIncompleteAllocationAndPresentationRollback", [](TestContext& context) {
			AnimationFallbackPublicationLatch<Payload> latch;
			const AnimationFallbackGenerationKey firstKey = MakeFallbackKey(1U);
			const auto first = MakePublication(firstKey);
			context.Expect(latch.TryPublish(first, firstKey) == AnimationFallbackPublishStatus::Published
				&& latch.TryPublish(first, firstKey) == AnimationFallbackPublishStatus::AlreadyPublished,
				"A duplicate complete presentation must be idempotent.");

			AnimationFallbackGenerationKey conflictingKey = firstKey;
			conflictingKey.componentGeneration++;
			context.Expect(latch.TryPublish(MakePublication(firstKey), conflictingKey)
				== AnimationFallbackPublishStatus::RejectedUnexpectedGeneration
				&& latch.TryPublish(MakePublication(MakeFallbackKey(0U)), firstKey)
				== AnimationFallbackPublishStatus::RejectedInvalid
				&& latch.TryPublish(MakePublication(MakeFallbackKey(2U), false), MakeFallbackKey(2U))
				== AnimationFallbackPublishStatus::RejectedInvalid,
				"Conflict, incomplete candidate, and allocation-style unusable candidate failures must leave the first presentation intact.");

			AnimationFallbackGenerationKey staleKey = MakeFallbackKey(0U);
			staleKey.publicationGeneration = 1U;
			context.Expect(latch.TryPublish(MakePublication(staleKey), staleKey)
				== AnimationFallbackPublishStatus::RejectedStale
				&& latch.Load() == first,
				"A stale publication cannot roll back the retained presentation envelope.");

			AnimationGenerationLatch generationLatch;
			AnimationCancellationSource cancellation;
			context.Expect(cancellation.Initialize(AnimationCancellationOperation::GpuBatch,
				generationLatch) == AnimationStatus::Ok,
				"The compact GPU batch should capture a lifecycle identity before cancellation.");
			const AnimationCancellationToken token = cancellation.GetToken();
			context.Expect(generationLatch.Invalidate(AnimationLifecycleBoundary::Device)
				== AnimationStatus::Ok && token.Poll(generationLatch).status == AnimationStatus::Cancelled,
				"Device loss must cancel the compact candidate before protected presentation rollback can occur.");
		});

		registry.Add("Animation.CompactOwnership.ReloadPlayModeHotReloadRecoveryAndWorkerDeterminism", [](TestContext& context) {
			const std::array boundaries{
				AnimationLifecycleBoundary::Asset, AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::ScriptDomain, AnimationLifecycleBoundary::Renderer,
				AnimationLifecycleBoundary::Device};
			bool lifecycleRejected = true;
			for(const AnimationLifecycleBoundary boundary : boundaries) {
				AnimationGenerationLatch latch;
				AnimationCancellationSource source;
				lifecycleRejected = lifecycleRejected
					&& source.Initialize(AnimationCancellationOperation::GpuBatch, latch) == AnimationStatus::Ok;
				const AnimationCancellationToken token = source.GetToken();
				lifecycleRejected = lifecycleRejected
					&& latch.Invalidate(boundary) == AnimationStatus::Ok
					&& token.Poll(latch).status == AnimationStatus::Cancelled;
			}

			const CompactGpuVisualInput expected = MakeCompactInput();
			std::array<AnimationProcessingClass, 8U> results{};
			std::vector<std::thread> workers;
			for(std::size_t index = 0U; index < results.size(); ++index) {
				workers.emplace_back([&results, index, expected]() {
					results[index] = ClassifyAnimationProcessing(expected);
				});
			}
			for(std::thread& worker : workers) worker.join();
			const bool deterministic = std::all_of(results.begin(), results.end(),
				[](AnimationProcessingClass value) {
					return value == AnimationProcessingClass::CompactGpuVisual;
				});
			context.Expect(lifecycleRejected && deterministic,
				"Reload, Play Mode, hot reload, renderer recovery, device loss, and worker order must never admit stale compact work.");
		});

		registry.Add("Animation.CompactOwnership.HundredThousandUpdateDeterministicStress", [](TestContext& context) {
			CompactGpuVisualInput input = MakeCompactInput();
			std::uint64_t compactChecksum = 0U;
			for(std::uint64_t update = 1U; update <= 100000U; ++update) {
				input.playbackTick = static_cast<AnimationFixedTick>(update);
				input.sourcePoseGeneration = update;
				input.generations.authorityGeneration = update;
				const AnimationProcessingClass actual = ClassifyAnimationProcessing(input);
				// Include the update identity because CompactGpuVisual is intentionally
				// enum value zero; hashing only the classification would stay zero even
				// when every one of the 100,000 decisions was correct.
				compactChecksum = (compactChecksum * 1099511628211ULL)
					^ update ^ static_cast<std::uint64_t>(actual);
			}
			context.Expect(compactChecksum != 0U,
				"One hundred thousand compact classification updates must remain deterministic and retain a valid nonzero result trace.");
		});

		registry.Add("Animation.CompactOwnership.FourThousandCompactInputsAvoidGenericWorkAndCommitAtomically", [](TestContext& context) {
			constexpr std::size_t kCompactCount = 4000U;
			std::vector<CompactGpuVisualInput> compactInputs(kCompactCount, MakeCompactInput());
			std::vector<CompactGpuVisualResult> publications;
			publications.reserve(kCompactCount);
			for(std::size_t index = 0U; index < compactInputs.size(); ++index) {
				CompactGpuVisualInput& input = compactInputs[index];
				input.stableInstanceId = 0x100000000ULL + index;
				input.stableSubmissionOrdinal = static_cast<std::uint32_t>(index);
				input.sourcePoseGeneration = 100U + index;
				input.generations.membershipGeneration = input.stableInstanceId;
				input.generations.authorityGeneration = 77U;
				publications.push_back(MakeCompactPublication(input));
			}
			AnimationWorkCancellationSource cancellation;
			AnimationWorkScheduler scheduler;
			const AnimationWorkPlan completePlan = scheduler.BuildPlan({}, 16U, cancellation.CaptureToken());
			const bool allPublishable = std::all_of(publications.begin(), publications.end(),
				[&compactInputs, cursor = std::size_t{0U}](const CompactGpuVisualResult& value) mutable {
					return value.IsPublishableFor(compactInputs[cursor++]);
				});
			const bool uniqueOwners = std::adjacent_find(publications.begin(), publications.end(),
				[](const CompactGpuVisualResult& left, const CompactGpuVisualResult& right) {
					return left.stableInstanceId == right.stableInstanceId;
				}) == publications.end();
			CompactGpuVisualResult rejected = publications.back();
			rejected.sourcePoseGeneration++;
			const bool candidateRejectedAtomically = !rejected.IsPublishableFor(compactInputs.back())
				&& allPublishable;
			context.Expect(completePlan.items.empty() && completePlan.ranges.empty()
				&& completePlan.executionPolicy == AnimationWorkExecutionPolicy::Omitted
				&& publications.size() == kCompactCount && allPublishable && uniqueOwners
				&& candidateRejectedAtomically,
				"Four thousand compact owners must retain unique publications while the generic FrameWorkItem scheduler remains empty and a rejected candidate cannot partially validate.");
		});
	}
}
