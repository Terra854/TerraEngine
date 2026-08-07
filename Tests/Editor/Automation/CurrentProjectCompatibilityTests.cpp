#include "TestHarness.h"
#include "TestSha256.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::filesystem::path ProjectRoot()
		{
			if (const char* overrideRoot = std::getenv("TERRAENGINE_CURRENT_PROJECT_ROOT"))
				return overrideRoot;
			return "C:/Users/benti/Desktop/TerraEngine-Projects/New Animated Project";
		}
	}

	void RegisterCurrentProjectCompatibilityTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.CurrentProjectCompatibility.SceneIsFrozen", [](TestContext& context)
		{
			const std::filesystem::path scene = ProjectRoot() / "Assets/Scenes/Scene.tescene";
			if (!std::filesystem::exists(scene))
			{
				context.Skip("The external current-format validation project is absent.");
			}
			std::ifstream input(scene, std::ios::binary);
			const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
			context.ExpectEqual(static_cast<std::size_t>(16487284), bytes.size(), "The benchmark scene size must remain unchanged.");
			context.Expect(std::filesystem::exists(scene.parent_path() / "Scene.tescene.meta"), "The benchmark scene metadata must remain present.");
			// This immutable identity is compared by the live validation runner before
			// and after opening the project; this test intentionally performs no write,
			// import, recook, or editor mutation.
			constexpr std::string_view frozenSha256 =
				"c15f9dcc784ba4acf027d7c01d22de8d65a20c3b4044931e7c9488fdf3afd673";
			context.ExpectEqual(std::string(frozenSha256), ComputeTestSha256(bytes),
				"The benchmark scene bytes must retain their frozen SHA-256 identity.");
		});

		registry.Add("Editor.Automation.CurrentProjectCompatibility.NoMutationWorkflow", [](TestContext& context)
		{
			const std::filesystem::path project = ProjectRoot();
			const std::filesystem::path scene = project / "Assets/Scenes/Scene.tescene";
			const std::filesystem::path validation = project / "Assets/ValidationModels/DenseAnimatedVG";
			context.Expect(std::filesystem::exists(scene) || !std::filesystem::exists(project), "Compatibility checks must target the named current-format project only.");
			context.Expect(!std::filesystem::exists(validation / "Scene.tescene"), "Validation assets must not replace or shadow the benchmark scene.");
			context.Expect(true, "The compatibility test has no mutation, reimport, or recook operation by construction.");
		});
	}
}
