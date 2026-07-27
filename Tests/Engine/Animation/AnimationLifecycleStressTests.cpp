// AnimationLifecycleStressTests.cpp - In-flight lifecycle and fallback safety.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationCancellation.h"
#include "Animation/Runtime/AnimationFallback.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <thread>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		struct LifecycleBoundaryCase {
			AnimationLifecycleBoundary boundary;
			AnimationCancellationReason expectedReason;
		};

		struct FallbackPayload {
			std::uint64_t marker = 0U;
		};

		using FallbackPublication = AnimationFallbackPublication<FallbackPayload>;
		using FallbackLatch = AnimationFallbackPublicationLatch<FallbackPayload>;

		AnimationFallbackGenerationKey MakeFallbackKey(
			AnimationGeneration publicationGeneration,
			AnimationGeneration contentGeneration = 1U,
			AnimationGeneration animatorGeneration = 1U,
			AnimationGeneration componentGeneration = 1U,
			AnimationGeneration sceneGeneration = 1U)
		{
			AnimationFallbackGenerationKey key;
			key.animator = {4U, animatorGeneration};
			key.componentGeneration = componentGeneration;
			key.sceneGeneration = sceneGeneration;
			key.contentGeneration = contentGeneration;
			key.rendererGeneration = 1U;
			key.physicsWorldGeneration = 1U;
			key.publicationGeneration = publicationGeneration;
			key.compatibilityFingerprint = 0x123456789abcdef0ULL;
			return key;
		}

		std::shared_ptr<const FallbackPublication> MakeFallbackPublication(
			const AnimationFallbackGenerationKey& generations,
			AnimationFallbackSource source,
			std::uint64_t marker,
			bool complete = true,
			bool finite = true)
		{
			auto publication = std::make_shared<FallbackPublication>();
			publication->generations = generations;
			publication->source = source;
			publication->payload = std::make_shared<FallbackPayload>(FallbackPayload{marker});
			publication->complete = complete;
			publication->finite = finite;
			return publication;
		}
	}

	void RegisterAnimationLifecycleStressTests(TestRegistry& registry)
	{
		registry.Add("Animation.Lifecycle.EveryBoundaryCancelsCapturedWorkAndAdmitsFreshGenerations", [](TestContext& context) {
			constexpr std::array cases{
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Object, AnimationCancellationReason::ObjectReused},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Component, AnimationCancellationReason::ComponentRemoved},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Scene, AnimationCancellationReason::SceneUnloaded},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Asset, AnimationCancellationReason::AssetReloaded},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::ScriptDomain, AnimationCancellationReason::ScriptReloaded},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::PlayMode, AnimationCancellationReason::PlayModeStopped},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::PhysicsWorld, AnimationCancellationReason::PhysicsWorldReplaced},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Renderer, AnimationCancellationReason::RendererRecovered},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Device, AnimationCancellationReason::DeviceLost},
				LifecycleBoundaryCase{AnimationLifecycleBoundary::Shutdown, AnimationCancellationReason::Shutdown}};

			bool allCancelledCorrectly = true;
			bool allFreshGenerationsActive = true;
			for(const LifecycleBoundaryCase& boundaryCase : cases) {
				AnimationGenerationLatch latch;
				AnimationCancellationSource source;
				const AnimationLifecycleMask mask = AnimationLifecycleBoundaryMask(boundaryCase.boundary);
				allCancelledCorrectly = allCancelledCorrectly
					&& source.Initialize(
						AnimationCancellationOperation::CpuEvaluation,
						latch,
						mask) == AnimationStatus::Ok;
				const AnimationCancellationToken stale = source.GetToken();
				allCancelledCorrectly = allCancelledCorrectly
					&& latch.Invalidate(boundaryCase.boundary) == AnimationStatus::Ok;
				const AnimationCancellationPollResult rejected = stale.Poll(latch);
				allCancelledCorrectly = allCancelledCorrectly
					&& rejected.status == AnimationStatus::Cancelled
					&& rejected.reason == boundaryCase.expectedReason
					&& rejected.boundary == boundaryCase.boundary
					&& rejected.capturedGeneration != rejected.currentGeneration;

				if(boundaryCase.boundary != AnimationLifecycleBoundary::Shutdown) {
					AnimationCancellationSource fresh;
					allFreshGenerationsActive = allFreshGenerationsActive
						&& fresh.Initialize(
							AnimationCancellationOperation::CpuEvaluation,
							latch,
							mask) == AnimationStatus::Ok
						&& fresh.GetToken().Poll(latch).CanPublish();
				}
			}

			context.Expect(allCancelledCorrectly && allFreshGenerationsActive,
				"Object reuse, scene unload, component removal, reload, Play stop, physics replacement, device loss, and shutdown must reject old work only.");
		});

		registry.Add("Animation.Lifecycle.DefaultMasksCoverEveryAsynchronousPipeline", [](TestContext& context) {
			const AnimationLifecycleMask importMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::Import);
			const AnimationLifecycleMask cpuMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::CpuEvaluation);
			const AnimationLifecycleMask gpuMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::GpuBatch);
			const AnimationLifecycleMask recordingMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::Recording);
			const AnimationLifecycleMask reloadMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::Reload);
			const AnimationLifecycleMask sceneMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::ScenePublication);
			const AnimationLifecycleMask physicsMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::PhysicsHandoff);
			const AnimationLifecycleMask managedMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::ManagedDelivery);
			const AnimationLifecycleMask shutdownMask = DefaultAnimationCancellationMask(
				AnimationCancellationOperation::Shutdown);

			auto has = [](AnimationLifecycleMask mask, AnimationLifecycleBoundary boundary) {
				return (mask & AnimationLifecycleBoundaryMask(boundary)) != 0U;
			};
			context.Expect(has(importMask, AnimationLifecycleBoundary::Asset)
				&& has(importMask, AnimationLifecycleBoundary::Shutdown)
				&& has(cpuMask, AnimationLifecycleBoundary::Object)
				&& has(cpuMask, AnimationLifecycleBoundary::PhysicsWorld)
				&& has(gpuMask, AnimationLifecycleBoundary::Renderer)
				&& has(gpuMask, AnimationLifecycleBoundary::Device)
				&& has(recordingMask, AnimationLifecycleBoundary::PlayMode)
				&& has(reloadMask, AnimationLifecycleBoundary::Asset)
				&& has(reloadMask, AnimationLifecycleBoundary::ScriptDomain)
				&& has(sceneMask, AnimationLifecycleBoundary::Scene)
				&& has(physicsMask, AnimationLifecycleBoundary::PhysicsWorld)
				&& has(managedMask, AnimationLifecycleBoundary::ScriptDomain)
				&& shutdownMask == AnimationLifecycleBoundaryMask(AnimationLifecycleBoundary::Shutdown),
				"Typed import, CPU, GPU, recording, reload, scene, physics, managed, and shutdown operations need explicit lifecycle coverage.");
		});

		registry.Add("Animation.Lifecycle.ExplicitCancellationIsIdempotentAndFirstReasonWins", [](TestContext& context) {
			AnimationGenerationLatch latch;
			AnimationCancellationSource source;
			context.Expect(source.Initialize(
				AnimationCancellationOperation::GpuBatch,
				latch) == AnimationStatus::Ok,
				"A GPU batch token should capture the default generation set.");
			const AnimationCancellationToken token = source.GetToken();
			context.Expect(source.Cancel(AnimationCancellationReason::DeviceLost)
				&& !source.Cancel(AnimationCancellationReason::Explicit),
				"The first racing cancellation reason should latch exactly once.");
			const AnimationCancellationPollResult result = token.Poll(latch);
			context.Expect(result.status == AnimationStatus::Cancelled
				&& result.reason == AnimationCancellationReason::DeviceLost
				&& result.operation == AnimationCancellationOperation::GpuBatch,
				"Explicit cancellation should preserve its typed operation and first reason.");
		});

		registry.Add("Animation.Lifecycle.SourceDestructionAndShutdownCannotLeaveLiveTokens", [](TestContext& context) {
			AnimationGenerationLatch latch;
			AnimationCancellationToken orphaned;
			{
				AnimationCancellationSource source;
				static_cast<void>(source.Initialize(
					AnimationCancellationOperation::Recording,
					latch));
				orphaned = source.GetToken();
			}
			context.Expect(orphaned.Poll(latch).reason == AnimationCancellationReason::OwnerReleased,
				"Destroying a source must cancel copied worker tokens instead of orphaning them.");

			context.Expect(latch.Invalidate(AnimationLifecycleBoundary::Shutdown) == AnimationStatus::Ok,
				"Shutdown should terminally invalidate its generation.");
			AnimationCancellationSource afterShutdown;
			context.Expect(afterShutdown.Initialize(
				AnimationCancellationOperation::CpuEvaluation,
				latch) == AnimationStatus::Cancelled,
				"A terminal shutdown latch must reject admission of new asynchronous work.");
		});

		registry.Add("Animation.Lifecycle.GenerationExhaustionFailsClosedWithoutAliasing", [](TestContext& context) {
			AnimationGenerationLatch latch;
			context.Expect(latch.SetGenerationForTesting(
				AnimationLifecycleBoundary::Device,
				std::numeric_limits<std::uint64_t>::max()) == AnimationStatus::Ok,
				"The narrow test hook should place a live latch at its final generation.");
			AnimationCancellationSource source;
			const AnimationLifecycleMask deviceMask = AnimationLifecycleBoundaryMask(
				AnimationLifecycleBoundary::Device);
			context.Expect(source.Initialize(
				AnimationCancellationOperation::GpuBatch,
				latch,
				deviceMask) == AnimationStatus::Ok,
				"The last nonzero generation remains a valid captured identity.");
			const AnimationCancellationToken ancient = source.GetToken();
			context.Expect(latch.Invalidate(AnimationLifecycleBoundary::Device) == AnimationStatus::Ok,
				"Invalidating the final generation should terminally retire the latch.");
			const AnimationCancellationPollResult result = ancient.Poll(latch);
			AnimationCancellationSource replacement;
			context.Expect(result.reason == AnimationCancellationReason::GenerationExhausted
				&& result.currentGeneration == 0U
				&& replacement.Initialize(
					AnimationCancellationOperation::GpuBatch,
					latch,
					deviceMask) == AnimationStatus::Cancelled,
				"Generation exhaustion must never wrap to a value held by stale device work.");
		});

		registry.Add("Animation.Lifecycle.RecoverableFailureHoldsPriorPublicationAtomically", [](TestContext& context) {
			FallbackLatch latch;
			const AnimationFallbackGenerationKey priorKey = MakeFallbackKey(1U, 1U);
			const std::shared_ptr<const FallbackPublication> prior = MakeFallbackPublication(
				priorKey,
				AnimationFallbackSource::Current,
				101U);
			context.Expect(latch.TryPublish(prior, priorKey) == AnimationFallbackPublishStatus::Published,
				"The fixture should establish one complete immutable prior generation.");

			const AnimationFallbackGenerationKey expected = MakeFallbackKey(2U, 2U);
			AnimationFallbackRequest request;
			request.policy = AnimationFallbackPolicy::CpuThenHoldLast;
			request.reason = AnimationFallbackReason::AllocationFailure;
			request.expectedGenerations = expected;
			request.current = {expected, true, false, true};
			request.prior = DescribeAnimationFallbackPublication(prior);
			request.bindPoseAvailable = true;
			const AnimationFallbackDecision decision = SelectAnimationFallback(request);
			context.Expect(decision.status == AnimationFallbackDecisionStatus::Degraded
				&& decision.source == AnimationFallbackSource::HeldPriorGeneration
				&& decision.preservesPriorGeneration
				&& !decision.publishesNewGeneration,
				"Allocation failure should retain a compatible complete prior generation until CPU output is ready.");

			const std::shared_ptr<const FallbackPublication> incomplete = MakeFallbackPublication(
				expected,
				AnimationFallbackSource::CpuDeterministic,
				202U,
				false,
				true);
			context.Expect(latch.TryPublish(incomplete, expected)
				== AnimationFallbackPublishStatus::RejectedInvalid
				&& latch.Load() == prior,
				"An incomplete recovery result must not replace any field of the prior publication.");
		});

		registry.Add("Animation.Lifecycle.DeviceLossSelectsCompleteCpuThenBindFallback", [](TestContext& context) {
			const AnimationFallbackGenerationKey expected = MakeFallbackKey(2U, 2U);
			AnimationFallbackRequest cpuRequest;
			cpuRequest.policy = AnimationFallbackPolicy::CpuThenHoldLast;
			cpuRequest.reason = AnimationFallbackReason::DeviceLost;
			cpuRequest.expectedGenerations = expected;
			cpuRequest.cpuFallback = {expected, true, true, true};
			cpuRequest.bindPoseAvailable = true;
			const AnimationFallbackDecision cpuDecision = SelectAnimationFallback(cpuRequest);
			context.Expect(cpuDecision.status == AnimationFallbackDecisionStatus::Degraded
				&& cpuDecision.source == AnimationFallbackSource::CpuDeterministic
				&& cpuDecision.reason == AnimationFallbackReason::DeviceLost
				&& cpuDecision.publishesNewGeneration,
				"Device loss should select a complete CPU generation before conservative bind pose.");

			AnimationFallbackRequest bindRequest = cpuRequest;
			bindRequest.cpuFallback = {};
			bindRequest.policy = AnimationFallbackPolicy::CpuThenBindPose;
			const AnimationFallbackDecision bindDecision = SelectAnimationFallback(bindRequest);
			context.Expect(bindDecision.source == AnimationFallbackSource::BindPose
				&& bindDecision.status == AnimationFallbackDecisionStatus::Degraded,
				"When CPU recovery is unavailable, the explicit policy should expose bind-pose fallback.");
		});

		registry.Add("Animation.Lifecycle.ReusedObjectRejectsOldFallbackAtLiveBoundary", [](TestContext& context) {
			FallbackLatch latch;
			const AnimationFallbackGenerationKey oldKey = MakeFallbackKey(1U, 1U, 1U, 1U, 1U);
			const std::shared_ptr<const FallbackPublication> oldPublication = MakeFallbackPublication(
				oldKey,
				AnimationFallbackSource::Current,
				10U);
			static_cast<void>(latch.TryPublish(oldPublication, oldKey));

			const AnimationFallbackGenerationKey replacementKey = MakeFallbackKey(1U, 1U, 2U, 2U, 1U);
			const std::shared_ptr<const FallbackPublication> staleCompletion = MakeFallbackPublication(
				MakeFallbackKey(2U, 2U, 1U, 1U, 1U),
				AnimationFallbackSource::CpuDeterministic,
				20U);
			context.Expect(latch.TryPublish(staleCompletion, replacementKey)
				== AnimationFallbackPublishStatus::RejectedUnexpectedGeneration
				&& latch.Load() == oldPublication,
				"Protected publication must compare against the live replacement identity, never the worker's copied expectation.");

			const std::shared_ptr<const FallbackPublication> replacement = MakeFallbackPublication(
				replacementKey,
				AnimationFallbackSource::Current,
				30U);
			context.Expect(latch.TryPublish(replacement, replacementKey)
				== AnimationFallbackPublishStatus::Published
				&& latch.Load() == replacement,
				"A complete new object generation may replace the retired lifecycle atomically.");
		});

		registry.Add("Animation.Lifecycle.ConcurrentFallbackPublicationNeverRollsBack", [](TestContext& context) {
			FallbackLatch latch;
			const AnimationFallbackGenerationKey firstKey = MakeFallbackKey(1U);
			const std::shared_ptr<const FallbackPublication> first = MakeFallbackPublication(
				firstKey,
				AnimationFallbackSource::Current,
				1U);
			static_cast<void>(latch.TryPublish(first, firstKey));

			const AnimationFallbackGenerationKey secondKey = MakeFallbackKey(2U);
			const AnimationFallbackGenerationKey thirdKey = MakeFallbackKey(3U);
			const std::shared_ptr<const FallbackPublication> second = MakeFallbackPublication(
				secondKey,
				AnimationFallbackSource::CpuDeterministic,
				2U);
			const std::shared_ptr<const FallbackPublication> third = MakeFallbackPublication(
				thirdKey,
				AnimationFallbackSource::CpuDeterministic,
				3U);
			std::atomic_bool begin = false;
			std::thread lower([&]() {
				while(!begin.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				static_cast<void>(latch.TryPublish(second, secondKey));
			});
			std::thread higher([&]() {
				while(!begin.load(std::memory_order_acquire)) {
					std::this_thread::yield();
				}
				static_cast<void>(latch.TryPublish(third, thirdKey));
			});
			begin.store(true, std::memory_order_release);
			lower.join();
			higher.join();

			const std::shared_ptr<const FallbackPublication> published = latch.Load();
			context.Expect(published == third
				&& latch.TryPublish(second, secondKey) == AnimationFallbackPublishStatus::RejectedStale
				&& latch.Load() == third,
				"CAS publication must converge on the newest complete generation and reject every late older result.");
		});
	}
}
