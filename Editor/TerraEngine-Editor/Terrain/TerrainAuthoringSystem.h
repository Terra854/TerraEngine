// TerrainAuthoringSystem.h -- Scheduler-owned bridge from previews to terrain publication.

#pragma once

#include "Core/SystemBase.h"
#include "Terrain/TerrainMutationQueue.h"
#include "Terrain/TerrainPublication.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Terrain
{
	class TerrainAuthoringSystem final : public SystemsBase
	{
	public:
		// The editor owns neither store nor publication; the runtime coordinator
		// injects these long-lived services during setup.
		void Configure(TerraEngine::Terrain::TerrainMutationQueue& queue, TerraEngine::Terrain::TerrainPublication& publication);
		[[nodiscard]] bool EnqueueDetachedCommit(TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError);
		// Procedural and spline jobs use the same protected path as brush strokes.
		// A named history group makes their many worker batches one undoable edit.
		[[nodiscard]] bool EnqueueProceduralCommit(TerraEngine::Terrain::TerrainEditTransaction transaction, std::string& outError);
		// Cancels requests that have not entered the protected publication lane.
		// A monotonically increasing epoch prevents stale worker output from a
		// previous scene/tool/project session from crossing that later boundary.
		std::size_t CancelPreparedWork(std::string_view reason);
		// Cancels one queued detached request without affecting other terrain tools.
		// This keeps a layer-list Cancel from discarding a concurrently prepared stroke.
		bool CancelPreparedTransaction(TerraEngine::UUID transactionId, std::string_view reason);
		// MainEditorSubsystem calls this at its documented protected authoring
		// boundary. It is also the callback installed in the scheduler workload.
		void PublishAtProtectedBoundary();
		[[nodiscard]] std::vector<TerraEngine::Terrain::TerrainTransactionReceipt> DrainReceipts();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

	private:
		struct PreparedRequest
		{
			TerraEngine::Terrain::TerrainEditTransaction transaction;
			std::uint64_t epoch = 0U;
		};

		TerraEngine::Terrain::TerrainMutationQueue* mutationQueue = nullptr;
		TerraEngine::Terrain::TerrainPublication* publication = nullptr;
		std::atomic<bool> initialized{false};
		std::mutex authoringMutex;
		std::deque<PreparedRequest> preparedRequests;
		std::vector<TerraEngine::Terrain::TerrainTransactionReceipt> receipts;
		std::uint64_t authoringEpoch = 1U;
		static constexpr std::size_t kMaximumPreparedRequests = 256U;
	};
}
