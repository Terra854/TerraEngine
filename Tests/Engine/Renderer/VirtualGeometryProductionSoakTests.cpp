#include "TestHarness.h"
#include <filesystem>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iterator>
#include <string>
namespace TerraEngine::Tests
{
	namespace
	{
		// This reader validates the evidence document contract only. It cannot
		// establish that the native runner actually measured the supplied facts.
		// Installed soak execution and its raw receipts remain separate acceptance.
		bool IsCompleteSoakDocument(const nlohmann::json& value)
		{
			if(!value.is_object()) return false;
			const auto isTrue = [&value](const char* key) {
				const auto found = value.find(key);
				return found != value.end() && found->is_boolean() && found->get<bool>();
			};
			const auto status = value.find("status");
			const auto testRun = value.find("testRun");
			const auto exitCode = value.find("runnerExitCode");
			const auto samples = value.find("measuredSampleCount");
			return status != value.end() && status->is_string()
				&& status->get<std::string>() == "measured-result-published"
				&& isTrue("boundedCounters") && isTrue("gateDurationSatisfied")
				&& isTrue("runtimeDurationSatisfied")
				&& testRun != value.end() && testRun->is_boolean() && !testRun->get<bool>()
				&& exitCode != value.end() && exitCode->is_number_integer() && *exitCode == 0
				&& samples != value.end() && samples->is_number_integer() && *samples > 0;
		}
	}

	void RegisterVirtualGeometryProductionSoakTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Evidence.ProductionSoakDocumentContractRejectsMissingOrIncomplete",
			[](TestContext &c)
			{
				// These are deliberately synthetic protocol inputs, never a saved
				// production result or proof of Vulkan/30-minute soak completion.
				const nlohmann::json synthetic = {
					{"status", "measured-result-published"}, {"boundedCounters", true},
					{"gateDurationSatisfied", true}, {"runtimeDurationSatisfied", true},
					{"testRun", false}, {"runnerExitCode", 0}, {"measuredSampleCount", 1}};
				c.Expect(IsCompleteSoakDocument(synthetic), "A synthetic complete document must satisfy the parser contract only.");
				c.Expect(!IsCompleteSoakDocument(nullptr), "Missing evidence must be rejected.");
				c.Expect(!IsCompleteSoakDocument(nlohmann::json::parse("{", nullptr, false)), "Malformed evidence must be rejected.");
				for(const auto& item : synthetic.items())
				{
					auto missing = synthetic;
					missing.erase(item.key());
					c.Expect(!IsCompleteSoakDocument(missing), "Every required evidence field must be present.");
				}
				for(const char* field : {"boundedCounters", "gateDurationSatisfied", "runtimeDurationSatisfied"})
				{
					auto invalid = synthetic;
					invalid[field] = false;
					c.Expect(!IsCompleteSoakDocument(invalid), "False bounds or duration must not satisfy the contract.");
					invalid[field] = "true";
					c.Expect(!IsCompleteSoakDocument(invalid), "Text cannot substitute for a measured boolean.");
				}
				for(const auto& change : nlohmann::json::array({
					nlohmann::json{{"status", "incomplete"}}, nlohmann::json{{"testRun", true}},
					nlohmann::json{{"runnerExitCode", 2}}, nlohmann::json{{"measuredSampleCount", 0}},
					nlohmann::json{{"measuredSampleCount", -1}}}))
				{
					auto invalid = synthetic;
					invalid.update(change);
					c.Expect(!IsCompleteSoakDocument(invalid), "Incomplete, test-only, failed or empty runs must be rejected.");
				}
			});
		registry.Add("VirtualGeometry.Engine.ProductionSoak.RequiredDurationIsThirtyMinutes",
			[](TestContext &c)
			{
				const auto script = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Tests/Tools/VirtualGeometry/RunVirtualGeometryProductionSoak.ps1";
				std::ifstream input(script);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				c.Expect(text.find("30") != std::string::npos && text.find("bounded") != std::string::npos,
					"Production soak orchestration must require a bounded 30-minute run before acceptance.");
			});
	}
} // namespace TerraEngine::Tests
