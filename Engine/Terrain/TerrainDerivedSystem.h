// TerrainDerivedSystem.h — scheduler boundary for regenerable terrain products.

#pragma once

#include "Core/SystemBase.h"
#include "Terrain/TerrainAssetStore.h"

#include <atomic>
#include <string>
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

	private:
		TerrainDerivedSystem() = default;
		std::atomic<bool> initialized{ false };
	};
}
