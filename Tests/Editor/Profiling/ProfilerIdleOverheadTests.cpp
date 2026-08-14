#include "TestHarness.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadSource(const char* relativePath)
		{
			std::ifstream input(std::string(TERRAENGINE_SOURCE_DIR) + "/" + relativePath, std::ios::binary);
			std::ostringstream output;
			output << input.rdbuf();
			return output.str();
		}
	}

	void RegisterProfilerIdleOverheadTests(TestRegistry& registry)
	{
		registry.Add("Editor.ProfilerIdleOverhead.MedianAndP95Gate", [](TestContext& context) {
			const std::string window = ReadSource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			const std::string runner = ReadSource("Tests/Tools/Profiling/MeasureProfilerIdleOverhead.ps1");
			context.Expect(window.find("Timing Distribution") != std::string::npos
				&& window.find("ProfilerWindowRefreshInterval") != std::string::npos,
				"Idle overhead measurements must use cached, throttled timing distribution updates.");
			context.Expect(runner.find("medianDeltaPercent") != std::string::npos
				&& runner.find("p95DeltaPercent") != std::string::npos
				&& runner.find("median-of-paired-median-and-p95-deltas") != std::string::npos
				&& runner.find("$aggregateP95DeltaPercent -le 2.0") != std::string::npos
				&& runner.find("Abs($aggregateP95DeltaPercent)") == std::string::npos,
				"The installed idle-overhead gate must independently enforce one-sided median and p95 overhead caps.");
			context.Expect(runner.find("ScenePath = 'Assets/Scenes/Scene.tescene'") != std::string::npos
				&& runner.find("PrefabInstanceCount = 0") != std::string::npos
				&& runner.find("ValidationCameraPosition = @(0.0, 1.0, 9.0)") != std::string::npos
				&& runner.find("ValidationCameraTarget = @(0.0, 1.0, 8.0)") != std::string::npos
				&& runner.find("authored-scene-exact-placement") != std::string::npos,
				"Idle-overhead evidence must preserve the canonical authored scene, population, and camera identity.");
		});
		registry.Add("Editor.ProfilerIdleOverhead.UsesImmutableSnapshotsWhenIdle", [](TestContext& context)
		{
			const std::string window = ReadSource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			context.Expect(window.find("presentation->indexPage") != std::string::npos, "Idle profiler presentation must read one bounded V1 index page.");
			context.Expect(window.find("service->RequestSelectedFrame") != std::string::npos, "Selected-frame presentation must request one immutable V1 detail record.");
			context.Expect(window.find("service->GetPresentationRevision()") != std::string::npos
				&& window.find("if (presentationChanged)") != std::string::npos
				&& window.find("cache->presentationRevision = presentation->presentationRevision") != std::string::npos,
				"Idle refresh must gate immutable snapshot retention on the service revision rather than rebuilding history every draw.");
			context.Expect(window.find("cache->hasActiveProject = presentation->hasActiveProject") != std::string::npos
				&& window.find("cache->hasActiveOperation = std::any_of") != std::string::npos
				&& window.find("service->HasActiveProject()") == std::string::npos
				&& window.find("service->IsBusy()") == std::string::npos,
				"Open-idle toolbar state must reuse immutable publication fields instead of taking service locks every frame.");
			context.Expect(window.find("kProfilerWindowRefreshInterval") != std::string::npos, "Profiler refresh must remain throttled while the window is open and idle.");
			context.Expect(window.find("captureClipper.Begin") != std::string::npos,
				"The idle capture browser must submit only visible saved-session rows.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.SelectedSessionDefersDeepRefresh", [](TestContext& context)
		{
			const std::string service = ReadSource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
			context.Expect(service.find("ReadJournalIndex") != std::string::npos && service.find("ReadSelectedJournalFrame") != std::string::npos, "Selecting a completed session must open the V1 index and later load one selected detail record.");
			context.Expect(service.find("ProfilerCaptureStore") != std::string::npos, "Session selection must use the capture store rather than live telemetry collection.");
			context.Expect(service.find("Opening profiler capture") != std::string::npos && service.find("Loaded capture") != std::string::npos, "Selected-session loading must remain an explicit completed-capture transition.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.UsesEngineCpuWithoutRecording", [](TestContext& context)
		{
			const std::string automation = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::string engine = ReadSource("Engine/Core/MainEngine.cpp");
			const std::string scheduler = ReadSource("Engine/Core/Scheduling/SystemScheduler.cpp");
			const std::string harness = ReadSource("Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			context.Expect(automation.find("engineCpuTimeMs") != std::string::npos
				&& engine.find("BeginLatestEngineFrameTiming") != std::string::npos
				&& engine.find("PublishLatestEngineFrameTiming") != std::string::npos,
				"Idle-overhead automation must expose Engine::Update timing without requiring an active capture.");
			context.Expect(scheduler.find("{ \"EditorProfilerTiming\"") != std::string::npos,
				"The profiler timing resource must be registered so installed editor startup validation succeeds.");
			context.Expect(harness.find("profilerState = 'idle'") != std::string::npos
				&& harness.find("timingSource = 'Engine::Update editor-excluded wall duration'") != std::string::npos
				&& harness.find("hostFrameTiming") != std::string::npos,
				"The installed harness must gate Engine CPU while retaining host-frame timing as diagnostic context.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.AttributesWindowCpuWorkSeparately", [](TestContext& context)
		{
			const std::string window = ReadSource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			const std::string service = ReadSource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
			const std::string automation = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::string harness = ReadSource("Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			context.Expect(window.find("ScopedProfilerWindowDrawTiming") != std::string::npos
				&& window.find("PublishProfilerWindowDrawTiming") != std::string::npos,
				"ProfilerWindow should publish its own CPU duration independently of later editor rendering stages.");
			context.Expect(service.find("profilerWindowDrawMs.store") != std::string::npos
				&& service.find("ClearProfilerWindowDrawTiming") != std::string::npos
				&& service.find("profilerWindowDrawValid") != std::string::npos,
				"The profiler service should retain a lock-free typed last-draw snapshot for automation.");
			context.Expect(automation.find("profilerWindowDrawMs") != std::string::npos
				&& harness.find("profilerWindowDrawTiming") != std::string::npos,
				"The visibility control and installed harness should expose the window CPU timing beside host-frame delta evidence.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.AttributesPostWindowPresentationStages", [](TestContext& context)
		{
			const std::string service = ReadSource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
			const std::string editorSystem = ReadSource("Editor/TerraEngine-Editor/Core/EditorSystem.cpp");
			const std::string automation = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::string harness = ReadSource("Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			context.Expect(service.find("PublishImGuiRenderTiming") != std::string::npos
				&& service.find("PublishPresentUiTiming") != std::string::npos,
				"Profiler timing service should retain typed post-window stage measurements.");
			context.Expect(editorSystem.find("ImGui::Render") != std::string::npos
				&& editorSystem.find("PublishImGuiRenderTiming") != std::string::npos
				&& editorSystem.find("PublishPresentUiTiming") != std::string::npos,
				"EditorSystem should publish draw-data and PresentEditor durations at their real execution boundaries.");
			context.Expect(automation.find("imguiRenderMs") != std::string::npos
				&& automation.find("presentUiMs") != std::string::npos
				&& harness.find("imguiRenderTiming") != std::string::npos
				&& harness.find("presentUiTiming") != std::string::npos,
				"Idle-overhead evidence should expose both post-window stages instead of attributing all residual time to OnGui.");
		});
	}
}
