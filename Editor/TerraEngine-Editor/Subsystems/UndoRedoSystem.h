// UndoRedoSystem.h — Editor undo and redo system interface
//
// Defines the core interface and command structures for tracking reversible editor actions. it maintains a history of atomic operations like object creation, deletion, and property changes, allowing users to navigate through past scene states.

#pragma once

#include "IEditorSubsystem.h"
#include <deque>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <entt/entity/entity.hpp>
#include <nlohmann/json.hpp>
#include "Engine/Utilities/UUID.h"
#include "Engine/Scene/TransactionalMutation.h"
#include "Terrain/TerrainEditTransaction.h"

// Interface for atomic editor operations that can be reversed and reapplied.
// Concrete implementations must capture all state necessary to restore the 
// scene to its exact previous configuration.
struct ICommand {
	virtual ~ICommand() = default;
	virtual void Undo() = 0;
	virtual void Redo() = 0;
	virtual std::string GetName() const = 0;
	virtual std::string GetHistoryId() const { return {}; }
	// Deferred commands return the transaction they queued so the history owner
	// can wait for the protected scene flush before finalizing dirty-state tracking.
	virtual std::string GetPendingReplayTransactionId() const { return {}; }
	// Typed history can report retained blob bytes so long terrain sessions obey
	// a memory budget without pretending large patches are ordinary JSON edits.
	virtual std::size_t GetHistoryBytes() const { return 0U; }
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

// Records object metadata edits made through automation or inspector paths.
// All fields are captured because name, tag, layer, active, and static together
// define the editor-visible metadata surface for one object.
class SetObjectMetadataCommand : public ICommand {
public:
	SetObjectMetadataCommand(entt::entity entity, const nlohmann::json& oldMetadata, const nlohmann::json& newMetadata);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	nlohmann::json m_OldMetadata;
	nlohmann::json m_NewMetadata;
};

// Records hierarchy edits so assistant-driven parenting can be undone just like
// hand-authored hierarchy changes.
class SetParentCommand : public ICommand {
public:
	SetParentCommand(entt::entity child, std::optional<TerraEngine::UUID> oldParent, std::optional<TerraEngine::UUID> newParent);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_ChildUuid;
	std::optional<TerraEngine::UUID> m_OldParent;
	std::optional<TerraEngine::UUID> m_NewParent;
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

// Records a reflected component payload before and after a property edit. The
// command stores component JSON instead of a single variant so undo remains
// robust if multiple reflected fields are coupled by component code later.
class SetComponentPayloadCommand : public ICommand {
public:
	SetComponentPayloadCommand(entt::entity entity, const std::string& componentName, nlohmann::json oldPayload, nlohmann::json newPayload);
	void Undo() override;
	void Redo() override;
	std::string GetName() const override;

private:
	TerraEngine::UUID m_Uuid;
	std::string m_ComponentName;
	nlohmann::json m_OldPayload;
	nlohmann::json m_NewPayload;
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
	void PushTransactionGroup(std::string groupName, std::vector<std::unique_ptr<ICommand>> commands, std::string historyId = {});
	// Records a transaction only after its protected flush committed atomically.
	// Undo and redo enqueue inverse/forward typed writes for a later boundary.
	bool RecordCommittedTransaction(const TerraEngine::Scene::Transaction& transaction,
		const TerraEngine::Scene::TransactionResult& result);
	// Completes an undo/redo command that published through SceneMutationQueue.
	// Returning true means the receipt belonged to the active history replay.
	bool CompleteDeferredReplay(const TerraEngine::Scene::TransactionResult& result);
	bool CompleteTerrainReplay(const TerraEngine::Terrain::TerrainTransactionReceipt& receipt);
	
	// Reverses the most recent command on the undo stack and moves it to the 
	// redo stack.
	void Undo();
	
	// Reapplies the most recently undone command and moves it back to the 
	// undo stack.
	void Redo();
	
	bool CanUndo() const;
	bool CanRedo() const;
	std::size_t GetUndoDepth() const;
	std::size_t GetRedoDepth() const;
	std::string GetUndoLabel() const;
	std::string GetRedoLabel() const;
	std::string GetUndoHistoryId() const;
	std::string GetRedoHistoryId() const;
	std::uint64_t GetSequence() const;
	std::size_t GetHistoryBytes() const;
	std::size_t GetHistoryCountBudget() const;
	std::size_t GetHistoryByteBudget() const;
	bool IsAtSavePoint() const;
	// Projects and tests may tune both limits. Reducing either limit prunes the
	// oldest retained commands immediately while preserving the active cursor.
	void SetHistoryBudget(std::size_t maximumCommands, std::size_t maximumBytes);

	// Marks the current point in history as the 'Saved' state.
	void SetSavePoint();

	// Clears the undo/redo history.
	void Clear();

private:
	// Each history entry names both scene states surrounding its command. Stable
	// state identifiers let dirty tracking follow the exact saved revision even
	// when commands move between the undo and redo stacks or a branch is replaced.
	struct HistoryEntry
	{
		std::unique_ptr<ICommand> command;
		std::uint64_t beforeStateId = 0u;
		std::uint64_t afterStateId = 0u;
		std::size_t byteCost = 0U;
	};

	enum class DeferredReplayDirection
	{
		Undo,
		Redo
	};

	struct DeferredReplay
	{
		std::string transactionId;
		DeferredReplayDirection direction = DeferredReplayDirection::Undo;
	};

	void UpdateDirtyState();
	void PruneToHistoryBudget();

	std::deque<HistoryEntry> undoStack;
	std::deque<HistoryEntry> redoStack;
	// At most one deferred replay may be in flight. Serializing replay requests
	// prevents a second command from observing live component data that the first
	// protected transaction has not published yet.
	std::optional<DeferredReplay> m_DeferredReplay;

	// Saving records the current logical state rather than merely clearing a
	// boolean. Undoing back to this identifier makes the scene clean again, while
	// redoing past it makes the scene dirty.
	std::uint64_t m_CurrentStateId = 0u;
	std::uint64_t m_SavedStateId = 0u;
	std::uint64_t m_NextStateId = 1u;
	std::uint64_t historySequence = 0u;
	std::size_t m_HistoryBytes = 0U;

	// Defaults satisfy terrain's exact 1,000-stroke workflow while remaining
	// configurable for memory-constrained projects and deterministic tests.
	std::size_t m_MaxHistory = 1000U;
	std::size_t m_MaxHistoryBytes = 512U * 1024U * 1024U;
};
