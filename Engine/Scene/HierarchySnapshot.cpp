// HierarchySnapshot.cpp - validated flat hierarchy snapshots.

#include "HierarchySnapshot.h"

#include "Core/Object/ObjectManager.h"

#include <algorithm>
#include <sstream>
#include <tuple>
#include <unordered_map>

namespace TerraEngine::Scene {
	namespace {
		std::uint64_t BuildParentLinkGeneration(const TerraEngine::UUID& objectId, const TerraEngine::UUID& parentId)
		{
			// FNV-1a is a deterministic dependency tag, not a security hash. The
			// compact value avoids UUID comparisons in high-frequency hot paths.
			std::uint64_t generation = 1469598103934665603ull;
			const auto append = [&generation](const TerraEngine::UUID& id) {
				for (std::uint8_t byte : id)
				{
					generation ^= byte;
					generation *= 1099511628211ull;
				}
			};
			append(objectId);
			append(parentId);
			return generation == 0u ? 1u : generation;
		}

		void AddDepthRange(HierarchySnapshot& snapshot, std::uint32_t depth, std::size_t index)
		{
			if (!snapshot.depthRanges.empty() && snapshot.depthRanges.back().depth == depth)
			{
				snapshot.depthRanges.back().end = index + 1;
				return;
			}
			snapshot.depthRanges.push_back({ depth, index, index + 1 });
		}
	}

	std::string ToString(HierarchySnapshotDiagnosticSeverity severity)
	{
		switch (severity)
		{
		case HierarchySnapshotDiagnosticSeverity::Error:
			return "error";
		case HierarchySnapshotDiagnosticSeverity::Warning:
			return "warning";
		case HierarchySnapshotDiagnosticSeverity::Info:
		default:
			return "info";
		}
	}

	std::string FormatHierarchySnapshot(const HierarchySnapshot& snapshot)
	{
		std::ostringstream stream;
		stream << "Hierarchy snapshot: " << (snapshot.valid ? "valid" : "invalid")
			<< " revision=" << snapshot.revision
			<< " publishedRevision=" << snapshot.publishedRevision
			<< " objects=" << snapshot.orderedObjects.size()
			<< " depthRanges=" << snapshot.depthRanges.size()
			<< " reused=" << (snapshot.reused ? "true" : "false")
			<< " workerReadable=" << (snapshot.workerReadable ? "true" : "false")
			<< " label=" << snapshot.publicationLabel;
		if (!snapshot.diagnostics.empty())
		{
			stream << "\nDiagnostics:";
			for (const HierarchySnapshotDiagnostic& diagnostic : snapshot.diagnostics)
			{
				stream << "\n- " << ToString(diagnostic.severity)
					<< " object=" << TerraEngine::ToHexString(diagnostic.objectId)
					<< ": " << diagnostic.message
					<< " Action: " << diagnostic.recommendedAction;
			}
		}
		return stream.str();
	}

	bool IsHierarchySnapshotWorkerReadable(const HierarchySnapshot& snapshot, std::uint64_t expectedRevision)
	{
		// Worker readers need a valid immutable publication. The optional
		// revision check lets a prepare phase reject stale snapshots before it
		// reads compact object references that may no longer point at live data.
		return snapshot.valid
			&& snapshot.workerReadable
			&& (expectedRevision == 0 || snapshot.publishedRevision == expectedRevision);
	}

	HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects)
	{
		HierarchySnapshot snapshot;
		snapshot.revision = objects.GetHierarchyRevision();
		snapshot.publishedRevision = snapshot.revision;
		snapshot.publicationLabel = "HierarchySnapshot.rev" + std::to_string(snapshot.publishedRevision);

		SceneGraph graph = BuildSceneGraphFromObjectManager(objects, false);
		std::vector<SceneObjectId> order = BuildTraversalOrder(graph);
		std::unordered_map<SceneObjectId, std::size_t> indexByObject;
		for (const SceneObjectId& objectId : order)
		{
			const auto found = graph.objects.find(objectId);
			if (found == graph.objects.end())
			{
				continue;
			}
			const SceneGraphObject& graphObject = found->second;
			HierarchySnapshotEntry entry;
			entry.object = objects.makeObjectHandle(graphObject.entity);
			entry.objectId = objectId;
			entry.parentId = graphObject.parentId;
			entry.parentLinkGeneration = BuildParentLinkGeneration(entry.objectId, entry.parentId);
			entry.worldPose = graphObject.worldPose;
			entry.parentRelativePose = graphObject.parentRelativePose;
			entry.depth = 0;
			if (entry.parentId != TerraEngine::EMPTY_UUID)
			{
				auto parentIndex = indexByObject.find(entry.parentId);
				if (parentIndex == indexByObject.end())
				{
					snapshot.valid = false;
					snapshot.diagnostics.push_back({
						HierarchySnapshotDiagnosticSeverity::Error,
						objectId,
						"Parent was not available before child in hierarchy snapshot.",
						"Rebuild scene hierarchy links before using the snapshot."
					});
				}
				else
				{
					entry.parentIndex = static_cast<std::int64_t>(parentIndex->second);
					entry.depth = snapshot.orderedObjects[parentIndex->second].depth + 1;
				}
			}
			indexByObject[objectId] = snapshot.orderedObjects.size();
			AddDepthRange(snapshot, entry.depth, snapshot.orderedObjects.size());
			snapshot.orderedObjects.push_back(entry);
		}
		for (const SceneDiagnostic& diagnostic : graph.diagnostics)
		{
			if (diagnostic.severity == SceneDiagnosticSeverity::Error || diagnostic.severity == SceneDiagnosticSeverity::Fatal)
			{
				snapshot.valid = false;
				snapshot.diagnostics.push_back({
					HierarchySnapshotDiagnosticSeverity::Error,
					diagnostic.objectId,
					diagnostic.message,
					diagnostic.recommendedAction
				});
			}
		}
		snapshot.workerReadable = snapshot.valid;
		snapshot.formatted = FormatHierarchySnapshot(snapshot);
		return snapshot;
	}

	HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects, const HierarchySnapshot* previousSnapshot)
	{
		if (previousSnapshot != nullptr && previousSnapshot->valid && previousSnapshot->revision == objects.GetHierarchyRevision())
		{
			HierarchySnapshot reused = *previousSnapshot;
			reused.reused = true;
			reused.formatted = FormatHierarchySnapshot(reused);
			return reused;
		}

		// Rebuilding is intentionally centralized here so callers do not need to
		// duplicate revision checks before preparing transform or spatial work.
		HierarchySnapshot rebuilt = BuildHierarchySnapshot(objects);
		rebuilt.reused = false;
		rebuilt.formatted = FormatHierarchySnapshot(rebuilt);
		return rebuilt;
	}

	void ApplyHierarchyResidencyClosure(HierarchySnapshot& snapshot, const WorldRegionDirectory& directory, const PublishedWorldRegionResidency& residency)
	{
		for (HierarchySnapshotEntry& entry : snapshot.orderedObjects)
		{
			entry.homeRegion = directory.FindHomeRegion(entry.objectId);
			entry.parentProxyRequired = false;
			if (entry.parentId == TerraEngine::EMPTY_UUID) continue;
			const std::optional<WorldRegionKey> parentRegion = directory.FindHomeRegion(entry.parentId);
			if (!parentRegion) continue;
			// The child record can remain available while its parent region is not.
			// Marking this explicit closure lets callers render/query a stable proxy
			// instead of silently dropping the parent relationship during unload.
			entry.parentProxyRequired = !residency.residentRegions.contains(*parentRegion);
		}
		snapshot.formatted = FormatHierarchySnapshot(snapshot);
	}
}
