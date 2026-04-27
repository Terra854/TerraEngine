// ScriptWorkspaceService.cpp — Managed code workspace implementation
//
// Implements the workspace generation logic, including template expansion for solution and project files. It handles the synchronization of managed engine DLLs and source generators into the project's build directory and provides platform-specific IDE integration.

#include "ScriptWorkspaceService.h"

#include "Engine/Core/Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <Windows.h>
#include <OleAuto.h>
#include <Objbase.h>
#include <shellapi.h>
#include <wrl/client.h>
#endif

namespace
{
	// Keep the generated solution project GUID stable so workspace regeneration
	// does not churn Visual Studio metadata for the single Assembly-CSharp
	// project TerraEngine emits into each solution.
	constexpr std::string_view kSolutionProjectGuid = "{C5A544F7-6BB5-455A-8FA3-D9A6D3D8A1E2}";
#if defined(_WIN32)
	constexpr wchar_t kVsWherePath[] = L"C:\\Program Files (x86)\\Microsoft Visual Studio\\Installer\\vswhere.exe";
	constexpr wchar_t kVisualStudioDtePrefix[] = L"!VisualStudio.DTE.";
	constexpr ULONGLONG kVisualStudioAttachTimeoutMs = 15000;
	constexpr DWORD kVisualStudioAttachPollMs = 250;
#endif

	// ReplaceAll is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void ReplaceAll(std::string& text, std::string_view needle, std::string_view replacement)
	{
		std::size_t searchIndex = 0;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while ((searchIndex = text.find(needle.data(), searchIndex, needle.size())) != std::string::npos)
		{
			text.replace(searchIndex, needle.size(), replacement.data(), replacement.size());
			searchIndex += replacement.size();
		}
	}

	// LoadTextFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string LoadTextFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!input)
		{
			return {};
		}

		std::stringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

	// IsValidCSharpIdentifier is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsValidCSharpIdentifier(const std::string& className)
	{
		if (className.empty())
		{
			return false;
		}

		const auto isIdentifierStart = [](unsigned char value) {
			return std::isalpha(value) || value == '_';
		};
		const auto isIdentifierCharacter = [](unsigned char value) {
			return std::isalnum(value) || value == '_';
		};

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!isIdentifierStart(static_cast<unsigned char>(className.front())))
		{
			return false;
		}

		// std::all_of is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		return std::all_of(className.begin() + 1, className.end(), [&](char value) {
			return isIdentifierCharacter(static_cast<unsigned char>(value));
		});
	}

	// TrimWhitespace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string TrimWhitespace(std::string text)
	{
		const auto firstNonWhitespace = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
			return std::isspace(value) != 0;
		});
		const auto lastNonWhitespace = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
			return std::isspace(value) != 0;
		}).base();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (firstNonWhitespace >= lastNonWhitespace)
		{
			return {};
		}

		return std::string(firstNonWhitespace, lastNonWhitespace);
	}

	// ResolveEditorExecutableDirectory is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path ResolveEditorExecutableDirectory()
	{
#if defined(_WIN32)
		wchar_t path[MAX_PATH];
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (GetModuleFileNameW(nullptr, path, MAX_PATH) != 0)
		{
			return std::filesystem::path(path).parent_path();
		}
#endif

		return std::filesystem::current_path();
	}

	// AppendManagedRootCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void AppendManagedRootCandidates(const std::filesystem::path& baseDirectory, std::vector<std::filesystem::path>& roots)
	{
		if (baseDirectory.empty())
		{
			return;
		}

		// Development launches may set the working directory to either the build
		// output folder or the editor binary folder, so probe both shapes TerraEngine
		// uses for installed managed assets.
		roots.push_back(baseDirectory / "Managed");
		roots.push_back(baseDirectory / "Editor" / "Managed");
		roots.push_back(baseDirectory.parent_path() / "Editor" / "Managed");
	}

	// IsRepoRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool IsRepoRoot(const std::filesystem::path& candidate)
	{
		return std::filesystem::exists(candidate / "Engine" / "Managed" / "TerraEngine.csproj")
			&& std::filesystem::exists(candidate / "Editor" / "Managed" / "TerraEngine.Export.Generator" / "TerraEngine.Export.Generator.csproj");
	}

	// FindRepoRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path FindRepoRoot(std::filesystem::path start)
	{
		if (start.empty())
		{
			return {};
		}

		start = std::filesystem::absolute(start).lexically_normal();
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::filesystem::path current = start; !current.empty();)
		{
			if (IsRepoRoot(current))
			{
				return current;
			}

			const std::filesystem::path parent = current.parent_path();
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (parent == current)
			{
				break;
			}

			current = parent;
		}

		return {};
	}

	// AppendRepoBridgeCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void AppendRepoBridgeCandidates(const std::filesystem::path& repoRoot, std::vector<std::filesystem::path>& files)
	{
		if (repoRoot.empty())
		{
			return;
		}

		// Prefer repo-local managed outputs during development when they are newer
		// than the active install tree. This keeps Assembly-CSharp compilation in
		// sync with the latest bridge surface even before the editor binary is
		// restarted from a refreshed install tree.
		const std::filesystem::path binRoot = repoRoot / "Engine" / "Managed" / "bin";
		files.push_back(binRoot / "Debug" / "net10.0" / "TerraEngine.CSharp.dll");
		files.push_back(binRoot / "Debug" / "net10.0" / "win-x64" / "TerraEngine.CSharp.dll");
		files.push_back(binRoot / "Release" / "net10.0" / "TerraEngine.CSharp.dll");
		files.push_back(binRoot / "Release" / "net10.0" / "win-x64" / "TerraEngine.CSharp.dll");
	}

	// AppendRepoAnalyzerCandidates is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	void AppendRepoAnalyzerCandidates(const std::filesystem::path& repoRoot, std::vector<std::filesystem::path>& files)
	{
		if (repoRoot.empty())
		{
			return;
		}

		const std::filesystem::path binRoot = repoRoot / "Editor" / "Managed" / "TerraEngine.Export.Generator" / "bin";
		files.push_back(binRoot / "Debug" / "netstandard2.0" / "TerraEngine.Export.Generator.dll");
		files.push_back(binRoot / "Release" / "netstandard2.0" / "TerraEngine.Export.Generator.dll");
	}

	// SelectNewestExistingFile is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::filesystem::path SelectNewestExistingFile(const std::vector<std::filesystem::path>& candidates)
	{
		std::filesystem::path selectedPath;
		std::filesystem::file_time_type selectedWriteTime{};
		bool foundCandidate = false;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (const std::filesystem::path& candidate : candidates)
		{
			std::error_code statusError;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::filesystem::is_regular_file(candidate, statusError))
			{
				continue;
			}

			std::error_code timeError;
			const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(candidate, timeError);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (timeError)
			{
				continue;
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!foundCandidate || writeTime > selectedWriteTime)
			{
				selectedPath = candidate;
				selectedWriteTime = writeTime;
				foundCandidate = true;
			}
		}

		return foundCandidate ? selectedPath : std::filesystem::path{};
	}

#if defined(_WIN32)
	using Microsoft::WRL::ComPtr;

	bool RunProcessCapture(const std::filesystem::path& workingDirectory, const std::wstring& applicationPath, std::wstring& commandLine, std::string& output, DWORD& exitCode)
	{
		// Capture both stdout and stderr through one pipe so callers can surface
		// the exact tool output inside the editor log and status UI.
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
		const BOOL started = CreateProcessW(
			applicationPath.c_str(),
			commandLine.data(),
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
		GetExitCodeProcess(processInfo.hProcess, &exitCode);

		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		CloseHandle(readHandle);
		return true;
	}

	// struct ScopedComInitialization groups related state and operations so callers have one documented contract.
	struct ScopedComInitialization
	{
		HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		~ScopedComInitialization()
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (SUCCEEDED(result))
			{
				CoUninitialize();
			}
		}

		[[nodiscard]] bool IsReady() const
		{
			return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE;
		}
	};

	// NormalizeWindowsPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::wstring NormalizeWindowsPath(const std::filesystem::path& path)
	{
		// Running Object Table display names are case-insensitive Windows paths,
		// so normalize before comparing a loaded solution against our target one.
		std::wstring normalized = std::filesystem::absolute(path).lexically_normal().wstring();
		std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](wchar_t value) {
			return static_cast<wchar_t>(std::towlower(value));
		});
		return normalized;
	}

	// TryGetDispId is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryGetDispId(IDispatch* dispatch, const wchar_t* memberName, DISPID& outDispId)
	{
		if (dispatch == nullptr || memberName == nullptr)
		{
			return false;
		}

		LPOLESTR names[] = { const_cast<LPOLESTR>(memberName) };
		return SUCCEEDED(dispatch->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &outDispId));
	}

	// TryInvokeDispatch is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryInvokeDispatch(IDispatch* dispatch, DISPID dispId, WORD flags, VARIANT* arguments, UINT argumentCount, VARIANT* outResult = nullptr)
	{
		if (dispatch == nullptr)
		{
			return false;
		}

		DISPPARAMS parameters{};
		parameters.rgvarg = arguments;
		parameters.cArgs = argumentCount;
		return SUCCEEDED(dispatch->Invoke(dispId, IID_NULL, LOCALE_USER_DEFAULT, flags, &parameters, outResult, nullptr, nullptr));
	}

	// TryGetDispatchProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryGetDispatchProperty(IDispatch* dispatch, const wchar_t* propertyName, ComPtr<IDispatch>& outDispatch)
	{
		DISPID dispId = DISPID_UNKNOWN;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispId(dispatch, propertyName, dispId))
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryInvokeDispatch(dispatch, dispId, DISPATCH_PROPERTYGET, nullptr, 0, &result))
		{
			VariantClear(&result);
			return false;
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result.vt != VT_DISPATCH || result.pdispVal == nullptr)
		{
			VariantClear(&result);
			return false;
		}

		outDispatch.Attach(result.pdispVal);
		result.vt = VT_EMPTY;
		VariantClear(&result);
		return true;
	}

	// TryGetBstrProperty is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryGetBstrProperty(IDispatch* dispatch, const wchar_t* propertyName, std::wstring& outValue)
	{
		DISPID dispId = DISPID_UNKNOWN;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispId(dispatch, propertyName, dispId))
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryInvokeDispatch(dispatch, dispId, DISPATCH_PROPERTYGET, nullptr, 0, &result))
		{
			VariantClear(&result);
			return false;
		}

		bool success = false;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (result.vt == VT_BSTR)
		{
			outValue = result.bstrVal != nullptr ? result.bstrVal : L"";
			success = true;
		}
		// if is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
		else if (result.vt == VT_EMPTY || result.vt == VT_NULL)
		{
			outValue.clear();
			success = true;
		}

		VariantClear(&result);
		return success;
	}

	// TryCallMethod is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryCallMethod(IDispatch* dispatch, const wchar_t* methodName, VARIANT* arguments, UINT argumentCount, VARIANT* outResult = nullptr)
	{
		DISPID dispId = DISPID_UNKNOWN;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispId(dispatch, methodName, dispId))
		{
			return false;
		}

		return TryInvokeDispatch(dispatch, dispId, DISPATCH_METHOD, arguments, argumentCount, outResult);
	}

	// TryActivateVisualStudioInstance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryActivateVisualStudioInstance(IDispatch* visualStudioDispatch)
	{
		ComPtr<IDispatch> mainWindowDispatch;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispatchProperty(visualStudioDispatch, L"MainWindow", mainWindowDispatch))
		{
			return false;
		}

		return TryCallMethod(mainWindowDispatch.Get(), L"Activate", nullptr, 0);
	}

	// TryGetVisualStudioSolutionPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryGetVisualStudioSolutionPath(IDispatch* visualStudioDispatch, std::filesystem::path& outSolutionPath)
	{
		ComPtr<IDispatch> solutionDispatch;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispatchProperty(visualStudioDispatch, L"Solution", solutionDispatch))
		{
			return false;
		}

		std::wstring solutionFullName;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetBstrProperty(solutionDispatch.Get(), L"FullName", solutionFullName) || solutionFullName.empty())
		{
			return false;
		}

		outSolutionPath = std::filesystem::path(solutionFullName).lexically_normal();
		return true;
	}

	// TryFindRunningVisualStudioForSolution is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryFindRunningVisualStudioForSolution(const std::filesystem::path& solutionPath, ComPtr<IDispatch>& outDispatch)
	{
		ScopedComInitialization comInitialization;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!comInitialization.IsReady())
		{
			return false;
		}

		ComPtr<IRunningObjectTable> runningObjectTable;
		ComPtr<IEnumMoniker> monikerEnumerator;
		ComPtr<IBindCtx> bindContext;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (FAILED(GetRunningObjectTable(0, &runningObjectTable))
			|| FAILED(CreateBindCtx(0, &bindContext))
			|| FAILED(runningObjectTable->EnumRunning(&monikerEnumerator)))
		{
			return false;
		}

		const std::wstring normalizedSolutionPath = NormalizeWindowsPath(solutionPath);
		ComPtr<IMoniker> currentMoniker;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (monikerEnumerator->Next(1, &currentMoniker, nullptr) == S_OK)
		{
			LPOLESTR displayNameBuffer = nullptr;
			std::wstring displayName;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (SUCCEEDED(currentMoniker->GetDisplayName(bindContext.Get(), nullptr, &displayNameBuffer)) && displayNameBuffer != nullptr)
			{
				displayName = displayNameBuffer;
				CoTaskMemFree(displayNameBuffer);
			}

			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (displayName.rfind(kVisualStudioDtePrefix, 0) != 0)
			{
				currentMoniker.Reset();
				continue;
			}

			ComPtr<IUnknown> runningObject;
			ComPtr<IDispatch> visualStudioDispatch;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (FAILED(runningObjectTable->GetObject(currentMoniker.Get(), &runningObject))
				|| FAILED(runningObject.As(&visualStudioDispatch)))
			{
				currentMoniker.Reset();
				continue;
			}

			std::filesystem::path loadedSolutionPath;
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (TryGetVisualStudioSolutionPath(visualStudioDispatch.Get(), loadedSolutionPath)
				&& NormalizeWindowsPath(loadedSolutionPath) == normalizedSolutionPath)
			{
				outDispatch = visualStudioDispatch;
				return true;
			}

			currentMoniker.Reset();
		}

		return false;
	}

	// TryWaitForVisualStudioSolution is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryWaitForVisualStudioSolution(const std::filesystem::path& solutionPath, ComPtr<IDispatch>& outDispatch)
	{
		const ULONGLONG timeoutAt = GetTickCount64() + kVisualStudioAttachTimeoutMs;
		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		while (GetTickCount64() < timeoutAt)
		{
			if (TryFindRunningVisualStudioForSolution(solutionPath, outDispatch))
			{
				return true;
			}

			Sleep(kVisualStudioAttachPollMs);
		}

		return false;
	}

	// TryOpenFileInVisualStudioInstance is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool TryOpenFileInVisualStudioInstance(IDispatch* visualStudioDispatch, const std::filesystem::path& scriptPath)
	{
		if (visualStudioDispatch == nullptr)
		{
			return false;
		}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptPath.empty())
	{
		TryActivateVisualStudioInstance(visualStudioDispatch);
		return true;
	}

		ComPtr<IDispatch> itemOperationsDispatch;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!TryGetDispatchProperty(visualStudioDispatch, L"ItemOperations", itemOperationsDispatch))
		{
			return false;
		}

		VARIANT argument;
		VariantInit(&argument);
		argument.vt = VT_BSTR;
		const std::wstring wideScriptPath = std::filesystem::absolute(scriptPath).lexically_normal().wstring();
		argument.bstrVal = SysAllocString(wideScriptPath.c_str());
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (argument.bstrVal == nullptr)
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		const bool openedFile = TryCallMethod(itemOperationsDispatch.Get(), L"OpenFile", &argument, 1, &result);
		VariantClear(&argument);
		VariantClear(&result);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (!openedFile)
		{
			return false;
		}

		TryActivateVisualStudioInstance(visualStudioDispatch);
		return true;
	}
#endif
}

void ScriptWorkspaceService::SetProjectRoot(const std::filesystem::path& projectRoot)
{
	m_ProjectRoot = projectRoot.empty() ? std::filesystem::path{} : std::filesystem::absolute(projectRoot).lexically_normal();
	m_Status.solutionPath = GetSolutionPath();
	m_Status.projectPath = GetProjectPath();
	m_Status.outputRoot = GetOutputRoot();
	m_Status.referenceRoot = GetReferenceRoot();
	m_Status.health = m_ProjectRoot.empty() ? ScriptWorkspaceHealth::Missing : ScriptWorkspaceHealth::Broken;
	m_Status.lastMessage.clear();
}

// ScriptWorkspaceService::HasProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::HasProjectRoot() const
{
	return !m_ProjectRoot.empty();
}

// ScriptWorkspaceService::GetStatus is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
const ScriptWorkspaceStatus& ScriptWorkspaceService::GetStatus() const
{
	return m_Status;
}

// ScriptWorkspaceService::GetProjectRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetProjectRoot() const
{
	return m_ProjectRoot;
}

// ScriptWorkspaceService::GetSolutionPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetSolutionPath() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / (GetProjectName() + ".sln");
}

// ScriptWorkspaceService::GetProjectPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetProjectPath() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / "Assembly-CSharp.csproj";
}

// ScriptWorkspaceService::GetOutputRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetOutputRoot() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / "Build" / "Assembly-CSharp";
}

// ScriptWorkspaceService::GetReferenceRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetReferenceRoot() const
{
	return GetOutputRoot() / "References";
}

// ScriptWorkspaceService::GetAssemblyPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::GetAssemblyPath() const
{
	return GetOutputRoot() / "Assembly-CSharp.dll";
}

// ScriptWorkspaceService::EnsureWorkspace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::EnsureWorkspace(std::string* outError)
{
	if (!HasProjectRoot())
	{
		const std::string message = "Cannot generate the script workspace before a project is open.";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	// Write the solution and project files first so brand-new projects can be
	// opened in an IDE immediately, even if runtime reference sync fails later.
	if (!WriteWorkspaceFiles(outError))
	{
		return false;
	}

	return SyncManagedReferences(outError);
}

// ScriptWorkspaceService::RegenerateWorkspace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::RegenerateWorkspace(std::string* outError)
{
	return EnsureWorkspace(outError);
}

// ScriptWorkspaceService::CreateScriptFromTemplate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::CreateScriptFromTemplate(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError)
{
	if (!IsValidCSharpIdentifier(className))
	{
		const std::string message = "Script names must be valid C# identifiers that start with a letter or underscore.";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	const std::filesystem::path templatePath = ResolveTemplateRoot() / "DefaultScript.cs.txt";
	std::string templateText = LoadTemplate(templatePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (templateText.empty())
	{
		const std::string message = "The default C# script template could not be found at " + templatePath.string() + ".";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	std::filesystem::path outputDirectory = destinationDirectory.empty() ? (m_ProjectRoot / "Assets") : destinationDirectory;
	outputDirectory = std::filesystem::absolute(outputDirectory).lexically_normal();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(outputDirectory))
	{
		std::filesystem::create_directories(outputDirectory);
	}

	const std::filesystem::path outputPath = outputDirectory / (className + ".cs");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(outputPath))
	{
		const std::string message = "A C# script named " + outputPath.filename().string() + " already exists in this folder.";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	ReplaceAll(templateText, "__SCRIPT_NAME__", className);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!WriteTextIfChanged(outputPath, templateText, outError))
	{
		return false;
	}

	outCreatedPath = outputPath;
	return true;
}

// ScriptWorkspaceService::OpenScriptInExternalEditor is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::OpenScriptInExternalEditor(const std::filesystem::path& scriptPath, const std::string& commandTemplate, std::string* outError)
{
	if (!HasProjectRoot())
	{
		const std::string message = "Open a project before launching an external script editor.";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	// Ensure the root workspace files exist before launching the IDE so
	// project creation and script opening always target a stable solution.
	if (!WriteWorkspaceFiles(outError))
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!commandTemplate.empty())
	{
		return LaunchCommand(commandTemplate, scriptPath, outError);
	}

#if defined(_WIN32)
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (LaunchVisualStudioWorkspace(scriptPath, outError))
	{
		return true;
	}
#endif

	const std::filesystem::path targetPath = GetSolutionPath();
	return LaunchDefaultEditor(targetPath, outError);
}

// ScriptWorkspaceService::WriteWorkspaceFiles is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::WriteWorkspaceFiles(std::string* outError)
{
	const std::filesystem::path templateRoot = ResolveTemplateRoot();
	const std::string solutionTemplate = LoadTemplate(templateRoot / "Solution.sln.in");
	const std::string projectTemplate = LoadTemplate(templateRoot / "Assembly-CSharp.csproj.in");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (solutionTemplate.empty() || projectTemplate.empty())
	{
		const std::string message = "One or more scripting workspace templates are missing from " + templateRoot.string() + ".";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(GetOutputRoot()))
	{
		std::filesystem::create_directories(GetOutputRoot());
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(GetReferenceRoot()))
	{
		std::filesystem::create_directories(GetReferenceRoot());
	}

	std::string expandedSolution = solutionTemplate;
	ReplaceAll(expandedSolution, "@PROJECT_NAME@", GetProjectName());
	ReplaceAll(expandedSolution, "@PROJECT_GUID@", std::string(kSolutionProjectGuid));

	std::string expandedProject = projectTemplate;
	ReplaceAll(expandedProject, "@REFERENCE_DLL@", "Build/Assembly-CSharp/References/TerraEngine.CSharp.dll");

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!WriteTextIfChanged(GetSolutionPath(), expandedSolution, outError))
	{
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!WriteTextIfChanged(GetProjectPath(), expandedProject, outError))
	{
		return false;
	}

	m_Status.health = ScriptWorkspaceHealth::Healthy;
	m_Status.lastMessage = "The Assembly-CSharp workspace is synchronized with the current project.";
	m_Status.solutionPath = GetSolutionPath();
	m_Status.projectPath = GetProjectPath();
	m_Status.outputRoot = GetOutputRoot();
	m_Status.referenceRoot = GetReferenceRoot();
	return true;
}

// ScriptWorkspaceService::SyncManagedReferences is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::SyncManagedReferences(std::string* outError)
{
	const std::filesystem::path sourceDll = ResolveManagedBridgePath();
	const std::filesystem::path targetDll = GetReferenceRoot() / "TerraEngine.CSharp.dll";

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(sourceDll))
	{
		const std::string message = "The installed TerraEngine.CSharp.dll runtime bridge was not found at " + sourceDll.string() + ".";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(GetReferenceRoot()))
	{
		std::filesystem::create_directories(GetReferenceRoot());
	}

	std::error_code copyError;
	std::filesystem::copy_file(sourceDll, targetDll, std::filesystem::copy_options::overwrite_existing, copyError);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (copyError)
	{
		const std::string message = "Failed to synchronize TerraEngine.CSharp.dll into the project workspace: " + copyError.message();
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	const std::filesystem::path sourcePdb = sourceDll.parent_path() / "TerraEngine.CSharp.pdb";
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (std::filesystem::exists(sourcePdb))
	{
		std::filesystem::copy_file(sourcePdb, GetReferenceRoot() / "TerraEngine.CSharp.pdb", std::filesystem::copy_options::overwrite_existing, copyError);
	}

	// Copy the source generator (analyzer) into the project.
	const std::filesystem::path analyzerSource = ResolveManagedAnalyzerPath();
	const std::filesystem::path analyzerDest = GetReferenceRoot() / "TerraEngine.Export.Generator.dll";
	// Exported gameplay builds depend on this analyzer to generate the script
	// registry that the native export runtime calls into. If the editor ships
	// without it, projects may still compile but exported scripts will fail to
	// instantiate because the managed registry is stale or missing.
	if (!std::filesystem::exists(analyzerSource))
	{
		const std::string message = "The export script analyzer is missing from the editor install at " + analyzerSource.string() + ". Reinstall TerraEngine so project exports regenerate the managed script registry.";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	std::filesystem::copy_file(analyzerSource, analyzerDest, std::filesystem::copy_options::overwrite_existing, copyError);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (copyError)
	{
		const std::string message = "Failed to synchronize TerraEngine.Export.Generator.dll into the project workspace: " + copyError.message();
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	return true;
}

// ScriptWorkspaceService::WriteTextIfChanged is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::WriteTextIfChanged(const std::filesystem::path& path, const std::string& contents, std::string* outError) const
{
	if (LoadTextFile(path) == contents)
	{
		return true;
	}

	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!output)
	{
		const std::string message = "Failed to write " + path.string() + ".";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
	output.close();
	return true;
}

// ScriptWorkspaceService::ResolveTemplateRoot is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::ResolveTemplateRoot() const
{
	std::filesystem::path editorDir;
#ifdef _WIN32
	wchar_t path[MAX_PATH];
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (GetModuleFileNameW(NULL, path, MAX_PATH) != 0) {
		editorDir = std::filesystem::path(path).parent_path();
	} else {
		editorDir = std::filesystem::current_path();
	}
#else
	editorDir = std::filesystem::current_path();
#endif

	const std::filesystem::path candidates[] = {
		editorDir / "Project" / "Templates",
		editorDir / "Editor" / "TerraEngine-Editor" / "Project" / "Templates",
		editorDir / "Templates",
		editorDir.parent_path() / "Project" / "Templates"
	};

	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate / "Assembly-CSharp.csproj.in"))
		{
			return candidate;
		}
	}

	// Fall back to the in-repo editor template folder so errors mention the path
	// TerraEngine most expects to exist during local development.
	return candidates[1];
}

// ScriptWorkspaceService::ResolveManagedBridgePath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::ResolveManagedBridgePath() const
{
	std::vector<std::filesystem::path> candidates;
	AppendManagedRootCandidates(std::filesystem::current_path(), candidates);
	AppendManagedRootCandidates(ResolveEditorExecutableDirectory(), candidates);

	std::vector<std::filesystem::path> bridgeCandidates;
	bridgeCandidates.reserve(candidates.size() + 2);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::path& root : candidates)
	{
		bridgeCandidates.push_back(root / "TerraEngine.CSharp.dll");
	}

	std::filesystem::path repoRoot = FindRepoRoot(std::filesystem::current_path());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (repoRoot.empty())
	{
		repoRoot = FindRepoRoot(ResolveEditorExecutableDirectory());
	}

	// When TerraEngine runs from a source checkout, repo-local managed builds can
	// be newer than the install tree that launched the current editor process.
	// Choosing the newest valid file keeps the project compile reference aligned
	// with the latest bridge API surface.
	AppendRepoBridgeCandidates(repoRoot, bridgeCandidates);
	return SelectNewestExistingFile(bridgeCandidates);
}

// ScriptWorkspaceService::ResolveManagedAnalyzerPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::ResolveManagedAnalyzerPath() const
{
	std::vector<std::filesystem::path> roots;
	AppendManagedRootCandidates(std::filesystem::current_path(), roots);
	AppendManagedRootCandidates(ResolveEditorExecutableDirectory(), roots);

	std::vector<std::filesystem::path> analyzerCandidates;
	analyzerCandidates.reserve(roots.size() + 2);
	// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
	for (const std::filesystem::path& root : roots)
	{
		analyzerCandidates.push_back(root / "Analyzers" / "TerraEngine.Export.Generator.dll");
	}

	std::filesystem::path repoRoot = FindRepoRoot(std::filesystem::current_path());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (repoRoot.empty())
	{
		repoRoot = FindRepoRoot(ResolveEditorExecutableDirectory());
	}

	// Keep the source generator in lockstep with the bridge reference so editor
	// builds do not mix a fresh generator with a stale TerraEngine.CSharp.dll.
	AppendRepoAnalyzerCandidates(repoRoot, analyzerCandidates);
	return SelectNewestExistingFile(analyzerCandidates);
}

#if defined(_WIN32)
// ScriptWorkspaceService::ResolveVisualStudioPath is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::filesystem::path ScriptWorkspaceService::ResolveVisualStudioPath() const
{
	const std::filesystem::path vsWherePath(kVsWherePath);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(vsWherePath))
	{
		return {};
	}

	std::wstring commandLine = L"\"" + vsWherePath.wstring() + L"\" -latest -products * -requires Microsoft.Component.MSBuild -property productPath";
	std::string output;
	DWORD exitCode = 0;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!RunProcessCapture(vsWherePath.parent_path(), vsWherePath.wstring(), commandLine, output, exitCode) || exitCode != 0)
	{
		return {};
	}

	const std::string trimmedOutput = TrimWhitespace(std::move(output));
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (trimmedOutput.empty())
	{
		return {};
	}

	const std::filesystem::path visualStudioPath = std::filesystem::path(trimmedOutput).lexically_normal();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!std::filesystem::exists(visualStudioPath))
	{
		return {};
	}

	return visualStudioPath;
}

// ScriptWorkspaceService::LaunchVisualStudioWorkspace is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::LaunchVisualStudioWorkspace(const std::filesystem::path& scriptPath, std::string* outError) const
{
	const std::filesystem::path visualStudioPath = ResolveVisualStudioPath();
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (visualStudioPath.empty())
	{
		if (outError)
		{
			*outError = "Visual Studio was not found through vswhere.exe.";
		}
		return false;
	}

	const std::filesystem::path solutionPath = GetSolutionPath();
	ComPtr<IDispatch> runningVisualStudioDispatch;
	// Reuse an existing solution window when possible so double-clicking a C#
	// file keeps work inside the already open project session.
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (TryFindRunningVisualStudioForSolution(solutionPath, runningVisualStudioDispatch))
	{
		if (!TryOpenFileInVisualStudioInstance(runningVisualStudioDispatch.Get(), scriptPath))
		{
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (outError)
			{
				*outError = "Visual Studio is already running for this solution, but the requested C# file could not be opened in that window.";
			}
			return false;
		}

		return true;
	}

	// Launch the solution first, then use automation to open the requested file
	// once Visual Studio registers the new instance in the Running Object Table.
	const std::wstring wideVisualStudioPath = visualStudioPath.wstring();
	std::wstring commandLine = L"\"" + wideVisualStudioPath + L"\" \"" + solutionPath.wstring() + L"\"";

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInfo{};
	std::wstring mutableCommandLine = commandLine;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateProcessW(
		wideVisualStudioPath.c_str(),
		mutableCommandLine.data(),
		nullptr,
		nullptr,
		FALSE,
		0,
		nullptr,
		m_ProjectRoot.wstring().c_str(),
		&startupInfo,
		&processInfo))
	{
		const DWORD errorCode = GetLastError();
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = "Failed to launch Visual Studio for the Assembly-CSharp workspace. Windows error code: " + std::to_string(errorCode) + ".";
		}
		return false;
	}

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (scriptPath.empty())
	{
		return true;
	}

	ComPtr<IDispatch> launchedVisualStudioDispatch;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TryWaitForVisualStudioSolution(solutionPath, launchedVisualStudioDispatch))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Visual Studio launched for " << solutionPath.string() << " but did not register for automation before the timeout expired.");
		return true;
	}

	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!TryOpenFileInVisualStudioInstance(launchedVisualStudioDispatch.Get(), scriptPath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Visual Studio launched for " << solutionPath.string() << " but could not open the requested C# file through automation.");
	}

	return true;
}
#endif

// ScriptWorkspaceService::GetProjectName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ScriptWorkspaceService::GetProjectName() const
{
	return m_ProjectRoot.filename().string();
}

// ScriptWorkspaceService::LoadTemplate is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
std::string ScriptWorkspaceService::LoadTemplate(const std::filesystem::path& path) const
{
	return LoadTextFile(path);
}

// ScriptWorkspaceService::LaunchCommand is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
bool ScriptWorkspaceService::LaunchCommand(const std::string& commandTemplate, const std::filesystem::path& scriptPath, std::string* outError) const
{
#if defined(_WIN32)
	std::string expandedCommand = commandTemplate;
	ReplaceAll(expandedCommand, "{solution}", "\"" + GetSolutionPath().string() + "\"");
	ReplaceAll(expandedCommand, "{project}", "\"" + GetProjectPath().string() + "\"");
	ReplaceAll(expandedCommand, "{script}", "\"" + scriptPath.string() + "\"");

	STARTUPINFOW startupInfo{};
	startupInfo.cb = sizeof(startupInfo);
	PROCESS_INFORMATION processInfo{};
	std::wstring wideCommand(expandedCommand.begin(), expandedCommand.end());
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (!CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, m_ProjectRoot.wstring().c_str(), &startupInfo, &processInfo))
	{
		const DWORD errorCode = GetLastError();
		const std::string message = "Failed to launch the configured external editor command. Windows error code: " + std::to_string(errorCode) + ".";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);
	return true;
#else
	(void)commandTemplate;
	(void)scriptPath;
	if (outError)
	{
		*outError = "Custom external editor commands are currently implemented for Windows only.";
	}
	return false;
#endif
}

bool ScriptWorkspaceService::LaunchDefaultEditor(const std::filesystem::path& targetPath, std::string* outError) const
{
#if defined(_WIN32)
	const HINSTANCE result = ShellExecuteW(nullptr, L"open", targetPath.wstring().c_str(), nullptr, m_ProjectRoot.wstring().c_str(), SW_SHOWNORMAL);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (reinterpret_cast<std::intptr_t>(result) <= 32)
	{
		const std::string message = "Windows could not open " + targetPath.string() + " with the default associated editor.";
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	return true;
#else
	(void)targetPath;
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (outError)
	{
		*outError = "Launching the default external editor is currently implemented for Windows only.";
	}
	return false;
#endif
}
