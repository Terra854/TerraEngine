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
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <algorithm>
#include <utility>
#include <unordered_set>

namespace
{
class TransactionCommandGroup final : public ICommand
{
public:
	TransactionCommandGroup(std::string groupName, std::string historyId, std::vector<std::unique_ptr<ICommand>> commands)
		: m_GroupName(std::move(groupName)), m_HistoryId(std::move(historyId)), m_Commands(std::move(commands))
	{
	}

	void Undo() override
	{
		// Undo in reverse commit order so grouped editor transactions restore
		// parent/child/component dependencies before their owners disappear.
		for (auto command = m_Commands.rbegin(); command != m_Commands.rend(); ++command)
		{
			(*command)->Undo();
		}
	}

	void Redo() override
	{
		// Redo in original commit order to replay the transaction the same way
		// the protected mutation flush applied it.
		for (const std::unique_ptr<ICommand>& command : m_Commands)
		{
			command->Redo();
		}
	}

	std::string GetName() const override
	{
		return m_GroupName.empty() ? "Scene Transaction" : m_GroupName;
	}

	std::string GetHistoryId() const override
	{
		return m_HistoryId;
	}

private:
	std::string m_GroupName;
	std::string m_HistoryId;
	std::vector<std::unique_ptr<ICommand>> m_Commands;
};

class CommittedTransactionCommand final : public ICommand
{
public:
	CommittedTransactionCommand(std::string name, std::string historyId,
		std::vector<TerraEngine::Scene::TransactionWriteOperation> forward,
		std::vector<TerraEngine::Scene::TransactionWriteOperation> inverse)
		: m_Name(std::move(name)), m_HistoryId(std::move(historyId)),
		m_Forward(std::move(forward)), m_Inverse(std::move(inverse))
	{
	}

	void Undo() override { Enqueue(m_Inverse, "undo"); }
	void Redo() override { Enqueue(m_Forward, "redo"); }
	std::string GetName() const override { return m_Name; }
	std::string GetHistoryId() const override { return m_HistoryId; }

private:
	void Enqueue(const std::vector<TerraEngine::Scene::TransactionWriteOperation>& operations, const char* direction)
	{
		TerraEngine::Scene::Transaction replay;
		replay.id = "editor-transaction-replay-" + std::to_string(++m_ReplaySequence) + "-" + direction;
		replay.origin = TerraEngine::Scene::TransactionOrigin::Editor;
		replay.originName = "UndoRedoReplay";
		replay.writeSet.operations = operations;
		replay.budget.maxWriteCount = std::max<std::size_t>(replay.budget.maxWriteCount, operations.size());

		// Read observations are captured when the user asks for undo/redo. A stale
		// object generation therefore rejects the complete replay at publication.
		ObjectManager& objects = ObjectManager::instance();
		replay.readSet.sceneRevision = objects.GetHierarchyRevision();
		std::unordered_set<TerraEngine::UUID> observed;
		for (const TerraEngine::Scene::TransactionWriteOperation& operation : operations)
		{
			if (operation.target.objectId == TerraEngine::EMPTY_UUID || !observed.insert(operation.target.objectId).second)
			{
				continue;
			}
			if (const std::optional<Object> object = objects.getObject(operation.target.objectId))
			{
				replay.readSet.objects.push_back({
					operation.target.objectId,
					objects.getObjectLifetimeGeneration(*object),
					objects.getObjectMetadataGeneration(*object),
					objects.getObjectComponentGeneration(*object),
					objects.GetHierarchyRevision()
				});
			}
		}
		TerraEngine::Scene::SceneMutationQueue::Instance().EnqueueTransaction(std::move(replay));
	}

	std::string m_Name;
	std::string m_HistoryId;
	std::vector<TerraEngine::Scene::TransactionWriteOperation> m_Forward;
	std::vector<TerraEngine::Scene::TransactionWriteOperation> m_Inverse;
	std::uint64_t m_ReplaySequence = 0U;
};

// ResolveObject keeps undo/redo replay on the explicit ObjectManager service
// path so history commands never depend on raw entity ids surviving reloads.
std::optional<Object> ResolveObject(const TerraEngine::UUID& uuid)
{
	if (uuid == TerraEngine::EMPTY_UUID)
	{
		return std::nullopt;
	}

	return ObjectManager::instance().getObject(uuid);
}

TerraEngine::UUID GetObjectUuid(entt::entity entity)
{
	if (std::optional<Object> object = ObjectManager::instance().getObject(entity))
	{
		return ObjectManager::instance().getUUID(*object);
	}
	return TerraEngine::EMPTY_UUID;
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
	if (ObjectManager::instance().getObject(entity).has_value()) {
		m_Uuid = GetObjectUuid(entity);
		m_Json = Serializer::SerializeObject(entity);
	}
}

// CreateObjectCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void CreateObjectCommand::Undo() {
	if (auto object = ObjectManager::instance().getObject(m_Uuid)) {
		ObjectManager::instance().removeObject(*object);
	}
}

// CreateObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void CreateObjectCommand::Redo() {
	Serializer::DeserializeObjectEntity(m_Json);
}

// CreateObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string CreateObjectCommand::GetName() const { return "Create Object"; }

DeleteObjectCommand::DeleteObjectCommand(entt::entity entity) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ObjectManager::instance().getObject(entity).has_value()) {
		m_Json = Serializer::SerializeObject(entity);
		m_Uuid = GetObjectUuid(entity);
	}
}

// DeleteObjectCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DeleteObjectCommand::Undo() {
	Serializer::DeserializeObjectEntity(m_Json);
}

// DeleteObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void DeleteObjectCommand::Redo() {
	if (auto object = ObjectManager::instance().getObject(m_Uuid)) {
		ObjectManager::instance().removeObject(*object);
	}
}

// DeleteObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string DeleteObjectCommand::GetName() const { return "Delete Object"; }

RenameObjectCommand::RenameObjectCommand(entt::entity entity, const std::string& oldName, const std::string& newName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(GetObjectUuid(entity)), m_OldName(oldName), m_NewName(newName) {}

void RenameObjectCommand::Undo() {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto object = ResolveObject(m_Uuid)) {
		ObjectManager::instance().setObjectName(*object, m_OldName);
	}
}

// RenameObjectCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RenameObjectCommand::Redo() {
	if (auto object = ResolveObject(m_Uuid)) {
		ObjectManager::instance().setObjectName(*object, m_NewName);
	}
}

// RenameObjectCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RenameObjectCommand::GetName() const { return "Rename Object"; }

namespace
{
void ApplyObjectMetadata(const TerraEngine::UUID& uuid, const nlohmann::json& metadata)
{
	if (auto object = ResolveObject(uuid))
	{
		ObjectManager& objects = ObjectManager::instance();
		if (metadata.contains("name") && metadata["name"].is_string()) objects.setObjectName(*object, metadata["name"].get<std::string>());
		if (metadata.contains("tag") && metadata["tag"].is_string()) objects.setObjectTag(*object, metadata["tag"].get<std::string>());
		if (metadata.contains("layer") && metadata["layer"].is_string()) objects.setObjectLayer(*object, metadata["layer"].get<std::string>());
		if (metadata.contains("active") && metadata["active"].is_boolean()) objects.setObjectActive(*object, metadata["active"].get<bool>());
		if (metadata.contains("static") && metadata["static"].is_boolean()) objects.setObjectStatic(*object, metadata["static"].get<bool>());
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
}

std::optional<Object> ResolveParent(const std::optional<TerraEngine::UUID>& parentUuid)
{
	if (!parentUuid.has_value())
	{
		return std::nullopt;
	}
	return ResolveObject(*parentUuid);
}
}

SetObjectMetadataCommand::SetObjectMetadataCommand(entt::entity entity, const nlohmann::json& oldMetadata, const nlohmann::json& newMetadata)
	: m_Uuid(GetObjectUuid(entity)), m_OldMetadata(oldMetadata), m_NewMetadata(newMetadata)
{
}

void SetObjectMetadataCommand::Undo()
{
	ApplyObjectMetadata(m_Uuid, m_OldMetadata);
}

void SetObjectMetadataCommand::Redo()
{
	ApplyObjectMetadata(m_Uuid, m_NewMetadata);
}

std::string SetObjectMetadataCommand::GetName() const { return "Edit Object Metadata"; }

SetParentCommand::SetParentCommand(entt::entity child, std::optional<TerraEngine::UUID> oldParent, std::optional<TerraEngine::UUID> newParent)
	: m_ChildUuid(GetObjectUuid(child)), m_OldParent(oldParent), m_NewParent(newParent)
{
}

void SetParentCommand::Undo()
{
	if (auto child = ResolveObject(m_ChildUuid))
	{
		ObjectManager::instance().setParent(*child, ResolveParent(m_OldParent));
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
}

void SetParentCommand::Redo()
{
	if (auto child = ResolveObject(m_ChildUuid))
	{
		ObjectManager::instance().setParent(*child, ResolveParent(m_NewParent));
		SceneManager::Instance().ForceMarkActiveSceneDirty();
	}
}

std::string SetParentCommand::GetName() const { return "Set Object Parent"; }

AddComponentCommand::AddComponentCommand(entt::entity entity, const std::string& componentName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(GetObjectUuid(entity)), m_ComponentName(componentName) {}

void AddComponentCommand::Undo() {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].entityRemoveComponent(ObjectManager::instance().getRegistry(), obj->EntityId());
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
			reg[m_ComponentName].entityAddComponent(ObjectManager::instance().getRegistry(), obj->EntityId());
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// AddComponentCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string AddComponentCommand::GetName() const { return "Add " + m_ComponentName; }

RemoveComponentCommand::RemoveComponentCommand(entt::entity entity, const std::string& componentName)
	// m_Uuid is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	: m_Uuid(GetObjectUuid(entity)), m_ComponentName(componentName) {
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (ObjectManager::instance().getObject(entity).has_value()) {
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName)) {
			m_ComponentJson = reg[m_ComponentName].entitySerialize(ObjectManager::instance().getRegistry(), entity);
		}
	}
}

// RemoveComponentCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void RemoveComponentCommand::Undo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		auto& reg = ComponentAccessRegistry();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (reg.count(m_ComponentName)) {
			reg[m_ComponentName].entityAddComponent(ObjectManager::instance().getRegistry(), obj->EntityId());
			reg[m_ComponentName].entityDeserialize(ObjectManager::instance().getRegistry(), obj->EntityId(), m_ComponentJson);
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
			reg[m_ComponentName].entityRemoveComponent(ObjectManager::instance().getRegistry(), obj->EntityId());
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

// RemoveComponentCommand::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string RemoveComponentCommand::GetName() const { return "Remove " + m_ComponentName; }

SetComponentPayloadCommand::SetComponentPayloadCommand(entt::entity entity, const std::string& componentName, nlohmann::json oldPayload, nlohmann::json newPayload)
	: m_Uuid(GetObjectUuid(entity)), m_ComponentName(componentName), m_OldPayload(std::move(oldPayload)), m_NewPayload(std::move(newPayload))
{
}

void SetComponentPayloadCommand::Undo()
{
	if (auto obj = ResolveObject(m_Uuid))
	{
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName))
		{
			reg[m_ComponentName].entityDeserialize(ObjectManager::instance().getRegistry(), obj->EntityId(), m_OldPayload);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

void SetComponentPayloadCommand::Redo()
{
	if (auto obj = ResolveObject(m_Uuid))
	{
		auto& reg = ComponentAccessRegistry();
		if (reg.count(m_ComponentName))
		{
			reg[m_ComponentName].entityDeserialize(ObjectManager::instance().getRegistry(), obj->EntityId(), m_NewPayload);
			NotifySceneAfterComponentMutation(m_ComponentName);
		}
	}
}

std::string SetComponentPayloadCommand::GetName() const { return "Edit " + m_ComponentName; }

ChangeTransformCommand::ChangeTransformCommand(entt::entity entity, const glm::vec3& oldPos, const glm::quat& oldRot, const glm::vec3& oldScale,
											const glm::vec3& newPos, const glm::quat& newRot, const glm::vec3& newScale)
	: m_Uuid(GetObjectUuid(entity)), m_OldPos(oldPos), m_OldRot(oldRot), m_OldScale(oldScale),
	m_NewPos(newPos), m_NewRot(newRot), m_NewScale(newScale) {}

// ChangeTransformCommand::Undo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ChangeTransformCommand::Undo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		if (auto* t = ObjectManager::instance().GetComponent<Transform>(*obj)) {
			t->position = m_OldPos;
			t->rotation = m_OldRot;
			t->scale = m_OldScale;
			ObjectManager::instance().MarkObjectTransformDirty(*obj);
		}
	}
}

// ChangeTransformCommand::Redo is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ChangeTransformCommand::Redo() {
	if (auto obj = ResolveObject(m_Uuid)) {
		if (auto* t = ObjectManager::instance().GetComponent<Transform>(*obj)) {
			t->position = m_NewPos;
			t->rotation = m_NewRot;
			t->scale = m_NewScale;
			ObjectManager::instance().MarkObjectTransformDirty(*obj);
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
	++historySequence;
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

void UndoRedoSystem::PushTransactionGroup(std::string groupName, std::vector<std::unique_ptr<ICommand>> commands, std::string historyId)
{
	// Empty transaction groups are intentionally ignored; callers can submit a
	// preflight-only transaction without creating a confusing undo history item.
	if (commands.empty())
	{
		return;
	}
	if (commands.size() == 1 && historyId.empty())
	{
		PushCommand(std::move(commands.front()));
		return;
	}
	PushCommand(std::make_unique<TransactionCommandGroup>(std::move(groupName), std::move(historyId), std::move(commands)));
}

bool UndoRedoSystem::RecordCommittedTransaction(const TerraEngine::Scene::Transaction& transaction,
	const TerraEngine::Scene::TransactionResult& result)
{
	using namespace TerraEngine::Scene;
	if (transaction.origin != TransactionOrigin::Editor || transaction.undoGroupName.empty()
		|| result.status != TransactionStatus::Committed || result.inverseDeltas.empty())
	{
		return false;
	}

	// The editor bridge deliberately accepts reflected property writes only.
	// Structural/object snapshots need a richer asynchronous replay protocol and
	// must not be guessed from a receipt that omitted serialized forward state.
	if (!std::all_of(transaction.writeSet.operations.begin(), transaction.writeSet.operations.end(), [](const TransactionWriteOperation& operation) {
		return operation.kind == TransactionWriteKind::UpdateComponentProperty;
	}))
	{
		return false;
	}

	std::vector<TransactionWriteOperation> inverse;
	inverse.reserve(result.inverseDeltas.size());
	for (auto delta = result.inverseDeltas.rbegin(); delta != result.inverseDeltas.rend(); ++delta)
	{
		if (delta->restoresSerializedObject || delta->restoresSerializedComponent)
		{
			return false;
		}
		inverse.push_back(delta->operation);
	}
	PushCommand(std::make_unique<CommittedTransactionCommand>(transaction.undoGroupName,
		transaction.id, transaction.writeSet.operations, std::move(inverse)));
	return true;
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
	++historySequence;
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
	++historySequence;
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

std::size_t UndoRedoSystem::GetUndoDepth() const { return undoStack.size(); }
std::size_t UndoRedoSystem::GetRedoDepth() const { return redoStack.size(); }
std::string UndoRedoSystem::GetUndoLabel() const { return undoStack.empty() ? std::string{} : undoStack.back()->GetName(); }
std::string UndoRedoSystem::GetRedoLabel() const { return redoStack.empty() ? std::string{} : redoStack.back()->GetName(); }
std::string UndoRedoSystem::GetUndoHistoryId() const { return undoStack.empty() ? std::string{} : undoStack.back()->GetHistoryId(); }
std::string UndoRedoSystem::GetRedoHistoryId() const { return redoStack.empty() ? std::string{} : redoStack.back()->GetHistoryId(); }
std::uint64_t UndoRedoSystem::GetSequence() const { return historySequence; }

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
	++historySequence;
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
