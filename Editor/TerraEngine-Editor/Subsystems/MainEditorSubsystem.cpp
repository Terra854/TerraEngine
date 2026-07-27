// MainEditorSubsystem.cpp — Core editor workspace implementation
//
// Implements the primary editor workspace logic, including the main menu bar, toolbars, and global hotkey handling. It manages the lifecycle of all native editor windows and provides the necessary hooks for scene state capture and restoration during play mode.

#include "MainEditorSubsystem.h"
#include "PhysicsEditorAuthoringState.h"
#include "Editor/AssetCompilers/ModelPackageImporter.h"
#include "ProjectManager.h"
#include "AssetDatabase.h"
#include "EditorSystem.h"
#include "../Core/CheckedImGuiVulkanBridge.h"
#include "ScriptCompilationSubsystem.h"
#include "UndoRedoSystem.h"
#include "../Profiling/EditorProfilerService.h"
#include "Engine/Profiling/ProfilerScope.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/MainEngine.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Scheduling/EcsQueryTaskGraph.h"
#include "Engine/Components/CameraComponent.h"
#include "Engine/Components/MeshRenderComponent.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Renderer/RenderSystem.h"
#include "Engine/Renderer/RHI/Vulkan/VulkanContext.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneGraphRuntime.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Resource/ResourceManager.h"
#include "Engine/UI/UISystem.h"
#include "Engine/Scripting/ScriptGlue.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Components/Physics/ColliderSetComponent.h"
#include "Engine/Components/Physics/ConstraintSetComponent.h"
#include "Engine/Components/Physics/RigidBodyComponent.h"
#include "Engine/Physics/Assets/CookedColliderAsset.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Runtime/ExportStartupCoordinator.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Utilities/PlatformUtils.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <backends/imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <nlohmann/json.hpp>

// Include Window Headers
#include "../Windows/HierarchyWindow.h"
#include "../Windows/InspectorWindow.h"
#include "../Windows/ProjectBrowserWindow.h"
#include "../Windows/GameViewWindow.h"
#include "../Windows/SceneViewWindow.h"
#include "../Windows/BuildProfilesWindow.h"
#include "../Windows/ProfilerWindow.h"
#include "../Windows/PhysicsDiagnosticsWindow.h"
#include "../Windows/SchedulerDiagnosticsWindow.h"
#include "../Windows/ResourceResidencyWindow.h"
#include "../Windows/UIBuilderWindow.h"
#include "../Windows/ProjectSettingsWindow.h"
#include "../Windows/PreferencesWindow.h"
#include "../Windows/SkeletonEditorWindow.h"
#include "../Windows/AnimationClipEditorWindow.h"
#include "../Windows/AnimationControllerWindow.h"
#include "../Windows/RetargetEditorWindow.h"
#include "../Windows/MotionDatabaseWindow.h"
#include "../Windows/RagdollEditorWindow.h"
#include "../Windows/AnimationDiagnosticsWindow.h"

namespace
{
	// The graph transaction is shared by Asset Browser drag/drop and MCP-backed
	// prefab instantiation. Preparation completes before the first live object is
	// published, and every newly assigned UUID is retained for rollback.
	struct ProtectedPrefabInstantiationResult
	{
		bool ok = false;
		std::string errorCategory;
		std::string message;
		std::vector<entt::entity> roots;
		std::size_t objectCount = 0U;
	};

	ProtectedPrefabInstantiationResult InstantiateRegisteredPrefabGraph(
		const std::filesystem::path& prefabPath,
		const TerraEngine::UUID& prefabGuid,
		nlohmann::json prefabJson,
		std::string_view requestedName,
		std::optional<entt::entity> parent);
}

std::string PrefabAuthoringService::ResolvePrefabId(std::string_view identity) const {
	const std::string value(identity);
	if (prefabsById.contains(value)) {
		return value;
	}
	const std::string normalized = std::filesystem::path(value).lexically_normal().generic_string();
	const auto found = prefabIdByPath.find(normalized);
	return found == prefabIdByPath.end() ? std::string{} : found->second;
}

PrefabAuthoringService::Result PrefabAuthoringService::Create(
	entt::entity sourceEntity,
	const std::filesystem::path& projectRelativePath,
	const std::filesystem::path& absolutePath) {
	if (sourceEntity == entt::null || absolutePath.extension() != ".teprefab"
		|| std::filesystem::exists(absolutePath)) {
		return {false, "validation", "Prefab source or destination is invalid."};
	}
	const Object object = ObjectManager::instance().makeObjectHandle(sourceEntity);
	const std::string sourceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(object));
	const std::string sourceName = ObjectManager::instance().getObjectName(object);
	const std::string prefabId = "prefab-" + std::to_string(nextPrefabId++);
	std::error_code directoryError;
	std::filesystem::create_directories(absolutePath.parent_path(), directoryError);
	if (directoryError) {
		return {false, "internal", "Could not create the prefab directory: " + directoryError.message()};
	}
	std::ofstream output(absolutePath, std::ios::binary | std::ios::trunc);
	output << nlohmann::json{{"prefabId", prefabId}, {"sourceObjectId", sourceId}, {"name", sourceName}}.dump(2);
	if (!output.good()) {
		return {false, "internal", "Could not publish the prefab asset."};
	}
	const std::string relative = projectRelativePath.lexically_normal().generic_string();
	prefabsById[prefabId] = {prefabId, relative, sourceId, sourceName, 1u, {}, TerraEngine::EMPTY_UUID, {}, false};
	prefabIdByPath[relative] = prefabId;
	return {true, {}, "Created prefab.", {{"prefabId", prefabId}, {"sourceObjectId", sourceId}, {"path", relative}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::RegisterExistingAsset(
	const std::filesystem::path& projectRelativePath,
	const std::filesystem::path& absolutePath,
	const TerraEngine::UUID& assetGuid,
	std::uint64_t publishedGeneration) {
	const std::filesystem::path relativePath = projectRelativePath.lexically_normal();
	const auto firstSegment = relativePath.begin();
	if (assetGuid == TerraEngine::EMPTY_UUID
		|| relativePath.empty()
		|| relativePath.is_absolute()
		|| relativePath.extension() != ".teprefab"
		|| firstSegment == relativePath.end()
		|| firstSegment->generic_string() != "Assets"
		|| std::find(relativePath.begin(), relativePath.end(), std::filesystem::path("..")) != relativePath.end()
		|| !absolutePath.is_absolute()
		|| absolutePath.extension() != ".teprefab") {
		return {false, "validation", "Registered prefab identity or path is invalid."};
	}

	std::ifstream input(absolutePath, std::ios::binary);
	if (!input) {
		return {false, "missing_target", "Registered prefab file was not found."};
	}

	nlohmann::json document;
	try {
		input >> document;
	} catch (const nlohmann::json::exception&) {
		return {false, "validation", "Registered prefab document is malformed."};
	}
	if (!document.is_object()) {
		return {false, "validation", "Registered prefab document must be a JSON object."};
	}
	const bool hasSerializedGraph = document.contains("objects");
	if (hasSerializedGraph && (!document["objects"].is_array() || document["objects"].empty())) {
		return {false, "validation", "Registered prefab object graph must contain at least one root."};
	}

	// Prefer the authored document name, then its first root, and finally the
	// filename. This supports both generated model prefabs and older lightweight
	// prefabs created by the automation authoring service.
	std::string sourceName;
	std::string sourceObjectId;
	if (const auto name = document.find("name"); name != document.end()) {
		if (!name->is_string()) {
			return {false, "validation", "Registered prefab name must be a string."};
		}
		sourceName = name->get<std::string>();
	}
	if (const auto sourceId = document.find("sourceObjectId"); sourceId != document.end()) {
		if (!sourceId->is_string()) {
			return {false, "validation", "Registered prefab sourceObjectId must be a string."};
		}
		sourceObjectId = sourceId->get<std::string>();
	}
	if (document.contains("objects") && document["objects"].is_array() && !document["objects"].empty()
		&& document["objects"].front().is_object()) {
		const nlohmann::json& firstRoot = document["objects"].front();
		if (sourceName.empty()) {
			if (const auto name = firstRoot.find("name"); name != firstRoot.end()) {
				if (!name->is_string()) {
					return {false, "validation", "Registered prefab root name must be a string."};
				}
				sourceName = name->get<std::string>();
			}
		}
		if (sourceObjectId.empty()) {
			if (const auto sourceId = firstRoot.find("uuid"); sourceId != firstRoot.end()) {
				if (!sourceId->is_string()) {
					return {false, "validation", "Registered prefab root UUID must be a string."};
				}
				sourceObjectId = sourceId->get<std::string>();
			}
		}
	}
	if (sourceName.empty()) {
		sourceName = absolutePath.stem().string();
	}

	const std::string prefabId = TerraEngine::ToHexString(assetGuid);
	const std::string relative = relativePath.generic_string();
	const std::uint64_t generation = std::max<std::uint64_t>(1u, publishedGeneration);
	if (const auto existing = prefabsById.find(prefabId); existing != prefabsById.end()) {
		// A reimport may retain the GUID while moving or republishing the asset.
		// Remove the former alias before publishing the current indexed path.
		prefabIdByPath.erase(existing->second.relativePath);
	}
	prefabsById[prefabId] = {
		prefabId,
		relative,
		sourceObjectId,
		sourceName,
		generation,
		std::filesystem::absolute(absolutePath).lexically_normal(),
		assetGuid,
		std::move(document),
		hasSerializedGraph
	};
	prefabIdByPath[relative] = prefabId;
	return {true, {}, "Registered existing prefab asset.", {
		{"prefabId", prefabId},
		{"sourceObjectId", sourceObjectId},
		{"path", relative},
		{"prefabGeneration", generation}
	}};
}

PrefabAuthoringService::Result PrefabAuthoringService::Instantiate(
	std::string_view prefabIdentity,
	std::string_view requestedName,
	std::optional<entt::entity> parent) {
	const std::string prefabId = ResolvePrefabId(prefabIdentity);
	if (prefabId.empty()) {
		return {false, "missing_target", "Prefab was not found."};
	}
	if (!activeContextPrefabId.empty() && activeContextPrefabId == prefabId) {
		return {false, "conflict", "A prefab cannot contain itself.", {{"rolledBack", true}}};
	}
	const PrefabState& prefab = prefabsById.at(prefabId);
	if (prefab.hasSerializedGraph) {
		const ProtectedPrefabInstantiationResult graph = InstantiateRegisteredPrefabGraph(
			prefab.absolutePath, prefab.assetGuid, prefab.serializedDocument, requestedName, parent);
		if (!graph.ok || graph.roots.empty()) {
			return {
				false,
				graph.errorCategory.empty() ? "internal" : graph.errorCategory,
				graph.message.empty() ? "Could not instantiate the registered prefab graph." : graph.message,
				{{"rolledBack", true}}
			};
		}

		const Object instance = ObjectManager::instance().makeObjectHandle(graph.roots.front());
		const std::string instanceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(instance));
		instances[instanceId] = {prefabId, prefab.sourceName};
		return {true, {}, "Instantiated prefab graph.", {
			{"prefabId", prefabId},
			{"instanceId", instanceId},
			{"path", prefab.relativePath},
			{"prefabGeneration", prefab.generation},
			{"rootCount", graph.roots.size()},
			{"objectCount", graph.objectCount}
		}};
	}

	// A prefab created earlier in this automation session deliberately keeps the
	// legacy lightweight representation until AssetDatabase indexes its file.
	Object instance = ObjectManager::instance().createObject(requestedName.empty() ? prefab.sourceName : std::string(requestedName));
	const std::string instanceId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(instance));
	instances[instanceId] = {prefabId, prefab.sourceName};
	UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(instance.EntityId()));
	return {true, {}, "Instantiated prefab.", {
		{"prefabId", prefabId},
		{"instanceId", instanceId},
		{"path", prefab.relativePath},
		{"prefabGeneration", prefab.generation}
	}};
}

PrefabAuthoringService::Result PrefabAuthoringService::InspectOverrides(std::string_view instanceIdentity) const {
	const std::string instanceId(instanceIdentity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) {
		return {false, "missing_target", "Prefab instance was not found."};
	}
	const PrefabState& prefab = prefabsById.at(state->second.prefabId);
	const std::string currentName = ObjectManager::instance().getObjectName(*instance);
	nlohmann::json overrides = nlohmann::json::array();
	if (currentName != prefab.sourceName) {
		overrides.push_back({{"property", "name"}, {"value", currentName}, {"sourceValue", prefab.sourceName}});
	}
	return {true, {}, "Inspected prefab overrides.", {{"overrides", overrides}, {"prefabGeneration", prefab.generation}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::ApplyOverrides(
	std::string_view instanceIdentity,
	std::optional<std::uint64_t> expectedGeneration) {
	const std::string instanceId(instanceIdentity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) {
		return {false, "missing_target", "Prefab instance was not found."};
	}
	PrefabState& prefab = prefabsById.at(state->second.prefabId);
	if (expectedGeneration && *expectedGeneration != prefab.generation) {
		return {false, "stale_target", "Prefab source generation changed."};
	}
	prefab.sourceName = ObjectManager::instance().getObjectName(*instance);
	++prefab.generation;
	return {true, {}, "Applied prefab overrides.", {
		{"historyId", "history-prefab-" + std::to_string(prefab.generation)},
		{"prefabGeneration", prefab.generation}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::RevertOverrides(std::string_view instanceIdentity) {
	const std::string instanceId(instanceIdentity);
	const auto state = instances.find(instanceId);
	const auto uuid = TerraEngine::ParseUUID(instanceId);
	const auto instance = uuid ? ObjectManager::instance().getObject(*uuid) : std::nullopt;
	if (state == instances.end() || !instance) {
		return {false, "missing_target", "Prefab instance was not found."};
	}
	ObjectManager::instance().setObjectName(*instance, prefabsById.at(state->second.prefabId).sourceName);
	return {true, {}, "Reverted prefab overrides.", {{"remainingOverrideCount", 0u}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::OpenEditContext(std::string_view prefabIdentity) {
	const std::string prefabId = ResolvePrefabId(prefabIdentity);
	if (prefabId.empty()) {
		return {false, "missing_target", "Prefab was not found."};
	}
	if (!activeContextId.empty()) {
		return {false, "conflict", "Another prefab editing context is already active."};
	}
	activeContextId = "prefab-context-" + std::to_string(nextPrefabId++);
	activeContextPrefabId = prefabId;
	return {true, {}, "Opened prefab editing context.", {{"contextId", activeContextId}, {"prefabId", prefabId}}};
}

PrefabAuthoringService::Result PrefabAuthoringService::CloseEditContext(std::string_view contextIdentity) {
	if (contextIdentity != activeContextId) {
		return {false, "stale_target", "Prefab editing context identity is stale."};
	}
	activeContextId.clear();
	activeContextPrefabId.clear();
	return {true, {}, "Closed prefab editing context."};
}

namespace
{
	MainEditorSubsystem* g_ActiveMainEditorSubsystem = nullptr;

	struct WindowMenuNode
	{
		std::string label;
		std::shared_ptr<EditorWindow> window;
		std::vector<WindowMenuNode> children;
	};

	std::vector<std::string> SplitWindowMenuPath(std::string_view path)
	{
		std::vector<std::string> segments;
		std::size_t segmentStart = 0;
		while (segmentStart < path.size())
		{
			const std::size_t separator = path.find('/', segmentStart);
			const std::size_t segmentEnd = separator == std::string_view::npos ? path.size() : separator;
			if (segmentEnd > segmentStart)
			{
				segments.emplace_back(path.substr(segmentStart, segmentEnd - segmentStart));
			}
			if (separator == std::string_view::npos)
			{
				break;
			}
			segmentStart = separator + 1;
		}
		return segments;
	}

	void AddWindowMenuPath(std::vector<WindowMenuNode>& roots, const std::shared_ptr<EditorWindow>& window)
	{
		if (!window || window->GetWindowMenuCategoryPath().empty())
		{
			// Menu participation is opt-in. Merely registering a drawable window
			// never makes it part of the global Windows menu.
			return;
		}

		const std::vector<std::string> segments = SplitWindowMenuPath(window->GetWindowMenuCategoryPath());
		std::vector<WindowMenuNode>* siblings = &roots;
		WindowMenuNode* currentNode = nullptr;
		for (const std::string& segment : segments)
		{
			auto existing = std::find_if(siblings->begin(), siblings->end(), [&segment](const WindowMenuNode& node) {
				return node.label == segment;
			});
			if (existing == siblings->end())
			{
				siblings->push_back(WindowMenuNode{segment});
				currentNode = &siblings->back();
			}
			else
			{
				currentNode = &*existing;
			}
			siblings = &currentNode->children;
		}

		if (currentNode != nullptr)
		{
			// The category path never repeats the window label. Its registered title
			// becomes the leaf entry beneath the declared category hierarchy.
			currentNode->children.push_back(WindowMenuNode{window->GetTitle(), window});
		}
	}

	void DrawWindowMenuNodes(const std::vector<WindowMenuNode>& nodes)
	{
		for (const WindowMenuNode& node : nodes)
		{
			if (!node.children.empty())
			{
				if (ImGui::BeginMenu(node.label.c_str()))
				{
					DrawWindowMenuNodes(node.children);
					ImGui::EndMenu();
				}
				continue;
			}

			// Window visibility is already obvious from the dockspace itself, so the
			// menu entry remains a plain open/focus command without a checkmark.
			if (node.window && ImGui::MenuItem(node.label.c_str()))
			{
				// Preserve the previous native menu behavior: selecting an entry opens
				// or focuses its window rather than using the menu as a close action.
				node.window->SetVisible(true);
			}
		}
	}

	std::vector<WindowMenuNode> BuildWindowMenuTree(const std::vector<std::shared_ptr<EditorWindow>>& windows)
	{
		std::vector<WindowMenuNode> roots;
		for (const std::shared_ptr<EditorWindow>& window : windows)
		{
			AddWindowMenuPath(roots, window);
		}
		return roots;
	}

	// ResolveEditorRuntimeRoot returns the directory that contains the running
	// editor executable. Renderer-owned shaders and managed binaries are staged
	// beside that executable, so this must not depend on the process working
	// directory; --project launches often use the repository root as cwd.
	std::filesystem::path ResolveEditorRuntimeRoot()
	{
		const std::filesystem::path executableDirectory = TerraEngine::PlatformUtils::GetExecutableDirectory();
		if (!executableDirectory.empty())
		{
			return executableDirectory;
		}

		TERRAENGINE_LOG(LogLevel::Warning, "Could not resolve editor executable directory; falling back to current working directory for engine root.");
		return std::filesystem::current_path();
	}

	// StopPlayModeFromApplicationQuit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void StopPlayModeFromApplicationQuit()
	{
		if (g_ActiveMainEditorSubsystem != nullptr)
		{
			g_ActiveMainEditorSubsystem->StopPlayMode();
		}
	}

	enum class PlayModeToolbarIcon
	{
		Play,
		Stop,
		Pause,
		Next
	};

	void DrawPlayModeToolbarIconShape(ImDrawList* drawList, PlayModeToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 iconColor)
	{
		const float width = max.x - min.x;
		const float height = max.y - min.y;
		const float unit = std::min(width, height);
		const ImVec2 center(min.x + width * 0.5f, min.y + height * 0.5f);
		const float radius = unit * 0.28f;
		const float thickness = std::max(1.2f, unit * 0.08f);

		switch (icon)
		{
		case PlayModeToolbarIcon::Play:
			drawList->AddTriangleFilled(
				ImVec2(center.x - radius * 0.55f, center.y - radius),
				ImVec2(center.x - radius * 0.55f, center.y + radius),
				ImVec2(center.x + radius * 0.9f, center.y),
				iconColor);
			break;
		case PlayModeToolbarIcon::Stop:
			drawList->AddRectFilled(
				ImVec2(center.x - radius * 0.72f, center.y - radius * 0.72f),
				ImVec2(center.x + radius * 0.72f, center.y + radius * 0.72f),
				iconColor,
				2.0f);
			break;
		case PlayModeToolbarIcon::Pause:
			drawList->AddRectFilled(
				ImVec2(center.x - radius * 0.72f, center.y - radius),
				ImVec2(center.x - radius * 0.18f, center.y + radius),
				iconColor,
				1.5f);
			drawList->AddRectFilled(
				ImVec2(center.x + radius * 0.18f, center.y - radius),
				ImVec2(center.x + radius * 0.72f, center.y + radius),
				iconColor,
				1.5f);
			break;
		case PlayModeToolbarIcon::Next:
			drawList->AddTriangleFilled(
				ImVec2(center.x - radius * 0.9f, center.y - radius),
				ImVec2(center.x - radius * 0.9f, center.y + radius),
				ImVec2(center.x + radius * 0.28f, center.y),
				iconColor);
			drawList->AddLine(
				ImVec2(center.x + radius * 0.72f, center.y - radius),
				ImVec2(center.x + radius * 0.72f, center.y + radius),
				iconColor,
				thickness);
			break;
		}
	}

	void DrawPlayModeToolbarTooltip(const char* tooltip)
	{
		if (tooltip == nullptr)
		{
			return;
		}

		// The main toolbar deliberately removes most window padding so the
		// transport icons can sit tightly together. Tooltips are their own
		// windows, so restore a readable frame padding before drawing the text.
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(tooltip);
		ImGui::EndTooltip();
		ImGui::PopStyleVar(3);
	}

	bool DrawPlayModeToolbarIconButton(const char* id, PlayModeToolbarIcon icon, bool active, const char* tooltip, float size)
	{
		ImGui::PushID(id);
		const bool pressed = ImGui::InvisibleButton("##play_mode_toolbar_icon", ImVec2(size, size));
		const bool hovered = ImGui::IsItemHovered();
		const bool held = ImGui::IsItemActive();
		const ImVec2 min = ImGui::GetItemRectMin();
		const ImVec2 max = ImGui::GetItemRectMax();

		const ImU32 background = active
			? IM_COL32(84, 128, 164, 210)
			: (held ? IM_COL32(76, 80, 84, 210) : (hovered ? IM_COL32(70, 74, 78, 190) : IM_COL32(0, 0, 0, 0)));
		const ImU32 border = active ? IM_COL32(138, 188, 226, 230) : (hovered ? IM_COL32(100, 106, 112, 190) : IM_COL32(0, 0, 0, 0));
		const ImU32 iconColor = active ? IM_COL32(245, 251, 255, 255) : IM_COL32(230, 234, 238, 255);

		// Keep the global Play Mode controls icon-only like a standard editor
		// transport bar; the tooltip below carries the readable command name.
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		drawList->AddRectFilled(min, max, background, 3.0f);
		if (active || hovered)
		{
			drawList->AddRect(min, max, border, 3.0f, 0, active ? 1.4f : 1.0f);
		}
		DrawPlayModeToolbarIconShape(drawList, icon, min, max, iconColor);

		if (hovered)
		{
			DrawPlayModeToolbarTooltip(tooltip);
		}
		ImGui::PopID();
		return pressed;
	}

	std::optional<AssetType> AssetTypeFromImporter(std::string_view importerName)
	{
		if (importerName == "MeshImporter" || TerraEngine::AssetPipeline::IsModelPackageImporterName(importerName))
		{
			return AssetType::Mesh;
		}
		if (importerName == "TextureImporter")
		{
			return AssetType::Texture;
		}
		if (importerName == "MaterialImporter")
		{
			return AssetType::Material;
		}
		if (importerName == "ShaderImporter")
		{
			return AssetType::ShaderCollection;
		}
		return std::nullopt;
	}

	std::optional<std::pair<TerraEngine::UUID, AssetType>> ResolveEditorAssetPath(const std::filesystem::path& assetPath)
	{
		const std::filesystem::path normalizedPath = std::filesystem::absolute(assetPath).lexically_normal();
		for (const auto& [sourcePath, metadata] : AssetDatabase::Instance().GetAssets())
		{
			if (std::filesystem::absolute(sourcePath).lexically_normal() != normalizedPath)
			{
				continue;
			}

			if (auto assetType = AssetTypeFromImporter(metadata.importerName))
			{
				return std::make_pair(metadata.guid, *assetType);
			}
			return std::nullopt;
		}
		return std::nullopt;
	}

	void PublishMeshRenderAssetNeeds(Object object, TerraEngine::Resource::StreamingNeedType needType, std::string_view reason)
	{
		MeshRenderComponent* meshRenderer = ObjectManager::instance().GetComponent<MeshRenderComponent>(object);
		if (meshRenderer == nullptr)
		{
			return;
		}

		ResourceManager& resources = ResourceManager::Instance();
		resources.MarkStreamingNeed(meshRenderer->meshId, AssetType::Mesh, needType, 0u, reason, 80);
		resources.MarkStreamingNeed(meshRenderer->materialId, AssetType::Material, needType, 0u, reason, 80);
	}

	void CollectPrefabAssetNeedsFromObjectJson(const nlohmann::json& objectJson, std::vector<std::pair<TerraEngine::UUID, AssetType>>& assets)
	{
		if (const auto componentsIt = objectJson.find("components"); componentsIt != objectJson.end() && componentsIt->is_object())
		{
			if (const auto meshRendererIt = componentsIt->find("MeshRenderComponent"); meshRendererIt != componentsIt->end() && meshRendererIt->is_object())
			{
				if (const auto meshIdIt = meshRendererIt->find("meshId"); meshIdIt != meshRendererIt->end() && meshIdIt->is_string())
				{
					if (const auto parsed = TerraEngine::ParseUUID(meshIdIt->get<std::string>()))
					{
						assets.emplace_back(*parsed, AssetType::Mesh);
					}
				}
				if (const auto materialIdIt = meshRendererIt->find("materialId"); materialIdIt != meshRendererIt->end() && materialIdIt->is_string())
				{
					if (const auto parsed = TerraEngine::ParseUUID(materialIdIt->get<std::string>()))
					{
						assets.emplace_back(*parsed, AssetType::Material);
					}
				}
			}
		}

		if (const auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
		{
			for (const nlohmann::json& childJson : *childrenIt)
			{
				CollectPrefabAssetNeedsFromObjectJson(childJson, assets);
			}
		}
	}

	void PublishPrefabAssetPreviewNeeds(const std::filesystem::path& prefabPath)
	{
		std::ifstream prefabInput(prefabPath, std::ios::binary);
		if (!prefabInput)
		{
			return;
		}

		nlohmann::json prefabJson;
		try
		{
			prefabInput >> prefabJson;
		}
		catch (const nlohmann::json::parse_error&)
		{
			return;
		}

		std::vector<std::pair<TerraEngine::UUID, AssetType>> assets;
		if (const auto objectsIt = prefabJson.find("objects"); objectsIt != prefabJson.end() && objectsIt->is_array())
		{
			for (const nlohmann::json& objectJson : *objectsIt)
			{
				CollectPrefabAssetNeedsFromObjectJson(objectJson, assets);
			}
		}

		ResourceManager& resources = ResourceManager::Instance();
		for (const auto& [assetId, assetType] : assets)
		{
			resources.MarkStreamingNeed(assetId, assetType, TerraEngine::Resource::StreamingNeedType::AssetPreview, 0u, "selected prefab asset preview", 70);
		}
	}

	std::optional<TerraEngine::UUID> ReadSerializedObjectUuid(const nlohmann::json& objectJson)
	{
		if (const auto uuidIt = objectJson.find("uuid"); uuidIt != objectJson.end() && uuidIt->is_string())
		{
			return TerraEngine::ParseUUID(uuidIt->get<std::string>());
		}
		return std::nullopt;
	}

	bool CollectPrefabInstanceUuidRemap(
		nlohmann::json& objectJson,
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& uuidRemap,
		std::vector<TerraEngine::UUID>& generatedObjectIds,
		std::string& reason)
	{
		const auto uuidIt = objectJson.find("uuid");
		if (uuidIt != objectJson.end())
		{
			if (!uuidIt->is_string())
			{
				reason = "prefab object UUID is not a string";
				return false;
			}
			const std::optional<TerraEngine::UUID> sourceUuid = TerraEngine::ParseUUID(uuidIt->get<std::string>());
			if (!sourceUuid)
			{
				reason = "prefab object UUID is malformed";
				return false;
			}

			TerraEngine::UUID instanceUuid = TerraEngine::GenerateUUID();
			while (ObjectManager::instance().getObject(instanceUuid).has_value()
				|| std::find(generatedObjectIds.begin(), generatedObjectIds.end(), instanceUuid) != generatedObjectIds.end())
			{
				instanceUuid = TerraEngine::GenerateUUID();
			}
			if (!uuidRemap.emplace(*sourceUuid, instanceUuid).second)
			{
				reason = "prefab graph contains duplicate object UUIDs";
				return false;
			}
			generatedObjectIds.push_back(instanceUuid);
			objectJson["uuid"] = TerraEngine::ToHexString(instanceUuid);
		}
		else
		{
			// Older prefab documents may omit UUIDs. Assign them during immutable
			// preparation so rollback can still identify every created object.
			TerraEngine::UUID instanceUuid = TerraEngine::GenerateUUID();
			while (ObjectManager::instance().getObject(instanceUuid).has_value()
				|| std::find(generatedObjectIds.begin(), generatedObjectIds.end(), instanceUuid) != generatedObjectIds.end())
			{
				instanceUuid = TerraEngine::GenerateUUID();
			}
			generatedObjectIds.push_back(instanceUuid);
			objectJson["uuid"] = TerraEngine::ToHexString(instanceUuid);
		}

		if (auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
		{
			for (nlohmann::json& childJson : *childrenIt)
			{
				if (!CollectPrefabInstanceUuidRemap(childJson, uuidRemap, generatedObjectIds, reason))
				{
					return false;
				}
			}
		}
		return true;
	}

	void RewritePrefabInstanceUuidReferences(nlohmann::json& value, const std::unordered_map<TerraEngine::UUID, TerraEngine::UUID>& uuidRemap)
	{
		if (value.is_string())
		{
			if (const std::optional<TerraEngine::UUID> parsedUuid = TerraEngine::ParseUUID(value.get<std::string>()))
			{
				if (const auto replacement = uuidRemap.find(*parsedUuid); replacement != uuidRemap.end())
				{
					// Object references inside the prefab should target the new
					// instance IDs, while asset GUIDs stay unchanged because they
					// are not present in this source-object remap.
					value = TerraEngine::ToHexString(replacement->second);
				}
			}
			return;
		}

		if (value.is_array())
		{
			for (nlohmann::json& childValue : value)
			{
				RewritePrefabInstanceUuidReferences(childValue, uuidRemap);
			}
			return;
		}

		if (value.is_object())
		{
			for (auto& [name, childValue] : value.items())
			{
				(void)name;
				RewritePrefabInstanceUuidReferences(childValue, uuidRemap);
			}
		}
	}

	std::filesystem::path ResolvePrefabAssetPath(const AssetMetadata& metadata)
	{
		std::filesystem::path prefabPath = metadata.sourcePath;
		if (!prefabPath.is_absolute())
		{
			prefabPath = AssetDatabase::Instance().GetAssetRoot() / prefabPath;
		}
		return std::filesystem::absolute(prefabPath).lexically_normal();
	}

	bool AttachPrefabLink(entt::entity entity, const TerraEngine::UUID& prefabSourceId, const std::optional<TerraEngine::UUID>& sourceObjectId, const std::filesystem::path& prefabPath)
	{
		std::optional<Object> object = ObjectManager::instance().getObject(entity);
		if (!object)
		{
			return false;
		}

		PrefabLinkComponent* existingLink = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
		PrefabLinkComponent* ensuredLink = existingLink != nullptr ? existingLink : ObjectManager::instance().AddComponent<PrefabLinkComponent>(*object);
		if (ensuredLink == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Failed to attach prefab link metadata to dropped prefab instance.");
			return false;
		}

		PrefabLinkComponent& link = *ensuredLink;
		link.prefabSourceId = prefabSourceId;
		link.sourceObjectId = sourceObjectId.value_or(ObjectManager::instance().getUUID(*object));
		link.prefabName = prefabPath.stem().string();

		std::error_code relativeError;
		const std::filesystem::path relativePrefabPath = std::filesystem::relative(prefabPath, SceneManager::Instance().GetSceneRoot(), relativeError);
		link.prefabPath = relativeError ? prefabPath.generic_string() : relativePrefabPath.generic_string();
		link.overriddenProperties.clear();
		return true;
	}

	ProtectedPrefabInstantiationResult InstantiateRegisteredPrefabGraph(
		const std::filesystem::path& prefabPath,
		const TerraEngine::UUID& prefabGuid,
		nlohmann::json prefabJson,
		std::string_view requestedName,
		std::optional<entt::entity> parent)
	{
		ProtectedPrefabInstantiationResult result;
		const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(prefabJson);
		if (!prepared.accepted || prepared.cancelled || prepared.objectCount == 0U)
		{
			result.errorCategory = "validation";
			result.message = prepared.reason.empty() ? "Registered prefab has no root objects." : prepared.reason;
			return result;
		}

		ObjectManager& objects = ObjectManager::instance();
		if (objects.IsBulkPublicationActive())
		{
			result.errorCategory = "conflict";
			result.message = "Another protected scene publication is already active.";
			return result;
		}
		std::optional<TerraEngine::UUID> parentUuid;
		if (parent.has_value())
		{
			const std::optional<Object> parentObject = objects.getObject(*parent);
			if (!parentObject)
			{
				result.errorCategory = "validation";
				result.message = "Prefab parent no longer exists in the hierarchy.";
				return result;
			}
			parentUuid = objects.getUUID(*parentObject);
		}

		std::vector<std::optional<TerraEngine::UUID>> sourceRootObjectIds;
		std::unordered_map<TerraEngine::UUID, TerraEngine::UUID> uuidRemap;
		std::vector<TerraEngine::UUID> generatedObjectIds;
		std::string preparationFailure;
		sourceRootObjectIds.reserve(prefabJson["objects"].size());
		for (std::size_t rootIndex = 0U; rootIndex < prefabJson["objects"].size(); ++rootIndex)
		{
			nlohmann::json& objectJson = prefabJson["objects"][rootIndex];
			sourceRootObjectIds.push_back(ReadSerializedObjectUuid(objectJson));
			if (rootIndex == 0U && !requestedName.empty())
			{
				// A multi-root prefab has one canonical response root. Rename only that
				// root so secondary authored roots keep deterministic source names.
				objectJson["name"] = std::string(requestedName);
			}
			if (!CollectPrefabInstanceUuidRemap(objectJson, uuidRemap, generatedObjectIds, preparationFailure))
			{
				result.errorCategory = "validation";
				result.message = preparationFailure;
				return result;
			}
		}
		for (nlohmann::json& objectJson : prefabJson["objects"])
		{
			RewritePrefabInstanceUuidReferences(objectJson, uuidRemap);
		}

		const bool sceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();
		bool publicationActive = false;
		auto rollback = [&]() {
			// IDs are ordered root-before-child, so removing an existing root also
			// removes its descendants; later missing child lookups are harmless.
			for (const TerraEngine::UUID& objectId : generatedObjectIds)
			{
				if (std::optional<Object> object = objects.getObject(objectId))
				{
					objects.removeObject(*object);
				}
			}
			if (publicationActive)
			{
				objects.FinishBulkPublication(false);
				publicationActive = false;
			}
			if (!sceneWasDirty)
			{
				SceneManager::Instance().ClearActiveSceneDirty();
			}
		};

		const TerraEngine::UUID loadedPrefabId = SceneManager::Instance().LoadPrefab(prefabPath);
		const TerraEngine::UUID linkSourceId = loadedPrefabId == TerraEngine::EMPTY_UUID ? prefabGuid : loadedPrefabId;
		std::vector<std::unique_ptr<ICommand>> undoCommands;
		undoCommands.reserve(prefabJson["objects"].size() * (parentUuid.has_value() ? 2U : 1U));
		objects.BeginBulkPublication({prepared.objectCount, prepared.componentCount});
		publicationActive = true;
		try
		{
			for (std::size_t rootIndex = 0U; rootIndex < prefabJson["objects"].size(); ++rootIndex)
			{
				const entt::entity rootEntity = Serializer::DeserializeObjectEntity(prefabJson["objects"][rootIndex], parent);
				if (!objects.getObject(rootEntity).has_value()
					|| !AttachPrefabLink(rootEntity, linkSourceId, sourceRootObjectIds[rootIndex], prefabPath))
				{
					throw std::runtime_error("Prefab root could not be published with its link metadata.");
				}
				result.roots.push_back(rootEntity);
				undoCommands.push_back(std::make_unique<CreateObjectCommand>(rootEntity));
				if (parentUuid.has_value())
				{
					undoCommands.push_back(std::make_unique<SetParentCommand>(rootEntity, std::nullopt, parentUuid));
				}
			}

			for (const TerraEngine::UUID& objectId : generatedObjectIds)
			{
				if (!objects.getObject(objectId).has_value())
				{
					throw std::runtime_error("Prefab graph publication omitted an authored object.");
				}
			}
		}
		catch (const std::exception& error)
		{
			rollback();
			result.roots.clear();
			result.errorCategory = "validation";
			result.message = std::string("Registered prefab graph was rejected: ") + error.what();
			return result;
		}

		objects.FinishBulkPublication(true);
		publicationActive = false;
		UndoRedoSystem::Instance().PushTransactionGroup("Instantiate Prefab", std::move(undoCommands));
		SceneManager::Instance().ForceMarkActiveSceneDirty();
		result.ok = true;
		result.objectCount = prepared.objectCount;
		return result;
	}
}

// MainEditorSubsystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Initialize(EditorContext& context) {
	ctx = &context;

	// Set engine root to the executable's directory so default shaders can be found
	// even if the project asset root is changed later.
	ResourceManager::Instance().SetEngineRoot(ResolveEditorRuntimeRoot());
	
	// Initialize Windows
	context.editorProfiler = std::make_shared<EditorProfilerService>();
	context.editorProfiler->Initialize(context);
	m_Windows.push_back(std::make_shared<HierarchyWindow>(this));
	m_Windows.push_back(std::make_shared<InspectorWindow>(this));
	m_Windows.push_back(std::make_shared<ProjectBrowserWindow>(this));
	m_Windows.push_back(std::make_shared<GameViewWindow>(this));
	m_Windows.push_back(std::make_shared<SceneViewWindow>(this));
	m_Windows.push_back(std::make_shared<ProjectSettingsWindow>(this));
	m_Windows.push_back(std::make_shared<PreferencesWindow>(this));
	m_Windows.push_back(std::make_shared<BuildProfilesWindow>(this));
	m_Windows.push_back(std::make_shared<ProfilerWindow>(this));
	m_Windows.push_back(std::make_shared<PhysicsDiagnosticsWindow>(this));
	m_Windows.push_back(std::make_shared<SchedulerDiagnosticsWindow>(this));
	m_Windows.push_back(std::make_shared<ResourceResidencyWindow>(this));
	m_Windows.push_back(std::make_shared<UIBuilderWindow>(this));
	// These windows declare their own Animation menu paths. Registering the
	// persistent objects here lets the existing docking/layout system restore
	// visibility without giving a menu command any authoring side effects.
	// Animation authoring windows observe the shared Project Browser selection.
	// Passing this owner keeps selection lifetime in the workspace subsystem and
	// prevents any window from retaining a transient browser item pointer.
	m_Windows.push_back(std::make_shared<SkeletonEditorWindow>(this));
	m_Windows.push_back(std::make_shared<AnimationClipEditorWindow>(this));
	m_Windows.push_back(std::make_shared<AnimationControllerWindow>(this));
	m_Windows.push_back(std::make_shared<RetargetEditorWindow>(this));
	m_Windows.push_back(std::make_shared<MotionDatabaseWindow>(this));
	m_Windows.push_back(std::make_shared<RagdollEditorWindow>(this));
	m_Windows.push_back(std::make_shared<AnimationDiagnosticsWindow>());

	UndoRedoSystem::Instance().Initialize(context);
	ScriptCompilationSubsystem::Instance().Initialize(context);

	ObjectManager::instance().SetOnModifiedCallback([this]() {
		if (m_SuppressObjectModifiedCallback)
		{
			return;
		}
		if (ScriptingSystem::Instance().IsRunning())
		{
			// Play mode runs against a temporary scene snapshot. Runtime script
			// changes, spawned objects, and deferred mutation queue flushes should
			// be discarded on Stop instead of marking the authored scene file dirty.
			return;
		}
		if (m_IsPrefabEditingMode)
		{
			m_PrefabEditingDirty = true;
			return;
		}
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	});

	ScriptingSystem::Instance().SetRunning(false);
	// Editor authoring evaluates a fresh tick-zero palette without advancing it.
	// Some imported skins need that palette to display their authored pose; raw
	// unskinned vertex positions are not necessarily the visible reference pose.
	auto& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
	animationSystem.SetAuthoredComponentPlaybackEnabled(true);
	animationSystem.SetAuthoredComponentPlaybackAdvancing(false);
	ClearPlayModeSceneState();
	g_ActiveMainEditorSubsystem = this;
	SetApplicationEditorStopPlayModeCallback(&StopPlayModeFromApplicationQuit);
}

// MainEditorSubsystem::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnProjectOpened() {
	if (!ctx || !ctx->projectManager->HasActiveProject()) return;
	auto& animationSystem = TerraEngine::Animation::AnimationSystem::Instance();
	animationSystem.SetAuthoredComponentPlaybackEnabled(true);
	animationSystem.SetAuthoredComponentPlaybackAdvancing(false);

	auto projectRoot = ctx->projectManager->GetProjectRoot();
	std::filesystem::path assetRoot = projectRoot / "Assets";
	std::filesystem::path sceneRoot = assetRoot / "Scenes";
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(sceneRoot)) {
		std::filesystem::create_directories(sceneRoot);
	}

	// Set Scene Root to Assets folder so we can load scenes from anywhere inside Assets
	SceneManager::Instance().SetSceneRoot(assetRoot.string());
	ResourceManager::Instance().SetAssetRoot(assetRoot);
	RenderSystem::Instance().LoadSettings();
	if (auto preferences = GetWindow<PreferencesWindow>())
	{
		preferences->ApplyStoredPreferencesToRenderSystem();
	}
	
	// Check for last opened scene
	std::filesystem::path lastScenePath = projectRoot / "Library" / "LastScene.txt";
	bool loaded = false;
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(lastScenePath)) {
		std::ifstream in(lastScenePath);
		std::string relPath;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (std::getline(in, relPath) && !relPath.empty()) {
			// Verify it still exists
			if (std::filesystem::exists(assetRoot / relPath)) {
				OpenScene(relPath);
				if (sceneLoaded) loaded = true;
			}
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!loaded) {
		bool foundAny = false;
		if (std::filesystem::exists(assetRoot)) {
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (const auto& entry : std::filesystem::recursive_directory_iterator(assetRoot)) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (entry.is_regular_file() && entry.path().extension() == ".tescene") {
					std::error_code ec;
					auto rel = std::filesystem::relative(entry.path(), assetRoot, ec);
					// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
					if (!ec) {
						OpenScene(rel);
						foundAny = true;
						break;
					}
				}
			}
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!foundAny) {
			// Maybe create a new one? For now just do nothing or log.
		}
	}

	// Start simulation-essential managed readiness before refreshing editor-only
	// windows. The scene has already crossed its single protected publication
	// boundary, so no panel can observe a partially populated ObjectManager.
	ScriptCompilationSubsystem::Instance().OnProjectOpened();

	// Project Browser, build-profile UI, profiler documents, and UI Builder state
	// are editor metadata. Refresh them only after the active authored scene is
	// complete; this work cannot delay or partially expose simulation state.
	if (auto browser = GetWindow<ProjectBrowserWindow>()) browser->Reset();
	if (auto buildProfiles = GetWindow<BuildProfilesWindow>()) buildProfiles->OnProjectOpened();
	if (ctx->editorProfiler) ctx->editorProfiler->OnProjectOpened();
	if (auto uiBuilder = GetWindow<UIBuilderWindow>()) uiBuilder->ResetForProjectOpen();
	ctx->projectManager->MarkEditorMetadataReady();
}

// MainEditorSubsystem::CapturePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CapturePlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneSnapshotPacket.reset();
	m_PlayModeSelectedEntityUuid.reset();
	m_PlayModeSelectedEntityUuids.clear();
	m_PlayModeSceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();
	m_PlayModeSceneRenderSettings = SceneManager::Instance().GetActiveSceneRenderSettings();
	m_LastPlayModeRestorationResult = {};

	nlohmann::json snapshotJson;
	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	snapshotJson["name"] = activeScene != nullptr ? activeScene->name : "ActiveScene";
	snapshotJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(SceneManager::Instance().GetActiveSceneRenderSettings());
	snapshotJson["objects"] = nlohmann::json::array();

	// Capture the full live hierarchy and reflected component payloads before
	// play starts so Stop can rebuild the exact authored scene state without
	// touching the on-disk scene file.
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		snapshotJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
	auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
	TERRAENGINE_LOG(LogLevel::Info, "Captured editor play-mode scene summary with " << summary.objects.size() << " objects.");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PlayModeSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}
	// Store every stable selection ID, not only the primary object. The packet
	// changes EnTT handles during publication, while UUIDs continue to identify
	// the same authored selection after the protected restore boundary.
	m_PlayModeSelectedEntityUuids = selectedEntityUuids;

	// Prepare an owned, deterministic object table before Play begins. This
	// replaces the former text dump/reparse path: the cached packet carries the
	// authored data directly to the one protected Stop boundary.
	const std::uint64_t expectedGeneration = ObjectManager::instance().GetSceneGeneration();
	const TerraEngine::Scene::PreparedSceneDecode prepared = Serializer::PrepareSceneDecode(snapshotJson);
	m_PlayModeSceneSnapshotPacket = TerraEngine::Scene::BuildBulkScenePublicationPacket(
		prepared,
		expectedGeneration,
		expectedGeneration);
	m_LastPlayModeRestorationResult.snapshotCaptured = !m_PlayModeSceneSnapshotPacket->cancelled;
	m_LastPlayModeRestorationResult.preparedGeneration = expectedGeneration;
	m_LastPlayModeRestorationResult.reason = prepared.reason;
	if (!prepared.accepted)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Could not prepare the authored Play Mode restoration packet: " << prepared.reason);
		m_PlayModeSceneSnapshotPacket.reset();
		return;
	}
	m_HasPlayModeSceneSnapshot = true;
}

// MainEditorSubsystem::RestorePlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RestorePlayModeSceneState()
{
	if (!m_HasPlayModeSceneSnapshot || !m_PlayModeSceneSnapshotPacket.has_value())
	{
		return;
	}

	// Runtime code may replace or clear the live scene while Play Mode is active,
	// which legitimately advances ObjectManager's scene generation. The cached
	// packet is owned by this exact Play Mode session, so retarget its optimistic
	// generation check to the live runtime graph at Stop. Project close, editor
	// scene changes, and shutdown clear m_HasPlayModeSceneSnapshot separately;
	// therefore only the active session can authorize this protected restore.
	TerraEngine::Scene::BulkScenePublicationPacket restorePacket = *m_PlayModeSceneSnapshotPacket;
	restorePacket.expectedSceneGeneration = ObjectManager::instance().GetSceneGeneration();

	// Stop runs on the editor's protected lifecycle boundary, so it may make the
	// complete authored graph visible atomically after the session-owned packet
	// has been rebound to the runtime scene generation it is replacing.
	const TerraEngine::Scene::BulkScenePublicationPreflight preflight =
		TerraEngine::Scene::PreflightBulkScenePublication(restorePacket, ObjectManager::instance());
	if (!preflight.accepted)
	{
		m_LastPlayModeRestorationResult.stale = true;
		m_LastPlayModeRestorationResult.reason = preflight.reason;
		TERRAENGINE_LOG(LogLevel::Warning, "Play Mode authored restore was rejected: " << preflight.reason);
		ClearPlayModeSceneState();
		SetSelectedEntity(std::nullopt);
		return;
	}

	const TerraEngine::Scene::BulkScenePublicationResult publication =
		TerraEngine::Scene::PublishBulkScenePacket(restorePacket, ObjectManager::instance());
	m_LastPlayModeRestorationResult.cancelled = publication.cancelled;
	m_LastPlayModeRestorationResult.publishedGeneration = publication.publishedSceneGeneration;
	m_LastPlayModeRestorationResult.reason = publication.reason;
	if (!publication.published)
	{
		m_LastPlayModeRestorationResult.stale = publication.reason.find("stale scene generation") != std::string::npos;
		TERRAENGINE_LOG(LogLevel::Error, "Play Mode authored restore failed: " << publication.reason);
		ClearPlayModeSceneState();
		SetSelectedEntity(std::nullopt);
		return;
	}

	SceneManager::Instance().SetActiveSceneRenderSettings(m_PlayModeSceneRenderSettings);

	ScriptingSystem::Instance().RebuildRestoredSceneBindings();
	auto restoredGraph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
	auto restoredSummary = TerraEngine::Scene::BuildSceneRuntimeSummary(restoredGraph);
	TERRAENGINE_LOG(LogLevel::Info, "Restored editor play-mode scene summary with " << restoredSummary.objects.size() << " objects.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_PlayModeSceneWasDirty)
	{
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
	else
	{
		SceneManager::Instance().ClearActiveSceneDirty();
	}
	m_LastPlayModeRestorationResult.dirtyStateRestored = true;
	m_LastPlayModeRestorationResult.prefabStatePreserved = m_IsPrefabEditingMode;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	std::vector<entt::entity> restoredSelection;
	restoredSelection.reserve(m_PlayModeSelectedEntityUuids.size());
	for (const TerraEngine::UUID objectId : m_PlayModeSelectedEntityUuids)
	{
		if (const std::optional<Object> restoredObject = ObjectManager::instance().getObject(objectId))
		{
			restoredSelection.push_back(restoredObject->EntityId());
		}
	}
	if (!restoredSelection.empty())
	{
		SetSelectedEntities(restoredSelection);
		m_LastPlayModeRestorationResult.selectionRestored = restoredSelection.size() == m_PlayModeSelectedEntityUuids.size();
	}
	else if (m_PlayModeSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PlayModeSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
			m_LastPlayModeRestorationResult.selectionRestored = true;
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else
	{
		SetSelectedEntity(std::nullopt);
	}
	m_LastPlayModeRestorationResult.completed = true;

	ClearPlayModeSceneState();
}

// MainEditorSubsystem::ClearPlayModeSceneState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ClearPlayModeSceneState()
{
	m_HasPlayModeSceneSnapshot = false;
	m_PlayModeSceneWasDirty = false;
	m_PlayModeSceneSnapshotPacket.reset();
	m_PlayModeSelectedEntityUuid.reset();
	m_PlayModeSelectedEntityUuids.clear();
}

void MainEditorSubsystem::CapturePrefabReturnSceneState()
{
	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnSceneWasDirty = SceneManager::Instance().IsActiveSceneDirty();
	m_PrefabReturnGameViewCameras.clear();

	const auto sceneCameras = ObjectManager::instance().getObjectsWithComponents<Transform, CameraComponent>();
	for (Object cameraObject : sceneCameras)
	{
		const Transform* transform = ObjectManager::instance().GetComponent<Transform>(cameraObject);
		const CameraComponent* camera = ObjectManager::instance().GetComponent<CameraComponent>(cameraObject);
		if (transform == nullptr || camera == nullptr)
		{
			continue;
		}

		// Copy only the render-facing camera fields needed to rebuild the same
		// Game View camera after prefab mode swaps in an isolated ObjectManager.
		PrefabReturnGameViewCamera cachedCamera;
		cachedCamera.position = transform->position;
		cachedCamera.rotation = transform->rotation;
		cachedCamera.up = camera->up;
		cachedCamera.verticalFovDegrees = camera->verticalFovDegrees;
		cachedCamera.nearPlane = camera->nearPlane;
		cachedCamera.farPlane = camera->farPlane;
		m_PrefabReturnGameViewCameras.push_back(cachedCamera);
	}

	nlohmann::json snapshotJson;
	const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
	snapshotJson["name"] = activeScene != nullptr ? activeScene->name : "ActiveScene";
	snapshotJson["sceneSettings"] = Serializer::SerializeSceneRenderSettings(SceneManager::Instance().GetActiveSceneRenderSettings());
	snapshotJson["objects"] = nlohmann::json::array();

	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		snapshotJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PrefabReturnSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}

	m_PrefabReturnSceneSnapshotJson = snapshotJson.dump();
	m_HasPrefabReturnSceneSnapshot = true;
}

void MainEditorSubsystem::RestorePrefabReturnSceneState()
{
	if (!m_HasPrefabReturnSceneSnapshot)
	{
		return;
	}

	if (!LoadPrefabReturnSceneSnapshotIntoObjectManager(true))
	{
		return;
	}

	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnGameViewCameras.clear();
}

void MainEditorSubsystem::CaptureCurrentPrefabEditingState()
{
	if (!m_IsPrefabEditingMode || !m_PrefabObjectsLoadedForEditor)
	{
		return;
	}

	nlohmann::json prefabJson;
	prefabJson["name"] = m_PrefabEditingPath.empty() ? "Prefab" : m_PrefabEditingPath.stem().string();
	prefabJson["objects"] = nlohmann::json::array();
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		prefabJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	if (selectedEntity.has_value())
	{
		if (std::optional<Object> selectedObject = ObjectManager::instance().getObject(*selectedEntity))
		{
			m_PrefabEditingSelectedEntityUuid = ObjectManager::instance().getUUID(*selectedObject);
		}
	}
	else
	{
		m_PrefabEditingSelectedEntityUuid.reset();
	}

	m_PrefabEditingObjectSnapshotJson = prefabJson.dump();
}

bool MainEditorSubsystem::EnsurePrefabObjectsLoadedForEditor()
{
	if (!m_IsPrefabEditingMode || m_PrefabObjectsLoadedForEditor)
	{
		return false;
	}

	return LoadPrefabEditingSnapshotIntoObjectManager();
}

void MainEditorSubsystem::RestoreSceneObjectsForRuntimeDuringPrefabEdit()
{
	if (!m_IsPrefabEditingMode || !m_PrefabObjectsLoadedForEditor)
	{
		return;
	}

	if (!m_PrefabEditOpenedFromSceneObject)
	{
		return;
	}

	CaptureCurrentPrefabEditingState();
	if (LoadPrefabReturnSceneSnapshotIntoObjectManager(false))
	{
		m_PrefabObjectsLoadedForEditor = false;
		SetSelectedEntity(std::nullopt);
	}
}

bool MainEditorSubsystem::LoadPrefabEditingSnapshotIntoObjectManager()
{
	if (m_PrefabEditingObjectSnapshotJson.empty())
	{
		return false;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PrefabEditingObjectSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse prefab editing snapshot at byte " << parseError.byte << ": " << parseError.what());
		SetSelectedEntity(std::nullopt);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	m_PrefabObjectsLoadedForEditor = true;
	if (m_PrefabEditingSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PrefabEditingSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else
	{
		SetSelectedEntity(std::nullopt);
	}

	return true;
}

bool MainEditorSubsystem::LoadPrefabReturnSceneSnapshotIntoObjectManager(bool restoreSelection)
{
	if (!m_HasPrefabReturnSceneSnapshot)
	{
		return false;
	}

	nlohmann::json snapshotJson;
	try
	{
		snapshotJson = nlohmann::json::parse(m_PrefabReturnSceneSnapshotJson);
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to parse prefab return scene snapshot at byte " << parseError.byte << ": " << parseError.what());
		SetSelectedEntity(std::nullopt);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (snapshotJson.contains("sceneSettings"))
	{
		SceneManager::Instance().SetActiveSceneRenderSettings(Serializer::DeserializeSceneRenderSettings(snapshotJson["sceneSettings"]));
	}
	if (snapshotJson.contains("objects") && snapshotJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : snapshotJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	if (m_PrefabReturnSceneWasDirty)
	{
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
	else
	{
		SceneManager::Instance().ClearActiveSceneDirty();
	}

	if (restoreSelection && m_PrefabReturnSelectedEntityUuid.has_value())
	{
		if (auto restoredObject = ObjectManager::instance().getObject(*m_PrefabReturnSelectedEntityUuid))
		{
			SetSelectedEntity(restoredObject->EntityId());
		}
		else
		{
			SetSelectedEntity(std::nullopt);
		}
	}
	else if (restoreSelection)
	{
		SetSelectedEntity(std::nullopt);
	}

	return true;
}

bool MainEditorSubsystem::TryApplyPrefabModeGameViewCameras(float aspect) const
{
	if (!m_IsPrefabEditingMode || m_PrefabReturnGameViewCameras.empty())
	{
		return false;
	}

	if (aspect <= 0.0001f)
	{
		aspect = 1.0f;
	}

	for (const PrefabReturnGameViewCamera& camera : m_PrefabReturnGameViewCameras)
	{
		const glm::vec3 forward = camera.rotation * glm::vec3(0.0f, 0.0f, -1.0f);
		glm::vec3 up = camera.rotation * camera.up;
		if (glm::length(up) < 0.0001f)
		{
			up = glm::vec3(0.0f, 1.0f, 0.0f);
		}

		// Feed the renderer through its normal Game View camera entry point, so
		// prefab editing only changes camera ownership, not render behavior.
		const glm::mat4 view = glm::lookAt(camera.position, camera.position + forward, glm::normalize(up));
		// Keep prefab-return Game View cameras in Vulkan's 0..1 depth space so
		// renderer depth tests and Hi-Z occlusion interpret the same projection.
		glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDegrees), aspect, camera.nearPlane, camera.farPlane);
		proj[1][1] *= -1.0f;
		RenderSystem::Instance().SetGameViewCamera(view, proj, camera.position);
	}

	return true;
}

bool MainEditorSubsystem::LoadPrefabIntoIsolatedObjectManager(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to open prefab for editing: " << path);
		return false;
	}

	nlohmann::json prefabJson;
	try
	{
		input >> prefabJson;
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Prefab edit file failed JSON parse at byte " << parseError.byte << ": " << parseError.what() << " Path: " << path);
		return false;
	}

	m_SuppressObjectModifiedCallback = true;
	ObjectManager::instance().clear();
	if (prefabJson.contains("objects") && prefabJson["objects"].is_array())
	{
		for (const nlohmann::json& objectJson : prefabJson["objects"])
		{
			Serializer::DeserializeObjectEntity(objectJson);
		}
	}
	m_SuppressObjectModifiedCallback = false;

	SetSelectedEntity(std::nullopt);
	return true;
}

// MainEditorSubsystem::OpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OpenScene(const std::filesystem::path& path) {
	if (m_IsPrefabEditingMode)
	{
		ClosePrefabMode();
	}
	ProjectSceneLoadDecision loadDecision;
	TerraEngine::UUID sceneId = TerraEngine::EMPTY_UUID;
	if (ctx != nullptr && ctx->projectManager != nullptr && ctx->projectManager->HasActiveProject())
	{
		std::string preparationError;
		if (!ctx->projectManager->PrepareSceneForLoading(path, loadDecision, &preparationError))
		{
			TERRAENGINE_LOG(LogLevel::Error, "Could not prepare scene '" << path << "': " << preparationError);
			return;
		}
		if (!ctx->projectManager->IsProjectLoadGenerationCurrent(loadDecision.projectGeneration))
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Discarded a prepared scene after its project generation closed or changed.");
			return;
		}
		sceneId = SceneManager::Instance().LoadPreparedScene(
			path,
			std::move(loadDecision.sceneDocument),
			std::move(loadDecision.preparedScene));
	}
	else
	{
		sceneId = SceneManager::Instance().LoadScene(path);
	}
		if (sceneId != TerraEngine::EMPTY_UUID && SceneManager::Instance().SetActiveScene(sceneId)) {
			sceneLoaded = true;
			m_CurrentScenePath = path;
			ClearPlayModeSceneState();
			UndoRedoSystem::Instance().Clear();

			auto graph = TerraEngine::Scene::BuildSceneGraphFromObjectManager(ObjectManager::instance());
			auto summary = TerraEngine::Scene::BuildSceneRuntimeSummary(graph);
			TERRAENGINE_LOG(LogLevel::Info, "Editor scene graph loaded with " << summary.objects.size() << " objects and " << summary.diagnostics.size() << " diagnostics.");
			for (const auto& diagnostic : summary.diagnostics)
			{
				TERRAENGINE_LOG(LogLevel::Warning, "Editor scene graph diagnostic: " << TerraEngine::Scene::FormatSceneDiagnostic(diagnostic));
			}
			if (ctx != nullptr && ctx->projectManager != nullptr && loadDecision.prepared)
			{
				ctx->projectManager->MarkSimulationEssentialReady(loadDecision);
			}
	
			// Save to LastScene.txt
	
		if (ctx && ctx->projectManager->HasActiveProject()) {
			auto projectRoot = ctx->projectManager->GetProjectRoot();
			std::filesystem::path libDir = projectRoot / "Library";
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

bool MainEditorSubsystem::OpenPrefabAsset(const std::filesystem::path& path)
{
	return OpenPrefabAssetInternal(path, false);
}

bool MainEditorSubsystem::InstantiatePrefabAsset(const TerraEngine::UUID& prefabGuid, std::optional<entt::entity> parent)
{
	const AssetMetadata* metadata = AssetDatabase::Instance().FindAssetByGuid(prefabGuid);
	if (metadata == nullptr || metadata->importerName != "PrefabImporter")
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Dropped prefab payload does not reference a registered prefab asset.");
		return false;
	}

	const std::filesystem::path prefabPath = ResolvePrefabAssetPath(*metadata);
	std::filesystem::path databaseRelativePath = metadata->sourcePath.lexically_normal();
	if (databaseRelativePath.is_absolute())
	{
		std::error_code relativeError;
		databaseRelativePath = std::filesystem::relative(
			databaseRelativePath, AssetDatabase::Instance().GetAssetRoot(), relativeError);
		if (relativeError)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Registered prefab path is outside the active AssetDatabase root: " << prefabPath);
			return false;
		}
	}

	std::uint64_t publishedGeneration = 1U;
	GeneratedModelPackageChildMetadata generatedChild;
	if (AssetDatabase::Instance().TryGetGeneratedModelPackageChildMetadata(
		prefabGuid, GeneratedModelPackageChildKind::Prefab, generatedChild))
	{
		publishedGeneration = std::max<std::uint64_t>(1U, generatedChild.publishedGeneration);
	}

	// Drag/drop and automation register the indexed asset into the same service,
	// then enter one graph transaction. This prevents either caller from drifting
	// into a simplified or partially visible prefab implementation.
	const PrefabAuthoringService::Result registered = prefabAuthoringService.RegisterExistingAsset(
		(std::filesystem::path("Assets") / databaseRelativePath).lexically_normal(),
		prefabPath,
		prefabGuid,
		publishedGeneration);
	const PrefabAuthoringService::Result instantiated = registered.ok
		? prefabAuthoringService.Instantiate(registered.data.value("prefabId", std::string{}), {}, parent)
		: registered;
	if (!instantiated.ok)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to instantiate registered prefab graph: " << instantiated.message << " Path: " << prefabPath);
		return false;
	}

	const std::optional<TerraEngine::UUID> instanceId = TerraEngine::ParseUUID(
		instantiated.data.value("instanceId", std::string{}));
	const std::optional<Object> instance = instanceId ? ObjectManager::instance().getObject(*instanceId) : std::nullopt;
	if (!instance)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Prefab graph transaction returned an invalid canonical root: " << prefabPath);
		return false;
	}
	SetSelectedEntity(instance->EntityId());
	TERRAENGINE_LOG(LogLevel::Info, "Instantiated prefab into hierarchy: " << prefabPath);
	return true;
}

bool MainEditorSubsystem::OpenPrefabAssetInternal(const std::filesystem::path& path, bool openedFromSceneObject)
{
	const std::filesystem::path absolutePath = std::filesystem::absolute(path).lexically_normal();
	if (absolutePath.extension() != ".teprefab")
	{
		return false;
	}

	if (m_IsPrefabEditingMode && m_PrefabEditingDirty)
	{
		SaveOpenPrefab();
	}

	if (!m_IsPrefabEditingMode)
	{
		CapturePrefabReturnSceneState();
	}

	if (!LoadPrefabIntoIsolatedObjectManager(absolutePath))
	{
		if (!m_IsPrefabEditingMode)
		{
			RestorePrefabReturnSceneState();
		}
		return false;
	}

	m_IsPrefabEditingMode = true;
	m_PrefabEditingDirty = false;
	m_PrefabEditingPath = absolutePath;
	m_PrefabObjectsLoadedForEditor = true;
	m_PrefabEditOpenedFromSceneObject = openedFromSceneObject;
	CaptureCurrentPrefabEditingState();

	TERRAENGINE_LOG(LogLevel::Info, "Opened prefab in isolated edit mode: " << absolutePath);
	return true;
}

bool MainEditorSubsystem::OpenPrefabFromObject(entt::entity object)
{
	std::optional<Object> objectHandle = ObjectManager::instance().getObject(object);
	PrefabLinkComponent* link = objectHandle ? ObjectManager::instance().GetComponent<PrefabLinkComponent>(*objectHandle) : nullptr;
	if (link == nullptr || link->prefabPath.empty())
	{
		return false;
	}

	std::filesystem::path prefabPath = link->prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}
	return OpenPrefabAssetInternal(prefabPath, true);
}

bool MainEditorSubsystem::ResetPrefabInstanceToDefaults(entt::entity object)
{
	ObjectManager& objManager = ObjectManager::instance();
	std::optional<Object> objectHandle = objManager.getObject(object);
	PrefabLinkComponent* link = objectHandle ? objManager.GetComponent<PrefabLinkComponent>(*objectHandle) : nullptr;
	if (link == nullptr || link->prefabPath.empty())
	{
		return false;
	}

	std::filesystem::path prefabPath = link->prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}

	std::ifstream prefabInput(prefabPath, std::ios::binary);
	if (!prefabInput)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot reset prefab instance because the source file could not be opened: " << prefabPath);
		return false;
	}

	nlohmann::json prefabJson;
	try
	{
		prefabInput >> prefabJson;
	}
	catch (const nlohmann::json::parse_error& parseError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot reset prefab instance because the source file failed JSON parse at byte " << parseError.byte << ": " << parseError.what());
		return false;
	}

	if (!prefabJson.contains("objects") || !prefabJson["objects"].is_array() || prefabJson["objects"].empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Cannot reset prefab instance because the prefab has no root object: " << prefabPath);
		return false;
	}

	const TerraEngine::UUID prefabSourceId = link->prefabSourceId;
	const std::string prefabName = link->prefabName;
	std::optional<Object> parentHandle = objectHandle ? objManager.getParent(*objectHandle) : std::nullopt;
	std::optional<entt::entity> parentObject;
	if (parentHandle)
	{
		parentObject = parentHandle->EntityId();
	}

	objManager.removeObject(*objectHandle);
	entt::entity restoredObject = Serializer::DeserializeObjectEntity(prefabJson["objects"].front(), parentObject);
	std::optional<Object> restoredHandle = objManager.getObject(restoredObject);
	if (!restoredHandle)
	{
		return false;
	}
	PrefabLinkComponent* existingRestoredLink = objManager.GetComponent<PrefabLinkComponent>(*restoredHandle);
	PrefabLinkComponent* addedRestoredLink = existingRestoredLink != nullptr ? existingRestoredLink : objManager.AddComponent<PrefabLinkComponent>(*restoredHandle);
	if (addedRestoredLink == nullptr)
	{
		return false;
	}
	PrefabLinkComponent& restoredLink = *addedRestoredLink;
	restoredLink.prefabSourceId = prefabSourceId;
	restoredLink.sourceObjectId = objManager.getUUID(*restoredHandle);
	restoredLink.prefabName = prefabName.empty() ? prefabPath.stem().string() : prefabName;

	std::error_code relativeError;
	const std::filesystem::path relativePrefabPath = std::filesystem::relative(prefabPath, SceneManager::Instance().GetSceneRoot(), relativeError);
	restoredLink.prefabPath = relativeError ? prefabPath.generic_string() : relativePrefabPath.generic_string();
	restoredLink.overriddenProperties.clear();

	SetSelectedEntity(restoredObject);
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	TERRAENGINE_LOG(LogLevel::Info, "Reset prefab instance to defaults from " << prefabPath);
	return true;
}

bool MainEditorSubsystem::SaveOpenPrefab()
{
	if (!m_IsPrefabEditingMode || m_PrefabEditingPath.empty())
	{
		return false;
	}

	if (!m_PrefabObjectsLoadedForEditor && !m_PrefabEditingObjectSnapshotJson.empty())
	{
		nlohmann::json snapshotJson;
		try
		{
			snapshotJson = nlohmann::json::parse(m_PrefabEditingObjectSnapshotJson);
		}
		catch (const nlohmann::json::parse_error& parseError)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab because the cached edit snapshot failed JSON parse at byte " << parseError.byte << ": " << parseError.what());
			return false;
		}

		std::ofstream snapshotOutput(m_PrefabEditingPath, std::ios::binary);
		if (!snapshotOutput)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab: " << m_PrefabEditingPath);
			return false;
		}
		snapshotOutput << snapshotJson.dump(4);
		m_PrefabEditingDirty = false;
		TERRAENGINE_LOG(LogLevel::Info, "Saved prefab: " << m_PrefabEditingPath);
		return true;
	}

	nlohmann::json prefabJson;
	prefabJson["name"] = m_PrefabEditingPath.stem().string();
	prefabJson["objects"] = nlohmann::json::array();
	for (Object rootObject : ObjectManager::instance().getRootObjects())
	{
		prefabJson["objects"].push_back(Serializer::SerializeObject(rootObject));
	}

	std::ofstream output(m_PrefabEditingPath, std::ios::binary);
	if (!output)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to save prefab: " << m_PrefabEditingPath);
		return false;
	}

	output << prefabJson.dump(4);
	m_PrefabEditingDirty = false;
	TERRAENGINE_LOG(LogLevel::Info, "Saved prefab: " << m_PrefabEditingPath);
	return true;
}

void MainEditorSubsystem::ClosePrefabMode()
{
	if (!m_IsPrefabEditingMode)
	{
		return;
	}

	if (m_PrefabEditingDirty)
	{
		SaveOpenPrefab();
	}

	m_IsPrefabEditingMode = false;
	m_PrefabEditingDirty = false;
	m_PrefabEditingPath.clear();
	m_PrefabObjectsLoadedForEditor = false;
	m_PrefabEditOpenedFromSceneObject = false;
	m_PrefabEditingObjectSnapshotJson.clear();
	m_PrefabEditingSelectedEntityUuid.reset();
	RestorePrefabReturnSceneState();

	TERRAENGINE_LOG(LogLevel::Info, "Closed prefab edit mode and restored the active scene.");
}

// MainEditorSubsystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Shutdown() {
	// Provider-owned bodies must be gone before editor teardown can release the
	// authored ObjectManager state referenced by the current physics snapshot.
	TerraEngine::Physics::PhysicsSystem::Instance().StopWorld();
	ClearPlayModeSceneState();
	m_PrefabReturnGameViewCameras.clear();
	if (g_ActiveMainEditorSubsystem == this)
	{
		g_ActiveMainEditorSubsystem = nullptr;
	}
	SetApplicationEditorStopPlayModeCallback(nullptr);
	ScriptCompilationSubsystem::Instance().Shutdown();
	UndoRedoSystem::Instance().Shutdown();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr && ctx->editorProfiler) {
		ctx->editorProfiler->Shutdown();
		ctx->editorProfiler.reset();
	}
	ctx = nullptr;
	m_Windows.clear();
}

bool MainEditorSubsystem::StartPlayMode()
{
	if(ScriptingSystem::Instance().IsRunning())
		return false;
	if (ctx != nullptr && ctx->projectManager != nullptr && ctx->projectManager->HasActiveProject()
		&& !ctx->projectManager->GetReadinessState().simulationEssentialReady)
	{
		// Play Mode must never start against a project whose scene publication was
		// cancelled, rejected, or is still waiting for its protected boundary.
		return false;
	}
	// Commit pending authoring transactions before capturing the exact pre-play
	// snapshot consumed and later restored by both scripting and physics.
	SceneManager::Instance().ApplySceneMutations("play-mode-entry");
	CapturePlayModeSceneState();
	if (!m_HasPlayModeSceneSnapshot)
	{
		// Starting runtime systems without a valid authored restore packet would
		// leave Play Mode unable to return the editor to a complete scene.
		return false;
	}
	if(!TerraEngine::Physics::PhysicsSystem::Instance().StartWorld())
	{
		ClearPlayModeSceneState();
		return false;
	}
	if(ctx != nullptr && ctx->engine != nullptr)
		ctx->engine->ResetFrameTimer();
	// Entering Play Mode invalidates stopped edit-domain bindings. Their next
	// protected synchronization starts each authored default clip atomically.
	TerraEngine::Animation::AnimationSystem::Instance()
		.SetAuthoredComponentPlaybackEnabled(true);
	TerraEngine::Animation::AnimationSystem::Instance()
		.SetAuthoredComponentPlaybackAdvancing(true);
	ScriptingSystem::Instance().SetRunning(true);
	ScriptingSystem::Instance().SetPaused(false);
	(void)TerraEngine::Physics::PhysicsSystem::Instance().SetPaused(false);
	return true;
}

bool MainEditorSubsystem::SetPlayModePaused(bool paused)
{
	if(!ScriptingSystem::Instance().IsRunning())
		return false;
	if(!TerraEngine::Physics::PhysicsSystem::Instance().SetPaused(paused))
		return false;
	ScriptingSystem::Instance().SetPaused(paused);
	return true;
}

bool MainEditorSubsystem::StepPlayMode()
{
	if(!ScriptingSystem::Instance().IsRunning() || !ScriptingSystem::Instance().IsPaused())
		return false;
	auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
	if(!physics.RequestExplicitStep())
		return false;
	ScriptingSystem::Instance().StepExplicit(physics.FixedTimeStep(), 1);
	return true;
}

// MainEditorSubsystem::StopPlayMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::StopPlayMode()
{
	if (!ScriptingSystem::Instance().IsRunning()
		&& !TerraEngine::Physics::PhysicsSystem::Instance().WorldHandle().IsValid()
		&& !m_HasPlayModeSceneSnapshot)
	{
		return;
	}

	// Route every stop request through one helper so toolbar clicks and
	// Application.Quit() both restore the same cached pre-play scene snapshot.
	// Stop and drain provider work before restoring or destroying authored
	// objects. Runtime body handles never survive across editor play sessions.
	TerraEngine::Physics::PhysicsSystem::Instance().StopWorld();
	ScriptingSystem::Instance().SetRunning(false);
	ScriptingSystem::Instance().SetPaused(false);
	// Recreate component-owned playback at tick zero before the authored snapshot
	// returns. Edit Mode continues publishing that frozen reference palette, so
	// Stop cannot retain the last runtime deformation or expose raw skin vertices.
	TerraEngine::Animation::AnimationSystem::Instance()
		.SetAuthoredComponentPlaybackAdvancing(false);
	RestorePlayModeSceneState();
}

TerraEngine::Runtime::PhysicsInvariantReport MainEditorSubsystem::RunPhysicsExportInvariantFixture()
{
	// The installed editor executes the real serialized startup scene through
	// the same Engine scheduler and PhysicsSystem path as the export launcher.
	using namespace TerraEngine::Physics;
	TerraEngine::Runtime::PhysicsInvariantReport serializedReport;
	if (ctx == nullptr || ctx->engine == nullptr || ctx->projectManager == nullptr
		|| !ctx->projectManager->HasActiveProject()) {
		serializedReport.diagnostic = "The installed editor physics invariant run requires an already-open project.";
		return serializedReport;
	}

	try {
		const std::filesystem::path projectRoot = ctx->projectManager->GetProjectRoot();
		const std::filesystem::path settingsPath = projectRoot / "ProjectSettings" / "PhysicsSettings.json";
		const std::vector<std::uint8_t> settingsBytes = ResourceManager::Instance().ReadFileBytes(settingsPath);
		const nlohmann::json settingsJson = nlohmann::json::parse(settingsBytes.begin(), settingsBytes.end());
		PhysicsSettings settings = PhysicsSettings::Defaults();
		settings.schemaVersion = settingsJson.value("schemaVersion", settings.schemaVersion);
		if (!settingsJson.contains("gravity") || !settingsJson["gravity"].is_array() || settingsJson["gravity"].size() != 3U
			|| !settingsJson.contains("layers") || !settingsJson["layers"].is_array() || settingsJson["layers"].size() != settings.layers.size()) {
			serializedReport.diagnostic = "The serialized fixture PhysicsSettings.json is missing gravity or the complete layer table.";
			return serializedReport;
		}
		settings.gravity = {settingsJson["gravity"][0].get<float>(), settingsJson["gravity"][1].get<float>(), settingsJson["gravity"][2].get<float>()};
		settings.fixedTimeStep = settingsJson.value("fixedTimeStep", settings.fixedTimeStep);
		for (std::size_t index = 0U; index < settings.layers.size(); ++index) {
			const nlohmann::json& source = settingsJson["layers"][index];
			settings.layers[index].id = source.value("id", settings.layers[index].id);
			settings.layers[index].name = source.value("name", settings.layers[index].name);
			settings.layers[index].collisionMask = source.value("collisionMask", settings.layers[index].collisionMask);
			settings.layers[index].broadPhaseGroup = source.value("broadPhaseGroup", settings.layers[index].broadPhaseGroup);
			settings.layers[index].enabled = source.value("enabled", settings.layers[index].enabled);
		}
		if (!settings.Validate().accepted) {
			serializedReport.diagnostic = "The serialized fixture PhysicsSettings.json is invalid.";
			return serializedReport;
		}

		const std::filesystem::path cookedPath = projectRoot / "Build" / "Assets" / "Colliders" / "Startup.tepcoll";
		const std::vector<std::uint8_t> rawCooked = ResourceManager::Instance().ReadFileBytes(cookedPath);
		std::vector<std::byte> cookedBytes(rawCooked.size());
		if (!rawCooked.empty()) std::memcpy(cookedBytes.data(), rawCooked.data(), rawCooked.size());
		CookedColliderDiagnostics cookedDiagnostics;
		const std::optional<CookedColliderAsset> cooked = CookedColliderAsset::Deserialize(cookedBytes, cookedDiagnostics);
		if (!cooked) {
			serializedReport.diagnostic = "The serialized fixture could not load its installed cooked collider envelope.";
			return serializedReport;
		}
		ResourceManager::Instance().RegisterAssetPath(cooked->Envelope().assetId, cookedPath, AssetType::CookedCollider);

		auto& physics = PhysicsSystem::Instance();
		// Queue settings before Play Mode captures authored state. StartPlayMode
		// performs the normal lifecycle start, which applies this validated update
		// at its protected world-creation boundary.
		if (!physics.QueueSettingsUpdate(settings) || !StartPlayMode()) {
			serializedReport.diagnostic = "The serialized fixture could not start editor Play Mode with its project settings.";
			return serializedReport;
		}
		serializedReport = ctx->engine->RunPhysicsInvariantFixture();
		// The common runner stops provider work. This second stop is intentional:
		// it restores the editor's captured pre-play scene snapshot through the
		// normal lifecycle rather than manually clearing ObjectManager state.
		StopPlayMode();
		return serializedReport;
	}
	catch (const std::exception& exception) {
		serializedReport.diagnostic = std::string("Serialized editor physics fixture failed: ") + exception.what();
		StopPlayMode();
		return serializedReport;
	}

}

void MainEditorSubsystem::RefreshViewportTextures(VulkanContext& vulkan)
{
	VulkanContext::EditorHandles handles;
	vulkan.GetEditorHandles(handles);
	const auto progress = vulkan.GetEditorSubmissionProgress();
	const auto registerTexture = [&vulkan](void* imageView, void* sampler) -> void* {
		void* descriptor = nullptr;
		const auto result = TerraEngine::Editor::CheckedImGuiVulkanBridge::Execute(
			TerraEngine::Renderer::Vulkan::EditorUiFrameStage::RecordDrawData,
			[&]() {
				// AddTexture allocates and writes the editor descriptor pool. Join the
				// same renderer-state/queue boundary as ImGui font uploads so shutdown
				// or swapchain replacement cannot overlap this host-side mutation.
				vulkan.RunWithQueueAccess([&]() {
					const VkDescriptorSet set = ImGui_ImplVulkan_AddTexture(
						reinterpret_cast<VkSampler>(sampler),
						reinterpret_cast<VkImageView>(imageView),
						VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
					descriptor = reinterpret_cast<void*>(set);
				});
			});
		return result.success ? descriptor : nullptr;
	};
	if (!viewportTextures.Refresh(TerraEngine::Editor::ViewportTextureKind::Scene,
		handles.sceneImageView, handles.sceneSampler, handles.sceneImageGeneration,
		progress.lastSubmittedSerial, registerTexture)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Scene viewport texture registration is pending; retaining the previous valid generation.");
	} else {
		vulkan.AcknowledgeViewportTextureGeneration(VulkanContext::ViewType::Scene,
			handles.sceneImageGeneration, progress.lastSubmittedSerial);
	}
	if (!viewportTextures.Refresh(TerraEngine::Editor::ViewportTextureKind::Game,
		handles.gameImageView, handles.gameSampler, handles.gameImageGeneration,
		progress.lastSubmittedSerial, registerTexture)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Game viewport texture registration is pending; retaining the previous valid generation.");
	} else {
		vulkan.AcknowledgeViewportTextureGeneration(VulkanContext::ViewType::Game,
			handles.gameImageGeneration, progress.lastSubmittedSerial);
	}
	ReleaseCompletedViewportTextures(progress.completedSerial);
}

void MainEditorSubsystem::CaptureViewportDrawDataUses()
{
	viewportTextures.BeginDrawDataUsage();
	const ImDrawData* drawData = ImGui::GetDrawData();
	if (drawData == nullptr) return;
	for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
		const ImDrawList* list = drawData->CmdLists[listIndex];
		for (const ImDrawCmd& command : list->CmdBuffer) {
			viewportTextures.MarkDrawDataUse(reinterpret_cast<void*>(command.GetTexID()));
		}
	}
}

void MainEditorSubsystem::CommitViewportDrawDataUses(std::uint64_t submittedSerial)
{
	viewportTextures.CommitDrawDataUsage(submittedSerial);
}

void MainEditorSubsystem::AbandonViewportDrawDataUses()
{
	viewportTextures.AbandonDrawDataUsage();
}

void MainEditorSubsystem::ReleaseCompletedViewportTextures(std::uint64_t completedSerial)
{
	RenderSystem* renderSystem = ctx && ctx->engine ? ctx->engine->GetSystem<RenderSystem>() : nullptr;
	auto* vulkan = renderSystem ? dynamic_cast<VulkanContext*>(renderSystem->GetContext()) : nullptr;
	if (!vulkan) return;
	viewportTextures.ReleaseCompleted(completedSerial,
		[&](void* descriptor) { return vulkan->FreeEditorDescriptorSet(descriptor); });
}

void MainEditorSubsystem::ShutdownViewportTextures(bool deviceLost)
{
	RenderSystem* renderSystem = ctx && ctx->engine ? ctx->engine->GetSystem<RenderSystem>() : nullptr;
	auto* vulkan = renderSystem ? dynamic_cast<VulkanContext*>(renderSystem->GetContext()) : nullptr;
	viewportTextures.Shutdown(deviceLost, vulkan
		? TerraEngine::Editor::ViewportTextureRegistry::ReleaseCallback(
			[&](void* descriptor) { return vulkan->FreeEditorDescriptorSet(descriptor); })
		: TerraEngine::Editor::ViewportTextureRegistry::ReleaseCallback{});
}

// MainEditorSubsystem::OnBackendReady is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::OnBackendReady() {
	if (!ctx || !ctx->engine) return;
	
	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) return;

	RHIContext* rhi = renderSystem->GetContext();
	VulkanContext* vulkan = dynamic_cast<VulkanContext*>(rhi);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!vulkan) return;

	RefreshViewportTextures(*vulkan);
}

// MainEditorSubsystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::Update(float deltaTime) {
	PublishCommittedEditorTransactionsToUndo();
	// Advance the scripting coordinator after asset watcher events have been
	// processed so queued C# rebuilds can start and completed async builds can
	// clear their in-flight state during the same editor frame.
	ScriptCompilationSubsystem::Instance().Update(deltaTime);

	// Physics authoring widgets publish value-only requests. This editor update is
	// the protected main-thread handoff: the physics scheduler consumes settings
	// from its one-item queue at the next PhysicsSnapshot boundary, while component
	// edits are rediscovered through the immutable authoring snapshot. No ImGui
	// callback is allowed to rebuild or touch the live provider world directly.
	if (auto projectSettings = GetWindow<ProjectSettingsWindow>())
	{
		const auto& request = projectSettings->GetPendingPhysicsSettingsRequestForTests();
		if (request && TerraEngine::Physics::PhysicsSystem::Instance().QueueSettingsUpdate(request->settings))
		{
			projectSettings->AcknowledgePhysicsSettingsRequest(request->requestSequence);
		}
	}
	if (auto inspector = GetWindow<InspectorWindow>())
	{
		const std::vector<InspectorWindow::PhysicsRebuildIntent> intents =
			inspector->DrainPhysicsRebuildIntentsForProtectedBoundary();
		if (!intents.empty())
		{
			// Capturing all affected component generations in PhysicsSnapshot makes
			// these stable IDs visible atomically; draining merely closes the editor
			// ownership boundary and prevents per-widget requests from accumulating.
			TERRAENGINE_LOG(LogLevel::Debug, "Queued " << intents.size()
				<< " physics authoring rebuild intent(s) for the protected snapshot boundary.");
		}
	}

	// Ensure UI system updates even when the game is not playing so we get live previews.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ScriptingSystem::Instance().IsRunning())
	{
		UISystem::Instance().Update(deltaTime);
	}

	PublishEditorStreamingNeeds();

	// A blocking script compile pauses editor-side interactions until the build
	// either reloads the new assembly or restores the previous working one.
	if (ScriptCompilationSubsystem::Instance().ShouldBlockEditorInteraction()) {
		return;
	}

	// Logic update

	if (ctx != nullptr)
	{
		std::optional<uint32_t> watchedObjectId;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (selectedEntity.has_value())
		{
			watchedObjectId = static_cast<uint32_t>(*selectedEntity);
		}

		ctx->scripting.hasWatchedObject = watchedObjectId.has_value();
		ctx->scripting.watchedObjectId = watchedObjectId.value_or(0);
		ScriptingSystem::Instance().UpdateInspectorWatch(watchedObjectId, ctx->scripting.selectionVersion);
	}

	// Process editor-wide hotkeys.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
		if (ImGui::IsKeyPressed(ImGuiKey_S)) {
			if (m_IsPrefabEditingMode) {
				SaveOpenPrefab();
			} else {
				// The disk write and undo save point form one editor action. Recording
				// the state only after a successful write lets undo return to the exact
				// revision represented by the file on disk.
				if (SceneManager::Instance().SaveScene()) {
					UndoRedoSystem::Instance().SetSavePoint();
				}
			}
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Z)) {
			UndoRedoSystem::Instance().Undo();
		}
		// Traditional Windows redo shortcut (Ctrl+Y).
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_Y)) {
			UndoRedoSystem::Instance().Redo();
		}
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::IsKeyPressed(ImGuiKey_B)) {
			if (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) {
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			} else {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
		}
	}
}

void MainEditorSubsystem::PublishEditorStreamingNeeds()
{
	// This publisher runs once per editor frame and only emits compact need
	// records. ResourceManager owns the synchronized mutation and expiry, so the
	// inspector, project browser, and prefab workflow do not need to duplicate
	// residency bookkeeping in their ImGui draw code.
	if (selectedEntity.has_value())
	{
		if (std::optional<Object> object = ObjectManager::instance().getObject(*selectedEntity))
		{
			PublishMeshRenderAssetNeeds(*object, TerraEngine::Resource::StreamingNeedType::EditorSelection, "editor selected object");
			PublishMeshRenderAssetNeeds(*object, TerraEngine::Resource::StreamingNeedType::InspectorPreview, "inspector selected object preview");
		}
	}

	if (selectedAsset.has_value())
	{
		const std::filesystem::path selectedPath = std::filesystem::absolute(*selectedAsset).lexically_normal();
		if (auto resolvedAsset = ResolveEditorAssetPath(selectedPath))
		{
			ResourceManager::Instance().MarkStreamingNeed(
				resolvedAsset->first,
				resolvedAsset->second,
				TerraEngine::Resource::StreamingNeedType::AssetPreview,
				0u,
				"project browser selected asset preview",
				70);
		}
		else if (selectedPath.extension() == ".teprefab")
		{
			PublishPrefabAssetPreviewNeeds(selectedPath);
		}
	}

	if (m_IsPrefabEditingMode)
	{
		ObjectManager::instance().forEachObjectWithComponents<MeshRenderComponent>([](Object object, MeshRenderComponent&) {
			PublishMeshRenderAssetNeeds(object, TerraEngine::Resource::StreamingNeedType::PrefabEditing, "open prefab editing session");
		});
	}
}

// MainEditorSubsystem::SetOnProjectClosedCallback is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetOnProjectClosedCallback(std::function<void()> callback) {
	onProjectClosed = callback;
}

// MainEditorSubsystem::SetSelectedEntity is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedEntity(std::optional<entt::entity> entity)
{
	selectedEntity = std::nullopt;
	selectedEntityUuids.clear();
	if (entity)
	{
		if (const std::optional<Object> object = ObjectManager::instance().getObject(*entity))
		{
			selectedEntity = entity;
			selectedEntityUuids.push_back(ObjectManager::instance().getUUID(*object));
		}
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (selectedEntity)
	{
		selectedAsset = std::nullopt;
		selectedPackageChildAsset = std::nullopt;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = selectedEntity.has_value();
		ctx->scripting.watchedObjectId = selectedEntity.has_value() ? static_cast<std::uint32_t>(*selectedEntity) : 0;
	}
}

std::vector<entt::entity> MainEditorSubsystem::GetSelectedEntities() const
{
	std::vector<entt::entity> entities;
	entities.reserve(selectedEntityUuids.size());
	for (const TerraEngine::UUID objectId : selectedEntityUuids)
	{
		if (const std::optional<Object> object = ObjectManager::instance().getObject(objectId))
		{
			entities.push_back(object->EntityId());
		}
	}
	return entities;
}

std::optional<entt::entity> MainEditorSubsystem::GetSelectedEntity() const
{
	if (selectedEntityUuids.empty()) return std::nullopt;
	if (const std::optional<Object> object = ObjectManager::instance().getObject(selectedEntityUuids.back()))
	{
		return object->EntityId();
	}
	return std::nullopt;
}

bool MainEditorSubsystem::IsEntitySelected(entt::entity entity) const
{
	const std::optional<Object> object = ObjectManager::instance().getObject(entity);
	if (!object)
	{
		return false;
	}
	const TerraEngine::UUID objectId = ObjectManager::instance().getUUID(*object);
	return std::find(selectedEntityUuids.begin(), selectedEntityUuids.end(), objectId) != selectedEntityUuids.end();
}

void MainEditorSubsystem::SetSelectedEntities(const std::vector<entt::entity>& entities)
{
	TerraEngine::Editor::StableEditorSelection selection = TerraEngine::Editor::BuildStableEditorSelection(entities);
	selectedEntityUuids = std::move(selection.objectIds);
	selectedEntity = selection.primaryEntity;
	if (selectedEntity)
	{
		selectedAsset = std::nullopt;
		selectedPackageChildAsset = std::nullopt;
	}
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = selectedEntity.has_value();
		ctx->scripting.watchedObjectId = selectedEntity ? static_cast<std::uint32_t>(*selectedEntity) : 0U;
	}
}

void MainEditorSubsystem::ToggleSelectedEntity(entt::entity entity)
{
	std::vector<entt::entity> entities = GetSelectedEntities();
	const auto existing = std::find(entities.begin(), entities.end(), entity);
	if (existing == entities.end())
	{
		entities.push_back(entity);
	}
	else
	{
		entities.erase(existing);
	}
	SetSelectedEntities(entities);
}

void MainEditorSubsystem::CapturePendingEditorTransactionsForUndo()
{
	for (const TerraEngine::Scene::Transaction& transaction : TerraEngine::Scene::SceneMutationQueue::Instance().PendingTransactions())
	{
		if (transaction.origin != TerraEngine::Scene::TransactionOrigin::Editor || transaction.undoGroupName.empty())
		{
			continue;
		}
		const bool physicsPropertyBatch = !transaction.writeSet.operations.empty()
			&& std::all_of(transaction.writeSet.operations.begin(), transaction.writeSet.operations.end(), [](const TerraEngine::Scene::TransactionWriteOperation& operation) {
				return operation.kind == TerraEngine::Scene::TransactionWriteKind::UpdateComponentProperty
					&& (operation.componentName == "RigidBodyComponent"
						|| operation.componentName == "ColliderSetComponent"
						|| operation.componentName == "ConstraintSetComponent"
						|| operation.componentName == "CharacterControllerComponent");
			});
		if (physicsPropertyBatch)
		{
			pendingEditorUndoTransactions.try_emplace(transaction.id, transaction);
		}
	}
}

void MainEditorSubsystem::PublishCommittedEditorTransactionsToUndo()
{
	// Full editor receipts are retained separately from diagnostics because the
	// diagnostic ring intentionally strips inverse payloads. Consuming the full
	// receipt is what makes ColliderSet and other physics edits enter history.
	const std::vector<TerraEngine::Scene::TransactionResult> completedResults =
		TerraEngine::Scene::SceneMutationQueue::Instance().DrainCompletedEditorTransactionResults();
	for (const TerraEngine::Scene::TransactionResult& result : completedResults)
	{
		// Undo/redo transactions already own a history entry. Their receipt only
		// finalizes that entry and restores the dirty flag after SceneManager's
		// protected flush marked the scene changed.
		if (UndoRedoSystem::Instance().CompleteDeferredReplay(result))
		{
			continue;
		}
		const auto pending = pendingEditorUndoTransactions.find(result.transactionId);
		if (pending == pendingEditorUndoTransactions.end())
		{
			continue;
		}
		if (result.status == TerraEngine::Scene::TransactionStatus::Committed)
		{
			UndoRedoSystem::Instance().RecordCommittedTransaction(pending->second, result);
		}
		else
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Editor physics transaction '" << result.transactionId
				<< "' was rejected; no undo history entry was created.");
		}
		pendingEditorUndoTransactions.erase(pending);
	}
}

// MainEditorSubsystem::SetSelectedAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::SetSelectedAsset(std::optional<std::filesystem::path> asset)
{
	selectedAsset = asset;
	selectedPackageChildAsset = std::nullopt;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (asset)
	{
		selectedEntity = std::nullopt;
		selectedEntityUuids.clear();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
	}
}

bool MainEditorSubsystem::OpenAnimationAssetEditor(const std::filesystem::path& path)
{
	const std::string extension = path.extension().string();
	const char* title = nullptr;
	if(extension == ".teskel") title = "Skeleton Editor";
	else if(extension == ".teanim") title = "Animation Clip Editor";
	else if(extension == ".teanimctrl") title = "Animation Controller";
	else if(extension == ".teretarget") title = "Retarget Editor";
	else if(extension == ".temotiondb") title = "Motion Database";
	else if(extension == ".teragdoll") title = "Ragdoll Editor";
	else if(extension == ".teanimmask") {
		// Masks currently use the selected-asset inspector; there is no standalone
		// mask window to reveal. Selection still performs a complete Open action.
		SetSelectedAsset(std::filesystem::absolute(path));
		return true;
	}
	else return false;

	SetSelectedAsset(std::filesystem::absolute(path));
	for(const std::shared_ptr<EditorWindow>& window : m_Windows) {
		if(window && window->GetTitle() == title) {
			window->SetVisible(true);
			return true;
		}
	}
	return false;
}

bool MainEditorSubsystem::RebuildAssetFromInspector(const std::filesystem::path& path, std::string* outError) {
	const std::filesystem::path absolutePath = std::filesystem::absolute(path);
	if (!std::filesystem::exists(absolutePath)) {
		if (outError != nullptr) {
			*outError = "Source asset does not exist.";
		}
		return false;
	}

	// Reuse the asset database import boundary so Inspector-triggered rebuilds
	// follow the same atomic compile, sidecar, ResourceManager, and renderer
	// reload behavior as Project Browser and file-watcher imports.
	AssetDatabase::Instance().ImportAssetWithProgress(absolutePath, true);
	return true;
}

// MainEditorSubsystem::SetSelectedPackageChildAsset records a read-only generated child selection from a model package.
void MainEditorSubsystem::SetSelectedPackageChildAsset(std::optional<SelectedPackageChildAsset> asset)
{
	selectedPackageChildAsset = asset;
	selectedAsset = asset ? std::optional<std::filesystem::path>(asset->packagePath) : std::nullopt;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (asset)
	{
		selectedEntity = std::nullopt;
		selectedEntityUuids.clear();
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
	}
}

// MainEditorSubsystem::IsInspectorValueDragging is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::IsInspectorValueDragging() const {
	auto inspector = const_cast<MainEditorSubsystem*>(this)->GetWindow<InspectorWindow>();
	if (inspector) return inspector->IsInspectorValueDragging();
	return false;
}

// MainEditorSubsystem::TryGetGameViewRenderStats is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool MainEditorSubsystem::TryGetGameViewRenderStats(RenderStatsSnapshot& outStats) const {
	if (!ctx || !ctx->engine) {
		return false;
	}

	RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!renderSystem) {
		return false;
	}

	outStats = renderSystem->GetLatestRenderStats(RenderViewType::Game);
	return outStats.valid;
}

void MainEditorSubsystem::OpenProjectSettings(TerraEngine::Editor::ProjectSettings::ProjectSettingsCategoryId categoryId)
{
	if (auto projectSettings = GetWindow<ProjectSettingsWindow>())
	{
		projectSettings->OpenToCategory(categoryId);
	}
}

void MainEditorSubsystem::OpenPreferences(TerraEngine::Editor::Preferences::PreferencesCategoryId categoryId)
{
	if (auto preferences = GetWindow<PreferencesWindow>())
	{
		preferences->OpenToCategory(categoryId);
	}
}

void MainEditorSubsystem::DrawProjectHubWindows()
{
	// Preferences owns only user/editor state, so it is safe to draw without an
	// active project. Do not iterate every registered editor window here because
	// most of them read project, scene, asset, or renderer-owned workspace data.
	if (auto preferences = GetWindow<PreferencesWindow>())
	{
		preferences->Draw();
	}
}

// MainEditorSubsystem::DrawUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawUI() {
	EnsurePrefabObjectsLoadedForEditor();

	// Check for resize/recreation of offscreen resources
	if (ctx && ctx->engine) {
		RenderSystem* renderSystem = ctx->engine->GetSystem<RenderSystem>();
		if (renderSystem) {
			// Asset-style prefab editing is an isolated authoring mode, not a
			// gameplay preview. Suppressing the next Game View render avoids a
			// hidden/blank offscreen pass while Scene View remains responsive.
			renderSystem->SetGameViewSuppressed(IsAssetPrefabEditingMode());
			VulkanContext* vulkan = dynamic_cast<VulkanContext*>(renderSystem->GetContext());
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (vulkan) {
				RefreshViewportTextures(*vulkan);
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

		const float availWidth = ImGui::GetContentRegionAvail().x;
		const float buttonSize = 20.0f;
		const float buttonSpacing = ImGui::GetStyle().ItemSpacing.x;
		const float totalWidth = (buttonSize * 3.0f) + (buttonSpacing * 2.0f);
		ImGui::SetCursorPosX(std::max(0.0f, (availWidth - totalWidth) * 0.5f));

		const bool isRunning = ScriptingSystem::Instance().IsRunning();
		const bool isPaused = ScriptingSystem::Instance().IsPaused();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (DrawPlayModeToolbarIconButton(
			"play_mode_play_stop",
			isRunning ? PlayModeToolbarIcon::Stop : PlayModeToolbarIcon::Play,
			isRunning,
			isRunning ? "Stop" : "Play",
			buttonSize)) {
			if (!isRunning) {
				if (!StartPlayMode()) {
					TERRAENGINE_LOG(LogLevel::Error, "Play Mode did not start because the Jolt 5.6 physics world could not be created.");
				}
			} else {
				StopPlayMode();
			}
		}
		ImGui::SameLine();

		if (DrawPlayModeToolbarIconButton(
			"play_mode_pause",
			PlayModeToolbarIcon::Pause,
			isPaused,
			isPaused ? "Resume" : "Pause",
			buttonSize)) {
			(void)SetPlayModePaused(!isPaused);
		}
		ImGui::SameLine();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (DrawPlayModeToolbarIconButton("play_mode_next", PlayModeToolbarIcon::Next, false, "Next", buttonSize)) {
			(void)StepPlayMode();
		}

		ImGui::PopStyleVar(2);
	}

	ImGuiID dockspace_id = ImGui::GetID("MyDockSpace");
	ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);

	if (m_IsPrefabEditingMode && !m_PrefabObjectsLoadedForEditor)
	{
		// Runtime rendering keeps the scene snapshot loaded after each frame in
		// scene-context prefab mode. Reload the isolated prefab snapshot before
		// editor UI windows read hierarchy/inspector state so those windows do
		// not flicker between scene objects and prefab objects.
		EnsurePrefabObjectsLoadedForEditor();
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (auto& win : m_Windows) {
		const bool drawSceneViewInSceneContext = ShouldSceneViewUseSceneContextForPrefabEdit()
			&& std::dynamic_pointer_cast<SceneViewWindow>(win) != nullptr
			&& m_PrefabObjectsLoadedForEditor;
		if (drawSceneViewInSceneContext)
		{
			// Scene-context prefab editing should look like Unity's in-context
			// mode: Scene View uses the real scene snapshot, while the hierarchy
			// and inspector keep editing the isolated prefab data.
			CaptureCurrentPrefabEditingState();
			LoadPrefabReturnSceneSnapshotIntoObjectManager(true);
			m_PrefabObjectsLoadedForEditor = false;
			{
				TERRAENGINE_PROFILE_TASK_SCOPE("EditorWindow::Draw", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, win->GetTitle().c_str());
				win->Draw();
			}
			EnsurePrefabObjectsLoadedForEditor();
		}
		else
		{
			if (m_IsPrefabEditingMode && !m_PrefabObjectsLoadedForEditor)
			{
				EnsurePrefabObjectsLoadedForEditor();
			}
			TERRAENGINE_PROFILE_TASK_SCOPE("EditorWindow::Draw", TerraEngine::Profiling::ProfilerModuleId::CpuEngine, win->GetTitle().c_str());
			win->Draw();
		}
	}
	// Windows enqueue editor transactions during ImGui. Capture their immutable
	// forward payload now, before the scheduler consumes them next frame.
	CapturePendingEditorTransactionsForUndo();

	DrawSaveConfirmationModal();
	DrawImportCloseConfirmationModal();
	DrawScriptCompilationModal();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (showDemoWindow) ImGui::ShowDemoWindow(&showDemoWindow);

	ImGui::End();
	RestoreSceneObjectsForRuntimeDuringPrefabEdit();
}

// MainEditorSubsystem::DrawMainMenuBar is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawMainMenuBar() {
	if (ImGui::BeginMenuBar()) {
		if (ImGui::BeginMenu("File")) {
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Save Scene", "Ctrl+S")) {
				const bool saved = m_IsPrefabEditingMode ? SaveOpenPrefab() : SceneManager::Instance().SaveScene(m_CurrentScenePath);
				if (saved && !m_IsPrefabEditingMode) {
					UndoRedoSystem::Instance().SetSavePoint();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build Profiles...", "Ctrl+Shift+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
				}
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Build and Run", "Ctrl+B")) {
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (auto win = GetWindow<BuildProfilesWindow>()) {
					win->SetVisible(true);
					win->StartExportAndRun();
				}
			}
			ImGui::Separator();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Close Project")) {
				RequestCloseProject();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Exit")) {
				RequestExit();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Edit")) {
			// Context-aware menu enabling ensures users know when history is 
			// available or exhausted.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Undo", "Ctrl+Z", false, UndoRedoSystem::Instance().CanUndo())) {
				UndoRedoSystem::Instance().Undo();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Redo", "Ctrl+Y", false, UndoRedoSystem::Instance().CanRedo())) {
				UndoRedoSystem::Instance().Redo();
			}
			ImGui::Separator();
			// Project Settings lives under Edit because it changes persistent
			// project configuration rather than opening another transient tool.
			if (ImGui::MenuItem(TerraEngine::Editor::ProjectSettings::kProjectSettingsMenuLabel.data())) {
				OpenProjectSettings();
			}
			// Preferences also lives under Edit, but it changes editor-wide user
			// defaults rather than project/runtime configuration.
			if (ImGui::MenuItem(TerraEngine::Editor::Preferences::kPreferencesMenuLabel.data())) {
				OpenPreferences();
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("View")) {
			ImGui::MenuItem("ImGui Demo", nullptr, &showDemoWindow);
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Scripting")) {
			// Keep script actions reachable from the menu, but leave build status
			// text out of the chrome so attaching scripts stays drag-and-drop only.
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Open Workspace")) {
				ScriptCompilationSubsystem::Instance().OpenWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Regenerate Workspace")) {
				ScriptCompilationSubsystem::Instance().RegenerateWorkspace();
			}
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ImGui::MenuItem("Rebuild Scripts")) {
				ScriptCompilationSubsystem::Instance().RequestBuild(ScriptBuildReason::ManualRefresh);
			}
			ImGui::EndMenu();
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginMenu("Windows")) {
			// Categories and nesting come entirely from each window's declared
			// menu path. This renderer contains no list of category names.
			const std::vector<WindowMenuNode> nativeMenuTree = BuildWindowMenuTree(m_Windows);
			DrawWindowMenuNodes(nativeMenuTree);
			
			if (!nativeMenuTree.empty()) {
				ImGui::Separator();
			}

			EditorSystem* editorSystem = nullptr;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) {
				editorSystem = dynamic_cast<EditorSystem*>(ctx->engine->GetEditorSystem());
			}
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (editorSystem) {
				editorSystem->DrawWindowMenu();
			}
			
			ImGui::EndMenu();
		}
		ImGui::EndMenuBar();
	}
}

// MainEditorSubsystem::RequestOpenScene is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestOpenScene(const std::filesystem::path& path) {
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::OpenScene;
		m_PendingScenePath = path;
		m_ShowSaveConfirmation = true;
	} else {
		OpenScene(path);
	}
}

// MainEditorSubsystem::RequestCloseProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestCloseProject() {
	if (AssetDatabase::Instance().IsCompiling()) {
		m_PendingAction = PendingAction::CloseProject;
		m_ShowImportCloseConfirmation = true;
		return;
	}
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::CloseProject;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
	}
}

// MainEditorSubsystem::RequestExit is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::RequestExit() {
	if (AssetDatabase::Instance().IsCompiling()) {
		m_PendingAction = PendingAction::Exit;
		m_ShowImportCloseConfirmation = true;
		return;
	}
	if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
		m_PendingAction = PendingAction::Exit;
		m_ShowSaveConfirmation = true;
	} else {
		CloseCurrentProject();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ctx && ctx->engine) ctx->engine->SignalShutdown();
	}
}

// MainEditorSubsystem::ExecutePendingAction is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::ExecutePendingAction() {
	switch (m_PendingAction) {
		case PendingAction::OpenScene:
			OpenScene(m_PendingScenePath);
			break;
		case PendingAction::CloseProject:
			CloseCurrentProject();
			break;
		case PendingAction::Exit:
			CloseCurrentProject();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (ctx && ctx->engine) ctx->engine->SignalShutdown();
			break;
		default:
			break;
	}
	m_PendingAction = PendingAction::None;
	m_PendingScenePath.clear();
}

// MainEditorSubsystem::DrawSaveConfirmationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawSaveConfirmationModal() {
	if (m_ShowSaveConfirmation) {
		ImGui::OpenPopup("Save Changes?");
		m_ShowSaveConfirmation = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Save Changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("There are unsaved changes to the current scene or prefab.\nDo you want to save them?");
		ImGui::Separator();

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Yes", ImVec2(120, 0))) {
			if (m_IsPrefabEditingMode) {
				const bool restoreSceneNeedsSave = SceneManager::Instance().IsActiveSceneDirty();
				SaveOpenPrefab();
				ClosePrefabMode();
				if (restoreSceneNeedsSave) {
					if (SceneManager::Instance().SaveScene(m_CurrentScenePath)) {
						UndoRedoSystem::Instance().SetSavePoint();
					}
				}
			} else {
				if (SceneManager::Instance().SaveScene()) {
					UndoRedoSystem::Instance().SetSavePoint();
				}
			}
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("No", ImVec2(120, 0))) {
			if (m_IsPrefabEditingMode) {
				m_PrefabEditingDirty = false;
			}
			ExecutePendingAction();
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_PendingAction = PendingAction::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

void MainEditorSubsystem::DrawImportCloseConfirmationModal() {
	if (m_ShowImportCloseConfirmation) {
		ImGui::OpenPopup("Import In Progress");
		m_ShowImportCloseConfirmation = false;
	}

	if (ImGui::BeginPopupModal("Import In Progress", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		const size_t jobCount = AssetDatabase::Instance().GetCompileJobsCount();
		ImGui::TextWrapped("Asset import is still running (%zu job%s). Closing now will cancel active imports and remove partial imported assets from the project.",
			jobCount,
			jobCount == 1u ? "" : "s");
		ImGui::Separator();

		if (ImGui::Button("Continue Closing", ImVec2(150, 0))) {
			AssetDatabase::Instance().RequestCancelAllImports(true);
			ImGui::CloseCurrentPopup();
			if (SceneManager::Instance().IsActiveSceneDirty() || (m_IsPrefabEditingMode && m_PrefabEditingDirty)) {
				m_ShowSaveConfirmation = true;
			} else {
				ExecutePendingAction();
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0))) {
			m_PendingAction = PendingAction::None;
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::DrawScriptCompilationModal is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::DrawScriptCompilationModal() {
	const ScriptCompilationSubsystem& scriptCompilation = ScriptCompilationSubsystem::Instance();
	const ScriptCompilationStatus& scriptStatus = scriptCompilation.GetStatus();
	const bool isCompilingScripts = scriptCompilation.IsBusy();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (isCompilingScripts) {
		ImGui::OpenPopup("Compiling C# Scripts");
	}

	ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopupModal("Compiling C# Scripts", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!isCompilingScripts) {
			ImGui::CloseCurrentPopup();
			ImGui::EndPopup();
			return;
		}

		ImGui::TextWrapped("TerraEngine paused gameplay script execution while Assembly-CSharp recompiles.");
		ImGui::Spacing();
		ImGui::TextUnformatted(scriptStatus.lastSummary.c_str());
		ImGui::Spacing();
		ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.8f, 1.0f));
		const char* progressLabel = scriptStatus.state == ScriptCoordinatorState::Reloading ? "Reloading..." : "Compiling...";
		// Use a negative fraction animated by ImGui time to render an
		// indeterminate progress bar while the build runs in the background.
		ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(360.0f, 0.0f), progressLabel);
		ImGui::PopStyleColor();
		ImGui::EndPopup();
	}
}

// MainEditorSubsystem::CloseCurrentProject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void MainEditorSubsystem::CloseCurrentProject() {
	// Tear down provider state before scene unload and ObjectManager clearing.
	TerraEngine::Physics::PhysicsSystem::Instance().StopWorld();
	ScriptingSystem::Instance().SetRunning(false);
	TerraEngine::Animation::AnimationSystem::Instance()
		.SetAuthoredComponentPlaybackEnabled(false);
	ScriptingSystem::SetScriptingDiagnosticsRequested(false);
	TerraEngine::Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
	ClearPlayModeSceneState();
	ScriptCompilationSubsystem::Instance().OnProjectClosed();
	m_IsPrefabEditingMode = false;
	m_PrefabEditingDirty = false;
	m_HasPrefabReturnSceneSnapshot = false;
	m_PrefabReturnSceneSnapshotJson.clear();
	m_PrefabReturnSelectedEntityUuid.reset();
	m_PrefabReturnGameViewCameras.clear();
	m_PrefabEditingPath.clear();
	m_PrefabObjectsLoadedForEditor = false;
	m_PrefabEditOpenedFromSceneObject = false;
	m_PrefabEditingObjectSnapshotJson.clear();
	m_PrefabEditingSelectedEntityUuid.reset();

	// Clear Scene
	SceneManager::Instance().UnloadAll();
	
	// Clear Objects
	ObjectManager::instance().clear();

	// Clear Undo History
	UndoRedoSystem::Instance().Clear();
	
	// Clear Resources
	ResourceManager::Instance().ClearCaches();
	ResourceManager::Instance().LoadDefaultResources();
	
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
	selectedEntityUuids.clear();
	pendingEditorUndoTransactions.clear();
	selectedAsset = std::nullopt;
	if (ctx != nullptr)
	{
		ctx->scripting.selectionVersion++;
		ctx->scripting.hasWatchedObject = false;
		ctx->scripting.watchedObjectId = 0;
		ScriptingSystem::Instance().UpdateInspectorWatch(std::nullopt, ctx->scripting.selectionVersion);
	}
	
	// Trigger callbacks BEFORE closing the project in ProjectManager
	// so that subsystems can save data to the project directory.
	if (ctx && ctx->editorProfiler) {
		ctx->editorProfiler->OnProjectClosed();
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (onProjectClosed) onProjectClosed();

	// Close Project
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ctx && ctx->projectManager) {
		ctx->projectManager->CloseProject();
	}
}
