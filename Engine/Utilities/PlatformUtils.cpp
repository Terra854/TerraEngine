#include "Utilities/PlatformUtils.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <system_error>

#include "Core/Logger/Logger.h"

#ifdef _WIN32
#	ifndef NOMINMAX
#		define NOMINMAX
#	endif
#	include <Objbase.h>
#	include <ShlObj.h>
#	include <shellapi.h>
#	include <Windows.h>
#else
#	include <spawn.h>

extern char** environ;
#endif

namespace
{
	constexpr std::string_view kDefaultProductName = "TerraEngine";
	constexpr std::string_view kDefaultProductVersionFolder = "SaveData";

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
	if (xdgDataHome != nullptr && xdgDataHome[0] != '\0')
	{
		basePath = std::filesystem::path(xdgDataHome);
	}
	else
	{
		const char* homeDirectory = std::getenv("HOME");
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
	if (errorCode)
	{
		TERRAENGINE_LOG(LogLevel::Error, "PersistentDataPath could not create '" << persistentDataPath << "': " << errorCode.message());
	}

	return persistentDataPath;
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
