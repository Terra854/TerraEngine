#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "EditorSystem.h"
#include "UndoRedoSystem.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Resource/ResourceManager.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>

// Include Window Headers
#include "../Windows/HierarchyWindow.h"
#include "../Windows/InspectorWindow.h"
#include "../Windows/ProjectBrowserWindow.h"
#include "../Windows/GameViewWindow.h"
#include "../Windows/SceneViewWindow.h"

void MainEditorSubsystem::Initialize(EditorContext& context) {
    ctx = &context;
    
    // Initialize Windows
    m_Windows.push_back(std::make_shared<HierarchyWindow>(this));
    m_Windows.push_back(std::make_shared<InspectorWindow>(this));
    m_Windows.push_back(std::make_shared<ProjectBrowserWindow>(this));
    m_Windows.push_back(std::make_shared<GameViewWindow>(this));
    m_Windows.push_back(std::make_shared<SceneViewWindow>(this));

    UndoRedoSystem::Instance().Initialize(context);

    ObjectManager::instance().SetOnModifiedCallback([]() {
        SceneManager::Instance().MarkActiveSceneDirty();
    });
}

void MainEditorSubsystem::OnProjectOpened() {
    if (!ctx || !ctx->projectManager->HasActiveProject()) return;

    auto projectRoot = ctx->projectManager->GetProjectRoot();
    std::filesystem::path sceneRoot = projectRoot / "Assets" / "Scenes";
    
    if (!std::filesystem::exists(sceneRoot)) {
        std::filesystem::create_directories(sceneRoot);
    }

    SceneManager::Instance().SetSceneRoot(sceneRoot.string());
    
    TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene("Default.tescene");
    if (sceneId != TerraEngine::EMPTY_UUID) {
        SceneManager::Instance().SetActiveScene(sceneId);
        sceneLoaded = true;
    }
}

void MainEditorSubsystem::Shutdown() {
    UndoRedoSystem::Instance().Shutdown();
    ctx = nullptr;
    m_Windows.clear();
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

    if (handles.gameImageView && handles.gameSampler) {
        gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
            (VkSampler)handles.gameSampler,
            (VkImageView)handles.gameImageView,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
        );
        cachedGameImageView = handles.gameImageView;
    }
}

void MainEditorSubsystem::Update(float deltaTime) {
    // Logic update
    
    // Process editor-wide hotkeys.
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
        if (ImGui::IsKeyPressed(ImGuiKey_S)) {
            SceneManager::Instance().SaveScene();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
            UndoRedoSystem::Instance().Undo();
        }
        // Traditional Windows redo shortcut (Ctrl+Y).
        if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
            UndoRedoSystem::Instance().Redo();
        }
    }
}

void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
    onProjectClosed = callback;
}

bool MainEditorSubsystem::IsInspectorValueDragging() const {
    auto inspector = const_cast<MainEditorSubsystem*>(this)->GetWindow<InspectorWindow>();
    if (inspector) return inspector->IsInspectorValueDragging();
    return false;
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
                     if (handles.sceneImageView && handles.sceneSampler) {
                         sceneTextureID = (void*)ImGui_ImplVulkan_AddTexture(
                            (VkSampler)handles.sceneSampler,
                            (VkImageView)handles.sceneImageView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         );
                         cachedSceneImageView = handles.sceneImageView;
                     }
                 }
                 if (handles.gameImageView != cachedGameImageView) {
                     if (handles.gameImageView && handles.gameSampler) {
                         gameTextureID = (void*)ImGui_ImplVulkan_AddTexture(
                            (VkSampler)handles.gameSampler,
                            (VkImageView)handles.gameImageView,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                         );
                         cachedGameImageView = handles.gameImageView;
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
    
    for (auto& win : m_Windows) {
        win->Draw();
    }

    if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

    ImGui::End();
}

void MainEditorSubsystem::DrawMainMenuBar() {
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
                SceneManager::Instance().SaveScene();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close Project")) {
                CloseCurrentProject();
            }
            if (ImGui::MenuItem("Exit")) {
                // TODO: Handle app-level shutdown request.
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            // Context-aware menu enabling ensures users know when history is 
            // available or exhausted.
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false, UndoRedoSystem::Instance().CanUndo())) {
                UndoRedoSystem::Instance().Undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false, UndoRedoSystem::Instance().CanRedo())) {
                UndoRedoSystem::Instance().Redo();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            if (ImGui::BeginMenu("C++")) {
                for (auto& win : m_Windows) {
                    if (ImGui::MenuItem(win->GetTitle().c_str())) {
                        win->SetVisible(true);
                    }
                }
                ImGui::EndMenu();
            }
            
            if (ImGui::BeginMenu("C#")) {
                EditorSystem* editorSystem = nullptr;
                if (ctx && ctx->engine) {
                    editorSystem = dynamic_cast<EditorSystem*>(ctx->engine->GetEditorSystem());
                }
                
                if (editorSystem) {
                    editorSystem->DrawWindowMenu();
                } else {
                    ImGui::TextDisabled("Editor System not found");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }
}

void MainEditorSubsystem::CloseCurrentProject() {
    // Clear Scene
    SceneManager::Instance().UnloadAll();
    
    // Clear Objects
    ObjectManager::instance().clear();
    
    // Clear Resources
    ResourceManager::Instance().ClearCaches();
    
    // Clear Renderer Resources
    // This ensures GPU resources are flushed and binding indices are reset.
    if (ctx && ctx->engine) {
        RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
        if (renderSystem) {
            renderSystem->Clear();
        }
    }

    // Clear Selection
    selectedEntity = std::nullopt;
    selectedAsset = std::nullopt;
    
    // Close Project
    if (ctx && ctx->projectManager) {
        ctx->projectManager->CloseProject();
    }
    
    if (onProjectClosed) onProjectClosed();
}
