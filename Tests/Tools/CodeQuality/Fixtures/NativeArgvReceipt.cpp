// NativeArgvReceipt.cpp -- bounded validation-only OS argv recorder.
// Build/install as a standalone C++20 test child; no Engine/GPU dependency.
// Windows links only the OS user32 library for hidden-console observation.
// Usage: set TERRAENGINE_SOAK_ARGV_RECEIPT to a fresh absolute JSON path, then
// launch with the arguments under test. This child records them and exits; it
// never executes a navigation workload or waits for a declared soak duration.
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {
	constexpr std::size_t kMaximumArguments = 64u;
	constexpr std::size_t kMaximumBytes = 65536u;
	std::string JsonString(std::string_view value) {
		constexpr char hex[] = "0123456789abcdef";
		std::string result = "\"";
		for (unsigned char byte : value) {
			if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
			else if (byte < 32u) { result += "\\u00"; result += hex[byte >> 4u]; result += hex[byte & 15u]; }
			else result += static_cast<char>(byte);
		}
		return result + '"';
	}
#ifdef _WIN32
	std::string Utf8(std::wstring_view text) {
		if (text.size() > kMaximumBytes) throw std::invalid_argument("Native UTF16 input exceeds recorder bound.");
		if (text.empty()) return {};
		const int length = static_cast<int>(text.size());
		const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, nullptr, 0, nullptr, nullptr);
		if (bytes <= 0 || static_cast<std::size_t>(bytes) > kMaximumBytes) throw std::invalid_argument("Native argument is invalid Unicode or exceeds UTF8 bound.");
		std::string result(static_cast<std::size_t>(bytes), '\0');
		if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(), bytes, nullptr, nullptr) != bytes)
			throw std::runtime_error("Native UTF16 argument conversion failed.");
		return result;
	}
#else
	// POSIX argv is bytes. Reject invalid UTF8 instead of emitting lossy/invalid
	// JSON and falsely claiming it faithfully represents those native bytes.
	std::string Utf8(std::string_view text) {
		if (text.size() > kMaximumBytes) throw std::invalid_argument("Native argument exceeds recorder bound.");
		for (std::size_t index = 0u; index < text.size();) {
			const auto lead = static_cast<unsigned char>(text[index++]);
			if (lead < 0x80u) continue;
			unsigned trailing = 0u; std::uint32_t codepoint = 0u, minimum = 0u;
			if (lead >= 0xC2u && lead <= 0xDFu) { trailing = 1u; codepoint = lead & 0x1Fu; minimum = 0x80u; }
			else if (lead >= 0xE0u && lead <= 0xEFu) { trailing = 2u; codepoint = lead & 0x0Fu; minimum = 0x800u; }
			else if (lead >= 0xF0u && lead <= 0xF4u) { trailing = 3u; codepoint = lead & 7u; minimum = 0x10000u; }
			else throw std::invalid_argument("Native argv contains invalid UTF8.");
			if (trailing > text.size() - index) throw std::invalid_argument("Native argv contains truncated UTF8.");
			while (trailing-- != 0u) {
				const auto byte = static_cast<unsigned char>(text[index++]);
				if ((byte & 0xC0u) != 0x80u) throw std::invalid_argument("Native argv contains invalid UTF8 continuation.");
				codepoint = (codepoint << 6u) | (byte & 0x3Fu);
			}
			if (codepoint < minimum || codepoint > 0x10FFFFu || (codepoint >= 0xD800u && codepoint <= 0xDFFFu))
				throw std::invalid_argument("Native argv contains invalid UTF8 codepoint.");
		}
		return std::string(text);
	}
#endif
	int Record(const std::vector<std::string>& arguments, std::string_view executable,
		std::string_view workingDirectory, const std::filesystem::path& receipt, std::uint64_t processId) {
		if (arguments.size() > kMaximumArguments || receipt.empty() || !receipt.is_absolute()) return 2;
		std::size_t totalBytes = 0u;
		for (const auto& argument : arguments) {
			if (argument.size() > kMaximumBytes - totalBytes) return 2;
			totalBytes += argument.size();
		}
		std::string json = "{\"scope\":\"NativeArgvProtocol_NotNavigationSoak\",\"processId\":" + std::to_string(processId)
			+ ",\"argv0\":" + JsonString(executable) + ",\"workingDirectory\":" + JsonString(workingDirectory) + ",\"argv\":[";
		for (std::size_t index = 0u; index < arguments.size(); ++index) { if (index) json += ','; json += JsonString(arguments[index]); }
		json += "]";
#ifdef _WIN32
		const HWND console = GetConsoleWindow();
		json += ",\"consoleVisible\":";
		json += (console && IsWindowVisible(console)) ? "true" : "false";
#else
		json += ",\"consoleVisible\":null";
#endif
		json += '}';
		// Sequential work is intentional: one bounded native child's receipt must
		// represent its exact argv, not a prediction from another launcher/task.
		std::ofstream output(receipt, std::ios::binary | std::ios::trunc);
		output << json; output.close();
		if (!output) return 3;
		std::cout << json << '\n';
		return 0;
	}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
	if (argc < 1 || static_cast<std::size_t>(argc - 1) > kMaximumArguments) return 2;
	try {
		const auto size = GetEnvironmentVariableW(L"TERRAENGINE_SOAK_ARGV_RECEIPT", nullptr, 0);
		if (!size || size > kMaximumBytes) return 2;
		std::wstring receipt(size, L'\0');
		const auto actual = GetEnvironmentVariableW(L"TERRAENGINE_SOAK_ARGV_RECEIPT", receipt.data(), size);
		if (!actual || actual >= size) return 2;
		receipt.resize(actual);
		std::vector<std::string> arguments;
		for (int index = 1; index < argc; ++index) arguments.push_back(Utf8(argv[index]));
		return Record(arguments, Utf8(argv[0]), Utf8(std::filesystem::current_path().wstring()),
			std::filesystem::path(receipt), GetCurrentProcessId());
	} catch (const std::exception& error) { std::cerr << "Native argv recorder: " << error.what() << '\n'; return 4; }
}
#else
int main(int argc, char** argv) {
	if (argc < 1 || static_cast<std::size_t>(argc - 1) > kMaximumArguments) return 2;
	try {
		const auto* receipt = std::getenv("TERRAENGINE_SOAK_ARGV_RECEIPT");
		if (!receipt) return 2;
		std::vector<std::string> arguments;
		for (int index = 1; index < argc; ++index) arguments.push_back(Utf8(argv[index]));
		return Record(arguments, Utf8(argv[0]), Utf8(std::filesystem::current_path().native()),
			std::filesystem::path(Utf8(receipt)), static_cast<std::uint64_t>(getpid()));
	} catch (const std::exception& error) { std::cerr << "Native argv recorder: " << error.what() << '\n'; return 4; }
}
#endif
