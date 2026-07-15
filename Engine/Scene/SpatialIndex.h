// SpatialIndex.h — Spatial Partitioning System Definition
//
// Defines the SpatialIndex class, which organizes scene entities for efficient spatial retrieval.
// It maintains a high-performance acceleration structure that is updated as objects move or are modified.
// This system is critical for optimizing rendering visibility, physics broadphase, and editor interactions.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Scene/SpatialDiagnostics.h"
#include "Scene/SpatialQuery.h"

namespace TerraEngine::Scene
{
	enum class SpatialRefreshAction : std::uint8_t
	{
		Add,
		Update,
		Remove,
		Unchanged,
		FullRefreshFallback,
		Reject
	};

	struct SpatialRefreshRecord
	{
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		SpatialRefreshAction action = SpatialRefreshAction::Unchanged;
		SpatialObjectRecord record{};
		std::uint64_t previousRevision = 0;
		std::uint64_t nextRevision = 0;
	};

	struct SpatialRefreshBatch
	{
		std::uint64_t batchRevision = 0;
		bool fullRefresh = false;
		std::vector<SpatialRefreshRecord> records;
		std::uint32_t addedCount = 0;
		std::uint32_t updatedCount = 0;
		std::uint32_t removedCount = 0;
		std::uint32_t unchangedCount = 0;
	};

	struct SpatialBoundsPublicationChange
	{
		SpatialBoundsChangeKind kind = SpatialBoundsChangeKind::Update;
		SpatialObjectRecord record{};
	};

	// A publication batch is value-only worker output. ApplyBoundsPublicationBatch
	// validates the entire generation and identity set before either partition
	// becomes visible at the protected boundary.
	struct SpatialBoundsPublicationBatch
	{
		std::uint64_t sceneGeneration = 0;
		std::uint64_t publicationGeneration = 0;
		std::vector<SpatialBoundsPublicationChange> changes;
	};

	struct SpatialPartitionRevisionSnapshot
	{
		std::uint64_t staticRevision = 0;
		std::uint64_t movableRevision = 0;
		std::uint64_t publicationGeneration = 0;
		std::size_t staticRecordCount = 0;
		std::size_t movableRecordCount = 0;
	};

	// class SpatialIndex groups related state and operations so callers have one documented contract.
	class SpatialIndex
	{
	public:
		SpatialIndex();
		~SpatialIndex();
		SpatialIndex(const SpatialIndex&) = delete;
		SpatialIndex& operator=(const SpatialIndex&) = delete;

		void Clear();
		void Rebuild(const std::vector<SpatialObjectRecord>& records);
		// A scene replacement supplies a complete immutable record set.  This
		// rebuilds both partitions before recording one scene-generation boundary,
		// so consumers never mistake incremental changes for a partial load.
		[[nodiscard]] bool RebuildCompleteSceneSnapshot(const std::vector<SpatialObjectRecord>& records, std::uint64_t sceneGeneration);
		// Refresh compares the latest complete renderable snapshot against the
		// cached records. It mutates only changed entries when that is safe and
		// falls back to a full rebuild when root bounds must be recalculated.
		void Refresh(const std::vector<SpatialObjectRecord>& records, bool forceFullRefresh = false);
		void MarkDirty(const TerraEngine::UUID& objectId);
		void InvalidateForHierarchySnapshotRebuild(std::uint64_t hierarchyRevision);
		[[nodiscard]] bool ApplyBoundsPublicationBatch(const SpatialBoundsPublicationBatch& batch);
		[[nodiscard]] SpatialPartitionRevisionSnapshot GetPartitionRevisionSnapshot() const;
		[[nodiscard]] std::vector<TerraEngine::UUID> GetLastBoundsPublicationOrder() const;

		[[nodiscard]] SpatialQueryResult QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter = {}) const;
		// Streaming uses the same indexed snapshot as renderer visibility so
		// large worlds can discover near-view preload candidates without
		// scanning every object each frame.
		[[nodiscard]] SpatialQueryResult QueryStreamingCandidates(const glm::vec3& center, float radius, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

	private:
		// struct Node groups related state and operations so callers have one documented contract.
		struct Node
		{
			SpatialAABB looseBounds{};
			SpatialAABB tightBounds{};
			std::array<int, 8> children{};
			std::vector<TerraEngine::UUID> objectIds;
			std::uint32_t depth = 0;
		};

		// QueryKind names the supported states explicitly so serialization and switches stay readable.
		enum class QueryKind : std::uint8_t
		{
			AABB,
			Sphere,
			Frustum,
			Ray
		};

		// struct QueryShape groups related state and operations so callers have one documented contract.
		struct QueryShape
		{
			QueryKind kind = QueryKind::AABB;
			SpatialAABB aabb{};
			SpatialSphere sphere{};
			SpatialFrustum frustum{};
			SpatialRay ray{};
		};

		static constexpr std::uint32_t kMaxDepth = 8;
		static constexpr std::uint32_t kNodeSplitObjectCount = 16;
		static constexpr float kLooseFactor = 1.5f;

		std::vector<Node> nodes;
		std::unordered_map<TerraEngine::UUID, SpatialObjectRecord> recordsById;
		std::unordered_map<TerraEngine::UUID, int> nodeByObjectId;
		std::vector<TerraEngine::UUID> fallbackObjectIds;
		std::unordered_set<TerraEngine::UUID> dirtyObjectIds;
		mutable std::mutex diagnosticsMutex;
		mutable SpatialDiagnosticsSnapshot diagnostics{};
		std::uint64_t lastHierarchySnapshotRevision = 0;
		bool hasBuiltIndex = false;
		bool partitionLeaf = false;
		std::unique_ptr<SpatialIndex> staticPartition;
		std::unique_ptr<SpatialIndex> movablePartition;
		std::uint64_t staticPartitionRevision = 0;
		std::uint64_t movablePartitionRevision = 0;
		std::uint64_t lastBoundsPublicationGeneration = 0;
		std::uint64_t boundsSceneGeneration = 0;
		std::vector<TerraEngine::UUID> lastBoundsPublicationOrder;

		explicit SpatialIndex(bool createPartitionLeaf);
		void EnsurePartitions();
		void RefreshAggregateDiagnostics(SpatialRefreshMode requestedMode);

		int CreateNode(const SpatialAABB& tightBounds, std::uint32_t depth);
		void InsertObject(int nodeIndex, const SpatialObjectRecord& record);
		bool InsertStoredRecord(const SpatialObjectRecord& record);
		bool RemoveStoredRecord(const TerraEngine::UUID& objectId);
		bool CanRefreshRecordIncrementally(const SpatialObjectRecord& record) const;
		void RecalculateStoredDiagnostics();
		void ResetRefreshCounters(SpatialRefreshMode mode);
		[[nodiscard]] int FindContainingChild(const Node& node, const SpatialAABB& bounds) const;
		[[nodiscard]] SpatialAABB MakeChildBounds(const SpatialAABB& parentBounds, int childIndex) const;
		[[nodiscard]] SpatialAABB MakeLooseBounds(const SpatialAABB& tightBounds) const;
		[[nodiscard]] bool NodeCouldIntersect(const QueryShape& shape, const SpatialAABB& bounds) const;
		[[nodiscard]] std::optional<float> ObjectIntersects(const QueryShape& shape, const SpatialObjectRecord& record) const;
		[[nodiscard]] bool PassesFilter(const SpatialObjectRecord& record, const SpatialQueryFilter& filter) const;
		void AppendHit(SpatialQueryResult& result, const SpatialObjectRecord& record, float distance, const SpatialQueryFilter& filter) const;
		void QueryNode(int nodeIndex, const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result, std::unordered_set<TerraEngine::UUID>& seen) const;
		void FinishResult(const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result) const;
	};

	// EditorSelectableSpatialSnapshot is an immutable, worker-readable view of
	// marker-bearing objects. It deliberately owns a separate index from the
	// renderer so physics-only objects, lights, cameras, and true empties are not
	// lost merely because they have no renderable component.
	class EditorSelectableSpatialSnapshot
	{
	public:
		EditorSelectableSpatialSnapshot() = default;

		[[nodiscard]] bool IsValid() const;
		[[nodiscard]] std::uint64_t Generation() const;
		[[nodiscard]] std::size_t RecordCount() const;
		[[nodiscard]] SpatialQueryResult QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter = {}) const;

	private:
		friend class EditorSelectableSpatialSnapshotPublisher;

		std::uint64_t generation = 0;
		std::size_t recordCount = 0;
		std::shared_ptr<const SpatialIndex> index;
	};

	// EditorSelectableSpatialSnapshotPublisher constructs a complete immutable
	// candidate snapshot off the live scene and swaps it atomically only after
	// validation. Readers can retain an older snapshot safely while a newer
	// generation is prepared on another thread.
	class EditorSelectableSpatialSnapshotPublisher
	{
	public:
		[[nodiscard]] bool Publish(std::vector<SpatialObjectRecord> records, std::uint64_t generation);
		[[nodiscard]] std::shared_ptr<const EditorSelectableSpatialSnapshot> Latest() const;

	private:
		mutable std::mutex publishMutex;
		std::shared_ptr<const EditorSelectableSpatialSnapshot> latest;
	};
}
