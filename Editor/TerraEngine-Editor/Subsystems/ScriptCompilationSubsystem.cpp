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
#include "NativeAotProjectWorkspace.h"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstring>
#include <exception>
#include <regex>
#include <system_error>

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

#if defined(_WIN32)
	std::string DescribeWindowsProcessError(std::string_view operation, DWORD error)
	{
		// Capture the numeric Win32 code as well as the localized system message.
		// The number remains stable in automation evidence even when the editor
		// runs under a different display language.
		return std::string(operation) + " failed with Windows error "
			+ std::to_string(error) + ": "
			+ std::system_category().message(static_cast<int>(error)) + ".";
	}
#endif

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
		case ScriptCoordinatorState::Canceled:
			return "Canceled";
		}

		return "Unknown";
	}

	// RunProcessCapture is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool RunProcessCapture(const std::filesystem::path& workingDirectory, const std::string& commandLine, const std::atomic_bool& cancellationRequested, std::string& output, int& exitCode, bool& canceled)
	{
		canceled = false;
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

		// A kill-on-close Job Object owns the complete dotnet/MSBuild/compiler
		// process tree. Canceling only the directly launched dotnet process can
		// otherwise leave Roslyn or MSBuild descendants writing into a project
		// after the editor has closed it.
		HANDLE jobHandle = CreateJobObjectW(nullptr, nullptr);
		if (jobHandle == nullptr)
		{
			const DWORD jobError = GetLastError();
			CloseHandle(writeHandle);
			CloseHandle(readHandle);
			output = DescribeWindowsProcessError("CreateJobObjectW", jobError);
			return false;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits{};
		jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if (!SetInformationJobObject(
				jobHandle,
				JobObjectExtendedLimitInformation,
				&jobLimits,
				sizeof(jobLimits)))
		{
			const DWORD jobError = GetLastError();
			CloseHandle(jobHandle);
			CloseHandle(writeHandle);
			CloseHandle(readHandle);
			output = DescribeWindowsProcessError("SetInformationJobObject", jobError);
			return false;
		}

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
			// Suspend until job assignment succeeds so no compiler child can
			// escape the cancellation boundary between launch and assignment.
			CREATE_NO_WINDOW | CREATE_SUSPENDED,
			nullptr,
			workingDirectory.wstring().c_str(),
			&startupInfo,
			&processInfo);
		const DWORD launchError = started ? ERROR_SUCCESS : GetLastError();

		CloseHandle(writeHandle);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!started)
		{
			output = DescribeWindowsProcessError("CreateProcessW", launchError);
			CloseHandle(jobHandle);
			CloseHandle(readHandle);
			return false;
		}
		if (!AssignProcessToJobObject(jobHandle, processInfo.hProcess))
		{
			const DWORD assignmentError = GetLastError();
			TerminateProcess(processInfo.hProcess, assignmentError);
			WaitForSingleObject(processInfo.hProcess, INFINITE);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			CloseHandle(jobHandle);
			CloseHandle(readHandle);
			output = DescribeWindowsProcessError("AssignProcessToJobObject", assignmentError);
			return false;
		}
		if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1))
		{
			const DWORD resumeError = GetLastError();
			// Closing this configured job terminates the still-suspended process
			// and any descendants before their project handles are released.
			CloseHandle(jobHandle);
			WaitForSingleObject(processInfo.hProcess, INFINITE);
			CloseHandle(processInfo.hThread);
			CloseHandle(processInfo.hProcess);
			CloseHandle(readHandle);
			output = DescribeWindowsProcessError("ResumeThread", resumeError);
			return false;
		}

		std::array<char, 4096> buffer{};
		bool processExited = false;
		bool terminationRequested = false;
		while (!processExited)
		{
			DWORD available = 0u;
			if (PeekNamedPipe(readHandle, nullptr, 0u, nullptr, &available, nullptr) && available > 0u)
			{
				DWORD bytesRead = 0u;
				const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
				if (ReadFile(readHandle, buffer.data(), requested, &bytesRead, nullptr) && bytesRead > 0u)
				{
					output.append(buffer.data(), bytesRead);
				}
			}

			if (!terminationRequested && cancellationRequested.load(std::memory_order_relaxed))
			{
				// Terminate the whole job, not just dotnet, so cancellation is a
				// protected boundary for all compiler descendants.
				if (!TerminateJobObject(jobHandle, ERROR_CANCELLED))
				{
					// A direct termination remains a conservative fallback if
					// the operating system rejects job-wide termination.
					TerminateProcess(processInfo.hProcess, ERROR_CANCELLED);
				}
				canceled = true;
				terminationRequested = true;
			}
			processExited = WaitForSingleObject(processInfo.hProcess, 50u) == WAIT_OBJECT_0;
		}

		// Drain output already written before process termination/completion.
		while (true)
		{
			DWORD available = 0u;
			if (!PeekNamedPipe(readHandle, nullptr, 0u, nullptr, &available, nullptr) || available == 0u)
			{
				break;
			}
			DWORD bytesRead = 0u;
			if (!ReadFile(readHandle, buffer.data(), std::min<DWORD>(available, static_cast<DWORD>(buffer.size())), &bytesRead, nullptr) || bytesRead == 0u)
			{
				break;
			}
			output.append(buffer.data(), bytesRead);
		}

		DWORD processExitCode = 0;
		GetExitCodeProcess(processInfo.hProcess, &processExitCode);
		exitCode = static_cast<int>(processExitCode);

		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		// Normal completion can still leave a tool holding inherited handles.
		// Closing the kill-on-close job guarantees those descendants cannot
		// outlive the captured build operation.
		CloseHandle(jobHandle);
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
			if (!childExited && cancellationRequested.load(std::memory_order_relaxed))
			{
				canceled = true;
				kill(-childProcess, SIGTERM);
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				kill(-childProcess, SIGKILL);
				waitpid(childProcess, &status, 0);
				childExited = true;
				break;
			}
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
	// Shutdown must not let a compiler process retain project files after the
	// editor context is released. The worker observes this token and terminates
	// its complete Windows Job Object before the future is joined.
	m_CancelRequested.store(true, std::memory_order_relaxed);
	CancelAndDrainActiveBuild();

	m_Context = nullptr;
	m_Status = ScriptCompilationStatus{};
	m_BuildInFlight = false;
	m_DebounceTimer = 0.0f;
	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
	m_CancelRequested.store(false, std::memory_order_relaxed);
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
	m_CancelRequested.store(false, std::memory_order_relaxed);
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
	else
	{
		std::string readinessError;
		if (TryLoadManagedReadiness(&readinessError))
		{
			PublishAttachableScriptTypes();
			UpdateSharedState();
			return;
		}
		// A readiness miss is expected on a clean checkout. Keep the reason in
		// the status so project-opening diagnostics can distinguish a cold build
		// from recovery after stale or corrupt derived managed output.
		m_Status.lastSummary = readinessError;
	}

	RequestBuild(ScriptBuildReason::ProjectOpen);
	PublishAttachableScriptTypes();
}

// ScriptCompilationSubsystem::OnProjectClosed is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::OnProjectClosed()
{
	// Project close clears the workspace service below. Cancel and join first so
	// the worker cannot race that mutation while reading project paths or while
	// a dotnet descendant is still writing derived outputs.
	m_Status.queuedBuild = false;
	m_CancelRequested.store(true, std::memory_order_relaxed);
	CancelAndDrainActiveBuild();

	m_Status = ScriptCompilationStatus{};
	m_ShouldRestorePausedGameplayState = false;
	m_WasRunningBeforeBuild = false;
	m_WasPausedBeforeBuild = false;
	m_CancelRequested.store(false, std::memory_order_relaxed);
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
	const std::string priorReadinessSummary = m_Status.lastSummary;
	m_Status.reusedReadyOutput = false;
	m_Status.forcedBuild = reason == ScriptBuildReason::ManualRefresh || reason == ScriptBuildReason::ExplicitRebuild;
	m_Status.queuedBuild = true;
	m_Status.cancellationRequested = false;
	m_Status.progress = 0.05;
	m_Status.diagnostics.clear();
	m_CancelRequested.store(false, std::memory_order_relaxed);
	m_Status.state = ScriptCoordinatorState::Queued;
	m_Status.lastSummary = "A project script build is queued for " + BuildProfileToString(m_Status.activeProfile) + ".";
	if (reason == ScriptBuildReason::ProjectOpen && !priorReadinessSummary.empty())
	{
		m_Status.lastSummary += " Readiness fallback: " + priorReadinessSummary;
	}
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

bool ScriptCompilationSubsystem::RequestCancellation()
{
	if (!m_Status.queuedBuild && !m_BuildInFlight)
	{
		return false;
	}

	m_Status.cancellationRequested = true;
	m_CancelRequested.store(true, std::memory_order_relaxed);
	if (!m_BuildInFlight)
	{
		// A queued build has not crossed the worker boundary, so cancellation
		// leaves no compilation output or assembly publication to roll back.
		m_Status.queuedBuild = false;
		m_Status.state = ScriptCoordinatorState::Canceled;
		m_Status.progress = 1.0;
		m_Status.lastSummary = "Assembly-CSharp compilation was canceled before launch.";
		UpdateSharedState();
	}
	return true;
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

bool ScriptCompilationSubsystem::CreateApprovedScript(const ScriptCreationRequest& request, ScriptCreationResult& outResult, std::string* outError)
{
	if (!m_WorkspaceService.CreateApprovedScript(request, outResult, outError))
	{
		return false;
	}
	AssetDatabase::Instance().ImportAsset(outResult.absolutePath, true);
	PublishAttachableScriptTypes();
	RequestBuild(ScriptBuildReason::FileChange);
	return true;
}

bool ScriptCompilationSubsystem::SynchronizeWorkspace(ScriptWorkspaceStatus& outStatus, std::string* outError)
{
	return m_WorkspaceService.SynchronizeWorkspace(outStatus, outError);
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

	RequestBuild(ScriptBuildReason::ExplicitRebuild);
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

bool ScriptCompilationSubsystem::TryLoadManagedReadiness(std::string* outError)
{
	const std::string profile = BuildProfileToString(m_Status.activeProfile);
	std::string validationError;
	const ManagedReadinessValidation validation = m_WorkspaceService.ValidateManagedReadiness(profile, nullptr, &validationError);
	m_Status.readinessValidation = validation;
	if (validation != ManagedReadinessValidation::Valid)
	{
		if (outError != nullptr)
		{
			*outError = validationError;
		}
		return false;
	}

	// The manifest only proves that bytes and inputs match. The managed runtime
	// still owns load validation; failure here deliberately falls back to an
	// explicit rebuild instead of treating a cache record as executable truth.
	std::string loadError;
	const std::filesystem::path assemblyPath = m_WorkspaceService.GetAssemblyPath();
	if (!ScriptingSystem::Instance().LoadGameplayAssembly(assemblyPath, &loadError))
	{
		m_Status.readinessValidation = ManagedReadinessValidation::OutputHashMismatch;
		if (outError != nullptr)
		{
			*outError = "The manifest-validated managed output could not be loaded: " + loadError;
		}
		return false;
	}

	m_Status.state = ScriptCoordinatorState::Ready;
	m_Status.progress = 1.0;
	m_Status.gameplayAssemblyLoaded = true;
	m_Status.reusedReadyOutput = true;
	m_Status.forcedBuild = false;
	m_Status.activeAssemblyPath = assemblyPath;
	m_Status.lastSummary = "Assembly-CSharp readiness manifest matched and the existing managed output was loaded without rebuilding.";
	AssetDatabase::Instance().SetScriptCompileState("Ready", m_Status.lastSummary);
	TERRAENGINE_LOG(LogLevel::Info, "Reused manifest-validated Assembly-CSharp output from " << assemblyPath << ".");
	if (outError != nullptr)
	{
		outError->clear();
	}
	return true;
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
	auto& objectManager = ObjectManager::instance();
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (Object object : objectManager.getObjectsWithComponents<ScriptComponent>())
	{
		ScriptComponent* scriptComponent = objectManager.GetComponent<ScriptComponent>(object);
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

void ScriptCompilationSubsystem::PublishAttachableScriptTypes()
{
	std::vector<AttachableScriptType> types;
	const std::filesystem::path projectRoot = m_WorkspaceService.GetProjectRoot();
	for (const auto& [path, metadata] : AssetDatabase::Instance().GetAssets())
	{
		const auto attachable = metadata.settings.find("isAttachable");
		const auto qualifiedName = metadata.settings.find("scriptFullName");
		if (attachable == metadata.settings.end() || !attachable->is_boolean() || !attachable->get<bool>()
			|| qualifiedName == metadata.settings.end() || !qualifiedName->is_string() || qualifiedName->get<std::string>().empty())
		{
			continue;
		}

		std::error_code relativeError;
		const std::filesystem::path sourcePath = projectRoot.empty()
			? path
			: std::filesystem::relative(path, projectRoot, relativeError);
		types.push_back({qualifiedName->get<std::string>(), relativeError ? path : sourcePath});
	}
	std::sort(types.begin(), types.end(), [](const AttachableScriptType& left, const AttachableScriptType& right) {
		return left.qualifiedName < right.qualifiedName;
	});
	types.erase(std::unique(types.begin(), types.end(), [](const AttachableScriptType& left, const AttachableScriptType& right) {
		return left.qualifiedName == right.qualifiedName;
	}), types.end());
	m_Status.attachableTypes = std::move(types);
}

void ScriptCompilationSubsystem::PublishCompilerDiagnostics(std::string_view buildOutput)
{
	m_Status.diagnostics.clear();
	// MSBuild emits one stable diagnostic per line in this form on every
	// supported platform. Unmatched informational output remains available in
	// lastBuildOutput but is not misrepresented as a compiler diagnostic.
	static const std::regex diagnosticPattern(
		R"(^(.+)\(([0-9]+),([0-9]+)\):\s+(warning|error)\s+([A-Za-z]+[0-9]+):\s+(.+?)(?:\s+\[.*\])?$)",
		std::regex::icase);
	std::istringstream lines{std::string(buildOutput)};
	std::string line;
	while (std::getline(lines, line))
	{
		std::smatch match;
		if (!std::regex_match(line, match, diagnosticPattern))
		{
			continue;
		}
		ScriptCompilerDiagnostic diagnostic;
		diagnostic.file = match[1].str();
		diagnostic.line = static_cast<std::uint32_t>(std::stoul(match[2].str()));
		diagnostic.column = static_cast<std::uint32_t>(std::stoul(match[3].str()));
		diagnostic.severity = match[4].str();
		std::transform(diagnostic.severity.begin(), diagnostic.severity.end(), diagnostic.severity.begin(), [](unsigned char value) {
			return static_cast<char>(std::tolower(value));
		});
		diagnostic.code = match[5].str();
		diagnostic.message = match[6].str();
		m_Status.diagnostics.push_back(std::move(diagnostic));
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
	m_Status.progress = 0.2;
	m_Status.cancellationRequested = false;
	m_CancelRequested.store(false, std::memory_order_relaxed);
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

void ScriptCompilationSubsystem::CancelAndDrainActiveBuild()
{
	if (!m_BuildInFlight || !m_ActiveBuild.valid())
	{
		m_BuildInFlight = false;
		return;
	}

	try
	{
		// get() both joins the async worker and consumes its result. Project close
		// deliberately discards that result because publishing or reloading an
		// assembly after the close boundary would target the wrong session.
		static_cast<void>(m_ActiveBuild.get());
	}
	catch (const std::exception& exception)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp worker failed while editor lifecycle teardown drained it: " << exception.what());
	}
	catch (...)
	{
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp worker failed with an unknown exception while editor lifecycle teardown drained it.");
	}
	m_BuildInFlight = false;
}

// ScriptCompilationSubsystem::HandleCompletedBuild is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
void ScriptCompilationSubsystem::HandleCompletedBuild(ScriptBuildResult result)
{
	m_CancelRequested.store(false, std::memory_order_relaxed);
	m_Status.cancellationRequested = result.canceled;
	m_Status.lastBuildOutput = result.buildOutput;
	PublishCompilerDiagnostics(result.buildOutput);
	if (result.canceled)
	{
		m_Status.state = ScriptCoordinatorState::Canceled;
		m_Status.queuedBuild = false;
		m_Status.cancellationRequested = true;
		m_Status.progress = 1.0;
		m_Status.lastSummary = "Assembly-CSharp compilation was canceled before assembly publication.";
		RestorePausedGameplayState();
		AssetDatabase::Instance().SetScriptCompileState("Canceled", m_Status.lastSummary);
		UpdateSharedState();
		return;
	}
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
		m_Status.progress = 1.0;
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
	m_Status.progress = 0.85;
	m_Status.lastSummary = "Reloading the newly built Assembly-CSharp runtime into the current session.";
	UpdateSharedState();

	SynchronizeBoundScriptTypesFromAssets();

	std::string reloadError;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!ScriptingSystem::Instance().LoadGameplayAssembly(result.assemblyPath, &reloadError))
	{
		m_Status.state = ScriptCoordinatorState::Failed;
		m_Status.progress = 1.0;
		m_Status.lastSummary = reloadError;
		TERRAENGINE_LOG(LogLevel::Error, "Assembly-CSharp build completed but the gameplay assembly reload failed: " << reloadError);
		m_Status.lastSummary += " TerraEngine keeps only the project managed Assembly-CSharp output, so no separate rollback copy is available.";

		RestorePausedGameplayState();
		AssetDatabase::Instance().SetScriptCompileState("CompileError", m_Status.lastSummary);
		UpdateSharedState();
		return;
	}

	m_Status.state = ScriptCoordinatorState::Ready;
	m_Status.progress = 1.0;
	m_Status.gameplayAssemblyLoaded = true;
	m_Status.reusedReadyOutput = false;
	m_Status.activeAssemblyPath = result.assemblyPath;
	std::string manifestError;
	if (!m_WorkspaceService.PublishManagedReadiness(BuildProfileToString(result.profile), &manifestError))
	{
		// The freshly loaded assembly is still correct for this session. Do not
		// publish a partial readiness claim: the next project open will rebuild
		// rather than trusting incomplete cache metadata.
		m_Status.readinessValidation = ManagedReadinessValidation::Corrupt;
		m_Status.lastSummary = "Assembly-CSharp built and reloaded successfully, but managed readiness was not persisted: " + manifestError;
		TERRAENGINE_LOG(LogLevel::Warning, m_Status.lastSummary);
	}
	else
	{
		m_Status.readinessValidation = ManagedReadinessValidation::Valid;
		m_Status.lastSummary = "Assembly-CSharp built, reloaded, and atomically published as managed-ready output.";
	}
	TERRAENGINE_LOG(LogLevel::Info, "Assembly-CSharp built successfully and reloaded from " << result.assemblyPath << ".");
	RestorePausedGameplayState();
	AssetDatabase::Instance().SetScriptCompileState("Ready", m_Status.lastSummary);
	PublishAttachableScriptTypes();
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

	// Snapshot every canonical path before the worker launches dotnet. Project
	// close joins this worker before mutating ScriptWorkspaceService, and these
	// immutable values ensure diagnostics and output checks never observe a
	// temporary alias or a later project's workspace.
	const std::filesystem::path canonicalProjectRoot = m_WorkspaceService.GetProjectRoot();
	const std::filesystem::path canonicalProjectPath = m_WorkspaceService.GetProjectPath();
	const std::filesystem::path canonicalAssemblyPath = m_WorkspaceService.GetAssemblyPath();

	NativeAotProjectWorkspace processWorkspace(canonicalProjectRoot);
	std::string processWorkspaceError;
	if (!processWorkspace.Prepare(processWorkspaceError))
	{
		result.failureMessage = "Could not prepare the private short-path workspace for Assembly-CSharp compilation: "
			+ processWorkspaceError;
		return result;
	}

	const TerraEngine::Editor::Scripting::ScriptBuildInvocation invocation = TerraEngine::Editor::Scripting::CreateScriptBuildInvocation(
		canonicalProjectRoot,
		canonicalProjectPath,
		BuildProfileToString(profile),
		processWorkspace.BuildProjectRoot(),
		processWorkspace.BuildProjectRoot() / canonicalProjectPath.filename());

	int exitCode = 0;
	if (!RunProcessCapture(invocation.processWorkingDirectory, invocation.commandLine, m_CancelRequested, result.buildOutput, exitCode, result.canceled))
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeScriptBuildLaunchFailure(
			invocation,
			result.buildOutput);
		return result;
	}
	if (result.canceled)
	{
		result.failureMessage = "Assembly-CSharp compilation was canceled.";
		return result;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (exitCode != 0)
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeScriptBuildExitFailure(invocation, exitCode, result.buildOutput);
		return result;
	}

	result.assemblyPath = canonicalAssemblyPath;
	if (!std::filesystem::exists(result.assemblyPath))
	{
		result.failureMessage = TerraEngine::Editor::Scripting::DescribeMissingScriptAssembly(result.assemblyPath);
		return result;
	}

	// Keep the alias alive until process completion and pipe drain above, then
	// remove it explicitly before publishing success. The destructor remains a
	// conservative fallback for every earlier failure return.
	std::string cleanupError;
	if (!processWorkspace.Cleanup(cleanupError))
	{
		result.failureMessage = "Assembly-CSharp compiled successfully, but its private short-path workspace could not be removed: "
			+ cleanupError;
		return result;
	}

	result.success = true;
	return result;
}
