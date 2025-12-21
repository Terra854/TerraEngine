#include "HierarchyWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include <imgui.h>
#include <algorithm>

HierarchyWindow::HierarchyWindow(MainEditorSubsystem* owner)
    : EditorWindow("Scene Hierarchy"), m_Owner(owner) {
}

void HierarchyWindow::OnGui() {
    auto& objManager = ObjectManager::instance();
    
    // Header: Add Button & Search
    if (ImGui::Button("+")) {
        ImGui::OpenPopup("CreateEntityPopup");
    }
    if (ImGui::BeginPopup("CreateEntityPopup")) {
        if (ImGui::MenuItem("Create Empty")) {
            objManager.createObject("New Entity");
        }
        ImGui::EndPopup();
    }
    
    ImGui::SameLine();
    char hierarchySearchBuf[128];
    strncpy(hierarchySearchBuf, m_SearchFilter.c_str(), sizeof(hierarchySearchBuf));
    
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##HierarchySearch", "Search...", hierarchySearchBuf, IM_ARRAYSIZE(hierarchySearchBuf))) {
        m_SearchFilter = hierarchySearchBuf;
    }

    ImGui::Separator();

    const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
    std::string sceneName = activeScene ? activeScene->name : "No Active Scene";

    ImGuiTreeNodeFlags sceneNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    bool sceneOpened = ImGui::TreeNodeEx(sceneName.c_str(), sceneNodeFlags);

    // Accept Drop on Root (Scene Node) -> Detach Parent
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
            IM_ASSERT(payload->DataSize == sizeof(entt::entity));
            entt::entity payload_n = *(const entt::entity*)payload->Data;
            
            if (auto childObj = objManager.getObject(payload_n)) {
                // Detach from parent (make root)
                objManager.setParent(*childObj, std::nullopt);
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (sceneOpened) {
        auto roots = objManager.getRootObjects();
        
        for (auto& obj : roots) {
            DrawEntityNode(obj);
        }
        ImGui::TreePop(); // Pop for scene name node
    }
    
    // Right click on background to create entity
    // Note: BeginPopupContextWindow might conflict with EditorWindow::Draw context menu if not handled carefully.
    // EditorWindow uses "WindowContextMenu". 
    // If we want a specific context menu for the content area, we might need to use a specific ID or OpenPopupOnItemClick.
    // BeginPopupContextWindow(nullptr) attaches to the window.
    // Let's use a unique name.
    if (ImGui::BeginPopupContextWindow("HierarchyContext")) {
        if (ImGui::MenuItem("Create Empty Entity")) {
            objManager.createObject("New Entity");
        }
        ImGui::EndPopup();
    }
}

void HierarchyWindow::DrawEntityNode(Object& obj) {
    auto& objManager = ObjectManager::instance();
    std::string name = objManager.getObjectName(obj);
    
    bool matchesFilter = m_SearchFilter.empty();
    if (!matchesFilter) {
        std::string lowerName = name;
        std::string lowerFilter = m_SearchFilter;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
        if (lowerName.find(lowerFilter) != std::string::npos) {
            matchesFilter = true;
        }
    }
    
    if (!matchesFilter) {
        // Simple skip for now
         // return; // Uncomment to strict filter
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    
    auto selected = m_Owner->GetSelectedEntity();
    if (selected && *selected == obj.EntityId()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    auto children = objManager.getChildren(obj);
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    if (!m_SearchFilter.empty()) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    
    bool opened = ImGui::TreeNodeEx((void*)(uint64_t)obj.EntityId(), flags, "%s", name.c_str());
    
    if (ImGui::IsItemClicked()) {
        m_Owner->SetSelectedEntity(obj.EntityId());
    }

    // Drag Source
    if (ImGui::BeginDragDropSource()) {
        entt::entity entityId = obj.EntityId();
        ImGui::SetDragDropPayload("ENTITY_CELL", &entityId, sizeof(entt::entity));
        ImGui::Text("%s", name.c_str());
        ImGui::EndDragDropSource();
    }

    // Drop Target (Parenting)
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
            IM_ASSERT(payload->DataSize == sizeof(entt::entity));
            entt::entity payload_n = *(const entt::entity*)payload->Data;
            
            if (payload_n != obj.EntityId()) {
                if (auto childObj = objManager.getObject(payload_n)) {
                    objManager.setParent(*childObj, obj);
                }
            }
        }
        ImGui::EndDragDropTarget();
    }
    
    if (opened) {
        for (auto& child : children) {
            DrawEntityNode(child);
        }
        ImGui::TreePop();
    }
}
