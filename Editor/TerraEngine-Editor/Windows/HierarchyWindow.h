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
#include <unordered_set>
#include <vector>
#include <entt/entity/entity.hpp>
#include "Engine/Utilities/UUID.h"

class MainEditorSubsystem;

// class HierarchyWindow groups related state and operations so callers have one documented contract.
class HierarchyWindow : public EditorWindow {
public:
	HierarchyWindow(MainEditorSubsystem* owner);
	
protected:
	virtual void OnGui() override;

private:
	void DrawEntityNode(
		entt::entity obj,
		const std::unordered_set<TerraEngine::UUID>* prefabSourceObjectIds = nullptr,
		bool knownLeaf = false);
	void RefreshRootEntityCache(class ObjectManager& objectManager, bool prefabEditingMode);
	void DrawCachedRootNodes();
	void CreatePrefabFromSelection(entt::entity obj);
	bool AcceptPrefabAssetDrop(std::optional<entt::entity> parent);
	
	MainEditorSubsystem* m_Owner;
	std::string m_SearchFilter;
	// The hierarchy revision changes for root/parent relationships, not for
	// ordinary property edits. Caching root entity ids therefore avoids creating
	// a full SceneGraph every ImGui frame while rows still read live names and
	// selection state below.
	std::vector<entt::entity> m_CachedRootEntities;
	// One byte per cached root records whether the hierarchy snapshot proved that
	// row is a leaf. DrawCachedRootNodes uses contiguous leaf runs with ImGui's
	// clipper, so a handful of expandable roots no longer forces every flat root
	// through ImGui each frame.
	std::vector<std::uint8_t> m_CachedRootLeafFlags;
	// ImGui keys tree openness by the stable authored UUID string. Retaining those
	// keys beside the structural cache lets DrawCachedRootNodes clip collapsed
	// prefab roots without resolving every UUID on every editor frame.
	std::vector<std::string> m_CachedRootTreeNodeIds;
	std::uint64_t m_CachedHierarchyRevision = std::numeric_limits<std::uint64_t>::max();
	bool m_CachedPrefabEditingMode = false;
	entt::entity m_ContextEntityId = entt::null;
	bool m_OpenEntityMenu = false;
};
