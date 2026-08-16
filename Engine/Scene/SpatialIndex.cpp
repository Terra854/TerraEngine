// SpatialIndex.cpp — Spatial Partitioning System Implementation
//
// Implements the core logic for the SpatialIndex, including structure rebuilds and query execution.
// It handles the registration and tracking of spatial objects, ensuring the index remains synchronized with the scene.
// The implementation focuses on maximizing query throughput while minimizing update overhead for dynamic objects.

#include "Scene/SpatialIndex.h"

#include "Core/Scheduling/WorkerSafety.h"

#include <algorithm>
#include <iterator>

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
			return record.localBounds.state == SpatialBoundsState::Fallback || record.localBounds.state == SpatialBoundsState::Invalid;
		}

		[[nodiscard]] bool FitsReferenceFrame(const SpatialObjectRecord& record, const SpatialReferenceFrame& frame)
		{
			// Local AABBs are useful only inside one bounded camera/physics frame.
			// Rejecting an oversized float prevents producers from using it as a
			// lossy replacement for the canonical WorldPosition address.
			if (!record.localBounds.IsUsable())
			{
				return true;
			}
			const double maximum = frame.maximumLocalDistanceMetres;
			return static_cast<double>(record.localBounds.min.x) >= -maximum
				&& static_cast<double>(record.localBounds.min.y) >= -maximum
				&& static_cast<double>(record.localBounds.min.z) >= -maximum
				&& static_cast<double>(record.localBounds.max.x) <= maximum
				&& static_cast<double>(record.localBounds.max.y) <= maximum
				&& static_cast<double>(record.localBounds.max.z) <= maximum;
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
				&& left.canonicalOwner == right.canonicalOwner
				&& left.frameGeneration == right.frameGeneration
				&& BoundsEquivalent(left.localBounds, right.localBounds);
		}
	}

	SpatialIndex::SpatialIndex() = default;

	SpatialIndex::SpatialIndex(bool createPartitionLeaf)
		: partitionLeaf(createPartitionLeaf)
	{
	}

	SpatialIndex::~SpatialIndex() = default;

	bool SpatialIndex::SetReferenceFrame(SpatialReferenceFrame frame)
	{
		const WorldCoordinateResult normalized = NormalizeWorldPosition(frame.canonicalOrigin);
		if (!normalized.Succeeded() || !std::isfinite(frame.maximumLocalDistanceMetres) || frame.maximumLocalDistanceMetres < 0.0)
		{
			return false;
		}
		frame.canonicalOrigin = normalized.position;
		if (frame.generation != referenceFrame.generation || frame.canonicalOrigin != referenceFrame.canonicalOrigin)
		{
			Clear();
			referenceFrame = std::move(frame);
		}
		return true;
	}

	SpatialReferenceFrame SpatialIndex::GetReferenceFrame() const
	{
		return referenceFrame;
	}

	void SpatialIndex::EnsurePartitions()
	{
		if (partitionLeaf)
		{
			return;
		}
		if (!staticPartition)
		{
			staticPartition = std::unique_ptr<SpatialIndex>(new SpatialIndex(true));
		}
		if (!movablePartition)
		{
			movablePartition = std::unique_ptr<SpatialIndex>(new SpatialIndex(true));
		}
	}

	// SpatialIndex::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SpatialIndex::Clear()
	{
		if (!partitionLeaf)
		{
			staticPartition.reset();
			movablePartition.reset();
			staticPartitionRevision = 0u;
			movablePartitionRevision = 0u;
			lastBoundsPublicationGeneration = 0u;
			boundsSceneGeneration = 0u;
			lastBoundsPublicationOrder.clear();
			diagnostics = {};
			hasBuiltIndex = false;
			return;
		}
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
		if (!partitionLeaf)
		{
			const auto start = std::chrono::steady_clock::now();
			EnsurePartitions();
			std::vector<SpatialObjectRecord> staticRecords;
			std::vector<SpatialObjectRecord> movableRecords;
			staticRecords.reserve(records.size());
			movableRecords.reserve(records.size());
			for (const SpatialObjectRecord& record : records)
			{
				(record.isStatic ? staticRecords : movableRecords).push_back(record);
			}
			(void)staticPartition->SetReferenceFrame(referenceFrame);
			(void)movablePartition->SetReferenceFrame(referenceFrame);
			staticPartition->Rebuild(staticRecords);
			movablePartition->Rebuild(movableRecords);
			++staticPartitionRevision;
			++movablePartitionRevision;
			hasBuiltIndex = true;
			RefreshAggregateDiagnostics(SpatialRefreshMode::Full);
			const auto end = std::chrono::steady_clock::now();
			diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
			return;
		}
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
			if (record.frameGeneration != referenceFrame.generation || !FitsReferenceFrame(record, referenceFrame))
			{
				// A stale local frame is not an uncertainty fallback: returning it
				// could select an object at the wrong distant world location.
				++diagnostics.rejectedObjectCount;
				continue;
			}
			recordsById[record.objectId] = record;
			diagnostics.indexedObjectCount++;
			diagnostics.staticObjectCount += record.isStatic ? 1u : 0u;
			diagnostics.dynamicObjectCount += record.isStatic ? 0u : 1u;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!record.localBounds.IsUsable() || IsFallbackRecord(record))
			{
				fallbackObjectIds.push_back(record.objectId);
				nodeByObjectId[record.objectId] = -1;
				diagnostics.fallbackObjectCount++;
				diagnostics.invalidBoundsObjectCount += record.localBounds.IsUsable() ? 0u : 1u;
				continue;
			}
			rootBounds = rootBounds.IsUsable() ? Merge(rootBounds, record.localBounds) : record.localBounds;
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
			if (record.objectId != TerraEngine::EMPTY_UUID && record.frameGeneration == referenceFrame.generation
				&& FitsReferenceFrame(record, referenceFrame) && record.localBounds.IsUsable() && !IsFallbackRecord(record))
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
		if (!partitionLeaf)
		{
			if (forceFullRefresh || !hasBuiltIndex)
			{
				Rebuild(records);
				return;
			}
			const auto start = std::chrono::steady_clock::now();
			EnsurePartitions();
			std::vector<SpatialObjectRecord> staticRecords;
			std::vector<SpatialObjectRecord> movableRecords;
			staticRecords.reserve(records.size());
			movableRecords.reserve(records.size());
			for (const SpatialObjectRecord& record : records)
			{
				(record.isStatic ? staticRecords : movableRecords).push_back(record);
			}
			(void)staticPartition->SetReferenceFrame(referenceFrame);
			(void)movablePartition->SetReferenceFrame(referenceFrame);
			staticPartition->Refresh(staticRecords);
			movablePartition->Refresh(movableRecords);
			const SpatialRefreshMode staticMode = staticPartition->diagnostics.refreshMode;
			const SpatialRefreshMode movableMode = movablePartition->diagnostics.refreshMode;
			if (staticMode != SpatialRefreshMode::Unchanged)
			{
				++staticPartitionRevision;
			}
			if (movableMode != SpatialRefreshMode::Unchanged)
			{
				++movablePartitionRevision;
			}
			const SpatialRefreshMode aggregateMode = (staticMode == SpatialRefreshMode::Full || movableMode == SpatialRefreshMode::Full)
				? SpatialRefreshMode::Full
				: ((staticMode == SpatialRefreshMode::Incremental || movableMode == SpatialRefreshMode::Incremental)
					? SpatialRefreshMode::Incremental
					: SpatialRefreshMode::Unchanged);
			RefreshAggregateDiagnostics(aggregateMode);
			const auto end = std::chrono::steady_clock::now();
			diagnostics.lastUpdateDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
			return;
		}
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
		if (!partitionLeaf)
		{
			EnsurePartitions();
			if (staticPartition->recordsById.contains(objectId))
			{
				staticPartition->MarkDirty(objectId);
			}
			else if (movablePartition->recordsById.contains(objectId))
			{
				movablePartition->MarkDirty(objectId);
			}
			diagnostics.dirtyObjectCount = staticPartition->diagnostics.dirtyObjectCount + movablePartition->diagnostics.dirtyObjectCount;
			return;
		}
		if (objectId != TerraEngine::EMPTY_UUID)
		{
			dirtyObjectIds.insert(objectId);
			diagnostics.dirtyObjectCount = static_cast<std::uint32_t>(dirtyObjectIds.size());
		}
	}

	void SpatialIndex::InvalidateForHierarchySnapshotRebuild(std::uint64_t hierarchyRevision)
	{
		if (!partitionLeaf)
		{
			if (hierarchyRevision == 0 || hierarchyRevision == lastHierarchySnapshotRevision)
			{
				return;
			}
			lastHierarchySnapshotRevision = hierarchyRevision;
			hasBuiltIndex = false;
			++diagnostics.revisionMismatchFallbackCount;
			return;
		}
		if (hierarchyRevision == 0 || hierarchyRevision == lastHierarchySnapshotRevision)
		{
			return;
		}
		lastHierarchySnapshotRevision = hierarchyRevision;
		hasBuiltIndex = false;
		std::scoped_lock lock(diagnosticsMutex);
		// A hierarchy snapshot rebuild can move whole branches even when an
		// individual bounds record looks unchanged. Force the next refresh to
		// rebuild from the complete snapshot so spatial consumers never query a
		// stale tree after transform ordering changes.
		++diagnostics.revisionMismatchFallbackCount;
	}

	bool SpatialIndex::RebuildCompleteSceneSnapshot(const std::vector<SpatialObjectRecord>& records, std::uint64_t sceneGeneration)
	{
		if (partitionLeaf || sceneGeneration == 0U)
		{
			return false;
		}
		// Validate identities before clearing the previous index.  Duplicate or
		// empty ids would otherwise turn a bad prepared packet into a partial
		// visible broadphase snapshot.
		std::unordered_set<TerraEngine::UUID> seen;
		seen.reserve(records.size());
		for (const SpatialObjectRecord& record : records)
		{
			if (record.objectId == TerraEngine::EMPTY_UUID || !seen.insert(record.objectId).second)
			{
				return false;
			}
		}
		Rebuild(records);
		boundsSceneGeneration = sceneGeneration;
		lastBoundsPublicationGeneration = sceneGeneration;
		return true;
	}

	bool SpatialIndex::ApplyBoundsPublicationBatch(const SpatialBoundsPublicationBatch& batch)
	{
		if (TerraEngine::Scheduling::IsWorkerSafetyScopeActive())
		{
			TerraEngine::Scheduling::ReportWorkerSafetyViolation(
				TerraEngine::Scheduling::WorkerSafetySeverity::Error,
				"SpatialIndex.ApplyBoundsPublicationBatch",
				"Worker code attempted to publish changed bounds directly.",
				"Prepare the value-only batch on workers and apply it at a protected scheduler boundary.");
			return false;
		}
		if (partitionLeaf || batch.sceneGeneration == 0u || batch.publicationGeneration == 0u
			|| batch.publicationGeneration <= lastBoundsPublicationGeneration
			|| (boundsSceneGeneration != 0u && boundsSceneGeneration != batch.sceneGeneration))
		{
			return false;
		}
		EnsurePartitions();

		std::vector<SpatialBoundsPublicationChange> changes = batch.changes;
		std::sort(changes.begin(), changes.end(), [](const SpatialBoundsPublicationChange& left, const SpatialBoundsPublicationChange& right) {
			return UuidLess(left.record.objectId, right.record.objectId);
		});
		for (std::size_t index = 0; index < changes.size(); ++index)
		{
			const SpatialBoundsPublicationChange& change = changes[index];
			if (change.record.objectId == TerraEngine::EMPTY_UUID
				|| (index > 0u && changes[index - 1u].record.objectId == change.record.objectId))
			{
				return false;
			}
			const bool inStatic = staticPartition->recordsById.contains(change.record.objectId);
			const bool inMovable = movablePartition->recordsById.contains(change.record.objectId);
			if ((change.kind == SpatialBoundsChangeKind::Add && (inStatic || inMovable))
				|| (change.kind != SpatialBoundsChangeKind::Add && !inStatic && !inMovable))
			{
				return false;
			}
		}

		// Preflight completed without touching live storage. The following value
		// maps are the full candidate state for each independently published tree.
		auto staticRecordsById = staticPartition->recordsById;
		auto movableRecordsById = movablePartition->recordsById;
		bool touchesStatic = false;
		bool touchesMovable = false;
		for (const SpatialBoundsPublicationChange& change : changes)
		{
			const TerraEngine::UUID& objectId = change.record.objectId;
			if (change.kind != SpatialBoundsChangeKind::Remove
				&& (change.record.frameGeneration != referenceFrame.generation || !FitsReferenceFrame(change.record, referenceFrame)))
			{
				return false;
			}
			if (auto existing = staticRecordsById.find(objectId); existing != staticRecordsById.end())
			{
				staticRecordsById.erase(existing);
				touchesStatic = true;
			}
			if (auto existing = movableRecordsById.find(objectId); existing != movableRecordsById.end())
			{
				movableRecordsById.erase(existing);
				touchesMovable = true;
			}
			if (change.kind != SpatialBoundsChangeKind::Remove)
			{
				if (change.record.isStatic)
				{
					staticRecordsById[objectId] = change.record;
					touchesStatic = true;
				}
				else
				{
					movableRecordsById[objectId] = change.record;
					touchesMovable = true;
				}
			}
		}

		const auto makeSortedRecords = [](const auto& recordsById) {
			std::vector<SpatialObjectRecord> records;
			records.reserve(recordsById.size());
			for (const auto& [objectId, record] : recordsById)
			{
				(void)objectId;
				records.push_back(record);
			}
			std::sort(records.begin(), records.end(), [](const SpatialObjectRecord& left, const SpatialObjectRecord& right) {
				return UuidLess(left.objectId, right.objectId);
			});
			return records;
		};

		if (touchesStatic)
		{
			staticPartition->Refresh(makeSortedRecords(staticRecordsById));
			++staticPartitionRevision;
		}
		if (touchesMovable)
		{
			movablePartition->Refresh(makeSortedRecords(movableRecordsById));
			++movablePartitionRevision;
		}
		lastBoundsPublicationGeneration = batch.publicationGeneration;
		boundsSceneGeneration = batch.sceneGeneration;
		hasBuiltIndex = true;
		lastBoundsPublicationOrder.clear();
		lastBoundsPublicationOrder.reserve(changes.size());
		for (const SpatialBoundsPublicationChange& change : changes)
		{
			lastBoundsPublicationOrder.push_back(change.record.objectId);
		}
		const bool full = (touchesStatic && staticPartition->diagnostics.refreshMode == SpatialRefreshMode::Full)
			|| (touchesMovable && movablePartition->diagnostics.refreshMode == SpatialRefreshMode::Full);
		RefreshAggregateDiagnostics(full ? SpatialRefreshMode::Full : SpatialRefreshMode::Incremental);
		return true;
	}

	SpatialPartitionRevisionSnapshot SpatialIndex::GetPartitionRevisionSnapshot() const
	{
		SpatialPartitionRevisionSnapshot snapshot;
		snapshot.staticRevision = staticPartitionRevision;
		snapshot.movableRevision = movablePartitionRevision;
		snapshot.publicationGeneration = lastBoundsPublicationGeneration;
		snapshot.staticRecordCount = staticPartition ? staticPartition->recordsById.size() : 0u;
		snapshot.movableRecordCount = movablePartition ? movablePartition->recordsById.size() : 0u;
		return snapshot;
	}

	std::vector<TerraEngine::UUID> SpatialIndex::GetLastBoundsPublicationOrder() const
	{
		return lastBoundsPublicationOrder;
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
			const int childSlot = FindContainingChild(node, record.localBounds);
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
		if (record.objectId == TerraEngine::EMPTY_UUID || record.frameGeneration != referenceFrame.generation
			|| !FitsReferenceFrame(record, referenceFrame))
		{
			return false;
		}

		recordsById[record.objectId] = record;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!record.localBounds.IsUsable() || IsFallbackRecord(record))
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
		if (record.frameGeneration != referenceFrame.generation || !FitsReferenceFrame(record, referenceFrame))
		{
			return false;
		}
		if (!record.localBounds.IsUsable() || IsFallbackRecord(record))
		{
			return true;
		}

		// The query traversal starts at the current root loose bounds. Any new
		// indexable record outside that root could be missed by queries, so fall
		// back to a full rebuild to grow the tree around the latest aggregate.
		return !nodes.empty() && Contains(nodes.front().looseBounds, record.localBounds);
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

	void SpatialIndex::RefreshAggregateDiagnostics(SpatialRefreshMode requestedMode)
	{
		if (partitionLeaf || !staticPartition || !movablePartition)
		{
			return;
		}
		const SpatialDiagnosticsSnapshot& statics = staticPartition->diagnostics;
		const SpatialDiagnosticsSnapshot& movables = movablePartition->diagnostics;
		SpatialDiagnosticsSnapshot aggregate;
		aggregate.indexedObjectCount = statics.indexedObjectCount + movables.indexedObjectCount;
		aggregate.staticObjectCount = statics.indexedObjectCount;
		aggregate.dynamicObjectCount = movables.indexedObjectCount;
		aggregate.dirtyObjectCount = statics.dirtyObjectCount + movables.dirtyObjectCount;
		aggregate.transformDirtyRootCount = statics.transformDirtyRootCount + movables.transformDirtyRootCount;
		aggregate.transformExpandedObjectCount = statics.transformExpandedObjectCount + movables.transformExpandedObjectCount;
		aggregate.transformProcessedObjectCount = statics.transformProcessedObjectCount + movables.transformProcessedObjectCount;
		aggregate.transformSkippedCleanObjectCount = statics.transformSkippedCleanObjectCount + movables.transformSkippedCleanObjectCount;
		aggregate.transformHierarchyLevelCount = std::max(statics.transformHierarchyLevelCount, movables.transformHierarchyLevelCount);
		aggregate.boundsRecomputedObjectCount = statics.boundsRecomputedObjectCount + movables.boundsRecomputedObjectCount;
		aggregate.invalidTransformObjectCount = statics.invalidTransformObjectCount + movables.invalidTransformObjectCount;
		aggregate.invalidBoundsObjectCount = statics.invalidBoundsObjectCount + movables.invalidBoundsObjectCount;
		aggregate.addedObjectCount = statics.addedObjectCount + movables.addedObjectCount;
		aggregate.updatedObjectCount = statics.updatedObjectCount + movables.updatedObjectCount;
		aggregate.removedObjectCount = statics.removedObjectCount + movables.removedObjectCount;
		aggregate.unchangedObjectCount = statics.unchangedObjectCount + movables.unchangedObjectCount;
		aggregate.fullRefreshObjectCount = statics.fullRefreshObjectCount + movables.fullRefreshObjectCount;
		aggregate.incrementalRefreshCount = requestedMode == SpatialRefreshMode::Incremental ? 1u : 0u;
		aggregate.fullRefreshCount = requestedMode == SpatialRefreshMode::Full ? 1u : 0u;
		aggregate.revisionMismatchFallbackCount = statics.revisionMismatchFallbackCount + movables.revisionMismatchFallbackCount;
		aggregate.fallbackObjectCount = statics.fallbackObjectCount + movables.fallbackObjectCount;
		aggregate.largeObjectCount = statics.largeObjectCount + movables.largeObjectCount;
		aggregate.maxNodeDepth = std::max(statics.maxNodeDepth, movables.maxNodeDepth);
		aggregate.refreshMode = requestedMode;
		diagnostics = aggregate;
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
				return Overlaps(shape.aabb, record.localBounds) ? std::optional<float>(0.0f) : std::nullopt;
			case QueryKind::Sphere:
				return Intersects(shape.sphere, record.localBounds) ? std::optional<float>(glm::length(record.localBounds.Center() - shape.sphere.center)) : std::nullopt;
			case QueryKind::Frustum:
				return Intersects(shape.frustum, record.localBounds) ? std::optional<float>(0.0f) : std::nullopt;
			case QueryKind::Ray:
				return Intersects(shape.ray, record.localBounds);
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
		hit.bounds = record.localBounds;
		hit.canonicalOwner = record.canonicalOwner;
		hit.frameGeneration = record.frameGeneration;
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
		if (!partitionLeaf)
		{
			SpatialQueryFilter unboundedFilter = filter;
			unboundedFilter.maxResults = 0u;
			const auto appendPartition = [&](const std::unique_ptr<SpatialIndex>& partition) {
				if (!partition)
				{
					return;
				}
				if (!partition->nodes.empty())
				{
					partition->QueryNode(0, shape, unboundedFilter, result, seen);
				}
				for (const TerraEngine::UUID& objectId : partition->fallbackObjectIds)
				{
					if (!seen.insert(objectId).second)
					{
						continue;
					}
					partition->AppendHit(result, partition->recordsById.at(objectId), 0.0f, unboundedFilter);
				}
			};
			appendPartition(staticPartition);
			appendPartition(movablePartition);
			std::sort(result.hits.begin(), result.hits.end(), [&](const SpatialQueryHit& left, const SpatialQueryHit& right) {
				if (filter.sortMode == SpatialSortMode::DistanceAscending && left.distance != right.distance)
				{
					return left.distance < right.distance;
				}
				return UuidLess(left.objectId, right.objectId);
			});
			if (filter.maxResults != 0u && result.hits.size() > filter.maxResults)
			{
				result.rejectedCount += static_cast<std::uint32_t>(result.hits.size() - filter.maxResults);
				result.hits.resize(filter.maxResults);
			}
			const auto end = std::chrono::steady_clock::now();
			std::scoped_lock lock(diagnosticsMutex);
			diagnostics.visibleCandidateCount = static_cast<std::uint32_t>(result.hits.size());
			diagnostics.rejectedObjectCount = result.rejectedCount;
			diagnostics.fallbackObjectCount = result.fallbackCount;
			diagnostics.lastQueryDurationMs = std::chrono::duration<double, std::milli>(end - start).count();
			return;
		}
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

	bool EditorSelectableSpatialSnapshot::IsValid() const
	{
		return generation != 0u && index != nullptr;
	}

	std::uint64_t EditorSelectableSpatialSnapshot::Generation() const
	{
		return generation;
	}

	std::size_t EditorSelectableSpatialSnapshot::RecordCount() const
	{
		return recordCount;
	}

	SpatialQueryResult EditorSelectableSpatialSnapshot::QueryAABB(const SpatialAABB& area, const SpatialQueryFilter& filter) const
	{
		if (!index)
		{
			SpatialQueryResult result{};
			result.validInput = false;
			return result;
		}
		return index->QueryAABB(area, filter);
	}

	SpatialQueryResult EditorSelectableSpatialSnapshot::QuerySphere(const SpatialSphere& sphere, const SpatialQueryFilter& filter) const
	{
		if (!index)
		{
			SpatialQueryResult result{};
			result.validInput = false;
			return result;
		}
		return index->QuerySphere(sphere, filter);
	}

	SpatialQueryResult EditorSelectableSpatialSnapshot::QueryFrustum(const SpatialFrustum& frustum, const SpatialQueryFilter& filter) const
	{
		if (!index)
		{
			SpatialQueryResult result{};
			result.validInput = false;
			return result;
		}
		return index->QueryFrustum(frustum, filter);
	}

	SpatialQueryResult EditorSelectableSpatialSnapshot::QueryRay(const SpatialRay& ray, const SpatialQueryFilter& filter) const
	{
		if (!index)
		{
			SpatialQueryResult result{};
			result.validInput = false;
			return result;
		}
		return index->QueryRay(ray, filter);
	}

	bool EditorSelectableSpatialSnapshotPublisher::Publish(std::vector<SpatialObjectRecord> records, std::uint64_t generation)
	{
		if (generation == 0u)
		{
			return false;
		}
		{
			std::scoped_lock lock(publishMutex);
			if (latest && generation <= latest->Generation())
			{
				// Reject an already-stale generation before doing any index build work.
				return false;
			}
		}

		// Only explicitly editor-selectable records enter this snapshot. A record
		// does not need to be renderable, which is the key distinction from the
		// renderer-owned visibility index.
		records.erase(std::remove_if(records.begin(), records.end(), [](const SpatialObjectRecord& record) {
			return record.objectId == TerraEngine::EMPTY_UUID
				|| (record.categoryMask & SpatialCategoryEditorSelectable) == 0u;
		}), records.end());

		std::sort(records.begin(), records.end(), [](const SpatialObjectRecord& left, const SpatialObjectRecord& right) {
			return UuidLess(left.objectId, right.objectId);
		});
		const auto duplicate = std::adjacent_find(records.begin(), records.end(), [](const SpatialObjectRecord& left, const SpatialObjectRecord& right) {
			return left.objectId == right.objectId;
		});
		if (duplicate != records.end())
		{
			return false;
		}

		auto preparedIndex = std::make_shared<SpatialIndex>();
		preparedIndex->Rebuild(records);
		auto preparedSnapshot = std::make_shared<EditorSelectableSpatialSnapshot>();
		preparedSnapshot->generation = generation;
		preparedSnapshot->recordCount = records.size();
		preparedSnapshot->index = std::move(preparedIndex);

		std::scoped_lock lock(publishMutex);
		if (latest && generation <= latest->Generation())
		{
			return false;
		}
		latest = std::move(preparedSnapshot);
		return true;
	}

	std::shared_ptr<const EditorSelectableSpatialSnapshot> EditorSelectableSpatialSnapshotPublisher::Latest() const
	{
		std::scoped_lock lock(publishMutex);
		return latest;
	}

	namespace
	{
		template<typename Query>
		CrossRegionSpatialQueryResult QuerySparseResidentRegions(
			const PublishedWorldRegionResidency& residency,
			std::vector<WorldRegionKey> keys,
			const SpatialQueryFilter& filter,
			Query&& query)
		{
			CrossRegionSpatialQueryResult output{};
			std::sort(keys.begin(), keys.end());
			keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
			std::unordered_set<TerraEngine::UUID> seen;
			for (const WorldRegionKey& key : keys)
			{
				const auto region = residency.residentRegions.find(key);
				if (region == residency.residentRegions.end() || !region->second) continue;
				++output.visitedRegionCount;
				SpatialIndex fallbackIndex;
				if (region->second->staticSpatialIndex && region->second->movableSpatialIndex)
				{
					// Snapshot indices were built by the worker candidate and are already
					// immutable. Query both partitions before UUID merging below.
					SpatialQueryResult staticResult = query(*region->second->staticSpatialIndex);
					SpatialQueryResult movableResult = query(*region->second->movableSpatialIndex);
					output.result.validInput = output.result.validInput && staticResult.validInput && movableResult.validInput;
					output.result.rejectedCount += staticResult.rejectedCount + movableResult.rejectedCount;
					output.result.fallbackCount += staticResult.fallbackCount + movableResult.fallbackCount;
					staticResult.hits.insert(staticResult.hits.end(), std::make_move_iterator(movableResult.hits.begin()), std::make_move_iterator(movableResult.hits.end()));
					for (SpatialQueryHit& hit : staticResult.hits) if (seen.insert(hit.objectId).second) output.result.hits.push_back(std::move(hit));
					continue;
				}
				(void)fallbackIndex.SetReferenceFrame(region->second->referenceFrame);
				fallbackIndex.Rebuild(region->second->spatialRecords);
				SpatialQueryResult local = query(fallbackIndex);
				output.result.validInput = output.result.validInput && local.validInput;
				output.result.rejectedCount += local.rejectedCount;
				output.result.fallbackCount += local.fallbackCount;
				for (SpatialQueryHit& hit : local.hits) if (seen.insert(hit.objectId).second) output.result.hits.push_back(std::move(hit));
			}
			// The proxy collection is one bounded coarse index, not a directory-wide
			// scan. A temporary index keeps proxy intersection behavior identical to
			// ordinary local records while UUID filtering removes owner/cover copies.
			if (!residency.coarseProxies.empty())
			{
				std::vector<SpatialObjectRecord> proxies;
				for (const OversizedSpatialProxy& proxy : residency.coarseProxies)
				{
					SpatialObjectRecord record{}; record.objectId = proxy.objectId; record.canonicalOwner = proxy.canonicalOwner; record.localBounds = proxy.localBounds; record.frameGeneration = 0;
					proxies.push_back(std::move(record));
				}
				SpatialIndex proxyIndex; proxyIndex.Rebuild(proxies);
				SpatialQueryResult local = query(proxyIndex);
				output.coarseProxyCount = static_cast<std::uint32_t>(proxies.size());
				for (SpatialQueryHit& hit : local.hits) if (seen.insert(hit.objectId).second) output.result.hits.push_back(std::move(hit));
			}
			std::sort(output.result.hits.begin(), output.result.hits.end(), [](const SpatialQueryHit& left, const SpatialQueryHit& right) { return UuidLess(left.objectId, right.objectId); });
			return output;
		}
	}

	CrossRegionSpatialQueryResult QueryResidentRegionsAABB(const PublishedWorldRegionResidency& residency, std::vector<WorldRegionKey> keys, const SpatialAABB& area, const SpatialQueryFilter& filter)
	{
		return QuerySparseResidentRegions(residency, std::move(keys), filter, [&area, &filter](const SpatialIndex& index) { return index.QueryAABB(area, filter); });
	}
	CrossRegionSpatialQueryResult QueryResidentRegionsSphere(const PublishedWorldRegionResidency& residency, std::vector<WorldRegionKey> keys, const SpatialSphere& sphere, const SpatialQueryFilter& filter)
	{
		return QuerySparseResidentRegions(residency, std::move(keys), filter, [&sphere, &filter](const SpatialIndex& index) { return index.QuerySphere(sphere, filter); });
	}
	CrossRegionSpatialQueryResult QueryResidentRegionsFrustum(const PublishedWorldRegionResidency& residency, std::vector<WorldRegionKey> keys, const SpatialFrustum& frustum, const SpatialQueryFilter& filter)
	{
		return QuerySparseResidentRegions(residency, std::move(keys), filter, [&frustum, &filter](const SpatialIndex& index) { return index.QueryFrustum(frustum, filter); });
	}
	CrossRegionSpatialQueryResult QueryResidentRegionsRay(const PublishedWorldRegionResidency& residency, std::vector<WorldRegionKey> keys, const SpatialRay& ray, const SpatialQueryFilter& filter)
	{
		return QuerySparseResidentRegions(residency, std::move(keys), filter, [&ray, &filter](const SpatialIndex& index) { return index.QueryRay(ray, filter); });
	}
}
