// RuntimeLayout.h - compact worker-readable scene layout contracts.
//
// These records are intentionally smaller than Object handles and authored
// metadata. Runtime systems can copy or share them for one frame without
// pulling editor/serialization state into hot loops.

#pragma once

#include "Core/Export.h"
#include "Components/Transform.h"
#include "Utilities/UUID.h"

#include <entt/entt.hpp>
#include <glm/mat4x4.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class ObjectManager;

namespace TerraEngine::Scene {

	struct HierarchySnapshot;
	struct SceneGraph;
	struct SceneTransformPropagationResult;

	enum class RuntimeLayoutRecordKind {
		Camera,
		Renderable,
		Light
	};

	struct RuntimeActiveMembership {
		entt::entity entity = entt::null;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		bool objectActive = false;
		std::uint64_t componentActiveMask = 0;
		std::uint64_t membershipRevision = 0;
		bool authoredStateVisible = true;
	};

	struct FlatHierarchyEntry {
		entt::entity entity = entt::null;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::optional<std::size_t> parentIndex;
		std::size_t firstChildIndex = 0;
		std::size_t childCount = 0;
		std::uint32_t depth = 0;
		bool runtimeActive = true;
		std::uint64_t transformRevision = 0;
		std::uint64_t spatialRevision = 0;
	};

	struct FlatHierarchyLayout {
		std::uint64_t layoutRevision = 0;
		std::uint64_t sourceHierarchyRevision = 0;
		std::uint64_t sourceMembershipRevision = 0;
		std::vector<FlatHierarchyEntry> entries;
		std::size_t activeObjectCount = 0;
		std::size_t totalObjectCount = 0;
		std::string rebuildReason;
		bool valid = true;
		bool reused = false;
	};

	struct TransformRuntimeRecord {
		entt::entity entity = entt::null;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::size_t layoutIndex = 0;
		std::optional<std::size_t> parentIndex;
		glm::vec3 localPosition{0.0f, 0.0f, 0.0f};
		glm::quat localRotation{1.0f, 0.0f, 0.0f, 0.0f};
		glm::vec3 localScale{1.0f, 1.0f, 1.0f};
		Transform worldTransform{};
		glm::mat4 modelMatrix{1.0f};
		std::uint64_t sourceRevision = 0;
		bool processed = false;
	};

	struct RenderRuntimeRecord {
		RuntimeLayoutRecordKind recordKind = RuntimeLayoutRecordKind::Renderable;
		entt::entity entity = entt::null;
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::optional<std::size_t> transformIndex;
		std::vector<TerraEngine::UUID> resourceIds;
		std::uint32_t renderFlags = 0;
		std::uint64_t sourceRevision = 0;
		std::string rejectionReason;
	};

	struct LayoutDiagnosticsSummary {
		std::uint64_t frameId = 0;
		bool layoutReused = false;
		std::size_t layoutRebuildCount = 0;
		std::vector<std::string> rebuildReasons;
		std::size_t activeObjectCount = 0;
		std::size_t totalObjectCount = 0;
		std::size_t transformRecordCount = 0;
		std::size_t renderRecordCount = 0;
		std::size_t fallbackCount = 0;
		std::size_t validationMismatchCount = 0;
		bool smallSceneGuardTriggered = false;
	};

	struct RuntimeLayoutMismatch {
		TerraEngine::UUID objectId = TerraEngine::EMPTY_UUID;
		std::string category;
		std::string optimizedValue;
		std::string compatibilityValue;
		std::string fallbackRecommendation;
	};

	struct CompatibilitySnapshot {
		std::vector<TerraEngine::UUID> activeObjectIds;
		std::vector<TerraEngine::UUID> hierarchyOrder;
		std::vector<TerraEngine::UUID> transformOutputs;
		std::vector<TerraEngine::UUID> renderOutputs;
		LayoutDiagnosticsSummary diagnostics;
	};

	struct RuntimeLayoutValidationResult {
		bool valid = true;
		std::vector<RuntimeLayoutMismatch> mismatches;
		LayoutDiagnosticsSummary diagnostics;
	};

	TERRAENGINE_API const char* ToString(RuntimeLayoutRecordKind kind);
	TERRAENGINE_API std::string FormatLayoutDiagnosticsSummary(const LayoutDiagnosticsSummary& summary);
	TERRAENGINE_API RuntimeLayoutValidationResult ValidateRuntimeLayoutCompatibility(const CompatibilitySnapshot& optimized, const CompatibilitySnapshot& compatibility);
	TERRAENGINE_API CompatibilitySnapshot BuildCompatibilitySnapshot(ObjectManager& objects);
	TERRAENGINE_API FlatHierarchyLayout BuildFlatHierarchyLayout(ObjectManager& objects, const HierarchySnapshot& snapshot, const FlatHierarchyLayout* previousLayout = nullptr);
	TERRAENGINE_API std::vector<TransformRuntimeRecord> BuildTransformRuntimeRecords(const FlatHierarchyLayout& layout, const SceneTransformPropagationResult& propagation);
	TERRAENGINE_API LayoutDiagnosticsSummary BuildLayoutDiagnosticsSummary(const FlatHierarchyLayout& layout, std::size_t transformRecordCount, std::size_t renderRecordCount, std::uint64_t frameId = 0);

}
