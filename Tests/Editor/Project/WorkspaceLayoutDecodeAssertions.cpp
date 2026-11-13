// WorkspaceLayoutDecodeAssertions.cpp - isolated production-decoder assertion checks.
// Build this single translation unit with assertions enabled and bundled JSON only.
// It deliberately links neither the Engine DLL nor release editor translation units.
#include "Editor/TerraEngine-Editor/Core/WorkspaceLayoutDecode.h"
#include <cassert>
#include <cstddef>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>
#ifdef NDEBUG
#error Workspace layout assertion qualification requires NDEBUG to be undefined.
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdlib>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace {
	using namespace TerraEngine::Editor;
	using Json = nlohmann::json;
	struct AssertionChild final { bool started = false; bool timedOut = false; bool failed = false; };
#ifdef _WIN32
	struct OwnedHandle final {
		HANDLE value = nullptr;
		~OwnedHandle() { if(value != nullptr && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
		OwnedHandle() = default;
		OwnedHandle(const OwnedHandle&) = delete;
		OwnedHandle& operator=(const OwnedHandle&) = delete;
	};
#endif
	AssertionChild RunAssertionChild(const std::filesystem::path& executable, const std::filesystem::path& log)
	{
		// The deliberate invalid const lookup is isolated in an owned native child.
		// Its deadline is bounded; every child is reaped before observation storage ends.
		AssertionChild result;
#ifdef _WIN32
		OwnedHandle job;
		job.value = CreateJobObjectW(nullptr, nullptr);
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
		limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		if(job.value == nullptr || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return result;
		SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
		OwnedHandle output;
		output.value = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(output.value == INVALID_HANDLE_VALUE) return result;
		SIZE_T size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		std::vector<std::byte> storage(size);
		auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
		if(!InitializeProcThreadAttributeList(attributes, 1, 0, &size)) return result;
		struct AttributeOwner final { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeOwner() { DeleteProcThreadAttributeList(value); } } owner{attributes};
		HANDLE inherited[] = {output.value};
		if(!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) return result;
		STARTUPINFOEXW startup{};
		startup.StartupInfo.cb = sizeof(startup);
		startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
		startup.StartupInfo.hStdOutput = output.value;
		startup.StartupInfo.hStdError = output.value;
		startup.lpAttributeList = attributes;
		PROCESS_INFORMATION process{};
		std::wstring command = L"\"" + executable.native() + L"\" --assert-control";
		if(!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
			CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process)) return result;
		OwnedHandle child, thread;
		child.value = process.hProcess; thread.value = process.hThread; result.started = true;
		// Attach ownership before running user code; closing the job also retires
		// the child if any later observation or timeout path fails.
		if(!AssignProcessToJobObject(job.value, child.value) || ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
			TerminateProcess(child.value, 255);
			WaitForSingleObject(child.value, 5000);
			return result;
		}
		const DWORD wait = WaitForSingleObject(child.value, 5000);
		if(wait != WAIT_OBJECT_0) {
			result.timedOut = true;
			TerminateProcess(child.value, 255);
			WaitForSingleObject(child.value, 5000);
		}
		DWORD code = 0;
		result.failed = GetExitCodeProcess(child.value, &code) && code != 0;
#else
		const pid_t child = fork();
		if(child == 0) {
			const rlimit noCore{0, 0};
			setrlimit(RLIMIT_CORE, &noCore);
			const int output = open(log.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
			if(output < 0 || dup2(output, STDOUT_FILENO) < 0 || dup2(output, STDERR_FILENO) < 0) _exit(120);
			close(output);
			execl(executable.c_str(), executable.c_str(), "--assert-control", static_cast<char*>(nullptr));
			_exit(121);
		}
		if(child < 0) return result;
		result.started = true;
		int status = 0;
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while(true) {
			const pid_t waited = waitpid(child, &status, WNOHANG);
			if(waited == child) break;
			if(waited < 0 && errno != EINTR) return result;
			if(std::chrono::steady_clock::now() >= deadline) {
				result.timedOut = true;
				kill(child, SIGKILL);
				while(waitpid(child, &status, 0) < 0 && errno == EINTR) {}
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		result.failed = WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) != 0);
#endif
		return result;
	}

	int AssertionControl()
	{
#ifdef _WIN32
		// Route CRT assertion text to the owned log, with no modal dialog or WER UI.
		_set_error_mode(_OUT_TO_STDERR);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
		const Json value = Json::object();
		(void)value["deliberately-missing-assertion-control"];
		return 0;
	}

	int Run(const std::filesystem::path& executable, const std::filesystem::path& evidenceRoot)
	{
		std::size_t selected = 0, failures = 0;
		const auto check = [](bool value, const char* reason) { if(!value) throw std::runtime_error(reason); };
		const auto test = [&](const char* name, const auto& body) {
			++selected;
			try { body(); std::cout << "[PASS] " << name << '\n'; }
			catch(const std::exception& error) { ++failures; std::cout << "[FAIL] " << name << ": " << error.what() << '\n'; }
		};
		test("WorkspaceLayout.AssertionsEnabled", [&] { bool enabled = false; assert((enabled = true)); check(enabled, "Native assert must execute its operand."); });
		test("WorkspaceLayout.MalformedInventoryRejected", [&] {
			for(const auto& value : {Json(nullptr), Json(4), Json::array(), Json::object(), Json{{"windows", 7}}}) check(FindWorkspaceLayoutWindows(value) == nullptr, "Malformed inventory must reject before const lookup.");
		});
		for(const auto& [name, value] : std::vector<std::pair<const char*, Json>>{
			{"WorkspaceLayout.MissingTitle", {{"visible", true}}},
			{"WorkspaceLayout.MissingVisible", {{"title", "missing-visible"}}},
			{"WorkspaceLayout.WrongTitleType", {{"title", 42}, {"visible", true}}},
			{"WorkspaceLayout.WrongVisibleType", {{"title", "window"}, {"visible", "yes"}}},
			{"WorkspaceLayout.WrongManagedType", {{"title", "window"}, {"visible", true}, {"managed", 1}}}}) {
			test(name, [&] { check(!DecodeWorkspaceWindowVisibility(value), "Malformed entry must reject safely."); });
		}
		test("WorkspaceLayout.NonObjectEntryRejected", [&] { for(const auto& value : {Json(nullptr), Json(7), Json::array()}) check(!DecodeWorkspaceWindowVisibility(value), "Nonobject entry must reject."); });
		test("WorkspaceLayout.NativeVisibleControls", [&] { for(bool visible : {false, true}) { const auto value = DecodeWorkspaceWindowVisibility(Json{{"title", "native"}, {"visible", visible}, {"managed", false}}); check(value && value->title == "native" && value->visible == visible && !value->managed, "Native visibility must retain exact fields."); } });
		test("WorkspaceLayout.ManagedControl", [&] { const auto value = DecodeWorkspaceWindowVisibility(Json{{"title", "managed"}, {"visible", true}, {"managed", true}}); check(value && value->managed && value->visible, "Managed flag must remain authored."); });
		test("WorkspaceLayout.AbsentManagedDefaultsFalse", [&] { const auto value = DecodeWorkspaceWindowVisibility(Json{{"title", "native"}, {"visible", true}}); check(value && !value->managed, "Absent managed flag must retain native default."); });
		test("WorkspaceLayout.DecodedTitleOwnsStorage", [&] { Json input{{"title", "owned title"}, {"visible", true}}; const auto value = DecodeWorkspaceWindowVisibility(input); input.clear(); check(value && value->title == "owned title", "Decoded title must outlive source JSON changes."); });
		test("WorkspaceLayout.MalformedThenValidDecode", [&] {
			const Json layout{{"windows", Json::array({Json{{"visible", true}}, Json{{"title", "valid"}, {"visible", true}, {"managed", true}}})}};
			const auto* inventory = FindWorkspaceLayoutWindows(layout); check(inventory != nullptr, "Valid array must remain usable.");
			std::size_t accepted = 0, rejected = 0;
			for(const auto& entry : *inventory) { const auto value = DecodeWorkspaceWindowVisibility(entry); if(value) { ++accepted; check(value->title == "valid" && value->managed, "Later valid entry must decode exactly."); } else ++rejected; }
			check(accepted == 1 && rejected == 1, "Malformed sibling must not prevent valid decode.");
		});
		test("WorkspaceLayout.ActualJsonAssertionChild", [&] {
			const auto owned = evidenceRoot / ("assertion-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			check(std::filesystem::create_directories(owned), "Assertion evidence directory must be fresh and owned.");
			const auto log = owned / "assertion-control.log";
			const auto child = RunAssertionChild(executable, log);
			check(child.started && !child.timedOut && child.failed, "Actual missing-key const JSON lookup must assert in its bounded child.");
			check(std::filesystem::file_size(log) <= 65536, "Assertion diagnostic must remain bounded.");
			std::ifstream input(log); std::string output{std::istreambuf_iterator<char>(input), {}};
			check(output.find("Assertion") != std::string::npos && output.find("json.hpp") != std::string::npos, "Child failure must identify an actual assertion in the bundled JSON header.");
			const auto pathBytes = log.generic_u8string();
			std::cout << "Assertion diagnostic: " << std::string(reinterpret_cast<const char*>(pathBytes.data()), pathBytes.size()) << '\n';
		});
		// An empty or accidentally shortened driver must fail independently of
		// CTest output matching. Keep the explicit count tied to this fixed suite.
		constexpr std::size_t expectedChecks = 14;
		if(selected != expectedChecks) {
			++failures;
			std::cout << "[FAIL] WorkspaceLayout.RequiredCheckCount: expected " << expectedChecks << ", selected " << selected << '\n';
		}
		std::cout << selected << " selected checks, " << failures << " failures; shared production decoder, assertions enabled; no full Debug editor claim.\n";
		return failures == 0 && selected == expectedChecks ? 0 : 1;
	}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
	if(argc == 2 && std::wstring_view(argv[1]) == L"--assert-control") return AssertionControl();
	if(argc != 1 && !(argc == 3 && std::wstring_view(argv[1]) == L"--evidence-root")) return 2;
	const auto root = argc == 3 && std::wstring_view(argv[1]) == L"--evidence-root" ? std::filesystem::path(argv[2]) : std::filesystem::current_path() / "out/validation/workspace-layout-assertions";
	return Run(std::filesystem::absolute(argv[0]), root);
}
#else
int main(int argc, char** argv)
{
	if(argc == 2 && std::string_view(argv[1]) == "--assert-control") return AssertionControl();
	if(argc != 1 && !(argc == 3 && std::string_view(argv[1]) == "--evidence-root")) return 2;
	const auto root = argc == 3 && std::string_view(argv[1]) == "--evidence-root" ? std::filesystem::path(argv[2]) : std::filesystem::current_path() / "out/validation/workspace-layout-assertions";
	return Run(std::filesystem::absolute(argv[0]), root);
}
#endif
