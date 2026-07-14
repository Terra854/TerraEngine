#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	void RegisterEditorPostFrameBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Editor.PostFrameBoundary.IsSchedulerOwnedAndProtected", [](TestContext& context) {
			auto read = [](const std::filesystem::path& path) {
				std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR) / path);
				std::ostringstream source; source << input.rdbuf(); return source.str();
			};
			const std::string editor = read("Editor/TerraEngine-Editor/Core/EditorSystem.cpp");
			const std::string engine = read("Engine/Core/MainEngine.cpp");
			context.Expect(editor.find("declaration.phase = \"PostFrame\"") != std::string::npos, "EditorSystem must declare PostFrame.");
			context.Expect(editor.find("HardBoundaryClassification::Protected") != std::string::npos, "ImGui/Vulkan work must remain protected.");
			context.Expect(engine.find("systemScheduler.RegisterSystem(*editorSystem") != std::string::npos, "The scheduler must own editor frame execution.");
			context.Expect(engine.find("editorSystem->Update(deltaTime)") == std::string::npos, "Manual post-scheduler editor execution must be removed.");
			// An armed capture must use the runtime lifecycle state at the engine
			// boundary instead of relying on editor-window visibility or button state.
			context.Expect(engine.find("const bool playModeFrame = ScriptingSystem::Instance().IsRunning()") != std::string::npos,
				"Engine profiling must derive capture eligibility from active Play Mode.");
			context.Expect(engine.find("profiler.BeginFrame(playModeFrame)") != std::string::npos,
				"The main loop must pass Play Mode eligibility into every armed profiler frame boundary.");
		});
	}
}
