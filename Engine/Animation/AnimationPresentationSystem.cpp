// AnimationPresentationSystem.cpp - worker preparation and protected plan swap.

#include "AnimationPresentationSystem.h"

#include "AnimationSystem.h"
#include "Core/Scheduling/JobGraph.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <functional>
#include <mutex>
#include <new>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace TerraEngine::Animation {
	void PublishedGpuAnimationPresentationPlan::BuildOwnerObjectLookup() noexcept
	{
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
		if(ownerObjectId == EMPTY_UUID || ownerObjectMappings.empty()) {
			return nullptr;
		}
		if(!ownerObjectLookupIndices.empty()
			&& ownerObjectLookupIndices.size()
				== static_cast<std::size_t>(ownerObjectLookupMask) + 1U) {
			const std::uint32_t emptyIndex = std::numeric_limits<std::uint32_t>::max();
			std::uint32_t bucket = static_cast<std::uint32_t>(
				std::hash<UUID>{}(ownerObjectId)) & ownerObjectLookupMask;
			for(std::size_t probe = 0U; probe < ownerObjectLookupIndices.size(); ++probe) {
				const std::uint32_t storedIndex = ownerObjectLookupIndices[bucket];
				if(storedIndex == emptyIndex) {
					return nullptr;
				}
				if(storedIndex < ownerObjectMappings.size()
					&& ownerObjectMappings[storedIndex].ownerObjectId == ownerObjectId) {
					return &ownerObjectMappings[storedIndex];
				}
				bucket = (bucket + 1U) & ownerObjectLookupMask;
			}
			return nullptr;
		}

		const auto mapping = std::lower_bound(
			ownerObjectMappings.begin(), ownerObjectMappings.end(), ownerObjectId,
			[](const GpuAnimationOwnerObjectMapping& candidate, const UUID& value) {
				return candidate.ownerObjectId < value;
			});
		return mapping != ownerObjectMappings.end()
			&& mapping->ownerObjectId == ownerObjectId ? &*mapping : nullptr;
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
		// The immutable combined atlas normally survives many dynamic sample ticks.
		// Preparation owns this cache under a narrow mutex so future overlapping
		// frame production cannot race a relocation-layout replacement.
		mutable std::mutex atlasCacheMutex;
		GpuAnimationFrameAtlasCache atlasCache;
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
		std::uint64_t sourceGeneration = 0U;
		std::uint32_t planGeneration = 0U;
		std::atomic_bool complete = false;
		std::atomic_bool cancelled = false;
	};

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

	struct LoweredOwnerKey {
		std::uint64_t stableInstanceId = 0U;
		std::uint32_t instanceSlot = 0U;
		std::uint32_t instanceGeneration = 0U;
		std::uint32_t componentGeneration = 0U;

		bool operator==(const LoweredOwnerKey&) const noexcept = default;
	};

	struct LoweredOwnerKeyHash {
		std::size_t operator()(const LoweredOwnerKey& value) const noexcept
		{
			// Generation lanes are already well distributed counters. Combining them
			// with the stable 64-bit identity gives average O(1) owner validation.
			std::size_t hash = std::hash<std::uint64_t>{}(value.stableInstanceId);
			hash ^= std::hash<std::uint32_t>{}(value.instanceSlot) + 0x9e3779b9U
				+ (hash << 6U) + (hash >> 2U);
			hash ^= std::hash<std::uint32_t>{}(value.instanceGeneration) + 0x9e3779b9U
				+ (hash << 6U) + (hash >> 2U);
			hash ^= std::hash<std::uint32_t>{}(value.componentGeneration) + 0x9e3779b9U
				+ (hash << 6U) + (hash >> 2U);
			return hash;
		}
	};

	[[nodiscard]] LoweredOwnerKey MakeLoweredOwnerKey(
		const GpuAnimationOwnerObjectMapping& mapping) noexcept
	{
		return {mapping.stableInstanceId, mapping.instanceSlot,
			mapping.instanceGeneration, mapping.componentGeneration};
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
			kResourceAnimationVisualPackets,
			kResourceAnimationEvaluationPrograms,
			kResourceAnimationExecutionModes,
			kResourceAnimationRuntimeState,
			"PhysicsPublishedSnapshots"
		};
		declaration.writes = {
			kResourceAnimationRagdollMappings,
			kResourceAnimationRecordingSamples,
			kResourceAnimationPresentationPrepared,
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
		declaration.safeSubwork = "Immutable visual/program/mode and physics snapshot lowering into a deterministic render-neutral presentation plan.";
		declaration.protectedCommit = "Complete generation-validated presentation plan publication before UI and Render consume it.";
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
		prepare.reads = {kResourceAnimationVisualPackets, kResourceAnimationEvaluationPrograms,
			kResourceAnimationExecutionModes, kResourceAnimationRuntimeState, "PhysicsPublishedSnapshots"};
		prepare.writes = {kResourceAnimationRagdollMappings, kResourceAnimationRecordingSamples, kResourceAnimationPresentationPrepared};
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
		publish.reads = {kResourceAnimationRagdollMappings, kResourceAnimationRecordingSamples, kResourceAnimationPresentationPrepared};
		publish.writes = {kResourceAnimationPresentationPlan};
		publish.explicitDependencies = {prepare.stableDescriptorId};
		publish.estimatedWorkItemCount = animatorCount;
		publish.workerEligible = false;
		publish.mainThreadRequired = true;
		publish.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
		publish.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		publish.boundaryClassification = HardBoundaryClassification::Protected;
		publish.hardBoundary = true;
		publish.sameFrameVisibilityRequired = animatorCount != 0U;
		publish.protectedCommit = "Validates the complete prepared generation and swaps one immutable presentation plan for UI and Render.";
		publish.protectedReason = publish.protectedCommit;
		publish.profilingLabel = "AnimationPresentation.PublishAnimationPresentation";

		for(FrameWorkloadDeclaration& descriptor : descriptors) {
			descriptor.active = animatorCount != 0U;
			descriptor.ready = true;
		}
		prepare.work = [this, runtimeSessionEpoch, frameDeclarationEpoch, prepared, gpuCancellation](JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				(void)impl_->gpuCancellationSource.Cancel(AnimationCancellationReason::Explicit);
				prepared->cancelled.store(true, std::memory_order_release);
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch
				|| !impl_->lifecycleLatch
				|| gpuCancellation.IsCancellationRequested(*impl_->lifecycleLatch)) {
				prepared->cancelled.store(true, std::memory_order_release);
				return;
			}
			// Capture after Animation's dependent workloads have completed, rather
			// than while declarations are being assembled. This makes a same-frame
			// authority submission visible to presentation without mixing generations.
			std::shared_ptr<const GpuAnimationPresentationFrame> source;
			{
				std::scoped_lock lock(impl_->frameMutex);
				source = impl_->submittedFrame;
			}
			if(!source) {
				auto fallback = std::make_shared<GpuAnimationPresentationFrame>();
				fallback->sourceGeneration = frameDeclarationEpoch;
				source = std::move(fallback);
			}
			prepared->source = source;
			prepared->sourceGeneration = source->sourceGeneration;
			prepared->planGeneration = ResolvePlanGeneration(*source, frameDeclarationEpoch);
			GpuAnimationArena scratchArena(
				GpuAnimationArenaKind::Scratch, prepared->source->scratchArenaCapacityBytes);
			GpuAnimationArena paletteArena(
				GpuAnimationArenaKind::Palette, prepared->source->paletteArenaCapacityBytes);
			// The atomic publication remains immutable for this worker's lifetime.
			// It supplies a topology template only; a mismatched generation, layout,
			// payload owner, arena shape, or limit falls back to the complete builder.
			const std::shared_ptr<const PublishedGpuAnimationPresentationPlan> previous =
				std::atomic_load_explicit(&impl_->publishedPlan, std::memory_order_acquire);
			{
				std::scoped_lock cacheLock(impl_->atlasCacheMutex);
				(void)BuildLoweredGpuAnimationPlan(
					prepared->source->inputs, prepared->planGeneration, scratchArena, paletteArena,
					prepared->plan, prepared->result, prepared->source->planLimits,
					&impl_->atlasCache, previous ? &previous->plan : nullptr);
			}
			if(gpuCancellation.IsCancellationRequested(*impl_->lifecycleLatch)) {
				prepared->plan = {};
				prepared->cancelled.store(true, std::memory_order_release);
			}
			// This release is the worker-to-protected boundary for every vector in
			// prepared. Publication reads it once and never observes partial lowering.
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
			const bool buildSucceeded = prepared->result.status == GpuAnimationLoweringStatus::Ok;
			if(!buildSucceeded && std::atomic_load_explicit(&impl_->publishedPlan, std::memory_order_acquire)) {
				// Preserve a known-good atomic envelope when a fresh GPU plan is stale,
				// malformed, or out of arena capacity; never blend its vectors with this frame.
				return;
			}
			auto publication = std::make_shared<PublishedGpuAnimationPresentationPlan>();
			publication->presentationGeneration = impl_->presentationPublicationGeneration.load(std::memory_order_relaxed) + 1U;
			publication->sourceGeneration = prepared->sourceGeneration;
			publication->loweringStatus = prepared->result.status;
			publication->planStatus = prepared->result.planStatus;
			// Empty work is the explicit CPU/no-GPU result for missing capability or
			// residency input. It must retire an older GPU plan instead of leaving it
			// visible under a newer source generation.
			publication->safeFallback = !buildSucceeded || prepared->plan.dispatchCommands.empty();
			if(buildSucceeded) {
				publication->plan = std::move(prepared->plan);
				try {
					std::unordered_map<LoweredOwnerKey, std::uint32_t,
						LoweredOwnerKeyHash> loweredOwners;
					loweredOwners.reserve(publication->plan.workPackets.size());
					for(std::uint32_t packetIndex = 0U;
						packetIndex < publication->plan.workPackets.size(); ++packetIndex) {
						const GpuAnimationWorkPacket& packet =
							publication->plan.workPackets[packetIndex];
						if(packet.generationRecordIndex
							>= publication->plan.generationRecords.size()) continue;
						const GpuAnimationGenerationRecord& generation =
							publication->plan.generationRecords[packet.generationRecordIndex];
						if(!IsGpuAnimationGenerationRecordValid(generation)) continue;
						const LoweredOwnerKey key{packet.stableInstanceId,
							generation.instanceSlot, generation.instanceGeneration,
							generation.componentGeneration};
						const auto [existing, inserted] =
							loweredOwners.emplace(key, packetIndex);
						if(!inserted) {
							// Duplicate exact owner identities are ambiguous. Mark the key invalid
							// so Render never guesses which palette belongs to an authored object.
							existing->second = std::numeric_limits<std::uint32_t>::max();
						}
					}
					publication->ownerObjectMappings.reserve(prepared->source->ownerObjectMappings.size());
					for(const GpuAnimationOwnerObjectMapping& mapping : prepared->source->ownerObjectMappings) {
						// Publish only a complete, generation-equal mapping that names an
						// actual lowered packet. A rejected/missing packet must not leave
						// render collection an object identity it could accidentally reuse.
						const auto lowered = loweredOwners.find(MakeLoweredOwnerKey(mapping));
						if(mapping.IsComplete() && lowered != loweredOwners.end()
							&& lowered->second != std::numeric_limits<std::uint32_t>::max()) {
							GpuAnimationOwnerObjectMapping published = mapping;
							published.workPacketIndex = lowered->second;
							publication->ownerObjectMappings.push_back(std::move(published));
						}
					}
					// Binary search over a retained immutable vector is cheaper than rebuilding
					// two hash tables in RenderSystem. Secondary identities keep duplicates
					// adjacent so the renderer can preserve its fail-closed ambiguity check.
					std::sort(publication->ownerObjectMappings.begin(),
						publication->ownerObjectMappings.end(),
						[](const GpuAnimationOwnerObjectMapping& left,
							const GpuAnimationOwnerObjectMapping& right) {
							return std::tie(left.ownerObjectId, left.componentGeneration,
								left.stableInstanceId, left.workPacketIndex)
								< std::tie(right.ownerObjectId, right.componentGeneration,
									right.stableInstanceId, right.workPacketIndex);
						});
					publication->BuildOwnerObjectLookup();
				} catch(const std::bad_alloc&) {
					// An incomplete owner map cannot safely support GPU-only rendering.
					// Retire this candidate as a CPU fallback instead of publishing a
					// plan whose packets cannot be tied to their originating objects.
					publication->ownerObjectMappings.clear();
					publication->ownerObjectLookupIndices.clear();
					publication->ownerObjectLookupMask = 0U;
					publication->plan = {};
					publication->safeFallback = true;
				}
			}
			impl_->lastPublishedFrameDeclarationEpoch = frameDeclarationEpoch;
			impl_->publishedAnimatorCount.store(animatorCount, std::memory_order_release);
			impl_->presentationPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
			std::shared_ptr<const PublishedGpuAnimationPresentationPlan> immutablePublication = publication;
			std::atomic_store_explicit(&impl_->publishedPlan, immutablePublication, std::memory_order_release);
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
		}
		return status;
	}

	std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
	AnimationPresentationSystem::LoadPublishedGpuAnimationPresentationPlan() const noexcept
	{
		return std::atomic_load_explicit(&impl_->publishedPlan, std::memory_order_acquire);
	}
}
