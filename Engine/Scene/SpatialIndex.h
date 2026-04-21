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
	class SpatialIndex
	{
	public:
		void Clear();
		void Rebuild(const std::vector<SpatialObjectRecord>& records);
		void MarkDirty(const TerraEngine::UUID& objectId);

		[[nodiscard]] SpatialQueryResult QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialQueryResult QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter = {}) const;
		[[nodiscard]] SpatialDiagnosticsSnapshot GetDiagnosticsSnapshot() const;

	private:
		struct Node
		{
			SpatialAABB looseBounds{};
			SpatialAABB tightBounds{};
			std::array<int, 8> children{};
			std::vector<TerraEngine::UUID> objectIds;
			std::uint32_t depth = 0;
		};

		enum class QueryKind : std::uint8_t
		{
			AABB,
			Sphere,
			Frustum,
			Ray
		};

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
		std::vector<TerraEngine::UUID> fallbackObjectIds;
		std::unordered_set<TerraEngine::UUID> dirtyObjectIds;
		mutable SpatialDiagnosticsSnapshot diagnostics{};

		int CreateNode(const SpatialAABB& tightBounds, std::uint32_t depth);
		void InsertObject(int nodeIndex, const SpatialObjectRecord& record);
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
