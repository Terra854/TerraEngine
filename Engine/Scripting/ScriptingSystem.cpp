#include "ScriptingSystem.h"

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "DotNet/DotNetHost.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <future>
#include <stdexcept>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

namespace
{
	constexpr float kLiveInspectorRefreshSeconds = 0.25f;
	constexpr std::size_t kMaxExportFieldValueBytes = 256;

	uint64_t FnvHash64(const std::string& text) {
		const uint64_t FnvOffsetBasis = 14695981039346656037ULL;
		const uint64_t FnvPrime = 1099511628211ULL;
		uint64_t hash = FnvOffsetBasis;
		for (char c : text) {
			hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
			hash *= FnvPrime;
		}
		return hash;
	}

	std::string ExtractExportFieldName(const ExportFieldDescriptor& descriptor)
	{
		std::size_t nameLength = 0;
		while (nameLength < sizeof(descriptor.nameUtf8) && descriptor.nameUtf8[nameLength] != '\0')
		{
			++nameLength;
		}
		return std::string(descriptor.nameUtf8, descriptor.nameUtf8 + nameLength);
	}

	std::string ExtractRootFieldPath(std::string_view fieldPath)
	{
		const std::size_t dotIndex = fieldPath.find('.');
		const std::size_t bracketIndex = fieldPath.find('[');
		std::size_t cutIndex = std::string_view::npos;
		if (dotIndex != std::string_view::npos)
		{
			cutIndex = dotIndex;
		}
		if (bracketIndex != std::string_view::npos)
		{
			cutIndex = cutIndex == std::string_view::npos ? bracketIndex : std::min(cutIndex, bracketIndex);
		}

		return cutIndex == std::string_view::npos
			? std::string(fieldPath)
			: std::string(fieldPath.substr(0, cutIndex));
	}

	bool TryBuildExportFieldBuffer(const ScriptFieldValue& fieldValue, const ExportFieldDescriptor& descriptor, std::array<std::byte, kMaxExportFieldValueBytes>& buffer)
	{
		buffer.fill(std::byte{ 0 });
		const ExportFieldValueKind valueKind = static_cast<ExportFieldValueKind>(descriptor.valueKind);

		try
		{
			switch (valueKind)
			{
			case ExportFieldValueKind::Bool:
			{
				if (!fieldValue.value.is_boolean() || descriptor.valueSize != sizeof(bool))
				{
					return false;
				}

				const bool value = fieldValue.value.get<bool>();
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Int:
			{
				if (!fieldValue.value.is_number_integer() || descriptor.valueSize != sizeof(int))
				{
					return false;
				}

				const int value = fieldValue.value.get<int>();
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Float:
			{
				if (!fieldValue.value.is_number() || descriptor.valueSize != sizeof(float))
				{
					return false;
				}

				const float value = fieldValue.value.get<float>();
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::EntityReference:
			{
				if (!fieldValue.value.is_number_unsigned() && !fieldValue.value.is_number_integer())
				{
					return false;
				}
				if (descriptor.valueSize != sizeof(uint32_t))
				{
					return false;
				}

				const uint32_t value = fieldValue.value.get<uint32_t>();
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Vector2:
			{
				if (!fieldValue.value.is_array() || fieldValue.value.size() != 2 || descriptor.valueSize != sizeof(glm::vec2))
				{
					return false;
				}

				const glm::vec2 value(
					fieldValue.value[0].get<float>(),
					fieldValue.value[1].get<float>());
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Vector3:
			{
				if (!fieldValue.value.is_array() || fieldValue.value.size() != 3 || descriptor.valueSize != sizeof(glm::vec3))
				{
					return false;
				}

				const glm::vec3 value(
					fieldValue.value[0].get<float>(),
					fieldValue.value[1].get<float>(),
					fieldValue.value[2].get<float>());
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Vector4:
			{
				if (!fieldValue.value.is_array() || fieldValue.value.size() != 4 || descriptor.valueSize != sizeof(glm::vec4))
				{
					return false;
				}

				const glm::vec4 value(
					fieldValue.value[0].get<float>(),
					fieldValue.value[1].get<float>(),
					fieldValue.value[2].get<float>(),
					fieldValue.value[3].get<float>());
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::Quaternion:
			{
				if (!fieldValue.value.is_array() || fieldValue.value.size() != 4 || descriptor.valueSize != sizeof(glm::quat))
				{
					return false;
				}

				const glm::quat value(
					fieldValue.value[3].get<float>(),
					fieldValue.value[0].get<float>(),
					fieldValue.value[1].get<float>(),
					fieldValue.value[2].get<float>());
				std::memcpy(buffer.data(), &value, sizeof(value));
				return true;
			}
			case ExportFieldValueKind::String:
			{
				if (descriptor.valueSize <= 0 || static_cast<std::size_t>(descriptor.valueSize) > buffer.size())
				{
					return false;
				}

				const std::string value = fieldValue.value.is_null()
					? std::string{}
					: fieldValue.value.is_string() ? fieldValue.value.get<std::string>() : std::string{};
				if (!fieldValue.value.is_null() && !fieldValue.value.is_string())
				{
					return false;
				}

				const std::size_t copyLength = std::min<std::size_t>(value.size(), static_cast<std::size_t>(descriptor.valueSize - 1));
				std::memcpy(buffer.data(), value.data(), copyLength);
				return true;
			}
			}
		}
		catch (const std::exception&)
		{
			return false;
		}

		return false;
	}

	void ApplyExportBindingValues(const ManagedVTable& vtable, const ScriptBinding& binding, void* handle, uint64_t scriptId)
	{
		if (handle == nullptr || vtable.setFieldValue == nullptr || vtable.getFieldCount == nullptr || vtable.getFieldDescriptors == nullptr)
		{
			return;
		}

		const int32_t fieldCount = vtable.getFieldCount(scriptId);
		if (fieldCount <= 0)
		{
			return;
		}

		std::vector<ExportFieldDescriptor> descriptors(static_cast<std::size_t>(fieldCount));
		vtable.getFieldDescriptors(scriptId, descriptors.data(), fieldCount);

		for (const ScriptFieldValue& fieldValue : binding.serializedValues)
		{
			const std::string rootFieldName = ExtractRootFieldPath(fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath);
			const uint64_t fieldNameHash = FnvHash64(rootFieldName);
			auto descriptorIt = std::find_if(descriptors.begin(), descriptors.end(), [&](const ExportFieldDescriptor& descriptor) {
				return descriptor.nameHash == fieldNameHash || ExtractExportFieldName(descriptor) == rootFieldName;
			});
			if (descriptorIt == descriptors.end())
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Scripting] Export hydration skipped unknown field '" << rootFieldName << "' on binding " << binding.bindingId);
				continue;
			}

			std::array<std::byte, kMaxExportFieldValueBytes> valueBuffer{};
			if (!TryBuildExportFieldBuffer(fieldValue, *descriptorIt, valueBuffer))
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Scripting] Export hydration could not decode field '" << rootFieldName << "' on binding " << binding.bindingId);
				continue;
			}

			vtable.setFieldValue(handle, descriptorIt->fieldIndex, valueBuffer.data(), descriptorIt->valueSize);
		}
	}

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

void ScriptingSystem::ConfigureExportRuntime(const std::filesystem::path& nativeAotLibraryPath) {
	// Store the absolute path up front so later system initialization does not
	// depend on whatever the current working directory happens to be then.
	if (nativeAotLibraryPath.empty()) {
		m_ConfiguredExportRuntimePath.reset();
		return;
	}

	m_ConfiguredExportRuntimePath = fs::absolute(nativeAotLibraryPath);
}

void ScriptingSystem::Initialize() {
	TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

	if (m_ExportMode) {
		TERRAENGINE_LOG(LogLevel::Info, "Scripting System already initialized in export mode. Skipping standard hostfxr initialization.");
		return;
	}

	if (m_ConfiguredExportRuntimePath.has_value()) {
		// Export builds still discover the NativeAOT library in the launcher,
		// but the actual runtime load now happens here so scripting is brought
		// online with the rest of the engine systems instead of before them.
		ManagedVTable vtable{};
		if (!DotNetHost::Instance().InitializeExportRuntime(*m_ConfiguredExportRuntimePath, &vtable)) {
			const std::string hostDiagnostic = DotNetHost::Instance().GetLastNativeError();
			throw std::runtime_error(hostDiagnostic.empty()
				? "Failed to initialize NativeAOT export runtime from " + m_ConfiguredExportRuntimePath->string()
				: hostDiagnostic);
		}

		InitializeExportMode(vtable);
		return;
	}

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
	m_LastDeltaTime = deltaTime;
	if (m_ExportMode) {
		UpdateExportMode(deltaTime);
		return;
	}

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

	// Reset export-mode caches so a later engine session in the same process
	// can reinitialize the NativeAOT scripting path cleanly.
	std::vector<void*> handlesToDestroy;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		for (const ExportScriptInstance& instance : m_ExportInstances)
		{
			handlesToDestroy.push_back(instance.handle);
		}
		m_ExportInstances.clear();
	}
	for (void* handle : handlesToDestroy)
	{
		if (m_ExportVTable.destroyScript != nullptr && handle != nullptr)
		{
			m_ExportVTable.destroyScript(handle);
		}
	}
	m_ExportVTable = {};
	m_ExportMode = false;

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

void ScriptingSystem::InitializeExportMode(const ManagedVTable& vtable) {
	TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System in export mode (VTable v" << vtable.version << ")...");

	// Copy the VTable so the scripting system owns its own snapshot.
	m_ExportVTable = vtable;
	m_ExportMode = true;

	// Log function pointers to detect potential marshalling/corruption issues.
	TERRAENGINE_LOG(LogLevel::Debug, "VTable Function Pointers:");
	TERRAENGINE_LOG(LogLevel::Debug, "  getScriptCount: " << (void*)m_ExportVTable.getScriptCount);
	TERRAENGINE_LOG(LogLevel::Debug, "  getScriptIds: " << (void*)m_ExportVTable.getScriptIds);
	TERRAENGINE_LOG(LogLevel::Debug, "  createScript: " << (void*)m_ExportVTable.createScript);
	TERRAENGINE_LOG(LogLevel::Debug, "  invokeUpdate: " << (void*)m_ExportVTable.invokeUpdate);

	// Query the registry to learn what script types are available.
	if (m_ExportVTable.getScriptCount != nullptr) {
		const int32_t count = m_ExportVTable.getScriptCount();
		TERRAENGINE_LOG(LogLevel::Info, "Export runtime provides " << count << " registered script type(s).");
	}
	else {
		TERRAENGINE_LOG(LogLevel::Error, "Export VTable getScriptCount pointer is null!");
	}

	m_Running = true;
	TERRAENGINE_LOG(LogLevel::Info, "Scripting System (export mode) initialized successfully.");
}

void ScriptingSystem::UpdateExportMode(float deltaTime) {
	// Export-mode update dispatches through the VTable function pointers.
	// The threading model mirrors the editor runtime: bindings are gathered,
	// batched across the thread pool, and waited on before returning.
	if (!m_ExportMode || !m_Running) {
		return;
	}

	if (m_Paused) {
		if (m_StepFrames > 0) {
			m_StepFrames--;
		} else {
			return;
		}
	}

	// Collect active script bindings from the ECS, same as editor mode.
	struct ExportBatchItem {
		uint32_t objectId = 0;
		uint64_t scriptId = 0;
		std::string bindingId;
		std::string className;
		std::vector<ScriptFieldValue> serializedValues;
	};

	std::vector<ExportBatchItem> allBindings;
	auto& objectManager = ObjectManager::instance();
	size_t objectsWithScripts = 0;
	for (Object object : objectManager.getObjectsWithComponents<ScriptComponent>()) {
		objectsWithScripts++;
		ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
		if (scriptComponent == nullptr || !scriptComponent->active) {
			continue;
		}

		auto guard = scriptComponent->acquireReadLock();
		for (const ScriptBinding& binding : scriptComponent->GetBindings()) {
			if (!binding.enabled) {
				continue;
			}

			if (binding.scriptId == 0) {
				TERRAENGINE_LOG(LogLevel::Error, "[Scripting] Export binding " << binding.bindingId << " on object " << static_cast<uint32_t>(object.EntityId()) << " is missing staged scriptId data.");
				continue;
			}

			ExportBatchItem item;
			item.objectId = static_cast<uint32_t>(object.EntityId());
			item.scriptId = binding.scriptId;
			item.bindingId = binding.bindingId;
			item.className = binding.className.empty() ? ("scriptId:" + std::to_string(binding.scriptId)) : binding.className;
			item.serializedValues = binding.serializedValues;
			allBindings.push_back(std::move(item));
		}
	}

	std::vector<ExportScriptInstance> staleInstances;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		auto staleBegin = std::remove_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
			const bool stillActive = std::any_of(allBindings.begin(), allBindings.end(), [&](const ExportBatchItem& item) {
				return item.objectId == instance.objectId
					&& item.bindingId == instance.bindingId
					&& item.scriptId == instance.scriptId;
			});
			if (!stillActive) {
				staleInstances.push_back(instance);
			}
			return !stillActive;
		});
		m_ExportInstances.erase(staleBegin, m_ExportInstances.end());
	}

	for (const ExportScriptInstance& instance : staleInstances) {
		if (instance.handle == nullptr) {
			continue;
		}
		if (instance.started && m_ExportVTable.invokeDisable != nullptr) {
			m_ExportVTable.invokeDisable(instance.handle);
		}
		if (m_ExportVTable.destroyScript != nullptr) {
			m_ExportVTable.destroyScript(instance.handle);
		}
	}

	if (allBindings.empty()) {
		if (objectsWithScripts > 0) {
			static bool loggedOnce = false;
			if (!loggedOnce) {
				TERRAENGINE_LOG(LogLevel::Debug, "[Scripting] Found " << objectsWithScripts << " objects with ScriptComponents, but 0 enabled bindings.");
				loggedOnce = true;
			}
		}
		return;
	}

	// Create missing instances first so gameplay callbacks always see hydrated
	// scene values before Awake and Start begin executing.
	std::vector<ExportBatchItem> pendingCreations;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		for (const ExportBatchItem& item : allBindings) {
			auto instanceIt = std::find_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
				return instance.objectId == item.objectId
					&& instance.bindingId == item.bindingId
					&& instance.scriptId == item.scriptId;
			});
			if (instanceIt == m_ExportInstances.end()) {
				pendingCreations.push_back(item);
			}
		}
	}

	for (const ExportBatchItem& item : pendingCreations) {
		if (m_ExportVTable.createScript == nullptr) {
			break;
		}

		TERRAENGINE_LOG(LogLevel::Debug, "[Scripting] Creating instance for " << item.className << " (ID: " << item.scriptId << ") on object " << item.objectId);
		void* handle = m_ExportVTable.createScript(item.scriptId, item.objectId);
		if (handle == nullptr) {
			TERRAENGINE_LOG(LogLevel::Error, "[Scripting] createScript returned NULL for " << item.className << " (ID: " << item.scriptId << ")");
			continue;
		}

		ScriptBinding stagedBinding;
		stagedBinding.bindingId = item.bindingId;
		stagedBinding.className = item.className;
		stagedBinding.scriptId = item.scriptId;
		stagedBinding.serializedValues = item.serializedValues;
		ApplyExportBindingValues(m_ExportVTable, stagedBinding, handle, item.scriptId);
		if (m_ExportVTable.invokeAwake != nullptr) {
			m_ExportVTable.invokeAwake(handle);
		}

		ExportScriptInstance instance;
		instance.handle = handle;
		instance.scriptId = item.scriptId;
		instance.objectId = item.objectId;
		instance.bindingId = item.bindingId;
		instance.started = false;

		std::scoped_lock lock(m_ExportInstancesMutex);
		m_ExportInstances.push_back(std::move(instance));
	}

	// Batch and dispatch through the thread pool.
	const std::size_t threadCount = std::max<std::size_t>(1, ThreadPool::instance().getThreadCount());
	const std::size_t targetTaskCount = threadCount * 4;
	std::size_t batchSize = allBindings.size() / std::max<std::size_t>(1, targetTaskCount);
	batchSize = std::max<std::size_t>(8, batchSize);

	std::vector<std::future<void>> futures;
	std::vector<ExportBatchItem> currentBatch;
	currentBatch.reserve(batchSize);

	auto dispatchBatch = [&](std::vector<ExportBatchItem> items) {
		auto task = ThreadPool::instance().createTask([items = std::move(items), this, deltaTime]() {
			std::scoped_lock lock(m_ExportInstancesMutex);
			for (const ExportBatchItem& entry : items) {
				auto it = std::find_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& inst) {
					return inst.objectId == entry.objectId
						&& inst.bindingId == entry.bindingId
						&& inst.scriptId == entry.scriptId;
				});

				if (it != m_ExportInstances.end()) {
					if (!it->started) {
						if (m_ExportVTable.invokeStart) m_ExportVTable.invokeStart(it->handle);
						it->started = true;
					}
					if (m_ExportVTable.invokeUpdate) m_ExportVTable.invokeUpdate(it->handle, deltaTime);
				}
			}
		});
		futures.push_back(task->getFuture());
		ThreadPool::instance().enqueue(task);
	};

	for (const ExportBatchItem& item : allBindings) {
		currentBatch.push_back(item);
		if (currentBatch.size() >= batchSize) {
			dispatchBatch(std::move(currentBatch));
			currentBatch = std::vector<ExportBatchItem>();
			currentBatch.reserve(batchSize);
		}
	}

	if (!currentBatch.empty()) {
		dispatchBatch(std::move(currentBatch));
	}

	for (std::future<void>& future : futures) {
		future.get();
	}
}
