// TerrainCollisionSystem.cpp -- generation-checked terrain collision publication.

#include "Physics/Terrain/TerrainCollisionSystem.h"

#include "Physics/PhysicsSystem.h"

#include <algorithm>
#include <utility>

namespace TerraEngine::Physics::Terrain
{
	TerrainCollisionCandidate PrepareTerrainCollisionCandidate(TerrainHeightFieldRecipe recipe, TerraEngine::Terrain::ContentHash sourceRootHash, TerraEngine::Terrain::Generation sourcePublicationGeneration, const std::atomic_bool* cancellation)
	{
		TerrainCollisionCandidate candidate{};
		candidate.recipe = std::move(recipe); candidate.sourceRootHash = sourceRootHash;
		candidate.sourcePublicationGeneration = sourcePublicationGeneration; candidate.cancellation = cancellation;
		candidate.complete = ValidateTerrainHeightFieldRecipe(candidate.recipe).accepted && sourceRootHash.IsValid() && sourcePublicationGeneration != 0u;
		return candidate;
	}

	MountedTerrainCollisionCandidate PrepareMountedTerrainCollisionCandidate(
		TerrainHeightFieldRecipe recipe, TerraEngine::Terrain::ContentHash sourceRootHash,
		TerraEngine::Terrain::Generation sourcePublicationGeneration)
	{
		MountedTerrainCollisionCandidate candidate{};
		candidate.recipe = std::move(recipe);
		candidate.sourceRootHash = sourceRootHash;
		candidate.sourcePublicationGeneration = sourcePublicationGeneration;
		candidate.complete = ValidateTerrainHeightFieldRecipe(candidate.recipe).accepted
			&& candidate.recipe.terrainAssetId != TerraEngine::EMPTY_UUID
			&& candidate.sourceRootHash.IsValid()
			&& candidate.sourcePublicationGeneration != 0u;
		return candidate;
	}

	bool PublishTerrainCollisionCandidate(TerrainCollisionSnapshot& inOutSnapshot, const TerrainCollisionCandidate& candidate, const TerraEngine::Terrain::TerrainAssetStore& store)
	{
		if (candidate.cancellation != nullptr && candidate.cancellation->load(std::memory_order_acquire))
		{
			inOutSnapshot.status = TerrainCollisionStatus::Cancelled;
			inOutSnapshot.message = "Terrain collision worker result was cancelled; prior collision remains active.";
			return false;
		}
		const auto root = store.CaptureRoot();
		if (!root.IsValid() || root.root->rootContentHash != candidate.sourceRootHash || root.publicationGeneration != candidate.sourcePublicationGeneration)
		{
			inOutSnapshot.status = TerrainCollisionStatus::Stale;
			inOutSnapshot.message = "Terrain collision worker result observed a stale authored root; last-known-good collision remains active.";
			return false;
		}
		if (!candidate.complete || !ValidateTerrainHeightFieldRecipe(candidate.recipe).accepted)
		{
			inOutSnapshot.status = TerrainCollisionStatus::Failed;
			inOutSnapshot.message = "Terrain collision recipe failed provider-neutral validation.";
			return false;
		}
		if(inOutSnapshot.currentRecipe.IsComplete()) {
			inOutSnapshot.lastKnownGoodRecipe = inOutSnapshot.currentRecipe;
			inOutSnapshot.lastKnownGoodSourceRootHash = inOutSnapshot.sourceRootHash;
			inOutSnapshot.lastKnownGoodSourcePublicationGeneration = inOutSnapshot.sourcePublicationGeneration;
		}
		inOutSnapshot.currentRecipe = candidate.recipe;
		inOutSnapshot.sourceRootHash = candidate.sourceRootHash;
		inOutSnapshot.sourcePublicationGeneration = candidate.sourcePublicationGeneration;
		inOutSnapshot.ageMilliseconds = 0.0;
		inOutSnapshot.buildLatencyMilliseconds = candidate.buildLatencyMilliseconds;
		inOutSnapshot.status = TerrainCollisionStatus::Current;
		inOutSnapshot.message = "Terrain collision replacement is current at the protected before-physics boundary.";
		return true;
	}

	bool PublishTerrainCollisionRegionBatch(TerrainCollisionSnapshot& inOutSnapshot,
		const TerrainCollisionRegionBatch& batch,
		const TerraEngine::Terrain::TerrainAssetStore& store,
		bool providerBatchAccepted)
	{
		if(batch.cancellation != nullptr && batch.cancellation->load(std::memory_order_acquire)) return false;
		const auto root = store.CaptureRoot();
		if(!root.IsValid() || batch.assetId != root.root->assetUuid
			|| batch.sourceRootHash != root.root->rootContentHash
			|| batch.sourcePublicationGeneration != root.publicationGeneration
			|| !batch.complete || !providerBatchAccepted) return false;
		for(std::size_t index = 0u; index < batch.upserts.size(); ++index)
		{
			const auto& region = batch.upserts[index];
			if(!ValidateTerrainHeightFieldRecipe(region.recipe).accepted
				|| region.recipe.terrainAssetId != batch.assetId
				|| region.recipe.authoredGeneration != batch.sourcePublicationGeneration
				|| (index > 0u && !(batch.upserts[index - 1u].coordinate < region.coordinate))) return false;
		}
		if(!std::is_sorted(batch.removals.begin(), batch.removals.end())
			|| std::adjacent_find(batch.removals.begin(), batch.removals.end()) != batch.removals.end()) return false;
		for(const auto& region : batch.upserts)
			if(std::binary_search(batch.removals.begin(), batch.removals.end(), region.coordinate)) return false;
		// The candidate map stays detached until every recipe and the provider batch
		// have passed. A rejection therefore cannot leak a partial regional set.
		auto candidateRegions = inOutSnapshot.regions;
		for(const auto coordinate : batch.removals) candidateRegions.erase(coordinate);
		for(const auto& region : batch.upserts) candidateRegions.insert_or_assign(region.coordinate, region.recipe);
		inOutSnapshot.regions = std::move(candidateRegions);
		inOutSnapshot.sourceRootHash = batch.sourceRootHash;
		inOutSnapshot.sourcePublicationGeneration = batch.sourcePublicationGeneration;
		inOutSnapshot.status = TerrainCollisionStatus::Current;
		inOutSnapshot.message = "Terrain collision regional batch is current at the protected before-physics boundary.";
		return true;
	}

	bool PublishMountedTerrainCollisionCandidate(
		TerrainCollisionSnapshot& inOutSnapshot, const MountedTerrainCollisionCandidate& candidate,
		const std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)>& commit)
	{
		if(!candidate.complete || !candidate.sourceRootHash.IsValid()
			|| candidate.sourcePublicationGeneration == 0u
			|| !ValidateTerrainHeightFieldRecipe(candidate.recipe).accepted) {
			inOutSnapshot.status = TerrainCollisionStatus::Failed;
			inOutSnapshot.message = "Mounted terrain collision candidate failed immutable recipe validation; last-known-good remains active.";
			return false;
		}
		if(!commit) {
			inOutSnapshot.status = TerrainCollisionStatus::Failed;
			inOutSnapshot.message = "Mounted terrain collision candidate has no protected physics committer.";
			return false;
		}

		TerrainCollisionReplacementPacket packet{};
		packet.id = "mounted-terrain-collision-"
			+ std::to_string(candidate.sourcePublicationGeneration);
		packet.sourceRootHash = candidate.sourceRootHash;
		packet.sourcePublicationGeneration = candidate.sourcePublicationGeneration;
		packet.recipe = candidate.recipe;
		const TerrainCollisionReplacementReceipt receipt = commit(std::move(packet));
		if(!receipt.committed) {
			// The provider rejected the candidate, so retain every current/last-known
			// recipe field. Only diagnostics change to describe the failed attempt.
			inOutSnapshot.status = TerrainCollisionStatus::Failed;
			inOutSnapshot.message = receipt.message.empty()
				? "The active physics backend rejected the mounted terrain collision replacement; last-known-good remains active."
				: receipt.message;
			return false;
		}

		TerrainCollisionSnapshot committedSnapshot = inOutSnapshot;
		if(committedSnapshot.currentRecipe.IsComplete()) {
			committedSnapshot.lastKnownGoodRecipe = committedSnapshot.currentRecipe;
			committedSnapshot.lastKnownGoodSourceRootHash = committedSnapshot.sourceRootHash;
			committedSnapshot.lastKnownGoodSourcePublicationGeneration = committedSnapshot.sourcePublicationGeneration;
		}
		committedSnapshot.currentRecipe = candidate.recipe;
		committedSnapshot.sourceRootHash = candidate.sourceRootHash;
		committedSnapshot.sourcePublicationGeneration = candidate.sourcePublicationGeneration;
		committedSnapshot.ageMilliseconds = 0.0;
		committedSnapshot.buildLatencyMilliseconds = 0.0;
		committedSnapshot.status = TerrainCollisionStatus::Current;
		committedSnapshot.message = "Mounted terrain collision replacement is current at the protected before-physics boundary.";
		inOutSnapshot = std::move(committedSnapshot);
		return true;
	}

	TerrainCollisionSystem& TerrainCollisionSystem::Instance() { static TerrainCollisionSystem instance; return instance; }
	void TerrainCollisionSystem::Initialize() { initialized.store(true, std::memory_order_release); }
	void TerrainCollisionSystem::Update(float) { /* Work is declared below; Update never mutates collision state. */ }
	void TerrainCollisionSystem::Shutdown() { initialized.store(false, std::memory_order_release); std::scoped_lock lock(mutex); prepared.reset(); desired.reset(); mountedPrepared.clear(); mountedDesired.clear(); preparedRegions.reset(); desiredRegions.reset(); snapshot = {}; publicationStore = nullptr; commitReplacement = {}; commitRegionBatch = {}; mountedCommitter = {}; }
	std::string TerrainCollisionSystem::SystemName() { return "TerrainCollisionSystem"; }
	TerraEngine::Scheduling::SystemDeclaration TerrainCollisionSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration{}; declaration.name = SystemName(); declaration.phase = "Physics";
		declaration.before = {"PhysicsSystem"};
		declaration.reads = {kResourceTerrainAssetRoots, kResourceTerrainPublishedDerived};
		declaration.writes = {kResourceTerrainPreparedCollision, kResourceTerrainPublishedCollision, kResourceTerrainDiagnostics};
		declaration.mainThread = false; declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe; declaration.hardBoundary = true;
		declaration.safeSubwork = "Builds immutable 257-grid terrain heightfield candidates from captured terrain roots.";
		declaration.protectedCommit = "Publishes only a current complete terrain collision replacement before physics simulation; stale work retains last-known-good.";
		declaration.protectedReason = declaration.protectedCommit; declaration.profilingLabel = "Terrain Collision"; declaration.usesDefaultMetadata = false;
		return declaration;
	}
	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> TerrainCollisionSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		const bool active = initialized.load(std::memory_order_acquire);
		auto prepare = MakeWorkerPreparePhase("TerrainCollisionPrepare", SystemName(), {kResourceTerrainAssetRoots, kResourceTerrainPublishedDerived}, {kResourceTerrainPreparedCollision}, "TerrainCollision.Prepare");
		prepare.stableDescriptorId = "TerrainCollisionSystem/Physics/TerrainCollisionPrepare";
		// Helper factories describe worker/protected kind, but the scheduler executes
		// only named engine phases. Collision publication belongs before PhysicsSystem
		// in the real Physics phase, never in a synthetic helper phase.
		prepare.phase = "Physics";
		prepare.activationPredicateKey = "terrain-collision-lifecycle";
		prepare.active = prepare.ready = active;
		auto publish = MakeProtectedCommitPhase("TerrainCollisionPublish", SystemName(), {kResourceTerrainPreparedCollision}, {kResourceTerrainPublishedCollision, kResourceTerrainDiagnostics}, "Validates a complete terrain collision candidate before PhysicsSystem simulation.", "TerrainCollision.Publish");
		publish.stableDescriptorId = "TerrainCollisionSystem/Physics/TerrainCollisionPublish";
		publish.phase = "Physics";
		publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.explicitDependencies = {prepare.stableDescriptorId}; publish.sameFrameVisibilityRequired = true; publish.active = publish.ready = active;
		publish.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
			const TerraEngine::Terrain::TerrainAssetStore* store = nullptr;
			{
				std::scoped_lock lock(mutex);
				store = publicationStore;
			}
			if(store != nullptr) (void)PublishBeforePhysics(*store);
			else (void)PublishMountedRuntimeBeforePhysics();
		};
		return {std::move(prepare), std::move(publish)};
	}
	TerrainCollisionSnapshot TerrainCollisionSystem::Snapshot() const { std::scoped_lock lock(mutex); return snapshot; }
	void TerrainCollisionSystem::ConfigurePublication(
		const TerraEngine::Terrain::TerrainAssetStore* store,
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit,
		std::function<bool(const TerrainCollisionRegionBatch&)> commitRegions)
	{
		std::scoped_lock lock(mutex);
		publicationStore = store;
		commitReplacement = std::move(commit);
		commitRegionBatch = std::move(commitRegions);
	}
	void TerrainCollisionSystem::ClearPublication(const TerraEngine::Terrain::TerrainAssetStore* store) noexcept
	{
		std::scoped_lock lock(mutex);
		if(publicationStore != store) return;
		prepared.reset();
		desired.reset();
		preparedRegions.reset();
		desiredRegions.reset();
		publicationStore = nullptr;
		commitReplacement = {};
		commitRegionBatch = {};
	}
	void TerrainCollisionSystem::SetPreparedCandidate(TerrainCollisionCandidate candidate)
	{
		std::scoped_lock lock(mutex);
		// Keep one immutable desired copy in addition to the one-shot prepared work.
		// Typed vectors own their samples, so this copy cannot observe later editor
		// mutations and is safe to reuse after a physics-world generation change.
		desired = candidate;
		prepared = std::move(candidate);
		snapshot.status = TerrainCollisionStatus::Pending;
	}
	bool TerrainCollisionSystem::SetMountedRuntimeCandidate(MountedTerrainCollisionCandidate candidate)
	{
		if(!candidate.complete || !candidate.sourceRootHash.IsValid()
			|| candidate.sourcePublicationGeneration == 0u
			|| !ValidateTerrainHeightFieldRecipe(candidate.recipe).accepted) {
			return false;
		}
		std::scoped_lock lock(mutex);
		// Runtime package values are already copied from the mounted archive. Keep
		// every area keyed by immutable asset/area identity for a later physics-world
		// recreation, never a mount or ECS reference that could outlive this engine
		// session. Startup can legitimately stage multiple mounted terrain assets.
		const TerraEngine::UUID assetId = candidate.recipe.terrainAssetId;
		const TerraEngine::Terrain::TerrainAreaCoordinate area{
			candidate.recipe.areaX, candidate.recipe.areaZ};
		mountedDesired[assetId].insert_or_assign(area, candidate);
		mountedPrepared[assetId].insert_or_assign(area, std::move(candidate));
		++snapshot.mountedCandidatesStaged;
		snapshot.mountedPendingCandidateCount = static_cast<std::uint32_t>(std::accumulate(
			mountedPrepared.begin(), mountedPrepared.end(), std::size_t{0u},
			[](const std::size_t count, const auto& asset) { return count + asset.second.size(); }));
		snapshot.status = TerrainCollisionStatus::Pending;
		snapshot.message = "Mounted terrain collision is queued for protected physics publication.";
		return true;
	}

	void TerrainCollisionSystem::ConfigureMountedRuntimeCommitter(
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit)
	{
		std::scoped_lock lock(mutex);
		mountedCommitter = std::move(commit);
	}

	bool TerrainCollisionSystem::PublishMountedRuntimeBeforePhysics()
	{
		MountedTerrainCollisionCandidates candidates;
		TerrainCollisionSnapshot candidateSnapshot{};
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit;
		{
			std::scoped_lock lock(mutex);
			if(mountedPrepared.empty()) return false;
			candidates = std::move(mountedPrepared);
			mountedPrepared.clear();
			candidateSnapshot = snapshot;
			commit = mountedCommitter;
		}
		if(!commit) {
			commit = [](TerrainCollisionReplacementPacket packet) {
				return TerraEngine::Physics::PhysicsSystem::Instance()
					.CommitTerrainCollisionReplacement(std::move(packet));
			};
		}
		const std::size_t candidateCount = std::accumulate(candidates.begin(), candidates.end(), std::size_t{0u},
			[](const std::size_t count, const auto& asset) { return count + asset.second.size(); });
		candidateSnapshot.mountedPendingCandidateCount = 0u;
		candidateSnapshot.mountedPublicationAttempts += candidateCount;
		bool published = true;
		for(const auto& [assetId, areas] : candidates)
		{
			(void)assetId;
			for(const auto& [area, candidate] : areas)
			{
				(void)area;
				// Each provider replacement is individually atomic. Continue after a
				// rejection so unrelated mounted assets cannot be dropped because one
				// package recipe is invalid; the snapshot records the most recent
				// result while the backend retains every previously committed area.
				const bool candidatePublished = PublishMountedTerrainCollisionCandidate(
					candidateSnapshot, candidate, commit);
				if(candidatePublished) ++candidateSnapshot.mountedPublicationCommits;
				else ++candidateSnapshot.mountedPublicationRejections;
				published = candidatePublished && published;
			}
		}
		{
			std::scoped_lock lock(mutex);
			snapshot = std::move(candidateSnapshot);
		}
		return published;
	}
	void TerrainCollisionSystem::SetPreparedRegionBatch(TerrainCollisionRegionBatch batch)
	{
		std::scoped_lock lock(mutex);
		desiredRegions = batch;
		preparedRegions = std::move(batch);
		snapshot.status = TerrainCollisionStatus::Pending;
		snapshot.message = "Terrain collision regional batch is queued for protected publication.";
	}
	void TerrainCollisionSystem::RequestRepublish()
	{
		std::scoped_lock lock(mutex);
		if(!mountedDesired.empty()) {
			mountedPrepared = mountedDesired;
			snapshot.mountedPendingCandidateCount = static_cast<std::uint32_t>(std::accumulate(
				mountedPrepared.begin(), mountedPrepared.end(), std::size_t{0u},
				[](const std::size_t count, const auto& asset) { return count + asset.second.size(); }));
		}
		else if(desiredRegions) preparedRegions = *desiredRegions;
		else if(desired) prepared = *desired;
		else return;
		snapshot.status = TerrainCollisionStatus::Pending;
		snapshot.message = "Terrain collision is queued automatically for the fresh physics world.";
	}
	void TerrainCollisionSystem::CancelPreparedCandidate(std::string message)
	{
		std::scoped_lock lock(mutex);
		prepared.reset();
		mountedPrepared.clear();
		snapshot.mountedPendingCandidateCount = 0u;
		preparedRegions.reset();
		snapshot.status = TerrainCollisionStatus::Cancelled;
		snapshot.message = std::move(message);
	}
	bool TerrainCollisionSystem::PublishBeforePhysics(const TerraEngine::Terrain::TerrainAssetStore& store) {
		TerrainCollisionRegionBatch regionalBatch;
		bool hasRegionalBatch = false;
		TerrainCollisionCandidate candidate;
		TerrainCollisionSnapshot candidateSnapshot;
		std::function<TerrainCollisionReplacementReceipt(TerrainCollisionReplacementPacket)> commit;
		std::function<bool(const TerrainCollisionRegionBatch&)> commitRegions;
		{
			std::scoped_lock lock(mutex);
			if(preparedRegions)
			{
				regionalBatch = std::move(*preparedRegions);
				preparedRegions.reset();
				hasRegionalBatch = true;
			}
			else
			{
				if(!prepared) return false;
				candidate = std::move(*prepared);
				prepared.reset();
			}
			candidateSnapshot = snapshot;
			commit = commitReplacement;
			commitRegions = commitRegionBatch;
		}
		if(hasRegionalBatch)
		{
			TerrainCollisionSnapshot validatedSnapshot = candidateSnapshot;
			bool published = PublishTerrainCollisionRegionBatch(validatedSnapshot, regionalBatch, store, true);
			if(published && commitRegions) published = commitRegions(regionalBatch);
			if(published) candidateSnapshot = std::move(validatedSnapshot);
			else
			{
				candidateSnapshot.status = TerrainCollisionStatus::Failed;
				candidateSnapshot.message = "Terrain collision regional batch was rejected; last-known-good regions remain active.";
			}
			std::scoped_lock lock(mutex);
			snapshot = std::move(candidateSnapshot);
			return published;
		}
		// A candidate is a one-shot immutable worker result. Validate it into a
		// detached copy first: a provider rejection must not expose an uncommitted
		// recipe through the collision snapshot.
		TerrainCollisionSnapshot validatedSnapshot = candidateSnapshot;
		bool published = PublishTerrainCollisionCandidate(validatedSnapshot, candidate, store);
		if(published && commit)
		{
			TerrainCollisionReplacementPacket packet;
			packet.id = "terrain-collision-" + std::to_string(candidate.sourcePublicationGeneration);
			packet.sourceRootHash = candidate.sourceRootHash;
			packet.sourcePublicationGeneration = candidate.sourcePublicationGeneration;
			packet.recipe = candidate.recipe;
			const auto receipt = commit(std::move(packet));
			if(!receipt.committed)
			{
				published = false;
				candidateSnapshot.status = TerrainCollisionStatus::Failed;
				candidateSnapshot.message = receipt.message.empty()
					? "The active physics backend rejected terrain collision replacement; last-known-good remains active."
					: receipt.message;
			}
		}
		if(published) candidateSnapshot = std::move(validatedSnapshot);
		{
			std::scoped_lock lock(mutex);
			snapshot = std::move(candidateSnapshot);
		}
		return published;
	}
}
