// HierarchyWindow.cpp — Scene Hierarchy Window Implementation
//
// Implements the scene hierarchy interface using ImGui tree nodes to visualize the entity-component structure.
// It handles user interactions for entity selection, drag-and-drop parenting, and context menu actions.

#include "HierarchyWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include <imgui.h>
#include <algorithm>

HierarchyWindow::HierarchyWindow(MainEditorSubsystem* owner)
	: EditorWindow("Scene Hierarchy"), m_Owner(owner) {
}

// HierarchyWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void HierarchyWindow::OnGui() {
	auto& objManager = ObjectManager::instance();
	
	// Header: Add Button & Search
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("+")) {
		ImGui::OpenPopup("CreateEntityPopup");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopup("CreateEntityPopup")) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::MenuItem("Create Empty Object")) {
			Object obj = objManager.createObject("Object");
			UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(obj.EntityId()));
		}
		ImGui::EndPopup();
	}
	
	ImGui::SameLine();
	char hierarchySearchBuf[128];
	strncpy(hierarchySearchBuf, m_SearchFilter.c_str(), sizeof(hierarchySearchBuf));
	
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::InputTextWithHint("##HierarchySearch", "Search...", hierarchySearchBuf, IM_ARRAYSIZE(hierarchySearchBuf))) {
		m_SearchFilter = hierarchySearchBuf;
	}

	ImGui::Separator();

	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	std::string sceneName = activeScene ? activeScene->name : "No Active Scene";
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (SceneManager::Instance().IsActiveSceneDirty()) {
		sceneName += "*";
	}

	ImGuiTreeNodeFlags sceneNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
	bool sceneOpened = ImGui::TreeNodeEx(sceneName.c_str(), sceneNodeFlags);

	// Accept Drop on Root (Scene Node) -> Detach Parent
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
			IM_ASSERT(payload->DataSize == sizeof(entt::entity));
			entt::entity payload_n = *(const entt::entity*)payload->Data;
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto childObj = objManager.getObject(payload_n)) {
				// Detach from parent (make root)
				objManager.setParent(*childObj, std::nullopt);
			}
		}
		ImGui::EndDragDropTarget();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (sceneOpened) {
		auto roots = objManager.getRootObjects();
		
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (auto& obj : roots) {
			DrawEntityNode(obj);
		}
		ImGui::TreePop(); // Pop for scene name node
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_OpenEntityMenu) {
		ImGui::OpenPopup("EntityContextMenu");
		m_OpenEntityMenu = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopup("EntityContextMenu")) {
		if (m_ContextEntityId != entt::null && objManager.getObject(m_ContextEntityId)) {
			if (ImGui::MenuItem("Delete")) {
				UndoRedoSystem::Instance().PushCommand(std::make_unique<DeleteObjectCommand>(m_ContextEntityId));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto obj = objManager.getObject(m_ContextEntityId)) {
					objManager.removeObject(*obj);
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto selected = m_Owner->GetSelectedEntity(); selected && *selected == m_ContextEntityId) {
					m_Owner->SetSelectedEntity(std::nullopt);
				}
				m_ContextEntityId = entt::null;
			}
		}
		ImGui::EndPopup();
	}

	// Handle Delete Key
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsKeyReleased(ImGuiKey_Delete)) {
		auto selected = m_Owner->GetSelectedEntity();
		if (selected && objManager.getObject(*selected)) {
			UndoRedoSystem::Instance().PushCommand(std::make_unique<DeleteObjectCommand>(*selected));
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (auto obj = objManager.getObject(*selected)) {
				objManager.removeObject(*obj);
			}
			m_Owner->SetSelectedEntity(std::nullopt);
		}
	}
}

// HierarchyWindow::DrawEntityNode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void HierarchyWindow::DrawEntityNode(Object& obj) {
	auto& objManager = ObjectManager::instance();
	std::string name = objManager.getObjectName(obj);
	
	bool matchesFilter = m_SearchFilter.empty();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!matchesFilter) {
		std::string lowerName = name;
		std::string lowerFilter = m_SearchFilter;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
		std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lowerName.find(lowerFilter) != std::string::npos) {
			matchesFilter = true;
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!matchesFilter) {
		// Simple skip for now
		// return; // Uncomment to strict filter
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
	
	auto selected = m_Owner->GetSelectedEntity();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selected && *selected == obj.EntityId()) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}
	
	auto children = objManager.getChildren(obj);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_SearchFilter.empty()) {
		flags |= ImGuiTreeNodeFlags_DefaultOpen;
	}
	
	bool opened = ImGui::TreeNodeEx((void*)(uint64_t)obj.EntityId(), flags, "%s", name.c_str());
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsItemClicked()) {
		m_Owner->SetSelectedEntity(obj.EntityId());
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
		m_ContextEntityId = obj.EntityId();
		m_OpenEntityMenu = true;
	}

	// Drag Source
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropSource()) {
		entt::entity entityId = obj.EntityId();
		ImGui::SetDragDropPayload("ENTITY_CELL", &entityId, sizeof(entt::entity));
		ImGui::Text("%s", name.c_str());
		ImGui::EndDragDropSource();
	}

	// Drop Target (Parenting)
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
			IM_ASSERT(payload->DataSize == sizeof(entt::entity));
			entt::entity payload_n = *(const entt::entity*)payload->Data;
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (payload_n != obj.EntityId()) {
				if (auto childObj = objManager.getObject(payload_n)) {
					objManager.setParent(*childObj, obj);
				}
			}
		}
		ImGui::EndDragDropTarget();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (opened) {
		for (auto& child : children) {
			DrawEntityNode(child);
		}
		ImGui::TreePop();
	}
}
