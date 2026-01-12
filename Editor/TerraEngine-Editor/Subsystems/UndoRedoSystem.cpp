#include "UndoRedoSystem.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/Serializer.h"
#include "Engine/Core/Object/IDComponent.h"

CreateObjectCommand::CreateObjectCommand(entt::entity entity) : m_Entity(entity) {
    if (auto obj = ObjectManager::instance().getObject(entity)) {
        if (auto idComp = obj->getComponent<IDComponent>()) {
            m_Uuid = idComp->id;
        }
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
        if (auto idComp = obj->getComponent<IDComponent>()) {
            m_Uuid = idComp->id;
        }
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
        undoStack.pop_front();
    }

    // Every undoable action is considered a scene modification.
    SceneManager::Instance().MarkActiveSceneDirty();
}

void UndoRedoSystem::Undo() {
    if (undoStack.empty()) return;

    auto command = std::move(undoStack.back());
    undoStack.pop_back();

    TERRAENGINE_LOG(LogLevel::Debug, "Undoing command: " << command->GetName());
    command->Undo();

    redoStack.push_back(std::move(command));
    SceneManager::Instance().MarkActiveSceneDirty();
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
    SceneManager::Instance().MarkActiveSceneDirty();
}

bool UndoRedoSystem::CanUndo() const {
    return !undoStack.empty();
}

bool UndoRedoSystem::CanRedo() const {
    return !redoStack.empty();
}
