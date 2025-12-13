#include "EditorSystem.h"
#include "ProjectHubSubsystem.h"
#include "MainEditorSubsystem.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Window/WindowSystem.h"

// ImGui
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>
#include <ImGuiFileDialog.h>
#include <GLFW/glfw3.h>
#include <utility>

void EditorSystem::Setup(Engine* enginePtr) {
    if (initialized) return;

    // Setup Context
    context.engine = enginePtr;
    context.projectManager = std::make_shared<ProjectManager>();
    // context.assetDatabase = std::make_shared<AssetDatabase>(); // Pending

    // Create Subsystems
    projectHub = std::make_shared<ProjectHubSubsystem>();
    mainEditor = std::make_shared<MainEditorSubsystem>();

    projectHub->Initialize(context);
    mainEditor->Initialize(context);

    // Wire Callbacks
    projectHub->SetOnProjectLoadedCallback([this]() {
        // Transition logic
        mainEditor->OnProjectOpened();
    });

    mainEditor->SetOnProjectClosedCallback([this]() {
        // Transition logic
    });

    initialized = true;
}

void EditorSystem::Initialize() {
    // Initialize ImGui and Backends
    
    // 1. Get Vulkan Context
    auto& renderSystem = RenderSystem::Instance();
    auto* rhi = renderSystem.GetContext();
    auto* vulkan = dynamic_cast<VulkanContext*>(rhi);
    if (!vulkan) {
        // Log Error
        return;
    }
    
    vulkan->EnableEditorMode();
    VulkanContext::EditorHandles handles;
    vulkan->GetEditorHandles(handles);

    // 2. Get Window
    auto& windowSystem = WindowSystem::Instance();
    GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());

    // 3. Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    // io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Optional, maybe later

    ImGui::StyleColorsDark();

    // 4. Init Backends
    ImGui_ImplGlfw_InitForVulkan(window, true);
    
    ImGui_ImplVulkan_InitInfo init_info = {};
    init_info.Instance = (VkInstance)handles.instance;
    init_info.PhysicalDevice = (VkPhysicalDevice)handles.physicalDevice;
    init_info.Device = (VkDevice)handles.device;
    init_info.QueueFamily = handles.queueFamily;
    init_info.Queue = (VkQueue)handles.queue;
    init_info.PipelineCache = VK_NULL_HANDLE;
    init_info.DescriptorPool = (VkDescriptorPool)handles.descriptorPool;
    init_info.MinImageCount = handles.minImageCount;
    init_info.ImageCount = handles.imageCount;
    init_info.Allocator = nullptr;
    init_info.CheckVkResultFn = nullptr;
    init_info.PipelineInfoMain.RenderPass = (VkRenderPass)handles.renderPass;
    init_info.PipelineInfoMain.Subpass = 0;
    init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    
    // Load Vulkan functions
    std::pair<PFN_vkGetInstanceProcAddr, VkInstance> loaderParams = {
        (PFN_vkGetInstanceProcAddr)handles.vkGetInstanceProcAddr,
        (VkInstance)handles.instance
    };
    ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, [](const char* function_name, void* user_data) {
        auto* p = (std::pair<PFN_vkGetInstanceProcAddr, VkInstance>*)user_data;
        return p->first(p->second, function_name);
    }, &loaderParams);
    
    ImGui_ImplVulkan_Init(&init_info);
    
    // ImGui_ImplVulkan_CreateFontsTexture(); // Handled by NewFrame or Init

    if (projectHub) projectHub->OnBackendReady();
    if (mainEditor) mainEditor->OnBackendReady();
}

void EditorSystem::Update() {
    if (!initialized) return;

    // Standard ImGui Frame
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    
    float dt = 0.016f; // Placeholder, MainEngine handles timing for subsystems usually via Update() delta
    
    if (context.projectManager->HasActiveProject()) {
        mainEditor->Update(dt);
        mainEditor->DrawUI();
    } else {
        projectHub->Update(dt);
        projectHub->DrawUI();
    }
    
    ImGui::Render();
    
    // Present
    auto& renderSystem = RenderSystem::Instance();
    auto* rhi = renderSystem.GetContext();
    auto* vulkan = dynamic_cast<VulkanContext*>(rhi);
    if (vulkan) {
        vulkan->PresentEditor([](void* cmd){
             ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)cmd);
        });
    }
}

void EditorSystem::Shutdown() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    if (mainEditor) mainEditor->Shutdown();
    if (projectHub) projectHub->Shutdown();
    
    context.projectManager.reset();
    context.assetDatabase.reset();
    
    initialized = false;
}

std::string EditorSystem::SystemName() {
    return "EditorSystem";
}
