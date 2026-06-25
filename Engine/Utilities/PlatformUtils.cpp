// PlatformUtils.cpp — Implementation of platform-specific utility functions.
//
// This file contains the implementation of OS-specific logic for Windows and
// Linux, including shell execution for opening URLs and path resolution for
// persistent storage. It provides a unified interface for platform-dependent
// operations used throughout the engine.

#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
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

	// SanitizeProductName is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::string SanitizeProductName(std::string_view productName)
	{
		std::string sanitized = productName.empty()
			? std::string(kDefaultProductName)
			: std::string(productName);

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (char& character : sanitized)
		{
			if (character == '/' || character == '\\' || character == ':' || character == '*' || character == '?' || character == '"' || character == '<' || character == '>' || character == '|')
			{
				character = '_';
			}
		}

		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (sanitized.empty())
		{
			sanitized = kDefaultProductName;
		}

		return sanitized;
	}

	// HasUrlScheme is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	bool HasUrlScheme(std::string_view url)
	{
		const std::size_t separatorIndex = url.find(':');
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (separatorIndex == std::string_view::npos || separatorIndex == 0)
		{
			return false;
		}

		// Iterate over the current data snapshot; avoid changing ownership rules inside the loop body.
		for (std::size_t index = 0; index < separatorIndex; ++index)
		{
			const unsigned char value = static_cast<unsigned char>(url[index]);
			// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
			if (!std::isalnum(value) && value != '+' && value != '-' && value != '.')
			{
				return false;
			}
		}

		return true;
	}

#ifdef _WIN32
	// Utf8ToWide is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
	std::wstring Utf8ToWide(std::string_view text)
	{
		if (text.empty())
		{
			return {};
		}

		const int wideLength = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (wideLength <= 0)
		{
			return {};
		}

		std::wstring wideText(static_cast<std::size_t>(wideLength), L'\0');
		const int convertedLength = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wideText.data(), wideLength);
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (convertedLength <= 0)
		{
			return {};
		}

		return wideText;
	}
#endif
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
	std::filesystem::path basePath;

#ifdef _WIN32
	PWSTR localAppDataPath = nullptr;
	const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppDataPath);
	if (FAILED(result) || localAppDataPath == nullptr)
	{
		TERRAENGINE_LOG(LogLevel::Error, "PersistentDataPath could not resolve FOLDERID_LocalAppData for product '" << sanitizedProductName << "'.");
		return {};
	}

	basePath = std::filesystem::path(localAppDataPath);
	CoTaskMemFree(localAppDataPath);
#else
	const char* xdgDataHome = std::getenv("XDG_DATA_HOME");
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (xdgDataHome != nullptr && xdgDataHome[0] != '\0')
	{
		basePath = std::filesystem::path(xdgDataHome);
	}
	else
	{
		const char* homeDirectory = std::getenv("HOME");
		// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
		if (homeDirectory == nullptr || homeDirectory[0] == '\0')
		{
			TERRAENGINE_LOG(LogLevel::Error, "PersistentDataPath could not resolve HOME or XDG_DATA_HOME for product '" << sanitizedProductName << "'.");
			return {};
		}

		basePath = std::filesystem::path(homeDirectory) / ".local" / "share";
	}
#endif

	std::filesystem::path persistentDataPath = (basePath / sanitizedProductName / kDefaultProductVersionFolder).lexically_normal();
	std::error_code errorCode;
	std::filesystem::create_directories(persistentDataPath, errorCode);
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (errorCode)
	{
		TERRAENGINE_LOG(LogLevel::Error, "PersistentDataPath could not create '" << persistentDataPath << "': " << errorCode.message());
	}

	return persistentDataPath;
}

// TerraEngine::PlatformUtils::OpenUrl is an implementation entry point; keep its inputs, side effects, and ownership expectations aligned with callers.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
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
	// Guard this branch so invalid input or unavailable runtime state exits before touching shared engine data.
	if (spawnResult != 0)
	{
		TERRAENGINE_LOG(LogLevel::Warning, "Application.OpenURL failed to launch '" << ownedUrl << "' with xdg-open (error " << spawnResult << ").");
		return false;
	}

	return true;
#endif
}
