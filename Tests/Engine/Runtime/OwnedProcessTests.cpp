#include "TestHarness.h"
#include "CorrectnessTestSupport.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "ThreadPool/ThreadPool.h"
#include <cstdlib>
#include <array>
#include <fstream>
#include <future>
#include <thread>
#ifdef _WIN32
#include <Windows.h>
#else
#include <signal.h>
#endif

namespace TerraEngine::Tests {
	namespace {
		using TerraEngine::Utilities::OwnedProcess;
		using TerraEngine::Utilities::OwnedProcessRequest;
		std::string Utf8Path(const std::filesystem::path& path) {
			const auto encoded = path.u8string(); return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
		}
		std::string Quote(const std::string& value) {
#ifdef _WIN32
			std::string result = "\""; std::size_t slashes = 0;
			for (const auto character : value) {
				if (character == '\\') { ++slashes; continue; }
				result.append(character == '"' ? slashes * 2 + 1 : slashes, '\\'); result.push_back(character); slashes = 0;
			}
			result.append(slashes * 2, '\\'); return result + '"';
#else
			std::string result = "'";
			for (const auto character : value) result += character == '\'' ? "'\\''" : std::string(1, character);
			return result + '\'';
#endif
		}
		OwnedProcessRequest FixtureRequest(const std::vector<std::string>& arguments) {
			const auto fixture = TerraEngine::PlatformUtils::GetExecutableDirectory() /
#ifdef _WIN32
				"CorrectnessProcessFixture.exe";
#else
				"CorrectnessProcessFixture";
#endif
			if (!std::filesystem::is_regular_file(fixture)) throw std::runtime_error("Build/install the correctness child fixture beside Engine tests.");
			OwnedProcessRequest request; request.commandLine = Quote(Utf8Path(fixture));
			for (const auto& argument : arguments) request.commandLine += " " + Quote(argument);
			return request;
		}
		bool WaitForFiles(const std::filesystem::path& prefix) {
			const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			while (std::chrono::steady_clock::now() < deadline) {
				if (std::filesystem::exists(prefix.string() + ".0") && std::filesystem::exists(prefix.string() + ".1") && std::filesystem::exists(prefix.string() + ".2")) return true;
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			}
			return false;
		}
		std::string DescendantSettlementFailure(const std::filesystem::path& prefix) {
			for (int depth = 0; depth < 3; ++depth) {
				const auto receipt = prefix.string() + "." + std::to_string(depth);
				std::ifstream input(receipt); unsigned long pid = 0; input >> pid;
				// Missing startup evidence is a different failure from a live child.
				// Include the receipt and native wait result so a deadline regression
				// cannot be diagnosed merely from a conflated boolean assertion.
				if (!input || !pid) return "Missing or incomplete descendant PID receipt: " + receipt;
#ifdef _WIN32
				const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
				// Settlement belongs to production Run, not to a grace period added
				// by the assertion after project dependencies could already be gone.
				if (process) {
					const auto waited = WaitForSingleObject(process, 0); const auto error = GetLastError(); CloseHandle(process);
					if (waited != WAIT_OBJECT_0) return "Descendant PID " + std::to_string(pid) + " was not settled at return; wait=" + std::to_string(waited) + " error=" + std::to_string(error);
				} else if (const auto error = GetLastError(); error != ERROR_INVALID_PARAMETER) {
					return "Could not inspect descendant PID " + std::to_string(pid) + "; error=" + std::to_string(error);
				}
#else
				const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (kill(static_cast<pid_t>(pid), 0) == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
				if (kill(static_cast<pid_t>(pid), 0) == 0) return "Descendant PID " + std::to_string(pid) + " survived settlement.";
#endif
			}
			return {};
		}
#ifdef _WIN32
		class RetainedDescendants {
		public:
			~RetainedDescendants() { for (const auto handle : handles_) if (handle) CloseHandle(handle); }
			std::string AwaitStartup(const std::filesystem::path& prefix, std::chrono::milliseconds timeout) {
				// Retain the actual process objects while all three children are gated.
				// Reopening a numeric PID after Run returns can inspect an unrelated
				// process when Windows has already recycled the terminated child's ID.
				const auto deadline = std::chrono::steady_clock::now() + timeout;
				while (std::chrono::steady_clock::now() < deadline) {
					bool complete = true;
					for (std::size_t depth = 0; depth < handles_.size(); ++depth) {
						if (handles_[depth]) continue;
						std::ifstream input(prefix.string() + "." + std::to_string(depth));
						unsigned long pid = 0;
						if (!(input >> pid) || !pid) { complete = false; continue; }
						handles_[depth] = OpenProcess(SYNCHRONIZE, FALSE, pid);
						if (!handles_[depth]) return "Could not retain gated descendant PID " + std::to_string(pid) + "; error=" + std::to_string(GetLastError());
						pids_[depth] = pid;
						if (WaitForSingleObject(handles_[depth], 0) != WAIT_TIMEOUT) return "Descendant exited before the all-three startup barrier: " + std::to_string(pid);
					}
					if (complete) return {};
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
				return "All three descendant startup receipts did not arrive before the existing process deadline.";
			}
			std::string SettlementFailure() const {
				for (std::size_t depth = 0; depth < handles_.size(); ++depth) {
					if (!handles_[depth]) return "A descendant has no retained startup handle.";
					const auto waited = WaitForSingleObject(handles_[depth], 0);
					if (waited != WAIT_OBJECT_0) return "Retained descendant PID " + std::to_string(pids_[depth]) + " was not settled at Run return; wait=" + std::to_string(waited);
				}
				return {};
			}
		private:
			std::array<HANDLE, 3> handles_{};
			std::array<unsigned long, 3> pids_{};
		};
#endif
	}
	void RegisterOwnedProcessTests(TestRegistry& registry) {
		registry.Add("Engine.OwnedProcess.OutputExitEnvironmentAndUtf8", [](TestContext& context) {
			Correctness::DisposableFixturePath files;
			const std::string unicode = "UTF8-\xe9\x9b\xaa quote\" spaces";
			auto request = FixtureRequest({"--stdout", unicode, "--stderr", "stderr-contract", "--env", "TERRA_PROCESS_CHILD_ONLY", "--exit-code", "7"});
			const char* prior = std::getenv("TERRA_PROCESS_CHILD_ONLY"); const std::string priorValue = prior ? prior : ""; const bool hadPrior = prior != nullptr;
			request.environment["TERRA_PROCESS_CHILD_ONLY"] = unicode;
			request.workingDirectory = files.Child(std::filesystem::path(u8"working-\u96ea"));
			std::filesystem::create_directory(request.workingDirectory);
			const std::atomic_bool cancellation = false;
			const auto result = OwnedProcess::Run(request, cancellation);
			context.Expect(result.started && !result.canceled && !result.timedOut, "A successful child launch should retain its termination category.");
			context.ExpectEqual(7, result.exitCode, "Process capture should retain the child's exit status.");
			context.Expect(result.output.find(unicode) != std::string::npos && result.output.find("env:TERRA_PROCESS_CHILD_ONLY=" + unicode) != std::string::npos && result.output.find("stderr-contract") != std::string::npos, "Capture should preserve UTF-8 argv, child environment and both output streams.");
			const char* after = std::getenv("TERRA_PROCESS_CHILD_ONLY");
			context.Expect((after != nullptr) == hadPrior && (after ? after : "") == priorValue, "Environment overrides must never modify the parent.");
		});
		registry.Add("Engine.OwnedProcess.SuccessfulGateReleaseAndLargeOutput", [](TestContext& context) {
			Correctness::DisposableFixturePath files;
			const auto gate = files.Child("release");
			const auto prefix = files.Child("pid");
			std::ofstream(gate).put('\n');
			const std::string payload(20000, 'x');
			auto request = FixtureRequest({"--gate", Utf8Path(gate), "--spawn-depth", "2", "--pid-file", Utf8Path(prefix), "--stdout", payload});
			const std::atomic_bool cancellation = false;
			const auto result = OwnedProcess::Run(request, cancellation);
			context.Expect(result.started && !result.canceled && !result.timedOut && result.exitCode == 0 && result.output.find(payload) != std::string::npos, "Explicit released descendants and output larger than one pipe read should settle without capture loss.");
			const auto settlement = DescendantSettlementFailure(prefix);
			context.Expect(settlement.empty(), settlement);
		});
		registry.Add("Engine.OwnedProcess.LaunchFailureIsObservable", [](TestContext& context) {
			Correctness::DisposableFixturePath files;
			auto request = FixtureRequest({}); request.workingDirectory = files.Child("missing-directory");
			const std::atomic_bool cancellation = false;
			const auto result = OwnedProcess::Run(request, cancellation);
#ifdef _WIN32
			context.Expect(!result.started && !result.output.empty(), "Native launch failure should contain its diagnostic.");
#else
			context.Expect(result.started && result.exitCode == 127, "Portable failed child cwd should retain the shell's launch failure status.");
#endif
		});
		registry.Add("Engine.OwnedProcess.CanceledBeforeLaunch", [](TestContext& context) {
			const std::atomic_bool cancellation = true;
			const auto result = OwnedProcess::Run(FixtureRequest({}), cancellation);
			context.Expect(!result.started && result.canceled, "Cancellation before launch should create no child.");
		});
		registry.Add("Engine.OwnedProcess.CancellationReapsDescendants", [](TestContext& context) {
			Correctness::DisposableFixturePath files;
			const auto prefix = files.Child("pid");
			auto request = FixtureRequest({"--gate", Utf8Path(files.Child("never-release")), "--spawn-depth", "2", "--pid-file", Utf8Path(prefix), "--timeout-ms", "300000"});
			request.timeout = std::chrono::seconds(8);
			std::atomic_bool cancellation = false;
			auto& pool = ThreadPool::instance(); pool.shutdown(); pool.initialize(1);
			auto task = pool.createTask([request, &cancellation] { return OwnedProcess::Run(request, cancellation); });
			auto future = task->getFuture(); pool.enqueue(task);
#ifdef _WIN32
			RetainedDescendants descendants;
			const auto startup = descendants.AwaitStartup(prefix, Correctness::kDefaultTimeout);
			const bool arrived = startup.empty();
#else
			const bool arrived = WaitForFiles(prefix);
#endif
			cancellation.store(true);
			const bool ready = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
			const auto result = future.get();
#ifdef _WIN32
			const auto settlement = descendants.SettlementFailure();
#else
			const auto settlement = DescendantSettlementFailure(prefix);
#endif
			pool.shutdown();
			context.Expect(arrived && ready && result.started && result.canceled, "Cancellation should settle actual process capture after all descendants launch.");
			context.Expect(settlement.empty(), settlement);
		});
		registry.Add("Engine.OwnedProcess.TimeoutReapsDescendants", [](TestContext& context) {
			Correctness::DisposableFixturePath files;
			const auto prefix = files.Child("pid");
			auto request = FixtureRequest({"--gate", Utf8Path(files.Child("never-release")), "--spawn-depth", "2", "--pid-file", Utf8Path(prefix), "--timeout-ms", "300000"});
			request.timeout = std::chrono::seconds(2);
			const std::atomic_bool cancellation = false;
#ifdef _WIN32
			auto& pool = ThreadPool::instance(); pool.shutdown(); pool.initialize(1);
			auto task = pool.createTask([request, &cancellation] { return OwnedProcess::Run(request, cancellation); });
			auto future = task->getFuture(); pool.enqueue(task);
			RetainedDescendants descendants;
			// Startup must complete within the first half of the unchanged two-second
			// production deadline. The test cannot arm or extend OwnedProcess's clock.
			const auto startup = descendants.AwaitStartup(prefix, std::chrono::seconds(1));
			const auto result = future.get();
			const auto settlement = descendants.SettlementFailure();
			pool.shutdown();
			context.Expect(startup.empty(), startup);
#else
			const auto result = OwnedProcess::Run(request, cancellation);
			const auto settlement = DescendantSettlementFailure(prefix);
#endif
			context.Expect(result.started && result.timedOut && !result.canceled && result.exitCode == 124, "Timeout should retain its own terminal category.");
			context.Expect(settlement.empty(), settlement);
		});
	}
}
