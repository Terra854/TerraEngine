// TerrainStreamingSystem.h — scheduler boundary for terrain asset residency.

#pragma once

#include "Core/SystemBase.h"
#include "Terrain/TerrainTypes.h"

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Terrain
{
	// The source describes why an area is needed. Keeping it typed lets the
	// planner make edit and seam work non-evictable without special strings.
	enum class TerrainDemandSource : std::uint8_t
	{
		SceneView,
		GameView,
		Shadow,
		Reflection,
		Capture,
		Play,
		Selection,
		ActiveEdit,
		ExplicitPin
	};

	struct TerrainResidencyBudget
	{
		std::size_t authoredBytes = 256u * 1024u * 1024u;
		std::size_t renderBytes = 512u * 1024u * 1024u;
		std::size_t controlBytes = 128u * 1024u * 1024u;
		std::size_t collisionBytes = 128u * 1024u * 1024u;
	};

	struct TerrainResidencyDemand
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerrainAreaCoordinate area{};
		Generation rootGeneration = 0u;
		TerrainDemandSource source = TerrainDemandSource::SceneView;
		std::uint32_t priority = 0u;
		std::size_t authoredBytes = 0u;
		std::size_t renderBytes = 0u;
		std::size_t controlBytes = 0u;
		std::size_t collisionBytes = 0u;
		// A regional root is the only permitted visual fallback for an area.
		// It therefore remains resident while any of its children are visible.
		bool regionalRoot = false;
		bool seamDependency = false;
	};

	struct TerrainResidentArea
	{
		TerraEngine::UUID assetId = TerraEngine::EMPTY_UUID;
		TerrainAreaCoordinate area{};
		Generation rootGeneration = 0u;
		ContentHash authoredHash{};
		std::size_t authoredBytes = 0u;
		std::size_t renderBytes = 0u;
		std::size_t controlBytes = 0u;
		std::size_t collisionBytes = 0u;
		std::uint32_t priority = 0u;
		bool protectedFromEviction = false;
		bool regionalRoot = false;
	};

	struct TerrainResidencySnapshot
	{
		Generation rootGeneration = 0u;
		Generation publicationGeneration = 0u;
		TerrainResidencyBudget budget{};
		std::vector<TerrainResidentArea> areas;
		std::array<std::size_t, 4u> residentBytes{};
		std::array<std::size_t, 4u> highWaterBytes{};
		std::size_t evictions = 0u;
		std::size_t coalescedDemands = 0u;
		std::size_t cancelledDemands = 0u;
		bool hasCoherentFallbackCoverage = true;
	};

	struct TerrainResidencyPlan
	{
		Generation observedRootGeneration = 0u;
		TerrainResidencySnapshot candidate{};
		std::vector<TerrainAreaCoordinate> loadAreas;
		std::vector<TerrainAreaCoordinate> evictAreas;
		bool cancelled = false;
		std::string diagnostic;
	};

	// These value-only helpers are worker-safe.  The caller must use the
	// protected publication helper before exposing a plan to the next frame.
	TERRAENGINE_API TerrainResidencyPlan PrepareTerrainResidencyPlan(
		Generation observedRootGeneration,
		const TerrainResidencySnapshot& prior,
		std::vector<TerrainResidencyDemand> demands,
		TerrainResidencyBudget budget,
		const std::atomic_bool* cancellation = nullptr);
	TERRAENGINE_API bool PublishTerrainResidencyPlan(
		TerrainResidencySnapshot& inOutPublished,
		const TerrainResidencyPlan& plan,
		Generation currentRootGeneration);

	// This system owns demand aggregation and immutable residency snapshots. Blob
	// decode stays in TerrainAssetStore worker helpers, so this scheduler owner
	// never grants workers a mutable view of live terrain state.
	class TERRAENGINE_API TerrainStreamingSystem final : public SystemsBase
	{
	public:
		static TerrainStreamingSystem& Instance();

		void Initialize() override;
		void Update(float deltaTime) override;
		void Shutdown() override;
		std::string SystemName() override;
		TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override;
		std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float deltaTime) override;

		// Demand submission accepts copied packets from all views and editor modes.
		// It merely queues data; Update never mutates the visible snapshot.
		void SubmitDemands(std::vector<TerrainResidencyDemand> demands);
		void SetBudget(TerrainResidencyBudget budget);
		[[nodiscard]] TerrainResidencySnapshot ResidencySnapshot() const;
		[[nodiscard]] std::optional<TerrainResidencyPlan> PrepareSnapshot(
			Generation observedRootGeneration, const std::atomic_bool* cancellation = nullptr);
		[[nodiscard]] bool PublishPreparedSnapshot(const TerrainResidencyPlan& plan, Generation currentRootGeneration);

	private:
		TerrainStreamingSystem() = default;
		std::atomic<bool> initialized{ false };
		mutable std::mutex residencyMutex;
		TerrainResidencyBudget budget{};
		TerrainResidencySnapshot publishedResidency{};
		std::vector<TerrainResidencyDemand> pendingDemands;
	};
}
