#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadAutomationServiceSource()
		{
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};

			const std::string path = "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";
			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			const std::string thisFile = __FILE__;
			const auto marker = thisFile.find("Tests\\Editor\\Automation\\ProfilerDiagnosticsAutomationTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Automation/ProfilerDiagnosticsAutomationTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}
	}

	void RegisterProfilerDiagnosticsAutomationTests(TestRegistry& registry)
	{
		registry.Add("ProfilerDiagnosticsAutomation.StructuredTimingAndVgStats.DoNotScrapeOverlays", [](TestContext& context) {
			const std::string source = ReadAutomationServiceSource();

			context.Expect(source.find("SerializeProfilerTimingStreams") != std::string::npos, "Detailed diagnostics should expose profiler timing streams structurally.");
			context.Expect(source.find("\"profilerTimingStreams\"") != std::string::npos, "Diagnostics summary should contain profiler timing stream data.");
			context.Expect(source.find("renderer.virtualGeometry.stats") != std::string::npos, "Automation should retain the structured virtual geometry stats tool.");
			context.Expect(source.find("\"meshletsSelected\"") != std::string::npos, "Virtual geometry automation should expose meshlet counters as fields.");
			context.Expect(source.find("\"pageResidency\"") != std::string::npos, "Virtual geometry automation should expose page residency as structured data.");
		});
	}
}
