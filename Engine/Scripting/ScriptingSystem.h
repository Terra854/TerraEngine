#pragma once

#include "Core/SystemBase.h"
#include "Components/ScriptComponent.h"
#include "Scripting/ExportVTable.h"

#include <filesystem>
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
	void SetPaused(bool paused) { m_Paused = paused; }
	void Step(int frames = 1) { m_StepFrames = frames; }

	[[nodiscard]] bool IsRunning() const { return m_Running; }
	[[nodiscard]] bool IsPaused() const { return m_Paused; }
	[[nodiscard]] bool HasLoadedGameplayAssembly() const { return m_GameplayAssemblyLoaded; }
	[[nodiscard]] const std::filesystem::path& GetLoadedGameplayAssemblyPath() const { return m_LoadedGameplayAssemblyPath; }
	[[nodiscard]] const std::string& GetLastGameplayDiagnostic() const { return m_LastGameplayDiagnostic; }
	[[nodiscard]] float GetLastDeltaTime() const { return m_LastDeltaTime; }

	bool LoadGameplayAssembly(const std::filesystem::path& assemblyPath, std::string* outError = nullptr);
	void ClearGameplayAssembly();
	bool IsScriptTypeAttachable(const std::string& className) const;
	void InitializeBindingMetadata(ScriptBinding& binding);
	void RebuildRestoredSceneBindings();

	void UpdateInspectorWatch(std::optional<uint32_t> objectId, std::uint64_t selectionVersion);
	bool WriteLiveBindingValue(uint32_t objectId, ScriptBinding& binding, std::string_view fieldPath, const nlohmann::json& value, std::string* outError = nullptr);

private:
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

	bool m_Running = true;
	bool m_Paused = false;
	int m_StepFrames = 0;
	bool m_GameplayAssemblyLoaded = false;
	std::filesystem::path m_LoadedGameplayAssemblyPath;
	std::string m_LastGameplayDiagnostic;
	float m_LastDeltaTime = 0.0f;

	std::mutex m_InspectorWatchMutex;
	InspectorWatchState m_InspectorWatchState;

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
