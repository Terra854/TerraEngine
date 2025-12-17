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

#include <unordered_map>

// Static storage for callback chaining
static std::unordered_map<GLFWwindow*, EditorSystem*> g_EditorSystems;
static GLFWwindowclosefun g_PrevCloseCallback = nullptr;
static GLFWframebuffersizefun g_PrevResizeCallback = nullptr;
static GLFWmousebuttonfun g_PrevMouseButtonCallback = nullptr;
static GLFWscrollfun g_PrevScrollCallback = nullptr;
static GLFWkeyfun g_PrevKeyCallback = nullptr;
static GLFWcharfun g_PrevCharCallback = nullptr;
static GLFWdropfun g_PrevDropCallback = nullptr;

// Wraps the OS cursor to the opposite edge while dragging inspector values.
// Input: window handle for size/position queries. Output: cursor warp plus ImGui state sync.
static void WrapCursorForInspectorDrag(GLFWwindow* window) {
	if (!window) {
		return;
	}

	int width = 0;
	int height = 0;
	glfwGetWindowSize(window, &width, &height);
	if (width <= 4 || height <= 4) {
		return;
	}

	double mouseX = 0.0;
	double mouseY = 0.0;
	glfwGetCursorPos(window, &mouseX, &mouseY);

	const double wrapPadding = 2.0;
	const double maxX = static_cast<double>(width - 1);
	const double maxY = static_cast<double>(height - 1);
	bool wrapped = false;

	if (mouseX <= wrapPadding) {
		mouseX = maxX - wrapPadding;
		wrapped = true;
	} else if (mouseX >= maxX - wrapPadding) {
		mouseX = wrapPadding;
		wrapped = true;
	}

	if (mouseY <= wrapPadding) {
		mouseY = maxY - wrapPadding;
		wrapped = true;
	} else if (mouseY >= maxY - wrapPadding) {
		mouseY = wrapPadding;
		wrapped = true;
	}

	if (wrapped) {
		glfwSetCursorPos(window, mouseX, mouseY);
		ImGuiIO& io = ImGui::GetIO();
		// Keep ImGui deltas small so value drags stay smooth after wrapping.
		io.MousePos = ImVec2(static_cast<float>(mouseX), static_cast<float>(mouseY));
		io.MouseDelta = ImVec2(0.0f, 0.0f);
	}
}

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
        // Initialize Asset Database with project root
        if (context.projectManager->HasActiveProject()) {
            AssetDatabase::Instance().Initialize(context.projectManager->GetProjectRoot());
        }
        // Transition logic
        mainEditor->OnProjectOpened();
    });

    mainEditor->SetOnProjectClosedCallback([this]() {
        AssetDatabase::Instance().Shutdown();
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

    // Register Drop Callback safely using static map
    g_EditorSystems[window] = this;
    
    // 3. Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();

    // 4. Init Backends
    // IMPORTANT: InstallCallbacks = false to prevent overwriting Engine's callbacks
    ImGui_ImplGlfw_InitForVulkan(window, false);
    
    // 5. Chain Callbacks
    g_PrevResizeCallback = glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int width, int height) {
        if (g_PrevResizeCallback) g_PrevResizeCallback(w, width, height);
        // ImGui doesn't export WindowSize callback, but NewFrame pulls it.
        // However, we can call this if needed or rely on NewFrame.
        // ImGui_ImplGlfw_WindowSizeCallback(w, width, height); 
        // Note: ImGui_ImplGlfw usually installs WindowSize callback? No, FramebufferSize.
        // We will leave ImGui to poll in NewFrame or use specific function if exported.
        // Actually ImGui_ImplGlfw_NewFrame calls glfwGetFramebufferSize.
    });

    g_PrevMouseButtonCallback = glfwSetMouseButtonCallback(window, [](GLFWwindow* w, int button, int action, int mods) {
        if (g_PrevMouseButtonCallback) g_PrevMouseButtonCallback(w, button, action, mods);
        ImGui_ImplGlfw_MouseButtonCallback(w, button, action, mods);
    });

    g_PrevScrollCallback = glfwSetScrollCallback(window, [](GLFWwindow* w, double xoffset, double yoffset) {
        if (g_PrevScrollCallback) g_PrevScrollCallback(w, xoffset, yoffset);
        ImGui_ImplGlfw_ScrollCallback(w, xoffset, yoffset);
    });

    g_PrevKeyCallback = glfwSetKeyCallback(window, [](GLFWwindow* w, int key, int scancode, int action, int mods) {
        if (g_PrevKeyCallback) g_PrevKeyCallback(w, key, scancode, action, mods);
        ImGui_ImplGlfw_KeyCallback(w, key, scancode, action, mods);
    });

    g_PrevCharCallback = glfwSetCharCallback(window, [](GLFWwindow* w, unsigned int c) {
        if (g_PrevCharCallback) g_PrevCharCallback(w, c);
        ImGui_ImplGlfw_CharCallback(w, c);
    });
    
    // Restore/Chain Drop Callback
    g_PrevDropCallback = glfwSetDropCallback(window, [](GLFWwindow* w, int count, const char** paths) {
         if (g_EditorSystems.count(w)) {
             EditorSystem* self = g_EditorSystems[w];
             if (self && self->context.projectManager->HasActiveProject()) {
                std::filesystem::path assetRoot = AssetDatabase::Instance().GetAssetRoot();
                for (int i = 0; i < count; i++) {
                    std::filesystem::path src = paths[i];
                    if (std::filesystem::exists(src)) {
                        try {
                            std::filesystem::copy(src, assetRoot / src.filename(), std::filesystem::copy_options::overwrite_existing);
                        } catch (...) {}
                    }
                }
            }
         }
    });
    
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
        AssetDatabase::Instance().Update();
        mainEditor->Update(dt);
        mainEditor->DrawUI();
		// Enable cursor wrap only while dragging inspector values to avoid edge clipping.
		if (mainEditor && mainEditor->IsInspectorValueDragging()) {
			auto& windowSystem = WindowSystem::Instance();
			GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem.GetNativeHandle());
			WrapCursorForInspectorDrag(window);
		}
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
    
    AssetDatabase::Instance().Shutdown();
    context.projectManager.reset();
    
    initialized = false;
}

std::string EditorSystem::SystemName() {
    return "EditorSystem";
}
