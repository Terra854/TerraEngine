// ScriptCompilationSubsystem.cpp — Managed script compilation implementation
//
// Implements the script build coordination logic, including debounce-aware rebuild requests and automated reference synchronization. It manages the transition of engine states during a build, ensuring that gameplay is safely paused and reloaded when the Assembly-CSharp DLL is updated.

#include "ScriptCompilationSubsystem.h"

#include "../Assets/AssetDatabase.h"
#include "../Project/ProjectManager.h"
#include "ScriptBuildUtilities.h"
#include "Engine/Core/Logger/Logger.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Components/ScriptComponent.h"
#include "Engine/Scripting/ScriptingSystem.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	constexpr float kScriptBuildDebounceSeconds = 0.25f;

	// BuildProfileToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string BuildProfileToString(ScriptBuildProfile profile)
	{
		return profile == ScriptBuildProfile::Debug ? "Debug" : "Release";
	}

	// CoordinatorStateToString is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

	// RunProcessCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool RunProcessCapture(const std::filesystem::path& workingDirectory, const std::string& commandLine, std::string& output, int& exitCode)
	{
#if defined(_WIN32)
		SECURITY_ATTRIBUTES securityAttributes{};
		securityAttributes.nLength = sizeof(securityAttributes);
		securityAttributes.bInheritHandle = TRUE;

		HANDLE readHandle = nullptr;
		HANDLE writeHandle = nullptr;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
		std::wstring mutableCommandLine(commandLine.begin(), commandLine.end());
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!started)
		{
			CloseHandle(readHandle);
			return false;
		}

		std::array<char, 4096> buffer{};
		DWORD bytesRead = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) && bytesRead > 0)
		{
			output.append(buffer.data(), bytesRead);
		}

		WaitForSingleObject(processInfo.hProcess, INFINITE);
		DWORD processExitCode = 0;
		GetExitCodeProcess(processInfo.hProcess, &processExitCode);
		exitCode = static_cast<int>(processExitCode);

		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		CloseHandle(readHandle);
		return true;
#else
		constexpr auto kBuildTimeout = std::chrono::minutes(5);
		constexpr auto kPostExitPipeDrainTimeout = std::chrono::seconds(2);

		int pipeHandles[2] = {-1, -1};
		// Capture stdout and stderr together so Linux compiler diagnostics are
		// shown in the editor exactly like they would be in a terminal.
		if (pipe2(pipeHandles, O_CLOEXEC) != 0)
		{
			output = std::string("Failed to create process output pipe: ") + std::strerror(errno);
			return false;
		}

		const pid_t childProcess = fork();
		if (childProcess < 0)
		{
			output = std::string("Failed to fork dotnet build process: ") + std::strerror(errno);
			close(pipeHandles[0]);
			close(pipeHandles[1]);
			return false;
		}

		if (childProcess == 0)
		{
			// Put the shell and any dotnet children in their own process group so
			// timeout cleanup cannot accidentally signal the editor process.
			setpgid(0, 0);
			close(pipeHandles[0]);
			dup2(pipeHandles[1], STDOUT_FILENO);
			dup2(pipeHandles[1], STDERR_FILENO);
			close(pipeHandles[1]);

			if (!workingDirectory.empty())
			{
				std::error_code pathError;
				const std::filesystem::path absoluteWorkingDirectory = std::filesystem::absolute(workingDirectory, pathError);
				const std::string workingDirectoryText = pathError ? workingDirectory.string() : absoluteWorkingDirectory.string();
				if (chdir(workingDirectoryText.c_str()) != 0)
				{
					_exit(127);
				}
			}

			execl("/bin/sh", "sh", "-c", commandLine.c_str(), static_cast<char*>(nullptr));
			_exit(127);
		}

		setpgid(childProcess, childProcess);
		close(pipeHandles[1]);
		const int currentFlags = fcntl(pipeHandles[0], F_GETFL, 0);
		if (currentFlags >= 0)
		{
			fcntl(pipeHandles[0], F_SETFL, currentFlags | O_NONBLOCK);
		}

		std::array<char, 4096> buffer{};
		const auto startedAt = std::chrono::steady_clock::now();
		std::optional<std::chrono::steady_clock::time_point> childExitedAt;
		int status = 0;
		bool childExited = false;
		bool pipeClosed = false;
		bool timedOut = false;

		while (!pipeClosed)
		{
			pollfd readPoll{};
			readPoll.fd = pipeHandles[0];
			readPoll.events = POLLIN | POLLHUP | POLLERR;
			const int pollResult = poll(&readPoll, 1, 100);
			if (pollResult > 0 && (readPoll.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			{
				while (true)
				{
					const ssize_t bytesRead = read(pipeHandles[0], buffer.data(), buffer.size());
					if (bytesRead > 0)
					{
						output.append(buffer.data(), static_cast<std::size_t>(bytesRead));
						continue;
					}
					if (bytesRead == 0)
					{
						pipeClosed = true;
					}
					break;
				}
			}

			if (!childExited)
			{
				const pid_t waitResult = waitpid(childProcess, &status, WNOHANG);
				if (waitResult == childProcess)
				{
					childExited = true;
					childExitedAt = std::chrono::steady_clock::now();
				}
				else if (waitResult < 0 && errno != EINTR)
				{
					close(pipeHandles[0]);
					output += std::string("Failed to wait for dotnet build process: ") + std::strerror(errno);
					return false;
				}
			}

			const auto now = std::chrono::steady_clock::now();
			if (!childExited && now - startedAt > kBuildTimeout)
			{
				timedOut = true;
				kill(-childProcess, SIGTERM);
				std::this_thread::sleep_for(std::chrono::milliseconds(200));
				kill(-childProcess, SIGKILL);
				waitpid(childProcess, &status, 0);
				childExited = true;
				break;
			}

			// Some tools can leave grandchildren with inherited stdout/stderr
			// handles. Once the shell has exited, drain briefly for final output
			// and then let the editor continue instead of waiting forever for EOF.
			if (childExitedAt.has_value() && now - *childExitedAt > kPostExitPipeDrainTimeout)
			{
				break;
			}
		}
		close(pipeHandles[0]);

		if (!childExited)
		{
			while (waitpid(childProcess, &status, 0) < 0)
			{
				if (errno == EINTR)
				{
					continue;
				}
				output += std::string("Failed to wait for dotnet build process: ") + std::strerror(errno);
				return false;
			}
		}

		if (timedOut)
		{
			exitCode = 124;
			output += "\ndotnet build exceeded the editor script compilation timeout and was terminated.";
			return true;
		}

		if (WIFEXITED(status))
		{
			exitCode = WEXITSTATUS(status);
		}
		else if (WIFSIGNALED(status))
		{
			exitCode = 128 + WTERMSIG(status);
		}
		else
		{
			exitCode = status;
		}

		return true;
#endif
	}
}

ScriptCompilationSubsystem& ScriptCompilationSubsystem::Instance()
{
	static ScriptCompilationSubsystem instance;
	return instance;
}

// ScriptCompilationSubsystem::Initialize is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::Initialize(EditorContext& context)
{
	m_Context = &context;
	UpdateSharedState();
}

// ScriptCompilationSubsystem::Update is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::Update(float deltaTime)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		return;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_BuildInFlight && m_ActiveBuild.valid() && m_ActiveBuild.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
	{
		ScriptBuildResult result = m_ActiveBuild.get();
		m_BuildInFlight = false;
		HandleCompletedBuild(std::move(result));
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_BuildInFlight && m_Status.queuedBuild)
	{
		m_DebounceTimer -= deltaTime;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (m_DebounceTimer <= 0.0f)
		{
			BeginBuild();
		}
	}

	UpdateSharedState();
}

// ScriptCompilationSubsystem::DrawUI is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::DrawUI()
{
}

// ScriptCompilationSubsystem::Shutdown is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::GetName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ScriptCompilationSubsystem::GetName() const
{
	return "Script Compilation";
}

// ScriptCompilationSubsystem::OnProjectOpened is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_WorkspaceService.EnsureWorkspace(&workspaceError))
	{
		// Generate the root solution immediately so new projects can open in
		// Visual Studio even before the asynchronous build completes.
		TERRAENGINE_LOG(LogLevel::Warning, "Assembly-CSharp workspace generation reported an issue during project open: " << workspaceError);
		m_Status.lastSummary = workspaceError;
	}

	RequestBuild(ScriptBuildReason::ProjectOpen);
}

// ScriptCompilationSubsystem::OnProjectClosed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::RequestBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::NotifyScriptSourceChanged is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::CreateScriptAsset is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::OpenScript is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::OpenScript(const std::filesystem::path& scriptPath, std::string* outError)
{
	if (m_Context == nullptr || m_Context->projectManager == nullptr)
	{
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = "Project manager state is unavailable.";
		}
		return false;
	}

	return m_WorkspaceService.OpenScriptInExternalEditor(scriptPath, m_Context->projectManager->GetExternalScriptEditorCommand(), outError);
}

// ScriptCompilationSubsystem::OpenWorkspace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::OpenWorkspace(std::string* outError)
{
	return OpenScript({}, outError);
}

// ScriptCompilationSubsystem::RegenerateWorkspace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::RegenerateWorkspace(std::string* outError)
{
	if (!m_WorkspaceService.RegenerateWorkspace(outError))
	{
		return false;
	}

	RequestBuild(ScriptBuildReason::ManualRefresh);
	return true;
}

// ScriptCompilationSubsystem::GetStatus is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const ScriptCompilationStatus& ScriptCompilationSubsystem::GetStatus() const
{
	return m_Status;
}

// ScriptCompilationSubsystem::GetWorkspaceService is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const ScriptWorkspaceService& ScriptCompilationSubsystem::GetWorkspaceService() const
{
	return m_WorkspaceService;
}

// ScriptCompilationSubsystem::IsBusy is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::IsBusy() const
{
	return m_Status.state == ScriptCoordinatorState::Building || m_Status.state == ScriptCoordinatorState::Reloading;
}

// ScriptCompilationSubsystem::ShouldBlockEditorInteraction is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::ShouldBlockEditorInteraction() const
{
	return IsBusy();
}

// ScriptCompilationSubsystem::PrepareForBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::PrepareForBuild()
{
	auto& scriptingSystem = ScriptingSystem::Instance();
	m_WasRunningBeforeBuild = scriptingSystem.IsRunning();
	m_WasPausedBeforeBuild = scriptingSystem.IsPaused();
	m_ShouldRestorePausedGameplayState = m_WasRunningBeforeBuild;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_WasRunningBeforeBuild && !m_WasPausedBeforeBuild)
	{
		// Pause the active gameplay session while TerraEngine unloads and rebuilds
		// Assembly-CSharp so script state does not keep ticking against stale code.
		scriptingSystem.SetPaused(true);
		TERRAENGINE_LOG(LogLevel::Info, "Paused gameplay script execution while Assembly-CSharp recompiles.");
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptingSystem.HasLoadedGameplayAssembly())
	{
		const std::filesystem::path activeAssemblyPath = scriptingSystem.GetLoadedGameplayAssemblyPath();
		TERRAENGINE_LOG(LogLevel::Info, "Unloading the active gameplay assembly before rebuilding " << activeAssemblyPath << ".");
		scriptingSystem.ClearGameplayAssembly();
	}
}

// ScriptCompilationSubsystem::RestorePausedGameplayState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::TryReloadWorkspaceAssembly is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptCompilationSubsystem::TryReloadWorkspaceAssembly(std::string* outError)
{
	const std::filesystem::path assemblyPath = m_WorkspaceService.GetAssemblyPath();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (assemblyPath.empty() || !std::filesystem::exists(assemblyPath))
	{
		if (outError)
		{
			*outError = "The managed Assembly-CSharp output is unavailable at " + assemblyPath.string() + ", so TerraEngine cannot restore script execution after the failed compile.";
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
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = loadError;
		}
		return false;
	}

	TERRAENGINE_LOG(LogLevel::Info, "Reloaded the project gameplay assembly from " << assemblyPath << " after the failed compile.");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outError)
	{
		outError->clear();
	}
	return true;
}

// ScriptCompilationSubsystem::SynchronizeBoundScriptTypesFromAssets is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::SynchronizeBoundScriptTypesFromAssets()
{
	auto& assetDatabase = AssetDatabase::Instance();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (Object object : ObjectManager::instance().getObjectsWithComponents<ScriptComponent>())
	{
		ScriptComponent* scriptComponent = object.getComponent<ScriptComponent>();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (scriptComponent == nullptr)
		{
			continue;
		}

		bool bindingsChanged = false;
		auto guard = scriptComponent->acquireWriteLock();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (ScriptBinding& binding : scriptComponent->GetBindings())
		{
			const AssetMetadata* assetMetadata = binding.assetGuid == TerraEngine::EMPTY_UUID
				? nullptr
				: assetDatabase.FindAssetByGuid(binding.assetGuid);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assetMetadata == nullptr && !binding.className.empty())
			{
				assetMetadata = assetDatabase.FindScriptAssetByFullName(binding.className);
				// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (assetMetadata == nullptr)
			{
				continue;
			}

			const auto fullNameIt = assetMetadata->settings.find("scriptFullName");
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (fullNameIt == assetMetadata->settings.end() || !fullNameIt->is_string())
			{
				continue;
			}

			const std::string resolvedFullName = fullNameIt->get<std::string>();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (bindingsChanged)
		{
			scriptComponent->SynchronizeStorageFromBindings();
		}
	}
}

// ScriptCompilationSubsystem::BeginBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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

// ScriptCompilationSubsystem::HandleCompletedBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::HandleCompletedBuild(ScriptBuildResult result)
{
	m_Status.lastBuildOutput = result.buildOutput;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.buildOutput.empty())
	{
		// Mirror dotnet's stdout/stderr into the editor log so script rebuilds are
		// observable without opening the workspace or inspecting hidden state.
		TERRAENGINE_LOG(LogLevel::Info, "Assembly-CSharp build output:\n" << result.buildOutput);
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!result.success)
	{
		m_Status.state = ScriptCoordinatorState::Failed;
		m_Status.lastSummary = result.failureMessage;
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp build failed: " << result.failureMessage);
		std::string reloadError;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (TryReloadWorkspaceAssembly(&reloadError))
		{
			m_Status.gameplayAssemblyLoaded = ScriptingSystem::Instance().HasLoadedGameplayAssembly();
			m_Status.activeAssemblyPath = ScriptingSystem::Instance().GetLoadedGameplayAssemblyPath();
			m_Status.lastSummary += " Reloaded the existing managed Assembly-CSharp output.";
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ScriptingSystem::Instance().LoadGameplayAssembly(result.assemblyPath, &reloadError))
	{
		m_Status.state = ScriptCoordinatorState::Failed;
		m_Status.lastSummary = reloadError;
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp build completed but the gameplay assembly reload failed: " << reloadError);
		m_Status.lastSummary += " TerraEngine keeps only the project managed Assembly-CSharp output, so no separate rollback copy is available.";

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

// ScriptCompilationSubsystem::UpdateSharedState is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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

// ScriptCompilationSubsystem::RunBuildJob is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
ScriptCompilationSubsystem::ScriptBuildResult ScriptCompilationSubsystem::RunBuildJob(ScriptBuildProfile profile)
{
	ScriptBuildResult result;
	result.profile = profile;

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (m_Context == nullptr || m_Context->projectManager == nullptr || !m_Context->projectManager->HasActiveProject())
	{
		result.failureMessage = "No open project is available for Assembly-CSharp generation.";
		return result;
	}

	std::string workspaceError;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!m_WorkspaceService.EnsureWorkspace(&workspaceError))
	{
		result.failureMessage = workspaceError;
		return result;
	}

	const TerraEngine::Editor::Scripting::ScriptBuildInvocation invocation = TerraEngine::Editor::Scripting::CreateScriptBuildInvocation(
		m_WorkspaceService.GetProjectRoot(),
		m_WorkspaceService.GetProjectPath(),
		BuildProfileToString(profile));

	int exitCode = 0;
	if (!RunProcessCapture(invocation.workingDirectory, invocation.commandLine, result.buildOutput, exitCode))
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeScriptBuildLaunchFailure(invocation);
		if (!result.buildOutput.empty())
		{
			result.failureMessage += " " + result.buildOutput;
		}
		return result;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (exitCode != 0)
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeScriptBuildExitFailure(invocation, exitCode, result.buildOutput);
		return result;
	}

	result.assemblyPath = m_WorkspaceService.GetAssemblyPath();
	if (!std::filesystem::exists(result.assemblyPath))
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeMissingScriptAssembly(result.assemblyPath);
		return result;
	}

	result.success = true;
	return result;
}
