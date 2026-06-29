// RuntimeLayout.cpp - deterministic helpers for compact runtime layout records.

#include "RuntimeLayout.h"

#include "Core/Object/ObjectManager.h"
#include "Scene/HierarchySnapshot.h"
#include "Scene/SceneGraphRuntime.h"

#include <algorithm>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace TerraEngine::Scene {
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

		const bool includeAll = dirtyState.fullRefreshRequested || dirtyObjects.empty();
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
			if (const auto propagated = propagatedByObject.find(entry.objectId); propagated != propagatedByObject.end())
			{
				record.worldTransform = propagated->second.worldTransform;
				record.modelMatrix = propagated->second.modelMatrix;
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
