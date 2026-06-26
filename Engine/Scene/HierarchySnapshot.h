// HierarchySnapshot.h - flattened parent-before-child scene hierarchy snapshots.

#pragma once

#include "Core/Export.h"
#include "Core/Object/CompactObjectReference.h"
#include "SceneGraphRuntime.h"

#include <cstdint>
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
		TerraEngine::CompactObjectReference object;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID;
		std::int64_t parentIndex = -1;
		std::uint32_t depth = 0;
	};

	struct HierarchySnapshot {
		std::uint64_t revision = 0;
		std::vector<HierarchySnapshotEntry> orderedObjects;
		std::vector<HierarchyDepthRange> depthRanges;
		std::vector<HierarchySnapshotDiagnostic> diagnostics;
		bool valid = true;
		bool reused = false;
		std::string formatted;
	};

	TERRAENGINE_API std::string ToString(HierarchySnapshotDiagnosticSeverity severity);
	TERRAENGINE_API std::string FormatHierarchySnapshot(const HierarchySnapshot& snapshot);
	TERRAENGINE_API HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects);
	TERRAENGINE_API HierarchySnapshot BuildHierarchySnapshot(ObjectManager& objects, const HierarchySnapshot* previousSnapshot);

}
