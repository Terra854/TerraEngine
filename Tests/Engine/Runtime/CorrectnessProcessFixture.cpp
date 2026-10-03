// Build/install CorrectnessProcessFixture through BUILD_TESTING before using the
// installed Build/Tests fixture. This standalone child deliberately has no engine
// dependencies. Example: CorrectnessProcessFixture --gate release --spawn-depth 2.
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <Windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	std::filesystem::path Utf8Path(const std::string& value)
	{
		// C++20 char8_t construction keeps paths UTF-8 on every host platform.
		std::u8string encoded;
		for (const unsigned char character : value) encoded.push_back(static_cast<char8_t>(character));
		return std::filesystem::path(encoded);
	}
#ifdef _WIN32
	std::string Utf8(const std::wstring& value)
	{
		if (value.empty()) return {};
		const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		std::string result(length, '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
		return result;
	}
#endif
	int Run(const std::vector<std::string>& arguments)
	{
		std::filesystem::path gate;
		std::filesystem::path pidFile;
		int depth = 0;
		int timeoutMs = 10000;
		int exitCode = 0;
		// Report arguments separately so quoting and UTF-8 transport are observable.
		for (size_t index = 1; index < arguments.size(); ++index)
		{
			const auto& argument = arguments[index];
			if (index + 1 == arguments.size()) { std::cerr << "missing value: " << argument << '\n'; return 2; }
			const auto& value = arguments[++index];
			if (argument == "--gate") gate = Utf8Path(value);
			else if (argument == "--pid-file") pidFile = Utf8Path(value);
			else if (argument == "--spawn-depth") depth = std::stoi(value);
			else if (argument == "--timeout-ms") timeoutMs = std::stoi(value);
			else if (argument == "--exit-code") exitCode = std::stoi(value);
			else if (argument == "--stdout") std::cout << value << '\n';
			else if (argument == "--stderr") std::cerr << value << '\n';
			else if (argument == "--env")
			{
#ifdef _WIN32
				const auto name = Utf8Path(value).wstring();
				const auto length = GetEnvironmentVariableW(name.c_str(), nullptr, 0);
				std::wstring content(length, L'\0');
				if (length) { GetEnvironmentVariableW(name.c_str(), content.data(), length); content.resize(length - 1); }
				std::cout << "env:" << value << '=' << Utf8(content) << '\n';
#else
				const char* content = std::getenv(value.c_str());
				std::cout << "env:" << value << '=' << (content ? content : "") << '\n';
#endif
			}
			else { std::cerr << "unknown option: " << argument << '\n'; return 2; }
		}
		if (depth < 0 || depth > 8 || timeoutMs < 1 || timeoutMs > 300000) return 2;
#ifdef _WIN32
		const auto pid = GetCurrentProcessId();
#else
		const auto pid = getpid();
#endif
		if (!pidFile.empty())
		{
			// One file per depth avoids concurrent append ordering assumptions.
			auto depthPath = pidFile;
			depthPath += "." + std::to_string(depth);
			std::ofstream stream(depthPath);
			stream << pid << '\n';
			if (!stream) return 3;
		}
		std::cout << "ready:" << pid << ":depth=" << depth << '\n' << std::flush;
		std::cerr << std::flush;
#ifdef _WIN32
		HANDLE child = nullptr;
#else
		pid_t child = -1;
#endif
		if (depth > 0)
		{
			// Descendants inherit the owner's process job/group and output handles.
#ifdef _WIN32
			std::wstring executable(32768, L'\0');
			executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
			std::wstring command = L"\"" + executable + L"\" --spawn-depth " + std::to_wstring(depth - 1) + L" --timeout-ms " + std::to_wstring(timeoutMs);
			if (!gate.empty()) command += L" --gate \"" + gate.wstring() + L"\"";
			if (!pidFile.empty()) command += L" --pid-file \"" + pidFile.wstring() + L"\"";
			STARTUPINFOW startup{};
			startup.cb = sizeof(startup);
			startup.dwFlags = STARTF_USESTDHANDLES;
			startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
			startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
			startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
			PROCESS_INFORMATION process{};
			if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 4;
			CloseHandle(process.hThread);
			child = process.hProcess;
#else
			child = fork();
			if (child == 0)
			{
				std::vector<std::string> values{ arguments.front(), "--spawn-depth", std::to_string(depth - 1), "--timeout-ms", std::to_string(timeoutMs) };
				if (!gate.empty()) { values.push_back("--gate"); values.push_back(gate.string()); }
				if (!pidFile.empty()) { values.push_back("--pid-file"); values.push_back(pidFile.string()); }
				std::vector<char*> pointers;
				for (auto& value : values) pointers.push_back(value.data());
				pointers.push_back(nullptr);
				execv(pointers.front(), pointers.data());
				_exit(4);
			}
			if (child < 0) return 4;
#endif
		}
		// File existence is an explicit release signal; the deadline is only a
		// safety net. No engine worker or future is occupied by this test process.
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
		bool released = gate.empty();
		while (!released && std::chrono::steady_clock::now() < deadline)
		{
			std::error_code error;
			released = std::filesystem::exists(gate, error);
			if (!released) std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
#ifdef _WIN32
		if (child)
		{
			if (WaitForSingleObject(child, static_cast<DWORD>(timeoutMs + 1000)) != WAIT_OBJECT_0) TerminateProcess(child, 124);
			CloseHandle(child);
		}
#else
		if (child > 0) { int status = 0; while (waitpid(child, &status, 0) < 0 && errno == EINTR) {} }
#endif
		std::cout << (released ? "released:" : "timeout:") << pid << '\n' << std::flush;
		return released ? exitCode : 124;
	}
}
#ifdef _WIN32
int wmain(int count, wchar_t** values)
{
	std::vector<std::string> arguments;
	for (int index = 0; index < count; ++index) arguments.push_back(Utf8(values[index]));
#else
int main(int count, char** values)
{
	std::vector<std::string> arguments(values, values + count);
#endif
	try { return Run(arguments); }
	catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
