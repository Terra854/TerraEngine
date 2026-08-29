// TerrainDerivedSystem.h — scheduler boundary for regenerable terrain products.

#pragma once

#include "Core/SystemBase.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainEditTransaction.h"
#include "Terrain/TerrainMaterial.h"
#include "Terrain/TerrainPublication.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace TerraEngine::Terrain
{
	struct TerrainDerivedCandidate
	{
		TerraEngine::UUID assetUuid = TerraEngine::EMPTY_UUID;
		ContentHash sourceRootHash{};
		Generation sourcePublicationGeneration = 0u;
		bool complete = false;
		const std::atomic_bool* cancellation = nullptr;
	};

	// This result is intentionally value-only so worker batches can check their
	// captured source again immediately before a protected consumer publishes.
	struct TerrainDerivedPublicationCheck
	{
		TerrainResultCode code = TerrainResultCode::Ok;
		std::string message;
		[[nodiscard]] bool Succeeded() const noexcept { return code == TerrainResultCode::Ok; }
	};

	// Geometry, holes, and surface controls share one derived barrier. A partial
	// update is rejected so a visible frame cannot pair new material weights with
	// old mesh coverage (or vice versa).
	struct TerrainDerivedSurfaceBatch
	{
		TerrainDerivedCandidate source{};
		ContentHash geometryIdentity{};
		ContentHash holeIdentity{};
		ContentHash surfaceControlIdentity{};
		Generation derivedGeneration = 0u;
		bool geometryComplete = false;
		bool holesComplete = false;
		bool surfaceControlsComplete = false;
	};
	TERRAENGINE_API TerrainDerivedPublicationCheck ValidateTerrainDerivedSurfaceBatch(
		const TerrainDerivedSurfaceBatch& batch, const TerrainAssetStore& store);

	TERRAENGINE_API TerrainDerivedPublicationCheck ValidateTerrainDerivedCandidate(
		const TerrainDerivedCandidate& candidate, const TerrainAssetStore& store);

	struct TerrainLayerMaterialPhase
	{
		std::uint16_t stableLayerId = 0u;
		TerrainAreaMaterialPhase phase{};
	};

	// The worker candidate contains no renderer handles or mutable component
	// references. A protected boundary can reject it solely from root identity.
	struct TerrainMaterialMappingCandidate
	{
		TerrainDerivedCandidate source{};
		TerrainDerivedInvalidation invalidation{};
		std::vector<TerrainLayerMaterialPhase> layerPhases;
		bool mappingComplete = false;
	};

	struct TerrainDerivedPublicationReceipt
	{
		TerrainDerivedInvalidation invalidation{};
		TerrainResultCode code = TerrainResultCode::Ok;
		bool committed = false;
		bool materialBindingsPublished = false;
		std::string message;
	};

	TERRAENGINE_API TerrainDerivedPublicationCheck ValidateTerrainMaterialMappingCandidate(
		const TerrainMaterialMappingCandidate& candidate, const TerrainAssetStore& store);

	// Render, collision, and surface products are derived from authored terrain.
	// This system keeps their worker build and protected publication phases visible.
	class TERRAENGINE_API TerrainDerivedSystem final : public SystemsBase
	{
	public:
		static TerrainDerivedSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;
		// Receipts are enqueued only after authored publication. The immutable root
		// capture lets the following worker phase prepare without live ECS access.
		[[nodiscard]] bool EnqueueInvalidation(
			TerrainDerivedInvalidation invalidation, TerrainAssetStore& store);
		[[nodiscard]] std::optional<TerrainMaterialMappingCandidate> ResolvePublishedMaterialCandidate(
			TerraEngine::UUID assetUuid) const;
		// A candidate is queued from worker-safe subsystem observations and becomes
		// visible only in the system's protected publish workload after revalidation.
		[[nodiscard]] bool EnqueueCommonReadyCandidate(
			TerrainCommonReadyCandidate candidate, TerrainAssetStore& store);
		[[nodiscard]] std::optional<TerrainCommonReadyCandidate> ResolveCommonReadyCandidate(
			TerraEngine::UUID assetUuid) const;
		[[nodiscard]] std::vector<TerrainDerivedPublicationReceipt> DrainPublicationReceipts();

	private:
		TerrainDerivedSystem() = default;
		struct PendingInvalidation
		{
			TerrainDerivedInvalidation invalidation{};
			TerrainRootCapture capture{};
			TerrainAssetStore* store = nullptr;
		};
		struct PreparedCandidate
		{
			TerrainMaterialMappingCandidate candidate{};
			TerrainAssetStore* store = nullptr;
		};

		void PrepareInvalidationUnion();
		void PublishPreparedAtProtectedBoundary();

		std::atomic<bool> initialized{ false };
		std::atomic<bool> cancelRequested{ false };
		mutable std::mutex queueMutex;
		std::vector<PendingInvalidation> pendingInvalidations;
		std::vector<PreparedCandidate> preparedCandidates;
		struct PendingCommonReady
		{
			TerrainCommonReadyCandidate candidate{};
			TerrainAssetStore* store = nullptr;
		};
		std::vector<PendingCommonReady> pendingCommonReady;
		std::vector<TerrainDerivedPublicationReceipt> publicationReceipts;
		std::unordered_map<TerraEngine::UUID, TerrainMaterialMappingCandidate> publishedMaterialCandidates;
		std::unordered_map<TerraEngine::UUID, TerrainCommonReadyCandidate> publishedCommonReadyCandidates;
	};
}
