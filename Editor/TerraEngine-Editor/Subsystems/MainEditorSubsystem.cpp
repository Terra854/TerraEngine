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
    }
}

void MainEditorSubsystem::Update(float deltaTime) {
    // Logic update
}

void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
    onProjectClosed = callback;
}

void MainEditorSubsystem::DrawUI() {
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
    const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
    std::string sceneName = activeScene ? activeScene->name : "No Active Scene";

    ImGuiTreeNodeFlags sceneNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen;
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
    
    // Right click on background to create entity?
    if (ImGui::BeginPopupContextWindow()) {
        if (ImGui::MenuItem("Create Empty Entity")) {
            objManager.createObject("New Entity");
        }
        ImGui::EndPopup();
    }
    
    ImGui::End();
}

void MainEditorSubsystem::DrawEntityNode(Object& obj) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick;
    if (selectedEntity && *selectedEntity == obj.EntityId()) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    
    auto& objManager = ObjectManager::instance();
    auto children = objManager.getChildren(obj);
    if (children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf;
    }
    
    std::string name = objManager.getObjectName(obj);
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
            
            // Check if dropped entity is not same as current (handled by ObjectManager probably, but good to check)
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
            
            std::string name = objManager.getObjectName(obj);
            ImGui::Text("Entity: %s", name.c_str());
            ImGui::TextDisabled("ID: %d", (uint32_t)obj.EntityId());
            ImGui::Separator();
            
            // --- Sync Component List ---
            // Get or create order list for this entity
            std::vector<std::string>& inspectorComponentOrder = perObjectComponentOrders[obj.EntityId()];
            auto& accessReg = ComponentAccessRegistry();
            
            // Identify currently active components
            std::unordered_set<std::string> activeComponents;
            bool hasTransform = false;
            for (auto& [compName, access] : accessReg) {
                if (access.hasComponent(obj)) {
                    activeComponents.insert(compName);
                    if (compName == "Transform") hasTransform = true;
                }
            }

            // Remove stale
            for (auto it = inspectorComponentOrder.begin(); it != inspectorComponentOrder.end(); ) {
                if (activeComponents.find(*it) == activeComponents.end()) {
                    it = inspectorComponentOrder.erase(it);
                } else {
                    ++it;
                }
            }

            // Add new
            for (const auto& compName : activeComponents) {
                bool found = false;
                for (const auto& existing : inspectorComponentOrder) {
                    if (existing == compName) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    // New component found
                    inspectorComponentOrder.push_back(compName);
                }
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
                
                bool isActive = true;
                rttr::type type = rttr::type::get_by_name(compName);
                rttr::property activeProp = type.get_property("active");
                if (activeProp.is_valid()) {
                    rttr::variant var = access.getter(obj, "active");
                    if (var.is_valid()) isActive = var.get_value<bool>();
                }

                // --- Manual Header Drawing ---
                bool isOpen = ImGui::GetStateStorage()->GetInt(ImGui::GetID("IsOpen"), 1) != 0;
                
                ImVec2 p = ImGui::GetCursorScreenPos();
                float availWidth = ImGui::GetContentRegionAvail().x;
                float lineHeight = ImGui::GetFrameHeight();

                // Background
                ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + availWidth, p.y + lineHeight), ImGui::GetColorU32(ImGuiCol_Header));

                // Toggle Button (Invisible, full width)
                ImGui::SetNextItemAllowOverlap();
                if (ImGui::InvisibleButton("##header_bg", ImVec2(availWidth, lineHeight))) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                // Drag Source (Exclude Transform)
                if (compName != "Transform") {
                    if (ImGui::BeginDragDropSource()) {
                        int sourceIdx = (int)i;
                        ImGui::SetDragDropPayload("COMPONENT_ORDER", &sourceIdx, sizeof(int));
                        ImGui::Text("Reorder %s", compName.c_str());
                        ImGui::EndDragDropSource();
                    }
                }

                // Drop Target (Exclude Transform)
                if (compName != "Transform") {
                    if (ImGui::BeginDragDropTarget()) {
                        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_ORDER")) {
                            int sourceIdx = *(const int*)payload->Data;
                            int targetIdx = (int)i;
                            
                            // Ensure target is valid size and source is valid
                            if (sourceIdx >= 0 && sourceIdx < inspectorComponentOrder.size() && targetIdx < inspectorComponentOrder.size()) {
                                std::string sourceName = inspectorComponentOrder[sourceIdx];
                                if (sourceName != "Transform") { // Cannot move Transform
                                    // Move logic
                                    if (sourceIdx < targetIdx) {
                                        // Moving down: insert at target+1, then erase source
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx + 1, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx);
                                    } else if (sourceIdx > targetIdx) {
                                        // Moving up: insert at target, then erase source+1
                                        inspectorComponentOrder.insert(inspectorComponentOrder.begin() + targetIdx, sourceName);
                                        inspectorComponentOrder.erase(inspectorComponentOrder.begin() + sourceIdx + 1);
                                    }
                                }
                            }
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                // Draw Widgets on Top
                ImGui::SetCursorScreenPos(p);

                // Arrow
                ImGui::Dummy(ImVec2(2, 0)); ImGui::SameLine(); 
                if (ImGui::ArrowButton("##arrow", isOpen ? ImGuiDir_Down : ImGuiDir_Right)) {
                    isOpen = !isOpen;
                    ImGui::GetStateStorage()->SetInt(ImGui::GetID("IsOpen"), isOpen);
                }

                ImGui::SameLine();

                // Checkbox
                ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.2f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Checkbox("##active", &isActive)) {
                    if (activeProp.is_valid()) {
                        access.setter(obj, "active", isActive);
                    }
                }
                ImGui::PopStyleColor();

                ImGui::SameLine();

                // Name
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(compName.c_str());

                // Advance cursor to next line
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lineHeight));
                ImGui::Dummy(ImVec2(0,0));

                if (isOpen) {
                    ImGui::Indent();
                    for (auto prop : type.get_properties()) {
                        // Skip 'active' as it's in the header
                        if (prop.get_name() == "active") continue;
                        DrawProperty(prop, obj, access);
                    }
                    ImGui::Unindent();
                }
                ImGui::PopID();
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

static void DrawDirectoryRecursive(const std::filesystem::path& path) {
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (entry.path().extension() == ".meta") continue;

        if (entry.is_directory()) {
            if (ImGui::TreeNode(entry.path().filename().string().c_str())) {
                DrawDirectoryRecursive(entry.path());
                ImGui::TreePop();
            }
        } else {
            std::string filename = entry.path().filename().string();
            ImGui::Selectable(filename.c_str());

            if (ImGui::BeginDragDropSource()) {
                // Find GUID
                auto& assets = AssetDatabase::Instance().GetAssets();
                auto it = assets.find(std::filesystem::absolute(entry.path()));
                if (it != assets.end()) {
                    TerraEngine::UUID guid = it->second.guid;
                    ImGui::SetDragDropPayload("ASSET_GUID", &guid, sizeof(TerraEngine::UUID));
                    ImGui::Text("%s", filename.c_str());
                }
                ImGui::EndDragDropSource();
            }
        }
    }
}

void MainEditorSubsystem::DrawProjectBrowser() {
    ImGui::Begin("Project Browser");
    
    if (ImGui::Button("Import Asset")) {
        IGFD::FileDialogConfig config;
        config.path = ".";
        ImGuiFileDialog::Instance()->OpenDialog("ImportAssetDlg", "Choose File", ".*", config);
    }
    
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

    ImGui::Separator();

    if (ctx->projectManager->HasActiveProject()) {
        std::filesystem::path root = AssetDatabase::Instance().GetAssetRoot();
        if (std::filesystem::exists(root)) {
             DrawDirectoryRecursive(root);
        } else {
            ImGui::Text("Assets directory not found.");
        }
    }
    ImGui::End();
}