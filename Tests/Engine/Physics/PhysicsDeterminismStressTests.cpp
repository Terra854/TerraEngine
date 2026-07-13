// PhysicsDeterminismStressTests.cpp - seeded Jolt 5.6 stress replay coverage.
//
// This test intentionally creates a new backend for every worker configuration.
// A fresh backend has the same relevant isolation properties as a new process:
// no previous world, query packets, or provider state can influence the seed.

#include "../TestHarness.h"

#include "Physics/Backend/JoltPhysicsBackend.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace TerraEngine::Tests {
	using namespace TerraEngine::Physics;
	using namespace TerraEngine::Physics::Backend;

	namespace {
		constexpr std::uint32_t kStressBodyCount = 10000U;
		constexpr std::uint32_t kStressActiveBodyCount = 1000U;
		constexpr std::uint32_t kStressRayCount = 10000U;
		constexpr std::uint32_t kStressRayRepetitions = 100U;
		constexpr std::uint32_t kStressSeed = 137903421U;

		// Jolt jobs must use the engine pool. This guard makes each replay isolated
		// and prevents work from one worker-count run leaking into the next one.
		class ThreadPoolSession final {
		public:
			ThreadPoolSession() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
			}

			~ThreadPoolSession() {
				ThreadPool::instance().shutdown();
			}
		};

		struct ReplayHashes {
			std::uint64_t backendState = 0U;
			std::uint64_t sortedPackets = 0U;
			std::uint64_t warmPeakAllocations = 0U;
			std::uint64_t finalPeakAllocations = 0U;
		};

		void HashWord(std::uint64_t& hash, std::uint64_t value) {
			// FNV-1a is sufficient here because this is a reproducibility fingerprint,
			// not a security primitive. Fixed-width input keeps it process-compatible.
			hash ^= value;
			hash *= 1099511628211ULL;
		}

		void HashFloat(std::uint64_t& hash, float value) {
			HashWord(hash, std::bit_cast<std::uint32_t>(value));
		}

		TerraEngine::UUID StableUuid(std::uint32_t index) {
			TerraEngine::UUID result{};
			for(std::size_t byte = 0U; byte < result.size(); ++byte) {
				result[byte] = static_cast<std::uint8_t>((kStressSeed + index * 37U + static_cast<std::uint32_t>(byte) * 19U) >> ((byte % 4U) * 8U));
			}
			return result;
		}

		struct StressBackend final {
			ThreadPoolSession pool;
			JoltPhysicsBackend backend;
			PhysicsWorldHandle world{{9301U}, 1U};
			PhysicsShapeHandle shape{{9301U}, 1U, 0U, 1U};

			explicit StressBackend(std::uint32_t workers)
				: backend(ThreadPool::instance(), JoltPhysicsBackendConfiguration{
					.maximumPhysicsWorkers = workers,
					.maximumBarriers = 8U,
					.maximumJobs = 16384U,
					.temporaryAllocatorBytes = 32U * 1024U * 1024U}) {
			}

			~StressBackend() {
				(void)backend.DestroyWorld(world);
				(void)backend.Shutdown();
			}

			bool CreateSeededWorld() {
				if(!backend.Initialize().Succeeded()) return false;
				PhysicsBackendWorldConfiguration configuration;
				configuration.world = world;
				configuration.maximumBodies = kStressBodyCount + 64U;
				configuration.maximumBodyPairs = 65536U;
				configuration.maximumContactConstraints = 65536U;
				if(!backend.CreateWorld(configuration).Succeeded()) return false;

				PhysicsBackendStagingToken staging;
				PhysicsShapeRecipe recipe;
				recipe.kind = PhysicsShapeKind::Box;
				recipe.dimensions = {0.45F, 0.45F, 0.45F};
				if(!backend.CreateShape(world, shape, recipe, staging).Succeeded()) return false;

				for(std::uint32_t index = 0U; index < kStressBodyCount; ++index) {
					PhysicsBodyCreateRequest request;
					request.handle = {{world.worldId}, world.worldGeneration, index, 1U};
					request.objectId = StableUuid(index);
					request.objectGeneration = 1U;
					request.componentGeneration = 1U;
					request.shape = shape;
					request.motionType = index < kStressActiveBodyCount ? PhysicsMotionType::Dynamic : PhysicsMotionType::Static;
					// The active grid starts above a broad static lattice. It produces many
					// ordinary contacts without deliberately exceeding provider pair bounds.
					request.pose.position = {
						static_cast<float>(index % 100U) * 1.1F,
						index < 256U ? -0.1F : (index < kStressActiveBodyCount ? 2.0F + static_cast<float>((index / 100U) % 10U) * 1.1F : -1.0F),
						static_cast<float>((index / 100U) % 90U) * 1.1F};
					request.useGravity = index < kStressActiveBodyCount;
					request.startAwake = index < kStressActiveBodyCount;
					if(!backend.CreateBody(request, staging).Succeeded()) return false;
				}
				return backend.CommitStaging(staging).Succeeded();
			}

			PhysicsBackendQueryRequest MakeRay(std::uint32_t index) const {
				PhysicsBackendQueryRequest request;
				request.world = world;
				request.kind = PhysicsQueryKind::RayCast;
				request.hitMode = PhysicsQueryHitMode::All;
				request.pose.position = {static_cast<float>(index % 100U) * 1.1F, 8.0F, static_cast<float>((index / 100U) % 90U) * 1.1F};
				request.direction = {0.0F, -1.0F, 0.0F};
				request.distance = 32.0F;
				request.maximumHits = 8U;
				return request;
			}
		};

		std::uint64_t HashExtraction(PhysicsBackendExtraction extraction) {
			std::sort(extraction.bodies.begin(), extraction.bodies.end(), [](const auto& left, const auto& right) {
				return left.handle.slotIndex < right.handle.slotIndex;
			});
			std::uint64_t hash = 1469598103934665603ULL;
			HashWord(hash, extraction.activeBodyCount);
			HashWord(hash, extraction.contactCount);
			for(const PhysicsBackendBodyState& body : extraction.bodies) {
				HashWord(hash, body.handle.slotIndex);
				HashWord(hash, body.handle.slotGeneration);
				HashFloat(hash, body.pose.position.x); HashFloat(hash, body.pose.position.y); HashFloat(hash, body.pose.position.z);
				HashFloat(hash, body.linearVelocity.x); HashFloat(hash, body.linearVelocity.y); HashFloat(hash, body.linearVelocity.z);
				HashWord(hash, body.awake ? 1U : 0U);
			}
			return hash;
		}

		std::uint64_t HashRays(StressBackend& fixture, std::uint32_t repetitions, std::uint64_t* outWarmPeak) {
			std::uint64_t hash = 1469598103934665603ULL;
			std::uint64_t firstBatchHash = 0U;
			for(std::uint32_t repetition = 0U; repetition < repetitions; ++repetition) {
				std::uint64_t batchHash = 1469598103934665603ULL;
				for(std::uint32_t ray = 0U; ray < kStressRayCount; ++ray) {
					const PhysicsBackendQueryResult result = fixture.backend.Query(fixture.MakeRay(ray));
					HashWord(batchHash, result.result.Succeeded() ? 1U : 0U);
					std::vector<PhysicsBackendQueryHit> sortedHits = result.hits;
					std::sort(sortedHits.begin(), sortedHits.end(), [](const auto& left, const auto& right) {
						if(left.distance != right.distance) return left.distance < right.distance;
						if(left.body.slotIndex != right.body.slotIndex) return left.body.slotIndex < right.body.slotIndex;
						return left.subShapeKey < right.subShapeKey;
					});
					HashWord(batchHash, sortedHits.size());
					for(const PhysicsBackendQueryHit& hit : sortedHits) {
						HashWord(batchHash, hit.body.slotIndex);
						HashFloat(batchHash, hit.distance);
						HashFloat(batchHash, hit.position.x); HashFloat(batchHash, hit.position.y); HashFloat(batchHash, hit.position.z);
					}
				}
				if(repetition == 0U) {
					firstBatchHash = batchHash;
				} else if(batchHash != firstBatchHash) {
					// A zero fingerprint is an explicit test failure value, rather than
					// silently folding one nondeterministic batch into the aggregate hash.
					return 0U;
				}
				HashWord(hash, batchHash);
				if(repetition == 0U && outWarmPeak != nullptr) {
					*outWarmPeak = fixture.backend.AllocationDiagnostics().highWaterOutstandingAllocations;
				}
			}
			return hash;
		}

		ReplayHashes Replay(std::uint32_t workers, std::uint32_t rayRepetitions) {
			StressBackend fixture(workers);
			if(!fixture.CreateSeededWorld()) return {};
			for(PhysicsTick tick = 1U; tick <= 3U; ++tick) {
				(void)fixture.backend.Step({fixture.world, tick, 1.0F / 60.0F, 1U});
			}
			PhysicsBackendExtraction extraction;
			extraction.world = fixture.world;
			if(!fixture.backend.Extract(extraction).Succeeded()) return {};

			ReplayHashes hashes;
			hashes.backendState = HashExtraction(std::move(extraction));
			hashes.sortedPackets = HashRays(fixture, rayRepetitions, &hashes.warmPeakAllocations);
			hashes.finalPeakAllocations = fixture.backend.AllocationDiagnostics().highWaterOutstandingAllocations;
			return hashes;
		}
	}

	void RegisterPhysicsDeterminismStressTests(TestRegistry& registry) {
		registry.Add("Physics.DeterminismStress.ProcessCompatibleSeedMatchesAcrossOneTwoFourAndMaxWorkers", [](TestContext& context) {
			ThreadPoolSession capacitySession;
			const std::uint32_t maximumWorkers = std::max(1U, static_cast<std::uint32_t>(ThreadPool::instance().getThreadCount()));
			// The worker configurations are intentionally separate fresh lifetimes. The
			// query pass is one repetition here so all four configurations remain a
			// practical CI determinism gate; the next test owns the full 100-pass soak.
			const std::array<std::uint32_t, 4U> workerCounts{1U, 2U, 4U, maximumWorkers};
			const ReplayHashes baseline = Replay(workerCounts.front(), 1U);
			context.Expect(baseline.backendState != 0U && baseline.sortedPackets != 0U,
				"The pinned Jolt 5.6 seed must produce non-empty backend and sorted-packet hashes.");
			for(const std::uint32_t workers : workerCounts) {
				const ReplayHashes replay = Replay(workers, 1U);
				context.Expect(replay.backendState == baseline.backendState && replay.sortedPackets == baseline.sortedPackets,
					"Fresh process-compatible replays must preserve backend state and sorted packet hashes at every supported worker count.");
			}
		});

		registry.Add("Physics.DeterminismStress.OneHundredIdenticalTenThousandRayBatchesRemainOrderedAndMemoryBounded", [](TestContext& context) {
			const ReplayHashes replay = Replay(1U, kStressRayRepetitions);
			context.Expect(replay.backendState != 0U && replay.sortedPackets != 0U,
				"The 10,000-body fixture must complete all one hundred 10,000-ray repetitions.");
			context.Expect(replay.finalPeakAllocations <= replay.warmPeakAllocations + 32U,
				"After the first full ray batch, repeated identical queries must not grow peak provider allocation count beyond a small bounded allowance.");
		});
	}
}
