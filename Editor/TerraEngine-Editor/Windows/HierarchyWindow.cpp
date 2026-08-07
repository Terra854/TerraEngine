// HierarchyWindow.cpp — Scene Hierarchy Window Implementation
//
// Implements the scene hierarchy interface using ImGui tree nodes to visualize the entity-component structure.
// It handles user interactions for entity selection, drag-and-drop parenting, and context menu actions.

#include "HierarchyWindow.h"
#include "../Subsystems/MainEditorSubsystem.h"
#include "../Subsystems/UndoRedoSystem.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/PrefabLinkComponent.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/Serializer.h"
#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>

namespace
{
std::string MakePrefabFileStem(const std::string& objectName)
{
	std::string result;
	result.reserve(objectName.size());
	for (char character : objectName)
	{
		const unsigned char value = static_cast<unsigned char>(character);
		if (std::isalnum(value) || character == '_' || character == '-' || character == ' ')
		{
			result.push_back(character);
		}
	}

	if (result.empty())
	{
		return "New Prefab";
	}
	return result;
}

std::filesystem::path MakeUniquePrefabPath(const std::filesystem::path& prefabDirectory, const std::string& baseName)
{
	std::filesystem::path candidatePath = prefabDirectory / (baseName + ".teprefab");
	int duplicateIndex = 1;
	while (std::filesystem::exists(candidatePath))
	{
		candidatePath = prefabDirectory / (baseName + " (" + std::to_string(duplicateIndex++) + ").teprefab");
	}
	return candidatePath;
}

void CollectPrefabObjectIds(const nlohmann::json& objectJson, std::unordered_set<TerraEngine::UUID>& objectIds)
{
	if (const auto uuidIt = objectJson.find("uuid"); uuidIt != objectJson.end() && uuidIt->is_string())
	{
		if (const auto parsed = TerraEngine::ParseUUID(uuidIt->get<std::string>()))
		{
			objectIds.insert(*parsed);
		}
	}

	if (const auto childrenIt = objectJson.find("children"); childrenIt != objectJson.end() && childrenIt->is_array())
	{
		for (const nlohmann::json& childJson : *childrenIt)
		{
			CollectPrefabObjectIds(childJson, objectIds);
		}
	}
}

std::unordered_set<TerraEngine::UUID> LoadPrefabSourceObjectIds(const PrefabLinkComponent& link)
{
	std::unordered_set<TerraEngine::UUID> objectIds;
	if (link.prefabPath.empty())
	{
		return objectIds;
	}

	std::filesystem::path prefabPath = link.prefabPath;
	if (!prefabPath.is_absolute())
	{
		prefabPath = SceneManager::Instance().GetSceneRoot() / prefabPath;
	}

	std::ifstream prefabInput(prefabPath, std::ios::binary);
	if (!prefabInput)
	{
		return objectIds;
	}

	nlohmann::json prefabJson;
	try
	{
		prefabInput >> prefabJson;
	}
	catch (const nlohmann::json::parse_error&)
	{
		return objectIds;
	}

	if (const auto objectsIt = prefabJson.find("objects"); objectsIt != prefabJson.end() && objectsIt->is_array())
	{
		for (const nlohmann::json& objectJson : *objectsIt)
		{
			CollectPrefabObjectIds(objectJson, objectIds);
		}
	}

	return objectIds;
}
}

bool HierarchyWindow::AcceptPrefabAssetDrop(std::optional<entt::entity> parent)
{
	if (m_Owner == nullptr)
	{
		return false;
	}

	if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_PREFAB"))
	{
		IM_ASSERT(payload->DataSize == sizeof(TerraEngine::UUID));
		const TerraEngine::UUID prefabGuid = *static_cast<const TerraEngine::UUID*>(payload->Data);
		// Keep hierarchy creation behind MainEditorSubsystem so the same
		// prefab GUID payload can later feed inspector/script fields without
		// every target reimplementing path lookup and prefab link setup.
		return m_Owner->InstantiatePrefabAsset(prefabGuid, parent);
	}
	return false;
}

HierarchyWindow::HierarchyWindow(MainEditorSubsystem* owner)
	: EditorWindow("Scene Hierarchy", true, "General"), m_Owner(owner) {
}

void HierarchyWindow::RefreshRootEntityCache(ObjectManager& objectManager, bool prefabEditingMode)
{
	const std::uint64_t hierarchyRevision = objectManager.GetHierarchyRevision();
	const std::uint64_t sceneGeneration = objectManager.GetSceneGeneration();
	bool cacheIsUsable = sceneGeneration == m_CachedSceneGeneration
		&& hierarchyRevision == m_CachedHierarchyRevision
		&& prefabEditingMode == m_CachedPrefabEditingMode;
	if (cacheIsUsable)
	{
		// Prefab context swaps can replace EnTT handles without changing this
		// window's state. Checking the stable endpoints avoids turning a flat
		// ten-thousand-body cache hit back into a full per-frame root traversal.
		if (!m_CachedRootEntities.empty()
			&& (!objectManager.getObject(m_CachedRootEntities.front())
				|| !objectManager.getObject(m_CachedRootEntities.back())))
		{
			cacheIsUsable = false;
		}
	}
	if (cacheIsUsable)
	{
		return;
	}

	m_CachedRootEntities.clear();
	std::vector<Object> roots = objectManager.getRootObjects();
	m_CachedRootEntities.reserve(roots.size());
	for (const Object& root : roots)
	{
		m_CachedRootEntities.push_back(root.EntityId());
	}

	m_CachedHierarchyRevision = hierarchyRevision;
	m_CachedSceneGeneration = sceneGeneration;
	m_CachedPrefabEditingMode = prefabEditingMode;
}

void HierarchyWindow::DrawCachedRootNodes()
{
	RebuildVisibleRowCache(ObjectManager::instance(), m_CachedPrefabEditingMode);
	ImGuiListClipper clipper;
	// Supply the fixed tree-row height instead of asking ImGui to infer it from
	// the first cached object. This remains safe if a scene swap lands precisely
	// between cache preparation and drawing and makes that object unavailable.
	clipper.Begin(
		static_cast<int>(m_CachedVisibleRows.size()),
		ImGui::GetFrameHeightWithSpacing());
	while (clipper.Step())
	{
		for (int index = clipper.DisplayStart; index < clipper.DisplayEnd; ++index)
		{
			DrawVisibleRow(m_CachedVisibleRows[static_cast<std::size_t>(index)]);
		}
	}
}

void HierarchyWindow::RebuildVisibleRowCache(ObjectManager& objectManager, bool prefabEditingMode)
{
	const std::uint64_t hierarchyRevision = objectManager.GetHierarchyRevision();
	const std::uint64_t sceneGeneration = objectManager.GetSceneGeneration();
	if (m_CachedVisibleSceneGeneration == sceneGeneration
		&& m_CachedVisibleHierarchyRevision == hierarchyRevision
		&& m_CachedVisiblePrefabEditingMode == prefabEditingMode
		&& m_CachedSearchFilter == m_SearchFilter
		&& m_CachedExpansionRevision == m_ExpansionRevision)
	{
		return;
	}

	m_CachedVisibleRows.clear();
	m_CachedVisibleSceneGeneration = sceneGeneration;
	m_CachedVisibleHierarchyRevision = hierarchyRevision;
	m_CachedVisiblePrefabEditingMode = prefabEditingMode;
	m_CachedSearchFilter = m_SearchFilter;
	m_CachedExpansionRevision = m_ExpansionRevision;
	const std::string filter = m_SearchFilter;
	std::string loweredFilter = filter;
	std::transform(loweredFilter.begin(), loweredFilter.end(), loweredFilter.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	const auto matches = [&](const Object& object) {
		if (filter.empty()) return true;
		std::string name = objectManager.getObjectName(object);
		std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		return name.find(loweredFilter) != std::string::npos;
	};

	// Build a depth-first list. Search expands ancestors without changing the
	// user's persisted expansion map; ordinary views only descend expanded nodes.
	std::function<bool(entt::entity, std::uint32_t,
		const std::unordered_set<TerraEngine::UUID>*)> appendVisible =
		[&](entt::entity entity, std::uint32_t depth,
			const std::unordered_set<TerraEngine::UUID>* prefabSourceObjectIds) {
			const std::optional<Object> object = objectManager.getObject(entity);
			if (!object) return false;
			const PrefabLinkComponent* prefabLink = objectManager.GetComponent<PrefabLinkComponent>(*object);
			std::unordered_set<TerraEngine::UUID> localPrefabSourceObjectIds;
			const std::unordered_set<TerraEngine::UUID>* activePrefabSourceObjectIds =
				prefabSourceObjectIds;
			if (prefabLink != nullptr)
			{
				localPrefabSourceObjectIds = LoadPrefabSourceObjectIds(*prefabLink);
				activePrefabSourceObjectIds = &localPrefabSourceObjectIds;
			}
			const bool prefabAuthored = prefabLink != nullptr
				|| (activePrefabSourceObjectIds != nullptr
					&& activePrefabSourceObjectIds->contains(objectManager.getUUID(*object)));
			const std::vector<Object> children = objectManager.getChildren(*object);
			const std::string uuid = TerraEngine::ToHexString(objectManager.getUUID(*object));
			const bool expanded = filter.empty() ? m_ExpandedByUuid[uuid] : true;
			const bool selfMatch = matches(*object);
			std::vector<VisibleHierarchyRow> descendants;
			const std::size_t before = m_CachedVisibleRows.size();
			if (expanded)
			{
				for (const Object& child : children)
					appendVisible(child.EntityId(), depth + 1u, activePrefabSourceObjectIds);
				descendants.assign(m_CachedVisibleRows.begin() + static_cast<std::ptrdiff_t>(before), m_CachedVisibleRows.end());
				m_CachedVisibleRows.resize(before);
			}
			const bool childMatch = !descendants.empty();
			if (!filter.empty() && !selfMatch && !childMatch) return false;
			m_CachedVisibleRows.push_back({entity, depth, children.empty(), prefabAuthored});
			m_CachedVisibleRows.insert(m_CachedVisibleRows.end(), descendants.begin(), descendants.end());
			return selfMatch || childMatch;
		};

	// The cache is intentionally rebuilt with a simple recursive walk. Rendering
	// remains bounded by the clipper, while hierarchy and search state stay stable.
	m_CachedVisibleRows.clear();
	for (const entt::entity root : m_CachedRootEntities)
	{
		appendVisible(root, 0u, nullptr);
	}
}

void HierarchyWindow::DrawVisibleRow(const VisibleHierarchyRow& row)
{
	// A flattened list no longer inherits ImGui's recursive indentation stack.
	// Apply the cached depth only around the visible row so clipped descendants
	// retain the same hierarchy presentation without drawing their ancestors.
	const float indentation = static_cast<float>(row.depth) * ImGui::GetTreeNodeToLabelSpacing();
	if (indentation > 0.0f)
	{
		ImGui::Indent(indentation);
	}
	DrawEntityNode(row.entity, nullptr, row.knownLeaf, nullptr, false, row.prefabAuthored);
	if (indentation > 0.0f)
	{
		ImGui::Unindent(indentation);
	}
}

// HierarchyWindow::OnGui is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void HierarchyWindow::OnGui() {
	auto& objManager = ObjectManager::instance();

	if (m_Owner->IsPrefabEditingMode()) {
		ImGui::Text("Prefab: %s%s",
			m_Owner->GetPrefabEditingPath().filename().string().c_str(),
			m_Owner->IsPrefabEditingDirty() ? "*" : "");
		if (ImGui::Button("Save Prefab")) {
			m_Owner->SaveOpenPrefab();
		}
		ImGui::SameLine();
		if (ImGui::Button("Back To Scene")) {
			m_Owner->ClosePrefabMode();
			return;
		}
		ImGui::Separator();
	}
	
	// Header: Add Button & Search
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::Button("+")) {
		ImGui::OpenPopup("CreateEntityPopup");
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopup("CreateEntityPopup")) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::MenuItem("Create Empty Object")) {
			Object obj = objManager.createObject("Object");
			const entt::entity objEntity = obj.EntityId();
			if (m_Owner != nullptr && m_Owner->IsPrefabEditingMode()) {
				// Prefab assets must remain a single authored object tree. The
				// hierarchy add button therefore creates a child of the current
				// prefab root instead of introducing a second root beside it.
				const std::vector<Object> roots = objManager.getRootObjects();
				for (const Object& rootObject : roots) {
					if (rootObject != obj) {
						objManager.setParent(obj, rootObject);
						break;
					}
				}
			}
			UndoRedoSystem::Instance().PushCommand(std::make_unique<CreateObjectCommand>(objEntity));
		}
		ImGui::EndPopup();
	}
	
	ImGui::SameLine();
	char hierarchySearchBuf[128];
	strncpy(hierarchySearchBuf, m_SearchFilter.c_str(), sizeof(hierarchySearchBuf));
	
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::InputTextWithHint("##HierarchySearch", "Search...", hierarchySearchBuf, IM_ARRAYSIZE(hierarchySearchBuf))) {
		m_SearchFilter = hierarchySearchBuf;
	}

	ImGui::Separator();
	const bool prefabEditingMode = m_Owner->IsPrefabEditingMode();
	RefreshRootEntityCache(objManager, prefabEditingMode);

	if (prefabEditingMode) {
		DrawCachedRootNodes();
	} else {
		const SceneDocument* activeScene = SceneManager::Instance().GetActiveScene();
		std::string sceneName = activeScene ? activeScene->name : "No Active Scene";
		
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (SceneManager::Instance().IsActiveSceneDirty()) {
			sceneName += "*";
		}

		ImGuiTreeNodeFlags sceneNodeFlags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
		bool sceneOpened = ImGui::TreeNodeEx(sceneName.c_str(), sceneNodeFlags);

		// Accept Drop on Root (Scene Node) -> Detach Parent
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ImGui::BeginDragDropTarget()) {
			if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
				IM_ASSERT(payload->DataSize == sizeof(entt::entity));
				entt::entity payload_n = *(const entt::entity*)payload->Data;
				
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (std::optional<Object> payloadObject = objManager.getObject(payload_n)) {
					// Detach from parent (make root)
					objManager.setParent(*payloadObject, std::nullopt);
				}
			}
			AcceptPrefabAssetDrop(std::nullopt);
			ImGui::EndDragDropTarget();
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sceneOpened) {
			DrawCachedRootNodes();
			ImGui::TreePop(); // Pop for scene name node
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_OpenEntityMenu) {
		ImGui::OpenPopup("EntityContextMenu");
		m_OpenEntityMenu = false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginPopup("EntityContextMenu")) {
		if (m_ContextEntityId != entt::null) {
			std::optional<Object> contextObject = objManager.getObject(m_ContextEntityId);
			const bool hasPrefabLink = contextObject && ObjectManager::instance().GetComponent<PrefabLinkComponent>(*contextObject) != nullptr;
			if (!contextObject)
			{
				ImGui::EndPopup();
				return;
			}
			if (ImGui::MenuItem("Delete")) {
				std::vector<entt::entity> remainingSelection = m_Owner->GetSelectedEntities();
				const auto removedSelection = std::remove(remainingSelection.begin(), remainingSelection.end(), m_ContextEntityId);
				const bool deletedObjectWasSelected = removedSelection != remainingSelection.end();
				remainingSelection.erase(removedSelection, remainingSelection.end());
				UndoRedoSystem::Instance().PushCommand(std::make_unique<DeleteObjectCommand>(m_ContextEntityId));
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				objManager.removeObject(*contextObject);
				if (deletedObjectWasSelected) {
					m_Owner->SetSelectedEntities(remainingSelection);
				}
				m_ContextEntityId = entt::null;
			}
			ImGui::Separator();
			if (ImGui::MenuItem("Create Prefab From Selection")) {
				CreatePrefabFromSelection(m_ContextEntityId);
			}
			if (ImGui::MenuItem("Open Prefab Source", nullptr, false, hasPrefabLink)) {
				m_Owner->OpenPrefabFromObject(m_ContextEntityId);
			}
			if (ImGui::MenuItem("Reset To Prefab Defaults", nullptr, false, hasPrefabLink)) {
				m_Owner->ResetPrefabInstanceToDefaults(m_ContextEntityId);
			}
			if (ImGui::MenuItem("Unlink Prefab", nullptr, false, hasPrefabLink)) {
				ObjectManager::instance().RemoveComponent<PrefabLinkComponent>(*contextObject);
				SceneManager::Instance().ForceMarkActiveSceneDirty();
			}
		}
		ImGui::EndPopup();
	}

	// Handle Delete Key
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsKeyReleased(ImGuiKey_Delete)) {
		auto selected = m_Owner->GetSelectedEntity();
		if (selected) {
			std::optional<Object> selectedObject = objManager.getObject(*selected);
			if (!selectedObject) {
				return;
			}
			UndoRedoSystem::Instance().PushCommand(std::make_unique<DeleteObjectCommand>(*selected));
			objManager.removeObject(*selectedObject);
			m_Owner->SetSelectedEntity(std::nullopt);
		}
	}
}

void HierarchyWindow::CreatePrefabFromSelection(entt::entity obj)
{
	if (m_Owner == nullptr)
	{
		return;
	}

	const std::filesystem::path assetRoot = SceneManager::Instance().GetSceneRoot();
	if (assetRoot.empty())
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot create prefab because the project asset root is not configured.");
		return;
	}

	const std::filesystem::path prefabDirectory = assetRoot / "Prefabs";
	std::error_code directoryError;
	std::filesystem::create_directories(prefabDirectory, directoryError);
	if (directoryError)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Cannot create prefab directory " << prefabDirectory << ": " << directoryError.message());
		return;
	}

	std::optional<Object> object = ObjectManager::instance().getObject(obj);
	if (!object)
	{
		return;
	}

	const std::string objectName = ObjectManager::instance().getObjectName(*object);
	const std::filesystem::path prefabPath = MakeUniquePrefabPath(prefabDirectory, MakePrefabFileStem(objectName));

	nlohmann::json prefabJson;
	prefabJson["name"] = prefabPath.stem().string();
	prefabJson["objects"] = nlohmann::json::array();
	// Save the selected branch as the prefab root. The scene parent is not
	// serialized, so opening the prefab shows this object as the top-level
	// prefab object rather than adding a container parent.
	prefabJson["objects"].push_back(Serializer::SerializeObject(obj));

	std::ofstream prefabOutput(prefabPath, std::ios::binary);
	if (!prefabOutput)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to write prefab file: " << prefabPath);
		return;
	}
	prefabOutput << prefabJson.dump(4);
	prefabOutput.close();

	PrefabLinkComponent* existingLink = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
	PrefabLinkComponent* ensuredLink = existingLink != nullptr ? existingLink : ObjectManager::instance().AddComponent<PrefabLinkComponent>(*object);
	if (ensuredLink == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to add prefab link to selected object.");
		return;
	}
	PrefabLinkComponent& link = *ensuredLink;
	link.prefabSourceId = TerraEngine::GenerateUUID();
	link.sourceObjectId = ObjectManager::instance().getUUID(*object);
	link.prefabName = prefabPath.stem().string();

	std::error_code relativeError;
	const std::filesystem::path relativePrefabPath = std::filesystem::relative(prefabPath, assetRoot, relativeError);
	link.prefabPath = relativeError ? prefabPath.generic_string() : relativePrefabPath.generic_string();
	link.overriddenProperties.clear();

	SceneManager::Instance().ForceMarkActiveSceneDirty();
	m_Owner->SetSelectedEntity(obj);
	TERRAENGINE_LOG(LogLevel::Info, "Created prefab from selection: " << prefabPath);
}

// HierarchyWindow::DrawEntityNode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void HierarchyWindow::DrawEntityNode(
	entt::entity obj,
	const std::unordered_set<TerraEngine::UUID>* prefabSourceObjectIds,
	bool knownLeaf,
	std::uint8_t* cachedExpandedFlag,
	bool drawChildren,
	std::optional<bool> prefabAuthoredOverride) {
	auto& objManager = ObjectManager::instance();
	std::optional<Object> object = objManager.getObject(obj);
	if (!object)
	{
		return;
	}
	std::string name = objManager.getObjectName(*object);
	
	bool matchesFilter = m_SearchFilter.empty();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!matchesFilter) {
		std::string lowerName = name;
		std::string lowerFilter = m_SearchFilter;
		std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);
		std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (lowerName.find(lowerFilter) != std::string::npos) {
			matchesFilter = true;
		}
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!matchesFilter) {
		// Simple skip for now
		// return; // Uncomment to strict filter
	}

	ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_SpanAvailWidth;
	
	// Selection highlighting resolves the stable UUID collection each frame, so
	// Ctrl-selected rows survive prefab-context entity-handle replacement.
	if (m_Owner->IsEntitySelected(obj)) {
		flags |= ImGuiTreeNodeFlags_Selected;
	}
	
	// Cached root leaves skip a registry traversal. Recursive children and branch
	// roots still query live hierarchy data so expansion and reparenting behavior
	// remains identical after the revision invalidates the root cache.
	std::vector<Object> children;
	if (!knownLeaf)
	{
		children = objManager.getChildren(*object);
	}
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (knownLeaf || children.empty()) {
		flags |= ImGuiTreeNodeFlags_Leaf;
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_SearchFilter.empty()) {
		flags |= ImGuiTreeNodeFlags_DefaultOpen;
	}
	
	PrefabLinkComponent* prefabLink = ObjectManager::instance().GetComponent<PrefabLinkComponent>(*object);
	std::unordered_set<TerraEngine::UUID> localPrefabSourceObjectIds;
	const std::unordered_set<TerraEngine::UUID>* activePrefabSourceObjectIds = prefabSourceObjectIds;
	if (prefabLink != nullptr)
	{
		localPrefabSourceObjectIds = LoadPrefabSourceObjectIds(*prefabLink);
		activePrefabSourceObjectIds = &localPrefabSourceObjectIds;
	}

	const bool isPrefabAuthoredObject = prefabAuthoredOverride.value_or(
		prefabLink != nullptr
		|| (activePrefabSourceObjectIds != nullptr
			&& activePrefabSourceObjectIds->find(ObjectManager::instance().getUUID(*object)) != activePrefabSourceObjectIds->end()));
	if (isPrefabAuthoredObject) {
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.72f, 1.0f, 1.0f));
	}
	// Scene-context prefab editing reloads object-manager snapshots around the
	// Scene View render path, which can change transient entt handles between
	// frames. Key the ImGui tree by the authored object UUID so expanded child
	// branches remain stable while the prefab is being edited in context.
	const std::string treeNodeId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(*object));
	bool opened = ImGui::TreeNodeEx(treeNodeId.c_str(), flags, "%s", name.c_str());
	if(cachedExpandedFlag != nullptr) {
		// ImGui has already applied arrow/double-click toggles for this visible
		// row. Store the result once so off-screen siblings require no state lookup.
		*cachedExpandedFlag = opened && (flags & ImGuiTreeNodeFlags_Leaf) == 0
			? 1U : 0U;
	}
	if (!drawChildren && (flags & ImGuiTreeNodeFlags_Leaf) == 0)
	{
		const bool previous = m_ExpandedByUuid[treeNodeId];
		if (previous != opened)
		{
			m_ExpandedByUuid[treeNodeId] = opened;
			++m_ExpansionRevision;
		}
	}
	if (isPrefabAuthoredObject) {
		ImGui::PopStyleColor();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsItemClicked()) {
		const ImGuiIO& io = ImGui::GetIO();
		if (io.KeyCtrl)
		{
			m_Owner->ToggleSelectedEntity(obj);
		}
		else
		{
			m_Owner->SetSelectedEntity(obj);
		}
	}
	if (prefabLink != nullptr && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
		m_Owner->OpenPrefabFromObject(obj);
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
		m_ContextEntityId = obj;
		m_OpenEntityMenu = true;
	}

	// Drag Source
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropSource()) {
		entt::entity entityId = obj;
		ImGui::SetDragDropPayload("ENTITY_CELL", &entityId, sizeof(entt::entity));
		ImGui::Text("%s", name.c_str());
		ImGui::EndDragDropSource();
	}

	// Drop Target (Parenting)
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENTITY_CELL")) {
			IM_ASSERT(payload->DataSize == sizeof(entt::entity));
			entt::entity payload_n = *(const entt::entity*)payload->Data;
			
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (payload_n != obj) {
				std::optional<Object> payloadObject = objManager.getObject(payload_n);
				if (payloadObject) {
					objManager.setParent(*payloadObject, object);
				}
			}
		}
		AcceptPrefabAssetDrop(obj);
		ImGui::EndDragDropTarget();
	}
	
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (opened) {
		if (drawChildren) {
			for (Object child : children) {
				DrawEntityNode(child.EntityId(), activePrefabSourceObjectIds);
			}
		}
		ImGui::TreePop();
	}
}
