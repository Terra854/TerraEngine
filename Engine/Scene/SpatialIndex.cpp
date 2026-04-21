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
	}

	void SpatialIndex::Clear()
	{
		nodes.clear();
		recordsById.clear();
		fallbackObjectIds.clear();
		dirtyObjectIds.clear();
		diagnostics = {};
	}

	void SpatialIndex::Rebuild(const std::vector<SpatialObjectRecord>& records)
	{
		const auto start = std::chrono::steady_clock::now();
		Clear();

		SpatialAABB rootBounds{};
		for (const SpatialObjectRecord& record : records)
		{
			recordsById[record.objectId] = record;
			diagnostics.indexedObjectCount++;
			diagnostics.staticObjectCount += record.isStatic ? 1u : 0u;
			diagnostics.dynamicObjectCount += record.isStatic ? 0u : 1u;
			if (!record.worldBounds.IsUsable() || IsFallbackRecord(record))
			{
				fallbackObjectIds.push_back(record.objectId);
				diagnostics.fallbackObjectCount++;
				continue;
			}
			rootBounds = rootBounds.IsUsable() ? Merge(rootBounds, record.worldBounds) : record.worldBounds;
		}

		std::sort(fallbackObjectIds.begin(), fallbackObjectIds.end(), UuidLess);
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
		for (const SpatialObjectRecord& record : records)
		{
			if (record.worldBounds.IsUsable() && !IsFallbackRecord(record))
			{
				InsertObject(root, record);
			}
		}

		const auto end = std::chrono::steady_clock::now();
		diagnostics.dirtyObjectCount = static_cast<std::uint32_t>(dirtyObjectIds.size());
		diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
	}

	void SpatialIndex::MarkDirty(const TerraEngine::UUID& objectId)
	{
		if (objectId != TerraEngine::EMPTY_UUID)
		{
			dirtyObjectIds.insert(objectId);
			diagnostics.dirtyObjectCount = static_cast<std::uint32_t>(dirtyObjectIds.size());
		}
	}

	SpatialQueryResult SpatialIndex::QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
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

	SpatialQueryResult SpatialIndex::QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
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

	SpatialQueryResult SpatialIndex::QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
		QueryShape shape{};
		shape.kind = QueryKind::Frustum;
		shape.frustum = frustum;
		FinishResult(shape, filter, result);
		return result;
	}

	SpatialQueryResult SpatialIndex::QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter) const
	{
		SpatialQueryResult result{};
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

	SpatialDiagnosticsSnapshot SpatialIndex::GetDiagnosticsSnapshot() const
	{
		return diagnostics;
	}

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

	void SpatialIndex::InsertObject(int nodeIndex, const SpatialObjectRecord& record)
	{
		const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
		if (node.depth < kMaxDepth)
		{
			const int childSlot = FindContainingChild(node, record.worldBounds);
			if (childSlot >= 0)
			{
				int childNode = nodes[static_cast<std::size_t>(nodeIndex)].children[static_cast<std::size_t>(childSlot)];
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
		if (writableNode.depth == 0 && writableNode.objectIds.size() > kNodeSplitObjectCount)
		{
			diagnostics.largeObjectCount++;
		}
		writableNode.objectIds.push_back(record.objectId);
		std::sort(writableNode.objectIds.begin(), writableNode.objectIds.end(), UuidLess);
	}

	int SpatialIndex::FindContainingChild(const Node& node, const SpatialAABB& bounds) const
	{
		for (int childIndex = 0; childIndex < 8; ++childIndex)
		{
			const SpatialAABB childBounds = MakeLooseBounds(MakeChildBounds(node.tightBounds, childIndex));
			if (Contains(childBounds, bounds))
			{
				return childIndex;
			}
		}
		return -1;
	}

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

	SpatialAABB SpatialIndex::MakeLooseBounds(const SpatialAABB& tightBounds) const
	{
		const glm::vec3 center = tightBounds.Center();
		const glm::vec3 extent = glm::max(tightBounds.Extent() * kLooseFactor, glm::vec3(0.001f));
		return MakeAABB(center - extent, center + extent, SpatialBoundsSource::Aggregate);
	}

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

	std::optional<float> SpatialIndex::ObjectIntersects(const QueryShape& shape, const SpatialObjectRecord& record) const
	{
		if (IsFallbackRecord(record))
		{
			return 0.0f;
		}

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

	bool SpatialIndex::PassesFilter(const SpatialObjectRecord& record, const SpatialQueryFilter& filter) const
	{
		if (!filter.includeInactive && !record.active)
		{
			return false;
		}
		return (record.categoryMask & filter.categoryMask) != 0u;
	}

	void SpatialIndex::AppendHit(SpatialQueryResult& result, const SpatialObjectRecord& record, float distance, const SpatialQueryFilter& filter) const
	{
		if (!PassesFilter(record, filter))
		{
			result.rejectedCount++;
			return;
		}
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

	void SpatialIndex::QueryNode(int nodeIndex, const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result, std::unordered_set<TerraEngine::UUID>& seen) const
	{
		const Node& node = nodes[static_cast<std::size_t>(nodeIndex)];
		if (!NodeCouldIntersect(shape, node.looseBounds))
		{
			return;
		}

		for (const TerraEngine::UUID& objectId : node.objectIds)
		{
			if (!seen.insert(objectId).second)
			{
				continue;
			}
			const SpatialObjectRecord& record = recordsById.at(objectId);
			if (const std::optional<float> distance = ObjectIntersects(shape, record))
			{
				AppendHit(result, record, *distance, filter);
			}
			else
			{
				result.rejectedCount++;
			}
		}

		for (int child : node.children)
		{
			if (child >= 0)
			{
				QueryNode(child, shape, filter, result, seen);
			}
		}
	}

	void SpatialIndex::FinishResult(const QueryShape& shape, const SpatialQueryFilter& filter, SpatialQueryResult& result) const
	{
		const auto start = std::chrono::steady_clock::now();
		std::unordered_set<TerraEngine::UUID> seen;
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
			if (filter.sortMode == SpatialSortMode::DistanceAscending && left.distance != right.distance)
			{
				return left.distance < right.distance;
			}
			return UuidLess(left.objectId, right.objectId);
		});

		const auto end = std::chrono::steady_clock::now();
		diagnostics.visibleCandidateCount = static_cast<std::uint32_t>(result.hits.size());
		diagnostics.rejectedObjectCount = result.rejectedCount;
		diagnostics.fallbackObjectCount = result.fallbackCount;
		diagnostics.lastQueryDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
	}
}
