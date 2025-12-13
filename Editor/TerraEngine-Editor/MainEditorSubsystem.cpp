#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Component.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/vec3.hpp>
#include <glm/gtc/quaternion.hpp>

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
    auto roots = objManager.getRootObjects();
    
    for (auto& obj : roots) {
        DrawEntityNode(obj);
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
            
            // Components
            auto& accessReg = ComponentAccessRegistry();
            for (auto& [compName, access] : accessReg) {
                if (access.hasComponent(obj)) {
                    if (ImGui::CollapsingHeader(compName.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        rttr::type type = rttr::type::get_by_name(compName);
                        for (auto prop : type.get_properties()) {
                            DrawProperty(prop, obj, access);
                        }
                    }
                }
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

void MainEditorSubsystem::DrawProjectBrowser() {
    ImGui::Begin("Project Browser");
    if (ctx->projectManager->HasActiveProject()) {
        auto root = ctx->projectManager->GetProjectRoot();
        ImGui::Text("Root: %s", root.string().c_str());
        // Simple recursive list (TODO: AssetDatabase integration)
        // For now, just show assets folder
        ImGui::Separator();
        ImGui::Text("Assets/");
    }
    ImGui::End();
}