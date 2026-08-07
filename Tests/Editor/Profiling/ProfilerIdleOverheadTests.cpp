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
		registry.Add("Editor.ProfilerIdleOverhead.UsesImmutableSnapshotsWhenIdle", [](TestContext& context)
		{
			const std::string window = ReadSource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			context.Expect(window.find("GetFrameListSnapshot") != std::string::npos, "Idle profiler presentation must read a bounded frame-list snapshot.");
			context.Expect(window.find("GetSelectedFrameSnapshot") != std::string::npos, "Selected-frame presentation must use an immutable profiler snapshot.");
			context.Expect(window.find("GetSnapshotRevision") != std::string::npos, "Idle refresh must be revision-driven rather than rebuilding history every draw.");
			context.Expect(window.find("kProfilerWindowRefreshInterval") != std::string::npos, "Profiler refresh must remain throttled while the window is open and idle.");
			context.Expect(window.find("captureClipper.Begin") != std::string::npos,
				"The idle capture browser must submit only visible saved-session rows.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.SelectedSessionDefersDeepRefresh", [](TestContext& context)
		{
			const std::string service = ReadSource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
			context.Expect(service.find("LoadDocument") != std::string::npos, "Selecting a completed session must load an immutable document snapshot.");
			context.Expect(service.find("ProfilerCaptureStore") != std::string::npos, "Session selection must use the capture store rather than live telemetry collection.");
			context.Expect(service.find("Opening profiler capture") != std::string::npos && service.find("Loaded capture") != std::string::npos, "Selected-session loading must remain an explicit completed-capture transition.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.UsesEditorWallDeltaWithoutRecording", [](TestContext& context)
		{
			const std::string automation = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::string editorSystem = ReadSource("Editor/TerraEngine-Editor/Core/EditorSystem.cpp");
			const std::string scheduler = ReadSource("Engine/Core/Scheduling/SystemScheduler.cpp");
			const std::string harness = ReadSource("Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			context.Expect(automation.find("editorFrameTimeMs") != std::string::npos
				&& editorSystem.find("ImGui::GetIO().DeltaTime") != std::string::npos
				&& editorSystem.find("EditorProfilerTiming") != std::string::npos,
				"Idle-overhead automation must expose the editor host wall delta rather than game-only profiler timing.");
			context.Expect(scheduler.find("{ \"EditorProfilerTiming\"") != std::string::npos,
				"The profiler timing resource must be registered so installed editor startup validation succeeds.");
			context.Expect(harness.find("profilerState = 'idle'") != std::string::npos
				&& harness.find("timingSource = 'ImGuiIO.DeltaTime'") != std::string::npos,
				"The installed harness must measure the window while profiler recording remains idle.");
		});

		registry.Add("Editor.ProfilerIdleOverhead.AttributesWindowCpuWorkSeparately", [](TestContext& context)
		{
			const std::string window = ReadSource("Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
			const std::string service = ReadSource("Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
			const std::string automation = ReadSource("Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			const std::string harness = ReadSource("Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
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
			const std::string harness = ReadSource("Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
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
