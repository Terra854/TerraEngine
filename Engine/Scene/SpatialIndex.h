// SpatialIndex.h — Spatial Partitioning System Definition
//
// Defines the SpatialIndex class, which organizes scene entities for efficient spatial retrieval.
// It maintains a high-performance acceleration structure that is updated as objects move or are modified.
// This system is critical for optimizing rendering visibility, physics broadphase, and editor interactions.

#pragma once

#include <array>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Scene/SpatialDiagnostics.h"
#include "Scene/SpatialQuery.h"

namespace TerraEngine::Scene
{
	// class SpatialIndex groups related state and operations so callers have one documented contract.
	class SpatialIndex
	{
	public:
		void Clear();
		void Rebuild(const std::vector<SpatialObjectRecord>& records);
		// Refresh compares the latest complete renderable snapshot against the
		// cached records. It mutates only changed entries when that is safe and
		// falls back to a full rebuild when root bounds must be recalculated.
		void Refresh(const std::vector<SpatialObjectRecord>& records, bool forceFullRefresh = false);
		void MarkDirty(const TerraEngine::UUID& objectId);

		[[nodiscard]] SpatialQueryResult QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter = {}) const;
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
		mutable SpatialDiagnosticsSnapshot diagnostics{};
		bool hasBuiltIndex = false;

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
}
