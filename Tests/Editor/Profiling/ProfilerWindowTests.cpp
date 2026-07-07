#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadProfilerWindowSource()
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

			const std::string path = "Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp";
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
			const auto marker = thisFile.find("Tests\\Editor\\Profiling\\ProfilerWindowTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Profiling/ProfilerWindowTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}

		std::string ReadProfilerSource()
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

			const std::string path = "Engine/Profiling/Profiler.cpp";
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
			const auto marker = thisFile.find("Tests\\Editor\\Profiling\\ProfilerWindowTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Profiling/ProfilerWindowTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}
	}

	void RegisterProfilerWindowTests(TestRegistry& registry)
	{
		registry.Add("ProfilerWindow.Toolbar.GroupsActionsAndExplainsDisabledStates", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();

			context.Expect(source.find("DrawToolbarGroupLabel(\"Live\")") != std::string::npos, "Profiler toolbar should group live capture controls.");
			context.Expect(source.find("DrawToolbarGroupLabel(\"Captures\")") != std::string::npos, "Profiler toolbar should group capture-management controls.");
			context.Expect(source.find("DrawToolbarGroupLabel(\"Status\")") != std::string::npos, "Profiler toolbar should expose state separately from actions.");
			context.Expect(source.find("DrawDisabledReasonTooltip") != std::string::npos, "Disabled toolbar actions should explain why they are unavailable.");
			context.Expect(source.find("ToolbarButton(\"Clear\"") != std::string::npos, "Destructive clear action should remain separate from live capture controls.");
		});

		registry.Add("ProfilerWindow.TimingStreams.SeparateProfilerTimingFromRendererDiagnostics", [](TestContext& context) {
			const std::string source = ReadProfilerWindowSource();
			const std::string profilerSource = ReadProfilerSource();

			context.Expect(source.find("ProfilerTimingStreams") != std::string::npos, "Frame details should show timing streams.");
			context.Expect(source.find("BuildTimingStreamAggregates") != std::string::npos, "Profiler UI should use capture-level timing aggregate helpers.");
			context.Expect(source.find("TimingAggregateValue") != std::string::npos && source.find("return \"n/a\"") != std::string::npos, "Unavailable timing aggregates should display n/a instead of fake zeroes.");
			context.Expect(profilerSource.find("Viewport GPU") != std::string::npos && profilerSource.find("viewport_gpu") != std::string::npos, "GPU timing streams should be consolidated into one viewport row.");
			context.Expect(source.find("ProfilerVirtualGeometryPacketRows") == std::string::npos, "Profiler should not duplicate VG packet tables owned by Resource Residency.");
			context.Expect(source.find("ProfilerVirtualGeometryPageSourceRows") == std::string::npos, "Profiler should not duplicate VG page-source tables owned by Resource Residency.");
		});
	}
}
