#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>

void MainEditorSubsystem::Initialize(EditorContext& context) {
    ctx = &context;
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
    
    // Placeholder Windows
    ImGui::Begin("Inspector");
    ImGui::Text("No object selected.");
    ImGui::End();

    ImGui::Begin("Scene Hierarchy");
    ImGui::Text("Scene Root");
    ImGui::End();

    ImGui::Begin("Game View");
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