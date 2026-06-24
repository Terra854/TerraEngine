// UndoRedoSystem.cpp — Editor undo and redo system implementation
//
// Implements the detailed command logic for reversing and reapplying editor actions. It manages a dual-stack history of commands and coordinates with the SceneManager to ensure the active scene's dirty state correctly reflects its position relative to the last save point.

#include "UndoRedoSystem.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Scripting/ScriptingSystem.h"

namespace
{
// ResolveObject is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::optional<Object> ResolveObject(const TerraEngine::UUID& uuid)
{
	if (uuid == TerraEngine::EMPTY_UUID)
	{
		return std::nullopt;
	}

	return ObjectManager::instance().getObject(uuid);
}

bool IsSkyComponentName(const std::string& componentName)
{
	return componentName == "SkyComponent";
}

void NotifySceneAfterComponentMutation(const std::string& componentName)
{
	SceneManager::Instance().ForceMarkActiveSceneDirty();
	if (IsSkyComponentName(componentName))
	{
		SceneManager::Instance().MarkActiveSceneSkyStateDirty();
	}
}
}

CreateObjectCommand::CreateObjectCommand(entt::entity entity) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ObjectManager::instance().getObject(entity)) {
		m_Uuid = obj->getUUID();
		m_Json = Serializer::SerializeObject(*obj);
	}
}

// CreateObjectCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void CreateObjectCommand::Undo() {
	if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
		ObjectManager::instance().removeObject(*obj);
	}
}

// CreateObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void CreateObjectCommand::Redo() {
	Serializer::DeserializeObject(m_Json);
}

// CreateObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string CreateObjectCommand::GetName() const { return "Create Object"; }

DeleteObjectCommand::DeleteObjectCommand(entt::entity entity) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ObjectManager::instance().getObject(entity)) {
		m_Json = Serializer::SerializeObject(*obj);
		m_Uuid = obj->getUUID();
	}
}

// DeleteObjectCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DeleteObjectCommand::Undo() {
	Serializer::DeserializeObject(m_Json);
}

// DeleteObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DeleteObjectCommand::Redo() {
	if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
		ObjectManager::instance().removeObject(*obj);
	}
}

// DeleteObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string DeleteObjectCommand::GetName() const { return "Delete Object"; }

RenameObjectCommand::RenameObjectCommand(entt::entity entity, const std::string& oldName, const std::string& newName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(ObjectManager::instance().getUUID(entity)), m_OldName(oldName), m_NewName(newName) {}

void RenameObjectCommand::Undo() {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ResolveObject(m_Uuid)) {
		ObjectManager::instance().setObjectName(*obj, m_OldName);
	}
}

// RenameObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenameObjectCommand::Redo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		ObjectManager::instance().setObjectName(*obj, m_NewName);
	}
}

// RenameObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RenameObjectCommand::GetName() const { return "Rename Object"; }

AddComponentCommand::AddComponentCommand(entt::entity entity, const std::string& componentName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(ObjectManager::instance().getUUID(entity)), m_ComponentName(componentName) {}

void AddComponentCommand::Undo() {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].removeComponent(*obj);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// AddComponentCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void AddComponentCommand::Redo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].addComponent(*obj);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// AddComponentCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AddComponentCommand::GetName() const { return "Add " + m_ComponentName; }

RemoveComponentCommand::RemoveComponentCommand(entt::entity entity, const std::string& componentName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(ObjectManager::instance().getUUID(entity)), m_ComponentName(componentName) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ObjectManager::instance().getObject(entity)) {
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName)) {
			m_ComponentJson = reg[m_ComponentName].serialize(*obj);
		}
	}
}

// RemoveComponentCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RemoveComponentCommand::Undo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].addComponent(*obj);
			reg[m_ComponentName].deserialize(*obj, m_ComponentJson);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// RemoveComponentCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RemoveComponentCommand::Redo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].removeComponent(*obj);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// RemoveComponentCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RemoveComponentCommand::GetName() const { return "Remove " + m_ComponentName; }

ChangeTransformCommand::ChangeTransformCommand(entt::entity entity, const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
											const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale)
	: m_Uuid(ObjectManager::instance().getUUID(entity)), m_OldPos(oldPos), m_OldRot(oldRot), m_OldScale(oldScale),
	m_NewPos(newPos), m_NewRot(newRot), m_NewScale(newScale) {}

// ChangeTransformCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ChangeTransformCommand::Undo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		if (auto* t = obj->getComponent<Transform>()) {
			t->position = m_OldPos;
			t->rotation = m_OldRot;
			t->scale = m_OldScale;
		}
	}
}

// ChangeTransformCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ChangeTransformCommand::Redo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		if (auto* t = obj->getComponent<Transform>()) {
			t->position = m_NewPos;
			t->rotation = m_NewRot;
			t->scale = m_NewScale;
		}
	}
}

// ChangeTransformCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ChangeTransformCommand::GetName() const { return "Transform Object"; }

UndoRedoSystem& UndoRedoSystem::Instance() {
	static UndoRedoSystem instance;
	return instance;
}

// UndoRedoSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::Initialize(EditorContext& context) {
	// Initialization logic if needed
}

void UndoRedoSystem::Update(float deltaTime) {
	// Update logic if needed
}

void UndoRedoSystem::DrawUI() {
	// Debug UI if needed
}

void UndoRedoSystem::Shutdown() {
	undoStack.clear();
	redoStack.clear();
}

// UndoRedoSystem::PushCommand is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::PushCommand(std::unique_ptr<ICommand> command) {
	if (!command) return;
	// Play-mode edits are transient because Stop restores the pre-play scene
	// snapshot, so TerraEngine keeps the editor history frozen while running.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ScriptingSystem::Instance().IsRunning()) return;

	TERRAENGINE_LOG(LogLevel::Debug, "Pushing command: " << command->GetName());

	undoStack.push_back(std::move(command));
	// New actions invalidate the redo branch as the scene state has branched 
	// from the previous history.
	redoStack.clear();

	if (undoStack.size() > MaxHistory) {
		auto& first = undoStack.front();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (first.get() == m_SavedCommand) {
			m_SavedStateIsReachable = false;
		}
		undoStack.pop_front();
	}

	UpdateDirtyState();
}

// UndoRedoSystem::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::Undo() {
	if (undoStack.empty()) return;
	if (ScriptingSystem::Instance().IsRunning()) return;

	auto command = std::move(undoStack.back());
	undoStack.pop_back();

	TERRAENGINE_LOG(LogLevel::Debug, "Undoing command: " << command->GetName());
	command->Undo();

	redoStack.push_back(std::move(command));
	UpdateDirtyState();
}

// UndoRedoSystem::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::Redo() {
	if (redoStack.empty()) return;
	if (ScriptingSystem::Instance().IsRunning()) return;

	// Redo stack entries are pulled from the back to maintain chronological 
	// order when reapplying multiple Undone steps.
	auto command = std::move(redoStack.back());
	redoStack.pop_back();

	TERRAENGINE_LOG(LogLevel::Debug, "Redoing command: " << command->GetName());
	command->Redo();

	undoStack.push_back(std::move(command));
	UpdateDirtyState();
}

// UndoRedoSystem::CanUndo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UndoRedoSystem::CanUndo() const {
	return !ScriptingSystem::Instance().IsRunning() && !undoStack.empty();
}

// UndoRedoSystem::CanRedo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool UndoRedoSystem::CanRedo() const {
	return !ScriptingSystem::Instance().IsRunning() && !redoStack.empty();
}

// UndoRedoSystem::SetSavePoint is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::SetSavePoint() {
	m_SavedCommand = undoStack.empty() ? nullptr : undoStack.back().get();
	m_SavedStateIsReachable = true;
	UpdateDirtyState();
}

// UndoRedoSystem::Clear is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::Clear() {
	undoStack.clear();
	redoStack.clear();
	m_SavedCommand = nullptr;
	m_SavedStateIsReachable = true;
	UpdateDirtyState();
}

// UndoRedoSystem::UpdateDirtyState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void UndoRedoSystem::UpdateDirtyState() {
	if (!m_SavedStateIsReachable) {
		SceneManager::Instance().ForceMarkActiveSceneDirty();
		return;
	}

	ICommand* currentTop = undoStack.empty() ? nullptr : undoStack.back().get();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (currentTop == m_SavedCommand) {
		SceneManager::Instance().ClearActiveSceneDirty();
	} else {
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
}
