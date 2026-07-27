// AnimationCachingTests.cpp - generation-complete animation evaluation reuse checks.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationEvaluationCache.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		AnimationEvaluationCacheKey MakeCacheKey(std::uint32_t seed)
		{
			AnimationEvaluationCacheKey key;
			key.animator = {seed, 1U};
			key.evaluationTick = static_cast<AnimationFixedTick>(1000U + seed);
			key.stateGeneration = 10U + seed;
			key.parameterGeneration = 20U + seed;
			key.consumerGeneration = 30U + seed;
			key.executionModeGeneration = 40U + seed;
			key.poseGeneration = 50U + seed;
			key.morphGeneration = 60U + seed;
			key.graphGeneration = 70U + seed;
			key.viewGeneration = 80U + seed;
			key.assetGeneration = 90U + seed;
			key.residencyGeneration = 100U + seed;
			key.materialGeneration = 110U + seed;
			key.rendererGeneration = 120U + seed;
			key.physicsGeneration = 130U + seed;
			key.deterministicGeneration = 140U + seed;
			return key;
		}

		AnimationEvaluationCacheValue MakeCacheValue(std::uint64_t sourceFrame)
		{
			AnimationEvaluationCacheValue value;
			value.sourceFrameGeneration = sourceFrame;
			value.publicationGeneration = sourceFrame + 1000U;
			value.poseToken = sourceFrame + 2000U;
			value.visualPacketToken = sourceFrame + 3000U;
			value.deterministicHash = sourceFrame + 4000U;
			return value;
		}

		void IncrementGeneration(
			AnimationEvaluationCacheKey& key,
			AnimationEvaluationCacheGenerationDomain domain)
		{
			switch(domain) {
			case AnimationEvaluationCacheGenerationDomain::Pose: ++key.poseGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Morph: ++key.morphGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Graph: ++key.graphGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::View: ++key.viewGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Asset: ++key.assetGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Residency: ++key.residencyGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Material: ++key.materialGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Renderer: ++key.rendererGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Physics: ++key.physicsGeneration; break;
			case AnimationEvaluationCacheGenerationDomain::Deterministic: ++key.deterministicGeneration; break;
			}
		}
	}

	void RegisterAnimationCachingTests(TestRegistry& registry)
	{
		registry.Add("Animation.Caching.UnchangedInstanceReusesCompleteImmutableResult", [](TestContext& context) {
			AnimationEvaluationCache cache;
			const AnimationEvaluationCacheKey key = MakeCacheKey(1U);
			const AnimationEvaluationCacheValue value = MakeCacheValue(10U);
			context.Expect(cache.Store(key, value) == AnimationStatus::Ok,
				"A complete generation key must accept one immutable evaluation result.");

			const AnimationEvaluationCacheLookup lookup = cache.Lookup(key, {});
			context.Expect(lookup.status == AnimationStatus::Ok
				&& lookup.disposition == AnimationEvaluationCacheDisposition::Reused
				&& lookup.value != nullptr
				&& lookup.value->publicationGeneration == value.publicationGeneration,
				"An unchanged animator must reuse the exact immutable cached result.");
			context.Expect(!lookup.requiresEvaluation && !lookup.requiresPublication
				&& !lookup.requiresUpload && !lookup.requiresVisibilityInvalidation
				&& !lookup.requiresDetailedDiagnostics,
				"A cache hit must avoid evaluation, publication, upload, visibility, and detailed diagnostic work.");
		});

		registry.Add("Animation.Caching.EveryCorrectnessGenerationInvalidatesReuse", [](TestContext& context) {
			AnimationEvaluationCache cache;
			const AnimationEvaluationCacheKey key = MakeCacheKey(2U);
			context.Expect(cache.Store(key, MakeCacheValue(20U)) == AnimationStatus::Ok,
				"The baseline complete key must be cacheable before domain invalidation checks.");
			constexpr std::array domains{
				AnimationEvaluationCacheGenerationDomain::Pose,
				AnimationEvaluationCacheGenerationDomain::Morph,
				AnimationEvaluationCacheGenerationDomain::Graph,
				AnimationEvaluationCacheGenerationDomain::View,
				AnimationEvaluationCacheGenerationDomain::Asset,
				AnimationEvaluationCacheGenerationDomain::Residency,
				AnimationEvaluationCacheGenerationDomain::Material,
				AnimationEvaluationCacheGenerationDomain::Renderer,
				AnimationEvaluationCacheGenerationDomain::Physics,
				AnimationEvaluationCacheGenerationDomain::Deterministic};
			for(const AnimationEvaluationCacheGenerationDomain domain : domains) {
				AnimationEvaluationCacheKey changed = key;
				IncrementGeneration(changed, domain);
				const AnimationEvaluationCacheLookup lookup = cache.Lookup(changed, {});
				context.Expect(lookup.status == AnimationStatus::NotReady
					&& lookup.disposition == AnimationEvaluationCacheDisposition::Miss,
					"Changing any correctness generation must prevent reuse of a mixed-generation result.");
			}
			// Controller state, parameters, consumers, and resolved mode are equally
			// authoritative even though explicit bulk invalidation targets the ten
			// external subsystem domains above. Exercise every complete-key input.
			constexpr std::array authorityGenerations{
				&AnimationEvaluationCacheKey::stateGeneration,
				&AnimationEvaluationCacheKey::parameterGeneration,
				&AnimationEvaluationCacheKey::consumerGeneration,
				&AnimationEvaluationCacheKey::executionModeGeneration};
			for(const auto generation : authorityGenerations) {
				AnimationEvaluationCacheKey changed = key;
				++(changed.*generation);
				const AnimationEvaluationCacheLookup lookup = cache.Lookup(changed, {});
				context.Expect(lookup.status == AnimationStatus::NotReady
					&& lookup.disposition == AnimationEvaluationCacheDisposition::Miss,
					"Changing controller, parameter, consumer, or mode authority must invalidate reuse.");
			}
			AnimationEvaluationCacheKey changedTick = key;
			++changedTick.evaluationTick;
			context.Expect(cache.Lookup(changedTick, {}).disposition
				== AnimationEvaluationCacheDisposition::Miss,
				"A different fixed evaluation tick must never alias the prior immutable result.");
			AnimationEvaluationCacheKey changedAnimator = key;
			++changedAnimator.animator.generation;
			context.Expect(cache.Lookup(changedAnimator, {}).disposition
				== AnimationEvaluationCacheDisposition::Miss,
				"A recycled animator generation must not reuse the previous handle's result.");

			AnimationEvaluationCacheKey incomplete = key;
			incomplete.rendererGeneration = 0U;
			const AnimationEvaluationCacheLookup invalid = cache.Lookup(incomplete, {});
			context.Expect(invalid.status == AnimationStatus::InvalidArgument
				&& invalid.disposition == AnimationEvaluationCacheDisposition::InvalidKey,
				"A key that omits one required generation must fail closed instead of aliasing a valid entry.");
		});

		registry.Add("Animation.Caching.OffscreenVisibilityNeverSuppressesAuthority", [](TestContext& context) {
			AnimationEvaluationCache cache;
			const AnimationEvaluationCacheKey key = MakeCacheKey(3U);
			(void)cache.Store(key, MakeCacheValue(30U));

			AnimationEvaluationCacheRequirements visualOnly;
			visualOnly.visible = false;
			const AnimationEvaluationCacheLookup offscreenReuse = cache.Lookup(key, visualOnly);
			context.Expect(offscreenReuse.disposition == AnimationEvaluationCacheDisposition::Reused,
				"A truly unchanged offscreen visual-only animator may reuse its complete result.");

			std::array<AnimationEvaluationCacheRequirements, 8U> authoritative{};
			for(AnimationEvaluationCacheRequirements& requirements : authoritative) requirements.visible = false;
			authoritative[0].timeAdvances = true;
			authoritative[1].events = true;
			authoritative[2].rootMotion = true;
			authoritative[3].physics = true;
			authoritative[4].recording = true;
			authoritative[5].replication = true;
			authoritative[6].rollback = true;
			authoritative[7].deterministic = true;
			for(const AnimationEvaluationCacheRequirements& requirements : authoritative) {
				const AnimationEvaluationCacheLookup lookup = cache.Lookup(key, requirements);
				context.Expect(lookup.status == AnimationStatus::NotReady
					&& lookup.disposition == AnimationEvaluationCacheDisposition::RequiredWork
					&& lookup.requiresEvaluation && lookup.requiresPublication
					&& !lookup.requiresUpload,
					"Offscreen gameplay, physics, recording, networking, rollback, and deterministic authority must still evaluate and publish.");
			}
		});

		registry.Add("Animation.Caching.BatchLookupReportsGranularityWithoutReordering", [](TestContext& context) {
			AnimationEvaluationCacheConfiguration configuration;
			configuration.maximumEntries = 256U;
			configuration.maximumBatchRequests = 512U;
			AnimationEvaluationCache cache(configuration);
			std::vector<AnimationEvaluationCacheLookupRequest> requests;
			for(std::uint32_t index = 1U; index <= 160U; ++index) {
				AnimationEvaluationCacheLookupRequest request;
				request.key = MakeCacheKey(index);
				if(index <= 80U) (void)cache.Store(request.key, MakeCacheValue(100U + index));
				if(index <= 10U) {
					request.requirements.visible = false;
					request.requirements.events = true;
				}
				requests.push_back(request);
			}

			std::vector<AnimationEvaluationCacheLookup> results;
			const AnimationEvaluationCacheBatchSummary summary = cache.LookupBatch(requests, results);
			context.Expect(summary.status == AnimationStatus::Ok
				&& summary.requestCount == requests.size()
				&& summary.reusedCount == 70U
				&& summary.requiredWorkCount == 10U
				&& summary.missCount == 80U
				&& results.size() == requests.size(),
				"One bounded lookup batch must report reusable and required work at useful granularity.");
			for(std::size_t index = 0U; index < results.size(); ++index) {
				context.Expect(results[index].requestIndex == index,
					"Cache batching must retain deterministic request order for the scheduler merge.");
			}
		});

		registry.Add("Animation.Caching.ExplicitInvalidationAndStaleStoresStayAtomic", [](TestContext& context) {
			AnimationEvaluationCacheConfiguration configuration;
			configuration.maximumEntries = 2U;
			AnimationEvaluationCache cache(configuration);
			const AnimationEvaluationCacheKey first = MakeCacheKey(10U);
			const AnimationEvaluationCacheKey second = MakeCacheKey(11U);
			const AnimationEvaluationCacheKey third = MakeCacheKey(12U);
			(void)cache.Store(first, MakeCacheValue(10U));
			(void)cache.Store(second, MakeCacheValue(20U));
			(void)cache.Store(third, MakeCacheValue(30U));
			context.Expect(cache.Size() == 2U
				&& cache.Lookup(first, {}).disposition == AnimationEvaluationCacheDisposition::Miss,
				"A bounded cache must evict the least-recent deterministic entry instead of growing without limit.");
			AnimationEvaluationCacheKey evictedLate = first;
			++evictedLate.viewGeneration;
			context.Expect(cache.Store(evictedLate, MakeCacheValue(9U)) == AnimationStatus::StaleHandle,
				"Payload eviction must retain a bounded watermark that rejects an older in-flight worker result.");

			AnimationEvaluationCacheKey newer = second;
			++newer.viewGeneration;
			context.Expect(cache.Store(newer, MakeCacheValue(40U)) == AnimationStatus::Ok,
				"A newer frame for the same animator may replace older view-specific results.");
			AnimationEvaluationCacheKey late = newer;
			++late.viewGeneration;
			context.Expect(cache.Store(late, MakeCacheValue(39U)) == AnimationStatus::StaleHandle,
				"Late worker completion must not replace a newer cached animator generation.");

			const std::size_t invalidated = cache.InvalidateGeneration(
				AnimationEvaluationCacheGenerationDomain::Renderer, third.rendererGeneration);
			context.Expect(invalidated == 1U
				&& cache.Lookup(third, {}).disposition == AnimationEvaluationCacheDisposition::Miss,
				"Explicit renderer recovery invalidation must remove only matching generations.");
			context.Expect(cache.InvalidateAnimator(second.animator) == 1U && cache.Size() == 0U,
				"Animator destruction must remove every remaining cached view atomically.");
		});

		registry.Add("Animation.Caching.ConcurrentAccessRemainsBounded", [](TestContext& context) {
			AnimationEvaluationCacheConfiguration configuration;
			configuration.maximumEntries = 64U;
			configuration.maximumBatchRequests = 256U;
			AnimationEvaluationCache cache(configuration);
			std::atomic_bool failed = false;
			std::vector<std::thread> workers;
			for(std::uint32_t worker = 0U; worker < 8U; ++worker) {
				workers.emplace_back([worker, &cache, &failed]() {
					for(std::uint32_t iteration = 1U; iteration <= 200U; ++iteration) {
						const std::uint32_t seed = 1U + ((worker * 37U + iteration) % 96U);
						const AnimationEvaluationCacheKey key = MakeCacheKey(seed);
						const AnimationStatus stored = cache.Store(key, MakeCacheValue(iteration));
						if(stored != AnimationStatus::Ok && stored != AnimationStatus::StaleHandle) {
							failed.store(true, std::memory_order_release);
						}
						const AnimationEvaluationCacheLookup lookup = cache.Lookup(key, {});
						if(lookup.status != AnimationStatus::Ok && lookup.status != AnimationStatus::NotReady) {
							failed.store(true, std::memory_order_release);
						}
					}
				});
			}
			for(std::thread& worker : workers) worker.join();
			context.Expect(!failed.load(std::memory_order_acquire) && cache.Size() <= 64U,
				"Concurrent worker lookups and stores must remain race-safe and inside the configured bound.");
		});
	}
}
