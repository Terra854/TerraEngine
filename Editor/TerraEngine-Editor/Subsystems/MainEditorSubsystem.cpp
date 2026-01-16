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
#include "Engine/Scripting/ScriptingSystem.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <fstream>

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

    ScriptingSystem::Instance().SetRunning(false);
}

void MainEditorSubsystem::OnProjectOpened() {
    if (!ctx || !ctx->projectManager->HasActiveProject()) return;

    if (auto browser = GetWindow<ProjectBrowserWindow>()) {
        browser->Reset();
    }

    auto projectRoot = ctx->projectManager->GetProjectRoot();
    std::filesystem::path assetRoot = projectRoot / "Assets";
    std::filesystem::path sceneRoot = assetRoot / "Scenes";
    
    if (!std::filesystem::exists(sceneRoot)) {
        std::filesystem::create_directories(sceneRoot);
    }

    // Set Scene Root to Assets folder so we can load scenes from anywhere inside Assets
    SceneManager::Instance().SetSceneRoot(assetRoot.string());
    
    // Check for last opened scene
    std::filesystem::path lastScenePath = projectRoot / "Library" / "LastScene.txt";
    bool loaded = false;
    
    if (std::filesystem::exists(lastScenePath)) {
        std::ifstream in(lastScenePath);
        std::string relPath;
        if (std::getline(in, relPath) && !relPath.empty()) {
            // Verify it still exists
            if (std::filesystem::exists(assetRoot / relPath)) {
                OpenScene(relPath);
                if (sceneLoaded) loaded = true;
            }
        }
    }
    
    if (!loaded) {
        bool foundAny = false;
        if (std::filesystem::exists(assetRoot)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
                if (entry.is_regular_file() && entry.path().extension() == ".tescene") {
                    std::error_code ec;
                    auto rel = std::filesystem::relative(entry.path(), assetRoot, ec);
                    if (!ec) {
                        OpenScene(rel);
                        foundAny = true;
                        break;
                    }
                }
            }
        }
        
        if (!foundAny) {
             // Maybe create a new one? For now just do nothing or log.
        }
    }
}

void MainEditorSubsystem::OpenScene(const std::filesystem::path& path) {
    TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(path);
    if (sceneId != TerraEngine::EMPTY_UUID) {
        SceneManager::Instance().SetActiveScene(sceneId);
        sceneLoaded = true;
        
        // Save to LastScene.txt
        if (ctx && ctx->projectManager->HasActiveProject()) {
            auto projectRoot = ctx->projectManager->GetProjectRoot();
            std::filesystem::path libDir = projectRoot / "Library";
            if (!std::filesystem::exists(libDir)) {
                std::filesystem::create_directories(libDir);
            }
            
            std::ofstream out(libDir / "LastScene.txt");
            // path is typically relative to SceneRoot.
            // If path is absolute, we should make it relative if possible,
            // but SceneManager usually takes relative paths.
            // Assuming path is relative to SceneRoot (Assets/Scenes)
            out << path.generic_string();
        }
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

    DrawMainMenuBar();

    // Toolbar
    {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        auto& colors = ImGui::GetStyle().Colors;
        const auto& buttonHovered = colors[ImGuiCol_ButtonHovered];
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(buttonHovered.x, buttonHovered.y, buttonHovered.z, 0.5f));
        const auto& buttonActive = colors[ImGuiCol_ButtonActive];
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(buttonActive.x, buttonActive.y, buttonActive.z, 0.5f));

        float size = ImGui::GetWindowHeight() - 4.0f; // Constrain to not take too much space? No, this is the main window.
        // We want a small toolbar strip.
        // Since we are inside the DockSpace window, but before DockSpace node, we are just drawing at the top.
        
        // Let's use a fixed height for the toolbar area to ensure DockSpace doesn't overlap weirdly or just let flow handle it.
        // But we want to center the buttons.
        
        float availWidth = ImGui::GetContentRegionAvail().x;
        // Buttons: Play/Stop, Pause, Step
        float buttonSize = 20.0f; // Approx height
        const char* playLabel = ScriptingSystem::Instance().IsRunning() ? "Stop" : "Play";
        const char* pauseLabel = "Pause";
        const char* stepLabel = "Next";
        
        // Calculate width
        ImVec2 playSize = ImGui::CalcTextSize(playLabel);
        ImVec2 pauseSize = ImGui::CalcTextSize(pauseLabel);
        ImVec2 stepSize = ImGui::CalcTextSize(stepLabel);
        
        float padding = 10.0f;
        float totalWidth = playSize.x + pauseSize.x + stepSize.x + (padding * 2) + (ImGui::GetStyle().ItemSpacing.x * 2);
        
        ImGui::SetCursorPosX((availWidth - totalWidth) * 0.5f);
        
        // Play/Stop
        if (ImGui::Button(playLabel)) {
            bool running = ScriptingSystem::Instance().IsRunning();
            ScriptingSystem::Instance().SetRunning(!running);
            if (!running) {
                // We just started
                ScriptingSystem::Instance().SetPaused(false);
                // TODO: Save scene state
            } else {
                // We just stopped
                // TODO: Restore scene state
            }
        }
        ImGui::SameLine();
        
        // Pause
        // Highlight if paused
        bool isPaused = ScriptingSystem::Instance().IsPaused();
        if (isPaused) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
        if (ImGui::Button(pauseLabel)) {
             ScriptingSystem::Instance().SetPaused(!isPaused);
        }
        if (isPaused) ImGui::PopStyleColor();
        ImGui::SameLine();
        
        // Step
        if (ImGui::Button(stepLabel)) {
            if (ScriptingSystem::Instance().IsRunning() && ScriptingSystem::Instance().IsPaused()) {
                ScriptingSystem::Instance().Step(1);
            }
        }

        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar(2);
    }

    ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
    ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

    for (auto& win : m_Windows) {
        win->Draw();
    }

    DrawSaveConfirmationModal();

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
                RequestCloseProject();
            }
            if (ImGui::MenuItem("Exit")) {
                RequestExit();
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

void MainEditorSubsystem::RequestOpenScene(const std::filesystem::path& path) {
    if (SceneManager::Instance().IsActiveSceneDirty()) {
        m_PendingAction = PendingAction::OpenScene;
        m_PendingScenePath = path;
        m_ShowSaveConfirmation = true;
    } else {
        OpenScene(path);
    }
}

void MainEditorSubsystem::RequestCloseProject() {
    if (SceneManager::Instance().IsActiveSceneDirty()) {
        m_PendingAction = PendingAction::CloseProject;
        m_ShowSaveConfirmation = true;
    } else {
        CloseCurrentProject();
    }
}

void MainEditorSubsystem::RequestExit() {
    if (SceneManager::Instance().IsActiveSceneDirty()) {
        m_PendingAction = PendingAction::Exit;
        m_ShowSaveConfirmation = true;
    } else {
        if (ctx && ctx->engine) ctx->engine->SignalShutdown();
    }
}

void MainEditorSubsystem::ExecutePendingAction() {
    switch (m_PendingAction) {
        case PendingAction::OpenScene:
            OpenScene(m_PendingScenePath);
            break;
        case PendingAction::CloseProject:
            CloseCurrentProject();
            break;
        case PendingAction::Exit:
            if (ctx && ctx->engine) ctx->engine->SignalShutdown();
            break;
        default:
            break;
    }
    m_PendingAction = PendingAction::None;
    m_PendingScenePath.clear();
}

void MainEditorSubsystem::DrawSaveConfirmationModal() {
    if (m_ShowSaveConfirmation) {
        ImGui::OpenPopup("Save Changes?");
        m_ShowSaveConfirmation = false;
    }

    if (ImGui::BeginPopupModal("Save Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("There are unsaved changes to the current scene.\nDo you want to save them?");
        ImGui::Separator();

        if (ImGui::Button("Yes", ImVec2(120, 0))) {
            SceneManager::Instance().SaveScene();
            ExecutePendingAction();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SetItemDefaultFocus();
        ImGui::SameLine();
        if (ImGui::Button("No", ImVec2(120, 0))) {
            ExecutePendingAction();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120, 0))) {
            m_PendingAction = PendingAction::None;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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
