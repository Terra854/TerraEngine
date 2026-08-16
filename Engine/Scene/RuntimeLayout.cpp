// RuntimeLayout.cpp - deterministic helpers for compact runtime layout records.

#include "RuntimeLayout.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/SceneGraphRuntime.h"
#include "Scene/SceneChangePacket.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Scene {
	namespace {
		std::uint64_t CombineRuntimeGeneration(std::uint64_t seed, std::uint64_t value)
		{
			// This deterministic hash-combine is cheap enough for every transform
			// record. Zero remains reserved for unavailable dependency state.
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
			return seed == 0u ? 1u : seed;
		}
	}

	const char* ToString(RuntimeLayoutRecordKind kind)
	{
		switch (kind)
		{
		case RuntimeLayoutRecordKind::Camera:
			return "camera";
		case RuntimeLayoutRecordKind::Light:
			return "light";
		case RuntimeLayoutRecordKind::Renderable:
		default:
			return "renderable";
		}
	}

	std::string FormatLayoutDiagnosticsSummary(const LayoutDiagnosticsSummary& summary)
	{
		std::ostringstream stream;
		stream << "Runtime layout diagnostics:"
			<< " frame=" << summary.frameId
			<< " reused=" << (summary.layoutReused ? "true" : "false")
			<< " rebuilds=" << summary.layoutRebuildCount
			<< " active=" << summary.activeObjectCount
			<< " total=" << summary.totalObjectCount
			<< " transforms=" << summary.transformRecordCount
			<< " renderRecords=" << summary.renderRecordCount
			<< " fallbacks=" << summary.fallbackCount
			<< " metadataScans=" << summary.metadataScanFallbackCount
			<< " facadeFallbacks=" << summary.fullObjectFacadeFallbackCount
			<< " compactFallbacks=" << summary.objectHandleFallbackCount
			<< " staleRefs=" << summary.staleObjectHandleCount
			<< " mismatches=" << summary.validationMismatchCount
			<< " smallSceneGuard=" << (summary.smallSceneGuardTriggered ? "true" : "false");
		for (const std::string& reason : summary.rebuildReasons)
		{
			stream << " [" << reason << "]";
		}
		return stream.str();
	}

	void AddRuntimeLayoutFallback(LayoutDiagnosticsSummary& summary, std::string reason, std::size_t affectedCount)
	{
		// Fallback counters intentionally live beside the formatted reason list
		// so tests, automation, and the editor window can all distinguish a
		// compatibility path from an optimized active-membership reuse path.
		summary.fallbackCount += affectedCount;
		summary.rebuildReasons.push_back(std::move(reason));
	}

	RuntimeLayoutValidationResult ValidateRuntimeLayoutCompatibility(const CompatibilitySnapshot& optimized, const CompatibilitySnapshot& compatibility)
	{
		RuntimeLayoutValidationResult result;
		auto compareList = [&result](const char* category, const std::vector<TerraEngine::UUID>& left, const std::vector<TerraEngine::UUID>& right) {
			if (left == right)
			{
				return;
			}
			RuntimeLayoutMismatch mismatch;
			mismatch.category = category;
			mismatch.optimizedValue = std::to_string(left.size());
			mismatch.compatibilityValue = std::to_string(right.size());
			mismatch.fallbackRecommendation = "Use compatibility path until the optimized runtime layout matches authored output.";
			result.mismatches.push_back(std::move(mismatch));
		};

		compareList("active-membership", optimized.activeObjectIds, compatibility.activeObjectIds);
		compareList("hierarchy-order", optimized.hierarchyOrder, compatibility.hierarchyOrder);
		compareList("transform-output", optimized.transformOutputs, compatibility.transformOutputs);
		compareList("render-output", optimized.renderOutputs, compatibility.renderOutputs);
		result.valid = result.mismatches.empty();
		result.diagnostics = optimized.diagnostics;
		result.diagnostics.validationMismatchCount = result.mismatches.size();
		if (!result.valid)
		{
			AddRuntimeLayoutFallback(result.diagnostics, "compatibility-snapshot-mismatch");
		}
		return result;
	}

	CompatibilitySnapshot BuildCompatibilitySnapshot(ObjectManager& objects)
	{
		CompatibilitySnapshot snapshot;
		for (const RuntimeActiveMembership& membership : objects.GetRuntimeActiveMembershipSnapshot())
		{
			if (membership.objectActive)
			{
				snapshot.activeObjectIds.push_back(membership.objectId);
			}
		}
		SceneGraph graph = BuildSceneGraphFromObjectManager(objects, true);
		snapshot.hierarchyOrder = BuildTraversalOrder(graph);
		for (const SceneObjectId& objectId : snapshot.hierarchyOrder)
		{
			const auto object = graph.objects.find(objectId);
			if (object != graph.objects.end() && object->second.active)
			{
				snapshot.transformOutputs.push_back(objectId);
			}
		}
		std::sort(snapshot.activeObjectIds.begin(), snapshot.activeObjectIds.end(), [](const TerraEngine::UUID& left, const TerraEngine::UUID& right) {
			return TerraEngine::ToHexString(left) < TerraEngine::ToHexString(right);
		});
		snapshot.diagnostics.activeObjectCount = snapshot.activeObjectIds.size();
		snapshot.diagnostics.totalObjectCount = graph.objects.size();
		snapshot.diagnostics.transformRecordCount = snapshot.transformOutputs.size();
		if (!graph.objects.empty())
		{
			snapshot.diagnostics.fullObjectFacadeFallbackCount = graph.objects.size();
			AddRuntimeLayoutFallback(snapshot.diagnostics, "compatibility-full-object-facade", graph.objects.size());
		}
		return snapshot;
	}

	FlatHierarchyLayout BuildFlatHierarchyLayout(ObjectManager& objects, const HierarchySnapshot& snapshot, const FlatHierarchyLayout* previousLayout)
	{
		const std::uint64_t membershipRevision = objects.GetRuntimeActiveMembershipRevision();
		if (previousLayout != nullptr
			&& previousLayout->valid
			&& previousLayout->sourceHierarchyRevision == snapshot.revision
			&& previousLayout->sourceMembershipRevision == membershipRevision)
		{
			FlatHierarchyLayout reused = *previousLayout;
			reused.reused = true;
			return reused;
		}

		FlatHierarchyLayout layout;
		layout.layoutRevision = snapshot.publishedRevision;
		layout.sourceHierarchyRevision = snapshot.revision;
		layout.sourceMembershipRevision = membershipRevision;
		layout.valid = snapshot.valid;
		layout.reused = false;
		layout.rebuildReason = snapshot.valid ? "hierarchy-or-membership-revision-changed" : "hierarchy-snapshot-invalid";
		layout.totalObjectCount = snapshot.orderedObjects.size();

		std::unordered_map<TerraEngine::UUID, bool> activeByObjectId;
		std::unordered_map<TerraEngine::UUID, Object> objectHandleByObjectId;
		for (const RuntimeActiveMembership& membership : objects.GetRuntimeActiveMembershipSnapshot())
		{
			activeByObjectId[membership.objectId] = membership.objectActive;
			objectHandleByObjectId[membership.objectId] = membership.objectHandle;
		}

		layout.entries.reserve(snapshot.orderedObjects.size());
		for (std::size_t index = 0; index < snapshot.orderedObjects.size(); ++index)
		{
			const HierarchySnapshotEntry& source = snapshot.orderedObjects[index];
			FlatHierarchyEntry entry;
			entry.entity = static_cast<entt::entity>(source.object.entityValue);
			entry.objectId = source.objectId;
			entry.parentObjectId = source.parentId;
			entry.parentLinkGeneration = source.parentLinkGeneration;
			if (const auto compact = objectHandleByObjectId.find(source.objectId); compact != objectHandleByObjectId.end())
			{
				entry.objectHandle = compact->second;
			}
			else
			{
				entry.objectHandle = objects.makeObjectHandle(entry.entity);
			}
			if (source.parentIndex >= 0)
			{
				entry.parentIndex = static_cast<std::size_t>(source.parentIndex);
				if (*entry.parentIndex >= index)
				{
					layout.valid = false;
					layout.rebuildReason = "parent-index-not-before-child";
				}
			}
			entry.depth = source.depth;
			entry.runtimeActive = activeByObjectId.contains(source.objectId) ? activeByObjectId[source.objectId] : true;
			entry.transformRevision = objects.GetObjectTransformRevision(entry.objectHandle);
			entry.spatialRevision = objects.GetObjectSpatialRevision(entry.objectHandle);
			entry.isStatic = objects.isObjectStatic(entry.objectHandle);
			if (entry.runtimeActive)
			{
				++layout.activeObjectCount;
			}
			layout.entries.push_back(entry);
		}

		return layout;
	}

	std::vector<std::size_t> BuildDirtyTransformLayoutIndices(const FlatHierarchyLayout& layout, const SceneDirtyTransformState& dirtyState)
	{
		std::vector<std::size_t> indices;
		indices.reserve(layout.activeObjectCount);
		if (!layout.valid)
		{
			return indices;
		}

		std::unordered_set<TerraEngine::UUID> dirtyObjects(dirtyState.expandedDirtyObjects.begin(), dirtyState.expandedDirtyObjects.end());
		for (const TerraEngine::UUID& dirtyRoot : dirtyState.dirtyRoots)
		{
			dirtyObjects.insert(dirtyRoot);
		}

		const bool includeAll = dirtyState.fullRefreshRequested;
		std::vector<bool> parentOrSelfDirty(layout.entries.size(), false);
		for (std::size_t index = 0; index < layout.entries.size(); ++index)
		{
			const FlatHierarchyEntry& entry = layout.entries[index];
			const bool parentDirty = entry.parentIndex.has_value()
				&& *entry.parentIndex < parentOrSelfDirty.size()
				&& parentOrSelfDirty[*entry.parentIndex];
			const bool selfDirty = dirtyObjects.find(entry.objectId) != dirtyObjects.end();
			parentOrSelfDirty[index] = includeAll || parentDirty || selfDirty;
			if (entry.runtimeActive && parentOrSelfDirty[index])
			{
				indices.push_back(index);
			}
		}
		return indices;
	}

	std::vector<TransformRuntimeRecord> BuildTransformRuntimeRecords(const FlatHierarchyLayout& layout, const SceneTransformPropagationResult& propagation)
	{
		std::unordered_map<TerraEngine::UUID, SceneWorldTransformRecord> propagatedByObject;
		for (const SceneWorldTransformRecord& record : propagation.records)
		{
			propagatedByObject[record.objectId] = record;
		}

		std::vector<TransformRuntimeRecord> records;
		records.reserve(layout.activeObjectCount);
		for (std::size_t index = 0; index < layout.entries.size(); ++index)
		{
			const FlatHierarchyEntry& entry = layout.entries[index];
			if (!entry.runtimeActive)
			{
				continue;
			}
			TransformRuntimeRecord record;
			record.entity = entry.entity;
			record.objectHandle = entry.objectHandle;
			record.objectId = entry.objectId;
			record.layoutIndex = index;
			record.parentIndex = entry.parentIndex;
			record.sourceRevision = entry.transformRevision;
			record.isStatic = entry.isStatic;
			if (const auto propagated = propagatedByObject.find(entry.objectId); propagated != propagatedByObject.end())
			{
				record.worldPose = propagated->second.worldPose;
				record.processed = true;
			}
			records.push_back(record);
		}
		return records;
	}

	std::vector<TransformRuntimeRecord> BuildDirtyTransformRuntimeRecords(const FlatHierarchyLayout& layout, const SceneTransformPropagationResult& propagation, const SceneDirtyTransformState& dirtyState)
	{
		const std::vector<std::size_t> dirtyIndices = BuildDirtyTransformLayoutIndices(layout, dirtyState);
		std::unordered_set<std::size_t> dirtyIndexSet(dirtyIndices.begin(), dirtyIndices.end());
		std::vector<TransformRuntimeRecord> records = BuildTransformRuntimeRecords(layout, propagation);
		records.erase(std::remove_if(records.begin(), records.end(), [&dirtyIndexSet](const TransformRuntimeRecord& record) {
			return dirtyIndexSet.find(record.layoutIndex) == dirtyIndexSet.end();
		}), records.end());
		return records;
	}

	CompactTransformRuntimeSnapshot BuildCompactTransformRuntimeSnapshot(const FlatHierarchyLayout& layout, const SceneGraph& graph, const CompactTransformRuntimeSnapshot* previousSnapshot)
	{
		CompactTransformRuntimeSnapshot snapshot;
		snapshot.valid = layout.valid;
		snapshot.sourceGraphRevision = graph.revision;
		snapshot.sourceHierarchyRevision = layout.sourceHierarchyRevision;
		snapshot.parentWorldStates.reserve(layout.entries.size());
		snapshot.records.reserve(layout.activeObjectCount);

		std::unordered_map<TerraEngine::UUID, const ParentWorldRuntimeState*> previousByObject;
		if (previousSnapshot != nullptr && previousSnapshot->valid)
		{
			for (const ParentWorldRuntimeState& state : previousSnapshot->parentWorldStates)
			{
				previousByObject[state.objectId] = &state;
			}
		}

		for (std::size_t index = 0; index < layout.entries.size(); ++index)
		{
			const FlatHierarchyEntry& entry = layout.entries[index];
			const auto graphObject = graph.objects.find(entry.objectId);
			if (graphObject == graph.objects.end())
			{
				snapshot.valid = false;
				break;
			}

			const SceneGraphObject& source = graphObject->second;
			ParentWorldRuntimeState parentState;
			parentState.objectId = entry.objectId;
			parentState.parentObjectId = entry.parentObjectId;
			parentState.parentIndex = entry.parentIndex;
			parentState.generation = CombineRuntimeGeneration(1u, entry.parentLinkGeneration);
			if (entry.parentIndex.has_value())
			{
				if (*entry.parentIndex >= index || *entry.parentIndex >= layout.entries.size())
				{
					snapshot.valid = false;
					break;
				}
				const FlatHierarchyEntry& parentEntry = layout.entries[*entry.parentIndex];
				const auto parentObject = graph.objects.find(parentEntry.objectId);
				if (parentObject == graph.objects.end())
				{
					snapshot.valid = false;
					break;
				}
				const std::uint64_t ancestorGeneration = snapshot.parentWorldStates[*entry.parentIndex].generation;
				parentState.generation = CombineRuntimeGeneration(parentState.generation, parentObject->second.transformRevision);
				parentState.generation = CombineRuntimeGeneration(parentState.generation, ancestorGeneration);
				parentState.parentWorldPose = parentObject->second.worldPose;
			}

			if (const auto previous = previousByObject.find(entry.objectId);
				previous != previousByObject.end()
				&& previous->second->parentObjectId == parentState.parentObjectId
				&& previous->second->generation == parentState.generation)
			{
				// Retain the already-normalized parent pose when its exact dependency
				// generation is unchanged.
				parentState = *previous->second;
				parentState.reused = true;
				++snapshot.reusedParentStateCount;
			}
			snapshot.parentWorldStates.push_back(parentState);

			if (!entry.runtimeActive)
			{
				continue;
			}
			CompactTransformRuntimeRecord record;
			record.objectId = entry.objectId;
			record.layoutIndex = index;
			record.parentIndex = entry.parentIndex;
			record.parentRelativePose = source.parentRelativePose;
			record.worldPose = source.worldPose;
			record.transformRevision = source.transformRevision;
			record.parentWorldGeneration = parentState.generation;
			record.isStatic = entry.isStatic;
			snapshot.records.push_back(record);
		}

		if (!snapshot.valid || snapshot.parentWorldStates.size() != layout.entries.size())
		{
			// Reject the complete immutable snapshot if any parent index or graph
			// record is stale; workers must never observe partial indexed state.
			snapshot.valid = false;
			snapshot.records.clear();
			snapshot.parentWorldStates.clear();
			snapshot.reusedParentStateCount = 0;
		}
		return snapshot;
	}

	RuntimeLayoutChangeBatch BuildRuntimeLayoutChangeBatch(const FlatHierarchyLayout& layout, const SceneTransformPropagationResult& propagation, const SceneDirtyTransformState& dirtyState, const SceneChangePacket& changes)
	{
		RuntimeLayoutChangeBatch batch;
		batch.sourceSceneGeneration = changes.SceneGeneration();
		batch.sourceLayoutRevision = layout.layoutRevision;
		const SceneChangePacketValidationResult validation = changes.Validate();
		if (!layout.valid || !propagation.valid || !dirtyState.valid || !validation.valid)
		{
			// A caller may safely discard this batch: no partial runtime record
			// publication is produced from stale generations or failed propagation.
			batch.valid = false;
			return batch;
		}

		batch.transformChanges = BuildDirtyTransformRuntimeRecords(layout, propagation, dirtyState);
		for (const TransformRuntimeRecord& transform : batch.transformChanges)
		{
			if (transform.layoutIndex >= layout.entries.size())
			{
				batch.valid = false;
				batch.transformChanges.clear();
				batch.staticSpatialChanges.clear();
				batch.movableSpatialChanges.clear();
				return batch;
			}
			const FlatHierarchyEntry& entry = layout.entries[transform.layoutIndex];
			RuntimeSpatialRecordChange spatial;
			spatial.objectId = transform.objectId;
			spatial.layoutIndex = transform.layoutIndex;
			spatial.spatialRevision = entry.spatialRevision;
			spatial.isStatic = entry.isStatic;
			(entry.isStatic ? batch.staticSpatialChanges : batch.movableSpatialChanges).push_back(spatial);
		}
		return batch;
	}

	LayoutDiagnosticsSummary BuildLayoutDiagnosticsSummary(const FlatHierarchyLayout& layout, std::size_t transformRecordCount, std::size_t renderRecordCount, std::uint64_t frameId)
	{
		LayoutDiagnosticsSummary summary;
		summary.frameId = frameId;
		summary.layoutReused = layout.reused;
		summary.layoutRebuildCount = layout.reused ? 0u : 1u;
		if (!layout.rebuildReason.empty())
		{
			summary.rebuildReasons.push_back(layout.rebuildReason);
		}
		summary.activeObjectCount = layout.activeObjectCount;
		summary.totalObjectCount = layout.totalObjectCount;
		summary.transformRecordCount = transformRecordCount;
		summary.renderRecordCount = renderRecordCount;
		summary.fallbackCount = layout.valid ? 0u : 1u;
		summary.metadataScanFallbackCount = 0u;
		summary.fullObjectFacadeFallbackCount = 0u;
		summary.objectHandleFallbackCount = 0u;
		summary.staleObjectHandleCount = 0u;
		summary.smallSceneGuardTriggered = layout.totalObjectCount <= 8u;
		if (!layout.valid)
		{
			summary.rebuildReasons.push_back("runtime-layout-invalid");
		}
		return summary;
	}
}
