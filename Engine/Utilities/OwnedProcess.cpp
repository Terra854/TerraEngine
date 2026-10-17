// Owned process capture extracted from the managed build coordinator.
#include "OwnedProcess.h"
#include <array>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace {
	// Exception paths must release ownership as reliably as normal completion.
	template<typename Cleanup> struct ScopeExit {
		Cleanup cleanup;
		~ScopeExit() { cleanup(); }
	};
#ifdef _WIN32
	void CloseOwned(HANDLE& handle) { if (handle) { CloseHandle(handle); handle = nullptr; } }
	class OwnedJobProcesses {
	public:
		~OwnedJobProcesses() { for (auto& process : processes_) CloseOwned(process.handle); }
		DWORD Capture(HANDLE job, std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max()) noexcept {
			try {
				// The job owns membership; retain process objects rather than reopening
				// PIDs after termination, when identities can already be recycled.
				std::vector<ULONG_PTR> storage(66U);
				for (unsigned attempt = 0; attempt < 8U; ++attempt) {
					if (std::chrono::steady_clock::now() >= deadline) return ERROR_TIMEOUT;
					auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(storage.data());
					const BOOL queried = QueryInformationJobObject(job, JobObjectBasicProcessIdList, list,
						static_cast<DWORD>(storage.size() * sizeof(ULONG_PTR)), nullptr);
					if (queried && list->NumberOfProcessIdsInList == list->NumberOfAssignedProcesses) {
						for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) {
							if (std::chrono::steady_clock::now() >= deadline) return ERROR_TIMEOUT;
							const auto pid = static_cast<DWORD>(list->ProcessIdList[index]);
							if (std::any_of(processes_.begin(), processes_.end(), [pid](const auto& process) { return process.pid == pid; })) continue;
							HANDLE handle = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_INFORMATION, FALSE, pid);
							if (!handle) {
								const auto error = GetLastError();
								if (error == ERROR_INVALID_PARAMETER) continue; // Already exited before this observation.
								return error;
							}
							ScopeExit close{[&] { CloseOwned(handle); }};
							BOOL member = FALSE;
							if (!IsProcessInJob(handle, job, &member)) return GetLastError();
							if (!member) continue; // Never retain or signal an unrelated recycled PID.
							processes_.push_back({pid, handle});
							handle = nullptr;
						}
						return ERROR_SUCCESS;
					}
					// Some Windows versions return success with a truncated PID list.
					// Counts, not only ERROR_MORE_DATA, determine complete membership.
					const auto error = queried ? ERROR_MORE_DATA : GetLastError();
					if (error != ERROR_MORE_DATA) return error;
					const auto required = static_cast<std::size_t>(list->NumberOfAssignedProcesses) + 2U;
					if (required <= storage.size() || required > MAXDWORD / sizeof(ULONG_PTR)) return ERROR_MORE_DATA;
					storage.resize(required);
				}
				return ERROR_MORE_DATA; // Continuous spawning cannot extend capture without bound.
			} catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
		}
		DWORD Inspect(bool& settled, std::chrono::steady_clock::time_point deadline) const noexcept {
			settled = true;
			for (const auto& process : processes_) {
				if (std::chrono::steady_clock::now() >= deadline) { settled = false; return ERROR_TIMEOUT; }
				const auto waited = WaitForSingleObject(process.handle, 0);
				if (waited == WAIT_FAILED) return GetLastError();
				if (waited != WAIT_OBJECT_0) settled = false;
			}
			return ERROR_SUCCESS;
		}
	private:
		struct Process { DWORD pid; HANDLE handle; };
		std::vector<Process> processes_;
	};
	DWORD SettleOwnedJob(HANDLE job, OwnedJobProcesses& processes,
		std::optional<std::chrono::steady_clock::time_point>& deadline) noexcept {
		if (!job) return ERROR_SUCCESS;
		// The first settlement owns the entire two-second budget, including
		// capture and exception cleanup. Retries never restart that deadline.
		if (!deadline) deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		// Close descendant admission before the final membership capture. A new
		// child exceeding this cleanup-only limit is terminated by Windows rather
		// than escaping between enumeration and the job-wide termination request.
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
		limits.BasicLimitInformation.ActiveProcessLimit = 1U;
		DWORD firstError = ERROR_SUCCESS;
		if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) firstError = GetLastError();
		const auto captured = processes.Capture(job, *deadline);
		if (!firstError) firstError = captured;
		if (!TerminateJobObject(job, ERROR_CANCELLED) && !firstError) firstError = GetLastError();
		// Accounting can reach zero before a retained process object signals.
		// Both observations must settle inside the existing two-second bound.
		for (;;) {
			JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
			const BOOL queried = QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr);
			if (!queried && !firstError) firstError = GetLastError();
			bool handlesSettled = false;
			const auto inspected = processes.Inspect(handlesSettled, *deadline);
			if (inspected && !firstError) firstError = inspected;
			if (queried && accounting.ActiveProcesses == 0 && !inspected && handlesSettled) return firstError;
			if (std::chrono::steady_clock::now() >= *deadline) return ERROR_TIMEOUT;
			Sleep(2);
		}
	}
	std::wstring WideUtf8(const std::string& text) {
		if (text.empty()) return {};
		const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
		if (!count) throw std::invalid_argument("Process arguments/environment must be valid UTF-8.");
		std::wstring result(count, L'\0');
		MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), result.data(), count);
		return result;
	}
	std::vector<wchar_t> ChildEnvironment(const std::map<std::string, std::string>& overrides) {
		// Windows variable names are case-insensitive; preserve drive cwd entries.
		struct Compare { bool operator()(const std::wstring& a, const std::wstring& b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
		std::map<std::wstring, std::wstring, Compare> values;
		wchar_t* inherited = GetEnvironmentStringsW();
		if (!inherited) throw std::runtime_error("Could not capture process environment.");
		ScopeExit cleanup{[&] { FreeEnvironmentStringsW(inherited); }};
		for (auto* entry = inherited; *entry; entry += wcslen(entry) + 1) {
			std::wstring value(entry);
			const auto split = value.find(L'=', value.front() == L'=' ? 1 : 0);
			if (split != std::wstring::npos) values[value.substr(0, split)] = value.substr(split + 1);
		}
		for (const auto& [name, value] : overrides) {
			if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) throw std::invalid_argument("Invalid process environment override.");
			values[WideUtf8(name)] = WideUtf8(value);
		}
		std::vector<wchar_t> block;
		for (const auto& [name, value] : values) {
			const auto entry = name + L"=" + value;
			block.insert(block.end(), entry.begin(), entry.end()); block.push_back(L'\0');
		}
		block.push_back(L'\0');
		if (block.size() == 1) block.push_back(L'\0');
		return block;
	}
std::string DescribeWindowsProcessError(std::string_view operation, DWORD error) {
	return std::string(operation) + " failed with Windows error " + std::to_string(error) + ": " + std::system_category().message(static_cast<int>(error));
}
#endif
bool RunProcessCapture(const TerraEngine::Utilities::OwnedProcessRequest& request, const std::atomic_bool& cancellationRequested, std::string& output, int& exitCode, bool& canceled, bool& timedOut)
	{
		canceled = false;
		const auto& workingDirectory = request.workingDirectory;
		const auto& commandLine = request.commandLine;
#if defined(_WIN32)
		SECURITY_ATTRIBUTES securityAttributes{};
		securityAttributes.nLength = sizeof(securityAttributes);
		securityAttributes.bInheritHandle = TRUE;

		HANDLE readHandle = nullptr;
		HANDLE writeHandle = nullptr;
		HANDLE jobHandle = nullptr;
		OwnedJobProcesses ownedProcesses;
		std::optional<std::chrono::steady_clock::time_point> settlementDeadline;
		PROCESS_INFORMATION processInfo{};
		ScopeExit cleanup{[&] {
			// Exception cleanup has the same bounded descendant settlement as the
			// normal path. Closing the job remains the final fallback on OS error.
			(void)SettleOwnedJob(jobHandle, ownedProcesses, settlementDeadline);
			CloseOwned(jobHandle);
			if (processInfo.hProcess) {
				// A fallback root wait shares the first termination budget; an expired
				// deadline must not acquire another second during exception cleanup.
				DWORD remaining = 1000U;
				if (settlementDeadline) {
					const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
						*settlementDeadline - std::chrono::steady_clock::now()).count();
					remaining = static_cast<DWORD>(std::clamp<decltype(milliseconds)>(milliseconds, 0, 1000));
				}
				WaitForSingleObject(processInfo.hProcess, remaining);
			}
			CloseOwned(processInfo.hThread); CloseOwned(processInfo.hProcess);
			CloseOwned(writeHandle); CloseOwned(readHandle);
		}};

		if (!CreatePipe(&readHandle, &writeHandle, &securityAttributes, 0))
		{
			return false;
		}

		SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0);

		// A kill-on-close Job Object owns the complete dotnet/MSBuild/compiler
		// process tree. Canceling only the directly launched dotnet process can
		// otherwise leave Roslyn or MSBuild descendants writing into a project
		// after the editor has closed it.
		jobHandle = CreateJobObjectW(nullptr, nullptr);
		if (jobHandle == nullptr)
		{
			const DWORD jobError = GetLastError();
			CloseOwned(writeHandle);
			CloseOwned(readHandle);
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
			CloseOwned(jobHandle);
			CloseOwned(writeHandle);
			CloseOwned(readHandle);
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

		std::wstring mutableCommandLine = WideUtf8(commandLine);
		auto environment = ChildEnvironment(request.environment);
		const BOOL started = CreateProcessW(
			nullptr,
			mutableCommandLine.data(),
			nullptr,
			nullptr,
			TRUE,
			// Suspend until job assignment succeeds so no compiler child can
			// escape the cancellation boundary between launch and assignment.
			CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
			environment.data(),
			workingDirectory.empty() ? nullptr : workingDirectory.c_str(),
			&startupInfo,
			&processInfo);
		const DWORD launchError = started ? ERROR_SUCCESS : GetLastError();

		CloseOwned(writeHandle);

		if (!started)
		{
			output = DescribeWindowsProcessError("CreateProcessW", launchError);
			CloseOwned(jobHandle);
			CloseOwned(readHandle);
			return false;
		}
		if (!AssignProcessToJobObject(jobHandle, processInfo.hProcess))
		{
			const DWORD assignmentError = GetLastError();
			TerminateProcess(processInfo.hProcess, assignmentError);
			WaitForSingleObject(processInfo.hProcess, 1000);
			CloseOwned(processInfo.hThread);
			CloseOwned(processInfo.hProcess);
			CloseOwned(jobHandle);
			CloseOwned(readHandle);
			output = DescribeWindowsProcessError("AssignProcessToJobObject", assignmentError);
			return false;
		}
		if (ResumeThread(processInfo.hThread) == static_cast<DWORD>(-1))
		{
			const DWORD resumeError = GetLastError();
			// Closing this configured job terminates the still-suspended process
			// and any descendants before their project handles are released.
			CloseOwned(jobHandle);
			WaitForSingleObject(processInfo.hProcess, 1000);
			CloseOwned(processInfo.hThread);
			CloseOwned(processInfo.hProcess);
			CloseOwned(readHandle);
			output = DescribeWindowsProcessError("ResumeThread", resumeError);
			return false;
		}

		std::array<char, 4096> buffer{};
		bool processExited = false;
		bool terminationRequested = false;
		auto terminationDeadline = std::chrono::steady_clock::time_point::max();
		const auto deadline = std::chrono::steady_clock::now() + request.timeout;
		while (!processExited)
		{
			const auto captureError = ownedProcesses.Capture(jobHandle);
			if (captureError != ERROR_SUCCESS) {
				output += DescribeWindowsProcessError("Owned process membership capture", captureError);
				return false; // The scope guard still terminates and waits for retained owners.
			}
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

			if (!terminationRequested && (cancellationRequested.load(std::memory_order_relaxed) || std::chrono::steady_clock::now() >= deadline))
			{
				// Terminate the whole job, not just dotnet, so cancellation is a
				// protected boundary for all compiler descendants.
				canceled = cancellationRequested.load(std::memory_order_relaxed);
				timedOut = !canceled;
				const auto settlement = SettleOwnedJob(jobHandle, ownedProcesses, settlementDeadline);
				if (settlement != ERROR_SUCCESS) {
					output += DescribeWindowsProcessError("Owned process tree settlement", settlement);
					return false;
				}
				terminationRequested = true;
				terminationDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
			}
			const auto waited = WaitForSingleObject(processInfo.hProcess, 50u);
			if (waited == WAIT_FAILED) { output += DescribeWindowsProcessError("WaitForSingleObject", GetLastError()); return false; }
			processExited = waited == WAIT_OBJECT_0;
			if (!processExited && terminationRequested && std::chrono::steady_clock::now() >= terminationDeadline) {
				output += "Owned process did not settle within the bounded termination deadline.";
				return false;
			}
		}

		// Stop surviving writers before draining. An exited root with a noisy
		// descendant must not keep this final capture loop alive indefinitely.
		const DWORD settlementError = SettleOwnedJob(jobHandle, ownedProcesses, settlementDeadline);
		CloseOwned(jobHandle);
		if (settlementError != ERROR_SUCCESS) {
			output += DescribeWindowsProcessError("Owned process tree settlement", settlementError);
			return false;
		}
		// Drain output already written before process termination/completion.
		const auto drainDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (true)
		{
			if (std::chrono::steady_clock::now() >= drainDeadline) {
				output += "Owned process output exceeded the bounded final drain deadline.";
				return false;
			}
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

		CloseOwned(processInfo.hThread);
		CloseOwned(processInfo.hProcess);
		// Normal completion can still leave a tool holding inherited handles.
		// Closing the kill-on-close job guarantees those descendants cannot
		// outlive the captured build operation.
		CloseOwned(jobHandle);
		CloseOwned(readHandle);
		return true;
#else
		const auto kBuildTimeout = request.timeout;
		constexpr auto kPostExitPipeDrainTimeout = std::chrono::seconds(2);
		// Build child environment and cwd before fork: the child performs only
		// async-signal-safe OS calls and cannot change the editor environment.
		std::map<std::string, std::string> values;
		for (auto** entry = environ; *entry; ++entry) {
			const std::string value(*entry); const auto split = value.find('=');
			if (split != std::string::npos) values[value.substr(0, split)] = value.substr(split + 1);
		}
		for (const auto& [name, value] : request.environment) {
			if (name.empty() || name.find('=') != std::string::npos || name.find('\0') != std::string::npos || value.find('\0') != std::string::npos) throw std::invalid_argument("Invalid process environment override.");
			values[name] = value;
		}
		std::vector<std::string> environment;
		for (const auto& [name, value] : values) environment.push_back(name + "=" + value);
		std::vector<char*> environmentPointers;
		for (auto& value : environment) environmentPointers.push_back(value.data());
		environmentPointers.push_back(nullptr);
		const std::string directory = workingDirectory.empty() ? std::string{} : std::filesystem::absolute(workingDirectory).string();

		int pipeHandles[2] = {-1, -1};
		// Capture stdout and stderr together so Linux compiler diagnostics are
		// shown in the editor exactly like they would be in a terminal.
		if (pipe2(pipeHandles, O_CLOEXEC) != 0)
		{
			output = std::string("Failed to create process output pipe: ") + std::strerror(errno);
			return false;
		}

		pid_t childProcess = -1;
		bool reaped = false;
		ScopeExit cleanup{[&] {
			// Even successful shell exit cannot leave descendants holding files.
			if (childProcess > 0) { kill(-childProcess, SIGKILL); if (!reaped) { int ignored = 0; while (waitpid(childProcess, &ignored, 0) < 0 && errno == EINTR) {} } }
			for (auto& handle : pipeHandles) { if (handle >= 0) close(handle); handle = -1; }
		}};
		childProcess = fork();
		if (childProcess < 0)
		{
			output = std::string("Failed to fork dotnet build process: ") + std::strerror(errno);
			close(pipeHandles[0]); pipeHandles[0] = -1;
			close(pipeHandles[1]); pipeHandles[1] = -1;
			return false;
		}

		if (childProcess == 0)
		{
			// Put the shell and any dotnet children in their own process group so
			// timeout cleanup cannot accidentally signal the editor process.
			setpgid(0, 0);
			close(pipeHandles[0]); pipeHandles[0] = -1;
			dup2(pipeHandles[1], STDOUT_FILENO);
			dup2(pipeHandles[1], STDERR_FILENO);
			close(pipeHandles[1]); pipeHandles[1] = -1;

			if (!directory.empty())
			{
				if (chdir(directory.c_str()) != 0)
				{
					_exit(127);
				}
			}

			char* arguments[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(commandLine.c_str()), nullptr};
			execve("/bin/sh", arguments, environmentPointers.data());
			_exit(127);
		}

		setpgid(childProcess, childProcess);
		close(pipeHandles[1]); pipeHandles[1] = -1;
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

		while (!pipeClosed || !childExited)
		{
			pollfd readPoll{};
			readPoll.fd = pipeHandles[0];
			readPoll.events = POLLIN | POLLHUP | POLLERR;
			const int pollResult = pipeClosed ? 0 : poll(&readPoll, 1, 100);
			if (pipeClosed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
					reaped = true;
					childExited = true;
					childExitedAt = std::chrono::steady_clock::now();
				}
				else if (waitResult < 0 && errno != EINTR)
				{
					close(pipeHandles[0]); pipeHandles[0] = -1;
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
				reaped = true;
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
				reaped = true;
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
		close(pipeHandles[0]); pipeHandles[0] = -1;

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
			reaped = true;
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

TerraEngine::Utilities::OwnedProcessResult TerraEngine::Utilities::OwnedProcess::Run(const OwnedProcessRequest& request, const std::atomic_bool& cancellation) {
	OwnedProcessResult result;
	if (cancellation.load(std::memory_order_relaxed)) { result.canceled = true; return result; }
	try {
		result.started = RunProcessCapture(request, cancellation, result.output, result.exitCode, result.canceled, result.timedOut);
		if (result.timedOut) result.exitCode = 124;
	} catch (const std::exception& error) { result.output += error.what(); }
	return result;
}
