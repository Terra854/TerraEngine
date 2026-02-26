#include "UndoRedoSystem.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Core/Object/Transform.h"
#include "Engine/Core/Object/Component.h"
#include "Engine/Scene/Serializer.h"

CreateObjectCommand::CreateObjectCommand(entt::entity entity) : m_Entity(entity) {
    if (auto obj = ObjectManager::instance().getObject(entity)) {
        m_Uuid = obj->getUUID();
        m_Json = Serializer::SerializeObject(*obj);
    }
}

void CreateObjectCommand::Undo() {
    if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
        ObjectManager::instance().removeObject(*obj);
    }
}

void CreateObjectCommand::Redo() {
    Serializer::DeserializeObject(m_Json);
}

std::string CreateObjectCommand::GetName() const { return "Create Object"; }

DeleteObjectCommand::DeleteObjectCommand(entt::entity entity) : m_Entity(entity) {
    if (auto obj = ObjectManager::instance().getObject(entity)) {
        m_Json = Serializer::SerializeObject(*obj);
        m_Uuid = obj->getUUID();
    }
}

void DeleteObjectCommand::Undo() {
    Serializer::DeserializeObject(m_Json);
}

void DeleteObjectCommand::Redo() {
    if (auto obj = ObjectManager::instance().getObject(m_Uuid)) {
        ObjectManager::instance().removeObject(*obj);
    }
}

std::string DeleteObjectCommand::GetName() const { return "Delete Object"; }

RenameObjectCommand::RenameObjectCommand(entt::entity entity, const std::string& oldName, const std::string& newName)
    : m_Entity(entity), m_OldName(oldName), m_NewName(newName) {}

void RenameObjectCommand::Undo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        ObjectManager::instance().setObjectName(*obj, m_OldName);
    }
}

void RenameObjectCommand::Redo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        ObjectManager::instance().setObjectName(*obj, m_NewName);
    }
}

std::string RenameObjectCommand::GetName() const { return "Rename Object"; }

AddComponentCommand::AddComponentCommand(entt::entity entity, const std::string& componentName)
    : m_Entity(entity), m_ComponentName(componentName) {}

void AddComponentCommand::Undo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        auto& reg = ComponentAccessRegistry();
        if (reg.count(m_ComponentName)) {
            reg[m_ComponentName].removeComponent(*obj);
        }
    }
}

void AddComponentCommand::Redo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        auto& reg = ComponentAccessRegistry();
        if (reg.count(m_ComponentName)) {
            reg[m_ComponentName].addComponent(*obj);
        }
    }
}

std::string AddComponentCommand::GetName() const { return "Add " + m_ComponentName; }

RemoveComponentCommand::RemoveComponentCommand(entt::entity entity, const std::string& componentName)
    : m_Entity(entity), m_ComponentName(componentName) {
    if (auto obj = ObjectManager::instance().getObject(entity)) {
        auto& reg = ComponentAccessRegistry();
        if (reg.count(m_ComponentName)) {
            m_ComponentJson = reg[m_ComponentName].serialize(*obj);
        }
    }
}

void RemoveComponentCommand::Undo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        auto& reg = ComponentAccessRegistry();
        if (reg.count(m_ComponentName)) {
            reg[m_ComponentName].addComponent(*obj);
            reg[m_ComponentName].deserialize(*obj, m_ComponentJson);
        }
    }
}

void RemoveComponentCommand::Redo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        auto& reg = ComponentAccessRegistry();
        if (reg.count(m_ComponentName)) {
            reg[m_ComponentName].removeComponent(*obj);
        }
    }
}

std::string RemoveComponentCommand::GetName() const { return "Remove " + m_ComponentName; }

ChangeTransformCommand::ChangeTransformCommand(entt::entity entity, const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
                                             const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale)
    : m_Entity(entity), m_OldPos(oldPos), m_OldRot(oldRot), m_OldScale(oldScale),
      m_NewPos(newPos), m_NewRot(newRot), m_NewScale(newScale) {}

void ChangeTransformCommand::Undo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        if (auto* t = ObjectManager::instance().getRegistry().try_get<Transform>(m_Entity)) {
            t->position = m_OldPos;
            t->rotation = m_OldRot;
            t->scale = m_OldScale;
        }
    }
}

void ChangeTransformCommand::Redo() {
    if (auto obj = ObjectManager::instance().getObject(m_Entity)) {
        if (auto* t = ObjectManager::instance().getRegistry().try_get<Transform>(m_Entity)) {
            t->position = m_NewPos;
            t->rotation = m_NewRot;
            t->scale = m_NewScale;
        }
    }
}

std::string ChangeTransformCommand::GetName() const { return "Transform Object"; }

UndoRedoSystem& UndoRedoSystem::Instance() {
    static UndoRedoSystem instance;
    return instance;
}

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

void UndoRedoSystem::PushCommand(std::unique_ptr<ICommand> command) {
    if (!command) return;

    TERRAENGINE_LOG(LogLevel::Debug, "Pushing command: " << command->GetName());

    undoStack.push_back(std::move(command));
    // New actions invalidate the redo branch as the scene state has branched 
    // from the previous history.
    redoStack.clear();

    if (undoStack.size() > MaxHistory) {
        auto& first = undoStack.front();
        if (first.get() == m_SavedCommand) {
            m_SavedStateIsReachable = false;
        }
        undoStack.pop_front();
    }

    UpdateDirtyState();
}

void UndoRedoSystem::Undo() {
    if (undoStack.empty()) return;

    auto command = std::move(undoStack.back());
    undoStack.pop_back();

    TERRAENGINE_LOG(LogLevel::Debug, "Undoing command: " << command->GetName());
    command->Undo();

    redoStack.push_back(std::move(command));
    UpdateDirtyState();
}

void UndoRedoSystem::Redo() {
    if (redoStack.empty()) return;

    // Redo stack entries are pulled from the back to maintain chronological 
    // order when reapplying multiple Undone steps.
    auto command = std::move(redoStack.back());
    redoStack.pop_back();

    TERRAENGINE_LOG(LogLevel::Debug, "Redoing command: " << command->GetName());
    command->Redo();

    undoStack.push_back(std::move(command));
    UpdateDirtyState();
}

bool UndoRedoSystem::CanUndo() const {
    return !undoStack.empty();
}

bool UndoRedoSystem::CanRedo() const {
    return !redoStack.empty();
}

void UndoRedoSystem::SetSavePoint() {
    m_SavedCommand = undoStack.empty() ? nullptr : undoStack.back().get();
    m_SavedStateIsReachable = true;
    UpdateDirtyState();
}

void UndoRedoSystem::Clear() {
    undoStack.clear();
    redoStack.clear();
    m_SavedCommand = nullptr;
    m_SavedStateIsReachable = true;
    UpdateDirtyState();
}

void UndoRedoSystem::UpdateDirtyState() {
    if (!m_SavedStateIsReachable) {
        SceneManager::Instance().ForceMarkActiveSceneDirty();
        return;
    }

    ICommand* currentTop = undoStack.empty() ? nullptr : undoStack.back().get();
    if (currentTop == m_SavedCommand) {
        SceneManager::Instance().ClearActiveSceneDirty();
    } else {
        SceneManager::Instance().ForceMarkActiveSceneDirty();
    }
}
