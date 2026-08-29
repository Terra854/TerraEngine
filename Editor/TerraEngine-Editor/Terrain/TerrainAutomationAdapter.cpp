// TerrainAutomationAdapter.cpp -- Generation-safe automation preflight.
#include "TerrainAutomationAdapter.h"
namespace TerraEngine::Editor::Terrain
{
	void TerrainAutomationAdapter::Configure(TerraEngine::Terrain::TerrainAssetStore& value, TerrainAuthoringSystem& system) { store=&value; authoring=&system; }
	void TerrainAutomationAdapter::SetPrepareCallback(std::function<std::optional<TerraEngine::Terrain::TerrainEditTransaction>(const Automation::TerrainAutomationRequest&, std::string&)> callback) { prepare=std::move(callback); }
	void TerrainAutomationAdapter::SetCommandCallback(std::function<TerrainAutomationCommandResult(std::string_view, const nlohmann::json&)> callback) { commandCallback=std::move(callback); }
	void TerrainAutomationAdapter::SetTypedOperationCallback(std::function<TerrainAutomationCommandResult(std::string_view, const Automation::TerrainAutomationRequest&)> callback) { typedOperationCallback=std::move(callback); }
	TerrainAutomationPreflight TerrainAutomationAdapter::Preflight(const Automation::TerrainAutomationRequest& request) const
	{
		if(!IsConfigured()) return {false,"Unavailable","Terrain automation is unavailable until an editor terrain session is open."};
		const auto capture=store->CaptureRoot();
		if(!capture.IsValid() || capture.root->assetUuid!=request.assetId) return {false,"StaleGeneration","The requested terrain asset is not the active authored root."};
		if(TerraEngine::Terrain::ToHexString(capture.root->rootContentHash)!=request.expectedRootHash) return {false,"StaleGeneration","The expected terrain root hash is stale."};
		if(request.observations.size()>request.budget.maximumAreas) return {false,"BudgetExceeded","Observed terrain areas exceed the declared budget."};
		for(const auto& observation:request.observations)
		{
			const auto areas=TerraEngine::Terrain::TerrainAreasForSamples(capture.root->authoredBounds);
			if(!areas || !areas->Contains(observation.coordinate)) return {false,"StaleGeneration","A required terrain area observation is outside the authored bounds."};
			const auto descriptor=store->ResolveDescriptorOrImplicit(*capture.root,observation.coordinate);
			if(descriptor.editGeneration!=observation.editGeneration || descriptor.authoredContentHash!=observation.authoredContentHash) return {false,"StaleGeneration","A required terrain area observation is stale or unavailable."};
		}
		const std::string fingerprint=Automation::TerrainApprovalFingerprint(request);
		if(!request.previewOnly && request.approvalFingerprint!=fingerprint) return {false,"ApprovalRequired","Commit requires the matching protected preflight fingerprint."};
		return {true,{},"Preflight accepted.",fingerprint,capture};
	}
	std::optional<TerraEngine::Terrain::TerrainRootCapture> TerrainAutomationAdapter::Inspect() const
	{
		if(!IsConfigured()) return std::nullopt;
		const auto capture = store->CaptureRoot();
		return capture.IsValid() ? std::optional<TerraEngine::Terrain::TerrainRootCapture>(capture) : std::nullopt;
	}
	bool TerrainAutomationAdapter::Enqueue(const Automation::TerrainAutomationRequest& request, TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError)
	{
		const auto preflight=Preflight(request); if(!preflight.accepted) { outError=preflight.message; return false; }
		if(request.previewOnly) { outError="Preview-only terrain automation cannot enter the publication queue."; return false; }
		transaction.origin=TerraEngine::Terrain::TerrainEditOrigin::Automation; transaction.targetAssetUuid=request.assetId; transaction.inputRootHash=preflight.capture.root->rootContentHash;
		transaction.affectedBounds=request.bounds; transaction.budget=request.budget; transaction.readSet=request.observations;
		if(transaction.historyGroup.empty()) transaction.historyGroup="Terrain automation " + request.operation;
		return authoring->EnqueueProceduralCommit(std::move(transaction),outError);
	}
	bool TerrainAutomationAdapter::PrepareAndEnqueue(const Automation::TerrainAutomationRequest& request, std::string jobId, std::string& outError)
	{
		if(!prepare) { outError="This terrain operation has no editor-owned detached preview producer."; return false; }
		auto transaction=prepare(request,outError); if(!transaction) return false;
		const auto transactionId=transaction->transactionId;
		if(!Enqueue(request,std::move(*transaction),outError)) return false;
		pendingJobs.emplace(transactionId,PendingJob{std::move(jobId),request}); return true;
	}
	TerrainAutomationCommandResult TerrainAutomationAdapter::DispatchCommand(std::string_view capability, const nlohmann::json& arguments) const
	{
		return commandCallback ? commandCallback(capability, arguments)
			: TerrainAutomationCommandResult{false, "Unavailable", "The terrain editor command service is not configured."};
	}
	TerrainAutomationCommandResult TerrainAutomationAdapter::DispatchTypedOperation(std::string_view capability, const Automation::TerrainAutomationRequest& request) const
	{
		return typedOperationCallback ? typedOperationCallback(capability, request)
			: TerrainAutomationCommandResult{false, "Unavailable", "The terrain editor typed operation service is not configured."};
	}
	void TerrainAutomationAdapter::TrackPendingTransaction(TerraEngine::UUID transactionId, std::string jobId, const Automation::TerrainAutomationRequest& request)
	{
		pendingJobs.insert_or_assign(transactionId, PendingJob{std::move(jobId), request});
	}
	void TerrainAutomationAdapter::SetReceiptCallback(std::function<void(std::string_view,const TerraEngine::Terrain::TerrainTransactionReceipt&,const Automation::TerrainAutomationRequest&)> callback) { receiptCallback=std::move(callback); }
	void TerrainAutomationAdapter::ObserveReceipt(const TerraEngine::Terrain::TerrainTransactionReceipt& receipt)
	{
		const auto found=pendingJobs.find(receipt.transactionId); if(found==pendingJobs.end()) return;
		PendingJob job=std::move(found->second); pendingJobs.erase(found);
		if(receiptCallback) receiptCallback(job.id,receipt,job.request);
	}
}
