#include "ScriptCompilationSubsystem.h"

#include "../Assets/AssetDatabase.h"
#include "../Project/ProjectManager.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/ScriptComponent.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <array>
#include <chrono>

#if defined(_WIN32)
#include <Windows.h>
#endif

namespace
{
	constexpr float kScriptBuildDebounceSeconds = 0.25f;

	std::string BuildProfileToString(ScriptBuildProfile profile)
	{
		return profile == ScriptBuildProfile::Debug ? "Debug" : "Release";
	}

	std::wstring BuildProfileToWideString(ScriptBuildProfile profile)
	{
		return profile == ScriptBuildProfile::Debug ? L"Debug" : L"Release";
	}

	std::string CoordinatorStateToString(ScriptCoordinatorState state)
	{
		switch (state)
		{
		case ScriptCoordinatorState::Idle:
			return "Idle";
		case ScriptCoordinatorState::Queued:
			return "Queued";
		case ScriptCoordinatorState::Building:
			return "Building";
		case ScriptCoordinatorState::Reloading:
			return "Reloading";
		case ScriptCoordinatorState::Ready:
			return "Ready";
		case ScriptCoordinatorState::Failed:
			return "Failed";
		}

		return "Unknown";
	}

#if defined(_WIN32)
	bool RunProcessCapture(const std::filesystem::path& workingDirectory, const std::wstring& commandLine, std::string& output, DWORD& exitCode)
	{
		SECURITY_ATTRIBUTES securityAttributes{};
		securityAttributes.nLength = sizeof(securityAttributes);
		securityAttributes.bInheritHandle = TRUE;

		HANDLE readHandle = nullptr;
		HANDLE writeHandle = nullptr;
		if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, 0))
		{
			return false;
		}

		SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

		STARTUPINFOW startupInfo{};
		startupInfo.cb = sizeof(startupInfo);
		startupInfo.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
		startupInfo.wShowWindow = SW_HIDE;
		startupInfo.hStdOutput = writeHandle;
		startupInfo.hStdError = writeHandle;
		startupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

		PROCESS_INFORMATION processInfo{};
		std::wstring mutableCommandLine = commandLine;
		const BOOL started = CreateProcessW(
			nullptr,
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			TRUE,
			CREATE_NO_WINDOW,
			nullptr,
			workingDirectory.wstring().c_str(),
			&startupInfo,
			&processInfo);

		CloseHandle(writeHandle);
		if (!started)
		{
			CloseHandle(readHandle);
			return false;
		}

		std::array<char, 4096> buffer{};
		DWORD bytesRead = 0;
		while (ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
		{
			output.append(buffer.data(), bytesRead);
		}

		WaitForSingleObject(processInfo.hProcess, INFINITE);
		GetExitCodeProcess(processInfo.hProcess, &exitCode);

		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		CloseHandle(readHandle);
		return true;
	}
#endif
}

ScriptCompilationSubsystem& ScriptCompilationSubsystem::Instance()
{
	static ScriptCompilationSubsystem instance;
	return instance;
}

void ScriptCompilationSubsystem::Initialize(EditorContext& context)
{
	m_Context = &context;
	UpdateSharedState();
}

void ScriptCompilationSubsystem::Update(float deltaTime)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		return;
	}

	if (m_BuildInFlight && m_ActiveBuild.valid() && m_ActiveBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		ScriptBuildResult result = m_ActiveBuild.get();
		m_BuildInFlight = false;
		HandleCompletedBuild(std::move(result));
	}

	if (!m_BuildInFlight && m_Status.queuedBuild)
	{
		m_DebounceTimer -= deltaTime;
		if (m_DebounceTimer <= 0.0f)
		{
			BeginBuild();
		}
	}

	UpdateSharedState();
}

void ScriptCompilationSubsystem::DrawUI()
{
}

void ScriptCompilationSubsystem::Shutdown()
{
	if (m_BuildInFlight && m_ActiveBuild.valid())
	{
		m_ActiveBuild.wait();
	}

	m_Context = nullptr;
	m_Status = ScriptCompilationStatus{};
	m_BuildInFlight = false;
	m_DebounceTimer = 0.0f;
	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
}

std::string ScriptCompilationSubsystem::GetName() const
{
	return "Script Compilation";
}

void ScriptCompilationSubsystem::OnProjectOpened()
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		return;
	}

	m_WorkspaceService.SetProjectRoot(m_Context->projectManager->GetProjectRoot());
	m_Status = ScriptCompilationStatus{};
	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
	ScriptingSystem::Instance().ClearGameplayAssembly();

	std::string workspaceError;
	if (!m_WorkspaceService.EnsureWorkspace(&workspaceError))
	{
		// Generate the root solution immediately so new projects can open in
		// Visual Studio even before the asynchronous build completes.
		TERRAENGINE_LOG(LogLevel::Warning, "Assembly-CSharp workspace generation reported an issue during project open: " << workspaceError);
		m_Status.lastSummary = workspaceError;
	}

	RequestBuild(ScriptBuildReason::ProjectOpen);
}

void ScriptCompilationSubsystem::OnProjectClosed()
{
	m_Status = ScriptCompilationStatus{};
	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
	m_WorkspaceService.SetProjectRoot({});
	ScriptingSystem::Instance().ClearGameplayAssembly();
	UpdateSharedState();
}

void ScriptCompilationSubsystem::RequestBuild(ScriptBuildReason reason)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		return;
	}

	m_LastQueuedReason = reason;
	m_Status.queuedBuild = true;
	m_Status.state = ScriptCoordinatorState::Queued;
	m_Status.lastSummary = "A project script build is queued for " + BuildProfileToString(m_Status.activeProfile) + ".";
	m_DebounceTimer = reason == ScriptBuildReason::ProjectOpen ? 0.0f : kScriptBuildDebounceSeconds;

	// Script file saves should kick off compilation immediately so the active
	// editor session does not sit in a queued state waiting for another frame to
	// notice the ready-to-run build request. We still keep the queued path for
	// cases where another build is already in flight.
	if (reason == ScriptBuildReason::FileChange && !m_BuildInFlight)
	{
		BeginBuild();
		return;
	}

	UpdateSharedState();
}

void ScriptCompilationSubsystem::NotifyScriptSourceChanged(const std::filesystem::path& scriptPath)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		return;
	}

	// Keep the loaded Assembly-CSharp runtime synchronized with asset-backed C#
	// sources at all times. Script edits, creates, renames, and deletes all flow
	// through the asset watcher, so every .cs change must queue a rebuild even if
	// an older editor preference disabled background script builds.
	TERRAENGINE_LOG(LogLevel::Info, "Detected C# source change at " << scriptPath << ". Queuing a gameplay script rebuild.");
	RequestBuild(ScriptBuildReason::FileChange);
}

bool ScriptCompilationSubsystem::CreateScriptAsset(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError)
{
	if (!m_WorkspaceService.CreateScriptFromTemplate(destinationDirectory, className, outCreatedPath, outError))
	{
		return false;
	}

	AssetDatabase::Instance().ImportAsset(outCreatedPath, true);
	RequestBuild(ScriptBuildReason::FileChange);
	return true;
}

bool ScriptCompilationSubsystem::OpenScript(const std::filesystem::path& scriptPath, std::string* outError)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr)
	{
		if (outError)
		{
			*outError = "Project manager state is unavailable.";
		}
		return false;
	}

	return m_WorkspaceService.OpenScriptInExternalEditor(scriptPath, m_Context->projectManager->GetExternalScriptEditorCommand(), outError);
}

bool ScriptCompilationSubsystem::OpenWorkspace(std::string* outError)
{
	return OpenScript({}, outError);
}

bool ScriptCompilationSubsystem::RegenerateWorkspace(std::string* outError)
{
	if (!m_WorkspaceService.RegenerateWorkspace(outError))
	{
		return false;
	}

	RequestBuild(ScriptBuildReason::ManualRefresh);
	return true;
}

const ScriptCompilationStatus& ScriptCompilationSubsystem::GetStatus() const
{
	return m_Status;
}

const ScriptWorkspaceService& ScriptCompilationSubsystem::GetWorkspaceService() const
{
	return m_WorkspaceService;
}

bool ScriptCompilationSubsystem::IsBusy() const
{
	return m_Status.state == ScriptCoordinatorState::Building || m_Status.state == ScriptCoordinatorState::Reloading;
}

bool ScriptCompilationSubsystem::ShouldBlockEditorInteraction() const
{
	return IsBusy();
}

void ScriptCompilationSubsystem::PrepareForBuild()
{
	auto& scriptingSystem = ScriptingSystem::Instance();
	m_WasRunningBeforeBuild = scriptingSystem.IsRunning();
	m_WasPausedBeforeBuild = scriptingSystem.IsPaused();
	m_ShouldRestorePausedGameplayState = m_WasRunningBeforeBuild;

	if (m_WasRunningBeforeBuild && !m_WasPausedBeforeBuild)
	{
		// Pause the active gameplay session while TerraEngine unloads and rebuilds
		// Assembly-CSharp so script state does not keep ticking against stale code.
		scriptingSystem.SetPaused(true);
		TERRAENGINE_LOG(LogLevel::Info, "Paused gameplay script execution while Assembly-CSharp recompiles.");
	}

	if (scriptingSystem.HasLoadedGameplayAssembly())
	{
		const std::filesystem::path activeAssemblyPath = scriptingSystem.GetLoadedGameplayAssemblyPath();
		TERRAENGINE_LOG(LogLevel::Info, "Unloading the active gameplay assembly before rebuilding " << activeAssemblyPath << ".");
		scriptingSystem.ClearGameplayAssembly();
	}
}

void ScriptCompilationSubsystem::RestorePausedGameplayState()
{
	if (m_ShouldRestorePausedGameplayState)
	{
		ScriptingSystem::Instance().SetPaused(m_WasPausedBeforeBuild);
	}

	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
}

bool ScriptCompilationSubsystem::TryReloadWorkspaceAssembly(std::string* outError)
{
	const std::filesystem::path assemblyPath = m_WorkspaceService.GetAssemblyPath();
	if (assemblyPath.empty() || !std::filesystem::exists(assemblyPath))
	{
		if (outError)
		{
			*outError = "The project Assembly-CSharp.dll is unavailable at " + assemblyPath.string() + ", so TerraEngine cannot restore script execution after the failed compile.";
		}
		return false;
	}

	// Compile failures usually leave the previous Assembly-CSharp output intact,
	// so TerraEngine reloads that in-place project DLL instead of maintaining a
	// second rollback copy under Library/.
	std::string loadError;
	if (!ScriptingSystem::Instance().LoadGameplayAssembly(assemblyPath, &loadError))
	{
		TERRAENGINE_LOG(LogLevel::Error, "Failed to reload the project gameplay assembly from " << assemblyPath << " after the failed compile: " << loadError);
		if (outError)
		{
			*outError = loadError;
		}
		return false;
	}

	TERRAENGINE_LOG(LogLevel::Info, "Reloaded the project gameplay assembly from " << assemblyPath << " after the failed compile.");
	if (outError)
	{
		outError->clear();
	}
	return true;
}

void ScriptCompilationSubsystem::SynchronizeBoundScriptTypesFromAssets()
{
	auto& assetDatabase = AssetDatabase::Instance();
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>())
	{
		ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
		if (scriptComponent == nullptr)
		{
			continue;
		}

		bool bindingsChanged = false;
		auto guard = scriptComponent->acquireWriteLock();
		for (ScriptBinding& binding : scriptComponent->GetBindings())
		{
			const AssetMetadata* assetMetadata = binding.assetGuid == TerraEngine::EMPTY_UUID
				? nullptr
				: assetDatabase.FindAssetByGuid(binding.assetGuid);
			if (assetMetadata == nullptr && !binding.className.empty())
			{
				assetMetadata = assetDatabase.FindScriptAssetByFullName(binding.className);
				if (assetMetadata != nullptr && binding.assetGuid != assetMetadata->guid)
				{
					// Recover from script renames that previously regenerated the
					// asset GUID so existing object bindings can heal automatically.
					TERRAENGINE_LOG(LogLevel::Info, "Recovered script asset GUID for bound type " << binding.className << " from "
						<< TerraEngine::ToHexString(binding.assetGuid) << " to " << TerraEngine::ToHexString(assetMetadata->guid) << ".");
					binding.assetGuid = assetMetadata->guid;
					bindingsChanged = true;
				}
			}
			if (assetMetadata == nullptr)
			{
				continue;
			}

			const auto fullNameIt = assetMetadata->settings.find("scriptFullName");
			if (fullNameIt == assetMetadata->settings.end() || !fullNameIt->is_string())
			{
				continue;
			}

			const std::string resolvedFullName = fullNameIt->get<std::string>();
			if (resolvedFullName.empty() || binding.className == resolvedFullName)
			{
				continue;
			}

			// After a successful compile, move the serialized binding over to the
			// script class name currently declared by the asset so renames behave
			// like Unity instead of pinning the object to a stale type name.
			TERRAENGINE_LOG(LogLevel::Info, "Updated bound script type from " << binding.className << " to " << resolvedFullName << " for asset " << assetMetadata->sourcePath << ".");
			binding.className = resolvedFullName;
			bindingsChanged = true;
		}

		if (bindingsChanged)
		{
			scriptComponent->SynchronizeStorageFromBindings();
		}
	}
}

void ScriptCompilationSubsystem::BeginBuild()
{
	// Ensure the project workspace has the latest engine references before
	// starting the background build job.
	std::string syncError;
	if (!m_WorkspaceService.SyncManagedReferences(&syncError))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Failed to synchronize managed references before build: " << syncError);
	}

	PrepareForBuild();
	m_Status.queuedBuild = false;
	m_Status.state = ScriptCoordinatorState::Building;
	m_Status.lastSummary = "Building Assembly-CSharp (" + BuildProfileToString(m_Status.activeProfile) + ").";
	m_Status.gameplayAssemblyLoaded = false;
	m_Status.activeAssemblyPath.clear();
	m_BuildInFlight = true;
	AssetDatabase::Instance().SetScriptCompileState("Compiling", m_Status.lastSummary);
	TERRAENGINE_LOG(LogLevel::Info, "Starting Assembly-CSharp build for " << m_WorkspaceService.GetProjectPath() << " using the " << BuildProfileToString(m_Status.activeProfile) << " profile.");
	const ScriptBuildProfile requestedProfile = m_Status.activeProfile;
	m_ActiveBuild = std::async(std::launch::async, [this, requestedProfile]() {
		return RunBuildJob(requestedProfile);
	});
	UpdateSharedState();
}

void ScriptCompilationSubsystem::HandleCompletedBuild(ScriptBuildResult result)
{
	m_Status.lastBuildOutput = result.buildOutput;
	if (!result.buildOutput.empty())
	{
		// Mirror dotnet's stdout/stderr into the editor log so script rebuilds are
		// observable without opening the workspace or inspecting hidden state.
		TERRAENGINE_LOG(LogLevel::Info, "Assembly-CSharp build output:\n" << result.buildOutput);
	}

	if (!result.success)
	{
		m_Status.state = ScriptCoordinatorState::Failed;
		m_Status.lastSummary = result.failureMessage;
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp build failed: " << result.failureMessage);
		std::string reloadError;
		if (TryReloadWorkspaceAssembly(&reloadError))
		{
			m_Status.gameplayAssemblyLoaded = ScriptingSystem::Instance().HasLoadedGameplayAssembly();
			m_Status.activeAssemblyPath = ScriptingSystem::Instance().GetLoadedGameplayAssemblyPath();
			m_Status.lastSummary += " Reloaded the existing project Assembly-CSharp.dll.";
		}
		else if (!reloadError.empty())
		{
			m_Status.lastSummary += " " + reloadError;
		}

		RestorePausedGameplayState();
		AssetDatabase::Instance().SetScriptCompileState("CompileError", m_Status.lastSummary);
		UpdateSharedState();
		return;
	}

	m_Status.state = ScriptCoordinatorState::Reloading;
	m_Status.lastSummary = "Reloading the newly built Assembly-CSharp runtime into the current session.";
	UpdateSharedState();

	SynchronizeBoundScriptTypesFromAssets();

	std::string reloadError;
	if (!ScriptingSystem::Instance().LoadGameplayAssembly(result.assemblyPath, &reloadError))
	{
		m_Status.state = ScriptCoordinatorState::Failed;
		m_Status.lastSummary = reloadError;
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp build completed but the gameplay assembly reload failed: " << reloadError);
		m_Status.lastSummary += " TerraEngine keeps only the project Assembly-CSharp.dll, so no separate rollback copy is available.";

		RestorePausedGameplayState();
		AssetDatabase::Instance().SetScriptCompileState("CompileError", m_Status.lastSummary);
		UpdateSharedState();
		return;
	}

	m_Status.state = ScriptCoordinatorState::Ready;
	m_Status.gameplayAssemblyLoaded = true;
	m_Status.activeAssemblyPath = result.assemblyPath;
	m_Status.lastSummary = "Assembly-CSharp built and reloaded successfully.";
	TERRAENGINE_LOG(LogLevel::Info, "Assembly-CSharp built successfully and reloaded from " << result.assemblyPath << ".");
	RestorePausedGameplayState();
	AssetDatabase::Instance().SetScriptCompileState("Ready", m_Status.lastSummary);
	UpdateSharedState();
}

void ScriptCompilationSubsystem::UpdateSharedState()
{
	if (m_Context == nullptr)
	{
		return;
	}

	const bool stateChanged =
		m_Context->scripting.coordinatorState != CoordinatorStateToString(m_Status.state)
		|| m_Context->scripting.gameplayAssemblyLoaded != m_Status.gameplayAssemblyLoaded
		|| m_Context->scripting.activeAssemblyPath != m_Status.activeAssemblyPath;
	if (stateChanged)
	{
		m_Context->scripting.selectionVersion++;
	}

	m_Context->scripting.coordinatorState = CoordinatorStateToString(m_Status.state);
	m_Context->scripting.activeProfile = BuildProfileToString(m_Status.activeProfile);
	m_Context->scripting.gameplayAssemblyLoaded = m_Status.gameplayAssemblyLoaded;
	m_Context->scripting.lastSummary = m_Status.lastSummary;
	m_Context->scripting.activeAssemblyPath = m_Status.activeAssemblyPath;
}

ScriptCompilationSubsystem::ScriptBuildResult ScriptCompilationSubsystem::RunBuildJob(ScriptBuildProfile profile)
{
	ScriptBuildResult result;
	result.profile = profile;

	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		result.failureMessage = "No open project is available for Assembly-CSharp generation.";
		return result;
	}

	std::string workspaceError;
	if (!m_WorkspaceService.EnsureWorkspace(&workspaceError))
	{
		result.failureMessage = workspaceError;
		return result;
	}

#if defined(_WIN32)
	std::wstring commandLine = L"dotnet build \"" + m_WorkspaceService.GetProjectPath().wstring() + L"\" -c " + BuildProfileToWideString(profile) + L" -nologo -p:GenerateFullPaths=true";
	DWORD exitCode = 0;
	if (!RunProcessCapture(m_WorkspaceService.GetProjectRoot(), commandLine, result.buildOutput, exitCode))
	{
		result.failureMessage = "Failed to start dotnet build for Assembly-CSharp.";
		return result;
	}

	if (exitCode != 0)
	{
		result.failureMessage = "dotnet build failed for Assembly-CSharp.";
		return result;
	}
#else
	result.failureMessage = "The current scripting build runner is implemented for Windows only.";
	return result;
#endif

	result.assemblyPath = m_WorkspaceService.GetAssemblyPath();
	if (!std::filesystem::exists(result.assemblyPath))
	{
		result.failureMessage = "dotnet build completed without producing Assembly-CSharp.dll at " + result.assemblyPath.string() + ".";
		return result;
	}

	result.success = true;
	return result;
}
