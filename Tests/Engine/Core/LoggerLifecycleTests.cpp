// Logger shutdown is process-global. Each lifecycle body runs in its own owned
// CPU child so a pre-fix stopped consumer never poisons other registered tests.
#include "TestHarness.h"
#include "Tests/Support/ProcessCommandLine.h"
#include "Core/Logger/Logger.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string_view>
#include <string>
#include <thread>
#include <vector>

namespace TerraEngine::Tests {
	// The friend only copies queue length under the production queue mutex.
	// Calls occur after Shutdown has joined; no thread/lifecycle fields are read.
	struct LoggerLifecycleTestAccess {
		static std::size_t QueuedEntries(Logger& logger) {
			std::lock_guard<std::mutex> lock(logger.m);
			return logger.q.size();
		}
	};
	namespace {
		constexpr const char* kLoggerChild = "TERRAENGINE_LOGGER_LIFECYCLE_CHILD";
		std::string Marker(unsigned cycle) { return "LOGGER_REUSE_DELIVERY_73149_CYCLE_" + std::to_string(cycle) + "_END"; }
		void ChildLifecycle(TestContext& context, unsigned cycles) {
			auto& logger = Logger::Instance();
			// Joining settles the ordinary before-shutdown positive control without
			// timing sleeps. Synchronous sinks alone would hide this console defect.
			logger.Log(LogLevel::Info, __FILE__, __LINE__, "LoggerLifecycle", Marker(0));
			logger.Shutdown();
			context.Expect(LoggerLifecycleTestAccess::QueuedEntries(logger) == 0, "Ordinary joined shutdown must drain its accepted entries.");
			std::size_t maximumOrphans = 0;
			// Independent terminal producers share the real console path. Each owns
			// disjoint markers, so the parent can verify complete, noninterleaved lines.
			std::vector<std::thread> producers;
			for(unsigned producer = 0; producer < 4U; ++producer) {
				producers.emplace_back([&logger, producer, cycles] {
					for(unsigned cycle = producer + 1U; cycle <= cycles; cycle += 4U) {
						logger.Shutdown();
						logger.Log(LogLevel::Info, __FILE__, __LINE__, "LoggerLifecycle", Marker(cycle));
						logger.Shutdown();
					}
				});
			}
			for(auto& producer : producers) producer.join();
			maximumOrphans = LoggerLifecycleTestAccess::QueuedEntries(logger);
			context.Expect(maximumOrphans == 0, "Settled reuse must not retain orphan queue entries; actual maximum=" + std::to_string(maximumOrphans));
			// The whole process owns any pre-fix orphan entries. No logger reference,
			// sink callback or stopped global consumer escapes back to the parent.
		}
		std::string QuoteExecutable(const std::filesystem::path& executable) {
			return QuoteProcessPath(executable);
		}
		std::size_t Occurrences(std::string_view output, std::string_view marker) {
			std::size_t count = 0, position = 0;
			while((position = output.find(marker, position)) != std::string_view::npos) { ++count; position += marker.size(); }
			return count;
		}
		void RunLifecycle(TestContext& context, const std::string& name, unsigned cycles) {
			const char* child = std::getenv(kLoggerChild);
			if(child && name == child) { ChildLifecycle(context, cycles); return; }
			const auto executable = PlatformUtils::GetExecutablePath();
			Utilities::OwnedProcessRequest request;
			request.commandLine = QuoteExecutable(executable);
			request.workingDirectory = executable.parent_path(); request.timeout = std::chrono::seconds(15);
			request.environment = {{kLoggerChild, name}, {"TERRAENGINE_TEST_FILTER", name}, {"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
			const std::atomic_bool canceled{false};
			const auto result = Utilities::OwnedProcess::Run(request, canceled);
			context.Expect(result.started && !result.timedOut && !result.canceled && result.exitCode == 0,
				"Owned CPU logger child must settle normally: " + result.output);
			for(unsigned cycle = 0; cycle <= cycles; ++cycle) {
				context.Expect(Occurrences(result.output, Marker(cycle)) == 1,
					"Actual logger console must deliver exactly once after each settled shutdown; missing/duplicate " + Marker(cycle) + ": " + result.output);
				const auto position = result.output.find(Marker(cycle));
				const auto previousNewline = result.output.rfind('\n', position);
				const auto begin = previousNewline == std::string::npos ? 0U : previousNewline + 1U;
				const auto end = result.output.find('\n', position);
				const auto line = std::string_view(result.output).substr(begin, end - begin);
				context.Expect(Occurrences(line, "LOGGER_REUSE_DELIVERY_") == 1
					&& Occurrences(line, "[LoggerLifecycle:") == 1,
					"Concurrent console publication must retain one complete message and prefix per line.");
			}
		}
	}
	void RegisterLoggerLifecycleTests(TestRegistry& registry) {
		for(const auto cycles : {1U, 32U}) {
			const std::string name = cycles == 1 ? "Engine.LoggerLifecycle.PostShutdownConsoleDelivery" : "Engine.LoggerLifecycle.RepeatedShutdownReuseIsBounded";
			registry.Add(name, [name, cycles](TestContext& context) { RunLifecycle(context, name, cycles); });
		}
	}
}
