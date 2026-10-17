// PlatformUtils.cpp — Implementation of platform-specific utility functions.
//
// This file contains the implementation of OS-specific logic for Windows and
// Linux, including shell execution for opening URLs and path resolution for
// persistent storage. It provides a unified interface for platform-dependent
// operations used throughout the engine.

#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "Core/Logger/Logger.h"

#ifdef _WIN32
#	ifndef WIN32_LEAN_AND_MEAN
#		define WIN32_LEAN_AND_MEAN
#	endif
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Objbase.h>
#	include <ShlObj.h>
#	include <shellapi.h>
#	include <Windows.h>
#	include <DbgHelp.h>
#elif defined(__APPLE__)
#	include <mach-o/dyld.h>
#	include <unistd.h>
#else
#	include <spawn.h>
#	include <unistd.h>

extern char** environ;
#endif

namespace
{
	constexpr std::string_view kDefaultProductName = "TerraEngine";
	constexpr std::string_view kDefaultProductVersionFolder = "SaveData";
	constexpr std::string_view kDiagnosticsFolderName = "Diagnostics";
	constexpr std::string_view kCrashDumpsFolderName = "CrashDumps";


	std::string SanitizeProductName(std::string_view productName)
	{
		std::string sanitized = productName.empty()
			? std::string(kDefaultProductName)
			: std::string(productName);


		for (char& character : sanitized)
		{
			if (character == '/' || character == '\\' || character == ':' || character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
			{
				character = '_';
			}
		}


		if (sanitized.empty())
		{
			sanitized = kDefaultProductName;
		}

		return sanitized;
	}


	bool HasUrlScheme(std::string_view url)
	{
		const std::size_t separatorIndex = url.find(':');

		if (separatorIndex == std::string_view::npos || separatorIndex == 0)
		{
			return false;
		}


		for (std::size_t index = 0; index < separatorIndex; ++index)
		{
			const unsigned char value = static_cast<unsigned char>(url[index]);

			if (!std::isalnum(value) && value != '+' && value != '-' && value != '.')
			{
				return false;
			}
		}

		return true;
	}

	std::string MakeCrashTimestamp()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
		std::tm utcTime{};
#ifdef _WIN32
		gmtime_s(&utcTime, &nowTime);
#else
		gmtime_r(&nowTime, &utcTime);
#endif
		std::ostringstream stream;
		stream << std::put_time(&utcTime, "%Y%m%d-%H%M%S");
		return stream.str();
	}

	std::filesystem::path ResolveLocalAppDataRoot(std::string_view sanitizedProductName)
	{
		std::filesystem::path basePath;
#ifdef _WIN32
		PWSTR localAppDataPath = nullptr;
		const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppDataPath);
		if (FAILED(result) || localAppDataPath == nullptr)
		{
			TERRAENGINE_LOG(LogLevel::Error, "Could not resolve FOLDERID_LocalAppData for product '" << sanitizedProductName << "'.");
			return {};
		}

		basePath = std::filesystem::path(localAppDataPath);
		CoTaskMemFree(localAppDataPath);
#else
		const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
		if (xdgDataHome != nullptr && xdgDataHome[0] != '\0')
		{
			basePath = std::filesystem::path(xdgDataHome);
		}
		else
		{
			const char* homeDirectory = std::getenv("HOME");
			if (homeDirectory == nullptr || homeDirectory[0] == '\0')
			{
				TERRAENGINE_LOG(LogLevel::Error, "Could not resolve HOME or XDG_DATA_HOME for product '" << sanitizedProductName << "'.");
				return {};
			}

			basePath = std::filesystem::path(homeDirectory) / ".local" / "share";
		}
#endif
		return (basePath / std::string(sanitizedProductName)).lexically_normal();
	}

	std::filesystem::path CreateDirectoryOrFallback(std::filesystem::path requestedPath, std::string_view fallbackLeafName)
	{
		std::error_code errorCode;
		std::filesystem::create_directories(requestedPath, errorCode);
		if (!errorCode)
		{
			return requestedPath;
		}

		TERRAENGINE_LOG(LogLevel::Error, "Could not create diagnostics path '" << requestedPath << "': " << errorCode.message());
		std::filesystem::path fallbackPath = std::filesystem::temp_directory_path(errorCode) / "TerraEngine" / fallbackLeafName;
		if (errorCode)
		{
			return {};
		}
		std::filesystem::create_directories(fallbackPath, errorCode);
		return errorCode ? std::filesystem::path{} : fallbackPath.lexically_normal();
	}

#ifdef _WIN32

	std::wstring Utf8ToWide(std::string_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);

		if (wideLength <= 0)
		{
			return {};
		}

		std::wstring wideText(static_cast<std::size_t>(wideLength), L'\0');
		const int convertedLength = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wideText.data(), wideLength);

		if (convertedLength <= 0)
		{
			return {};
		}

		return wideText;
	}
#endif

	bool WriteNativeCrashDump(const std::filesystem::path& dumpPath, std::string* errorMessage)
	{
#ifdef _WIN32
		const std::wstring wideDumpPath = dumpPath.wstring();
		HANDLE file = CreateFileW(
			wideDumpPath.c_str(),
			GENERIC_WRITE,
			0,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			if (errorMessage != nullptr)
			{
				*errorMessage = "CreateFileW failed for crash dump.";
			}
			return false;
		}

		// MiniDumpWriteDump is intentionally called without exception pointers
		// for controlled fatal paths. The dump still captures process/module and
		// thread state, which is enough for renderer recovery failures that are
		// not raised from an SEH exception handler.
		const BOOL written = MiniDumpWriteDump(
			GetCurrentProcess(),
			GetCurrentProcessId(),
			file,
			MiniDumpNormal,
			nullptr,
			nullptr,
			nullptr);
		CloseHandle(file);
		if (written == FALSE)
		{
			if (errorMessage != nullptr)
			{
				*errorMessage = "MiniDumpWriteDump failed.";
			}
			return false;
		}
		return true;
#else
		if (errorMessage != nullptr)
		{
			*errorMessage = "Native crash dump writing is unsupported on this platform.";
		}
		(void)dumpPath;
		return false;
#endif
	}
}

TerraEngine::RuntimePlatform TerraEngine::PlatformUtils::GetRuntimePlatform()
{
#ifdef _WIN32
	return RuntimePlatform::Windows;
#elif defined(__linux__)
	return RuntimePlatform::Linux;
#elif defined(__APPLE__)
	return RuntimePlatform::macOS;
#else
	return RuntimePlatform::Unknown;
#endif
}

std::filesystem::path TerraEngine::PlatformUtils::GetExecutablePath()
{
#ifdef _WIN32
	std::wstring buffer(MAX_PATH, L'\0');
	DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	while (length == buffer.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER)
	{
		buffer.resize(buffer.size() * 2u);
		length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	}

	if (length == 0 || length >= buffer.size())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Executable path discovery failed on Windows.");
		return {};
	}

	buffer.resize(length);
	return std::filesystem::path(buffer).lexically_normal();
#elif defined(__APPLE__)
	std::uint32_t bufferSize = 0;
	_NSGetExecutablePath(nullptr, &bufferSize);
	std::vector<char> buffer(static_cast<std::size_t>(bufferSize) + 1u, '\0');
	if (_NSGetExecutablePath(buffer.data(), &bufferSize) != 0)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Executable path discovery failed on macOS.");
		return {};
	}

	std::error_code errorCode;
	const std::filesystem::path resolved = std::filesystem::weakly_canonical(buffer.data(), errorCode);
	return errorCode ? std::filesystem::path(buffer.data()).lexically_normal() : resolved;
#elif defined(__linux__)
	std::vector<char> buffer(4096u, '\0');
	for (;;)
	{
		const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1u);
		if (length < 0)
		{
			TERRAENGINE_LOG(LogLevel::Warning, "Executable path discovery failed on Linux.");
			return {};
		}

		if (static_cast<std::size_t>(length) < buffer.size() - 1u)
		{
			buffer[static_cast<std::size_t>(length)] = '\0';
			return std::filesystem::path(buffer.data()).lexically_normal();
		}

		buffer.resize(buffer.size() * 2u, '\0');
	}
#else
	TERRAENGINE_LOG(LogLevel::Warning, "Executable path discovery is unavailable on this platform.");
	return {};
#endif
}

std::filesystem::path TerraEngine::PlatformUtils::GetExecutableDirectory()
{
	const std::filesystem::path executablePath = GetExecutablePath();
	return executablePath.empty() ? std::filesystem::path{} : executablePath.parent_path();
}

std::filesystem::path TerraEngine::PlatformUtils::GetPersistentDataPath(std::string_view productName)
{
	const std::string sanitizedProductName = SanitizeProductName(productName);
	const std::filesystem::path productRoot = ResolveLocalAppDataRoot(sanitizedProductName);
	if (productRoot.empty())
	{
		return {};
	}

	std::filesystem::path persistentDataPath = (productRoot / kDefaultProductVersionFolder).lexically_normal();
	std::error_code errorCode;
	std::filesystem::create_directories(persistentDataPath, errorCode);

	if (errorCode)
	{
		TERRAENGINE_LOG(LogLevel::Error, "PersistentDataPath could not create '" << persistentDataPath << "': " << errorCode.message());
	}

	return persistentDataPath;
}

std::filesystem::path TerraEngine::PlatformUtils::GetDiagnosticsDataPath(std::string_view productName)
{
	const std::string sanitizedProductName = SanitizeProductName(productName);
	const std::filesystem::path productRoot = ResolveLocalAppDataRoot(sanitizedProductName);
	if (productRoot.empty())
	{
		return {};
	}

	return CreateDirectoryOrFallback(productRoot / kDiagnosticsFolderName, "Diagnostics");
}

std::filesystem::path TerraEngine::PlatformUtils::GetCrashDumpDirectory(std::string_view productName)
{
	const std::filesystem::path diagnosticsPath = GetDiagnosticsDataPath(productName);
	if (diagnosticsPath.empty())
	{
		return {};
	}

	return CreateDirectoryOrFallback(diagnosticsPath / kCrashDumpsFolderName, "CrashDumps");
}

TerraEngine::FatalErrorDialogReport TerraEngine::PlatformUtils::WriteCrashDumpForReport(FatalErrorDialogReport report, std::string_view productName)
{
	report.crashDumpDirectory = GetCrashDumpDirectory(productName);
	if (report.crashDumpDirectory.empty())
	{
		report.crashDumpStatus = CrashDumpStatus::Failed;
		report.crashDumpFailureReason = "Could not resolve a writable diagnostics crash dump directory.";
		return report;
	}

	const std::string sanitizedCategory = SanitizeProductName(ToString(report.category));
#ifdef _WIN32
	const std::uint32_t processId = GetCurrentProcessId();
#else
	const std::uint32_t processId = static_cast<std::uint32_t>(getpid());
#endif
	const std::string fileStem =
		"TerraEngineCrash-"
		+ MakeCrashTimestamp()
		+ "-"
		+ std::to_string(processId)
		+ "-"
		+ sanitizedCategory;
	report.crashDumpPath = report.crashDumpDirectory / (fileStem + ".dmp");
	report.crashSummaryPath = report.crashDumpDirectory / (fileStem + ".txt");

	std::string dumpError;
	if (WriteNativeCrashDump(report.crashDumpPath, &dumpError))
	{
		report.crashDumpStatus = CrashDumpStatus::Written;
	}
	else
	{
#ifdef _WIN32
		report.crashDumpStatus = CrashDumpStatus::Failed;
#else
		report.crashDumpStatus = CrashDumpStatus::Unsupported;
#endif
		report.crashDumpFailureReason = dumpError;
	}

	std::string sidecarError;
	if (!SaveFatalErrorSummarySidecar(report, report.crashSummaryPath, &sidecarError) && report.crashDumpFailureReason.empty())
	{
		report.crashDumpFailureReason = "Crash summary sidecar write failed: " + sidecarError;
	}
	return report;
}


bool TerraEngine::PlatformUtils::OpenUrl(std::string_view url)
{
	if (url.empty() || !HasUrlScheme(url))
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Application.OpenURL ignored malformed input: '" << std::string(url) << "'.");
		return false;
	}

#ifdef _WIN32
	const std::wstring wideUrl = Utf8ToWide(url);
	if (wideUrl.empty())
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Application.OpenURL could not convert URL to UTF-16: '" << std::string(url) << "'.");
		return false;
	}

	const HINSTANCE result = ShellExecuteW(nullptr, L"open", wideUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

	if (reinterpret_cast<std::intptr_t>(result) <= 32)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Application.OpenURL failed to launch '" << std::string(url) << "' with ShellExecuteW.");
		return false;
	}

	return true;
#else
	std::string ownedUrl(url);
	char* const argv[] = {
		const_cast<char*>("xdg-open"),
		ownedUrl.data(),
		nullptr
	};

	pid_t childProcessId = 0;
	const int spawnResult = posix_spawnp(&childProcessId, "xdg-open", nullptr, nullptr, argv, environ);

	if (spawnResult != 0)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Application.OpenURL failed to launch '" << ownedUrl << "' with xdg-open (error " << spawnResult << ").");
		return false;
	}

	return true;
#endif
}
