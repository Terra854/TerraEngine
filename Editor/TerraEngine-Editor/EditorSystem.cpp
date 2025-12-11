#include "EditorSystem.h"
#include <GLFW/glfw3.h>
#include "Engine/Window/WindowSystem.h"
#include "Engine/Window/WindowBackend.h"
#include "Engine/Core/Logger/Logger.h"

static void check_vk_result(VkResult err) {
	if (err == 0) return;
	// Log the Vulkan error through the engine logger so it is captured consistently before aborting.
	TERRAENGINE_LOG(LogLevel::Error, "[ImGui] Vulkan Error: VkResult = " << err);
	if (err < 0) abort();
}

EditorSystem::EditorSystem(Engine& engine) : engine(engine) {}

EditorSystem::~EditorSystem() {}

void EditorSystem::Initialize() {
	// Ensure fatal initialization errors log once and request a clean engine shutdown.
	auto logCriticalAndShutdown = [this](const std::string& message) {
		TERRAENGINE_LOG(LogLevel::Critical, message);
		engine.SignalShutdown();
	};

	// Track ImGui startup state so we can unwind partial initialization when failures occur.
	bool imguiContextCreated = false;
	bool imguiGlfwInitialized = false;
	auto cleanupImGui = [&]() {
		if (imguiGlfwInitialized) {
			ImGui_ImplGlfw_Shutdown();
			imguiGlfwInitialized = false;
		}
		if (imguiContextCreated) {
			ImGui::DestroyContext();
			imguiContextCreated = false;
		}
	};

	RenderSystem* renderSystem = engine.GetSystem<RenderSystem>();
	windowSystem = engine.GetSystem<WindowSystem>();

	if (!renderSystem || !windowSystem) {
		logCriticalAndShutdown("Failed to retrieve required systems!");
		return;
	}

	rhi = dynamic_cast<VulkanContext*>(renderSystem->GetContext());
	if (!rhi) {
		logCriticalAndShutdown("RenderSystem is not using VulkanContext!");
		return;
	}

	rhi->EnableEditorMode();
	
	VulkanContext::EditorHandles handles;
	rhi->GetEditorHandles(handles);

	if (handles.device == nullptr) {
		logCriticalAndShutdown("Invalid Vulkan handles!");
		return;
	}

	GLFWwindow* window = static_cast<GLFWwindow*>(windowSystem->GetBackend()->GetNativeHandle());

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	imguiContextCreated = true;
	ImGuiIO& io = ImGui::GetIO(); (void)io;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

	ImGui::StyleColorsDark();

	ImGui_ImplGlfw_InitForVulkan(window, true);
	imguiGlfwInitialized = true;

	// Log the proc address so debugging Vulkan loader issues goes through the shared logger.
	TERRAENGINE_LOG(LogLevel::Debug, "vkGetInstanceProcAddr: " << handles.vkGetInstanceProcAddr);

	struct LoaderData {
		PFN_vkGetInstanceProcAddr loader;
		VkInstance instance;
	};
	LoaderData loaderData;
	loaderData.loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(handles.vkGetInstanceProcAddr);
	loaderData.instance = static_cast<VkInstance>(handles.instance);

	// Load Vulkan functions for ImGui using the engine's proc addr
	// We must use the instance for instance-level commands, and NULL for global commands.
	bool loadResult = ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_0, [](const char* function_name, void* user_data) {
		auto* data = reinterpret_cast<LoaderData*>(user_data);
		if (strcmp(function_name, "vkCreateInstance") == 0 || 
			strcmp(function_name, "vkEnumerateInstanceExtensionProperties") == 0 || 
			strcmp(function_name, "vkEnumerateInstanceLayerProperties") == 0 ||
			strcmp(function_name, "vkGetInstanceProcAddr") == 0) {
			return data->loader(VK_NULL_HANDLE, function_name);
		}
		return data->loader(data->instance, function_name);
	}, &loaderData);

	if (!loadResult) {
		cleanupImGui();
		logCriticalAndShutdown("ImGui_ImplVulkan_LoadFunctions failed!");
		return;
	}

	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = static_cast<VkInstance>(handles.instance);
	init_info.PhysicalDevice = static_cast<VkPhysicalDevice>(handles.physicalDevice);
	init_info.Device = static_cast<VkDevice>(handles.device);
	init_info.QueueFamily = handles.queueFamily;
	init_info.Queue = static_cast<VkQueue>(handles.queue);
	init_info.PipelineCache = VK_NULL_HANDLE;
	init_info.DescriptorPool = static_cast<VkDescriptorPool>(handles.descriptorPool);
	init_info.PipelineInfoMain.RenderPass = static_cast<VkRenderPass>(handles.renderPass);
	init_info.PipelineInfoMain.Subpass = 0;
	init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	init_info.MinImageCount = handles.minImageCount;
	init_info.ImageCount = handles.imageCount;
	init_info.Allocator = nullptr;
	init_info.CheckVkResultFn = check_vk_result;

	ImGui_ImplVulkan_Init(&init_info);

	// Create Scene Texture Descriptor
	VkSampler sampler = static_cast<VkSampler>(handles.sceneSampler);
	VkImageView imageView = static_cast<VkImageView>(handles.sceneImageView);
	sceneDescriptorSet = ImGui_ImplVulkan_AddTexture(sampler, imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	initialized = true;
	TERRAENGINE_LOG(LogLevel::Info, "Editor system Initialized.");
}

void EditorSystem::Update() {
	if (!initialized) return;

	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

	ImGui::Begin("Game Viewport");
	ImVec2 viewportSize = ImGui::GetContentRegionAvail();
	if (sceneDescriptorSet) {
		ImGui::Image((ImTextureID)sceneDescriptorSet, viewportSize);
	}
	ImGui::End();

	ImGui::ShowDemoWindow();

	ImGui::Render();

	// Pass the render callback to the RHI to execute inside the frame loop
	rhi->PresentEditor([this](void* cmd) {
		this->RenderUI(cmd);
	});
}

void EditorSystem::RenderUI(void* cmdBuffer) {
	ImDrawData* drawData = ImGui::GetDrawData();
	if (drawData) {
		ImGui_ImplVulkan_RenderDrawData(drawData, static_cast<VkCommandBuffer>(cmdBuffer));
	}
}

void EditorSystem::Shutdown() {
	if (initialized) {
		ImGui_ImplVulkan_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
	}
}

std::string EditorSystem::SystemName() {
	return "EditorSystem";
}
