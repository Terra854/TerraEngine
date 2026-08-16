// SceneViewMarkerPipeline.cpp - Incremental marker caching and bounded presentation.

#include "SceneViewMarkerPipeline.h"

#include "SceneViewOverlayMarkers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>

namespace TerraEngine::Editor::SceneViewOverlay
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		[[nodiscard]] bool UuidLess(const TerraEngine::UUID& left, const TerraEngine::UUID& right)
		{
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
		}

		[[nodiscard]] bool UuidValid(const TerraEngine::UUID& id)
		{
			return id != TerraEngine::EMPTY_UUID;
		}

		[[nodiscard]] bool Vec3Equal(const glm::vec3& left, const glm::vec3& right)
		{
			return glm::all(glm::equal(left, right));
		}

		[[nodiscard]] bool BoundsEqual(
			const std::optional<TerraEngine::Scene::SpatialAABB>& left,
			const std::optional<TerraEngine::Scene::SpatialAABB>& right)
		{
			if (left.has_value() != right.has_value())
			{
				return false;
			}
			if (!left)
			{
				return true;
			}
			return left->state == right->state
				&& left->source == right->source
				&& Vec3Equal(left->min, right->min)
				&& Vec3Equal(left->max, right->max);
		}

		[[nodiscard]] TerraEngine::Profiling::PerformanceStageRecord MakeStage(
			std::string stageName,
			Clock::time_point begin,
			std::uint64_t workItemCount,
			bool sampled = false)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - begin);
			TerraEngine::Profiling::PerformanceStageRecord record;
			record.domain = TerraEngine::Profiling::PerformanceStageDomain::SceneView;
			record.stageName = std::move(stageName);
			record.duration.availability = TerraEngine::Profiling::TelemetryAvailability::Available;
			record.duration.nanoseconds = static_cast<std::uint64_t>(std::max<std::int64_t>(0, elapsed.count()));
			record.workloadName = "Scene View marker preparation";
			record.workItemCount = workItemCount;
			record.sampled = sampled;
			return record;
		}

		[[nodiscard]] bool EntryPriorityLess(const MarkerScreenEntry& left, const MarkerScreenEntry& right)
		{
			if (left.reserved != right.reserved)
			{
				return left.reserved;
			}
			if (left.priority != right.priority)
			{
				return left.priority > right.priority;
			}
			if (left.depth != right.depth)
			{
				return left.depth < right.depth;
			}
			if (left.objectId != right.objectId)
			{
				return UuidLess(left.objectId, right.objectId);
			}
			return left.displayNameId < right.displayNameId;
		}

		[[nodiscard]] std::pair<std::int32_t, std::int32_t> CellFor(const glm::vec2& point, float cellSize)
		{
			return {
				static_cast<std::int32_t>(std::floor(point.x / cellSize)),
				static_cast<std::int32_t>(std::floor(point.y / cellSize))
			};
		}

		[[nodiscard]] auto FindCell(const MarkerScreenIndex& index, std::int32_t x, std::int32_t y)
		{
			return std::lower_bound(index.cells.begin(), index.cells.end(), std::pair{x, y}, [](const MarkerScreenCell& cell, const auto& coordinate) {
				return std::pair{cell.x, cell.y} < coordinate;
			});
		}

		void SortAndUnique(std::vector<TerraEngine::UUID>& ids)
		{
			std::sort(ids.begin(), ids.end(), UuidLess);
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
		}

		[[nodiscard]] bool LargeSceneModeEffective(const SceneOverlayPreference& preference, std::size_t objectCount)
		{
			switch (preference.largeSceneMode)
			{
			case LargeSceneMode::On:
				return true;
			case LargeSceneMode::Off:
				return false;
			case LargeSceneMode::Automatic:
			default:
				return objectCount >= preference.largeSceneObjectThreshold;
			}
		}

		[[nodiscard]] MarkerPrimitiveTier DenseTier(const SceneOverlayPreference& preference)
		{
			switch (preference.densePrimitiveTier)
			{
			case DensePrimitiveTier::Full:
				return MarkerPrimitiveTier::FullIcon;
			case DensePrimitiveTier::Point:
				return MarkerPrimitiveTier::Point;
			case DensePrimitiveTier::Simple:
			default:
				return MarkerPrimitiveTier::SimpleIcon;
			}
		}
	}

	bool MarkerViewportKey::operator==(const MarkerViewportKey& other) const
	{
		return SameProjectionInputs(other) && pointerCell == other.pointerCell;
	}

	bool MarkerViewportKey::SameProjectionInputs(const MarkerViewportKey& other) const
	{
		return viewGeneration == other.viewGeneration
			&& projectionGeneration == other.projectionGeneration
			&& glm::all(glm::equal(viewportRectangle, other.viewportRectangle))
			&& projectionMode == other.projectionMode
			&& overlayPreferenceGeneration == other.overlayPreferenceGeneration
			&& markerSpatialPublicationGeneration == other.markerSpatialPublicationGeneration
			&& markerPosePublicationGeneration == other.markerPosePublicationGeneration;
	}

	MarkerStringId MarkerPipeline::InternString(std::string_view value)
	{
		if (value.empty())
		{
			return kInvalidMarkerStringId;
		}
		const auto found = stringIds.find(std::string(value));
		if (found != stringIds.end())
		{
			return found->second;
		}
		const MarkerStringId id = static_cast<MarkerStringId>(strings.size());
		strings.emplace_back(value);
		stringIds.emplace(strings.back(), id);
		return id;
	}

	bool MarkerPipeline::UpsertStaticRecord(const MarkerStaticInput& input)
	{
		if (!UuidValid(input.objectId) || input.objectLifetimeGeneration == 0u || input.category == MarkerCategory::Unsupported)
		{
			return false;
		}

		const auto existing = staticRecords.find(input.objectId);
		if (existing != staticRecords.end())
		{
			if (input.objectLifetimeGeneration < existing->second.objectLifetimeGeneration)
			{
				return false;
			}
			if (input.objectLifetimeGeneration == existing->second.objectLifetimeGeneration
				&& (input.componentGeneration < existing->second.componentGeneration
					|| input.metadataGeneration < existing->second.metadataGeneration))
			{
				return false;
			}
		}

		MarkerStaticRecord next;
		next.objectId = input.objectId;
		next.objectLifetimeGeneration = input.objectLifetimeGeneration;
		next.entityReference = input.entityReference;
		next.componentGeneration = input.componentGeneration;
		next.metadataGeneration = input.metadataGeneration;
		next.category = input.category;
		next.displayNameId = InternString(input.displayName);
		next.tooltipId = InternString(input.tooltip);
		next.basePriority = MarkerCategoryPriority(input.category, input.selected);
		next.alwaysReserved = IsMarkerReserved(input.category, input.selected);

		if (existing != staticRecords.end())
		{
			const MarkerStaticRecord& current = existing->second;
			const bool unchanged = current.objectLifetimeGeneration == next.objectLifetimeGeneration
				&& current.entityReference == next.entityReference
				&& current.componentGeneration == next.componentGeneration
				&& current.metadataGeneration == next.metadataGeneration
				&& current.category == next.category
				&& current.displayNameId == next.displayNameId
				&& current.tooltipId == next.tooltipId
				&& current.basePriority == next.basePriority
				&& current.alwaysReserved == next.alwaysReserved;
			if (unchanged)
			{
				return false;
			}
		}

		staticRecords[input.objectId] = std::move(next);
		++staticPublicationGeneration;
		InvalidateViewportCache();
		return true;
	}

	bool MarkerPipeline::UpsertPoseRecord(const MarkerPoseRecord& pose)
	{
		if (!UuidValid(pose.objectId) || pose.objectLifetimeGeneration == 0u || !TerraEngine::Scene::IsFinite(pose.localPosition))
		{
			return false;
		}
		if (pose.localBounds && pose.localBounds->state == TerraEngine::Scene::SpatialBoundsState::Invalid)
		{
			return false;
		}
		const auto staticRecord = staticRecords.find(pose.objectId);
		if (staticRecord == staticRecords.end() || staticRecord->second.objectLifetimeGeneration != pose.objectLifetimeGeneration)
		{
			return false;
		}

		const auto existing = poseRecords.find(pose.objectId);
		if (existing != poseRecords.end())
		{
			if (pose.objectLifetimeGeneration < existing->second.objectLifetimeGeneration
				|| (pose.objectLifetimeGeneration == existing->second.objectLifetimeGeneration
					&& (pose.transformRevision < existing->second.transformRevision
						|| pose.hierarchyGeneration < existing->second.hierarchyGeneration
						|| pose.parentWorldGeneration < existing->second.parentWorldGeneration)))
			{
				return false;
			}
			const MarkerPoseRecord& current = existing->second;
			const bool unchanged = current.parentObjectId == pose.parentObjectId
				&& current.objectLifetimeGeneration == pose.objectLifetimeGeneration
				&& current.transformRevision == pose.transformRevision
				&& current.hierarchyGeneration == pose.hierarchyGeneration
				&& current.parentWorldGeneration == pose.parentWorldGeneration
				&& current.canonicalPosition == pose.canonicalPosition
				&& Vec3Equal(current.localPosition, pose.localPosition)
				&& BoundsEqual(current.localBounds, pose.localBounds)
				&& current.movable == pose.movable;
			if (unchanged)
			{
				return false;
			}
		}

		poseRecords[pose.objectId] = pose;
		++posePublicationGeneration;
		InvalidateViewportCache();
		return true;
	}

	MarkerInvalidationResult MarkerPipeline::ApplySceneChanges(const TerraEngine::Scene::SceneChangePacket& packet)
	{
		MarkerExactChangeSet changes;
		changes.sceneGeneration = packet.SceneGeneration();
		auto appendIds = [](const auto& snapshots, std::vector<TerraEngine::UUID>& output) {
			output.reserve(output.size() + snapshots.size());
			for (const TerraEngine::Scene::SceneObjectRevisionSnapshot& snapshot : snapshots)
			{
				output.push_back(snapshot.objectId);
			}
		};
		appendIds(packet.Added(), changes.addedObjectIds);
		appendIds(packet.Removed(), changes.removedObjectIds);
		appendIds(packet.MetadataChanged(), changes.metadataChangedObjectIds);
		appendIds(packet.ComponentChanged(), changes.componentChangedObjectIds);
		appendIds(packet.TransformChanged(), changes.transformChangedObjectIds);
		appendIds(packet.HierarchyChanged(), changes.hierarchyChangedObjectIds);
		appendIds(packet.BoundsChanged(), changes.boundsChangedObjectIds);
		return ApplyExactChanges(changes);
	}

	bool MarkerPipeline::IsDescendantOfAny(const MarkerPoseRecord& pose, std::span<const TerraEngine::UUID> roots) const
	{
		TerraEngine::UUID parent = pose.parentObjectId;
		std::size_t remaining = poseRecords.size();
		while (UuidValid(parent) && remaining-- > 0u)
		{
			if (std::find(roots.begin(), roots.end(), parent) != roots.end())
			{
				return true;
			}
			const auto ancestor = poseRecords.find(parent);
			if (ancestor == poseRecords.end() || ancestor->second.parentObjectId == parent)
			{
				break;
			}
			parent = ancestor->second.parentObjectId;
		}
		return false;
	}

	MarkerInvalidationResult MarkerPipeline::ApplyExactChanges(const MarkerExactChangeSet& changes)
	{
		MarkerInvalidationResult result;
		if (changes.sceneGeneration != 0u && sceneGeneration != 0u && changes.sceneGeneration != sceneGeneration)
		{
			Clear();
			result.sceneReplaced = true;
		}
		if (changes.sceneGeneration != 0u)
		{
			sceneGeneration = changes.sceneGeneration;
		}

		result.staticRefreshObjectIds = changes.addedObjectIds;
		result.staticRefreshObjectIds.insert(result.staticRefreshObjectIds.end(), changes.metadataChangedObjectIds.begin(), changes.metadataChangedObjectIds.end());
		result.staticRefreshObjectIds.insert(result.staticRefreshObjectIds.end(), changes.componentChangedObjectIds.begin(), changes.componentChangedObjectIds.end());
		result.poseRefreshObjectIds = changes.addedObjectIds;
		result.poseRefreshObjectIds.insert(result.poseRefreshObjectIds.end(), changes.transformChangedObjectIds.begin(), changes.transformChangedObjectIds.end());
		result.poseRefreshObjectIds.insert(result.poseRefreshObjectIds.end(), changes.hierarchyChangedObjectIds.begin(), changes.hierarchyChangedObjectIds.end());
		result.poseRefreshObjectIds.insert(result.poseRefreshObjectIds.end(), changes.boundsChangedObjectIds.begin(), changes.boundsChangedObjectIds.end());
		result.removedObjectIds = changes.removedObjectIds;

		// Determine descendants before erasing hierarchy roots. Every traversal is
		// bounded by the retained pose count, so malformed cycles cannot loop forever.
		for (const auto& [objectId, pose] : poseRecords)
		{
			if (IsDescendantOfAny(pose, changes.hierarchyChangedObjectIds))
			{
				result.poseRefreshObjectIds.push_back(objectId);
			}
		}

		SortAndUnique(result.staticRefreshObjectIds);
		SortAndUnique(result.poseRefreshObjectIds);
		SortAndUnique(result.removedObjectIds);

		bool staticChanged = false;
		bool poseChanged = false;
		for (const TerraEngine::UUID& id : result.removedObjectIds)
		{
			staticChanged = staticRecords.erase(id) > 0u || staticChanged;
			poseChanged = poseRecords.erase(id) > 0u || poseChanged;
		}
		for (const TerraEngine::UUID& id : result.staticRefreshObjectIds)
		{
			staticChanged = staticRecords.erase(id) > 0u || staticChanged;
		}
		for (const TerraEngine::UUID& id : result.poseRefreshObjectIds)
		{
			poseChanged = poseRecords.erase(id) > 0u || poseChanged;
		}
		if (staticChanged || !result.staticRefreshObjectIds.empty())
		{
			++staticPublicationGeneration;
		}
		if (poseChanged || !result.poseRefreshObjectIds.empty())
		{
			++posePublicationGeneration;
		}
		if (staticChanged || poseChanged || !result.staticRefreshObjectIds.empty() || !result.poseRefreshObjectIds.empty() || !result.removedObjectIds.empty())
		{
			InvalidateViewportCache();
		}
		return result;
	}

	void MarkerPipeline::Clear()
	{
		staticRecords.clear();
		poseRecords.clear();
		strings.assign(1u, std::string());
		stringIds.clear();
		sceneGeneration = 0u;
		++staticPublicationGeneration;
		++posePublicationGeneration;
		InvalidateViewportCache();
	}

	void MarkerPipeline::InvalidateViewportCache()
	{
		cachedFrame.reset();
		cachedStaticGeneration = 0u;
		cachedPoseGeneration = 0u;
	}

	MarkerFrameResult MarkerPipeline::PrepareViewport(const MarkerViewportQuery& query, const SceneOverlayPreference& rawPreference)
	{
		const Clock::time_point cacheBegin = Clock::now();
		if (cachedFrame
			&& cachedStaticGeneration == staticPublicationGeneration
			&& cachedPoseGeneration == posePublicationGeneration
			&& cachedFrame->candidates.viewportKey.SameProjectionInputs(query.viewportKey))
		{
			MarkerFrameResult result = *cachedFrame;
			result.reusedViewportCache = true;
			result.candidates.viewportKey.pointerCell = query.viewportKey.pointerCell;
			result.screenIndex.viewportKey.pointerCell = query.viewportKey.pointerCell;
			result.stageTelemetry.clear();
			result.stageTelemetry.push_back(MakeStage("SceneView.MarkerCache", cacheBegin, staticRecords.size()));
			// A valid cache hit makes the downstream stages real zero-work paths,
			// rather than unavailable measurements. Publishing typed zero-item
			// records lets the profiler distinguish reuse from missing evidence.
			for (const char* stageName : {
				"SceneView.CandidateQuery",
				"SceneView.Projection",
				"SceneView.Density",
				"SceneView.Emission"})
			{
				result.stageTelemetry.push_back(MakeStage(stageName, Clock::now(), 0u));
			}
			return result;
		}

		MarkerFrameResult result;
		result.stageTelemetry.push_back(MakeStage("SceneView.MarkerCache", cacheBegin, staticRecords.size()));
		const SceneOverlayPreference preference = SanitizePreference(rawPreference);
		result.candidates.viewportKey = query.viewportKey;
		result.candidates.sourceCount = static_cast<std::uint32_t>(std::min<std::size_t>(staticRecords.size(), std::numeric_limits<std::uint32_t>::max()));
		result.candidates.effectiveLargeSceneMode = LargeSceneModeEffective(preference, staticRecords.size());
		result.candidates.effectiveBudget = result.candidates.effectiveLargeSceneMode
			? preference.visibleMarkerBudget
			: std::numeric_limits<std::uint32_t>::max();

		const Clock::time_point queryBegin = Clock::now();
		std::vector<MarkerScreenEntry> reservedEntries;
		std::vector<MarkerScreenEntry> generalEntries;
		reservedEntries.reserve(query.spatialCandidates.hits.size());
		generalEntries.reserve(query.spatialCandidates.hits.size());
		std::unordered_set<TerraEngine::UUID> examined;
		examined.reserve(query.spatialCandidates.hits.size());
		struct ProjectableSource
		{
			const TerraEngine::Scene::SpatialQueryHit* hit = nullptr;
			const MarkerStaticRecord* stable = nullptr;
			const MarkerPoseRecord* pose = nullptr;
		};
		std::vector<ProjectableSource> projectable;
		projectable.reserve(query.spatialCandidates.hits.size());

		for (const TerraEngine::Scene::SpatialQueryHit& hit : query.spatialCandidates.hits)
		{
			if (!examined.insert(hit.objectId).second)
			{
				continue;
			}
			const auto staticIt = staticRecords.find(hit.objectId);
			const auto poseIt = poseRecords.find(hit.objectId);
			if (staticIt == staticRecords.end() || poseIt == poseRecords.end()
				|| staticIt->second.objectLifetimeGeneration != poseIt->second.objectLifetimeGeneration)
			{
				continue;
			}
			if (staticIt->second.category == MarkerCategory::Physics
				&& preference.physicsMarkerMode == PhysicsMarkerMode::SelectedOnly
				&& !staticIt->second.alwaysReserved)
			{
				++result.candidates.budgetRejectedCount;
				continue;
			}
			projectable.push_back({&hit, &staticIt->second, &poseIt->second});
		}
		result.stageTelemetry.push_back(MakeStage("SceneView.CandidateQuery", queryBegin, query.spatialCandidates.hits.size()));

		const Clock::time_point projectionBegin = Clock::now();
		for (const ProjectableSource& source : projectable)
		{
			const TerraEngine::Scene::SpatialQueryHit& hit = *source.hit;
			const MarkerStaticRecord& staticRecord = *source.stable;
			const MarkerPoseRecord& pose = *source.pose;

			const glm::vec4 clip = query.viewProjection * glm::vec4(pose.localPosition, 1.0f);
			if (!std::isfinite(clip.w) || clip.w <= 0.0001f || query.viewportSize.x <= 0.0f || query.viewportSize.y <= 0.0f)
			{
				++result.candidates.frustumRejectedCount;
				continue;
			}
			const glm::vec3 ndc = glm::vec3(clip) / clip.w;
			if (!TerraEngine::Scene::IsFinite(ndc) || ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
			{
				++result.candidates.frustumRejectedCount;
				continue;
			}

			const float distance = glm::length(pose.localPosition - query.cameraPosition);
			if (!std::isfinite(distance) || (preference.maximumMarkerDistance > 0.0f && distance > preference.maximumMarkerDistance))
			{
				++result.candidates.distanceRejectedCount;
				continue;
			}

			float projectedSize = 16.0f;
			const TerraEngine::Scene::SpatialAABB* bounds = pose.localBounds ? &*pose.localBounds : &hit.bounds;
			if (bounds->IsUsable())
			{
				const glm::vec3 diameter = bounds->Extent() * 2.0f;
				projectedSize = std::max({diameter.x, diameter.y, diameter.z}) * query.viewportSize.y / std::max(std::abs(clip.w), 0.0001f);
			}
			if (!std::isfinite(projectedSize) || projectedSize < preference.minimumProjectedSizePixels)
			{
				++result.candidates.sizeRejectedCount;
				continue;
			}

			MarkerScreenEntry entry;
			entry.objectId = hit.objectId;
			entry.screenPosition = glm::vec2(
				query.viewportPosition.x + (ndc.x + 1.0f) * 0.5f * query.viewportSize.x,
				query.viewportPosition.y + (1.0f - ndc.y) * 0.5f * query.viewportSize.y);
			entry.depth = ndc.z;
			entry.distance = distance;
			entry.projectedSizePixels = projectedSize;
			entry.priority = staticRecord.basePriority;
			entry.category = staticRecord.category;
			entry.displayNameId = staticRecord.displayNameId;
			entry.tooltipId = staticRecord.tooltipId;
			entry.hitTestRepresentative = entry.objectId;
			entry.reserved = staticRecord.alwaysReserved;
			entry.densityCandidate = !entry.reserved
				&& (entry.category == MarkerCategory::EmptyObject || entry.category == MarkerCategory::Physics);
			entry.clusterCandidate = entry.densityCandidate
				&& (entry.category != MarkerCategory::Physics || preference.physicsMarkerMode == PhysicsMarkerMode::Clustered);
			if (entry.reserved)
			{
				reservedEntries.push_back(entry);
			}
			else
			{
				generalEntries.push_back(entry);
			}
		}

		const std::uint32_t examinedCount = static_cast<std::uint32_t>(std::min<std::size_t>(examined.size(), std::numeric_limits<std::uint32_t>::max()));
		if (result.candidates.sourceCount > examinedCount)
		{
			result.candidates.frustumRejectedCount += result.candidates.sourceCount - examinedCount;
		}
		result.stageTelemetry.push_back(MakeStage("SceneView.Projection", projectionBegin, projectable.size()));

		std::sort(reservedEntries.begin(), reservedEntries.end(), EntryPriorityLess);
		std::sort(generalEntries.begin(), generalEntries.end(), EntryPriorityLess);
		const std::size_t generalBudget = result.candidates.effectiveBudget == std::numeric_limits<std::uint32_t>::max()
			? generalEntries.size()
			: std::min<std::size_t>(generalEntries.size(), result.candidates.effectiveBudget);
		result.candidates.budgetRejectedCount += static_cast<std::uint32_t>(generalEntries.size() - generalBudget);
		generalEntries.resize(generalBudget);
		for (const MarkerScreenEntry& entry : reservedEntries)
		{
			result.candidates.reservedMarkerIds.push_back(entry.objectId);
		}
		for (const MarkerScreenEntry& entry : generalEntries)
		{
			result.candidates.generalCandidateIds.push_back(entry.objectId);
		}

		result.screenIndex.viewportKey = query.viewportKey;
		result.screenIndex.cellSizePixels = std::max(preference.densityRadiusPixels, 1.0f);
		result.screenIndex.entries = std::move(reservedEntries);
		result.screenIndex.entries.insert(result.screenIndex.entries.end(), generalEntries.begin(), generalEntries.end());
		std::sort(result.screenIndex.entries.begin(), result.screenIndex.entries.end(), [&](const MarkerScreenEntry& left, const MarkerScreenEntry& right) {
			const auto leftCell = CellFor(left.screenPosition, result.screenIndex.cellSizePixels);
			const auto rightCell = CellFor(right.screenPosition, result.screenIndex.cellSizePixels);
			if (leftCell != rightCell)
			{
				return leftCell < rightCell;
			}
			return EntryPriorityLess(left, right);
		});
		for (std::size_t index = 0u; index < result.screenIndex.entries.size();)
		{
			const auto [cellX, cellY] = CellFor(result.screenIndex.entries[index].screenPosition, result.screenIndex.cellSizePixels);
			std::size_t end = index + 1u;
			while (end < result.screenIndex.entries.size() && CellFor(result.screenIndex.entries[end].screenPosition, result.screenIndex.cellSizePixels) == std::pair{cellX, cellY})
			{
				++end;
			}
			result.screenIndex.cells.push_back({cellX, cellY, index, end - index});
			index = end;
		}

		const Clock::time_point densityBegin = Clock::now();
		std::vector<std::size_t> parents(result.screenIndex.entries.size());
		std::iota(parents.begin(), parents.end(), 0u);
		auto findRoot = [&parents](std::size_t index) {
			while (parents[index] != index)
			{
				parents[index] = parents[parents[index]];
				index = parents[index];
			}
			return index;
		};
		auto join = [&parents, &findRoot](std::size_t left, std::size_t right) {
			const std::size_t leftRoot = findRoot(left);
			const std::size_t rightRoot = findRoot(right);
			if (leftRoot != rightRoot)
			{
				parents[std::max(leftRoot, rightRoot)] = std::min(leftRoot, rightRoot);
			}
		};
		if (result.candidates.effectiveLargeSceneMode)
		{
			const float radiusSquared = preference.densityRadiusPixels * preference.densityRadiusPixels;
			for (std::size_t index = 0u; index < result.screenIndex.entries.size(); ++index)
			{
				const MarkerScreenEntry& entry = result.screenIndex.entries[index];
				if (!entry.densityCandidate)
				{
					continue;
				}
				const auto [cellX, cellY] = CellFor(entry.screenPosition, result.screenIndex.cellSizePixels);
				for (std::int32_t y = cellY - 1; y <= cellY + 1; ++y)
				{
					for (std::int32_t x = cellX - 1; x <= cellX + 1; ++x)
					{
						const auto cell = FindCell(result.screenIndex, x, y);
						if (cell == result.screenIndex.cells.end() || cell->x != x || cell->y != y)
						{
							continue;
						}
						for (std::size_t other = cell->firstEntry; other < cell->firstEntry + cell->entryCount; ++other)
						{
							if (other >= index || !result.screenIndex.entries[other].densityCandidate)
							{
								continue;
							}
							const glm::vec2 delta = entry.screenPosition - result.screenIndex.entries[other].screenPosition;
							if (glm::dot(delta, delta) <= radiusSquared)
							{
								join(index, other);
							}
						}
					}
				}
			}
		}

		std::unordered_map<std::size_t, std::vector<std::size_t>> groups;
		groups.reserve(result.screenIndex.entries.size());
		for (std::size_t index = 0u; index < result.screenIndex.entries.size(); ++index)
		{
			groups[findRoot(index)].push_back(index);
		}
		std::vector<MarkerPresentationItem> presentation;
		presentation.reserve(result.screenIndex.entries.size());
		std::vector<std::size_t> groupRoots;
		groupRoots.reserve(groups.size());
		for (const auto& [root, members] : groups)
		{
			(void)members;
			groupRoots.push_back(root);
		}
		std::sort(groupRoots.begin(), groupRoots.end());
		for (const std::size_t root : groupRoots)
		{
			std::vector<std::size_t>& members = groups[root];
			std::sort(members.begin(), members.end(), [&](std::size_t left, std::size_t right) {
				return EntryPriorityLess(result.screenIndex.entries[left], result.screenIndex.entries[right]);
			});
			const bool consolidate = result.candidates.effectiveLargeSceneMode
				&& members.size() >= preference.densityThreshold
				&& std::all_of(members.begin(), members.end(), [&](std::size_t index) { return result.screenIndex.entries[index].clusterCandidate; });
			if (consolidate)
			{
				const MarkerScreenEntry& representative = result.screenIndex.entries[members.front()];
				for (const std::size_t member : members)
				{
					// Every hidden member resolves to the one representative that is
					// actually emitted. This keeps hit testing and presentation identical.
					result.screenIndex.entries[member].hitTestRepresentative = representative.objectId;
				}
				MarkerPresentationItem item;
				item.objectId = representative.objectId;
				item.clusterId = static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(representative.objectId));
				item.clusterMemberCount = static_cast<std::uint32_t>(members.size());
				item.primitiveTier = MarkerPrimitiveTier::Cluster;
				item.hitTestRepresentative = representative.objectId;
				item.screenPosition = representative.screenPosition;
				item.depth = representative.depth;
				item.category = representative.category;
				item.displayNameId = representative.displayNameId;
				item.tooltipId = representative.tooltipId;
				for (const std::size_t member : members)
				{
					item.expandableMemberIds.push_back(result.screenIndex.entries[member].objectId);
				}
				std::sort(item.expandableMemberIds.begin(), item.expandableMemberIds.end(), UuidLess);
				presentation.push_back(std::move(item));
				continue;
			}

			for (const std::size_t member : members)
			{
				const MarkerScreenEntry& entry = result.screenIndex.entries[member];
				MarkerPresentationItem item;
				item.objectId = entry.objectId;
				item.hitTestRepresentative = entry.objectId;
				item.screenPosition = entry.screenPosition;
				item.depth = entry.depth;
				item.category = entry.category;
				item.displayNameId = entry.displayNameId;
				item.tooltipId = entry.tooltipId;
				item.reserved = entry.reserved;
				const bool denseNeighborhood = result.candidates.effectiveLargeSceneMode
					&& members.size() >= preference.densityThreshold
					&& entry.densityCandidate;
				item.primitiveTier = denseNeighborhood ? DenseTier(preference) : MarkerPrimitiveTier::FullIcon;
				presentation.push_back(std::move(item));
			}
		}
		std::sort(presentation.begin(), presentation.end(), [](const MarkerPresentationItem& left, const MarkerPresentationItem& right) {
			if (left.reserved != right.reserved)
			{
				return left.reserved;
			}
			if (left.depth != right.depth)
			{
				return left.depth < right.depth;
			}
			return UuidLess(left.objectId, right.objectId);
		});
		for (std::size_t index = 0u; index < presentation.size(); ++index)
		{
			presentation[index].deterministicSelectionOrder = static_cast<std::uint32_t>(index);
		}
		result.stageTelemetry.push_back(MakeStage("SceneView.Density", densityBegin, result.screenIndex.entries.size()));

		const Clock::time_point emissionBegin = Clock::now();
		++batchGeneration;
		const std::size_t batchSize = std::max<std::size_t>(1u, preference.markerPresentationBatchSize);
		for (std::size_t cursor = 0u; cursor < presentation.size(); cursor += batchSize)
		{
			MarkerPresentationBatch batch;
			batch.batchGeneration = batchGeneration;
			batch.cursor = cursor;
			const std::size_t end = std::min(presentation.size(), cursor + batchSize);
			batch.items.insert(batch.items.end(), presentation.begin() + static_cast<std::ptrdiff_t>(cursor), presentation.begin() + static_cast<std::ptrdiff_t>(end));
			result.presentationBatches.push_back(std::move(batch));
		}
		result.stageTelemetry.push_back(MakeStage("SceneView.Emission", emissionBegin, presentation.size()));

		cachedFrame = result;
		cachedFrame->stageTelemetry.clear();
		cachedStaticGeneration = staticPublicationGeneration;
		cachedPoseGeneration = posePublicationGeneration;
		return result;
	}

	MarkerHitTestResult MarkerPipeline::HitTest(const MarkerScreenIndex& index, const glm::vec2& pointer, float radiusPixels) const
	{
		const Clock::time_point begin = Clock::now();
		MarkerHitTestResult result;
		if (radiusPixels <= 0.0f || index.cellSizePixels <= 0.0f || !std::isfinite(pointer.x) || !std::isfinite(pointer.y))
		{
			result.telemetry = MakeStage("SceneView.HitTest", begin, 0u);
			return result;
		}

		std::vector<const MarkerScreenEntry*> candidates;
		const auto [cellX, cellY] = CellFor(pointer, index.cellSizePixels);
		const std::int32_t range = static_cast<std::int32_t>(std::ceil(radiusPixels / index.cellSizePixels));
		const float radiusSquared = radiusPixels * radiusPixels;
		for (std::int32_t y = cellY - range; y <= cellY + range; ++y)
		{
			for (std::int32_t x = cellX - range; x <= cellX + range; ++x)
			{
				const auto cell = FindCell(index, x, y);
				if (cell == index.cells.end() || cell->x != x || cell->y != y)
				{
					continue;
				}
				for (std::size_t entryIndex = cell->firstEntry; entryIndex < cell->firstEntry + cell->entryCount; ++entryIndex)
				{
					const MarkerScreenEntry& entry = index.entries[entryIndex];
					const glm::vec2 delta = entry.screenPosition - pointer;
					if (glm::dot(delta, delta) <= radiusSquared)
					{
						candidates.push_back(&entry);
					}
				}
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const MarkerScreenEntry* left, const MarkerScreenEntry* right) {
			return EntryPriorityLess(*left, *right);
		});
		if (!candidates.empty())
		{
			result.objectId = candidates.front()->hitTestRepresentative;
		}
		result.telemetry = MakeStage("SceneView.HitTest", begin, candidates.size());
		return result;
	}

	const MarkerStaticRecord* MarkerPipeline::FindStaticRecord(const TerraEngine::UUID& objectId) const
	{
		const auto found = staticRecords.find(objectId);
		return found != staticRecords.end() ? &found->second : nullptr;
	}

	const MarkerPoseRecord* MarkerPipeline::FindPoseRecord(const TerraEngine::UUID& objectId) const
	{
		const auto found = poseRecords.find(objectId);
		return found != poseRecords.end() ? &found->second : nullptr;
	}

	std::string_view MarkerPipeline::ResolveString(MarkerStringId id) const
	{
		return id < strings.size() ? std::string_view(strings[id]) : std::string_view();
	}
}
