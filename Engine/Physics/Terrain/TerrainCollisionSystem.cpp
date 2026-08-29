// TerrainCollisionSystem.cpp -- generation-checked terrain collision publication.

#include "Physics/Terrain/TerrainCollisionSystem.h"

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

	TerrainCollisionSystem& TerrainCollisionSystem::Instance() { static TerrainCollisionSystem instance; return instance; }
	void TerrainCollisionSystem::Initialize() { initialized.store(true, std::memory_order_release); }
	void TerrainCollisionSystem::Update(float) { /* Work is declared below; Update never mutates collision state. */ }
	void TerrainCollisionSystem::Shutdown() { initialized.store(false, std::memory_order_release); std::scoped_lock lock(mutex); prepared.reset(); desired.reset(); preparedRegions.reset(); desiredRegions.reset(); snapshot = {}; publicationStore = nullptr; commitReplacement = {}; commitRegionBatch = {}; }
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
		prepare.stableDescriptorId = "TerrainCollisionSystem/Physics/TerrainCollisionPrepare"; prepare.active = prepare.ready = active;
		auto publish = MakeProtectedCommitPhase("TerrainCollisionPublish", SystemName(), {kResourceTerrainPreparedCollision}, {kResourceTerrainPublishedCollision, kResourceTerrainDiagnostics}, "Validates a complete terrain collision candidate before PhysicsSystem simulation.", "TerrainCollision.Publish");
		publish.stableDescriptorId = "TerrainCollisionSystem/Physics/TerrainCollisionPublish"; publish.explicitDependencies = {prepare.stableDescriptorId}; publish.sameFrameVisibilityRequired = true; publish.active = publish.ready = active;
		publish.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
			const TerraEngine::Terrain::TerrainAssetStore* store = nullptr;
			{
				std::scoped_lock lock(mutex);
				store = publicationStore;
			}
			if(store != nullptr) (void)PublishBeforePhysics(*store);
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
		if(desiredRegions) preparedRegions = *desiredRegions;
		else if(desired) prepared = *desired;
		else return;
		snapshot.status = TerrainCollisionStatus::Pending;
		snapshot.message = "Terrain collision is queued automatically for the fresh physics world.";
	}
	void TerrainCollisionSystem::CancelPreparedCandidate(std::string message)
	{
		std::scoped_lock lock(mutex);
		prepared.reset();
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
		// A candidate is a one-shot immutable worker result. Clearing it prevents a
		// stale or already-committed replacement from being published every frame.
		bool published = PublishTerrainCollisionCandidate(candidateSnapshot, candidate, store);
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
		{
			std::scoped_lock lock(mutex);
			snapshot = std::move(candidateSnapshot);
		}
		return published;
	}
}
