#include "ScriptingSystem.h"

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "DotNet/DotNetHost.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <filesystem>
#include <future>

namespace fs = std::filesystem;

namespace
{
	constexpr float kLiveInspectorRefreshSeconds = 0.25f;

	struct ManagedDescriptorResponse
	{
		std::string fullTypeName;
		std::vector<ScriptFieldDescriptor> fields;
		std::vector<ScriptFieldDiagnostic> diagnostics;
	};

	struct ManagedSnapshotResponse
	{
		std::string bindingId;
		std::string fullTypeName;
		std::string status = "Attached";
		bool runtimeAvailable = false;
		int runtimeOverrideCount = 0;
		std::vector<ScriptFieldSnapshot> fields;
		std::vector<ScriptFieldDiagnostic> diagnostics;
	};

	struct ManagedWriteResponse
	{
		bool success = false;
		std::string bindingId;
		std::string fieldPath;
		std::string message;
		nlohmann::json value;
		std::uint64_t snapshotVersion = 0;
	};

	struct PendingBindingSnapshotRefresh
	{
		std::string bindingId;
		std::string className;
	};

	bool IsRuntimeSnapshotDiagnostic(const ScriptFieldDiagnostic& diagnostic)
	{
		return diagnostic.code == "InstanceUnavailable"
			|| diagnostic.code == "BridgeFailure"
			|| diagnostic.code == "InvalidType";
	}

	struct ManagedMigrationResponse
	{
		std::vector<ScriptFieldValue> fields;
		std::vector<std::string> preservedFieldPaths;
		std::vector<std::string> convertedFieldPaths;
		std::vector<std::string> brokenFieldPaths;
		std::vector<std::string> hiddenStoredFieldPaths;
		std::vector<ScriptFieldDiagnostic> diagnostics;
	};

	void from_json(const nlohmann::json& jsonValue, ManagedDescriptorResponse& response)
	{
		response.fullTypeName = jsonValue.value("fullTypeName", std::string{});
		response.fields = jsonValue.value("fields", std::vector<ScriptFieldDescriptor>{});
		response.diagnostics = jsonValue.value("diagnostics", std::vector<ScriptFieldDiagnostic>{});
	}

	void from_json(const nlohmann::json& jsonValue, ManagedSnapshotResponse& response)
	{
		response.bindingId = jsonValue.value("bindingId", std::string{});
		response.fullTypeName = jsonValue.value("fullTypeName", std::string{});
		response.status = jsonValue.value("status", std::string{ "Attached" });
		response.runtimeAvailable = jsonValue.value("runtimeAvailable", false);
		response.runtimeOverrideCount = jsonValue.value("runtimeOverrideCount", 0);
		response.fields = jsonValue.value("fields", std::vector<ScriptFieldSnapshot>{});
		response.diagnostics = jsonValue.value("diagnostics", std::vector<ScriptFieldDiagnostic>{});
	}

	void from_json(const nlohmann::json& jsonValue, ManagedWriteResponse& response)
	{
		response.success = jsonValue.value("success", false);
		response.bindingId = jsonValue.value("bindingId", std::string{});
		response.fieldPath = jsonValue.value("fieldPath", std::string{});
		response.message = jsonValue.value("message", std::string{});
		response.value = jsonValue.contains("value") ? jsonValue["value"] : nlohmann::json{};
		response.snapshotVersion = jsonValue.value("snapshotVersion", static_cast<std::uint64_t>(0));
	}

	void from_json(const nlohmann::json& jsonValue, ManagedMigrationResponse& response)
	{
		response.fields = jsonValue.value("fields", std::vector<ScriptFieldValue>{});
		response.preservedFieldPaths = jsonValue.value("preservedFieldPaths", std::vector<std::string>{});
		response.convertedFieldPaths = jsonValue.value("convertedFieldPaths", std::vector<std::string>{});
		response.brokenFieldPaths = jsonValue.value("brokenFieldPaths", std::vector<std::string>{});
		response.hiddenStoredFieldPaths = jsonValue.value("hiddenStoredFieldPaths", std::vector<std::string>{});
		response.diagnostics = jsonValue.value("diagnostics", std::vector<ScriptFieldDiagnostic>{});
	}

	template <typename ResponseType>
	bool ParseManagedResponse(const char* payloadText, ResponseType& outResponse)
	{
		if (payloadText == nullptr || payloadText[0] == '\0')
		{
			return false;
		}

		try
		{
			outResponse = nlohmann::json::parse(payloadText).get<ResponseType>();
			return true;
		}
		catch (const std::exception&)
		{
			return false;
		}
	}

	std::string BuildRepairActionId(std::string_view kind, std::string_view bindingId, std::string_view targetPath)
	{
		return std::string(kind) + ":" + std::string(bindingId) + ":" + std::string(targetPath);
	}
}

ScriptingSystem& ScriptingSystem::Instance() {
	static ScriptingSystem instance;
	return instance;
}

void ScriptingSystem::Initialize() {
	TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

	const fs::path bridgeAssemblyPath = ResolveManagedBridgeAssemblyPath();
	const fs::path runtimeConfigPath = bridgeAssemblyPath.parent_path() / "TerraEngine.CSharp.runtimeconfig.json";
	if (!fs::exists(bridgeAssemblyPath) || !fs::exists(runtimeConfigPath)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Could not find the managed TerraEngine.CSharp bridge beside the running executable. Scripting will stay disabled.");
		return;
	}

	if (!DotNetHost::Instance().Initialize(bridgeAssemblyPath)) {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to initialize the embedded .NET runtime host.");
		return;
	}

	managedInit = reinterpret_cast<InitFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"Initialize"));
	managedUpdateScript = reinterpret_cast<UpdateScriptFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"UpdateScript"));
	managedLoadGameplayAssembly = reinterpret_cast<LoadGameplayAssemblyFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"LoadGameplayAssembly"));
	managedUnloadGameplayAssembly = reinterpret_cast<UnloadGameplayAssemblyFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"UnloadGameplayAssembly"));
	managedResetGameplaySession = reinterpret_cast<ResetGameplaySessionFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"ResetGameplaySession"));
	managedIsAttachableScriptType = reinterpret_cast<IsAttachableScriptTypeFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"IsScriptTypeAttachable"));
	managedDescribeBindingType = reinterpret_cast<DescribeBindingTypeFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"DescribeBindingType"));
	managedReadLiveBindingValues = reinterpret_cast<ReadLiveBindingValuesFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"ReadLiveBindingValues"));
	managedWriteLiveBindingValue = reinterpret_cast<WriteLiveBindingValueFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"WriteLiveBindingValue"));
	managedMigrateBindingAuthoredState = reinterpret_cast<MigrateBindingAuthoredStateFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"MigrateBindingAuthoredState"));

	if (managedInit != nullptr) {
		managedInit();
		TERRAENGINE_LOG(LogLevel::Info, "Scripting System initialized successfully.");
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to resolve TerraEngine.InternalCalls.Initialize from " << bridgeAssemblyPath.string());
	}
}

void ScriptingSystem::SetRunning(bool running)
{
	if (m_Running == running)
	{
		return;
	}

	m_Running = running;
	if (!m_Running)
	{
		if (managedResetGameplaySession != nullptr)
		{
			// Stopping play should discard every live managed Script instance so
			// the next play session starts from the restored scene snapshot
			// instead of reusing stale runtime field state.
			managedResetGameplaySession();
		}
		m_Paused = false;
		m_StepFrames = 0;
		ClearRuntimeInspectorState();
	}
}

void ScriptingSystem::Update(float deltaTime) {
	if (managedUpdateScript == nullptr || !m_GameplayAssemblyLoaded || !m_Running) {
		return;
	}

	if (m_Paused) {
		if (m_StepFrames > 0) {
			m_StepFrames--;
		} else {
			RefreshWatchedBindingSnapshots(deltaTime);
			return;
		}
	}

	struct ScriptBatchItem {
		uint32_t objectId = 0;
		std::string bindingId;
		std::string className;
	};

	std::vector<ScriptBatchItem> allBindings;
	auto& objectManager = ObjectManager::instance();
	for (Object object : objectManager.getObjectsWithComponents<ScriptComponent>()) {
		ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
		if (scriptComponent == nullptr || !scriptComponent->active) {
			continue;
		}

		auto guard = scriptComponent->acquireReadLock();
		for (const ScriptBinding& binding : scriptComponent->GetBindings()) {
			if (!binding.enabled || binding.className.empty()) {
				continue;
			}

			allBindings.push_back({ static_cast<uint32_t>(object.EntityId()), binding.bindingId, binding.className });
		}
	}

	if (!allBindings.empty()) {
		const std::size_t threadCount = std::max<std::size_t>(1, ThreadPool::instance().getThreadCount());
		const std::size_t targetTaskCount = threadCount * 4;
		std::size_t batchSize = allBindings.size() / std::max<std::size_t>(1, targetTaskCount);
		batchSize = std::max<std::size_t>(8, batchSize);

		// Wait for the current frame's script batches before returning so play
		// mode cannot enqueue an unbounded backlog of managed updates.
		std::vector<std::future<void>> futures;
		std::vector<ScriptBatchItem> currentBatch;
		currentBatch.reserve(batchSize);

		for (const ScriptBatchItem& item : allBindings) {
			currentBatch.push_back(item);
			if (currentBatch.size() >= batchSize) {
				auto task = ThreadPool::instance().createTask([items = std::move(currentBatch), this, deltaTime]() {
					for (const ScriptBatchItem& entry : items) {
						managedUpdateScript(entry.objectId, entry.bindingId.c_str(), entry.className.c_str(), deltaTime);
					}
				});
				futures.push_back(task->getFuture());
				ThreadPool::instance().enqueue(task);
				currentBatch = std::vector<ScriptBatchItem>();
				currentBatch.reserve(batchSize);
			}
		}

		if (!currentBatch.empty()) {
			auto task = ThreadPool::instance().createTask([items = std::move(currentBatch), this, deltaTime]() {
				for (const ScriptBatchItem& entry : items) {
					managedUpdateScript(entry.objectId, entry.bindingId.c_str(), entry.className.c_str(), deltaTime);
				}
			});
			futures.push_back(task->getFuture());
			ThreadPool::instance().enqueue(task);
		}

		for (std::future<void>& future : futures) {
			future.get();
		}
	}

	RefreshWatchedBindingSnapshots(deltaTime);
}

void ScriptingSystem::Shutdown() {
	ClearGameplayAssembly();
	DotNetHost::Instance().Shutdown();
}

std::string ScriptingSystem::SystemName() {
	return "ScriptingSystem";
}

bool ScriptingSystem::LoadGameplayAssembly(const std::filesystem::path& assemblyPath, std::string* outError) {
	if (managedLoadGameplayAssembly == nullptr) {
		const std::string message = "The managed gameplay assembly loader is unavailable because TerraEngine.CSharp did not expose LoadGameplayAssembly.";
		m_LastGameplayDiagnostic = message;
		if (outError) {
			*outError = message;
		}
		return false;
	}

	const std::string assemblyPathString = fs::absolute(assemblyPath).string();
	if (!fs::exists(assemblyPathString)) {
		const std::string message = "Assembly-CSharp.dll was expected at " + assemblyPathString + " but does not exist.";
		m_LastGameplayDiagnostic = message;
		if (outError) {
			*outError = message;
		}
		return false;
	}

	if (managedLoadGameplayAssembly(assemblyPathString.c_str()) == 0) {
		const std::string message = "TerraEngine.CSharp rejected the gameplay assembly reload for " + assemblyPathString + ".";
		m_LastGameplayDiagnostic = message;
		if (outError) {
			*outError = message;
		}
		return false;
	}

	m_GameplayAssemblyLoaded = true;
	m_LoadedGameplayAssemblyPath = assemblyPathString;
	m_LastGameplayDiagnostic = "Loaded gameplay assembly " + m_LoadedGameplayAssemblyPath.string();
	RefreshBindingStatuses();
	return true;
}

void ScriptingSystem::ClearGameplayAssembly() {
	if (managedUnloadGameplayAssembly != nullptr) {
		managedUnloadGameplayAssembly();
	}

	m_GameplayAssemblyLoaded = false;
	m_LoadedGameplayAssemblyPath.clear();
	m_LastGameplayDiagnostic.clear();
	SetAllBindingStatuses({});
	ClearRuntimeInspectorState();
}

bool ScriptingSystem::IsScriptTypeAttachable(const std::string& className) const {
	if (managedIsAttachableScriptType == nullptr || className.empty()) {
		return false;
	}

	return managedIsAttachableScriptType(className.c_str()) != 0;
}

void ScriptingSystem::InitializeBindingMetadata(ScriptBinding& binding)
{
	binding.fullTypeName = binding.className;
	binding.runtimeAvailable = false;
	binding.runtimeOverrideCount = 0;
	binding.runtimeOverrides.clear();
	binding.liveSnapshots.clear();
	binding.migrationResult = {};
	binding.lastBridgeMessage.clear();

	if (!m_GameplayAssemblyLoaded || !IsScriptTypeAttachable(binding.className))
	{
		binding.status.clear();
		binding.fieldDescriptors.clear();
		binding.diagnostics.clear();
		binding.diagnosticIds.clear();
		binding.repairActions.clear();
		return;
	}

	// A freshly attached binding should describe itself immediately when the
	// gameplay assembly is already loaded so the inspector can render fields
	// without waiting for another compile or reload cycle.
	binding.status = "Attached";
	RefreshBindingDescriptor(binding);
	PopulateBindingRepairActions(binding);
}

void ScriptingSystem::RebuildRestoredSceneBindings()
{
	if (!m_GameplayAssemblyLoaded)
	{
		SetAllBindingStatuses({});
		ClearRuntimeInspectorState();
		return;
	}

	// Restoring the pre-play hierarchy recreates ScriptComponent instances from
	// serialized data, so TerraEngine must rebuild their descriptor state
	// against the current gameplay assembly before the inspector reads them.
	RefreshBindingStatuses();
}

void ScriptingSystem::UpdateInspectorWatch(std::optional<uint32_t> objectId, std::uint64_t selectionVersion)
{
	std::scoped_lock lock(m_InspectorWatchMutex);
	const bool changed = m_InspectorWatchState.watchedObjectId != objectId || m_InspectorWatchState.selectionVersion != selectionVersion;
	m_InspectorWatchState.watchedObjectId = objectId;
	m_InspectorWatchState.selectionVersion = selectionVersion;
	if (changed)
	{
		m_InspectorWatchState.refreshCountdownSeconds = 0.0f;
	}
}

bool ScriptingSystem::WriteLiveBindingValue(uint32_t objectId, ScriptBinding& binding, std::string_view fieldPath, const nlohmann::json& value, std::string* outError)
{
	if (managedWriteLiveBindingValue == nullptr || !m_GameplayAssemblyLoaded)
	{
		if (outError)
		{
			*outError = "The live script write bridge is unavailable because TerraEngine.CSharp did not expose WriteLiveBindingValue.";
		}
		return false;
	}

	ManagedWriteResponse response;
	if (!ParseManagedResponse(managedWriteLiveBindingValue(objectId, binding.bindingId.c_str(), std::string(fieldPath).c_str(), value.dump().c_str()), response))
	{
		if (outError)
		{
			*outError = "The managed live script write bridge returned an invalid payload.";
		}
		return false;
	}

	binding.lastBridgeMessage = response.message;
	if (!response.success)
	{
		if (outError)
		{
			*outError = response.message;
		}
		return false;
	}

	const ScriptFieldDescriptor* fieldDescriptor = binding.FindFieldDescriptor(fieldPath);
	ScriptFieldValue runtimeOverride;
	runtimeOverride.fieldPath = std::string(fieldPath);
	runtimeOverride.fieldName = runtimeOverride.fieldPath;
	runtimeOverride.displayName = fieldDescriptor != nullptr ? fieldDescriptor->displayName : runtimeOverride.fieldPath;
	runtimeOverride.valueKind = fieldDescriptor != nullptr ? fieldDescriptor->valueKind : std::string{};
	runtimeOverride.value = response.value.is_null() ? value : response.value;
	runtimeOverride.compatibility = "RuntimeOnly";
	runtimeOverride.persistenceState = "RuntimeOnly";
	if (ScriptFieldValue* existingOverride = binding.FindRuntimeOverride(fieldPath))
	{
		*existingOverride = runtimeOverride;
	}
	else
	{
		binding.runtimeOverrides.push_back(std::move(runtimeOverride));
	}

	binding.runtimeOverrideCount = static_cast<int>(binding.runtimeOverrides.size());
	binding.runtimeAvailable = true;
	binding.status = "LiveReady";

	ScriptFieldSnapshot updatedSnapshot;
	updatedSnapshot.fieldPath = std::string(fieldPath);
	updatedSnapshot.value = response.value.is_null() ? value : response.value;
	updatedSnapshot.source = "RuntimeOverride";
	updatedSnapshot.refreshState = "Clean";
	updatedSnapshot.differsFromAuthored = true;
	updatedSnapshot.lastObservedVersion = response.snapshotVersion;

	auto snapshotIt = std::find_if(binding.liveSnapshots.begin(), binding.liveSnapshots.end(), [fieldPath](const ScriptFieldSnapshot& snapshot) {
		return snapshot.fieldPath == fieldPath;
	});
	if (snapshotIt != binding.liveSnapshots.end())
	{
		*snapshotIt = std::move(updatedSnapshot);
	}
	else
	{
		binding.liveSnapshots.push_back(std::move(updatedSnapshot));
	}

	return true;
}

void ScriptingSystem::RefreshBindingStatuses() {
	const std::string previousAssemblyId = m_LoadedGameplayAssemblyPath.string();
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>()) {
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>()) {
			auto guard = scriptComponent->acquireWriteLock();
			for (ScriptBinding& binding : scriptComponent->GetBindings()) {
				binding.fullTypeName = binding.className;
				if (IsScriptTypeAttachable(binding.className)) {
					binding.status = "Attached";
					RefreshBindingDescriptor(binding);
					MigrateBindingAuthoredValues(binding, scriptComponent->lastResolvedAssemblyId);
				} else {
					binding.status = "BrokenType";
					binding.fieldDescriptors.clear();
					binding.diagnostics.clear();
					binding.repairActions.clear();
					binding.diagnosticIds = { "binding.missingType" };
				}

				PopulateBindingRepairActions(binding);
			}

			scriptComponent->lastResolvedAssemblyId = previousAssemblyId;
			scriptComponent->SynchronizeStorageFromBindings();
		}
	}
}

void ScriptingSystem::RefreshBindingDescriptor(ScriptBinding& binding)
{
	binding.fieldDescriptors.clear();
	binding.diagnostics.clear();
	binding.diagnosticIds.clear();
	if (managedDescribeBindingType == nullptr || binding.className.empty())
	{
		return;
	}

	ManagedDescriptorResponse response;
	if (!ParseManagedResponse(managedDescribeBindingType(binding.className.c_str()), response))
	{
		binding.lastBridgeMessage = "Failed to parse the managed descriptor payload for " + binding.className + ".";
		return;
	}

	binding.fullTypeName = response.fullTypeName.empty() ? binding.className : response.fullTypeName;
	binding.fieldDescriptors = std::move(response.fields);
	binding.diagnostics = std::move(response.diagnostics);
	for (const ScriptFieldDiagnostic& diagnostic : binding.diagnostics)
	{
		if (!diagnostic.diagnosticId.empty())
		{
			binding.diagnosticIds.push_back(diagnostic.diagnosticId);
		}
	}
}

void ScriptingSystem::MigrateBindingAuthoredValues(ScriptBinding& binding, const std::string& previousAssemblyId)
{
	if (managedMigrateBindingAuthoredState == nullptr || binding.className.empty())
	{
		return;
	}

	ManagedMigrationResponse response;
	if (!ParseManagedResponse(managedMigrateBindingAuthoredState(binding.className.c_str(), nlohmann::json(binding.serializedValues).dump().c_str()), response))
	{
		binding.lastBridgeMessage = "Failed to parse the managed migration payload for " + binding.className + ".";
		return;
	}

	binding.serializedValues = std::move(response.fields);
	for (ScriptFieldValue& fieldValue : binding.serializedValues)
	{
		if (fieldValue.displayName.empty())
		{
			if (const ScriptFieldDescriptor* descriptor = binding.FindFieldDescriptor(fieldValue.fieldPath))
			{
				fieldValue.displayName = descriptor->displayName;
				if (fieldValue.valueKind.empty())
				{
					fieldValue.valueKind = descriptor->valueKind;
				}
			}
			else
			{
				fieldValue.displayName = fieldValue.fieldName;
			}
		}
	}

	binding.migrationResult.bindingId = binding.bindingId;
	binding.migrationResult.fromAssemblyId = previousAssemblyId;
	binding.migrationResult.toAssemblyId = m_LoadedGameplayAssemblyPath.string();
	binding.migrationResult.preservedFieldPaths = std::move(response.preservedFieldPaths);
	binding.migrationResult.convertedFieldPaths = std::move(response.convertedFieldPaths);
	binding.migrationResult.brokenFieldPaths = std::move(response.brokenFieldPaths);
	binding.migrationResult.hiddenStoredFieldPaths = std::move(response.hiddenStoredFieldPaths);
	binding.diagnostics.insert(binding.diagnostics.end(), response.diagnostics.begin(), response.diagnostics.end());
	for (const ScriptFieldDiagnostic& diagnostic : response.diagnostics)
	{
		if (!diagnostic.diagnosticId.empty())
		{
			binding.diagnosticIds.push_back(diagnostic.diagnosticId);
		}
	}
}

void ScriptingSystem::PopulateBindingRepairActions(ScriptBinding& binding)
{
	binding.repairActions.clear();

	for (const ScriptFieldValue& fieldValue : binding.serializedValues)
	{
		if (fieldValue.compatibility != "Broken")
		{
			continue;
		}

		binding.repairActions.push_back({
			BuildRepairActionId("clear", binding.bindingId, fieldValue.fieldPath),
			"ClearValue",
			"Clear Value",
			fieldValue.fieldPath,
			false
		});
		binding.repairActions.push_back({
			BuildRepairActionId("remove", binding.bindingId, fieldValue.fieldPath),
			"RemoveFieldData",
			"Remove Stale Data",
			fieldValue.fieldPath,
			false
		});
	}

	if (!binding.runtimeOverrides.empty())
	{
		binding.repairActions.push_back({
			BuildRepairActionId("applyRuntime", binding.bindingId, {}),
			"ApplyRuntimeToAuthored",
			"Apply Runtime Overrides",
			{},
			false
		});
		binding.repairActions.push_back({
			BuildRepairActionId("resetRuntime", binding.bindingId, {}),
			"ResetToAuthored",
			"Reset Runtime Overrides",
			{},
			false
		});
	}

	if (binding.status == "BrokenType")
	{
		binding.repairActions.push_back({
			BuildRepairActionId("removeBinding", binding.bindingId, {}),
			"RemoveBinding",
			"Remove Missing Script",
			{},
			false
		});
	}
}

void ScriptingSystem::RefreshWatchedBindingSnapshots(float deltaTime)
{
	std::optional<uint32_t> watchedObjectId;
	{
		std::scoped_lock lock(m_InspectorWatchMutex);
		if (!m_InspectorWatchState.watchedObjectId.has_value())
		{
			return;
		}

		m_InspectorWatchState.refreshCountdownSeconds -= deltaTime;
		if (m_InspectorWatchState.refreshCountdownSeconds > 0.0f)
		{
			return;
		}

		m_InspectorWatchState.refreshCountdownSeconds = kLiveInspectorRefreshSeconds;
		watchedObjectId = m_InspectorWatchState.watchedObjectId;
	}

	if (!watchedObjectId.has_value())
	{
		return;
	}

	auto watchedObject = ObjectManager::instance().getObject(static_cast<entt::entity>(*watchedObjectId));
	if (!watchedObject.has_value())
	{
		return;
	}

	ScriptComponent* scriptComponent = watchedObject->getComponent<ScriptComponent>();
	if (scriptComponent == nullptr)
	{
		return;
	}

	std::vector<PendingBindingSnapshotRefresh> pendingRefreshes;
	{
		auto guard = scriptComponent->acquireReadLock();
		pendingRefreshes.reserve(scriptComponent->GetBindings().size());
		for (const ScriptBinding& binding : scriptComponent->GetBindings())
		{
			if (!binding.enabled || binding.className.empty())
			{
				continue;
			}

			pendingRefreshes.push_back({
				binding.bindingId,
				binding.className
			});
		}
	}

	// The managed live-snapshot bridge reads authored binding payload back
	// through ScriptGlue, so TerraEngine must not hold the component write lock
	// while requesting the snapshot or it will deadlock on the same mutex.
	for (const PendingBindingSnapshotRefresh& pendingRefresh : pendingRefreshes)
	{
		if (managedReadLiveBindingValues == nullptr || !m_GameplayAssemblyLoaded)
		{
			return;
		}

		ManagedSnapshotResponse response;
		const bool parsedResponse = ParseManagedResponse(
			managedReadLiveBindingValues(*watchedObjectId, pendingRefresh.bindingId.c_str(), pendingRefresh.className.c_str()),
			response);

		auto guard = scriptComponent->acquireWriteLock();
		ScriptBinding* binding = scriptComponent->FindBinding(pendingRefresh.bindingId);
		if (binding == nullptr || !binding->enabled || binding->className != pendingRefresh.className)
		{
			continue;
		}

		if (!parsedResponse)
		{
			binding->lastBridgeMessage = "Failed to parse the managed live snapshot payload for " + pendingRefresh.className + ".";
			continue;
		}

		binding->fullTypeName = response.fullTypeName.empty() ? binding->className : response.fullTypeName;
		binding->status = response.status.empty() ? "Attached" : response.status;
		binding->runtimeAvailable = response.runtimeAvailable;
		binding->runtimeOverrideCount = response.runtimeOverrideCount;
		binding->liveSnapshots = std::move(response.fields);

		// Snapshot refresh should only replace transient runtime diagnostics.
		// Static descriptor warnings already live on the binding and would grow
		// forever if TerraEngine appended them again every refresh tick.
		binding->diagnostics.erase(
			std::remove_if(binding->diagnostics.begin(), binding->diagnostics.end(), [](const ScriptFieldDiagnostic& diagnostic) {
				return IsRuntimeSnapshotDiagnostic(diagnostic);
			}),
			binding->diagnostics.end());
		for (const ScriptFieldDiagnostic& diagnostic : response.diagnostics)
		{
			if (!IsRuntimeSnapshotDiagnostic(diagnostic))
			{
				continue;
			}

			auto existingDiagnostic = std::find_if(binding->diagnostics.begin(), binding->diagnostics.end(), [&diagnostic](const ScriptFieldDiagnostic& existing) {
				return existing.diagnosticId == diagnostic.diagnosticId
					&& existing.code == diagnostic.code
					&& existing.fieldPath == diagnostic.fieldPath;
			});
			if (existingDiagnostic == binding->diagnostics.end())
			{
				binding->diagnostics.push_back(diagnostic);
			}
		}

		if (!binding->runtimeAvailable && binding->status.empty())
		{
			binding->status = "InstanceUnavailable";
		}

		PopulateBindingRepairActions(*binding);
	}
}

void ScriptingSystem::ClearRuntimeInspectorState()
{
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>())
	{
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>())
		{
			auto guard = scriptComponent->acquireWriteLock();
			scriptComponent->ClearRuntimeState();
			for (ScriptBinding& binding : scriptComponent->GetBindings())
			{
				if (binding.status == "LiveReady" || binding.status == "InstanceUnavailable")
				{
					binding.status = IsScriptTypeAttachable(binding.className) ? "Attached" : "BrokenType";
				}
			}
		}
	}
}

void ScriptingSystem::SetAllBindingStatuses(const std::string& statusText) {
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>()) {
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>()) {
			auto guard = scriptComponent->acquireWriteLock();
			for (ScriptBinding& binding : scriptComponent->GetBindings()) {
				binding.status = statusText;
			}
			scriptComponent->lastResolvedAssemblyId.clear();
			scriptComponent->SynchronizeStorageFromBindings();
		}
	}
}

std::filesystem::path ScriptingSystem::ResolveManagedBridgeAssemblyPath() const {
	const fs::path current = fs::current_path();
	const fs::path candidates[] = {
		current / "Managed" / "TerraEngine.CSharp.dll",
		current / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll"
	};

	for (const fs::path& candidate : candidates) {
		if (fs::exists(candidate)) {
			return candidate;
		}
	}

	return candidates[0];
}
