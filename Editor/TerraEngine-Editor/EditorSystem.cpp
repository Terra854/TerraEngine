#include "EditorSystem.h"
#include <GLFW/glfw3.h>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <cmath>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/epsilon.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>
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
		ImGui::End();
		return;
	}

	ObjectManager& objects = ObjectManager::instance();
	ImGui::Text("Object: %s", objects.getObjectName(*selectedObject).c_str());

	EnsureComponentOrder(*selectedObject);
	auto& order = inspectorComponentOrder[selectedObject->EntityId()];
	for (size_t idx = 0; idx < order.size(); ++idx) {
		const std::string& componentName = order[idx];
		auto it = ComponentAccessRegistry().find(componentName);
		if (it == ComponentAccessRegistry().end()) continue;
		const ComponentAccessFunctions& access = it->second;
		if (!access.hasComponent || !access.hasComponent(*selectedObject)) continue;

		ImGui::PushID(static_cast<int>(idx));
		DrawComponentInspector(componentName, access, static_cast<int>(idx), [&](int from, int to) {
			ReorderComponent(order, from, to);
		});
		ImGui::PopID();
	}

	ImGui::End();
}

void EditorSystem::DrawComponentInspector(const std::string& componentName, const ComponentAccessFunctions& access, int index, std::function<void(int, int)> onReorder) {
	rttr::type componentType = rttr::type::get_by_name(componentName);
	if (!componentType.is_valid()) {
		return;
	}

	const bool isTransform = (componentName == std::string(Transform::kComponentName));
	
	if (!isTransform) {
		rttr::variant activeVar = access.getter(*selectedObject, "active");
		if (activeVar.is_valid()) {
			bool active = activeVar.get_value<bool>();
			ImGui::PushID((componentName + "_active").c_str());
			if (ImGui::Checkbox("##active", &active)) {
				access.setter(*selectedObject, "active", active);
			}
			ImGui::PopID();
			ImGui::SameLine();
		}
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen;
	bool open = ImGui::CollapsingHeader(componentName.c_str(), flags);

	if (!isTransform) {
		if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
			ImGui::SetDragDropPayload("COMPONENT_ORDER", &index, sizeof(int));
			ImGui::Text("Move %s", componentName.c_str());
			ImGui::EndDragDropSource();
		}
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("COMPONENT_ORDER")) {
				int from = *static_cast<const int*>(payload->Data);
				if (onReorder) {
					onReorder(from, index);
				}
			}
			ImGui::EndDragDropTarget();
		}
	}

	if (open) {
		for (const auto& prop : componentType.get_properties()) {
			std::string propName = prop.get_name().to_string();
			if (propName == "active") {
				continue;
			}
			rttr::variant value = access.getter(*selectedObject, propName);
			if (!value.is_valid()) {
				continue;
			}
			ImGui::PushID(propName.c_str());
			if (DrawPropertyEditor(propName, prop.get_type(), value, selectedObject->EntityId())) {
				access.setter(*selectedObject, propName, value);
			}
			ImGui::PopID();
		}
	}
}

bool EditorSystem::DrawPropertyEditor(const std::string& label, rttr::type type, rttr::variant& value, entt::entity entityId) {
	if (type == rttr::type::get<float>()) {
		float v = value.get_value<float>();
		if (ImGui::DragFloat(label.c_str(), &v, 0.1f)) {
			value = v;
			return true;
		}
		return false;
	}

	if (type == rttr::type::get<int>()) {
		int v = value.get_value<int>();
		if (ImGui::DragInt(label.c_str(), &v, 1)) {
			value = v;
			return true;
		}
		return false;
	}

	if (type == rttr::type::get<bool>()) {
		bool v = value.get_value<bool>();
		if (ImGui::Checkbox(label.c_str(), &v)) {
			value = v;
			return true;
		}
		return false;
	}

	if (type == rttr::type::get<glm::vec3>()) {
		glm::vec3 v = value.get_value<glm::vec3>();
		if (ImGui::DragFloat3(label.c_str(), &v.x, 0.1f)) {
			value = v;
			return true;
		}
		return false;
	}

	if (type == rttr::type::get<glm::quat>()) {
		glm::quat q = glm::normalize(value.get_value<glm::quat>());
		auto cacheIt = rotationEditCache.find(entityId);
		auto lastQuatIt = rotationLastQuat.find(entityId);

		bool rotationChanged = true;
		if (lastQuatIt != rotationLastQuat.end()) {
			float alignment = std::abs(glm::dot(lastQuatIt->second, q));
			rotationChanged = alignment < 0.9999f;
		}

		// Seed the cache from the current component value when none exists, or when the rotation changed externally.
		if (cacheIt == rotationEditCache.end() || rotationChanged) {
			glm::vec3 currentDeg = glm::degrees(glm::eulerAngles(q));
			rotationEditCache[entityId] = currentDeg;
			cacheIt = rotationEditCache.find(entityId);
			rotationLastQuat[entityId] = q;
		}

		glm::vec3 eulerDeg = cacheIt->second;
		// Expose rotation as yaw(Y) / pitch(X) / roll(Z) with a persistent cache so authors can edit beyond +/-180
		// without the UI wrapping values each frame.
		if (ImGui::DragFloat3((label + " (Euler deg)").c_str(), &eulerDeg.x, 0.5f)) {
			rotationEditCache[entityId] = eulerDeg;
			glm::quat newQuat = EulerDegreesToQuaternion(eulerDeg);
			rotationLastQuat[entityId] = newQuat;
			value = newQuat;
			return true;
		}
		return false;
	}

	if (type == rttr::type::get<TerraEngine::UUID>()) {
		ImGui::Text("%s: %s", label.c_str(), ToHexString(value.get_value<TerraEngine::UUID>()).c_str());
		return false;
	}

	ImGui::Text("%s (uneditable type)", label.c_str());
	return false;
}

glm::quat EditorSystem::EulerDegreesToQuaternion(const glm::vec3& euler) const {
	glm::vec3 radians = glm::radians(euler);
	// Build the quaternion from yaw(Y), pitch(X), roll(Z) to match the editor field ordering.
	glm::mat4 rotationMatrix = glm::yawPitchRoll(radians.y, radians.x, radians.z);
	return glm::quat_cast(rotationMatrix);
}

void EditorSystem::EnsureComponentOrder(Object& object) {
	auto& registry = ComponentAccessRegistry();
	std::vector<std::string> available;
	available.reserve(registry.size());
	for (const auto& [name, access] : registry) {
		if (access.hasComponent && access.hasComponent(object)) {
			available.push_back(name);
		}
	}

	auto& order = inspectorComponentOrder[object.EntityId()];
	if (order.empty()) {
		// Default order: Transform first (if present), then alphabetical.
		std::sort(available.begin(), available.end());
		for (const std::string& name : available) {
			if (name == std::string(Transform::kComponentName)) {
				order.insert(order.begin(), name);
			} else {
				order.push_back(name);
			}
		}
		return;
	}

	// Remove components that no longer exist.
	order.erase(std::remove_if(order.begin(), order.end(), [&](const std::string& name) {
		return std::find(available.begin(), available.end(), name) == available.end();
	}), order.end());

	// Append new components at the end (except Transform, which is forced to the front).
	for (const std::string& name : available) {
		if (std::find(order.begin(), order.end(), name) == order.end()) {
			order.push_back(name);
		}
	}

	// Keep Transform at the top when present.
	auto transformIt = std::find(order.begin(), order.end(), std::string(Transform::kComponentName));
	if (transformIt != order.end() && transformIt != order.begin()) {
		std::string transformName = *transformIt;
		order.erase(transformIt);
		order.insert(order.begin(), transformName);
	}
}

void EditorSystem::ReorderComponent(std::vector<std::string>& order, int from, int to) {
	if (from < 0 || to < 0 || from >= static_cast<int>(order.size()) || to >= static_cast<int>(order.size())) {
		return;
	}
	// Never move Transform away from the top.
	if (order[from] == std::string(Transform::kComponentName)) {
		return;
	}
	if (order[to] == std::string(Transform::kComponentName)) {
		to = 1;
	}
	if (from == to) return;
	std::string item = order[from];
	order.erase(order.begin() + from);
	order.insert(order.begin() + to, item);
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
