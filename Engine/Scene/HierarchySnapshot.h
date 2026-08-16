// HierarchySnapshot.h - flattened parent-before-child scene hierarchy snapshots.

#pragma once

#include "Core/Export.h"
#include "Core/Object/Object.h"
#include "SceneGraphRuntime.h"
#include "Scene/WorldRegion.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {

	enum class HierarchySnapshotDiagnosticSeverity {
		Info,
		Warning,
		Error
	};

	struct HierarchySnapshotDiagnostic {
		HierarchySnapshotDiagnosticSeverity severity = HierarchySnapshotDiagnosticSeverity::Info;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string message;
		std::string recommendedAction;
	};

	struct HierarchyDepthRange {
		std::uint32_t depth = 0;
		std::size_t begin = 0;
		std::size_t end = 0;
	};

	struct HierarchySnapshotEntry {
		Object object;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID;
		std::int64_t parentIndex = -1;
		std::uint32_t depth = 0;
		// This tag changes only when this exact parent link changes. Unrelated
		// hierarchy edits can therefore preserve cached parent-world state.
		std::uint64_t parentLinkGeneration = 0;
		// Detached canonical and bounded-local state let worker preparation avoid
		// live Transform reads while preserving exact world addresses.
		WorldPose worldPose{};
		ParentRelativePose parentRelativePose{};
		// Residency remains separate from hierarchy. This metadata preserves an
		// unloaded relation as a deterministic proxy rather than a live object.
		std::optional<WorldRegionKey> homeRegion;
		bool parentProxyRequired = false;
	};

	struct HierarchySnapshot {
		std::uint64_t revision = 0;
		std::uint64_t publishedRevision = 0;
		std::vector<HierarchySnapshotEntry> orderedObjects;
		std::vector<HierarchyDepthRange> depthRanges;
		std::vector<HierarchySnapshotDiagnostic> diagnostics;
		bool valid = true;
		bool reused = false;
		bool workerReadable = false;
		std::string publicationLabel;
		std::string formatted;
	};

	TERRAENGINE_API std::string ToString(HierarchySnapshotDiagnosticSeverity severity);
	TERRAENGINE_API std::string FormatHierarchySnapshot(const HierarchySnapshot& snapshot);
	TERRAENGINE_API bool IsHierarchySnapshotWorkerReadable(const HierarchySnapshot& snapshot, std::uint64_t expectedRevision = 0);
	TERRAENGINE_API HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects);
	TERRAENGINE_API HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects, const HierarchySnapshot* previousSnapshot);
	TERRAENGINE_API void ApplyHierarchyResidencyClosure(HierarchySnapshot& snapshot, const WorldRegionDirectory& directory, const PublishedWorldRegionResidency& residency);

}
