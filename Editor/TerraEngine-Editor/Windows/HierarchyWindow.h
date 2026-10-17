// HierarchyWindow.h — Scene Hierarchy Window Definition
//
// Defines the HierarchyWindow class, which provides a tree view of all entities in the current scene.
// It allows users to select, parent, and manage entities, communicating selection changes to the EditorContext.

#pragma once
#include "../Framework/EditorWindow.h"
#include <cstdint>
#include <limits>
#include <string>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <entt/entity/entity.hpp>
#include "Engine/Utilities/UUID.h"

class MainEditorSubsystem;


class HierarchyWindow : public EditorWindow {
public:
	HierarchyWindow(MainEditorSubsystem* owner);
	
protected:
	virtual void OnGui() override;

private:
	struct VisibleHierarchyRow;
	void DrawEntityNode(
		entt::entity obj,
		const std::unordered_set<TerraEngine::UUID>* prefabSourceObjectIds = nullptr,
		bool knownLeaf = false,
		std::uint8_t* cachedExpandedFlag = nullptr,
		bool drawChildren = true,
		std::optional<bool> prefabAuthoredOverride = std::nullopt);
	void RefreshRootEntityCache(class ObjectManager& objectManager, bool prefabEditingMode);
	void DrawCachedRootNodes();
	void RebuildVisibleRowCache(class ObjectManager& objectManager, bool prefabEditingMode);
	void DrawVisibleRow(const VisibleHierarchyRow& row);
	void CreatePrefabFromSelection(entt::entity obj);
	bool AcceptPrefabAssetDrop(std::optional<entt::entity> parent);
	
	MainEditorSubsystem* m_Owner;
	std::string m_SearchFilter;
	// The hierarchy revision changes for root/parent relationships, not for
	// ordinary property edits. Caching root entity ids therefore avoids creating
	// a full SceneGraph every ImGui frame while rows still read live names and
	// selection state below.
	std::vector<entt::entity> m_CachedRootEntities;
	struct VisibleHierarchyRow
	{
		entt::entity entity = entt::null;
		std::uint32_t depth = 0u;
		bool knownLeaf = false;
		bool prefabAuthored = false;
	};
	std::vector<VisibleHierarchyRow> m_CachedVisibleRows;
	std::unordered_map<std::string, bool> m_ExpandedByUuid;
	std::string m_CachedSearchFilter;
	std::uint64_t m_ExpansionRevision = 0u;
	std::uint64_t m_CachedExpansionRevision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t m_CachedVisibleHierarchyRevision = std::numeric_limits<std::uint64_t>::max();
	// Scene generation advances across authored/runtime swaps. Keep it separate
	// from hierarchy revision because a new object store can reuse both entity
	// values and the same numeric hierarchy revision after Play Mode exits.
	std::uint64_t m_CachedVisibleSceneGeneration = std::numeric_limits<std::uint64_t>::max();
	bool m_CachedVisiblePrefabEditingMode = false;
	std::uint64_t m_CachedHierarchyRevision = std::numeric_limits<std::uint64_t>::max();
	std::uint64_t m_CachedSceneGeneration = std::numeric_limits<std::uint64_t>::max();
	bool m_CachedPrefabEditingMode = false;
	entt::entity m_ContextEntityId = entt::null;
	bool m_OpenEntityMenu = false;
};
