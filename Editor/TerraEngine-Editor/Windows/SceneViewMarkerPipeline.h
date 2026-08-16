// SceneViewMarkerPipeline.h - Typed, incremental Scene View marker preparation.
//
// The interactive Scene View owns this cache, but worker-safe preparation only
// reads immutable values passed to it. No function in this file mutates a live
// scene, registry, hierarchy, or renderer resource.

#pragma once

#include "SceneViewOverlayTypes.h"

#include "Profiling/PerformanceTelemetry.h"
#include "Scene/SceneChangePacket.h"
#include "Scene/SpatialQuery.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <entt/entity/entity.hpp>
#include <glm/glm.hpp>

namespace TerraEngine::Editor::SceneViewOverlay
{
	using MarkerStringId = std::uint32_t;
	inline constexpr MarkerStringId kInvalidMarkerStringId = 0u;

	// MarkerStaticRecord deliberately excludes world position. Renaming an
	// object therefore changes only stable presentation data, while physics
	// movement can update the much smaller MarkerPoseRecord independently.
	struct MarkerStaticRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint64_t objectLifetimeGeneration = 0u;
		entt::entity entityReference = entt::null;
		std::uint64_t componentGeneration = 0u;
		std::uint64_t metadataGeneration = 0u;
		MarkerCategory category = MarkerCategory::Unsupported;
		MarkerStringId displayNameId = kInvalidMarkerStringId;
		MarkerStringId tooltipId = kInvalidMarkerStringId;
		std::uint32_t basePriority = 0u;
		bool alwaysReserved = false;
	};

	// MarkerStaticInput is the protected-boundary input used to intern strings
	// and update stable records without exposing mutable editor-owned storage.
	struct MarkerStaticInput
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint64_t objectLifetimeGeneration = 0u;
		entt::entity entityReference = entt::null;
		std::uint64_t componentGeneration = 0u;
		std::uint64_t metadataGeneration = 0u;
		MarkerCategory category = MarkerCategory::Unsupported;
		std::string displayName;
		std::string tooltip;
		bool selected = false;
	};

	// Pose records retain exact hierarchy evidence so a hierarchy-root change
	// can invalidate only that root and its cached descendants.
	struct MarkerPoseRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentObjectId = TerraEngine::EMPTY_UUID;
		std::uint64_t objectLifetimeGeneration = 0u;
		std::uint64_t transformRevision = 0u;
		std::uint64_t hierarchyGeneration = 0u;
		std::uint64_t parentWorldGeneration = 0u;
		// The canonical address remains exact while projection consumes only this
		// marker frame's bounded local value.
		TerraEngine::Scene::WorldPosition canonicalPosition{};
		glm::vec3 localPosition = glm::vec3(0.0f);
		std::optional<TerraEngine::Scene::SpatialAABB> localBounds;
		bool movable = false;
	};

	// Pointer-cell identity is optional because ordinary drawing can reuse the
	// same screen index without rebuilding it for every mouse pixel.
	struct MarkerPointerCell
	{
		std::int32_t x = 0;
		std::int32_t y = 0;

		bool operator==(const MarkerPointerCell&) const = default;
	};

	struct MarkerViewportKey
	{
		std::uint64_t viewGeneration = 0u;
		std::uint64_t projectionGeneration = 0u;
		glm::ivec4 viewportRectangle = glm::ivec4(0);
		ProjectionMode projectionMode = ProjectionMode::Perspective;
		std::uint64_t overlayPreferenceGeneration = 0u;
		std::uint64_t markerSpatialPublicationGeneration = 0u;
		std::uint64_t markerPosePublicationGeneration = 0u;
		std::optional<MarkerPointerCell> pointerCell;

		bool operator==(const MarkerViewportKey& other) const;
		[[nodiscard]] bool SameProjectionInputs(const MarkerViewportKey& other) const;
	};

	struct MarkerCandidateSet
	{
		MarkerViewportKey viewportKey;
		std::vector<TerraEngine::UUID> reservedMarkerIds;
		std::vector<TerraEngine::UUID> generalCandidateIds;
		std::uint32_t sourceCount = 0u;
		std::uint32_t frustumRejectedCount = 0u;
		std::uint32_t distanceRejectedCount = 0u;
		std::uint32_t sizeRejectedCount = 0u;
		std::uint32_t budgetRejectedCount = 0u;
		std::uint32_t effectiveBudget = 0u;
		bool effectiveLargeSceneMode = false;
	};

	struct MarkerScreenEntry
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		glm::vec2 screenPosition = glm::vec2(0.0f);
		float depth = 0.0f;
		float distance = 0.0f;
		float projectedSizePixels = 0.0f;
		std::uint32_t priority = 0u;
		MarkerCategory category = MarkerCategory::Unsupported;
		MarkerStringId displayNameId = kInvalidMarkerStringId;
		MarkerStringId tooltipId = kInvalidMarkerStringId;
		TerraEngine::UUID hitTestRepresentative = TerraEngine::EMPTY_UUID;
		bool reserved = false;
		bool densityCandidate = false;
		bool clusterCandidate = false;
	};

	// Cells store contiguous ranges into entries. Hit testing can binary-search
	// only cells touched by the pointer radius instead of scanning every marker.
	struct MarkerScreenCell
	{
		std::int32_t x = 0;
		std::int32_t y = 0;
		std::size_t firstEntry = 0u;
		std::size_t entryCount = 0u;
	};

	struct MarkerScreenIndex
	{
		MarkerViewportKey viewportKey;
		float cellSizePixels = 18.0f;
		std::vector<MarkerScreenCell> cells;
		std::vector<MarkerScreenEntry> entries;
	};

	enum class MarkerPrimitiveTier : std::uint8_t
	{
		FullIcon,
		SimpleIcon,
		Point,
		Cluster
	};

	struct MarkerPresentationItem
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::uint64_t clusterId = 0u;
		std::uint32_t clusterMemberCount = 1u;
		std::vector<TerraEngine::UUID> expandableMemberIds;
		MarkerPrimitiveTier primitiveTier = MarkerPrimitiveTier::FullIcon;
		TerraEngine::UUID hitTestRepresentative = TerraEngine::EMPTY_UUID;
		std::uint32_t deterministicSelectionOrder = 0u;
		glm::vec2 screenPosition = glm::vec2(0.0f);
		float depth = 0.0f;
		MarkerCategory category = MarkerCategory::Unsupported;
		MarkerStringId displayNameId = kInvalidMarkerStringId;
		MarkerStringId tooltipId = kInvalidMarkerStringId;
		bool reserved = false;
	};

	struct MarkerPresentationBatch
	{
		std::uint64_t batchGeneration = 0u;
		std::size_t cursor = 0u;
		std::vector<MarkerPresentationItem> items;
	};

	// Spatial hits come from the immutable editor-selectable spatial snapshot.
	// The pipeline then performs cheap exact clip, distance, and projected-size
	// rejection before it constructs any interaction or drawing records.
	struct MarkerViewportQuery
	{
		MarkerViewportKey viewportKey;
		glm::mat4 viewProjection = glm::mat4(1.0f);
		glm::vec3 cameraPosition = glm::vec3(0.0f);
		glm::vec2 viewportPosition = glm::vec2(0.0f);
		glm::vec2 viewportSize = glm::vec2(0.0f);
		TerraEngine::Scene::SpatialQueryResult spatialCandidates;
	};

	struct MarkerExactChangeSet
	{
		std::uint64_t sceneGeneration = 0u;
		std::vector<TerraEngine::UUID> addedObjectIds;
		std::vector<TerraEngine::UUID> removedObjectIds;
		std::vector<TerraEngine::UUID> metadataChangedObjectIds;
		std::vector<TerraEngine::UUID> componentChangedObjectIds;
		std::vector<TerraEngine::UUID> transformChangedObjectIds;
		std::vector<TerraEngine::UUID> hierarchyChangedObjectIds;
		std::vector<TerraEngine::UUID> boundsChangedObjectIds;
	};

	struct MarkerInvalidationResult
	{
		std::vector<TerraEngine::UUID> staticRefreshObjectIds;
		std::vector<TerraEngine::UUID> poseRefreshObjectIds;
		std::vector<TerraEngine::UUID> removedObjectIds;
		bool sceneReplaced = false;
	};

	struct MarkerFrameResult
	{
		MarkerCandidateSet candidates;
		MarkerScreenIndex screenIndex;
		std::vector<MarkerPresentationBatch> presentationBatches;
		std::vector<TerraEngine::Profiling::PerformanceStageRecord> stageTelemetry;
		bool reusedViewportCache = false;
	};

	struct MarkerHitTestResult
	{
		std::optional<TerraEngine::UUID> objectId;
		TerraEngine::Profiling::PerformanceStageRecord telemetry;
	};

	class MarkerPipeline
	{
	public:
		// Upserts reject stale lifetime/generation data. Callers can safely retry
		// the latest protected snapshot after a worker result loses a generation race.
		[[nodiscard]] bool UpsertStaticRecord(const MarkerStaticInput& input);
		[[nodiscard]] bool UpsertPoseRecord(const MarkerPoseRecord& pose);
		[[nodiscard]] MarkerInvalidationResult ApplySceneChanges(const TerraEngine::Scene::SceneChangePacket& packet);
		[[nodiscard]] MarkerInvalidationResult ApplyExactChanges(const MarkerExactChangeSet& changes);
		void Clear();

		[[nodiscard]] MarkerFrameResult PrepareViewport(const MarkerViewportQuery& query, const SceneOverlayPreference& preference);
		[[nodiscard]] MarkerHitTestResult HitTest(const MarkerScreenIndex& index, const glm::vec2& pointer, float radiusPixels = 12.0f) const;

		[[nodiscard]] const MarkerStaticRecord* FindStaticRecord(const TerraEngine::UUID& objectId) const;
		[[nodiscard]] const MarkerPoseRecord* FindPoseRecord(const TerraEngine::UUID& objectId) const;
		[[nodiscard]] std::string_view ResolveString(MarkerStringId id) const;
		[[nodiscard]] std::uint64_t StaticPublicationGeneration() const { return staticPublicationGeneration; }
		[[nodiscard]] std::uint64_t PosePublicationGeneration() const { return posePublicationGeneration; }

	private:
		[[nodiscard]] MarkerStringId InternString(std::string_view value);
		void InvalidateViewportCache();
		[[nodiscard]] bool IsDescendantOfAny(const MarkerPoseRecord& pose, std::span<const TerraEngine::UUID> roots) const;

		std::unordered_map<TerraEngine::UUID, MarkerStaticRecord> staticRecords;
		std::unordered_map<TerraEngine::UUID, MarkerPoseRecord> poseRecords;
		std::vector<std::string> strings = { std::string() };
		std::unordered_map<std::string, MarkerStringId> stringIds;
		std::uint64_t sceneGeneration = 0u;
		std::uint64_t staticPublicationGeneration = 0u;
		std::uint64_t posePublicationGeneration = 0u;
		std::uint64_t batchGeneration = 0u;
		std::optional<MarkerFrameResult> cachedFrame;
		std::uint64_t cachedStaticGeneration = 0u;
		std::uint64_t cachedPoseGeneration = 0u;
	};
}
