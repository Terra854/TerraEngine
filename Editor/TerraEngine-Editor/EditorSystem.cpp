#include "EditorSystem.h"
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/epsilon.hpp>
#include "Engine/Window/WindowSystem.h"
#include "Engine/Window/WindowBackend.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Resource/ResourceManager.h"

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

	// Visualize the scene hierarchy so authored objects are easy to inspect.
	DrawHierarchyWindow();
	DrawInspectorWindow();

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

void EditorSystem::DrawHierarchyWindow() {
	if (!ImGui::Begin("Hierarchy")) {
		ImGui::End();
		return;
	}

	ObjectManager& objects = ObjectManager::instance();
	if (ImGui::TreeNodeEx("Scene", ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth)) {
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
				entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
				if (auto childObj = objects.getObject(dropped)) {
					objects.setParent(*childObj, std::nullopt);
				}
			}
			ImGui::EndDragDropTarget();
		}
		std::vector<Object> roots = objects.getRootObjects();
		for (const Object& root : roots) {
			DrawHierarchyNode(root);
		}
		ImGui::TreePop();
	}

	ImGui::End();
}

void EditorSystem::DrawHierarchyNode(const Object& object) {
	ObjectManager& objects = ObjectManager::instance();
	std::string label = objects.getObjectName(object);
	std::vector<Object> children = objects.getChildren(object);

	const bool isSelected = selectedObject.has_value() && selectedObject->EntityId() == object.EntityId();
	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
	if (children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
	}
	if (isSelected) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}

	ImGui::PushID(static_cast<int>(object.EntityId()));
	const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
	if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
		selectedObject = object;
		cachedRotationValid = false;
		cachedRotationEntity = object.EntityId();
	}
	if (ImGui::BeginDragDropSource()) {
		entt::entity id = object.EntityId();
		ImGui::SetDragDropPayload("HIERARCHY_ENTITY", &id, sizeof(entt::entity));
		ImGui::TextUnformatted(label.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("HIERARCHY_ENTITY")) {
			entt::entity dropped = *static_cast<const entt::entity*>(payload->Data);
			if (auto childObj = objects.getObject(dropped)) {
				objects.setParent(*childObj, object);
			}
		}
		ImGui::EndDragDropTarget();
	}
	if (open && !children.empty()) {
		for (const Object& child : children) {
			DrawHierarchyNode(child);
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void EditorSystem::DrawInspectorWindow() {
	if (!ImGui::Begin("Inspector")) {
		ImGui::End();
		return;
	}

	if (!selectedObject.has_value()) {
		ImGui::TextUnformatted("No object selected.");
		cachedRotationValid = false;
		cachedRotationEntity = entt::null;
		ImGui::End();
		return;
	}

	ObjectManager& objects = ObjectManager::instance();
	ImGui::Text("Object: %s", objects.getObjectName(*selectedObject).c_str());

	if (Transform* transform = selectedObject->getComponent<Transform>()) {
		glm::quat currentRotation = GetComponentProperty<Transform, glm::quat>(*transform, "Rotation");
		if (!cachedRotationValid || cachedRotationEntity != selectedObject->EntityId() ||
			!glm::all(glm::epsilonEqual(currentRotation, cachedRotation, 0.0001f))) {
			cachedRotation = currentRotation;
			cachedEulerDegrees = QuaternionToEulerDegrees(currentRotation);
			cachedRotationValid = true;
			cachedRotationEntity = selectedObject->EntityId();
		}
		if (ImGui::CollapsingHeader("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
			DrawTransformInspector(*transform);
		}
	}
	else {
		cachedRotationValid = false;
		cachedRotationEntity = entt::null;
	}

	if (CameraComponent* camera = selectedObject->getComponent<CameraComponent>()) {
		if (ImGui::CollapsingHeader("Camera")) {
			DrawCameraInspector(*camera);
		}
	}

	if (MeshRenderComponent* mesh = selectedObject->getComponent<MeshRenderComponent>()) {
		if (ImGui::CollapsingHeader("Mesh Renderer")) {
			DrawMeshRenderInspector(*mesh);
		}
	}

	ImGui::End();
}

void EditorSystem::DrawTransformInspector(Transform& transform) {
	glm::vec3 position = GetComponentProperty<Transform, glm::vec3>(transform, "Position");
	glm::vec3 scale = GetComponentProperty<Transform, glm::vec3>(transform, "Scale");

	if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
		SetComponentProperty(transform, "Position", position);
	}

	glm::vec3 eulerDeg = cachedEulerDegrees;
	if (ImGui::DragFloat3("Rotation (Euler, deg)", &eulerDeg.x, 0.5f)) {
		cachedEulerDegrees = eulerDeg;
		cachedRotation = EulerDegreesToQuaternion(eulerDeg);
		cachedRotationValid = true;
		SetComponentProperty(transform, "Rotation", cachedRotation);
	}

	if (ImGui::DragFloat3("Scale", &scale.x, 0.1f)) {
		SetComponentProperty(transform, "Scale", scale);
	}
}

void EditorSystem::DrawCameraInspector(CameraComponent& camera) {
	float fov = GetComponentProperty<CameraComponent, float>(camera, "verticalFovDegrees");
	float aspect = GetComponentProperty<CameraComponent, float>(camera, "aspectRatio");
	float nearPlane = GetComponentProperty<CameraComponent, float>(camera, "nearPlane");
	float farPlane = GetComponentProperty<CameraComponent, float>(camera, "farPlane");
	glm::vec3 up = GetComponentProperty<CameraComponent, glm::vec3>(camera, "up");

	if (ImGui::DragFloat("Vertical FOV", &fov, 0.1f, 1.0f, 179.0f)) {
		SetComponentProperty(camera, "verticalFovDegrees", fov);
	}
	if (ImGui::DragFloat("Aspect Ratio", &aspect, 0.01f, 0.1f, 10.0f)) {
		SetComponentProperty(camera, "aspectRatio", aspect);
	}
	if (ImGui::DragFloat("Near Plane", &nearPlane, 0.01f, 0.001f, farPlane - 0.001f)) {
		SetComponentProperty(camera, "nearPlane", nearPlane);
	}
	if (ImGui::DragFloat("Far Plane", &farPlane, 1.0f, nearPlane + 0.01f, 10000.0f)) {
		SetComponentProperty(camera, "farPlane", farPlane);
	}
	if (ImGui::DragFloat3("Up", &up.x, 0.01f)) {
		if (glm::length(up) < 0.0001f) {
			up = glm::vec3(0.0f, 1.0f, 0.0f);
		}
		SetComponentProperty(camera, "up", up);
	}
}

void EditorSystem::DrawMeshRenderInspector(MeshRenderComponent& meshComponent) {
	TerraEngine::UUID meshId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComponent, "meshId");
	TerraEngine::UUID materialId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComponent, "materialId");
	TerraEngine::UUID shaderId = GetComponentProperty<MeshRenderComponent, TerraEngine::UUID>(meshComponent, "shaderCollectionId");

	ImGui::Text("Mesh UUID: %s", ToHexString(meshId).c_str());
	ImGui::Text("Material UUID: %s", ToHexString(materialId).c_str());
	ImGui::Text("Shader Collection UUID: %s", ToHexString(shaderId).c_str());
}

glm::vec3 EditorSystem::QuaternionToEulerDegrees(const glm::quat& q) const {
	glm::vec3 euler = glm::degrees(glm::eulerAngles(glm::normalize(q)));
	return euler;
}

glm::quat EditorSystem::EulerDegreesToQuaternion(const glm::vec3& euler) const {
	glm::vec3 radians = glm::radians(euler);
	return glm::quat(radians);
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
