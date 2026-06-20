// ScriptingSystem.h — Contains declarations for the Scripting System system state, startup/shutdown hooks, and per-frame work used by native script glue, exported C# vtables, and scripting system lifecycle state.

#pragma once

#include "Core/SystemBase.h"
#include "Components/ScriptComponent.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ExportVTable.h"
#include "Utilities/UUID.h"

#include <filesystem>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

class ScriptingSystem : public SystemsBase {
public:
	static ScriptingSystem& Instance();

	void Initialize() override;
	void Update(float deltaTime) override;
	void Shutdown() override;
	std::string SystemName() override;

	/// Records the exported NativeAOT gameplay library discovered by the
	/// launcher. The actual library load is deferred until Initialize() so
	/// export scripting comes up in the normal system lifecycle order.
	void ConfigureExportRuntime(const std::filesystem::path& nativeAotLibraryPath);

	void SetRunning(bool running);
	// SetPaused is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void SetPaused(bool paused) { m_Paused = paused; }
	// Step is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void Step(int frames = 1) { m_StepFrames = frames; }

	[[nodiscard]] bool IsRunning() const { return m_Running; }
	[[nodiscard]] bool IsPaused() const { return m_Paused; }
	[[nodiscard]] bool IsExportMode() const { return m_ExportMode; }
	[[nodiscard]] bool HasLoadedGameplayAssembly() const { return m_GameplayAssemblyLoaded; }
	[[nodiscard]] const std::filesystem::path& GetLoadedGameplayAssemblyPath() const { return m_LoadedGameplayAssemblyPath; }
	[[nodiscard]] const std::string& GetLastGameplayDiagnostic() const { return m_LastGameplayDiagnostic; }
	[[nodiscard]] float GetLastDeltaTime() const { return m_LastDeltaTime; }
	[[nodiscard]] bool ManagedCallbacksRunProtected() const { return false; }
	[[nodiscard]] bool ManagedCallbacksRequireSchedulerAnnotations() const { return false; }
	[[nodiscard]] std::string GetSchedulerBoundarySummary() const;

	enum class ScriptLifecycleState
	{
		Created,
		Started,
		Updating,
		Disabled,
		Destroyed
	};

	enum class ScriptExecutionClass
	{
		WorkerSafe,
		Protected,
		Skipped,
		Failed
	};

	struct ScriptWorkItem
	{
		std::uint32_t objectId = 0;
		TerraEngine::UUID objectUuid = TerraEngine::EMPTY_UUID;
		std::string bindingId;
		std::string scriptType;
		std::uint64_t scriptRuntimeId = 0;
		ScriptLifecycleState lifecycleState = ScriptLifecycleState::Updating;
		ScriptExecutionClass executionClass = ScriptExecutionClass::WorkerSafe;
		std::string diagnosticLabel;
		std::vector<ScriptFieldValue> serializedValues;
		void* runtimeHandle = nullptr;
		bool runtimeStarted = false;
		std::string protectedReason;
	};

	struct ScriptBatch
	{
		std::string batchId;
		std::vector<ScriptWorkItem> items;
		ScriptExecutionClass executionClass = ScriptExecutionClass::WorkerSafe;
		std::string profilingLabel;
		std::uint64_t mutationOrderBase = 0;
		std::string protectedReason;
	};

	struct ScriptUpdateWorkload
	{
		std::uint64_t frameId = 0;
		float deltaTime = 0.0f;
		std::vector<ScriptWorkItem> items;
		bool running = false;
		bool paused = false;
		bool stepping = false;
		bool exportMode = false;
		bool diagnosticsRequested = false;
	};

	struct ScriptAccessReport
	{
		std::uint64_t frameId = 0;
		std::size_t totalScripts = 0;
		std::size_t workerScripts = 0;
		std::size_t protectedScripts = 0;
		std::size_t deferredMutations = 0;
		std::size_t rejectedMutations = 0;
		std::vector<std::string> conflicts;
		std::vector<std::string> messages;
		std::vector<std::string> protectedReasons;
	};

	struct ScriptingFrameResult
	{
		std::uint64_t frameId = 0;
		std::size_t updatedScripts = 0;
		std::size_t failedScripts = 0;
		std::size_t skippedScripts = 0;
		std::size_t workerScripts = 0;
		std::size_t protectedScripts = 0;
		std::size_t acceptedMutations = 0;
		std::size_t rejectedMutations = 0;
		bool diagnosticsCollected = false;
		ScriptAccessReport accessReport;
		std::string formattedSummary;
	};

	struct ScriptDispatchContext
	{
		bool workerDispatch = false;
		bool exportMode = false;
		std::uint32_t sourceObjectId = 0;
		TerraEngine::UUID sourceObjectUuid = TerraEngine::EMPTY_UUID;
		std::string sourceBindingId;
		std::uint64_t frameOrder = 0;
	};

	[[nodiscard]] const ScriptingFrameResult& GetLatestScriptingFrameResult() const { return m_LatestFrameResult; }
	[[nodiscard]] bool IsScriptingDiagnosticsRequested() const;
	static void SetScriptingDiagnosticsRequested(bool requested);
	static bool ScriptingDiagnosticsRequested();
	static std::vector<ScriptBatch> PlanScriptBatches(const ScriptUpdateWorkload& workload, std::size_t workerCount);
	static std::string FormatScriptingFrameResult(const ScriptingFrameResult& result);
	static std::optional<ScriptDispatchContext> CurrentScriptDispatchContext();
	static void SetCurrentScriptDispatchContext(ScriptDispatchContext context);
	static void ClearCurrentScriptDispatchContext();
	ScriptUpdateWorkload BuildScriptUpdateWorkload(float deltaTime, bool exportMode);
	TerraEngine::Scene::SceneMutationRequest BuildScriptMutationRequest(TerraEngine::Scene::SceneMutationOperation operation, const ScriptDispatchContext& context, TerraEngine::UUID targetObjectId = TerraEngine::EMPTY_UUID, std::string displayName = {}, std::string componentName = {}, std::string payloadJson = {}) const;
	void RecordScriptMutationRequest(TerraEngine::Scene::SceneMutationRequest request);

	bool LoadGameplayAssembly(const std::filesystem::path& assemblyPath, std::string* outError = nullptr);
	void ClearGameplayAssembly();
	bool IsScriptTypeAttachable(const std::string& className) const;
	void InitializeBindingMetadata(ScriptBinding& binding);
	void RebuildRestoredSceneBindings();

	void UpdateInspectorWatch(std::optional<uint32_t> objectId, std::uint64_t selectionVersion);
	bool WriteLiveBindingValue(uint32_t objectId, ScriptBinding& binding, std::string_view fieldPath, const nlohmann::json& value, std::string* outError = nullptr);
	void InitializeExportModeForTesting(const ManagedVTable& vtable);

private:
	// struct InspectorWatchState groups related state and operations so callers have one documented contract.
	struct InspectorWatchState
	{
		// The live inspector only polls the currently selected object so runtime
		// refresh work stays proportional to what the user can actually see.
		std::optional<uint32_t> watchedObjectId;
		std::uint64_t selectionVersion = 0;
		float refreshCountdownSeconds = 0.0f;
	};

	ScriptingSystem() = default;

	void RefreshBindingStatuses();
	void RefreshBindingDescriptor(ScriptBinding& binding);
	void MigrateBindingAuthoredValues(ScriptBinding& binding, const std::string& previousAssemblyId);
	void PopulateBindingRepairActions(ScriptBinding& binding);
	void RefreshWatchedBindingSnapshots(float deltaTime);
	void ClearRuntimeInspectorState();
	void SetAllBindingStatuses(const std::string& statusText);
	std::filesystem::path ResolveManagedBridgeAssemblyPath() const;
	ScriptingFrameResult DispatchScriptWorkload(const ScriptUpdateWorkload& workload);
	void PublishLatestScriptingDiagnostics();
	void FlushScriptMutationsIntoResult(ScriptingFrameResult& result);

	bool m_Running = true;
	bool m_Paused = false;
	int m_StepFrames = 0;
	bool m_GameplayAssemblyLoaded = false;
	std::filesystem::path m_LoadedGameplayAssemblyPath;
	std::string m_LastGameplayDiagnostic;
	float m_LastDeltaTime = 0.0f;

	std::mutex m_InspectorWatchMutex;
	InspectorWatchState m_InspectorWatchState;
	ScriptingFrameResult m_LatestFrameResult;
	std::uint64_t m_NextScriptingFrameId = 1;
	std::atomic_uint64_t m_NextScriptMutationOrder = 1;

	// Managed bridge entry points resolved from TerraEngine.CSharp during
	// startup. Keeping them cached avoids repeated lookup work every frame.
	using InitFn = void (*)();
	using UpdateScriptFn = void (*)(uint32_t, const char*, const char*, float);
	using LoadGameplayAssemblyFn = int (*)(const char*);
	using UnloadGameplayAssemblyFn = void (*)();
	using ResetGameplaySessionFn = void (*)();
	using IsAttachableScriptTypeFn = int (*)(const char*);
	using DescribeBindingTypeFn = const char* (*)(const char*);
	using ReadLiveBindingValuesFn = const char* (*)(uint32_t, const char*, const char*);
	using WriteLiveBindingValueFn = const char* (*)(uint32_t, const char*, const char*, const char*);
	using MigrateBindingAuthoredStateFn = const char* (*)(const char*, const char*);

	InitFn managedInit = nullptr;
	UpdateScriptFn managedUpdateScript = nullptr;
	LoadGameplayAssemblyFn managedLoadGameplayAssembly = nullptr;
	UnloadGameplayAssemblyFn managedUnloadGameplayAssembly = nullptr;
	ResetGameplaySessionFn managedResetGameplaySession = nullptr;
	IsAttachableScriptTypeFn managedIsAttachableScriptType = nullptr;
	DescribeBindingTypeFn managedDescribeBindingType = nullptr;
	ReadLiveBindingValuesFn managedReadLiveBindingValues = nullptr;
	WriteLiveBindingValueFn managedWriteLiveBindingValue = nullptr;
	MigrateBindingAuthoredStateFn managedMigrateBindingAuthoredState = nullptr;

	void InitializeExportMode(const ManagedVTable& vtable);

	// Export-mode VTable — populated by InitializeExportMode() and used for
	// all script lifecycle dispatch when running an exported build.
	ManagedVTable m_ExportVTable{};
	bool m_ExportMode = false;
	std::optional<std::filesystem::path> m_ConfiguredExportRuntimePath;

	// Script instance handles tracked by the export-mode dispatcher so the
	// engine can invoke lifecycle methods on the correct managed objects.
	struct ExportScriptInstance {
		void* handle = nullptr;
		uint64_t scriptId = 0;
		uint32_t objectId = 0;
		std::string bindingId;
		bool started = false;
	};
	std::vector<ExportScriptInstance> m_ExportInstances;
	std::mutex m_ExportInstancesMutex;

	void UpdateExportMode(float deltaTime);
};
