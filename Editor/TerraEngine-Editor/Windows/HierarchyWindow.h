#pragma once
#include "../Framework/EditorWindow.h"
#include <string>
#include <entt/entity/entity.hpp>

class MainEditorSubsystem;
class Object;

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
