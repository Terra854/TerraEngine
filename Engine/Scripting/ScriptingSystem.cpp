// ScriptingSystem.cpp — Contains the Scripting System system state, startup/shutdown hooks, and per-frame work implementation for native script glue, exported C# vtables, and scripting system lifecycle state.

#include "ScriptingSystem.h"

#include "Core/Logger/Logger.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "DotNet/DotNetHost.h"
#include "Profiling/ProfilerScope.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <future>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace fs = std::filesystem;

namespace
{
	constexpr float kLiveInspectorRefreshSeconds = 0.25f;
	constexpr std::size_t kMaxExportFieldValueBytes = 256;
	std::atomic_bool g_ScriptingDiagnosticsRequested{ false };
	std::atomic_size_t g_ScriptingBridgeCacheHits{ 0 };
	std::atomic_size_t g_ScriptingBridgeCacheMisses{ 0 };
	std::atomic_size_t g_ScriptingBridgeCallsAvoided{ 0 };
	thread_local std::optional<ScriptingSystem::ScriptDispatchContext> g_CurrentScriptDispatchContext;

	fs::path ResolveExecutableDirectory()
	{
		const fs::path executableDirectory = TerraEngine::PlatformUtils::GetExecutableDirectory();
		if (!executableDirectory.empty())
		{
			return executableDirectory;
		}

		return fs::current_path();
	}

	std::string ToString(ScriptingSystem::ScriptExecutionClass executionClass)
	{
		switch (executionClass)
		{
		case ScriptingSystem::ScriptExecutionClass::WorkerSafe:
			return "worker-safe";
		case ScriptingSystem::ScriptExecutionClass::Protected:
			return "protected";
		case ScriptingSystem::ScriptExecutionClass::Skipped:
			return "skipped";
		case ScriptingSystem::ScriptExecutionClass::Failed:
			return "failed";
		}
		return "unknown";
	}

	std::string BuildScriptDiagnosticLabel(std::uint32_t objectId, std::string_view bindingId, std::string_view scriptType)
	{
		std::ostringstream stream;
		stream << "Object " << objectId << " :: ";
		stream << (scriptType.empty() ? "<unresolved script>" : scriptType);
		if (!bindingId.empty())
		{
			stream << " [" << bindingId << "]";
		}
		return stream.str();
	}

	ScriptingSystem::ScriptDispatchContext BuildDispatchContext(
		const ScriptingSystem::ScriptWorkItem& item,
		bool workerDispatch,
		bool exportMode,
		std::uint64_t frameOrder)
	{
		ScriptingSystem::ScriptDispatchContext context;
		context.workerDispatch = workerDispatch;
		context.exportMode = exportMode;
		context.sourceObjectId = item.objectId;
		context.sourceObjectUuid = item.objectUuid;
		context.sourceBindingId = item.bindingId;
		context.frameOrder = frameOrder;
		context.objectGeneration = item.objectGeneration;
		context.componentGeneration = item.componentGeneration;
		context.bindingGeneration = item.bindingGeneration;
		return context;
	}

	class ScopedScriptDispatchContext
	{
	public:
		explicit ScopedScriptDispatchContext(ScriptingSystem::ScriptDispatchContext context)
		{
			// Export lifecycle callbacks can now run inside worker-eligible
			// scripting batches. Push the same thread-local context used by
			// managed Update so component setters defer mutations instead of
			// writing live scene state from a worker thread.
			ScriptingSystem::SetCurrentScriptDispatchContext(std::move(context));
		}

		~ScopedScriptDispatchContext()
		{
			ScriptingSystem::ClearCurrentScriptDispatchContext();
		}

		ScopedScriptDispatchContext(const ScopedScriptDispatchContext&) = delete;
		ScopedScriptDispatchContext& operator=(const ScopedScriptDispatchContext&) = delete;
	};

	// FnvHash64 is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	uint64_t FnvHash64(const std::string& text) {
		const uint64_t FnvOffsetBasis = 14695981039346656037ULL;
		const uint64_t FnvPrime = 1099511628211ULL;
		uint64_t hash = FnvOffsetBasis;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (char c : text) {
			hash ^= static_cast<uint64_t>(static_cast<unsigned char>(c));
			hash *= FnvPrime;
		}
		return hash;
	}

	// ExtractExportFieldName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string ExtractExportFieldName(const ExportFieldDescriptor& descriptor)
	{
		std::size_t nameLength = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (nameLength < sizeof(descriptor.nameUtf8) && descriptor.nameUtf8[nameLength] != '\0')
		{
			++nameLength;
		}
		return std::string(descriptor.nameUtf8, descriptor.nameUtf8 + nameLength);
	}

	// ExtractRootFieldPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string ExtractRootFieldPath(std::string_view fieldPath)
	{
		const std::size_t dotIndex = fieldPath.find('.');
		const std::size_t bracketIndex = fieldPath.find('[');
		std::size_t cutIndex = std::string_view::npos;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (dotIndex != std::string_view::npos)
		{
			cutIndex = dotIndex;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (bracketIndex != std::string_view::npos)
		{
			cutIndex = cutIndex == std::string_view::npos ? bracketIndex : std::min(cutIndex, bracketIndex);
		}

		return cutIndex == std::string_view::npos
			? std::string(fieldPath)
			: std::string(fieldPath.substr(0, cutIndex));
	}

	// TryBuildExportFieldBuffer is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryBuildExportFieldBuffer(const ScriptFieldValue& fieldValue, const ExportFieldDescriptor& descriptor, std::array<std::byte, kMaxExportFieldValueBytes>& buffer)
	{
		buffer.fill(std::byte{ 0 });
		const ExportFieldValueKind valueKind = static_cast<ExportFieldValueKind>(descriptor.valueKind);

		try
		{
			// Dispatch by explicit engine state so each case documents one supported behavior.
			switch (valueKind)
			{
			case ExportFieldValueKind::Bool:
			{
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (!fieldValue.value.is_number_unsigned() && !fieldValue.value.is_number_integer())
				{
					return false;
				}
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
				if (descriptor.valueSize <= 0 || static_cast<std::size_t>(descriptor.valueSize) > buffer.size())
				{
					return false;
				}

				const std::string value = fieldValue.value.is_null()
					? std::string{}
					: fieldValue.value.is_string() ? fieldValue.value.get<std::string>() : std::string{};
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// ApplyExportBindingValues is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ApplyExportBindingValues(const ManagedVTable& vtable, const ScriptBinding& binding, void* handle, uint64_t scriptId)
	{
		if (handle == nullptr || vtable.setFieldValue == nullptr || vtable.getFieldCount == nullptr || vtable.getFieldDescriptors == nullptr)
		{
			return;
		}

		const int32_t fieldCount = vtable.getFieldCount(scriptId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (fieldCount <= 0)
		{
			return;
		}

		std::vector<ExportFieldDescriptor> descriptors(static_cast<std::size_t>(fieldCount));
		vtable.getFieldDescriptors(scriptId, descriptors.data(), fieldCount);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ScriptFieldValue& fieldValue : binding.serializedValues)
		{
			const std::string rootFieldName = ExtractRootFieldPath(fieldValue.fieldPath.empty() ? fieldValue.fieldName : fieldValue.fieldPath);
			const uint64_t fieldNameHash = FnvHash64(rootFieldName);
			auto descriptorIt = std::find_if(descriptors.begin(), descriptors.end(), [&](const ExportFieldDescriptor& descriptor) {
				return descriptor.nameHash == fieldNameHash || ExtractExportFieldName(descriptor) == rootFieldName;
			});
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (descriptorIt == descriptors.end())
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Scripting] Export hydration skipped unknown field '" << rootFieldName << "' on binding " << binding.bindingId);
				continue;
			}

			std::array<std::byte, kMaxExportFieldValueBytes> valueBuffer{};
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!TryBuildExportFieldBuffer(fieldValue, *descriptorIt, valueBuffer))
			{
				TERRAENGINE_LOG(LogLevel::Warning, "[Scripting] Export hydration could not decode field '" << rootFieldName << "' on binding " << binding.bindingId);
				continue;
			}

			vtable.setFieldValue(handle, descriptorIt->fieldIndex, valueBuffer.data(), descriptorIt->valueSize);
		}
	}

	// struct ManagedDescriptorResponse groups related state and operations so callers have one documented contract.
	struct ManagedDescriptorResponse
	{
		std::string fullTypeName;
		std::vector<ScriptFieldDescriptor> fields;
		std::vector<ScriptFieldDiagnostic> diagnostics;
	};

	// struct ManagedSnapshotResponse groups related state and operations so callers have one documented contract.
	struct ManagedSnapshotResponse
	{
		std::string bindingId;
		std::string fullTypeName;
		std::string status = "Attached";
		bool runtimeAvailable = false;
		int runtimeOverrideCount = 0;
		std::vector<ScriptFieldSnapshot> fields;
		std::vector<ScriptFieldDiagnostic> diagnostics;
		std::uint64_t snapshotVersion = 0;
	};

	// struct ManagedWriteResponse groups related state and operations so callers have one documented contract.
	struct ManagedWriteResponse
	{
		bool success = false;
		std::string bindingId;
		std::string fieldPath;
		std::string message;
		nlohmann::json value;
		std::uint64_t snapshotVersion = 0;
	};

	// struct PendingBindingSnapshotRefresh groups related state and operations so callers have one documented contract.
	struct PendingBindingSnapshotRefresh
	{
		std::string bindingId;
		std::string className;
	};

	// IsRuntimeSnapshotDiagnostic is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsRuntimeSnapshotDiagnostic(const ScriptFieldDiagnostic& diagnostic)
	{
		return diagnostic.code == "InstanceUnavailable"
			|| diagnostic.code == "BridgeFailure"
			|| diagnostic.code == "InvalidType";
	}

	// struct ManagedMigrationResponse groups related state and operations so callers have one documented contract.
	struct ManagedMigrationResponse
	{
		std::vector<ScriptFieldValue> fields;
		std::vector<std::string> preservedFieldPaths;
		std::vector<std::string> convertedFieldPaths;
		std::vector<std::string> brokenFieldPaths;
		std::vector<std::string> hiddenStoredFieldPaths;
		std::vector<ScriptFieldDiagnostic> diagnostics;
	};

	// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void from_json(const nlohmann::json& jsonValue, ManagedDescriptorResponse& response)
	{
		response.fullTypeName = jsonValue.value("fullTypeName", std::string{});
		response.fields = jsonValue.value("fields", std::vector<ScriptFieldDescriptor>{});
		response.diagnostics = jsonValue.value("diagnostics", std::vector<ScriptFieldDiagnostic>{});
	}

	// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void from_json(const nlohmann::json& jsonValue, ManagedSnapshotResponse& response)
	{
		response.bindingId = jsonValue.value("bindingId", std::string{});
		response.fullTypeName = jsonValue.value("fullTypeName", std::string{});
		response.status = jsonValue.value("status", std::string{ "Attached" });
		response.runtimeAvailable = jsonValue.value("runtimeAvailable", false);
		response.runtimeOverrideCount = jsonValue.value("runtimeOverrideCount", 0);
		response.fields = jsonValue.value("fields", std::vector<ScriptFieldSnapshot>{});
		response.diagnostics = jsonValue.value("diagnostics", std::vector<ScriptFieldDiagnostic>{});
		response.snapshotVersion = jsonValue.value("snapshotVersion", static_cast<std::uint64_t>(0));
	}

	// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void from_json(const nlohmann::json& jsonValue, ManagedWriteResponse& response)
	{
		response.success = jsonValue.value("success", false);
		response.bindingId = jsonValue.value("bindingId", std::string{});
		response.fieldPath = jsonValue.value("fieldPath", std::string{});
		response.message = jsonValue.value("message", std::string{});
		response.value = jsonValue.contains("value") ? jsonValue["value"] : nlohmann::json{};
		response.snapshotVersion = jsonValue.value("snapshotVersion", static_cast<std::uint64_t>(0));
	}

	// from_json is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// ParseManagedResponse is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// BuildRepairActionId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string BuildRepairActionId(std::string_view kind, std::string_view bindingId, std::string_view targetPath)
	{
		return std::string(kind) + ":" + std::string(bindingId) + ":" + std::string(targetPath);
	}
}

// ScriptingSystem::Instance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ScriptingSystem& ScriptingSystem::Instance() {
	static ScriptingSystem instance;
	return instance;
}

bool ScriptingSystem::IsScriptingDiagnosticsRequested() const
{
	return ScriptingDiagnosticsRequested();
}

void ScriptingSystem::SetScriptingDiagnosticsRequested(bool requested)
{
	g_ScriptingDiagnosticsRequested.store(requested, std::memory_order_relaxed);
}

bool ScriptingSystem::ScriptingDiagnosticsRequested()
{
	return g_ScriptingDiagnosticsRequested.load(std::memory_order_relaxed);
}

std::optional<ScriptingSystem::ScriptDispatchContext> ScriptingSystem::CurrentScriptDispatchContext()
{
	return g_CurrentScriptDispatchContext;
}

void ScriptingSystem::SetCurrentScriptDispatchContext(ScriptDispatchContext context)
{
	g_CurrentScriptDispatchContext = std::move(context);
}

void ScriptingSystem::ClearCurrentScriptDispatchContext()
{
	g_CurrentScriptDispatchContext.reset();
}

void ScriptingSystem::ResetBridgeCacheCounters()
{
	g_ScriptingBridgeCacheHits.store(0, std::memory_order_relaxed);
	g_ScriptingBridgeCacheMisses.store(0, std::memory_order_relaxed);
	g_ScriptingBridgeCallsAvoided.store(0, std::memory_order_relaxed);
}

void ScriptingSystem::RecordBridgeCacheCounters(std::size_t hits, std::size_t misses, std::size_t avoidedCalls)
{
	g_ScriptingBridgeCacheHits.fetch_add(hits, std::memory_order_relaxed);
	g_ScriptingBridgeCacheMisses.fetch_add(misses, std::memory_order_relaxed);
	g_ScriptingBridgeCallsAvoided.fetch_add(avoidedCalls, std::memory_order_relaxed);
}

std::vector<ScriptingSystem::ScriptBatch> ScriptingSystem::PlanScriptBatches(const ScriptUpdateWorkload& workload, std::size_t workerCount)
{
	std::vector<ScriptBatch> batches;
	if (workload.items.empty())
	{
		return batches;
	}

	std::vector<ScriptWorkItem> workerItems;
	std::vector<ScriptWorkItem> protectedItems;
	std::vector<ScriptWorkItem> skippedItems;
	workerItems.reserve(workload.items.size());
	protectedItems.reserve(workload.items.size());
	skippedItems.reserve(workload.items.size());

	for (const ScriptWorkItem& item : workload.items)
	{
		switch (item.executionClass)
		{
		case ScriptExecutionClass::WorkerSafe:
			workerItems.push_back(item);
			break;
		case ScriptExecutionClass::Protected:
			protectedItems.push_back(item);
			break;
		default:
			skippedItems.push_back(item);
			break;
		}
	}

	auto appendBatches = [&batches](std::vector<ScriptWorkItem>& items, ScriptExecutionClass executionClass, std::size_t chunkSize, std::string_view prefix, std::string protectedReason) {
		if (items.empty())
		{
			return;
		}
		const std::size_t safeChunkSize = std::max<std::size_t>(1, chunkSize);
		for (std::size_t begin = 0, batchIndex = 0; begin < items.size(); begin += safeChunkSize, ++batchIndex)
		{
			const std::size_t end = std::min(items.size(), begin + safeChunkSize);
			ScriptBatch batch;
			batch.batchId = std::string(prefix) + "-" + std::to_string(batchIndex);
			batch.executionClass = executionClass;
			batch.profilingLabel = "ScriptingSystem: " + batch.batchId;
			batch.mutationOrderBase = begin;
			batch.protectedReason = protectedReason;
			batch.items.assign(items.begin() + static_cast<std::ptrdiff_t>(begin), items.begin() + static_cast<std::ptrdiff_t>(end));
			batches.push_back(std::move(batch));
		}
	};

	const std::size_t threadCount = std::max<std::size_t>(1, workerCount);
	const std::size_t targetWorkerBatchCount = std::max<std::size_t>(1, threadCount * 4);
	const std::size_t workerBatchSize = std::max<std::size_t>(8, (workerItems.size() + targetWorkerBatchCount - 1) / targetWorkerBatchCount);
	appendBatches(workerItems, ScriptExecutionClass::WorkerSafe, workerBatchSize, "worker", {});
	appendBatches(protectedItems, ScriptExecutionClass::Protected, std::max<std::size_t>(1, protectedItems.size()), "protected", "Lifecycle or API access requires deterministic protected execution.");
	appendBatches(skippedItems, ScriptExecutionClass::Skipped, std::max<std::size_t>(1, skippedItems.size()), "skipped", "Script work item was not eligible for update this frame.");
	return batches;
}

std::string ScriptingSystem::FormatScriptingFrameResult(const ScriptingFrameResult& result)
{
	std::ostringstream stream;
	stream << "Scripting frame " << result.frameId
		<< ": updated=" << result.updatedScripts
		<< " failed=" << result.failedScripts
		<< " skipped=" << result.skippedScripts
		<< " worker=" << result.workerScripts
		<< " protected=" << result.protectedScripts
		<< " acceptedMutations=" << result.acceptedMutations
		<< " rejectedMutations=" << result.rejectedMutations
		<< " readView=" << result.readViewVersion
		<< " cacheHits=" << result.cacheHits
		<< " cacheMisses=" << result.cacheMisses
		<< " bridgeAvoided=" << result.bridgeCallsAvoided
		<< " typedMutations=" << result.typedMutations
		<< " jsonFallbacks=" << result.jsonMutationFallbacks
		<< " coalescedMutations=" << result.coalescedMutations;
	if (result.diagnosticsCollected)
	{
		stream << " diagnostics=on";
		for (const std::string& message : result.accessReport.messages)
		{
			stream << "\n  " << message;
		}
		for (const std::string& reason : result.accessReport.protectedReasons)
		{
			stream << "\n  protected: " << reason;
		}
	}
	else
	{
		stream << " diagnostics=off";
	}
	return stream.str();
}

// Converts a ScriptLifecycleState enum value into a user-friendly string representation.
// Input: The ScriptLifecycleState enum to format.
// Output: A string matching the state name (e.g. "Created", "Started", "Updating", "Disabled", "Destroyed").
std::string ScriptingSystem::FormatScriptLifecycleState(ScriptLifecycleState state)
{
	switch (state)
	{
	case ScriptLifecycleState::Created:
		return "Created";
	case ScriptLifecycleState::Started:
		return "Started";
	case ScriptLifecycleState::Updating:
		return "Updating";
	case ScriptLifecycleState::Disabled:
		return "Disabled";
	case ScriptLifecycleState::Destroyed:
		return "Destroyed";
	}
	return "Unknown";
}

// Classifies the status of a script instance based on its lifecycle and version tracking.
// Input: The ScriptInstanceRecord containing version numbers and lifecycle state.
// Output: A string describing the active status of the script. Returns "Stale (Update Pending)" if the 
// authored state version differs from what has been observed in the runtime assembly, or the lifecycle name.
std::string ScriptingSystem::ClassifyScriptInstanceStatus(const ScriptInstanceRecord& record)
{
	// If the user edited properties or recompiled scripts, the authored version increments.
	// We mark the script as Stale until the gameplay runtime catches up and processes the update.
	if (record.authoredStateVersion != record.runtimeObservedVersion)
	{
		return "Stale (Update Pending)";
	}

	switch (record.lifecycleState)
	{
	case ScriptLifecycleState::Created:
		return "Created";
	case ScriptLifecycleState::Started:
		return "Started";
	case ScriptLifecycleState::Updating:
		return "Ready"; // In updating state, the script is active and running correctly
	case ScriptLifecycleState::Disabled:
		return "Disabled";
	case ScriptLifecycleState::Destroyed:
		return "Destroyed";
	}
	return "Unknown";
}

ScriptingSystem::ScriptUpdateWorkload ScriptingSystem::BuildScriptUpdateWorkload(float deltaTime, bool exportMode)
{
	ScriptUpdateWorkload workload;
	workload.frameId = m_NextScriptingFrameId++;
	workload.deltaTime = deltaTime;
	workload.running = m_Running;
	workload.paused = m_Paused;
	workload.stepping = m_StepFrames > 0;
	workload.exportMode = exportMode;
	workload.diagnosticsRequested = ScriptingDiagnosticsRequested();
	workload.readViewVersion = m_NextScriptingReadViewVersion++;

	auto& objectManager = ObjectManager::instance();
	const std::uint64_t currentSceneRevision = objectManager.GetSpatialRevision();
	const bool rebuildActiveBindingIndex = m_ActiveScriptBindingIndexRevision != currentSceneRevision
		|| m_ActiveScriptBindingIndexExportMode != exportMode;
	if (rebuildActiveBindingIndex)
	{
		m_ActiveScriptBindingIndex.clear();
		m_ActiveScriptBindingIndexRevision = currentSceneRevision;
		m_ActiveScriptBindingIndexExportMode = exportMode;

		const std::vector<Object> objects = objectManager.SnapshotObjectsWithComponents<ScriptComponent>();
		m_ActiveScriptBindingIndex.reserve(objects.size());

		// Snapshot active script bindings into a main-thread index. Stable frames
		// can reuse these records directly, while structural or binding changes
		// rebuild the index before any worker script sees it.
		for (Object object : objects)
		{
			ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
			if (scriptComponent == nullptr || !scriptComponent->active)
			{
				continue;
			}

			auto guard = scriptComponent->acquireReadLock();
			for (const ScriptBinding& binding : scriptComponent->GetBindings())
			{
				if (!binding.enabled)
				{
					continue;
				}
				if (exportMode && binding.scriptId == 0)
				{
					TERRAENGINE_LOG(LogLevel::Error, "[Scripting] Export binding " << binding.bindingId << " on object " << static_cast<std::uint32_t>(object.EntityId()) << " is missing staged scriptId data.");
					continue;
				}
				if (!exportMode && binding.className.empty())
				{
					continue;
				}

				ScriptWorkItem item;
				item.objectId = static_cast<std::uint32_t>(object.EntityId());
				item.objectUuid = object.getUUID();
				item.bindingId = binding.bindingId;
				item.scriptType = binding.className.empty() ? ("scriptId:" + std::to_string(binding.scriptId)) : binding.className;
				item.scriptRuntimeId = binding.scriptId;
				item.authoredStateVersion = binding.authoredStateVersion;
				item.objectGeneration = objectManager.getObjectMetadataGeneration(object);
				item.componentGeneration = objectManager.getObjectComponentGeneration(object);
				item.bindingGeneration = objectManager.getObjectBindingGeneration(object);
				item.lifecycleState = ScriptLifecycleState::Updating;
				item.executionClass = ScriptExecutionClass::WorkerSafe;
				item.diagnosticLabel = BuildScriptDiagnosticLabel(item.objectId, item.bindingId, item.scriptType);
				item.serializedValues = binding.serializedValues;
				m_ActiveScriptBindingIndex.push_back(std::move(item));
			}
		}
	}

	bool activeBindingIndexStillValid = true;
	for (const ScriptWorkItem& cachedItem : m_ActiveScriptBindingIndex)
	{
		const auto object = objectManager.getObject(static_cast<entt::entity>(cachedItem.objectId));
		if (!object.has_value()
			|| objectManager.getObjectComponentGeneration(*object) != cachedItem.componentGeneration
			|| objectManager.getObjectBindingGeneration(*object) != cachedItem.bindingGeneration)
		{
			activeBindingIndexStillValid = false;
			break;
		}
	}
	if (!activeBindingIndexStillValid)
	{
		m_ActiveScriptBindingIndexRevision = std::numeric_limits<std::uint64_t>::max();
		return BuildScriptUpdateWorkload(deltaTime, exportMode);
	}

	workload.items.reserve(m_ActiveScriptBindingIndex.size());
	for (const ScriptWorkItem& cachedItem : m_ActiveScriptBindingIndex)
	{
		ScriptWorkItem item = cachedItem;
		item.readViewVersion = workload.readViewVersion;
		workload.items.push_back(std::move(item));
	}

	return workload;
}

ScriptingSystem::ScriptFrameReadView ScriptingSystem::BuildScriptFrameReadView(const ScriptUpdateWorkload& workload) const
{
	ScriptFrameReadView readView;
	readView.frameId = workload.frameId;
	readView.readViewVersion = workload.readViewVersion;
	readView.lookupIndexGeneration = ObjectManager::instance().GetSpatialRevision();
	readView.scriptRecords.reserve(workload.items.size());

	for (const ScriptWorkItem& item : workload.items)
	{
		ScriptInstanceRecord record;
		record.objectId = item.objectId;
		record.objectUuid = item.objectUuid;
		record.bindingId = item.bindingId;
		record.scriptType = item.scriptType;
		record.lifecycleState = item.lifecycleState;
		record.authoredStateVersion = item.authoredStateVersion;
		record.objectGeneration = item.objectGeneration;
		record.componentGeneration = item.componentGeneration;
		record.bindingGeneration = item.bindingGeneration;
		// Query runtimeObservedVersion from the live script binding's snapshotVersion via ObjectManager
		if (auto objectOpt = ObjectManager::instance().getObject(static_cast<entt::entity>(item.objectId)))
		{
			if (auto* scriptComponent = objectOpt->getComponent<ScriptComponent>())
			{
				auto guard = scriptComponent->acquireReadLock();
				if (const ScriptBinding* binding = scriptComponent->FindBinding(item.bindingId))
				{
					record.runtimeObservedVersion = binding->snapshotVersion;
				}
			}
		}

		readView.scriptRecords.push_back(std::move(record));
	}

	return readView;
}

ScriptingSystem::ScriptingFrameResult ScriptingSystem::DispatchScriptWorkload(const ScriptUpdateWorkload& workload)
{
	ScriptingFrameResult result;
	result.frameId = workload.frameId;
	result.readViewVersion = workload.readViewVersion;
	result.diagnosticsCollected = workload.diagnosticsRequested;
	result.accessReport.frameId = workload.frameId;
	result.accessReport.totalScripts = workload.items.size();

	if (!workload.running || workload.items.empty())
	{
		m_LatestFrameReadView = BuildScriptFrameReadView(workload);
		result.skippedScripts = workload.items.size();
		result.formattedSummary = FormatScriptingFrameResult(result);
		m_LatestFrameResult = result;
		return result;
	}

	const std::size_t threadCount = std::max<std::size_t>(1, ThreadPool::instance().getThreadCount());
	m_LatestFrameReadView = BuildScriptFrameReadView(workload);
	const std::vector<ScriptBatch> batches = PlanScriptBatches(workload, threadCount);
	ResetBridgeCacheCounters();
	std::atomic<std::size_t> updatedScripts{ 0 };
	std::atomic<std::size_t> failedScripts{ 0 };
	std::atomic<std::size_t> skippedScripts{ 0 };
	std::atomic<std::size_t> workerScripts{ 0 };
	std::atomic<std::size_t> protectedScripts{ 0 };
	std::mutex startedInstancesMutex;
	std::vector<ScriptWorkItem> startedInstances;

	TerraEngine::Scheduling::JobGraph graph("scripting-frame-" + std::to_string(workload.frameId), "Scripting Frame");
	std::vector<TerraEngine::Scheduling::DependencyHandle> batchJobs;
	batchJobs.reserve(batches.size());

	for (const ScriptBatch& batch : batches)
	{
		const std::uint64_t frameId = workload.frameId;
		const float frameDeltaTime = workload.deltaTime;
		const bool exportMode = workload.exportMode;
		const bool diagnosticsRequested = workload.diagnosticsRequested;
		TerraEngine::Scheduling::JobGraphJobDeclaration job;
		job.name = batch.batchId;
		job.executionPolicy = batch.executionClass == ScriptExecutionClass::WorkerSafe
			? TerraEngine::Scheduling::JobGraphExecutionPolicy::WorkerEligible
			: TerraEngine::Scheduling::JobGraphExecutionPolicy::Protected;
		job.cancellable = false;
		job.requiresSuccessfulDependencies = true;
		job.profilingLabel = batch.profilingLabel;
		job.work = [this, frameId, frameDeltaTime, exportMode, diagnosticsRequested, batch, &updatedScripts, &failedScripts, &skippedScripts, &workerScripts, &protectedScripts, &startedInstancesMutex, &startedInstances](TerraEngine::Scheduling::JobGraphContext&) {
			const bool workerDispatch = batch.executionClass == ScriptExecutionClass::WorkerSafe;
			if (!exportMode && managedUpdateScriptBatch != nullptr)
			{
				// Each worker owns its scratch vector, so batch dispatch can avoid
				// allocating a fresh managed-item buffer on every frame.
				thread_local std::vector<ManagedScriptBatchItem> managedItems;
				managedItems.clear();
				managedItems.reserve(batch.items.size());
				for (std::size_t itemIndex = 0; itemIndex < batch.items.size(); ++itemIndex)
				{
					const ScriptWorkItem& item = batch.items[itemIndex];
					if (item.executionClass == ScriptExecutionClass::Skipped)
					{
						skippedScripts.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					if (item.executionClass == ScriptExecutionClass::Failed)
					{
						failedScripts.fetch_add(1, std::memory_order_relaxed);
						continue;
					}

					ManagedScriptBatchItem managedItem;
					managedItem.objectId = item.objectId;
					managedItem.bindingId = item.bindingId.c_str();
					managedItem.scriptType = item.scriptType.c_str();
					managedItem.frameOrder = (frameId * 1000000ULL) + batch.mutationOrderBase + itemIndex;
					managedItem.objectGeneration = item.objectGeneration;
					managedItem.componentGeneration = item.componentGeneration;
					managedItem.bindingGeneration = item.bindingGeneration;
					managedItem.diagnosticsRequested = diagnosticsRequested ? 1 : 0;
					managedItem.workerDispatch = workerDispatch ? 1 : 0;
					managedItem.exportMode = 0;
					managedItem.authoredStateVersion = item.authoredStateVersion;
					managedItems.push_back(managedItem);
				}

				if (!managedItems.empty())
				{
					TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Managed Batch Worker Execution", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					managedUpdateScriptBatch(managedItems.data(), static_cast<std::int32_t>(managedItems.size()), frameDeltaTime, frameId);
					updatedScripts.fetch_add(managedItems.size(), std::memory_order_relaxed);
					if (workerDispatch)
					{
						workerScripts.fetch_add(managedItems.size(), std::memory_order_relaxed);
					}
					else
					{
						protectedScripts.fetch_add(managedItems.size(), std::memory_order_relaxed);
					}
				}
				return;
			}

			if (exportMode && m_ExportVTable.invokeUpdateBatch != nullptr)
			{
				std::vector<ExportScriptBatchItem> exportItems;
				exportItems.reserve(batch.items.size());
				for (std::size_t itemIndex = 0; itemIndex < batch.items.size(); ++itemIndex)
				{
					const ScriptWorkItem& item = batch.items[itemIndex];
					if (item.executionClass == ScriptExecutionClass::Skipped)
					{
						skippedScripts.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					if (item.executionClass == ScriptExecutionClass::Failed || item.runtimeHandle == nullptr)
					{
						failedScripts.fetch_add(1, std::memory_order_relaxed);
						continue;
					}
					if (!item.runtimeStarted && m_ExportVTable.invokeStart != nullptr)
					{
						ScopedScriptDispatchContext dispatchContext(BuildDispatchContext(
							item,
							workerDispatch,
							exportMode,
							(frameId * 1000000ULL) + batch.mutationOrderBase + itemIndex));
						m_ExportVTable.invokeStart(item.runtimeHandle);
						std::scoped_lock lock(startedInstancesMutex);
						startedInstances.push_back(item);
					}

					ExportScriptBatchItem exportItem{};
					exportItem.handle = item.runtimeHandle;
					exportItem.deltaTime = frameDeltaTime;
					exportItem.frameOrder = (frameId * 1000000ULL) + batch.mutationOrderBase + itemIndex;
					exportItem.objectGeneration = item.objectGeneration;
					exportItem.componentGeneration = item.componentGeneration;
					exportItem.bindingGeneration = item.bindingGeneration;
					exportItem.objectId = item.objectId;
					exportItem.workerDispatch = workerDispatch ? 1 : 0;
					exportItems.push_back(exportItem);
				}

				if (!exportItems.empty())
				{
					TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Export Batch Worker Execution", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					m_ExportVTable.invokeUpdateBatch(exportItems.data(), static_cast<std::int32_t>(exportItems.size()));
					updatedScripts.fetch_add(exportItems.size(), std::memory_order_relaxed);
					if (workerDispatch)
					{
						workerScripts.fetch_add(exportItems.size(), std::memory_order_relaxed);
					}
					else
					{
						protectedScripts.fetch_add(exportItems.size(), std::memory_order_relaxed);
					}
				}
				return;
			}

			for (std::size_t itemIndex = 0; itemIndex < batch.items.size(); ++itemIndex)
			{
				const ScriptWorkItem& item = batch.items[itemIndex];
				if (item.executionClass == ScriptExecutionClass::Skipped)
				{
					skippedScripts.fetch_add(1, std::memory_order_relaxed);
					continue;
				}
				if (item.executionClass == ScriptExecutionClass::Failed)
				{
					failedScripts.fetch_add(1, std::memory_order_relaxed);
					continue;
				}

				ScriptDispatchContext context;
				context.workerDispatch = workerDispatch;
				context.exportMode = exportMode;
				context.sourceObjectId = item.objectId;
				context.sourceObjectUuid = item.objectUuid;
				context.sourceBindingId = item.bindingId;
				context.frameOrder = (frameId * 1000000ULL) + batch.mutationOrderBase + itemIndex;
				context.objectGeneration = item.objectGeneration;
				context.componentGeneration = item.componentGeneration;
				context.bindingGeneration = item.bindingGeneration;
				SetCurrentScriptDispatchContext(std::move(context));

				try
				{
					if (exportMode)
					{
						if (item.runtimeHandle == nullptr)
						{
							failedScripts.fetch_add(1, std::memory_order_relaxed);
							ClearCurrentScriptDispatchContext();
							continue;
						}
						if (!item.runtimeStarted && m_ExportVTable.invokeStart != nullptr)
						{
							m_ExportVTable.invokeStart(item.runtimeHandle);
							std::scoped_lock lock(startedInstancesMutex);
							startedInstances.push_back(item);
						}
						if (m_ExportVTable.invokeUpdate != nullptr)
						{
							m_ExportVTable.invokeUpdate(item.runtimeHandle, frameDeltaTime);
						}
					}
					else if (managedUpdateScript != nullptr)
					{
						managedUpdateScript(item.objectId, item.bindingId.c_str(), item.scriptType.c_str(), frameDeltaTime);
					}
					updatedScripts.fetch_add(1, std::memory_order_relaxed);
					if (workerDispatch)
					{
						workerScripts.fetch_add(1, std::memory_order_relaxed);
					}
					else
					{
						protectedScripts.fetch_add(1, std::memory_order_relaxed);
					}
				}
				catch (...)
				{
					failedScripts.fetch_add(1, std::memory_order_relaxed);
				}
				ClearCurrentScriptDispatchContext();
			}
		};
		batchJobs.push_back(graph.AddJob(std::move(job)));
	}

	TerraEngine::Scheduling::JobGraphTaskGroupDeclaration group;
	group.name = "Scripting batches";
	group.jobs = batchJobs;
	group.executionPolicy = TerraEngine::Scheduling::JobGraphExecutionPolicy::MixedProtected;
	group.profilingLabel = "ScriptingSystem: Batches";
	graph.AddTaskGroup(std::move(group));
	const TerraEngine::Scheduling::JobGraphSummary& graphSummary = graph.Execute();

	if (!graphSummary.valid || !graphSummary.completed)
	{
		result.accessReport.conflicts.push_back("Scripting job graph did not complete cleanly.");
		for (const TerraEngine::Scheduling::JobGraphDiagnostic& diagnostic : graphSummary.diagnostics)
		{
			result.accessReport.messages.push_back(diagnostic.nodeLabel + ": " + diagnostic.message);
		}
	}

	if (workload.exportMode && !startedInstances.empty())
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		for (const ScriptWorkItem& item : startedInstances)
		{
			auto instanceIt = std::find_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
				return instance.objectId == item.objectId
					&& instance.bindingId == item.bindingId
					&& instance.scriptId == item.scriptRuntimeId;
			});
			if (instanceIt != m_ExportInstances.end())
			{
				instanceIt->started = true;
			}
		}
	}

	result.updatedScripts = updatedScripts.load(std::memory_order_relaxed);
	result.failedScripts = failedScripts.load(std::memory_order_relaxed);
	result.skippedScripts = skippedScripts.load(std::memory_order_relaxed);
	result.workerScripts = workerScripts.load(std::memory_order_relaxed);
	result.protectedScripts = protectedScripts.load(std::memory_order_relaxed);
	result.cacheHits = g_ScriptingBridgeCacheHits.load(std::memory_order_relaxed);
	result.cacheMisses = g_ScriptingBridgeCacheMisses.load(std::memory_order_relaxed);
	result.bridgeCallsAvoided = g_ScriptingBridgeCallsAvoided.load(std::memory_order_relaxed);
	result.accessReport.workerScripts = result.workerScripts;
	result.accessReport.protectedScripts = result.protectedScripts;
	if (workload.diagnosticsRequested)
	{
		result.accessReport.messages.push_back("Scripting batches=" + std::to_string(batches.size()) + " graphCompleted=" + (graphSummary.completed ? "true" : "false"));
	}

	FlushScriptMutationsIntoResult(result);
	result.formattedSummary = FormatScriptingFrameResult(result);
	m_LatestFrameResult = result;
	PublishLatestScriptingDiagnostics();
	return result;
}

void ScriptingSystem::PublishLatestScriptingDiagnostics()
{
	if (m_LatestFrameResult.diagnosticsCollected)
	{
		TERRAENGINE_LOG(LogLevel::Debug, m_LatestFrameResult.formattedSummary);
	}
}

TerraEngine::Scene::SceneMutationRequest ScriptingSystem::BuildScriptMutationRequest(TerraEngine::Scene::SceneMutationOperation operation, const ScriptDispatchContext& context, TerraEngine::UUID targetObjectId, std::string displayName, std::string componentName, std::string payloadJson) const
{
	TerraEngine::Scene::SceneMutationRequest request;
	request.requestId = "script-" + std::to_string(context.frameOrder);
	request.operation = operation;
	request.targetObjectId = targetObjectId;
	request.displayName = std::move(displayName);
	request.componentName = std::move(componentName);
	request.payloadJson = std::move(payloadJson);
	request.sourceObjectId = context.sourceObjectId;
	request.sourceObjectUuid = context.sourceObjectUuid;
	request.sourceBindingId = context.sourceBindingId;
	request.sourceFrameOrder = context.frameOrder;
	if (operation == TerraEngine::Scene::SceneMutationOperation::AddComponent)
	{
		request.componentPayloadKind = TerraEngine::Scene::SceneComponentMutationPayloadKind::Add;
	}
	return request;
}

void ScriptingSystem::RecordScriptMutationRequest(TerraEngine::Scene::SceneMutationRequest request)
{
	const std::uint64_t mutationOrder = m_NextScriptMutationOrder.fetch_add(1, std::memory_order_relaxed);
	request.requestId = request.requestId.empty()
		? ("script-mutation-" + std::to_string(mutationOrder))
		: (request.requestId + "-" + std::to_string(mutationOrder));
	TerraEngine::Scene::SceneMutationQueue::Instance().Enqueue(std::move(request));
}

void ScriptingSystem::FlushScriptMutationsIntoResult(ScriptingFrameResult& result)
{
	TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
	const std::size_t pendingBeforeFlush = queue.Size();
	if (pendingBeforeFlush == 0)
	{
		return;
	}

	TerraEngine::Scene::SceneMutationBatchResult mutationResult = queue.Apply(ObjectManager::instance());
	for (const TerraEngine::Scene::SceneMutationResult& entry : mutationResult.results)
	{
		if (entry.accepted)
		{
			++result.acceptedMutations;
		}
		else
		{
			++result.rejectedMutations;
		}
		for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : entry.diagnostics)
		{
			result.accessReport.messages.push_back(entry.requestId + ": " + diagnostic.message);
		}
	}
	for (const TerraEngine::Scene::SceneDiagnostic& diagnostic : mutationResult.diagnostics)
	{
		result.accessReport.messages.push_back(diagnostic.message);
	}
	result.typedMutations += mutationResult.typedMutationCount;
	result.jsonMutationFallbacks += mutationResult.jsonMutationFallbackCount;
	result.coalescedMutations += mutationResult.coalescedMutationCount;
	result.accessReport.deferredMutations = result.acceptedMutations;
	result.accessReport.rejectedMutations = result.rejectedMutations;
}

// ScriptingSystem::ConfigureExportRuntime is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::ConfigureExportRuntime(const std::filesystem::path& nativeAotLibraryPath) {
	// Store the absolute path up front so later system initialization does not
	// depend on whatever the current working directory happens to be then.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (nativeAotLibraryPath.empty()) {
		m_ConfiguredExportRuntimePath.reset();
		return;
	}

	m_ConfiguredExportRuntimePath = fs::absolute(nativeAotLibraryPath);
}

// ScriptingSystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::Initialize() {
	TERRAENGINE_LOG(LogLevel::Info, "Initializing Scripting System...");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_ExportMode) {
		TERRAENGINE_LOG(LogLevel::Info, "Scripting System already initialized in export mode. Skipping standard hostfxr initialization.");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_ConfiguredExportRuntimePath.has_value()) {
		// Export builds still discover the NativeAOT library in the launcher,
		// but the actual runtime load now happens here so scripting is brought
		// online with the rest of the engine systems instead of before them.
		ManagedVTable vtable{};
		if (!DotNetHost::Instance().InitializeExportRuntime(*m_ConfiguredExportRuntimePath, &vtable)) {
			const std::string hostDiagnostic = DotNetHost::Instance().GetLastNativeError();
			// std::runtime_error is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
			throw std::runtime_error(hostDiagnostic.empty()
				? "Failed to initialize NativeAOT export runtime from " + m_ConfiguredExportRuntimePath->string()
				: hostDiagnostic);
		}

		InitializeExportMode(vtable);
		return;
	}

	const fs::path bridgeAssemblyPath = ResolveManagedBridgeAssemblyPath();
	const fs::path runtimeConfigPath = bridgeAssemblyPath.parent_path() / "TerraEngine.CSharp.runtimeconfig.json";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(bridgeAssemblyPath) || !fs::exists(runtimeConfigPath)) {
		TERRAENGINE_LOG(LogLevel::Warning, "Could not find the managed TerraEngine.CSharp bridge beside the running executable. Scripting will stay disabled.");
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	managedUpdateScriptBatch = reinterpret_cast<UpdateScriptBatchFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"UpdateScriptBatch"));
	managedPumpFrameEvents = reinterpret_cast<PumpFrameEventsFn>(DotNetHost::Instance().GetManagedFunction(
		bridgeAssemblyPath,
		L"TerraEngine.InternalCalls, TerraEngine.CSharp",
		L"PumpFrameEvents"));
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedInit != nullptr) {
		managedInit();
		TERRAENGINE_LOG(LogLevel::Info, "Scripting System initialized successfully.");
	} else {
		TERRAENGINE_LOG(LogLevel::Error, "Failed to resolve TerraEngine.InternalCalls.Initialize from " << bridgeAssemblyPath.string());
	}
}

// ScriptingSystem::SetRunning is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::SetRunning(bool running)
{
	if (m_Running == running)
	{
		return;
	}

	m_Running = running;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::Update(float deltaTime) {
	TERRAENGINE_PROFILE_SCOPE("ScriptingSystem::Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
	m_LastDeltaTime = deltaTime;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_ExportMode) {
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Export Runtime Update", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		UpdateExportMode(deltaTime);
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedUpdateScript == nullptr || !m_GameplayAssemblyLoaded || !m_Running) {
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Paused) {
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Pause/Step Handling", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		if (m_StepFrames > 0) {
			m_StepFrames--;
		} else {
			RefreshWatchedBindingSnapshots(deltaTime);
			return;
		}
	}

	ScriptUpdateWorkload workload;
	{
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Build Script Workload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		workload = BuildScriptUpdateWorkload(deltaTime, false);
	}

	if (managedPumpFrameEvents != nullptr && workload.items.empty())
	{
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Pump UI Events", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		managedPumpFrameEvents(workload.frameId);
	}

	{
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Dispatch Script Workload", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		DispatchScriptWorkload(workload);
	}

	{
		TERRAENGINE_PROFILE_SCOPE("ScriptingSystem: Refresh Watched Bindings", TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
		RefreshWatchedBindingSnapshots(deltaTime);
	}
}

// ScriptingSystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::Shutdown() {
	ClearGameplayAssembly();

	// Reset export-mode caches so a later engine session in the same process
	// can reinitialize the NativeAOT scripting path cleanly.
	std::vector<void*> handlesToDestroy;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ExportScriptInstance& instance : m_ExportInstances)
		{
			handlesToDestroy.push_back(instance.handle);
		}
		m_ExportInstances.clear();
	}
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

// ScriptingSystem::SystemName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ScriptingSystem::SystemName() {
	return "ScriptingSystem";
}

TerraEngine::Scheduling::SystemDeclaration ScriptingSystem::SchedulerDeclaration()
{
	TerraEngine::Scheduling::SystemDeclaration declaration;
	declaration.name = "ScriptingSystem";
	declaration.phase = "Scripting";
	declaration.reads = { "SceneGraph", "Resources" };
	declaration.writes = { "Scripting", "ObjectManager", "SceneMutationQueue" };
	// Managed callbacks can be batched by native code, but the managed boundary
	// itself remains protected because scripts may call reflection APIs and
	// request structural scene mutations. Native pre/post work reports worker
	// counts through ScriptingFrameResult diagnostics instead.
	declaration.mainThread = true;
	declaration.requiresFlushedSceneMutations = false;
	declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
	declaration.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe;
	declaration.hardBoundary = true;
	declaration.safeSubwork = "Native script batch preparation, immutable dispatch inputs, generation validation, and deferred mutation capture.";
	declaration.protectedCommit = "Arbitrary managed callback invocation, managed lifecycle/reload hooks, and immediate native/reflection API calls.";
	declaration.protectedReason = declaration.protectedCommit;
	declaration.profilingLabel = declaration.name;
	declaration.usesDefaultMetadata = false;
	return declaration;
}

std::string ScriptingSystem::GetSchedulerBoundarySummary() const
{
	return "C# gameplay callbacks are scheduled as native script batches; C# authors keep the normal API while structural scene changes are deferred to SceneMutationQueue flush points.";
}

TerraEngine::Scheduling::HardBoundary ScriptingSystem::GetManagedCallbackBoundary() const
{
	for (const TerraEngine::Scheduling::HardBoundary& boundary : TerraEngine::Scheduling::InitialWorkerHardBoundaries())
	{
		if (boundary.boundaryName == "ManagedGameplayCallbacks")
		{
			return boundary;
		}
	}
	return {};
}

// ScriptingSystem::LoadGameplayAssembly is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptingSystem::LoadGameplayAssembly(const std::filesystem::path& assemblyPath, std::string* outError) {
	if (managedLoadGameplayAssembly == nullptr) {
		const std::string message = "The managed gameplay assembly loader is unavailable because TerraEngine.CSharp did not expose LoadGameplayAssembly.";
		m_LastGameplayDiagnostic = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError) {
			*outError = message;
		}
		return false;
	}

	const std::string assemblyPathString = fs::absolute(assemblyPath).string();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!fs::exists(assemblyPathString)) {
		const std::string message = "Assembly-CSharp.dll was expected at " + assemblyPathString + " but does not exist.";
		m_LastGameplayDiagnostic = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError) {
			*outError = message;
		}
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedLoadGameplayAssembly(assemblyPathString.c_str()) == 0) {
		const std::string message = "TerraEngine.CSharp rejected the gameplay assembly reload for " + assemblyPathString + ".";
		m_LastGameplayDiagnostic = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::ClearGameplayAssembly is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptingSystem::IsScriptTypeAttachable is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptingSystem::IsScriptTypeAttachable(const std::string& className) const {
	if (managedIsAttachableScriptType == nullptr || className.empty()) {
		return false;
	}

	return managedIsAttachableScriptType(className.c_str()) != 0;
}

// ScriptingSystem::InitializeBindingMetadata is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::InitializeBindingMetadata(ScriptBinding& binding)
{
	binding.fullTypeName = binding.className;
	binding.runtimeAvailable = false;
	binding.runtimeOverrideCount = 0;
	binding.runtimeOverrides.clear();
	binding.liveSnapshots.clear();
	binding.migrationResult = {};
	binding.lastBridgeMessage.clear();

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::RebuildRestoredSceneBindings is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptingSystem::UpdateInspectorWatch is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::UpdateInspectorWatch(std::optional<uint32_t> objectId, std::uint64_t selectionVersion)
{
	std::scoped_lock lock(m_InspectorWatchMutex);
	const bool changed = m_InspectorWatchState.watchedObjectId != objectId || m_InspectorWatchState.selectionVersion != selectionVersion;
	m_InspectorWatchState.watchedObjectId = objectId;
	m_InspectorWatchState.selectionVersion = selectionVersion;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (changed)
	{
		m_InspectorWatchState.refreshCountdownSeconds = 0.0f;
	}
}

// ScriptingSystem::WriteLiveBindingValue is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptingSystem::WriteLiveBindingValue(uint32_t objectId, ScriptBinding& binding, std::string_view fieldPath, const nlohmann::json& value, std::string* outError)
{
	if (managedWriteLiveBindingValue == nullptr || !m_GameplayAssemblyLoaded)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = "The live script write bridge is unavailable because TerraEngine.CSharp did not expose WriteLiveBindingValue.";
		}
		return false;
	}

	ManagedWriteResponse response;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ParseManagedResponse(managedWriteLiveBindingValue(objectId, binding.bindingId.c_str(), std::string(fieldPath).c_str(), value.dump().c_str()), response))
	{
		if (outError)
		{
			*outError = "The managed live script write bridge returned an invalid payload.";
		}
		return false;
	}

	binding.lastBridgeMessage = response.message;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::RefreshBindingStatuses is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::RefreshBindingStatuses() {
	const std::string previousAssemblyId = m_LoadedGameplayAssemblyPath.string();
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>()) {
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>()) {
			auto guard = scriptComponent->acquireWriteLock();
			for (ScriptBinding& binding : scriptComponent->GetBindings()) {
				binding.fullTypeName = binding.className;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::RefreshBindingDescriptor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::RefreshBindingDescriptor(ScriptBinding& binding)
{
	binding.fieldDescriptors.clear();
	binding.diagnostics.clear();
	binding.diagnosticIds.clear();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (managedDescribeBindingType == nullptr || binding.className.empty())
	{
		return;
	}

	ManagedDescriptorResponse response;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ParseManagedResponse(managedDescribeBindingType(binding.className.c_str()), response))
	{
		binding.lastBridgeMessage = "Failed to parse the managed descriptor payload for " + binding.className + ".";
		return;
	}

	binding.fullTypeName = response.fullTypeName.empty() ? binding.className : response.fullTypeName;
	binding.fieldDescriptors = std::move(response.fields);
	binding.diagnostics = std::move(response.diagnostics);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScriptFieldDiagnostic& diagnostic : binding.diagnostics)
	{
		if (!diagnostic.diagnosticId.empty())
		{
			binding.diagnosticIds.push_back(diagnostic.diagnosticId);
		}
	}
}

// ScriptingSystem::MigrateBindingAuthoredValues is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::MigrateBindingAuthoredValues(ScriptBinding& binding, const std::string& previousAssemblyId)
{
	if (managedMigrateBindingAuthoredState == nullptr || binding.className.empty())
	{
		return;
	}

	ManagedMigrationResponse response;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ParseManagedResponse(managedMigrateBindingAuthoredState(binding.className.c_str(), nlohmann::json(binding.serializedValues).dump().c_str()), response))
	{
		binding.lastBridgeMessage = "Failed to parse the managed migration payload for " + binding.className + ".";
		return;
	}

	binding.serializedValues = std::move(response.fields);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (ScriptFieldValue& fieldValue : binding.serializedValues)
	{
		if (fieldValue.displayName.empty())
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (const ScriptFieldDescriptor* descriptor = binding.FindFieldDescriptor(fieldValue.fieldPath))
			{
				fieldValue.displayName = descriptor->displayName;
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScriptFieldDiagnostic& diagnostic : response.diagnostics)
	{
		if (!diagnostic.diagnosticId.empty())
		{
			binding.diagnosticIds.push_back(diagnostic.diagnosticId);
		}
	}
}

// ScriptingSystem::PopulateBindingRepairActions is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::PopulateBindingRepairActions(ScriptBinding& binding)
{
	binding.repairActions.clear();

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::RefreshWatchedBindingSnapshots is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::RefreshWatchedBindingSnapshots(float deltaTime)
{
	std::optional<uint32_t> watchedObjectId;
	{
		std::scoped_lock lock(m_InspectorWatchMutex);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!m_InspectorWatchState.watchedObjectId.has_value())
		{
			return;
		}

		m_InspectorWatchState.refreshCountdownSeconds -= deltaTime;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_InspectorWatchState.refreshCountdownSeconds > 0.0f)
		{
			return;
		}

		m_InspectorWatchState.refreshCountdownSeconds = kLiveInspectorRefreshSeconds;
		watchedObjectId = m_InspectorWatchState.watchedObjectId;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!watchedObjectId.has_value())
	{
		return;
	}

	auto watchedObject = ObjectManager::instance().getObject(static_cast<entt::entity>(*watchedObjectId));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!watchedObject.has_value())
	{
		return;
	}

	ScriptComponent* scriptComponent = watchedObject->getComponent<ScriptComponent>();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptComponent == nullptr)
	{
		return;
	}

	std::vector<PendingBindingSnapshotRefresh> pendingRefreshes;
	{
		auto guard = scriptComponent->acquireReadLock();
		pendingRefreshes.reserve(scriptComponent->GetBindings().size());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (binding == nullptr || !binding->enabled || binding->className != pendingRefresh.className)
		{
			continue;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		binding->snapshotVersion = response.snapshotVersion;

		// Snapshot refresh should only replace transient runtime diagnostics.
		// Static descriptor warnings already live on the binding and would grow
		// forever if TerraEngine appended them again every refresh tick.
		binding->diagnostics.erase(
			std::remove_if(binding->diagnostics.begin(), binding->diagnostics.end(), [](const ScriptFieldDiagnostic& diagnostic) {
				return IsRuntimeSnapshotDiagnostic(diagnostic);
			}),
			binding->diagnostics.end());
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (existingDiagnostic == binding->diagnostics.end())
			{
				binding->diagnostics.push_back(diagnostic);
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!binding->runtimeAvailable && binding->status.empty())
		{
			binding->status = "InstanceUnavailable";
		}

		PopulateBindingRepairActions(*binding);
	}
}

// ScriptingSystem::ClearRuntimeInspectorState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::ClearRuntimeInspectorState()
{
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>())
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>())
		{
			auto guard = scriptComponent->acquireWriteLock();
			scriptComponent->ClearRuntimeState();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
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

// ScriptingSystem::SetAllBindingStatuses is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::SetAllBindingStatuses(const std::string& statusText) {
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>()) {
		if (ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>()) {
			auto guard = scriptComponent->acquireWriteLock();
			// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
			for (ScriptBinding& binding : scriptComponent->GetBindings()) {
				binding.status = statusText;
			}
			scriptComponent->lastResolvedAssemblyId.clear();
			scriptComponent->SynchronizeStorageFromBindings();
		}
	}
}

// ScriptingSystem::ResolveManagedBridgeAssemblyPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptingSystem::ResolveManagedBridgeAssemblyPath() const {
	const fs::path executableDirectory = ResolveExecutableDirectory();
	const fs::path current = fs::current_path();
	const fs::path candidates[] = {
		executableDirectory / "Managed" / "TerraEngine.CSharp.dll",
		executableDirectory / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		executableDirectory.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current / "Managed" / "TerraEngine.CSharp.dll",
		current / "Editor" / "Managed" / "TerraEngine.CSharp.dll",
		current.parent_path() / "Editor" / "Managed" / "TerraEngine.CSharp.dll"
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const fs::path& candidate : candidates) {
		if (fs::exists(candidate)) {
			return candidate;
		}
	}

	return candidates[0];
}

// ScriptingSystem::InitializeExportMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptingSystem::InitializeExportModeForTesting is intentionally narrow: it
// lets headless tests inject a fake NativeAOT VTable without loading a DLL.
void ScriptingSystem::InitializeExportModeForTesting(const ManagedVTable& vtable) {
	InitializeExportMode(vtable);
}

// ScriptingSystem::UpdateExportMode is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptingSystem::UpdateExportMode(float deltaTime) {
	if (!m_ExportMode || !m_Running) {
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Paused) {
		if (m_StepFrames > 0) {
			m_StepFrames--;
		} else {
			return;
		}
	}

	ScriptUpdateWorkload workload = BuildScriptUpdateWorkload(deltaTime, true);

	std::vector<ExportScriptInstance> staleInstances;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		auto staleBegin = std::remove_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
			const bool stillActive = std::any_of(workload.items.begin(), workload.items.end(), [&](const ScriptWorkItem& item) {
				return item.objectId == instance.objectId
					&& item.bindingId == instance.bindingId
					&& item.scriptRuntimeId == instance.scriptId;
			});
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!stillActive) {
				staleInstances.push_back(instance);
			}
			return !stillActive;
		});
		m_ExportInstances.erase(staleBegin, m_ExportInstances.end());
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ExportScriptInstance& instance : staleInstances) {
		if (instance.handle == nullptr) {
			continue;
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (instance.started && m_ExportVTable.invokeDisable != nullptr) {
			m_ExportVTable.invokeDisable(instance.handle);
		}
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ExportVTable.destroyScript != nullptr) {
			m_ExportVTable.destroyScript(instance.handle);
		}
	}

	if (workload.items.empty()) {
		DispatchScriptWorkload(workload);
		return;
	}

	// Create missing instances first so gameplay callbacks always see hydrated
	// scene values before Awake and Start begin executing.
	std::vector<ScriptWorkItem> pendingCreations;
	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const ScriptWorkItem& item : workload.items) {
			auto instanceIt = std::find_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
				return instance.objectId == item.objectId
					&& instance.bindingId == item.bindingId
					&& instance.scriptId == item.scriptRuntimeId;
			});
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (instanceIt == m_ExportInstances.end()) {
				pendingCreations.push_back(item);
			}
		}
	}

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const ScriptWorkItem& item : pendingCreations) {
		if (m_ExportVTable.createScript == nullptr) {
			break;
		}

		TERRAENGINE_LOG(LogLevel::Debug, "[Scripting] Creating instance for " << item.scriptType << " (ID: " << item.scriptRuntimeId << ") on object " << item.objectId);
		void* handle = m_ExportVTable.createScript(item.scriptRuntimeId, item.objectId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (handle == nullptr) {
			TERRAENGINE_LOG(LogLevel::Error, "[Scripting] createScript returned NULL for " << item.scriptType << " (ID: " << item.scriptRuntimeId << ")");
			continue;
		}

		ScriptBinding stagedBinding;
		stagedBinding.bindingId = item.bindingId;
		stagedBinding.className = item.scriptType;
		stagedBinding.scriptId = item.scriptRuntimeId;
		stagedBinding.serializedValues = item.serializedValues;
		ApplyExportBindingValues(m_ExportVTable, stagedBinding, handle, item.scriptRuntimeId);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_ExportVTable.invokeAwake != nullptr) {
			m_ExportVTable.invokeAwake(handle);
		}

		ExportScriptInstance instance;
		instance.handle = handle;
		instance.scriptId = item.scriptRuntimeId;
		instance.objectId = item.objectId;
		instance.bindingId = item.bindingId;
		instance.started = false;

		std::scoped_lock lock(m_ExportInstancesMutex);
		m_ExportInstances.push_back(std::move(instance));
	}

	{
		std::scoped_lock lock(m_ExportInstancesMutex);
		for (ScriptWorkItem& item : workload.items)
		{
			auto instanceIt = std::find_if(m_ExportInstances.begin(), m_ExportInstances.end(), [&](const ExportScriptInstance& instance) {
				return instance.objectId == item.objectId
					&& instance.bindingId == item.bindingId
					&& instance.scriptId == item.scriptRuntimeId;
			});
			if (instanceIt != m_ExportInstances.end())
			{
				item.runtimeHandle = instanceIt->handle;
				item.runtimeStarted = instanceIt->started;
			}
			else
			{
				item.executionClass = ScriptExecutionClass::Failed;
				item.protectedReason = "Export script instance could not be created.";
			}
		}
	}

	DispatchScriptWorkload(workload);
}
