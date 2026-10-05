// TerrainIndependentCopyService.h -- Durable copies owned through scene receipts.
#pragma once

#include "Scene/TransactionalMutation.h"
#include "Terrain/TerrainTypes.h"
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Terrain { struct TerrainAssetRoot; }
namespace TerraEngine::Editor::Terrain {
	struct TerrainCopyCapture {
		std::uint64_t projectEpoch = 0U;
		TerraEngine::UUID selectedObjectId{}, sourceAssetId{};
		std::filesystem::path sourceDescriptorPath, assetRoot, privateStagingRoot;
		TerraEngine::Terrain::ContentHash sourceRootHash{};
		TerraEngine::Scene::TransactionReadSet observedReadSet;
		std::string descriptorBytes;
	};
	struct TerrainCopyOwnership {
		std::uint64_t projectEpoch = 0U;
		TerraEngine::UUID operationId{}, freshAssetId{};
		std::filesystem::path descriptorPath, dataDirectory, stagingDirectory;
	};
	struct TerrainCopyPrepared {
		TerrainCopyOwnership ownership;
		TerrainCopyCapture sourceCapture;
		std::shared_ptr<const TerraEngine::Terrain::TerrainAssetRoot> root;
		std::filesystem::path stagedDescriptor, stagedData;
	};
	enum class TerrainCopyCleanupDisposition { Complete, Retry, OwnershipLost };
	struct TerrainCopyCleanupResult {
		TerrainCopyCleanupDisposition disposition = TerrainCopyCleanupDisposition::Complete;
		std::string diagnostic;
	};
	struct TerrainCopyCallbacks {
		// All callbacks run on the protected owner. Rollback verifies the exact UUID
		// and path before unregistering and must never null unrelated scene references.
		std::function<bool(TerraEngine::UUID)> identityAvailable;
		std::function<bool(const TerrainCopyCapture&, std::string&)> validateSource;
		std::function<bool(const TerrainCopyPrepared&, const TerrainCopyOwnership&, std::string&)> registerProvisional;
		std::function<void(const TerrainCopyOwnership&)> finalizeProvisional;
		std::function<TerrainCopyCleanupResult(const TerrainCopyOwnership&)> rollbackProvisional;
	};
	enum class TerrainCopyState { Preparing, RebindPending, Committed, Cancelled, Failed, CleanupPending, AwaitingReceipt, FinalizePending };
	struct TerrainCopyStatus {
		TerraEngine::UUID selectedObjectId{}, operationId{}, freshAssetId{};
		TerrainCopyState state = TerrainCopyState::Preparing;
		std::string transactionId, diagnostic;
	};
	struct TerrainCopySubmission { bool accepted = false; TerraEngine::UUID operationId{}; std::string message; };
	class TerrainIndependentCopyService {
	public:
		explicit TerrainIndependentCopyService(TerrainCopyCallbacks callbacks);
		~TerrainIndependentCopyService();
		TerrainIndependentCopyService(const TerrainIndependentCopyService&) = delete;
		TerrainIndependentCopyService& operator=(const TerrainIndependentCopyService&) = delete;
		// Capture/Begin/Poll/receipts are owner-only. Workers own detached roots and
		// private files; only bool futures escape the submitting task handle.
		[[nodiscard]] TerrainCopySubmission Begin(TerrainCopyCapture capture);
		void PollPrepared(std::uint64_t currentProjectEpoch);
		[[nodiscard]] bool HandleTransactionResult(const TerraEngine::Scene::TransactionResult& result);
		void Cancel(TerraEngine::UUID operationId);
		void RetireProject(std::uint64_t projectEpoch);
		void RetryCleanup(std::uint64_t currentProjectEpoch);
		[[nodiscard]] std::vector<TerrainCopyStatus> SnapshotStatus() const;
		// Called only at explicit teardown while the old project's owners still
		// exist. False means a receipt/cleanup obligation remains; never Clear first.
		[[nodiscard]] bool DrainAtShutdown();
#ifdef TERRAENGINE_EDITOR_TESTS
		enum class Boundary { AfterStageReservation, AfterOwnerMarkerCreation, BeforePreparation, BeforePersistence, BeforeEnqueue, BeforeCleanup };
		using TestHook = std::function<void(Boundary)>;
		void SetTestHook(TestHook hook);
#endif
	private:
		struct Impl;
		std::unique_ptr<Impl> implementation;
	};
}
