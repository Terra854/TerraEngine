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

		std::string ReadBenchmarkSource()
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/Tests/Tools/Profiling/CaptureProfilerReworkBenchmark.ps1", std::ios::binary);
			std::ostringstream text;
			text << input.rdbuf();
			return text.str();
		}
	}

	void RegisterProfilerDiagnosticsAutomationTests(TestRegistry& registry)
	{
		registry.Add("ProfilerDiagnosticsAutomation.StructuredTimingAndVgStats.DoNotScrapeOverlays", [](TestContext& context) {
			const std::string source = ReadAutomationServiceSource();

			context.Expect(source.find("SerializeProfilerTimingStreams") != std::string::npos, "Detailed diagnostics should expose profiler timing streams structurally.");
			context.Expect(source.find("\"profilerTimingStreams\"") != std::string::npos, "Diagnostics summary should contain profiler timing stream data.");
			context.Expect(source.find("SerializeProfilerCaptureEvidence") != std::string::npos, "Detailed diagnostics should expose capture-level V1 evidence structurally.");
			context.Expect(source.find("\"profilerCaptureEvidence\"") != std::string::npos, "Diagnostics summary should contain V1 capture evidence.");
			context.Expect(source.find("\"protectedBoundaryTimings\"") != std::string::npos, "Diagnostics evidence should retain protected-boundary timing breakdowns.");
			context.Expect(source.find("\"workVolumes\"") != std::string::npos, "Diagnostics evidence should retain processed-versus-published body counts.");
			context.Expect(source.find("renderer.virtualGeometry.stats") != std::string::npos, "Automation should retain the structured virtual geometry stats tool.");
			context.Expect(source.find("\"meshletsSelected\"") != std::string::npos, "Virtual geometry automation should expose meshlet counters as fields.");
			context.Expect(source.find("\"pageResidency\"") != std::string::npos, "Virtual geometry automation should expose page residency as structured data.");
			context.Expect(source.find("\"animatedVirtualGeometry\"") != std::string::npos,
				"Direct virtual geometry stats must expose the bounded animated-page snapshot.");
			context.Expect(source.find("\"diagnosticCoalesced\"") != std::string::npos
					&& source.find("\"diagnosticDropped\"") != std::string::npos,
				"Animated-page automation must preserve bounded coalesced and dropped diagnostic counters.");
			context.Expect(source.find("\"deformableDemandedPagesAvailable\"") != std::string::npos
					&& source.find("\"deformableDemandedPageEntryCount\"") != std::string::npos
					&& source.find("\"deformableDemandedPageSectionSetCount\"") != std::string::npos,
				"Animation diagnostics must explain loaded-mesh page-demand eligibility without exposing page bytes.");
			context.Expect(source.find("\"animatedPageDemandCount\"") != std::string::npos,
				"Animation diagnostics must expose the immutable frame's resulting animated-page demand count.");
		});

		registry.Add("ProfilerDiagnosticsAutomation.SceneViewPerformanceState.IsAdditiveAndStructured", [](TestContext& context) {
			const std::string source = ReadAutomationServiceSource();

			context.Expect(source.find("scene_view.settings.get") != std::string::npos, "Automation should expose typed Scene View performance settings reads.");
			context.Expect(source.find("scene_view.settings.set") != std::string::npos, "Automation should expose atomic Scene View performance settings replacement.");
			context.Expect(source.find("configuredMode") != std::string::npos, "Scene View state should distinguish configured large-scene mode.");
			context.Expect(source.find("densityConsolidated") != std::string::npos, "Scene View state should expose density work as structured evidence.");
			context.Expect(source.find("presentationBatches") != std::string::npos, "Scene View state should report bounded presentation batches.");
			context.Expect(source.find("selectedExemptions") != std::string::npos, "Scene View state should prove selected objects were exempt from general budgets.");
		});

		registry.Add("ProfilerDiagnosticsAutomation.BenchmarkIdentity.PinsAllComparisonInputs", [](TestContext& context) {
			const std::string source = ReadBenchmarkSource();
			context.Expect(source.find("projectHash") != std::string::npos
				&& source.find("sceneHash") != std::string::npos
				&& source.find("objectPlacementHash") != std::string::npos
				&& source.find("workloadHash") != std::string::npos
				&& source.find("cameraHash") != std::string::npos
				&& source.find("viewportHash") != std::string::npos
				&& source.find("captureSettingsHash") != std::string::npos
				&& source.find("installedBuildHash") != std::string::npos
				&& source.find("durationSeconds") != std::string::npos
				&& source.find("TerraEngineProfilerJournalV1") != std::string::npos,
				"Profiler comparisons must pin project, scene, placement/workload, camera, viewport, build, settings, duration, and V1 format.");
		});
	}
}
