#include "TestHarness.h"

#include "BuildProfile.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	void RegisterStreamingPolicyExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Export.Streaming.PolicyNotStoredInBuildProfile", [](TestContext& context) {
			BuildProfile profile;
			profile.name = "Streaming";
			profile.id = "streaming-test";
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineStreamingProfileTest.json";

			context.Expect(SaveBuildProfile(profile, path), "Build profiles should still save export-only settings.");
			nlohmann::json savedProfile;
			{
				std::ifstream input(path);
				input >> savedProfile;
			}
			std::filesystem::remove(path);

			context.Expect(!savedProfile.contains("streamingEnabled"), "Build profiles should not own visibility streaming enablement.");
			context.Expect(!savedProfile.contains("streamingDeferScenePayloadsOnLoad"), "Build profiles should not own scene payload deferral.");
			context.Expect(!savedProfile.contains("streamingDistance"), "Build profiles should not own streaming distance.");
			context.Expect(!savedProfile.contains("streamingVisibleRetentionSeconds"), "Build profiles should not own visible retention.");
			context.Expect(!savedProfile.contains("streamingNeededRetentionSeconds"), "Build profiles should not own needed retention.");
			context.Expect(!savedProfile.contains("streamingMaxLoadsPerUpdate"), "Build profiles should not own streaming load budgets.");
			context.Expect(!savedProfile.contains("streamingMemoryBudgetInteraction"), "Build profiles should not own streaming memory interaction.");
		});
	}
}
