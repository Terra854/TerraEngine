// HierarchyWindow.h — Scene Hierarchy Window Definition
//
// Defines the HierarchyWindow class, which provides a tree view of all entities in the current scene.
// It allows users to select, parent, and manage entities, communicating selection changes to the EditorContext.

#pragma once
#include "../Framework/EditorWindow.h"
#include <string>
#include <entt/entity/entity.hpp>

class MainEditorSubsystem;
class Object;

// class HierarchyWindow groups related state and operations so callers have one documented contract.
class HierarchyWindow : public EditorWindow {
public:
	HierarchyWindow(MainEditorSubsystem* owner);
	
protected:
	virtual void OnGui() override;

private:
	void DrawEntityNode(Object& obj);
	
	MainEditorSubsystem* m_Owner;
	std::string m_SearchFilter;
	entt::entity m_ContextEntityId = entt::null;
	bool m_OpenEntityMenu = false;
};
