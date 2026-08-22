#include "../TestHarness.h"
#include "EditorRuntimeLayout.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class ScopedRuntimeFixture
		{
		public:
			ScopedRuntimeFixture()
			{
				const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
				root = std::filesystem::temp_directory_path()
					/ ("terra-editor-runtime-layout-" + std::to_string(suffix));
				std::filesystem::create_directories(root);
			}

			~ScopedRuntimeFixture()
			{
				std::error_code errorCode;
				std::filesystem::remove_all(root, errorCode);
			}

			void Complete(const std::filesystem::path& directory)
			{
				// Tiny placeholder files are sufficient because layout resolution checks
				// ownership and presence; shader parsing and managed hosting are tested elsewhere.
				for(const std::filesystem::path relative : {
					std::filesystem::path{"Shaders/Default.mesh.tspf"},
					std::filesystem::path{"Shaders/Default.frag.tspf"},
					std::filesystem::path{"Managed/TerraEngine.Editor.runtimeconfig.json"},
					std::filesystem::path{"Project/Templates/Assembly-CSharp.csproj.in"}})
				{
					std::filesystem::create_directories((directory / relative).parent_path());
					std::ofstream(directory / relative) << "fixture";
				}
			}

			std::filesystem::path root;
		};
	}

	void RegisterEditorRuntimeLayoutTests(TestRegistry& registry)
	{
		registry.Add("Editor.RuntimeLayout.KeepsCompleteExecutableDirectory", [](TestContext& context) {
			ScopedRuntimeFixture fixture;
			const std::filesystem::path executable = fixture.root / "Editor";
			fixture.Complete(executable);
			const auto layout = Editor::ResolveEditorRuntimeLayout(executable, fixture.root);
			context.Expect(layout.complete && !layout.redirected
				&& layout.runtimeDirectory == executable,
				"A staged editor must use the complete runtime beside its executable.");
		});

		registry.Add("Editor.RuntimeLayout.RedirectsRawCMakeExecutable", [](TestContext& context) {
			ScopedRuntimeFixture fixture;
			const std::filesystem::path rawOutput = fixture.root / "RelWithDebInfo";
			const std::filesystem::path staged = rawOutput / "Build" / "Editor";
			std::filesystem::create_directories(rawOutput);
			fixture.Complete(staged);
			const auto layout = Editor::ResolveEditorRuntimeLayout(rawOutput, fixture.root);
			context.Expect(layout.complete && layout.redirected
				&& layout.runtimeDirectory == staged,
				"A raw CMake editor must redirect resource lookup to Build/Editor.");
		});

		registry.Add("Editor.RuntimeLayout.RejectsIncompleteRuntime", [](TestContext& context) {
			ScopedRuntimeFixture fixture;
			const std::filesystem::path rawOutput = fixture.root / "RelWithDebInfo";
			std::filesystem::create_directories(rawOutput / "Build" / "Editor" / "Shaders");
			const auto layout = Editor::ResolveEditorRuntimeLayout(rawOutput, fixture.root);
			context.Expect(!layout.complete && !layout.diagnostic.empty(),
				"An incomplete runtime must fail before opening a renderless editor window.");
		});
	}
}
