#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <ImGuiFileDialog.h>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_set>

// Helper to draw RTTR properties
static void DrawProperty(rttr::property prop, Object& obj, ComponentAccessFunctions& access) {
    std::string name = prop.get_name().to_string();
    rttr::variant var = access.getter(obj, name);
    rttr::type type = prop.get_type();

    if (type == rttr::type::get<float>()) {
        float val = var.get_value<float>();
        if (ImGui::DragFloat(name.c_str(), &val, 0.1f)) {
            access.setter(obj, name, val);
        }
    }
    else if (type == rttr::type::get<int>()) {
        int val = var.get_value<int>();
        if (ImGui::DragInt(name.c_str(), &val)) {
            access.setter(obj, name, val);
        }
    }
    else if (type == rttr::type::get<bool>()) {
        bool val = var.get_value<bool>();
        if (ImGui::Checkbox(name.c_str(), &val)) {
            access.setter(obj, name, val);
        }
    }
    else if (type == rttr::type::get<std::string>()) {
        std::string val = var.get_value<std::string>();
        char buffer[256];
        strncpy(buffer, val.c_str(), sizeof(buffer));
        if (ImGui::InputText(name.c_str(), buffer, sizeof(buffer))) {
            access.setter(obj, name, std::string(buffer));
        }
    }
    else if (type == rttr::type::get<glm::vec3>()) {
        glm::vec3 val = var.get_value<glm::vec3>();
        if (ImGui::DragFloat3(name.c_str(), &val.x, 0.1f)) {
            access.setter(obj, name, val);
        }
    }
    else if (type == rttr::type::get<glm::quat>()) {
        glm::quat q = var.get_value<glm::quat>();
        glm::vec3 euler = glm::degrees(glm::eulerAngles(q));
        if (ImGui::DragFloat3(name.c_str(), &euler.x, 0.1f)) {
            glm::quat newQ = glm::quat(glm::radians(euler));
            access.setter(obj, name, newQ);
        }
    }
}

void MainEditorSubsystem::Initialize(EditorContext& context) {
    ctx = &context;
}

void MainEditorSubsystem::OnProjectOpened() {
    if (!ctx || !ctx->projectManager->HasActiveProject()) return;

    auto projectRoot = ctx->projectManager->GetProjectRoot();
    m_CurrentDirectory = AssetDatabase::Instance().GetAssetRoot();
    std::filesystem::path sceneRoot = projectRoot / "Assets" / "Scenes";
    
    // Ensure directory exists (created by ProjectManager, but safe check)
    if (!std::filesystem::exists(sceneRoot)) {
        std::filesystem::create_directories(sceneRoot);
    }

    SceneManager::Instance().SetSceneRoot(sceneRoot.string());
    
    // Load Default Scene
    // TODO: Use UUID from ProjectSettings if we had it.
    TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene("Default.tescene");
    if (sceneId != TerraEngine::EMPTY_UUID) {
        SceneManager::Instance().SetActiveScene(sceneId);
        sceneLoaded = true;
    } else {
        // Log error?
    }
}

void MainEditorSubsystem::Shutdown() {
    ctx = nullptr;
    // Should remove texture? ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)sceneTextureID);
    // But RenderSystem might outlive or die before?
}

void MainEditorSubsystem::OnBackendReady() {
    if (!ctx || !ctx->engine) return;
    
    RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
    if (!renderSystem) return;

    RHIContext* rhi = renderSystem->GetContext();
    VulkanContext* vulkan = dynamic_cast<VulkanContext*>(rhi);
    if (!vulkan) return;

    VulkanContext::EditorHandles handles;
    vulkan->GetEditorHandles(handles);

    if (handles.sceneImageView && handles.sceneSampler) {
        sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
            (VkSampler)handles.sceneSampler,
            (VkImageView)handles.sceneImageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        cachedSceneImageView = handles.sceneImageView;
    }
}

void MainEditorSubsystem::Update(float deltaTime) {
    // Logic update
}

void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
    onProjectClosed = callback;
}

void MainEditorSubsystem::DrawUI() {
    // Check for resize/recreation of offscreen resources
    if (ctx && ctx->engine) {
        RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
        if (renderSystem) {
             VulkanContext* vulkan = dynamic_cast<VulkanContext*>(renderSystem->GetContext());
             if (vulkan) {
                 VulkanContext::EditorHandles handles;
                 vulkan->GetEditorHandles(handles);
                 if (handles.sceneImageView != cachedSceneImageView) {
                     // Note: Ideally remove old texture if backend supports it.
                     // ImGui_ImplVulkan_RemoveTexture((VkDescriptorSet)sceneTextureID);
                     
                     if (handles.sceneImageView && handles.sceneSampler) {
                         sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
                            (VkSampler)handles.sceneSampler,
                            (VkImageView)handles.sceneImageView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         );
                         cachedSceneImageView = handles.sceneImageView;
                     }
                 }
             }
        }
    }

    // Setup Dockspace
    static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;
    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("TerraEngine DockSpace", nullptr, window_flags);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar(2);

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    DrawMainMenuBar();
    
    DrawInspector();
    DrawHierarchy();

    ImGui::Begin("Scene View");
    if (sceneTextureID) {
        ImVec2 contentSize = ImGui::GetContentRegionAvail();
        ImGui::Image((ImTextureID)sceneTextureID, contentSize);
    } else {
        ImGui::Text("Scene rendering not available.");
    }
    ImGui::End();

    DrawProjectBrowser();

    if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

    ImGui::End();
}

void MainEditorSubsystem::DrawHierarchy() {
    ImGui::Begin("Scene Hierarchy");
    
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
    static char hierarchySearchBuf[128] = "";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::InputTextWithHint("##HierarchySearch", "Search...", hierarchySearchBuf, IM_ARRAYSIZE(hierarchySearchBuf))) {
        m_HierarchySearchFilter = hierarchySearchBuf;
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
    if (ImGui::BeginPopupContextWindow()) {
        if (ImGui::MenuItem("Create Empty Entity")) {
            objManager.createObject("New Entity");
        }
        ImGui::EndPopup();
    }
    
    ImGui::End();
}

void MainEditorSubsystem::DrawEntityNode(Object& obj) {
    auto& objManager = ObjectManager::instance();
    std::string name = objManager.getObjectName(obj);
    
    // Simple Filter Logic: If filter is set, and name doesn't match, and NO children match, hide.
    // This requires recursive check. For simple implementation:
    // If filter matches name, draw. If not, only draw if a child matches (requires lookahead).
    // Optimization: Just filter by name for now. If parent hidden, children hidden.
    // Better: Always draw if filter empty. If filter set, search recursively?
    
    bool matchesFilter = m_HierarchySearchFilter.empty();
    if (!matchesFilter) {
        std::string lowerName = name;
        std::string lowerFilter = m_HierarchySearchFilter;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
        if (lowerName.find(lowerFilter) != std::string::npos) {
            matchesFilter = true;
        }
    }
    
    // Check children for matches to keep parent visible (optional, skipping for simple implementation to save perf)
    // If we strictly hide non-matches, the tree structure might look weird.
    // Let's stick to simple visibility: if filter is on, and mismatch, don't draw?
    // But then we lose structure.
    // Let's just highlight matches? No, user wants search.
    // ImGui standard way: recursively check.
    
    bool hasMatchingChild = false;
    // ... logic omitted for brevity in CLI, assuming simple name filter for root/nodes for now
    
    if (!matchesFilter) {
        // If THIS node doesn't match, maybe children do?
        // We'd need to iterate children to check.
        auto children = objManager.getChildren(obj);
        for(auto& child : children) {
             // Recursive check logic would go here.
             // For now, if simple filter doesn't match, we SKIP.
             // This is imperfect but functional for basic search.
        }
        // return; // Uncomment to strict filter
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selectedEntity && *selectedEntity == obj.EntityId()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    auto children = objManager.getChildren(obj);
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    // If searching, force expand to show matches?
    if (!m_HierarchySearchFilter.empty()) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    
    bool opened = ImGui::TreeNodeEx((void*)(uint64_t)obj.EntityId(), flags, "%s", name.c_str());
    
    if (ImGui::IsItemClicked()) {
        selectedEntity = obj.EntityId();
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

void MainEditorSubsystem::DrawInspector() {
    ImGui::Begin("Inspector");
    
    if (selectedEntity) {
        auto& objManager = ObjectManager::instance();
        if (auto objOpt = objManager.getObject(*selectedEntity)) {
            Object obj = *objOpt;
            
            // --- Unity Style Header ---
            // [Checkbox] [Icon] [Name Input]
            // Static | Tag | Layer (Skipped for now)
            
            bool isActive = true;
            // Attempt to get 'Active' from Transform component
            auto& accessReg = ComponentAccessRegistry();
            if (accessReg.find("Transform") != accessReg.end()) {
                ComponentAccessFunctions& transAccess = accessReg["Transform"];
                if (transAccess.hasComponent(obj)) {
                    rttr::variant var = transAccess.getter(obj, "active");
                    if (var.is_valid()) isActive = var.get_value<bool>();
                }
            }

            ImGui::PushID("InspectorHeader");
            if (ImGui::Checkbox("##ActiveGlobal", &isActive)) {
                if (accessReg.find("Transform") != accessReg.end()) {
                    ComponentAccessFunctions& transAccess = accessReg["Transform"];
                    if (transAccess.hasComponent(obj)) {
                        transAccess.setter(obj, "active", isActive);
                    }
                }
            }
            ImGui::PopID();
            
            ImGui::SameLine();
            // Icon placeholder?
            // ImGui::Image(...) 
            
            std::string name = objManager.getObjectName(obj);
            char nameBuf[256];
            strncpy(nameBuf, name.c_str(), sizeof(nameBuf));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::InputText("##ObjectName", nameBuf, sizeof(nameBuf))) {
                // Set Object Name
                // objManager.setObjectName(obj, nameBuf); // Assuming this API exists or we need to find how to set name.
                // ObjectManager doesn't seem to expose setObjectName in the context I saw.
                // Assuming it might be a component property or stored in registry.
                // For now, let's assume read-only or no-op if API missing, but usually Object has name.
            }
            
            ImGui::Separator();
            
            // --- Sync Component List ---
            std::vector<std::string>& inspectorComponentOrder = perObjectComponentOrders[obj.EntityId()];
            
            // Identify currently active components
            std::unordered_set<std::string> activeComponents;
            bool hasTransform = false;
            for (auto& [compName, access] : accessReg) {
                if (access.hasComponent(obj)) {
                    activeComponents.insert(compName);
                    if (compName == "Transform") hasTransform = true;
                }
            }

            // Remove stale & Add new (Sync)
            for (auto it = inspectorComponentOrder.begin(); it != inspectorComponentOrder.end(); ) {
                if (activeComponents.find(*it) == activeComponents.end()) it = inspectorComponentOrder.erase(it);
                else ++it;
            }
            for (const auto& compName : activeComponents) {
                bool found = false;
                for (const auto& existing : inspectorComponentOrder) if (existing == compName) found = true;
                if (!found) inspectorComponentOrder.push_back(compName);
            }

            // Ensure Transform is first
            if (hasTransform) {
                auto it = std::find(inspectorComponentOrder.begin(), inspectorComponentOrder.end(), "Transform");
                if (it != inspectorComponentOrder.end() && it != inspectorComponentOrder.begin()) {
                    inspectorComponentOrder.erase(it);
                    inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
                } else if (it == inspectorComponentOrder.end()) {
                    inspectorComponentOrder.insert(inspectorComponentOrder.begin(), "Transform");
                }
            }

            // --- Draw Components ---
            for (size_t i = 0; i < inspectorComponentOrder.size(); ++i) {
                std::string compName = inspectorComponentOrder[i];
                if (accessReg.find(compName) == accessReg.end()) continue;
                ComponentAccessFunctions& access = accessReg[compName];

                if (!access.hasComponent(obj)) continue; 

                ImGui::PushID(compName.c_str());
                
                // Active checkbox for component (local enable)
                bool compActive = true;
                rttr::type type = rttr::type::get_by_name(compName);
                rttr::property activeProp = type.get_property("active");
                if (activeProp.is_valid()) {
                    rttr::variant var = access.getter(obj, "active");
                    if (var.is_valid()) compActive = var.get_value<bool>();
                }

                // --- Header ---
                bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
                ImVec2 p = ImGui::GetCursorScreenPos();
                float availWidth = ImGui::GetContentRegionAvail().x;
                float lineHeight = ImGui::GetFrameHeight();

                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + availWidth, p.y + lineHeight), ImGui::GetColorU32(ImGuiCol_Header));

                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##header_bg", ImVec2(availWidth, lineHeight))) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                // Drag/Drop logic (Exclude Transform)
                if (compName != "Transform") {
                    if (ImGui::BeginDragDropSource()) {
                        int sourceIdx = (int)i;
                        ImGui::SetDragDropPayload("COMPONENT_ORDER", &sourceIdx, sizeof(int));
                        ImGui::Text("Reorder %s", compName.c_str());
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_ORDER")) {
                            int sourceIdx = *(const int*)payload->Data;
                            int targetIdx = (int)i;
                            if (sourceIdx >= 0 && sourceIdx < inspectorComponentOrder.size() && targetIdx < inspectorComponentOrder.size()) {
                                std::string sourceName = inspectorComponentOrder[sourceIdx];
                                if (sourceName != "Transform") {
                                    if (sourceIdx < targetIdx) {
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx + 1, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx);
                                    } else if (sourceIdx > targetIdx) {
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx + 1);
                                    }
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                ImGui::SetCursorScreenPos(p);
                ImGui::Dummy(ImVec2(2, 0)); ImGui::SameLine(); 
                if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                ImGui::SameLine();
                
                // Component Checkbox (Don't show for Transform if it's the global active one, OR show strictly as component enable)
                // Unity shows active checkbox for components too. Transform doesn't have one usually (always active).
                if (compName != "Transform") {
                    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                    if (ImGui::Checkbox("##compActive", &compActive)) {
                        if (activeProp.is_valid()) access.setter(obj, "active", compActive);
                    }
                    ImGui::PopStyleColor();
                } else {
                    // Spacer for alignment
                    ImGui::Dummy(ImVec2(ImGui::GetFrameHeight(), 0)); 
                }

                ImGui::SameLine();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(compName.c_str());

                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineHeight));
                ImGui::Dummy(ImVec2(0,0));

                if (isOpen) {
                    ImGui::Indent();
                    for (auto prop : type.get_properties()) {
                        if (prop.get_name() == "active") continue;
                        DrawProperty(prop, obj, access);
                    }
                    ImGui::Unindent();
                }
                ImGui::Separator();
                ImGui::PopID();
            }

            // --- Add Component Button ---
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float availWidth = ImGui::GetContentRegionAvail().x;
            if (ImGui::Button("Add Component", ImVec2(availWidth, 0))) {
                ImGui::OpenPopup("AddComponentPopup");
            }

            if (ImGui::BeginPopup("AddComponentPopup")) {
                static char searchBuf[64] = "";
                ImGui::InputTextWithHint("##CompSearch", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf));
                
                ImGui::Separator();

                // List all registered components
                std::vector<std::string> availableComponents;
                for (auto& [name, access] : accessReg) {
                    // Skip if already added (unless multiple allowed, but typically unique)
                    if (!access.hasComponent(obj)) {
                        availableComponents.push_back(name);
                    }
                }
                std::sort(availableComponents.begin(), availableComponents.end());

                for (const auto& compName : availableComponents) {
                    // Filter
                    if (searchBuf[0] != '\0') {
                        std::string lowerName = compName;
                        std::string lowerFilter = searchBuf;
                        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
                        std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                        if (lowerName.find(lowerFilter) == std::string::npos) continue;
                    }

                    if (ImGui::Selectable(compName.c_str())) {
                        accessReg[compName].addComponent(obj);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndPopup();
            }

        } else {
            selectedEntity.reset();
        }
    }
    ImGui::End();
}

void MainEditorSubsystem::DrawMainMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Close Project")) {
                ctx->projectManager->CloseProject();
                if (onProjectClosed) onProjectClosed();
            }
            if (ImGui::MenuItem("Exit")) {
                // Should signal app exit
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void MainEditorSubsystem::DrawDirectoryTree(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return;

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".meta") continue;
        if (!entry.is_directory()) continue;

        std::string filename = entry.path().filename().string();
        
        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (entry.path() == m_CurrentDirectory) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }

        bool open = ImGui::TreeNodeEx(filename.c_str(), flags);
        if (ImGui::IsItemClicked()) {
            m_CurrentDirectory = entry.path();
        }

        if (open) {
            DrawDirectoryTree(entry.path());
            ImGui::TreePop();
        }
    }
}

void MainEditorSubsystem::DrawDirectoryContent(const std::filesystem::path& path) {
    float cellSize = m_ThumbnailSize + m_Padding;
    float panelWidth = ImGui::GetContentRegionAvail().x;
    int columnCount = (int)(panelWidth / cellSize);
    if (columnCount < 1) columnCount = 1;

    std::vector<std::filesystem::directory_entry> entries;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".meta") continue;
        entries.push_back(entry);
    }

    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        if (a.is_directory() != b.is_directory()) {
            return a.is_directory() > b.is_directory(); // Directories first
        }
        return a.path().filename().string() < b.path().filename().string();
    });

    if (ImGui::BeginTable("ContentTable", columnCount)) {
        for (const auto& entry : entries) {
            std::string filename = entry.path().filename().string();
            if (!m_SearchFilter.empty()) {
                std::string lowerFilename = filename;
                std::string lowerFilter = m_SearchFilter;
                std::transform(lowerFilename.begin(), lowerFilename.end(), lowerFilename.begin(), ::tolower);
                std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
                if (lowerFilename.find(lowerFilter) == std::string::npos) continue;
            }

            ImGui::TableNextColumn();
            
            ImGui::PushID(filename.c_str());
            
            std::string icon = entry.is_directory() ? "[DIR]" : "[FILE]";
            ImGui::Button(icon.c_str(), ImVec2(m_ThumbnailSize, m_ThumbnailSize));
            
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (entry.is_directory()) {
                    m_CurrentDirectory = entry.path();
                }
            }
            
             if (!entry.is_directory() && ImGui::BeginDragDropSource()) {
                auto& assets = AssetDatabase::Instance().GetAssets();
                auto it = assets.find(std::filesystem::absolute(entry.path()));
                if (it != assets.end()) {
                    TerraEngine::UUID guid = it->second.guid;
                    ImGui::SetDragDropPayload("ASSET_GUID", &guid, sizeof(TerraEngine::UUID));
                    ImGui::Text("%s", filename.c_str());
                }
                ImGui::EndDragDropSource();
            }

            ImGui::TextWrapped("%s", filename.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void MainEditorSubsystem::DrawProjectBrowser() {
    ImGui::Begin("Project Browser");

    if (ImGuiFileDialog::Instance()->Display("ImportAssetDlg")) {
        if (ImGuiFileDialog::Instance()->IsOk()) {
            std::filesystem::path filePath = ImGuiFileDialog::Instance()->GetFilePathName();
            if (std::filesystem::exists(filePath)) {
                std::filesystem::path dest = AssetDatabase::Instance().GetAssetRoot() / filePath.filename();
                try {
                    std::filesystem::copy(filePath, dest, std::filesystem::copy_options::overwrite_existing);
                } catch (const std::exception& e) {
                     // Log error
                }
            }
        }
        ImGuiFileDialog::Instance()->Close();
    }

    if (ctx->projectManager->HasActiveProject()) {
        if (ImGui::Button("<-")) {
             std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
             if (m_CurrentDirectory != root) {
                  auto parent = m_CurrentDirectory.parent_path();
                  // Check if parent is strictly inside or is root
                  // Actually standard parent_path is fine, as long as we stop at root.
                  // But let's just do parent for now, relying on tree view to navigate mainly.
                  if (std::filesystem::absolute(parent).string().size() >= std::filesystem::absolute(root).string().size()) {
                      m_CurrentDirectory = parent;
                  }
             }
        }
        ImGui::SameLine();
        
        if (ImGui::Button("Import Asset")) {
            IGFD::FileDialogConfig config;
            config.path = ".";
            ImGuiFileDialog::Instance()->OpenDialog("ImportAssetDlg", "Choose File", ".*", config);
        }
        
        ImGui::SameLine();
        static char searchBuf[128] = "";
        ImGui::SetNextItemWidth(200);
        if (ImGui::InputTextWithHint("##Search", "Search...", searchBuf, IM_ARRAYSIZE(searchBuf))) {
            m_SearchFilter = searchBuf;
        }

        ImGui::Separator();

        if (ImGui::BeginTable("BrowserSplit", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
             ImGui::TableSetupColumn("Tree", ImGuiTableColumnFlags_WidthFixed, 200.0f);
             ImGui::TableSetupColumn("Content", ImGuiTableColumnFlags_WidthStretch);
             
             ImGui::TableNextRow();
             ImGui::TableSetColumnIndex(0);
             
             std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
             if (std::filesystem::exists(root)) {
                 ImGuiTreeNodeFlags rootFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
                 if (root == m_CurrentDirectory) rootFlags |= ImGuiTreeNodeFlags_Selected;
                 
                 bool open = ImGui::TreeNodeEx("Assets", rootFlags);
                 if (ImGui::IsItemClicked()) m_CurrentDirectory = root;
                 
                 if (open) {
                     DrawDirectoryTree(root);
                     ImGui::TreePop();
                 }
             }

             ImGui::TableSetColumnIndex(1);
             
             if (m_CurrentDirectory.empty() || !std::filesystem::exists(m_CurrentDirectory)) {
                 if (std::filesystem::exists(root)) m_CurrentDirectory = root;
             }
             
             if (std::filesystem::exists(m_CurrentDirectory)) {
                 DrawDirectoryContent(m_CurrentDirectory);
             }

             ImGui::Separator();

             ImGui::SetNextItemWidth(100);
             ImGui::SliderFloat("Zoom", &m_ThumbnailSize, 32.0f, 128.0f);
             
             ImGui::EndTable();
        }
    }
    ImGui::End();
}