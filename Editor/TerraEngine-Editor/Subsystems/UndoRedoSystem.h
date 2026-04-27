// UndoRedoSystem.h — Editor undo and redo system interface
//
// Defines the core interface and command structures for tracking reversible editor actions. it maintains a history of atomic operations like object creation, deletion, and property changes, allowing users to navigate through past scene states.

#pragma once

#include "IEditorSubsystem.h"
#include <deque>
#include <memory>
#include <string>
#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include "Engine/Utilities/UUID.h"

// Interface for atomic editor operations that can be reversed and reapplied.
// Concrete implementations must capture all state necessary to restore the 
// scene to its exact previous configuration.
struct ICommand {
	virtual ~ICommand() = default;
	virtual void Undo() = 0;
	virtual void Redo() = 0;
	virtual std::string GetName() const = 0;
};

// Records the creation of a new object. Undo destroys the entity, while Redo
// recreates it from a JSON snapshot to ensure component data and children
// are perfectly restored.
class CreateObjectCommand : public ICommand {
public:
	CreateObjectCommand(entt::entity entity);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	nlohmann::json m_Json;
};

// Records the deletion of an object graph. The constructor takes a recursive
// snapshot of the entity and all its descendants so the entire branch can be
// brought back during an Undo operation.
class DeleteObjectCommand : public ICommand {
public:
	DeleteObjectCommand(entt::entity entity);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	nlohmann::json m_Json;
};

// class RenameObjectCommand groups related state and operations so callers have one documented contract.
class RenameObjectCommand : public ICommand {
public:
	RenameObjectCommand(entt::entity entity, const std::string& oldName, const std::string& newName);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	std::string m_OldName;
	std::string m_NewName;
};

// class AddComponentCommand groups related state and operations so callers have one documented contract.
class AddComponentCommand : public ICommand {
public:
	AddComponentCommand(entt::entity entity, const std::string& componentName);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	std::string m_ComponentName;
};

// class RemoveComponentCommand groups related state and operations so callers have one documented contract.
class RemoveComponentCommand : public ICommand {
public:
	RemoveComponentCommand(entt::entity entity, const std::string& componentName);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	std::string m_ComponentName;
	nlohmann::json m_ComponentJson;
};

// class ChangeTransformCommand groups related state and operations so callers have one documented contract.
class ChangeTransformCommand : public ICommand {
public:
	ChangeTransformCommand(entt::entity entity, const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
						const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	glm::vec3 m_OldPos, m_NewPos;
	glm::quat m_OldRot, m_NewRot;
	glm::vec3 m_OldScale, m_NewScale;
};

// Manages a history of ICommand instances to provide undo/redo capabilities in 
// the editor. The system maintains a fixed-size buffer of past actions and 
// clears the redo stack whenever a new command is pushed.
class UndoRedoSystem : public IEditorSubsystem {
public:
	static UndoRedoSystem& Instance();

	void Initialize(EditorContext& context) override;
	void Update(float deltaTime) override;
	void DrawUI() override;
	void Shutdown() override;
	// GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string GetName() const override { return "Undo/Redo System"; }

	// Adds a new action to the history. This clears any currently available 
	// redo steps and marks the active scene as dirty.
	void PushCommand(std::unique_ptr<ICommand> command);
	
	// Reverses the most recent command on the undo stack and moves it to the 
	// redo stack.
	void Undo();
	
	// Reapplies the most recently undone command and moves it back to the 
	// undo stack.
	void Redo();
	
	bool CanUndo() const;
	bool CanRedo() const;

	// Marks the current point in history as the 'Saved' state.
	void SetSavePoint();

	// Clears the undo/redo history.
	void Clear();

private:
	void UpdateDirtyState();

	std::deque<std::unique_ptr<ICommand>> undoStack;
	std::deque<std::unique_ptr<ICommand>> redoStack;
	
	// Points to the command that was at the top of the undo stack when last saved.
	// If null, the saved state is the empty undo stack.
	ICommand* m_SavedCommand = nullptr;
	// Tracks if the saved state is currently reachable (not pruned by MaxHistory).
	bool m_SavedStateIsReachable = true;

	// Limits history depth to prevent unbounded memory growth during long 
	// editing sessions.
	static constexpr size_t MaxHistory = 100;
};
