// AnimationPresentationSystem.cpp - worker preparation and protected plan swap.

#include "AnimationPresentationSystem.h"

#include "AnimationSystem.h"
#include "Core/Scheduling/JobGraph.h"
#include "Physics/PhysicsSystem.h"
#include "Profiling/ProfilerScope.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Animation {
	bool AnimationPostPhysicsBoundary::IsValid() const noexcept
	{
		if(schedulerGeneration == 0U) return false;
		switch(kind) {
		case AnimationPostPhysicsBoundaryKind::NoWorld:
			return physicsWorldGeneration == 0U && publishedTick == 0U
				&& snapshotGeneration == 0U && snapshot == nullptr;
		case AnimationPostPhysicsBoundaryKind::ActiveWorldWithoutSnapshot:
			// This state is diagnostic evidence, not a publishable boundary. An active
			// world must provide the exact immutable snapshot consumed by presentation.
			return false;
		case AnimationPostPhysicsBoundaryKind::PublishedSnapshot:
			return physicsWorldGeneration != 0U && publishedTick != 0U
				&& snapshotGeneration != 0U && snapshot != nullptr
				&& snapshot->world.IsValid()
				&& snapshot->world.worldGeneration == physicsWorldGeneration
				&& snapshot->tick == publishedTick
				&& snapshot->storeGeneration == snapshotGeneration;
		}
		return false;
	}

	void PublishedGpuAnimationPresentationPlan::BuildOwnerObjectLookup() noexcept
	{
		retainedOwnerObjectMappings.reset();
		retainedOwnerObjectLookupIndices.reset();
		ownerObjectLookupIndices.clear();
		ownerObjectLookupMask = 0U;
		if(ownerObjectMappings.empty()) {
			return;
		}

		// A maximum load factor of one half keeps the bounded linear probe short for
		// large crowds. Overflow or allocation failure deliberately leaves the sorted
		// vector as the exact, allocation-free lookup fallback.
		std::size_t tableSize = 1U;
		const std::size_t requiredSize = ownerObjectMappings.size() <=
			(std::numeric_limits<std::size_t>::max() / 2U)
			? ownerObjectMappings.size() * 2U
			: std::numeric_limits<std::size_t>::max();
		while(tableSize < requiredSize
			&& tableSize <= (std::numeric_limits<std::uint32_t>::max() / 2U)) {
			tableSize <<= 1U;
		}
		if(tableSize < requiredSize
			|| tableSize > static_cast<std::size_t>(
				std::numeric_limits<std::uint32_t>::max())) {
			return;
		}

		const std::uint32_t emptyIndex = std::numeric_limits<std::uint32_t>::max();
		try {
			ownerObjectLookupIndices.assign(tableSize, emptyIndex);
		} catch(const std::bad_alloc&) {
			ownerObjectLookupIndices.clear();
			return;
		}
		const std::uint32_t mask = static_cast<std::uint32_t>(tableSize - 1U);
		for(std::size_t mappingIndex = 0U;
			mappingIndex < ownerObjectMappings.size(); ++mappingIndex) {
			if(mappingIndex > std::numeric_limits<std::uint32_t>::max()) {
				ownerObjectLookupIndices.clear();
				return;
			}
			const GpuAnimationOwnerObjectMapping& mapping =
				ownerObjectMappings[mappingIndex];
			std::uint32_t bucket = static_cast<std::uint32_t>(
				std::hash<UUID>{}(mapping.ownerObjectId)) & mask;
			bool inserted = false;
			for(std::size_t probe = 0U; probe < tableSize; ++probe) {
				std::uint32_t& storedIndex = ownerObjectLookupIndices[bucket];
				if(storedIndex == emptyIndex) {
					storedIndex = static_cast<std::uint32_t>(mappingIndex);
					inserted = true;
					break;
				}
				if(ownerObjectMappings[storedIndex].ownerObjectId
					== mapping.ownerObjectId) {
					// Sorted publication order guarantees the first occurrence is the
					// beginning of the exact adjacent owner range.
					inserted = true;
					break;
				}
				bucket = (bucket + 1U) & mask;
			}
			if(!inserted) {
				ownerObjectLookupIndices.clear();
				return;
			}
		}
		ownerObjectLookupMask = mask;
	}

	const GpuAnimationOwnerObjectMapping*
	PublishedGpuAnimationPresentationPlan::FindFirstOwnerObjectMapping(
			const UUID& ownerObjectId) const noexcept
	{
		const auto& mappings = OwnerObjectMappings();
		const auto& lookupIndices = OwnerObjectLookupIndices();
		if(ownerObjectId == EMPTY_UUID || mappings.empty()) {
			return nullptr;
		}
		if(!lookupIndices.empty()
			&& lookupIndices.size()
				== static_cast<std::size_t>(ownerObjectLookupMask) + 1U) {
			const std::uint32_t emptyIndex = std::numeric_limits<std::uint32_t>::max();
			std::uint32_t bucket = static_cast<std::uint32_t>(
				std::hash<UUID>{}(ownerObjectId)) & ownerObjectLookupMask;
			for(std::size_t probe = 0U; probe < lookupIndices.size(); ++probe) {
				const std::uint32_t storedIndex = lookupIndices[bucket];
				if(storedIndex == emptyIndex) {
					return nullptr;
				}
				if(storedIndex < mappings.size()
					&& mappings[storedIndex].ownerObjectId == ownerObjectId) {
					return &mappings[storedIndex];
				}
				bucket = (bucket + 1U) & ownerObjectLookupMask;
			}
			return nullptr;
		}

		const auto mapping = std::lower_bound(
			mappings.begin(), mappings.end(), ownerObjectId,
			[](const GpuAnimationOwnerObjectMapping& candidate, const UUID& value) {
				return candidate.ownerObjectId < value;
			});
		return mapping != mappings.end()
			&& mapping->ownerObjectId == ownerObjectId ? &*mapping : nullptr;
	}

	const AnimationAuthorityRow*
	AnimationPresentationFrame::FindAuthorityRowForOwner(const UUID& ownerObjectId) const noexcept
	{
		if(!authority || !ownerBindings || ownerObjectId == EMPTY_UUID) return nullptr;
		const auto binding = std::lower_bound(ownerBindings->begin(), ownerBindings->end(), ownerObjectId,
			[](const OwnerBinding& value, const UUID& owner) { return value.ownerObjectId < owner; });
		if(binding == ownerBindings->end() || binding->ownerObjectId != ownerObjectId) return nullptr;
		// Multiple live animators for one object are not a valid render binding.
		// Rejecting this rather than choosing the first prevents a wrong palette.
		const auto next = std::next(binding);
		if(next != ownerBindings->end() && next->ownerObjectId == ownerObjectId) return nullptr;
		return authority->Find(binding->key);
	}

	namespace {
		constexpr std::size_t kInlinePresentationThreshold = 16U;
	}

	struct AnimationPresentationSystem::Impl {
		std::atomic_size_t publishedAnimatorCount = 0U;
		std::atomic_uint64_t presentationPublicationGeneration = 0U;
		// Submission is deliberately a copied immutable handoff. The mutex protects
		// only pointer replacement; workers never hold it while lowering a plan.
		mutable std::mutex frameMutex;
		std::shared_ptr<const GpuAnimationPresentationFrame> submittedFrame;
		std::uint64_t submittedSourceGeneration = 0U;
		std::shared_ptr<const PublishedGpuAnimationPresentationPlan> publishedPlan;
		// Consumers load one root containing both authority and presentation data.
		std::shared_ptr<const AnimationPresentationFrame> publishedFrame;
		// The immutable combined atlas normally survives many dynamic sample ticks.
		// Preparation owns this cache under a narrow mutex so future overlapping
		// frame production cannot race a relocation-layout replacement.
		mutable std::mutex atlasCacheMutex;
		GpuAnimationFrameAtlasCache atlasCache;
		// Dynamic lane recycling is independent from atlas mutation. Keeping a
		// separate lock lets exact warm topology refreshes avoid waiting behind cold
		// heterogeneous payload packing while preserving overlapping-frame safety.
		mutable std::mutex dynamicLanePoolMutex;
		// Dynamic plan lanes are immutable after publication but become recyclable
		// once their shared owner count returns to one. Four normal slots cover the
		// renderer's in-flight frames; the overflow slot preserves correctness when
		// a diagnostic/capture reader retains an older publication for longer.
		std::array<std::shared_ptr<GpuAnimationPlanDynamicLanes>, 4U>
			dynamicLanePool{};
		std::size_t dynamicLanePoolCursor = 0U;
		std::shared_ptr<GpuAnimationPlanDynamicLanes> dynamicLaneOverflowScratch;
		std::unique_ptr<AnimationGenerationLatch> lifecycleLatch =
			std::make_unique<AnimationGenerationLatch>();
		AnimationCancellationSource gpuCancellationSource;
		// The singleton starts active for the existing engine bootstrap path. Only
		// an explicit Initialize may begin another presentation runtime session after
		// Shutdown; dormant declarations must never resurrect publication ownership.
		std::atomic_bool runtimeActive = true;
		// Every shutdown advances the epoch before a later Initialize can resume the
		// system. Workload closures capture this value so descriptors retained by a
		// cancelled frame cannot publish into a replacement runtime session.
		std::atomic_uint64_t runtimeSessionEpoch = 1U;
		// Every coordinator declaration receives a distinct token. This prevents a
		// cancelled frame whose preparation finished from publishing after a newer
		// frame has already replaced it within the same runtime session.
		std::atomic_uint64_t frameDeclarationEpoch = 0U;
		// These tokens are touched only by coordinator-bound declaration/publication
		// callbacks. They keep the steady zero-animator path free of atomic writes.
		bool hasActiveFrameDeclaration = false;
		std::uint64_t lastPublishedFrameDeclarationEpoch = 0U;
		// Protected presentation ownership remains on the singleton's construction
		// thread for its entire process lifetime. Initialize must not silently move
		// scheduler-bound publication to whichever thread happens to call it later.
		const std::thread::id coordinatorThread = std::this_thread::get_id();

		[[nodiscard]] std::shared_ptr<GpuAnimationPlanDynamicLanes>&
		AcquireDynamicLaneScratch() noexcept
		{
			for(std::size_t attempt = 0U; attempt < dynamicLanePool.size(); ++attempt) {
				const std::size_t index = (dynamicLanePoolCursor + attempt)
					% dynamicLanePool.size();
				auto& candidate = dynamicLanePool[index];
				try {
					if(!candidate) {
						candidate = std::make_shared<GpuAnimationPlanDynamicLanes>();
					}
				} catch(const std::bad_alloc&) {
					continue;
				}
				if(candidate.use_count() == 1U) {
					candidate->ResetForReuse();
					dynamicLanePoolCursor = (index + 1U) % dynamicLanePool.size();
					return candidate;
				}
			}
			try {
				dynamicLaneOverflowScratch =
					std::make_shared<GpuAnimationPlanDynamicLanes>();
			} catch(const std::bad_alloc&) {
				dynamicLaneOverflowScratch.reset();
			}
			return dynamicLaneOverflowScratch;
		}

		void AssertCoordinatorThread() const
		{
			// The system reads AnimationSystem's coordinator-cached membership count.
			// It must never introduce a second AnimatorInstanceStore scan on a worker.
			assert(coordinatorThread == std::this_thread::get_id()
				&& "Animation presentation declarations must be bound on the coordinator thread.");
                }
        };

        struct PreparedGpuAnimationPresentationFrame {
          std::shared_ptr<const GpuAnimationPresentationFrame> source;
          GpuAnimationPlan plan{};
          GpuAnimationLoweredPlanResult result{};
          GpuAnimationTopology topology{};
          GpuAnimationTopologyDynamicState topologyDynamicState{};
          bool topologyReused = false;
          double topologyBuildMs = 0.0;
          double topologyRefreshMs = 0.0;
          std::uint64_t sourceGeneration = 0U;
          std::uint32_t planGeneration = 0U;
          AnimationPostPhysicsBoundary postPhysicsBoundary;
		  std::shared_ptr<const std::vector<AnimationPresentationFrame::OwnerBinding>>
			  ownerBindings;
          // Exact matching signatures and topology keys allow these already
          // validated immutable ordering roots to be copied without rebuilding
          // hashes or sorting.
          std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
              priorPlan;
          bool ownerMappingsReusable = false;
          // Worker preparation owns every fallible presentation allocation. The
          // protected workload later validates identities and exchanges these
          // already immutable roots without rebuilding owner maps or fallback
          // packets.
          std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
              publication;
          std::shared_ptr<const AnimationPresentationFrame> presentationFrame;
          std::uint64_t expectedPresentationGeneration = 0U;
          bool preservePriorPublication = false;
          std::atomic_bool complete = false;
          std::atomic_bool cancelled = false;
        };

        struct LoweredOwnerKey {
          std::uint64_t stableInstanceId = 0U;
          std::uint32_t instanceSlot = 0U;
          std::uint32_t instanceGeneration = 0U;
          std::uint32_t componentGeneration = 0U;

          bool operator==(const LoweredOwnerKey &) const noexcept = default;
        };

        struct LoweredOwnerKeyHash {
          std::size_t operator()(const LoweredOwnerKey &value) const noexcept {
            std::size_t hash =
                std::hash<std::uint64_t>{}(value.stableInstanceId);
            hash ^= std::hash<std::uint32_t>{}(value.instanceSlot) +
                    0x9e3779b9U + (hash << 6U) + (hash >> 2U);
            hash ^= std::hash<std::uint32_t>{}(value.instanceGeneration) +
                    0x9e3779b9U + (hash << 6U) + (hash >> 2U);
            hash ^= std::hash<std::uint32_t>{}(value.componentGeneration) +
                    0x9e3779b9U + (hash << 6U) + (hash >> 2U);
            return hash;
          }
        };

        [[nodiscard]] LoweredOwnerKey MakeLoweredOwnerKey(
            const GpuAnimationOwnerObjectMapping &mapping) noexcept {
          return {mapping.stableInstanceId, mapping.instanceSlot,
                  mapping.instanceGeneration, mapping.componentGeneration};
        }

        [[nodiscard]] bool BuildPreparedPresentationRoots(
            PreparedGpuAnimationPresentationFrame &prepared,
            std::uint64_t presentationGeneration) noexcept {
          try {
            auto publication =
                std::make_shared<PublishedGpuAnimationPresentationPlan>();
            publication->presentationGeneration = presentationGeneration;
            publication->sourceGeneration = prepared.sourceGeneration;
            publication->ownerMappingSignature =
                prepared.source->ownerMappingSignature;
            publication->loweringStatus = prepared.result.status;
            publication->planStatus = prepared.result.planStatus;
            publication->topologyReused = prepared.topologyReused;
            publication->topologyBuildMs = prepared.topologyBuildMs;
            publication->topologyRefreshMs = prepared.topologyRefreshMs;
            // Empty or incomplete worker output is an explicit safe result.  It
            // must never retain a mixture of this frame's generations and an
            // old topology.
            publication->safeFallback =
                prepared.result.status != GpuAnimationLoweringStatus::Ok ||
                prepared.plan.DispatchCommands().empty() ||
                !prepared.topology.IsComplete() ||
                !prepared.topologyDynamicState.IsCompleteFor(prepared.topology);
            if (!publication->safeFallback) {
              publication->plan = std::move(prepared.plan);
              publication->topology = std::move(prepared.topology);
              publication->topologyDynamicState =
                  std::move(prepared.topologyDynamicState);
              try {
                if (prepared.ownerMappingsReusable && prepared.priorPlan) {
                  // The topology generation key and stable-owner signature were
                  // both matched during worker preparation. Preserve the exact
                  // sorted mapping and lookup table instead of hashing and
                  // sorting 4,000 unchanged owners.
				  publication->retainedOwnerObjectMappings =
					  prepared.priorPlan->retainedOwnerObjectMappings
					  ? prepared.priorPlan->retainedOwnerObjectMappings
					  : std::shared_ptr<const std::vector<GpuAnimationOwnerObjectMapping>>(
						  prepared.priorPlan,
						  &prepared.priorPlan->ownerObjectMappings);
				  publication->retainedOwnerObjectLookupIndices =
					  prepared.priorPlan->retainedOwnerObjectLookupIndices
					  ? prepared.priorPlan->retainedOwnerObjectLookupIndices
					  : std::shared_ptr<const std::vector<std::uint32_t>>(
						  prepared.priorPlan,
						  &prepared.priorPlan->ownerObjectLookupIndices);
                  publication->ownerObjectLookupMask =
                      prepared.priorPlan->ownerObjectLookupMask;
                } else {
                  std::unordered_map<LoweredOwnerKey, std::uint32_t,
                                     LoweredOwnerKeyHash>
                      loweredOwners;
                  loweredOwners.reserve(publication->plan.WorkPackets().size());
                  for (std::uint32_t packetIndex = 0U;
                       packetIndex < publication->plan.WorkPackets().size();
                       ++packetIndex) {
                    const GpuAnimationWorkPacket &packet =
                        publication->plan.WorkPackets()[packetIndex];
                    if (packet.generationRecordIndex >=
                        publication->plan.GenerationRecordCount())
                      continue;
                    const GpuAnimationGenerationRecord &generation =
                        publication->plan.GenerationRecord(
							packet.generationRecordIndex);
                    if (!IsGpuAnimationGenerationRecordValid(generation))
                      continue;
                    const LoweredOwnerKey key{packet.stableInstanceId,
                                              generation.instanceSlot,
                                              generation.instanceGeneration,
                                              generation.componentGeneration};
                    const auto [existing, inserted] =
                        loweredOwners.emplace(key, packetIndex);
                    if (!inserted) {
                      // Duplicate exact owners are ambiguous, so renderer
                      // lookup must fail closed instead of guessing one
                      // palette.
                      existing->second =
                          std::numeric_limits<std::uint32_t>::max();
                    }
                  }
                  publication->ownerObjectMappings.reserve(
                      prepared.source->ownerObjectMappings.size());
                  for (const GpuAnimationOwnerObjectMapping &mapping :
                       prepared.source->ownerObjectMappings) {
                    const auto lowered =
                        loweredOwners.find(MakeLoweredOwnerKey(mapping));
                    if (mapping.IsComplete() &&
                        lowered != loweredOwners.end() &&
                        lowered->second !=
                            std::numeric_limits<std::uint32_t>::max()) {
                      GpuAnimationOwnerObjectMapping published = mapping;
                      published.workPacketIndex = lowered->second;
                      publication->ownerObjectMappings.push_back(
                          std::move(published));
                    }
                  }
                  std::sort(publication->ownerObjectMappings.begin(),
                            publication->ownerObjectMappings.end(),
                            [](const GpuAnimationOwnerObjectMapping &left,
                               const GpuAnimationOwnerObjectMapping &right) {
                              return std::tie(left.ownerObjectId,
                                              left.componentGeneration,
                                              left.stableInstanceId,
                                              left.workPacketIndex) <
                                     std::tie(right.ownerObjectId,
                                              right.componentGeneration,
                                              right.stableInstanceId,
                                              right.workPacketIndex);
                            });
                  publication->BuildOwnerObjectLookup();
                }
              } catch (const std::bad_alloc &) {
                // An incomplete owner map cannot safely enter GPU-only
                // rendering.
                publication->ownerObjectMappings.clear();
                publication->ownerObjectLookupIndices.clear();
				publication->retainedOwnerObjectMappings.reset();
				publication->retainedOwnerObjectLookupIndices.reset();
                publication->ownerObjectLookupMask = 0U;
                publication->plan = {};
                publication->topology = {};
                publication->topologyDynamicState = {};
                publication->safeFallback = true;
              }
            }

            prepared.publication = publication;
            if (prepared.source->authority &&
                prepared.source->authority->IsValid() &&
                prepared.source->authority->authorityGeneration ==
                    prepared.sourceGeneration &&
                prepared.postPhysicsBoundary.IsValid()) {
              auto frame = std::make_shared<AnimationPresentationFrame>();
              frame->presentationGeneration = presentationGeneration;
              frame->ownerMappingSignature =
                  prepared.source->ownerMappingSignature;
              frame->postPhysicsBoundary = prepared.postPhysicsBoundary;
              frame->authority = prepared.source->authority;
              frame->gpuPlan = publication;
              frame->ownerBindings = std::move(prepared.ownerBindings);
              prepared.presentationFrame = std::move(frame);
            }
            return true;
          } catch (const std::bad_alloc &) {
            // Allocation is worker-owned.  Protected publication will retain
            // the last complete root instead of constructing a partial fallback
            // under its lock.
            prepared.publication.reset();
            prepared.presentationFrame.reset();
            return false;
          }
        }

        [[nodiscard]] std::uint32_t ResolvePlanGeneration(
		const GpuAnimationPresentationFrame& frame,
		std::uint64_t declarationEpoch) noexcept
	{
		if(frame.planGeneration != 0U) return frame.planGeneration;
		const std::uint32_t derived = static_cast<std::uint32_t>(declarationEpoch);
		// GPU ABI generation zero is invalid. A wrapped scheduler token therefore
		// uses one instead of accidentally publishing a plan the renderer rejects.
		return derived == 0U ? 1U : derived;
	}

	[[nodiscard]] std::uint64_t MixTopologyGeneration(
		std::uint64_t hash, std::uint64_t value) noexcept
	{
		// Fixed arithmetic makes retained-topology keys stable across platforms;
		// the value is an invalidation identity, never a security hash.
		hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
		return hash;
	}

	[[nodiscard]] bool BuildTopologyGenerationKey(
		GpuAnimationPlan& plan,
		GpuAnimationTopologyGenerationKey& output) noexcept
	{
		if(plan.topologyMembershipGeneration != 0U
			&& plan.topologyResourceGeneration != 0U
			&& plan.topologyProgramGeneration != 0U
			&& plan.topologySceneGeneration != 0U
			&& plan.topologyRendererGeneration != 0U) {
			output = {plan.topologyMembershipGeneration,
				plan.topologyResourceGeneration, plan.topologyProgramGeneration,
				plan.topologySceneGeneration, plan.topologyRendererGeneration};
			return output.IsComplete();
		}
		if(plan.WorkPackets().empty() || plan.GenerationRecordCount() == 0U) return false;
		GpuAnimationTopologyGenerationKey candidate{};
		candidate.membershipGeneration = 0x6a09e667f3bcc909ULL;
		candidate.resourceGeneration = 0xbb67ae8584caa73bULL;
		candidate.programGeneration = 0x3c6ef372fe94f82bULL;
		candidate.sceneGeneration = 0xa54ff53a5f1d36f1ULL;
		candidate.rendererGeneration = 0x510e527fade682d1ULL;
		for(const GpuAnimationWorkPacket& packet : plan.WorkPackets()) {
			if(packet.generationRecordIndex >= plan.GenerationRecordCount()) return false;
			const GpuAnimationGenerationRecord& generation =
				plan.GenerationRecord(packet.generationRecordIndex);
			if(!IsGpuAnimationGenerationRecordValid(generation)
				|| packet.stableInstanceId == 0U) return false;
			candidate.membershipGeneration = MixTopologyGeneration(
				candidate.membershipGeneration, packet.stableInstanceId);
			candidate.membershipGeneration = MixTopologyGeneration(
				candidate.membershipGeneration,
				(static_cast<std::uint64_t>(generation.instanceSlot) << 32U)
					| generation.instanceGeneration);
			candidate.resourceGeneration = MixTopologyGeneration(
				candidate.resourceGeneration,
				(static_cast<std::uint64_t>(generation.resourceGeneration) << 32U)
					| generation.skeletonGeneration);
			candidate.programGeneration = MixTopologyGeneration(
				candidate.programGeneration,
				(static_cast<std::uint64_t>(generation.programGeneration) << 32U)
					| generation.controllerGeneration);
			candidate.sceneGeneration = MixTopologyGeneration(
				candidate.sceneGeneration, generation.sceneGeneration);
			candidate.rendererGeneration = MixTopologyGeneration(
				candidate.rendererGeneration, generation.rendererGeneration);
		}
		if(!candidate.IsComplete()) return false;
		plan.topologyMembershipGeneration = candidate.membershipGeneration;
		plan.topologyResourceGeneration = candidate.resourceGeneration;
		plan.topologyProgramGeneration = candidate.programGeneration;
		plan.topologySceneGeneration = candidate.sceneGeneration;
		plan.topologyRendererGeneration = candidate.rendererGeneration;
		output = candidate;
		return true;
	}

	[[nodiscard]] AnimationPostPhysicsBoundary CapturePostPhysicsBoundary(
		std::uint64_t schedulerGeneration) noexcept
	{
		AnimationPostPhysicsBoundary boundary;
		boundary.schedulerGeneration = schedulerGeneration;
		const Physics::PhysicsWorldHandle activeWorld =
			Physics::PhysicsSystem::Instance().WorldHandle();
		if(!activeWorld.IsValid()) {
			// A stopped world may leave retained diagnostics or snapshots available for
			// inspection. They are not current post-Physics inputs, so the explicit
			// no-world boundary must ignore them.
			return boundary;
		}
		boundary.physicsWorldGeneration = activeWorld.worldGeneration;
		// This workload is declared after Physics. Retaining this immutable snapshot
		// gives every presentation consumer the exact post-physics value, rather
		// than a later PhysicsSystem query with a potentially different generation.
		boundary.snapshot = Physics::PhysicsSystem::Instance().LatestIncrementalBodySnapshot();
		if(boundary.snapshot && boundary.snapshot->world.IsValid()
			&& boundary.snapshot->world == activeWorld
			&& boundary.snapshot->tick != 0U
			&& boundary.snapshot->storeGeneration != 0U) {
			boundary.kind = AnimationPostPhysicsBoundaryKind::PublishedSnapshot;
			boundary.publishedTick = boundary.snapshot->tick;
			boundary.snapshotGeneration = boundary.snapshot->storeGeneration;
			return boundary;
		}
		boundary.snapshot.reset();
		// A live physics world may have no completed provider step this frame. That
		// state must fail closed for physics-dependent presentation, not masquerade
		// as a consumed snapshot or as an absent world.
		boundary.kind = AnimationPostPhysicsBoundaryKind::ActiveWorldWithoutSnapshot;
		return boundary;
	}

	AnimationPresentationSystem& AnimationPresentationSystem::Instance()
	{
		static AnimationPresentationSystem system;
		return system;
	}

	AnimationPresentationSystem::AnimationPresentationSystem()
		: impl_(std::make_unique<Impl>())
	{
	}

	AnimationPresentationSystem::~AnimationPresentationSystem() = default;

	void AnimationPresentationSystem::Initialize()
	{
		impl_->AssertCoordinatorThread();
		// SystemsBase lifecycle entry is idempotent. Reinitializing an active system
		// must not clear a plan while its public generation still identifies it.
		if(impl_->runtimeActive.exchange(true, std::memory_order_acq_rel)) return;
		if(!impl_->lifecycleLatch
			|| impl_->lifecycleLatch->Capture().Generation(AnimationLifecycleBoundary::Shutdown) == 0U) {
			impl_->lifecycleLatch = std::make_unique<AnimationGenerationLatch>();
		}
		impl_->gpuCancellationSource = AnimationCancellationSource{};
		impl_->publishedAnimatorCount.store(0U, std::memory_order_release);
		// Publication generations are process-lifetime identities. Resetting this
		// counter during a Play Mode or engine restart could make a newly published
		// plan look identical to a stale plan retained by a renderer consumer.
	}

	void AnimationPresentationSystem::Update(float)
	{
		// The scheduler-owned workload split is the only valid frame path. A legacy
		// direct call must not publish a render-facing plan outside its boundary.
	}

	void AnimationPresentationSystem::Shutdown()
	{
		impl_->AssertCoordinatorThread();
		if(!impl_->runtimeActive.exchange(false, std::memory_order_acq_rel)) return;
		if(impl_->lifecycleLatch) {
			(void)impl_->lifecycleLatch->Invalidate(AnimationLifecycleBoundary::Shutdown);
		}
		(void)impl_->gpuCancellationSource.Cancel(AnimationCancellationReason::Shutdown);
		impl_->runtimeSessionEpoch.fetch_add(1U, std::memory_order_acq_rel);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		impl_->hasActiveFrameDeclaration = false;
		impl_->publishedAnimatorCount.store(0U, std::memory_order_release);
		{
			std::scoped_lock lock(impl_->frameMutex);
			impl_->submittedFrame.reset();
			impl_->submittedSourceGeneration = 0U;
			std::atomic_store_explicit(&impl_->publishedPlan,
				std::shared_ptr<const PublishedGpuAnimationPresentationPlan>{}, std::memory_order_release);
			std::atomic_store_explicit(&impl_->publishedFrame,
				std::shared_ptr<const AnimationPresentationFrame>{}, std::memory_order_release);
		}
		// Retire the runtime session exactly once, including a session that happened
		// to be dormant. Initialize deliberately preserves this monotonic identity.
		impl_->presentationPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
	}

	std::string AnimationPresentationSystem::SystemName()
	{
		return "AnimationPresentationSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration AnimationPresentationSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = "AnimationPresentationSystem";
		declaration.phase = "AnimationPresentation";
		declaration.after = {"PhysicsSystem"};
		declaration.before = {"UISystem", "RenderSystem"};
		declaration.reads = {
			kResourceAnimationAuthorityFrame,
			kResourceAnimationFrameSidecars,
			"PhysicsPublishedSnapshots"
		};
		declaration.writes = {
			kResourceAnimationPresentationPrepared,
			kResourceAnimationPresentationFrame,
			kResourceAnimationPresentationPlan
		};
		declaration.mainThread = true;
		// Physics publishes an immutable current-step snapshot. Presentation does
		// not require live ObjectManager transforms and therefore must not force an
		// additional scene mutation flush after Physics.
		declaration.requiresFlushedSceneMutations = false;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Immutable authority sidecars and one exact physics snapshot lower into a deterministic backend-neutral presentation candidate.";
		declaration.protectedCommit = "Validates authority and physics generations, then exchanges one immutable presentation root before Render consumes it.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "AnimationPresentation";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> AnimationPresentationSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		impl_->AssertCoordinatorThread();
		// A scheduler query after Shutdown must remain inert even if AnimationSystem
		// is still active. Lifecycle ownership can only be restored by Initialize.
		if(!impl_->runtimeActive.load(std::memory_order_acquire)) return {};
		// AnimationSystem performed the frame's only store membership check. This
		// atomic value is scheduler-owned activation metadata, not live instance data.
		const std::size_t animatorCount = AnimationSystem::Instance().ScheduledAnimatorCount();
		if(animatorCount == 0U) {
			if(impl_->hasActiveFrameDeclaration) {
				// Invalidate the last active descriptor exactly once when the system
				// becomes dormant; steady static frames perform no atomic token writes.
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
				impl_->hasActiveFrameDeclaration = false;
				if(impl_->publishedAnimatorCount.exchange(0U, std::memory_order_acq_rel) != 0U) {
					// A transition to a static project retires the last render-facing plan;
					// the generation bump lets every later consumer reject that stale plan.
					impl_->presentationPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
					std::scoped_lock lock(impl_->frameMutex);
					// A static project must not leave a render-visible active plan behind.
					std::atomic_store_explicit(&impl_->publishedPlan,
						std::shared_ptr<const PublishedGpuAnimationPresentationPlan>{}, std::memory_order_release);
					std::atomic_store_explicit(&impl_->publishedFrame,
						std::shared_ptr<const AnimationPresentationFrame>{}, std::memory_order_release);
				}
			}
			return {};
		}
		// Only active descriptors need a session identity. Deferring this load keeps
		// the steady zero-animator path to activation metadata alone.
		const std::uint64_t runtimeSessionEpoch =
			impl_->runtimeSessionEpoch.load(std::memory_order_acquire);
		const std::uint64_t frameDeclarationEpoch =
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U;
		impl_->hasActiveFrameDeclaration = true;
		const auto prepared = std::make_shared<PreparedGpuAnimationPresentationFrame>();
		impl_->gpuCancellationSource = AnimationCancellationSource{};
		if(!impl_->lifecycleLatch
			|| impl_->gpuCancellationSource.Initialize(
				AnimationCancellationOperation::GpuBatch,
				*impl_->lifecycleLatch) != AnimationStatus::Ok) {
			return {};
		}
		const AnimationCancellationToken gpuCancellation =
			impl_->gpuCancellationSource.GetToken();

		std::array<FrameWorkloadDeclaration, 2> descriptors;
		FrameWorkloadDeclaration& prepare = descriptors[0];
		prepare.workloadName = "PrepareAnimationPresentation";
		prepare.ownerSystem = "AnimationPresentationSystem";
		prepare.phase = "AnimationPresentation";
		prepare.stableDescriptorId = "AnimationPresentationSystem/AnimationPresentation/PrepareAnimationPresentation";
		prepare.activationPredicateKey = "animation-active-membership";
		prepare.reads = {kResourceAnimationAuthorityFrame, kResourceAnimationFrameSidecars,
			"PhysicsPublishedSnapshots"};
		prepare.writes = {kResourceAnimationPresentationPrepared};
		prepare.estimatedWorkItemCount = animatorCount;
		prepare.inlineWorkItemThreshold = kInlinePresentationThreshold;
		// Batch compatibility sorting and checked arena allocation are one atomic
		// transaction. They run in one worker task so a partial partition can never
		// become render-visible; later planners may parallelize only the immutable
		// per-input lowering before this deterministic merge.
		prepare.partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		prepare.partitionWorkItemCount = 0U;
		prepare.workerEligible = true;
		prepare.mainThreadRequired = false;
		prepare.mutationPolicy = FrameWorkloadMutationPolicy::None;
		prepare.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		prepare.boundaryClassification = HardBoundaryClassification::WorkerSafe;
		prepare.safeSubwork = "Lowers immutable current-step program, visual, and mode packets into one owned GPU plan without live scene or renderer access.";
		prepare.profilingLabel = "AnimationPresentation.PrepareAnimationPresentation";

		FrameWorkloadDeclaration& publish = descriptors[1];
		publish.workloadName = "PublishAnimationPresentation";
		publish.ownerSystem = "AnimationPresentationSystem";
		publish.phase = "AnimationPresentation";
		publish.stableDescriptorId = "AnimationPresentationSystem/AnimationPresentation/PublishAnimationPresentation";
		publish.activationPredicateKey = "animation-active-membership";
		publish.reads = {kResourceAnimationPresentationPrepared};
		publish.writes = {kResourceAnimationPresentationFrame, kResourceAnimationPresentationPlan};
		publish.explicitDependencies = {prepare.stableDescriptorId};
		publish.estimatedWorkItemCount = animatorCount;
		publish.workerEligible = false;
		publish.mainThreadRequired = true;
		publish.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
		publish.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
                publish.boundaryClassification =
                    HardBoundaryClassification::Protected;
                publish.hardBoundary = true;
                publish.sameFrameVisibilityRequired = animatorCount != 0U;
                publish.protectedCommit =
                    "Validates the complete prepared generation and swaps one "
                    "immutable presentation plan for UI and Render.";
                publish.protectedReason = publish.protectedCommit;
                publish.profilingLabel =
                    "AnimationPresentation.PublishAnimationPresentation";

                for (FrameWorkloadDeclaration &descriptor : descriptors) {
                  descriptor.active = animatorCount != 0U;
                  descriptor.ready = true;
                }
                prepare.work = [this, runtimeSessionEpoch,
                                frameDeclarationEpoch, prepared,
                                gpuCancellation](JobGraphContext &context) {
                  if (context.IsCancellationRequested()) {
                    (void)impl_->gpuCancellationSource.Cancel(
                        AnimationCancellationReason::Explicit);
                    prepared->cancelled.store(true, std::memory_order_release);
                    return;
                  }
                  if (!impl_->runtimeActive.load(std::memory_order_acquire) ||
                      impl_->runtimeSessionEpoch.load(
                          std::memory_order_acquire) != runtimeSessionEpoch ||
                      impl_->frameDeclarationEpoch.load(
                          std::memory_order_acquire) != frameDeclarationEpoch ||
                      !impl_->lifecycleLatch ||
                      gpuCancellation.IsCancellationRequested(
                          *impl_->lifecycleLatch)) {
                    prepared->cancelled.store(true, std::memory_order_release);
                    return;
                  }
                  // Capture after Animation's dependent workloads have
                  // completed, rather than while declarations are being
                  // assembled. This makes a same-frame authority submission
                  // visible to presentation without mixing generations.
                  std::shared_ptr<const GpuAnimationPresentationFrame> source;
                  {
                    std::scoped_lock lock(impl_->frameMutex);
                    source = impl_->submittedFrame;
                  }
                  if (!source) {
                    auto fallback =
                        std::make_shared<GpuAnimationPresentationFrame>();
                    fallback->sourceGeneration = frameDeclarationEpoch;
                    source = std::move(fallback);
                  }
                  prepared->source = source;
                  prepared->sourceGeneration = source->sourceGeneration;
                  prepared->postPhysicsBoundary =
                      CapturePostPhysicsBoundary(frameDeclarationEpoch);
                  prepared->planGeneration =
                      ResolvePlanGeneration(*source, frameDeclarationEpoch);
                  // Capture both prior immutable roots once. The signature
                  // below proves that stable owner identity did not change;
                  // dynamic samples are still rebuilt.
                  const std::shared_ptr<
                      const PublishedGpuAnimationPresentationPlan>
                      previous = std::atomic_load_explicit(
                          &impl_->publishedPlan, std::memory_order_acquire);
                  const std::shared_ptr<const AnimationPresentationFrame>
                      previousFrame = std::atomic_load_explicit(
                          &impl_->publishedFrame, std::memory_order_acquire);
                  // Owner bindings are presentation-owned lookup data.  Build
                  // and sort them here while allocation is permitted; protected
                  // publication only moves the finished vector into its
                  // immutable root.
                  if (source->ownerMappingSignature != 0U && previousFrame &&
                      previousFrame->ownerMappingSignature ==
                          source->ownerMappingSignature) {
                    TERRAENGINE_PROFILE_SCOPE(
                        "Animation Presentation: Reuse Owner Bindings",
                        TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
                    prepared->ownerBindings = previousFrame->ownerBindings;
                  } else if (source->authority && source->authority->pages) {
                    TERRAENGINE_PROFILE_SCOPE(
                        "Animation Presentation: Build Owner Bindings",
                        TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
                    try {
					  auto ownerBindings = std::make_shared<
						  std::vector<AnimationPresentationFrame::OwnerBinding>>();
                      for (const auto &page : *source->authority->pages)
                        if (page) {
                          for (const AnimationAuthorityRow &row : page->rows) {
							ownerBindings->push_back(
                                {row.ownerObjectId, row.key});
                          }
                        }
                      std::sort(
						  ownerBindings->begin(),
						  ownerBindings->end(),
                          [](const auto &left, const auto &right) {
                            return std::tie(left.ownerObjectId, left.key.slot,
                                            left.key.lifetimeGeneration) <
                                   std::tie(right.ownerObjectId, right.key.slot,
                                            right.key.lifetimeGeneration);
						  });
					  prepared->ownerBindings = std::move(ownerBindings);
                    } catch (const std::bad_alloc &) {
                      prepared->cancelled.store(true,
                                                std::memory_order_release);
                      return;
                    }
                  }
                  GpuAnimationArena scratchArena(
                      GpuAnimationArenaKind::Scratch,
                      prepared->source->scratchArenaCapacityBytes);
                  GpuAnimationArena paletteArena(
                      GpuAnimationArenaKind::Palette,
                      prepared->source->paletteArenaCapacityBytes);
                  // The atomic publication remains immutable for this worker's
                  // lifetime. It supplies a topology template only; a
                  // mismatched generation, layout, payload owner, arena shape,
                  // or limit falls back to the complete builder.
                  {
                    TERRAENGINE_PROFILE_SCOPE(
                        "Animation Presentation: Lower GPU Plan",
                        TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					std::shared_ptr<const GpuAnimationPlan> previousPlanOwner;
					if (previous)
					{
						// Alias the immutable plan member while retaining the complete prior
						// publication envelope. Warm refreshes can then share topology bytes
						// without copying or weakening publication lifetime ownership.
						previousPlanOwner = std::shared_ptr<const GpuAnimationPlan>(
							previous, &previous->plan);
					}
					bool dynamicOnlyRefresh = false;
					if(prepared->source->preparedDynamicPlanComplete
						&& prepared->source->retainedStaticPlan
						&& prepared->source->retainedStaticPlan->topologyInputSignature
							== prepared->source->topologyInputSignature
						&& prepared->source->preparedDynamicLanes.HasParallelLengths(
							prepared->source->retainedStaticPlan->WorkPackets().size())) {
						// AnimationSystem authored these lanes while it was already sealing the
						// current inputs. Alias the immutable source frame for their lifetime and
						// publish a small shell around the original cold topology root. No second
						// population scan, arena transaction, or vector allocation is required.
						const GpuAnimationPlan& staticPlan =
							*prepared->source->retainedStaticPlan;
						prepared->plan.generation = prepared->planGeneration;
						prepared->plan.buildLimits = staticPlan.buildLimits;
						prepared->plan.diagnostics = staticPlan.diagnostics;
						prepared->plan.sharedResidentPayloads =
							staticPlan.sharedResidentPayloads;
						prepared->plan.topologyInputSignature =
							staticPlan.topologyInputSignature;
						prepared->plan.topologyMembershipGeneration =
							staticPlan.topologyMembershipGeneration;
						prepared->plan.topologyResourceGeneration =
							staticPlan.topologyResourceGeneration;
						prepared->plan.topologyProgramGeneration =
							staticPlan.topologyProgramGeneration;
						prepared->plan.topologySceneGeneration =
							staticPlan.topologySceneGeneration;
						prepared->plan.topologyRendererGeneration =
							staticPlan.topologyRendererGeneration;
						prepared->plan.retainedStaticPlan =
							prepared->source->retainedStaticPlan;
						prepared->plan.retainedDynamicLanes =
							std::shared_ptr<const GpuAnimationPlanDynamicLanes>(
								prepared->source,
								&prepared->source->preparedDynamicLanes);
						dynamicOnlyRefresh = true;
					} else if(previous) {
						std::scoped_lock laneLock(impl_->dynamicLanePoolMutex);
						auto& dynamicLaneScratch = impl_->AcquireDynamicLaneScratch();
						dynamicOnlyRefresh = TryRefreshLoweredGpuAnimationDynamicPlan(
							prepared->source->inputs,
							prepared->source->topologyInputSignature,
							prepared->planGeneration, scratchArena, paletteArena,
							previous->plan, prepared->plan,
							prepared->source->planLimits, previousPlanOwner,
							dynamicLaneScratch);
					}
					if(dynamicOnlyRefresh) {
						prepared->result.planStatus = GpuAnimationPlanStatus::Ok;
						prepared->result.topologyReused = true;
						prepared->result.status = GpuAnimationLoweringStatus::Ok;
					} else {
						// Cold/changed topology alone needs the atlas cache. Both locks are
						// acquired together so overlapping frames cannot invert ownership.
						std::scoped_lock cacheLocks(
							impl_->atlasCacheMutex, impl_->dynamicLanePoolMutex);
						auto& dynamicLaneScratch = impl_->AcquireDynamicLaneScratch();
						(void)BuildLoweredGpuAnimationPlan(
							prepared->source->inputs, prepared->planGeneration,
							scratchArena, paletteArena, prepared->plan,
							prepared->result, prepared->source->planLimits,
							&impl_->atlasCache,
							previous ? &previous->plan : nullptr,
							std::move(previousPlanOwner),
							prepared->source->topologyInputSignature,
							dynamicLaneScratch);
					}
                  }
                  if (prepared->result.status ==
                          GpuAnimationLoweringStatus::Ok &&
                      !prepared->plan.DispatchCommands().empty()) {
                    const auto topologyStart = std::chrono::steady_clock::now();
                    GpuAnimationTopologyGenerationKey topologyKey;
                    const bool hasExactTopologyKey =
                        BuildTopologyGenerationKey(prepared->plan, topologyKey);
                    if (hasExactTopologyKey && previous &&
                        previous->topology.IsComplete() &&
                        previous->topology.generations == topologyKey) {
                      // The prior immutable envelope is the sole retained
                      // compact topology source. Dynamic samples are rebuilt
                      // below; no worker reopens a previous mutable plan or
                      // partially refreshes owner mappings.
                      prepared->topology = previous->topology;
                      prepared->topologyReused = true;
                      prepared->priorPlan = previous;
                      prepared->ownerMappingsReusable =
                          source->ownerMappingSignature != 0U &&
                          previous->ownerMappingSignature ==
                              source->ownerMappingSignature;
                    } else if (hasExactTopologyKey) {
                      (void)BuildGpuAnimationTopology(
                          prepared->plan, topologyKey, prepared->planGeneration,
                          prepared->topology);
                    }
                    if (!prepared->topology.IsComplete() ||
                        !BuildGpuAnimationTopologyDynamicState(
                            prepared->plan, prepared->topology,
                            prepared->sourceGeneration,
                            prepared->topologyDynamicState)) {
                      // A malformed cache key or failed dynamic refresh never
                      // falls back to an older topology. Reject the whole
                      // candidate so the next capture routes every affected
                      // animator through the complete safe path.
                      prepared->plan = {};
                      prepared->result.status =
                          GpuAnimationLoweringStatus::PlanBuildFailed;
                      prepared->result.planStatus =
                          GpuAnimationPlanStatus::InvalidArgument;
                    }
                    const double topologyMilliseconds =
                        static_cast<double>(
                            std::chrono::duration_cast<
                                std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() -
                                topologyStart)
                                .count()) /
                        1'000'000.0;
                    if (prepared->topologyReused)
                      prepared->topologyRefreshMs = topologyMilliseconds;
                    else
                      prepared->topologyBuildMs = topologyMilliseconds;
                  }
                  if (gpuCancellation.IsCancellationRequested(
                          *impl_->lifecycleLatch)) {
                    prepared->plan = {};
                    prepared->cancelled.store(true, std::memory_order_release);
                  } else {
                    prepared->expectedPresentationGeneration =
                        impl_->presentationPublicationGeneration.load(
                            std::memory_order_acquire);
                    if (prepared->expectedPresentationGeneration ==
                        std::numeric_limits<std::uint64_t>::max()) {
                      prepared->cancelled.store(true,
                                                std::memory_order_release);
                    } else {
                      prepared->preservePriorPublication =
                          prepared->result.status !=
                              GpuAnimationLoweringStatus::Ok &&
                          previous != nullptr;
                      TERRAENGINE_PROFILE_SCOPE(
                          "Animation Presentation: Build Publication Roots",
                          TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
                      if (!prepared->preservePriorPublication &&
                          !BuildPreparedPresentationRoots(
                              *prepared,
                              prepared->expectedPresentationGeneration + 1U)) {
                        prepared->cancelled.store(true,
                                                  std::memory_order_release);
                      }
                    }
                  }
                  // This release is the worker-to-protected boundary for every
                  // vector in prepared. Publication reads it once and never
                  // observes partial lowering.
                  prepared->complete.store(true, std::memory_order_release);
                };
                // Protected publication validates only this frame's private preparation state.
		publish.work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch, prepared, gpuCancellation](JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				(void)impl_->gpuCancellationSource.Cancel(AnimationCancellationReason::Explicit);
				prepared->cancelled.store(true, std::memory_order_release);
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch
				|| impl_->lastPublishedFrameDeclarationEpoch == frameDeclarationEpoch
				|| !prepared->complete.load(std::memory_order_acquire)
				|| prepared->cancelled.load(std::memory_order_acquire)
				|| !impl_->lifecycleLatch
				|| gpuCancellation.IsCancellationRequested(*impl_->lifecycleLatch)) {
				return;
			}
			{
				std::scoped_lock lock(impl_->frameMutex);
				// A newer producer handoff invalidates this candidate before it becomes
				// visible. This is the final no-mixed-generation check at the boundary.
				if(impl_->submittedFrame) {
					if(impl_->submittedFrame != prepared->source
						|| impl_->submittedSourceGeneration != prepared->sourceGeneration) return;
				} else if(prepared->sourceGeneration != frameDeclarationEpoch) {
					// A producer disappeared after preparation began; do not relabel its
					// frame as an empty fallback packet at publication time.
					return;
				}
			}
			if(prepared->preservePriorPublication) {
				// Ordinary lowering failure retains the prior complete envelope. This is
				// distinct from a lifecycle invalidation, which has already cleared it.
				return;
			}
			if(!prepared->publication
				|| prepared->publication->presentationGeneration == 0U
				|| prepared->expectedPresentationGeneration
					!= impl_->presentationPublicationGeneration.load(std::memory_order_acquire)) {
				return;
			}
			// Preparation owns all allocations and population-sized work.  The protected
			// boundary accepts this exact generation, then replaces immutable roots.
			impl_->lastPublishedFrameDeclarationEpoch = frameDeclarationEpoch;
			impl_->publishedAnimatorCount.store(animatorCount, std::memory_order_release);
			impl_->presentationPublicationGeneration.store(
				prepared->publication->presentationGeneration, std::memory_order_release);
			std::atomic_store_explicit(&impl_->publishedPlan, prepared->publication,
				std::memory_order_release);
			std::atomic_store_explicit(&impl_->publishedFrame, prepared->presentationFrame,
				std::memory_order_release);
		};
		return {descriptors.begin(), descriptors.end()};
	}

	std::size_t AnimationPresentationSystem::PublishedAnimatorCount() const noexcept
	{
		return impl_->publishedAnimatorCount.load(std::memory_order_acquire);
	}

	std::uint64_t AnimationPresentationSystem::PresentationPublicationGeneration() const noexcept
	{
		return impl_->presentationPublicationGeneration.load(std::memory_order_acquire);
	}

	AnimationStatus AnimationPresentationSystem::SubmitGpuAnimationPresentationFrame(
		std::shared_ptr<const GpuAnimationPresentationFrame> frame) noexcept
	{
		if(!frame || frame->sourceGeneration == 0U) return AnimationStatus::InvalidArgument;
		std::scoped_lock lock(impl_->frameMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)) return AnimationStatus::Cancelled;
		if(frame->sourceGeneration <= impl_->submittedSourceGeneration) return AnimationStatus::StaleHandle;
		impl_->submittedSourceGeneration = frame->sourceGeneration;
		impl_->submittedFrame = std::move(frame);
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationPresentationSystem::NotifyLifecycleInvalidation(
		AnimationLifecycleBoundary boundary) noexcept
	{
		impl_->AssertCoordinatorThread();
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->lifecycleLatch) return AnimationStatus::Cancelled;
		const AnimationStatus status = impl_->lifecycleLatch->Invalidate(boundary);
		if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
			const std::uint64_t currentGeneration =
				impl_->lifecycleLatch->Capture().Generation(boundary);
			(void)impl_->gpuCancellationSource.Cancel(
				CancellationReasonForBoundary(boundary, currentGeneration));
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
			// Every lifecycle invalidation names a new scene, resource, renderer, or
			// authority epoch. Retire both topology and owner mappings as one atomic
			// envelope so neither a hot reload nor device recovery can reuse a stale
			// compact presentation while the next capture falls back to complete work.
			{
				std::scoped_lock lock(impl_->frameMutex);
				impl_->submittedFrame.reset();
				std::atomic_store_explicit(&impl_->publishedPlan,
					std::shared_ptr<const PublishedGpuAnimationPresentationPlan>{},
					std::memory_order_release);
				std::atomic_store_explicit(&impl_->publishedFrame,
					std::shared_ptr<const AnimationPresentationFrame>{}, std::memory_order_release);
			}
		}
		return status;
	}

	std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
	AnimationPresentationSystem::LoadPublishedGpuAnimationPresentationPlan() const noexcept
	{
		return std::atomic_load_explicit(&impl_->publishedPlan, std::memory_order_acquire);
	}

	std::shared_ptr<const AnimationPresentationFrame>
	AnimationPresentationSystem::LoadPublishedAnimationPresentationFrame() const noexcept
	{
		return std::atomic_load_explicit(&impl_->publishedFrame, std::memory_order_acquire);
	}

}
