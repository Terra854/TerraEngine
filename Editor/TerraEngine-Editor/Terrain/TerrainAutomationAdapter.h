// TerrainAutomationAdapter.h -- Narrow automation bridge to the editor boundary.
#pragma once
#include "Editor/TerraEngine-Editor/Automation/TerrainAutomationSchema.h"
#include "TerrainAuthoringSystem.h"
#include "Terrain/TerrainAssetStore.h"
#include <functional>
#include <unordered_map>

namespace TerraEngine::Editor::Terrain
{
	struct TerrainAutomationPreflight { bool accepted=false; std::string category,message,fingerprint; TerraEngine::Terrain::TerrainRootCapture capture{}; };
	struct TerrainAutomationCommandResult
	{
		bool accepted = false;
		std::string category;
		std::string message;
		std::optional<TerraEngine::UUID> transactionId;
		nlohmann::json result = nlohmann::json::object();
	};
	// This adapter deliberately accepts only a fully typed transaction from an
	// editor preview producer. JSON callers cannot mutate a live terrain root.
	class TerrainAutomationAdapter
	{
	public:
		void Configure(TerraEngine::Terrain::TerrainAssetStore& store, TerrainAuthoringSystem& authoring);
		void SetPrepareCallback(std::function<std::optional<TerraEngine::Terrain::TerrainEditTransaction>(const Automation::TerrainAutomationRequest&, std::string&)> callback);
		void SetCommandCallback(std::function<TerrainAutomationCommandResult(std::string_view, const nlohmann::json&)> callback);
		void SetTypedOperationCallback(std::function<TerrainAutomationCommandResult(std::string_view, const Automation::TerrainAutomationRequest&)> callback);
		[[nodiscard]] TerrainAutomationPreflight Preflight(const Automation::TerrainAutomationRequest& request) const;
		// Read-only inspection returns a copied immutable root capture. Automation
		// clients never receive the mutable store or authoring-system internals.
		[[nodiscard]] std::optional<TerraEngine::Terrain::TerrainRootCapture> Inspect() const;
		[[nodiscard]] bool Enqueue(const Automation::TerrainAutomationRequest& request, TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError);
		[[nodiscard]] bool PrepareAndEnqueue(const Automation::TerrainAutomationRequest& request, std::string jobId, std::string& outError);
		[[nodiscard]] TerrainAutomationCommandResult DispatchCommand(std::string_view capability, const nlohmann::json& arguments) const;
		[[nodiscard]] TerrainAutomationCommandResult DispatchTypedOperation(std::string_view capability, const Automation::TerrainAutomationRequest& request) const;
		void TrackPendingTransaction(TerraEngine::UUID transactionId, std::string jobId, const Automation::TerrainAutomationRequest& request);
		void SetReceiptCallback(std::function<void(std::string_view, const TerraEngine::Terrain::TerrainTransactionReceipt&, const Automation::TerrainAutomationRequest&)> callback);
		void ObserveReceipt(const TerraEngine::Terrain::TerrainTransactionReceipt& receipt);
		[[nodiscard]] bool IsConfigured() const noexcept { return store != nullptr && authoring != nullptr; }
	private:
		struct PendingJob { std::string id; Automation::TerrainAutomationRequest request; };
		TerraEngine::Terrain::TerrainAssetStore* store=nullptr; TerrainAuthoringSystem* authoring=nullptr;
		std::function<std::optional<TerraEngine::Terrain::TerrainEditTransaction>(const Automation::TerrainAutomationRequest&, std::string&)> prepare;
		std::function<TerrainAutomationCommandResult(std::string_view, const nlohmann::json&)> commandCallback;
		std::function<TerrainAutomationCommandResult(std::string_view, const Automation::TerrainAutomationRequest&)> typedOperationCallback;
		std::function<void(std::string_view,const TerraEngine::Terrain::TerrainTransactionReceipt&,const Automation::TerrainAutomationRequest&)> receiptCallback;
		std::unordered_map<TerraEngine::UUID,PendingJob> pendingJobs;
	};
}
