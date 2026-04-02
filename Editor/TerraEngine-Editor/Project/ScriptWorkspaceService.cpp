#include "ScriptWorkspaceService.h"

#include "Engine/Core/Logger/Logger.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <sstream>

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

	void ReplaceAll(std::string& text, std::string_view needle, std::string_view replacement)
	{
		std::size_t searchIndex = 0;
		while ((searchIndex = text.find(needle.data(), searchIndex, needle.size())) != std::string::npos)
		{
			text.replace(searchIndex, needle.size(), replacement.data(), replacement.size());
			searchIndex += replacement.size();
		}
	}

	std::string LoadTextFile(const std::filesystem::path& path)
	{
		std::ifstream input(path, std::ios::binary);
		if (!input)
		{
			return {};
		}

		std::stringstream buffer;
		buffer << input.rdbuf();
		return buffer.str();
	}

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

		if (!isIdentifierStart(static_cast<unsigned char>(className.front())))
		{
			return false;
		}

		return std::all_of(className.begin() + 1, className.end(), [&](char value) {
			return isIdentifierCharacter(static_cast<unsigned char>(value));
		});
	}

	std::string TrimWhitespace(std::string text)
	{
		const auto firstNonWhitespace = std::find_if_not(text.begin(), text.end(), [](unsigned char value) {
			return std::isspace(value) != 0;
		});
		const auto lastNonWhitespace = std::find_if_not(text.rbegin(), text.rend(), [](unsigned char value) {
			return std::isspace(value) != 0;
		}).base();
		if (firstNonWhitespace >= lastNonWhitespace)
		{
			return {};
		}

		return std::string(firstNonWhitespace, lastNonWhitespace);
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

	struct ScopedComInitialization
	{
		HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

		~ScopedComInitialization()
		{
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

	bool TryGetDispId(IDispatch* dispatch, const wchar_t* memberName, DISPID& outDispId)
	{
		if (dispatch == nullptr || memberName == nullptr)
		{
			return false;
		}

		LPOLESTR names[] = { const_cast<LPOLESTR>(memberName) };
		return SUCCEEDED(dispatch->GetIDsOfNames(IID_NULL, names, 1, LOCALE_USER_DEFAULT, &outDispId));
	}

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

	bool TryGetDispatchProperty(IDispatch* dispatch, const wchar_t* propertyName, ComPtr<IDispatch>& outDispatch)
	{
		DISPID dispId = DISPID_UNKNOWN;
		if (!TryGetDispId(dispatch, propertyName, dispId))
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		if (!TryInvokeDispatch(dispatch, dispId, DISPATCH_PROPERTYGET, nullptr, 0, &result))
		{
			VariantClear(&result);
			return false;
		}

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

	bool TryGetBstrProperty(IDispatch* dispatch, const wchar_t* propertyName, std::wstring& outValue)
	{
		DISPID dispId = DISPID_UNKNOWN;
		if (!TryGetDispId(dispatch, propertyName, dispId))
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		if (!TryInvokeDispatch(dispatch, dispId, DISPATCH_PROPERTYGET, nullptr, 0, &result))
		{
			VariantClear(&result);
			return false;
		}

		bool success = false;
		if (result.vt == VT_BSTR)
		{
			outValue = result.bstrVal != nullptr ? result.bstrVal : L"";
			success = true;
		}
		else if (result.vt == VT_EMPTY || result.vt == VT_NULL)
		{
			outValue.clear();
			success = true;
		}

		VariantClear(&result);
		return success;
	}

	bool TryCallMethod(IDispatch* dispatch, const wchar_t* methodName, VARIANT* arguments, UINT argumentCount, VARIANT* outResult = nullptr)
	{
		DISPID dispId = DISPID_UNKNOWN;
		if (!TryGetDispId(dispatch, methodName, dispId))
		{
			return false;
		}

		return TryInvokeDispatch(dispatch, dispId, DISPATCH_METHOD, arguments, argumentCount, outResult);
	}

	bool TryActivateVisualStudioInstance(IDispatch* visualStudioDispatch)
	{
		ComPtr<IDispatch> mainWindowDispatch;
		if (!TryGetDispatchProperty(visualStudioDispatch, L"MainWindow", mainWindowDispatch))
		{
			return false;
		}

		return TryCallMethod(mainWindowDispatch.Get(), L"Activate", nullptr, 0);
	}

	bool TryGetVisualStudioSolutionPath(IDispatch* visualStudioDispatch, std::filesystem::path& outSolutionPath)
	{
		ComPtr<IDispatch> solutionDispatch;
		if (!TryGetDispatchProperty(visualStudioDispatch, L"Solution", solutionDispatch))
		{
			return false;
		}

		std::wstring solutionFullName;
		if (!TryGetBstrProperty(solutionDispatch.Get(), L"FullName", solutionFullName) || solutionFullName.empty())
		{
			return false;
		}

		outSolutionPath = std::filesystem::path(solutionFullName).lexically_normal();
		return true;
	}

	bool TryFindRunningVisualStudioForSolution(const std::filesystem::path& solutionPath, ComPtr<IDispatch>& outDispatch)
	{
		ScopedComInitialization comInitialization;
		if (!comInitialization.IsReady())
		{
			return false;
		}

		ComPtr<IRunningObjectTable> runningObjectTable;
		ComPtr<IEnumMoniker> monikerEnumerator;
		ComPtr<IBindCtx> bindContext;
		if (FAILED(GetRunningObjectTable(0, &runningObjectTable))
			|| FAILED(CreateBindCtx(0, &bindContext))
			|| FAILED(runningObjectTable->EnumRunning(&monikerEnumerator)))
		{
			return false;
		}

		const std::wstring normalizedSolutionPath = NormalizeWindowsPath(solutionPath);
		ComPtr<IMoniker> currentMoniker;
		while (monikerEnumerator->Next(1, &currentMoniker, nullptr) == S_OK)
		{
			LPOLESTR displayNameBuffer = nullptr;
			std::wstring displayName;
			if (SUCCEEDED(currentMoniker->GetDisplayName(bindContext.Get(), nullptr, &displayNameBuffer)) && displayNameBuffer != nullptr)
			{
				displayName = displayNameBuffer;
				CoTaskMemFree(displayNameBuffer);
			}

			if (displayName.rfind(kVisualStudioDtePrefix, 0) != 0)
			{
				currentMoniker.Reset();
				continue;
			}

			ComPtr<IUnknown> runningObject;
			ComPtr<IDispatch> visualStudioDispatch;
			if (FAILED(runningObjectTable->GetObject(currentMoniker.Get(), &runningObject))
				|| FAILED(runningObject.As(&visualStudioDispatch)))
			{
				currentMoniker.Reset();
				continue;
			}

			std::filesystem::path loadedSolutionPath;
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

	bool TryWaitForVisualStudioSolution(const std::filesystem::path& solutionPath, ComPtr<IDispatch>& outDispatch)
	{
		const ULONGLONG timeoutAt = GetTickCount64() + kVisualStudioAttachTimeoutMs;
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

	bool TryOpenFileInVisualStudioInstance(IDispatch* visualStudioDispatch, const std::filesystem::path& scriptPath)
	{
		if (visualStudioDispatch == nullptr)
		{
			return false;
		}

	if (scriptPath.empty())
	{
		TryActivateVisualStudioInstance(visualStudioDispatch);
		return true;
	}

		ComPtr<IDispatch> itemOperationsDispatch;
		if (!TryGetDispatchProperty(visualStudioDispatch, L"ItemOperations", itemOperationsDispatch))
		{
			return false;
		}

		VARIANT argument;
		VariantInit(&argument);
		argument.vt = VT_BSTR;
		const std::wstring wideScriptPath = std::filesystem::absolute(scriptPath).lexically_normal().wstring();
		argument.bstrVal = SysAllocString(wideScriptPath.c_str());
		if (argument.bstrVal == nullptr)
		{
			return false;
		}

		VARIANT result;
		VariantInit(&result);
		const bool openedFile = TryCallMethod(itemOperationsDispatch.Get(), L"OpenFile", &argument, 1, &result);
		VariantClear(&argument);
		VariantClear(&result);
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

bool ScriptWorkspaceService::HasProjectRoot() const
{
	return !m_ProjectRoot.empty();
}

const ScriptWorkspaceStatus& ScriptWorkspaceService::GetStatus() const
{
	return m_Status;
}

std::filesystem::path ScriptWorkspaceService::GetProjectRoot() const
{
	return m_ProjectRoot;
}

std::filesystem::path ScriptWorkspaceService::GetSolutionPath() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / (GetProjectName() + ".sln");
}

std::filesystem::path ScriptWorkspaceService::GetProjectPath() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / "Assembly-CSharp.csproj";
}

std::filesystem::path ScriptWorkspaceService::GetOutputRoot() const
{
	if (m_ProjectRoot.empty())
	{
		return {};
	}

	return m_ProjectRoot / "Build" / "Assembly-CSharp";
}

std::filesystem::path ScriptWorkspaceService::GetReferenceRoot() const
{
	return GetOutputRoot() / "References";
}

std::filesystem::path ScriptWorkspaceService::GetAssemblyPath() const
{
	return GetOutputRoot() / "Assembly-CSharp.dll";
}

bool ScriptWorkspaceService::EnsureWorkspace(std::string* outError)
{
	if (!HasProjectRoot())
	{
		const std::string message = "Cannot generate the script workspace before a project is open.";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
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

bool ScriptWorkspaceService::RegenerateWorkspace(std::string* outError)
{
	return EnsureWorkspace(outError);
}

bool ScriptWorkspaceService::CreateScriptFromTemplate(const std::filesystem::path& destinationDirectory, const std::string& className, std::filesystem::path& outCreatedPath, std::string* outError)
{
	if (!IsValidCSharpIdentifier(className))
	{
		const std::string message = "Script names must be valid C# identifiers that start with a letter or underscore.";
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	const std::filesystem::path templatePath = ResolveTemplateRoot() / "DefaultScript.cs.txt";
	std::string templateText = LoadTemplate(templatePath);
	if (templateText.empty())
	{
		const std::string message = "The default C# script template could not be found at " + templatePath.string() + ".";
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	std::filesystem::path outputDirectory = destinationDirectory.empty() ? (m_ProjectRoot / "Assets") : destinationDirectory;
	outputDirectory = std::filesystem::absolute(outputDirectory).lexically_normal();
	if (!std::filesystem::exists(outputDirectory))
	{
		std::filesystem::create_directories(outputDirectory);
	}

	const std::filesystem::path outputPath = outputDirectory / (className + ".cs");
	if (std::filesystem::exists(outputPath))
	{
		const std::string message = "A C# script named " + outputPath.filename().string() + " already exists in this folder.";
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	ReplaceAll(templateText, "__SCRIPT_NAME__", className);
	if (!WriteTextIfChanged(outputPath, templateText, outError))
	{
		return false;
	}

	outCreatedPath = outputPath;
	return true;
}

bool ScriptWorkspaceService::OpenScriptInExternalEditor(const std::filesystem::path& scriptPath, const std::string& commandTemplate, std::string* outError)
{
	if (!HasProjectRoot())
	{
		const std::string message = "Open a project before launching an external script editor.";
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

	if (!commandTemplate.empty())
	{
		return LaunchCommand(commandTemplate, scriptPath, outError);
	}

#if defined(_WIN32)
	if (LaunchVisualStudioWorkspace(scriptPath, outError))
	{
		return true;
	}
#endif

	const std::filesystem::path targetPath = GetSolutionPath();
	return LaunchDefaultEditor(targetPath, outError);
}

bool ScriptWorkspaceService::WriteWorkspaceFiles(std::string* outError)
{
	const std::filesystem::path templateRoot = ResolveTemplateRoot();
	const std::string solutionTemplate = LoadTemplate(templateRoot / "Solution.sln.in");
	const std::string projectTemplate = LoadTemplate(templateRoot / "Assembly-CSharp.csproj.in");

	if (solutionTemplate.empty() || projectTemplate.empty())
	{
		const std::string message = "One or more scripting workspace templates are missing from " + templateRoot.string() + ".";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	if (!std::filesystem::exists(GetOutputRoot()))
	{
		std::filesystem::create_directories(GetOutputRoot());
	}

	if (!std::filesystem::exists(GetReferenceRoot()))
	{
		std::filesystem::create_directories(GetReferenceRoot());
	}

	std::string expandedSolution = solutionTemplate;
	ReplaceAll(expandedSolution, "@PROJECT_NAME@", GetProjectName());
	ReplaceAll(expandedSolution, "@PROJECT_GUID@", std::string(kSolutionProjectGuid));

	std::string expandedProject = projectTemplate;
	ReplaceAll(expandedProject, "@REFERENCE_DLL@", "Build/Assembly-CSharp/References/TerraEngine.CSharp.dll");

	if (!WriteTextIfChanged(GetSolutionPath(), expandedSolution, outError))
	{
		return false;
	}

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

bool ScriptWorkspaceService::SyncManagedReferences(std::string* outError)
{
	const std::filesystem::path managedRoot = ResolveManagedRoot();
	const std::filesystem::path sourceDll = managedRoot / "TerraEngine.CSharp.dll";
	const std::filesystem::path targetDll = GetReferenceRoot() / "TerraEngine.CSharp.dll";

	if (!std::filesystem::exists(sourceDll))
	{
		const std::string message = "The installed TerraEngine.CSharp.dll runtime bridge was not found at " + sourceDll.string() + ".";
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	if (!std::filesystem::exists(GetReferenceRoot()))
	{
		std::filesystem::create_directories(GetReferenceRoot());
	}

	std::error_code copyError;
	std::filesystem::copy_file(sourceDll, targetDll, std::filesystem::copy_options::overwrite_existing, copyError);
	if (copyError)
	{
		const std::string message = "Failed to synchronize TerraEngine.CSharp.dll into the project workspace: " + copyError.message();
		m_Status.health = ScriptWorkspaceHealth::Broken;
		m_Status.lastMessage = message;
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	const std::filesystem::path sourcePdb = managedRoot / "TerraEngine.CSharp.pdb";
	if (std::filesystem::exists(sourcePdb))
	{
		std::filesystem::copy_file(sourcePdb, GetReferenceRoot() / "TerraEngine.CSharp.pdb", std::filesystem::copy_options::overwrite_existing, copyError);
	}

	return true;
}

bool ScriptWorkspaceService::WriteTextIfChanged(const std::filesystem::path& path, const std::string& contents, std::string* outError) const
{
	if (LoadTextFile(path) == contents)
	{
		return true;
	}

	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
	{
		const std::string message = "Failed to write " + path.string() + ".";
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

std::filesystem::path ScriptWorkspaceService::ResolveTemplateRoot() const
{
	const std::filesystem::path current = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		current / "Project" / "Templates",
		current / "Editor" / "TerraEngine-Editor" / "Project" / "Templates",
		current / "Templates",
		current.parent_path() / "Project" / "Templates"
	};

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

std::filesystem::path ScriptWorkspaceService::ResolveManagedRoot() const
{
	const std::filesystem::path current = std::filesystem::current_path();
	const std::filesystem::path candidates[] = {
		current / "Managed",
		current / "Editor" / "Managed",
		current.parent_path() / "Editor" / "Managed"
	};

	for (const auto& candidate : candidates)
	{
		if (std::filesystem::exists(candidate / "TerraEngine.CSharp.dll"))
		{
			return candidate;
		}
	}

	return candidates[0];
}

#if defined(_WIN32)
std::filesystem::path ScriptWorkspaceService::ResolveVisualStudioPath() const
{
	const std::filesystem::path vsWherePath(kVsWherePath);
	if (!std::filesystem::exists(vsWherePath))
	{
		return {};
	}

	std::wstring commandLine = L"\"" + vsWherePath.wstring() + L"\" -latest -products * -requires Microsoft.Component.MSBuild -property productPath";
	std::string output;
	DWORD exitCode = 0;
	if (!RunProcessCapture(vsWherePath.parent_path(), vsWherePath.wstring(), commandLine, output, exitCode) || exitCode != 0)
	{
		return {};
	}

	const std::string trimmedOutput = TrimWhitespace(std::move(output));
	if (trimmedOutput.empty())
	{
		return {};
	}

	const std::filesystem::path visualStudioPath = std::filesystem::path(trimmedOutput).lexically_normal();
	if (!std::filesystem::exists(visualStudioPath))
	{
		return {};
	}

	return visualStudioPath;
}

bool ScriptWorkspaceService::LaunchVisualStudioWorkspace(const std::filesystem::path& scriptPath, std::string* outError) const
{
	const std::filesystem::path visualStudioPath = ResolveVisualStudioPath();
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
	if (TryFindRunningVisualStudioForSolution(solutionPath, runningVisualStudioDispatch))
	{
		if (!TryOpenFileInVisualStudioInstance(runningVisualStudioDispatch.Get(), scriptPath))
		{
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
		if (outError)
		{
			*outError = "Failed to launch Visual Studio for the Assembly-CSharp workspace. Windows error code: " + std::to_string(errorCode) + ".";
		}
		return false;
	}

	CloseHandle(processInfo.hThread);
	CloseHandle(processInfo.hProcess);

	if (scriptPath.empty())
	{
		return true;
	}

	ComPtr<IDispatch> launchedVisualStudioDispatch;
	if (!TryWaitForVisualStudioSolution(solutionPath, launchedVisualStudioDispatch))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Visual Studio launched for " << solutionPath.string() << " but did not register for automation before the timeout expired.");
		return true;
	}

	if (!TryOpenFileInVisualStudioInstance(launchedVisualStudioDispatch.Get(), scriptPath))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Visual Studio launched for " << solutionPath.string() << " but could not open the requested C# file through automation.");
	}

	return true;
}
#endif

std::string ScriptWorkspaceService::GetProjectName() const
{
	return m_ProjectRoot.filename().string();
}

std::string ScriptWorkspaceService::LoadTemplate(const std::filesystem::path& path) const
{
	return LoadTextFile(path);
}

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
	if (!CreateProcessW(nullptr, wideCommand.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, m_ProjectRoot.wstring().c_str(), &startupInfo, &processInfo))
	{
		const DWORD errorCode = GetLastError();
		const std::string message = "Failed to launch the configured external editor command. Windows error code: " + std::to_string(errorCode) + ".";
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
	if (reinterpret_cast<std::intptr_t>(result) <= 32)
	{
		const std::string message = "Windows could not open " + targetPath.string() + " with the default associated editor.";
		if (outError)
		{
			*outError = message;
		}
		return false;
	}

	return true;
#else
	(void)targetPath;
	if (outError)
	{
		*outError = "Launching the default external editor is currently implemented for Windows only.";
	}
	return false;
#endif
}
