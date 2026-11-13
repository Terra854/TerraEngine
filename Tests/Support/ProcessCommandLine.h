// Test-only command-line encoding for the existing OwnedProcess boundary.
// The helper owns no process, thread, global state or engine API surface.
#pragma once
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace TerraEngine::Tests {
	inline std::string Utf8ProcessPath(const std::filesystem::path& path) {
		// OwnedProcess converts UTF-8 to UTF-16 for CreateProcessW. path.string()
		// may instead use a Windows ANSI code page and lose non-ASCII filenames.
		const auto encoded = path.u8string();
		return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
	}
	inline std::string QuoteProcessArgument(std::string_view value) {
#ifdef _WIN32
		// Match Windows argv parsing: backslashes before a quote are doubled,
		// and trailing backslashes are doubled before the closing quote. Empty
		// arguments therefore survive too. CreateProcessW invokes no shell here.
		std::string result = "\"";
		std::size_t slashes = 0;
		for(const char character : value) {
			if(character == '\\') { ++slashes; continue; }
			result.append(character == '"' ? slashes * 2 + 1 : slashes, '\\');
			result.push_back(character); slashes = 0;
		}
		result.append(slashes * 2, '\\');
		return result + '"';
#else
		// OwnedProcess uses /bin/sh -c on POSIX. Single quotes protect spaces,
		// dollar signs and backticks; close/escape/reopen for literal apostrophes.
		std::string result = "'";
		for(const char character : value) result += character == '\'' ? "'\\''" : std::string(1, character);
		return result + '\'';
#endif
	}
	inline std::string QuoteProcessPath(const std::filesystem::path& path) {
		return QuoteProcessArgument(Utf8ProcessPath(path));
	}
}
