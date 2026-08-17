// TerrainAuthoringSystem.cpp -- Protected terrain authoring publication.

#include "TerrainAuthoringSystem.h"

#include "Core/Scheduling/SystemScheduler.h"

#include <algorithm>
#include <iterator>

namespace TerraEngine::Editor::Terrain
{
	void TerrainAuthoringSystem::Configure(TerraEngine::Terrain::TerrainMutationQueue& queue, TerraEngine::Terrain::TerrainPublication& terrainPublication)
	{
		mutationQueue = &queue;
		publication = &terrainPublication;
	}

	bool TerrainAuthoringSystem::EnqueueDetachedCommit(TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError)
	{
		if(!initialized.load(std::memory_order_acquire) || mutationQueue == nullptr) { outError = "Terrain authoring is unavailable until its protected scheduler boundary is initialized."; return false; }
		std::scoped_lock lock(authoringMutex);
		if(preparedRequests.size() >= kMaximumPreparedRequests)
		{
			outError = "Terrain authoring has reached its bounded detached request capacity.";
			return false;
		}
		// Keep worker-prepared input outside the core queue until the documented
		// protected boundary. A lifecycle cancellation can therefore discard it
		// atomically without touching a live terrain root.
		preparedRequests.push_back({std::move(transaction), authoringEpoch});
		return true;
	}

	bool TerrainAuthoringSystem::EnqueueProceduralCommit(TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError)
	{
		if(transaction.historyGroup.empty())
		{
			outError = "Procedural terrain output requires one named history group.";
			return false;
		}
		// The generic typed enqueue keeps CAS observations, exact inverses, dirty
		// state, and derived invalidation identical to manual brush edits.
		return EnqueueDetachedCommit(std::move(transaction), outError);
	}

	std::size_t TerrainAuthoringSystem::CancelPreparedWork(std::string_view reason)
	{
		std::scoped_lock lock(authoringMutex);
		const std::size_t cancelledCount = preparedRequests.size();
		for(const PreparedRequest& request : preparedRequests)
		{
			TerraEngine::Terrain::TerrainTransactionReceipt receipt;
			receipt.transactionId = request.transaction.transactionId;
			receipt.outcome = TerraEngine::Terrain::TerrainEditOutcome::Cancelled;
			receipt.code = TerraEngine::Terrain::TerrainResultCode::Cancelled;
			receipt.message = std::string(reason);
			receipts.push_back(std::move(receipt));
		}
		preparedRequests.clear();
		++authoringEpoch;
		return cancelledCount;
	}

	bool TerrainAuthoringSystem::CancelPreparedTransaction(TerraEngine::UUID transactionId, std::string_view reason)
	{
		std::scoped_lock lock(authoringMutex);
		auto found = std::find_if(preparedRequests.begin(), preparedRequests.end(), [transactionId](const PreparedRequest& request) {
			return request.transaction.transactionId == transactionId;
		});
		if(found == preparedRequests.end()) return false;
		TerraEngine::Terrain::TerrainTransactionReceipt receipt;
		receipt.transactionId = transactionId;
		receipt.outcome = TerraEngine::Terrain::TerrainEditOutcome::Cancelled;
		receipt.code = TerraEngine::Terrain::TerrainResultCode::Cancelled;
		receipt.message = std::string(reason);
		receipts.push_back(std::move(receipt));
		preparedRequests.erase(found);
		return true;
	}

	void TerrainAuthoringSystem::PublishAtProtectedBoundary()
	{
		if(publication == nullptr || mutationQueue == nullptr) return;
		std::scoped_lock lock(authoringMutex);
		// The lock joins cancellation and the protected boundary: once a request
		// is handed to TerrainPublication it is a real committed/rejected edit;
		// before that point lifecycle cancellation removes it without publication.
		while(!preparedRequests.empty())
		{
			PreparedRequest request = std::move(preparedRequests.front());
			preparedRequests.pop_front();
			if(request.epoch != authoringEpoch)
			{
				TerraEngine::Terrain::TerrainTransactionReceipt receipt;
				receipt.transactionId = request.transaction.transactionId;
				receipt.outcome = TerraEngine::Terrain::TerrainEditOutcome::Cancelled;
				receipt.code = TerraEngine::Terrain::TerrainResultCode::Cancelled;
				receipt.message = "A stale terrain authoring request was cancelled before publication.";
				receipts.push_back(std::move(receipt));
				continue;
			}
			const TerraEngine::UUID transactionId = request.transaction.transactionId;
			if(const auto queued = mutationQueue->Enqueue(std::move(request.transaction)); !queued.Succeeded())
			{
				TerraEngine::Terrain::TerrainTransactionReceipt receipt;
				receipt.transactionId = transactionId;
				receipt.outcome = TerraEngine::Terrain::TerrainEditOutcome::Rejected;
				receipt.code = queued.code;
				receipt.message = queued.message;
				receipts.push_back(std::move(receipt));
			}
		}
		auto completed = publication->PublishQueued(*mutationQueue);
		receipts.insert(receipts.end(), std::make_move_iterator(completed.begin()), std::make_move_iterator(completed.end()));
	}

	std::vector<TerraEngine::Terrain::TerrainTransactionReceipt> TerrainAuthoringSystem::DrainReceipts()
	{
		std::scoped_lock lock(authoringMutex);
		std::vector<TerraEngine::Terrain::TerrainTransactionReceipt> drained;
		drained.swap(receipts);
		return drained;
	}

	void TerrainAuthoringSystem::Initialize() { initialized.store(true, std::memory_order_release); }
	void TerrainAuthoringSystem::Update(float) { /* All authoring publication is declared below. */ }
	void TerrainAuthoringSystem::Shutdown()
	{
		initialized.store(false, std::memory_order_release);
		std::scoped_lock lock(authoringMutex);
		preparedRequests.clear();
		receipts.clear();
		++authoringEpoch;
	}
	std::string TerrainAuthoringSystem::SystemName() { return "TerrainAuthoringSystem"; }

	TerraEngine::Scheduling::SystemDeclaration TerrainAuthoringSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = SystemName(); declaration.phase = "PostFrame";
		declaration.reads = { kResourceTerrainAssetRoots, kResourceTerrainPublishedResidency };
		declaration.writes = { kResourceTerrainPreparedTransactions, kResourceTerrainPublishedTransactions, kResourceTerrainDiagnostics };
		declaration.mainThread = false; declaration.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe; declaration.hardBoundary = true;
		declaration.safeSubwork = "Prepares detached brush patches from immutable root and residency captures.";
		declaration.protectedCommit = "Enqueues and publishes typed terrain transactions only after generation validation.";
		declaration.protectedReason = declaration.protectedCommit; declaration.profilingLabel = "Terrain Authoring"; declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> TerrainAuthoringSystem::DeclareFrameWorkloads(float)
	{
		using namespace TerraEngine::Scheduling;
		const bool ready = initialized.load(std::memory_order_acquire) && mutationQueue != nullptr && publication != nullptr;
		auto prepare = MakeWorkerPreparePhase("TerrainAuthoringPrepare", SystemName(),
			{ kResourceTerrainAssetRoots, kResourceTerrainPublishedResidency }, { kResourceTerrainPreparedTransactions }, "TerrainAuthoring.Prepare");
		prepare.phase = "PostFrame";
		prepare.stableDescriptorId = "TerrainAuthoringSystem/PostFrame/TerrainAuthoringPrepare";
		prepare.activationPredicateKey = "terrain-authoring-session"; prepare.safeSubwork = "Builds bounded, detached patch candidates from immutable terrain snapshots.";
		auto publish = MakeProtectedCommitPhase("TerrainAuthoringPublish", SystemName(),
			{ kResourceTerrainPreparedTransactions, kResourceTerrainPublishedTransactions }, { kResourceTerrainPublishedTransactions, kResourceTerrainDiagnostics },
			"Terrain mutation queue publication is the sole visibility boundary for editor sculpting.", "TerrainAuthoring.Publish");
		publish.phase = "PostFrame";
		publish.stableDescriptorId = "TerrainAuthoringSystem/PostFrame/TerrainAuthoringPublish"; publish.activationPredicateKey = prepare.activationPredicateKey;
		publish.explicitDependencies = { prepare.stableDescriptorId }; publish.sameFrameVisibilityRequired = true;
		// The scheduler invokes this only in its protected commit lane. Publishing
		// here keeps worker-prepared brush data detached until every transaction
		// can be revalidated atomically by TerrainPublication.
		publish.work = [this](JobGraphContext&) { PublishAtProtectedBoundary(); };
		for(auto* workload : {&prepare, &publish}) { workload->active = ready; workload->ready = ready; workload->readinessReason = ready ? std::string{} : "terrain authoring requires a configured publication service"; }
		return {std::move(prepare), std::move(publish)};
	}
}
