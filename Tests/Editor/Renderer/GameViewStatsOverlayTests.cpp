#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadGameViewWindowSource()
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

			const std::string path = "Editor/TerraEngine-Editor/Windows/GameViewWindow.cpp";
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
			const auto marker = thisFile.find("Tests\\Editor\\Renderer\\GameViewStatsOverlayTests.cpp");
			if (marker != std::string::npos)
			{
				return read(thisFile.substr(0u, marker) + path);
			}
			const auto slashMarker = thisFile.find("Tests/Editor/Renderer/GameViewStatsOverlayTests.cpp");
			if (slashMarker != std::string::npos)
			{
				return read(thisFile.substr(0u, slashMarker) + path);
			}
			return {};
		}
	}

	void RegisterGameViewStatsOverlayTests(TestRegistry& registry)
	{
		registry.Add("GameViewStatsOverlay.CompactRows.ExcludesRendererDiagnostics", [](TestContext& context) {
			const std::string source = ReadGameViewWindowSource();

			context.Expect(source.find("std::array<std::string, 5>") != std::string::npos, "Game View stats should be capped to a compact fixed row set.");
			context.Expect(source.find("FPS: %.1f") != std::string::npos, "Compact stats should keep the FPS row.");
			context.Expect(source.find("Frame: %.2f ms") != std::string::npos, "Compact stats should keep the frame-time row.");
			context.Expect(source.find("Draw Calls: %u") != std::string::npos, "Compact stats should keep the draw-call row.");
			context.Expect(source.find("Triangles: %u") != std::string::npos, "Compact stats should keep the triangle row.");
			context.Expect(source.find("\"Meshlets Selected\"") == std::string::npos, "Meshlet readbacks should not be drawn in Game View stats.");
			context.Expect(source.find("\"VG Cluster Packets\"") == std::string::npos, "VG packet counters should not be drawn in Game View stats.");
			context.Expect(source.find("\"VG Page Payload Bytes") == std::string::npos, "VG page byte counters should not be drawn in Game View stats.");
			context.Expect(source.find("\"Hi-Z Baseline\"") == std::string::npos, "Hi-Z validation counters should not be drawn in Game View stats.");
		});
	}
}
