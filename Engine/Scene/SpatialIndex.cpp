// SpatialIndex.cpp — Spatial Partitioning System Implementation
//
// Implements the core logic for the SpatialIndex, including structure rebuilds and query execution.
// It handles the registration and tracking of spatial objects, ensuring the index remains synchronized with the scene.
// The implementation focuses on maximizing query throughput while minimizing update overhead for dynamic objects.

#include "Scene/SpatialIndex.h"

#include <algorithm>

namespace TerraEngine::Scene
{
	namespace
	{
		[[nodiscard]] bool UuidLess(const TerraEngine::UUID& left, const TerraEngine::UUID& right)
		{
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] bool IsFallbackRecord(const SpatialObjectRecord& record)
		{
			return record.worldBounds.state == SpatialBoundsState::Fallback || record.worldBounds.state == SpatialBoundsState::Invalid;
		}

		[[nodiscard]] bool BoundsEquivalent(const SpatialAABB& left, const SpatialAABB& right)
		{
			return left.state == right.state
				&& left.source == right.source
				&& left.min.x == right.min.x
				&& left.min.y == right.min.y
				&& left.min.z == right.min.z
				&& left.max.x == right.max.x
				&& left.max.y == right.max.y
				&& left.max.z == right.max.z;
		}

		[[nodiscard]] bool RecordsEquivalent(const SpatialObjectRecord& left, const SpatialObjectRecord& right)
		{
			return left.objectId == right.objectId
				&& left.entity == right.entity
				&& left.active == right.active
				&& left.isStatic == right.isStatic
				&& left.categoryMask == right.categoryMask
				&& left.revision == right.revision
				&& BoundsEquivalent(left.worldBounds, right.worldBounds);
		}
	}

	// SpatialIndex::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::Clear()
	{
		nodes.clear();
		recordsById.clear();
		nodeByObjectId.clear();
		fallbackObjectIds.clear();
		dirtyObjectIds.clear();
		diagnostics = {};
		hasBuiltIndex = false;
	}

	// SpatialIndex::Rebuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::Rebuild(const std::vector<SpatialObjectRecord>& records)
	{
		const auto start = std::chrono::steady_clock::now();
		Clear();
		hasBuiltIndex = true;
		ResetRefreshCounters(SpatialRefreshMode::Full);
		diagnostics.fullRefreshObjectCount = static_cast<std::uint32_t>(records.size());

		SpatialAABB rootBounds{};
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : records)
		{
			if (record.objectId == TerraEngine::EMPTY_UUID)
			{
				continue;
			}
			recordsById[record.objectId] = record;
			diagnostics.indexedObjectCount++;
			diagnostics.staticObjectCount += record.isStatic ? 1u : 0u;
			diagnostics.dynamicObjectCount += record.isStatic ? 0u : 1u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!record.worldBounds.IsUsable() || IsFallbackRecord(record))
			{
				fallbackObjectIds.push_back(record.objectId);
				nodeByObjectId[record.objectId] = -1;
				diagnostics.fallbackObjectCount++;
				diagnostics.invalidBoundsObjectCount += record.worldBounds.IsUsable() ? 0u : 1u;
				continue;
			}
			rootBounds = rootBounds.IsUsable() ? Merge(rootBounds, record.worldBounds) : record.worldBounds;
		}

		std::sort(fallbackObjectIds.begin(), fallbackObjectIds.end(), UuidLess);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!rootBounds.IsUsable())
		{
			const auto end = std::chrono::steady_clock::now();
			diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
			return;
		}

		const glm::vec3 rootExtent = glm::max(rootBounds.Extent(), glm::vec3(1.0f));
		const glm::vec3 rootCenter = rootBounds.Center();
		const float maxExtent = std::max({ rootExtent.x, rootExtent.y, rootExtent.z, 1.0f });
		rootBounds = MakeAABB(rootCenter - glm::vec3(maxExtent), rootCenter + glm::vec3(maxExtent), SpatialBoundsSource::Aggregate);
		Expand(rootBounds, 0.01f);

		const int root = CreateNode(rootBounds, 0);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : records)
		{
			if (record.objectId != TerraEngine::EMPTY_UUID && record.worldBounds.IsUsable() && !IsFallbackRecord(record))
			{
				InsertObject(root, record);
			}
		}

		const auto end = std::chrono::steady_clock::now();
		diagnostics.dirtyObjectCount = static_cast<std::uint32_t>(dirtyObjectIds.size());
		diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
	}

	// SpatialIndex::Refresh is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::Refresh(const std::vector<SpatialObjectRecord>& records, bool forceFullRefresh)
	{
		if (forceFullRefresh || !hasBuiltIndex)
		{
			Rebuild(records);
			return;
		}

		const auto start = std::chrono::steady_clock::now();
		std::unordered_map<TerraEngine::UUID, SpatialObjectRecord> latestRecords;
		latestRecords.reserve(records.size());
		bool duplicateObjectIdFound = false;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : records)
		{
			if (record.objectId == TerraEngine::EMPTY_UUID)
			{
				continue;
			}
			const auto insertResult = latestRecords.emplace(record.objectId, record);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!insertResult.second)
			{
				duplicateObjectIdFound = true;
				break;
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (duplicateObjectIdFound)
		{
			Rebuild(records);
			return;
		}

		std::vector<TerraEngine::UUID> removedObjectIds;
		std::vector<SpatialObjectRecord> addedRecords;
		std::vector<SpatialObjectRecord> updatedRecords;
		removedObjectIds.reserve(recordsById.size());
		addedRecords.reserve(latestRecords.size());
		updatedRecords.reserve(latestRecords.size());

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : recordsById)
		{
			const TerraEngine::UUID& objectId = entry.first;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (latestRecords.find(objectId) == latestRecords.end())
			{
				removedObjectIds.push_back(objectId);
			}
		}

		std::uint32_t unchangedCount = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& [objectId, record] : latestRecords)
		{
			const auto existing = recordsById.find(objectId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (existing == recordsById.end())
			{
				addedRecords.push_back(record);
				continue;
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (dirtyObjectIds.find(objectId) != dirtyObjectIds.end() || !RecordsEquivalent(existing->second, record))
			{
				updatedRecords.push_back(record);
				continue;
			}
			unchangedCount++;
		}

		bool requiresFullRefresh = false;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : addedRecords)
		{
			requiresFullRefresh = requiresFullRefresh || !CanRefreshRecordIncrementally(record);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : updatedRecords)
		{
			requiresFullRefresh = requiresFullRefresh || !CanRefreshRecordIncrementally(record);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (requiresFullRefresh)
		{
			Rebuild(records);
			return;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (removedObjectIds.empty() && addedRecords.empty() && updatedRecords.empty())
		{
			ResetRefreshCounters(SpatialRefreshMode::Unchanged);
			diagnostics.unchangedObjectCount = unchangedCount;
			diagnostics.dirtyObjectCount = 0u;
			diagnostics.visibleCandidateCount = 0u;
			diagnostics.rejectedObjectCount = 0u;
			RecalculateStoredDiagnostics();
			dirtyObjectIds.clear();
			const auto end = std::chrono::steady_clock::now();
			diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
			return;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const TerraEngine::UUID& objectId : removedObjectIds)
		{
			RemoveStoredRecord(objectId);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : updatedRecords)
		{
			RemoveStoredRecord(record.objectId);
			InsertStoredRecord(record);
		}
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const SpatialObjectRecord& record : addedRecords)
		{
			InsertStoredRecord(record);
		}

		ResetRefreshCounters(SpatialRefreshMode::Incremental);
		diagnostics.addedObjectCount = static_cast<std::uint32_t>(addedRecords.size());
		diagnostics.updatedObjectCount = static_cast<std::uint32_t>(updatedRecords.size());
		diagnostics.removedObjectCount = static_cast<std::uint32_t>(removedObjectIds.size());
		diagnostics.unchangedObjectCount = unchangedCount;
		diagnostics.dirtyObjectCount = 0u;
		diagnostics.visibleCandidateCount = 0u;
		diagnostics.rejectedObjectCount = 0u;
		RecalculateStoredDiagnostics();
		dirtyObjectIds.clear();

		const auto end = std::chrono::steady_clock::now();
		diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
	}

	// SpatialIndex::MarkDirty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::MarkDirty(const TerraEngine::UUID& objectId)
	{
		if (objectId != TerraEngine::EMPTY_UUID)
		{
			dirtyObjectIds.insert(objectId);
			diagnostics.dirtyObjectCount = static_cast<std::uint32_t>(dirtyObjectIds.size());
		}
	}

	// SpatialIndex::QueryAABB is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialQueryResult SpatialIndex::QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!area.IsUsable())
		{
			result.validInput = false;
			return result;
		}
		QueryShape shape{};
		shape.kind = QueryKind::AABB;
		shape.aabb = area;
		FinishResult(shape, filter, result);
		return result;
	}

	// SpatialIndex::QuerySphere is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialQueryResult SpatialIndex::QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsFinite(sphere.center) || !IsFinite(sphere.radius) || sphere.radius < 0.0f)
		{
			result.validInput = false;
			return result;
		}
		QueryShape shape{};
		shape.kind = QueryKind::Sphere;
		shape.sphere = sphere;
		FinishResult(shape, filter, result);
		return result;
	}

	SpatialQueryResult SpatialIndex::QueryStreamingCandidates(const glm::vec3& center, float radius, const SpatialQueryFilter& filter) const
	{
		SpatialSphere sphere{};
		sphere.center = center;
		sphere.radius = std::max(0.0f, radius);
		// Streaming reuses the normal spatial sphere query so preload candidate
		// selection stays deterministic with renderer/editor broadphase data.
		return QuerySphere(sphere, filter);
	}

	// SpatialIndex::QueryFrustum is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialQueryResult SpatialIndex::QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
		QueryShape shape{};
		shape.kind = QueryKind::Frustum;
		shape.frustum = frustum;
		FinishResult(shape, filter, result);
		return result;
	}

	// SpatialIndex::QueryRay is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialQueryResult SpatialIndex::QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!IsFinite(ray.origin) || !IsFinite(ray.direction) || !IsFinite(ray.maxDistance) || ray.maxDistance < 0.0f || glm::dot(ray.direction, ray.direction) <= 1e-10f)
		{
			result.validInput = false;
			return result;
		}
		QueryShape shape{};
		shape.kind = QueryKind::Ray;
		shape.ray = ray;
		FinishResult(shape, filter, result);
		return result;
	}

	// SpatialIndex::GetDiagnosticsSnapshot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialDiagnosticsSnapshot SpatialIndex::GetDiagnosticsSnapshot() const
	{
		std::scoped_lock lock(diagnosticsMutex);
		return diagnostics;
	}

	// SpatialIndex::CreateNode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int SpatialIndex::CreateNode(const SpatialAABB& tightBounds, std::uint32_t depth)
	{
		Node node{};
		node.tightBounds = tightBounds;
		node.looseBounds = MakeLooseBounds(tightBounds);
		node.children.fill(-1);
		node.depth = depth;
		diagnostics.maxNodeDepth = std::max(diagnostics.maxNodeDepth, depth);
		nodes.push_back(std::move(node));
		return static_cast<int>(nodes.size() - 1);
	}

	// SpatialIndex::InsertObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::InsertObject(int nodeIndex, const SpatialObjectRecord& record)
	{
		const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (node.depth < kMaxDepth)
		{
			const int childSlot = FindContainingChild(node, record.worldBounds);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (childSlot >= 0)
			{
				int childNode = nodes[static_cast<std::size_t>(nodeIndex)].children[static_cast<std::size_t>(childSlot)];
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (childNode < 0)
				{
					childNode = CreateNode(MakeChildBounds(node.tightBounds, childSlot), node.depth + 1);
					nodes[static_cast<std::size_t>(nodeIndex)].children[static_cast<std::size_t>(childSlot)] = childNode;
				}
				InsertObject(childNode, record);
				return;
			}
		}

		Node& writableNode = nodes[static_cast<std::size_t>(nodeIndex)];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (writableNode.depth == 0 && writableNode.objectIds.size() > kNodeSplitObjectCount)
		{
			diagnostics.largeObjectCount++;
		}
		writableNode.objectIds.push_back(record.objectId);
		std::sort(writableNode.objectIds.begin(), writableNode.objectIds.end(), UuidLess);
		nodeByObjectId[record.objectId] = nodeIndex;
	}

	// SpatialIndex::InsertStoredRecord is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SpatialIndex::InsertStoredRecord(const SpatialObjectRecord& record)
	{
		if (record.objectId == TerraEngine::EMPTY_UUID)
		{
			return false;
		}

		recordsById[record.objectId] = record;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!record.worldBounds.IsUsable() || IsFallbackRecord(record))
		{
			fallbackObjectIds.push_back(record.objectId);
			std::sort(fallbackObjectIds.begin(), fallbackObjectIds.end(), UuidLess);
			nodeByObjectId[record.objectId] = -1;
			return true;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (nodes.empty())
		{
			return false;
		}

		InsertObject(0, record);
		return true;
	}

	// SpatialIndex::RemoveStoredRecord is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SpatialIndex::RemoveStoredRecord(const TerraEngine::UUID& objectId)
	{
		const auto membership = nodeByObjectId.find(objectId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (membership == nodeByObjectId.end())
		{
			return recordsById.erase(objectId) > 0u;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (membership->second < 0)
		{
			fallbackObjectIds.erase(std::remove(fallbackObjectIds.begin(), fallbackObjectIds.end(), objectId), fallbackObjectIds.end());
		}
		else
		{
			std::vector<TerraEngine::UUID>& objectIds = nodes[static_cast<std::size_t>(membership->second)].objectIds;
			objectIds.erase(std::remove(objectIds.begin(), objectIds.end(), objectId), objectIds.end());
		}

		nodeByObjectId.erase(membership);
		recordsById.erase(objectId);
		return true;
	}

	// SpatialIndex::CanRefreshRecordIncrementally is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SpatialIndex::CanRefreshRecordIncrementally(const SpatialObjectRecord& record) const
	{
		if (!record.worldBounds.IsUsable() || IsFallbackRecord(record))
		{
			return true;
		}

		// The query traversal starts at the current root loose bounds. Any new
		// indexable record outside that root could be missed by queries, so fall
		// back to a full rebuild to grow the tree around the latest aggregate.
		return !nodes.empty() && Contains(nodes.front().looseBounds, record.worldBounds);
	}

	// SpatialIndex::RecalculateStoredDiagnostics is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::RecalculateStoredDiagnostics()
	{
		diagnostics.indexedObjectCount = static_cast<std::uint32_t>(recordsById.size());
		diagnostics.staticObjectCount = 0u;
		diagnostics.dynamicObjectCount = 0u;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const auto& entry : recordsById)
		{
			const SpatialObjectRecord& record = entry.second;
			diagnostics.staticObjectCount += record.isStatic ? 1u : 0u;
			diagnostics.dynamicObjectCount += record.isStatic ? 0u : 1u;
		}

		diagnostics.fallbackObjectCount = static_cast<std::uint32_t>(fallbackObjectIds.size());
		diagnostics.largeObjectCount = 0u;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!nodes.empty() && nodes.front().objectIds.size() > kNodeSplitObjectCount)
		{
			diagnostics.largeObjectCount = static_cast<std::uint32_t>(nodes.front().objectIds.size());
		}
	}

	// SpatialIndex::ResetRefreshCounters is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::ResetRefreshCounters(SpatialRefreshMode mode)
	{
		diagnostics.refreshMode = mode;
		diagnostics.addedObjectCount = 0u;
		diagnostics.updatedObjectCount = 0u;
		diagnostics.removedObjectCount = 0u;
		diagnostics.unchangedObjectCount = 0u;
		diagnostics.fullRefreshObjectCount = 0u;
		diagnostics.incrementalRefreshCount = mode == SpatialRefreshMode::Incremental ? 1u : 0u;
		diagnostics.fullRefreshCount = mode == SpatialRefreshMode::Full ? 1u : 0u;
		diagnostics.revisionMismatchFallbackCount = 0u;
		diagnostics.invalidBoundsObjectCount = 0u;
	}

	// SpatialIndex::FindContainingChild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	int SpatialIndex::FindContainingChild(const Node& node, const SpatialAABB& bounds) const
	{
		for (int childIndex = 0; childIndex < 8; ++childIndex)
		{
			const SpatialAABB childBounds = MakeLooseBounds(MakeChildBounds(node.tightBounds, childIndex));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (Contains(childBounds, bounds))
			{
				return childIndex;
			}
		}
		return -1;
	}

	// SpatialIndex::MakeChildBounds is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialAABB SpatialIndex::MakeChildBounds(const SpatialAABB& parentBounds, int childIndex) const
	{
		const glm::vec3 center = parentBounds.Center();
		const glm::vec3 childMin(
			(childIndex & 1) != 0 ? center.x : parentBounds.min.x,
			(childIndex & 2) != 0 ? center.y : parentBounds.min.y,
			(childIndex & 4) != 0 ? center.z : parentBounds.min.z);
		const glm::vec3 childMax(
			(childIndex & 1) != 0 ? parentBounds.max.x : center.x,
			(childIndex & 2) != 0 ? parentBounds.max.y : center.y,
			(childIndex & 4) != 0 ? parentBounds.max.z : center.z);
		return MakeAABB(childMin, childMax, SpatialBoundsSource::Aggregate);
	}

	// SpatialIndex::MakeLooseBounds is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	SpatialAABB SpatialIndex::MakeLooseBounds(const SpatialAABB& tightBounds) const
	{
		const glm::vec3 center = tightBounds.Center();
		const glm::vec3 extent = glm::max(tightBounds.Extent() * kLooseFactor, glm::vec3(0.001f));
		return MakeAABB(center - extent, center + extent, SpatialBoundsSource::Aggregate);
	}

	// SpatialIndex::NodeCouldIntersect is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SpatialIndex::NodeCouldIntersect(const QueryShape& shape, const SpatialAABB& bounds) const
	{
		switch (shape.kind)
		{
			case QueryKind::AABB: return Overlaps(shape.aabb, bounds);
			case QueryKind::Sphere: return Intersects(shape.sphere, bounds);
			case QueryKind::Frustum: return Intersects(shape.frustum, bounds);
			case QueryKind::Ray: return Intersects(shape.ray, bounds).has_value();
		}
		return false;
	}

	// SpatialIndex::ObjectIntersects is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::optional<float> SpatialIndex::ObjectIntersects(const QueryShape& shape, const SpatialObjectRecord& record) const
	{
		if (IsFallbackRecord(record))
		{
			return 0.0f;
		}

		// Dispatch by explicit engine state so each case documents one supported behavior.
		switch (shape.kind)
		{
			case QueryKind::AABB:
				return Overlaps(shape.aabb, record.worldBounds) ? std::optional<float>(0.0f) : std::nullopt;
			case QueryKind::Sphere:
				return Intersects(shape.sphere, record.worldBounds) ? std::optional<float>(glm::length(record.worldBounds.Center() - shape.sphere.center)) : std::nullopt;
			case QueryKind::Frustum:
				return Intersects(shape.frustum, record.worldBounds) ? std::optional<float>(0.0f) : std::nullopt;
			case QueryKind::Ray:
				return Intersects(shape.ray, record.worldBounds);
		}
		return std::nullopt;
	}

	// SpatialIndex::PassesFilter is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool SpatialIndex::PassesFilter(const SpatialObjectRecord& record, const SpatialQueryFilter& filter) const
	{
		if (!filter.includeInactive && !record.active)
		{
			return false;
		}
		return (record.categoryMask & filter.categoryMask) != 0u;
	}

	// SpatialIndex::AppendHit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::AppendHit(SpatialQueryResult& result, const SpatialObjectRecord& record, float distance, const SpatialQueryFilter& filter) const
	{
		if (!PassesFilter(record, filter))
		{
			result.rejectedCount++;
			return;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (filter.maxResults != 0u && result.hits.size() >= filter.maxResults)
		{
			result.rejectedCount++;
			return;
		}

		SpatialQueryHit hit{};
		hit.objectId = record.objectId;
		hit.entity = record.entity;
		hit.bounds = record.worldBounds;
		hit.distance = distance;
		hit.fallback = IsFallbackRecord(record);
		result.fallbackCount += hit.fallback ? 1u : 0u;
		result.hits.push_back(hit);
	}

	// SpatialIndex::QueryNode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::QueryNode(int nodeIndex, const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result, std::unordered_set<TerraEngine::UUID>& seen) const
	{
		const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!NodeCouldIntersect(shape, node.looseBounds))
		{
			return;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const TerraEngine::UUID& objectId : node.objectIds)
		{
			if (!seen.insert(objectId).second)
			{
				continue;
			}
			const SpatialObjectRecord& record = recordsById.at(objectId);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (const std::optional<float> distance = ObjectIntersects(shape, record))
			{
				AppendHit(result, record, *distance, filter);
			}
			else
			{
				result.rejectedCount++;
			}
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (int child : node.children)
		{
			if (child >= 0)
			{
				QueryNode(child, shape, filter, result, seen);
			}
		}
	}

	// SpatialIndex::FinishResult is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::FinishResult(const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result) const
	{
		const auto start = std::chrono::steady_clock::now();
		std::unordered_set<TerraEngine::UUID> seen;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!nodes.empty())
		{
			QueryNode(0, shape, filter, result, seen);
		}

		// Fallback records are always checked after indexed records because they
		// represent uncertainty. Including them preserves correctness while the
		// diagnostics tell users how much precision was lost.
		for (const TerraEngine::UUID& objectId : fallbackObjectIds)
		{
			if (!seen.insert(objectId).second)
			{
				continue;
			}
			const SpatialObjectRecord& record = recordsById.at(objectId);
			AppendHit(result, record, 0.0f, filter);
		}

		std::sort(result.hits.begin(), result.hits.end(), [&](const SpatialQueryHit& left, const SpatialQueryHit& right) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (filter.sortMode == SpatialSortMode::DistanceAscending && left.distance != right.distance)
			{
				return left.distance < right.distance;
			}
			return UuidLess(left.objectId, right.objectId);
		});

		const auto end = std::chrono::steady_clock::now();
		{
			std::scoped_lock lock(diagnosticsMutex);
			diagnostics.visibleCandidateCount = static_cast<std::uint32_t>(result.hits.size());
			diagnostics.rejectedObjectCount = result.rejectedCount;
			diagnostics.fallbackObjectCount = result.fallbackCount;
			diagnostics.lastQueryDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
		}
	}
}
