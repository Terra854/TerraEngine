#include "TestHarness.h"

#include "BuildProfile.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests {
	void RegisterResidencyPolicyExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Export.Residency.PolicyNotStoredInBuildProfile", [](TestContext& context) {
			BuildProfile profile;
			profile.name = "Residency";
			profile.id = "residency-test";
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineResidencyProfileTest.json";

			context.Expect(SaveBuildProfile(profile, path), "Build profiles should still save export-only settings.");
			nlohmann::json savedProfile;
			{
				std::ifstream input(path);
				input >> savedProfile;
			}
			std::filesystem::remove(path);

			context.Expect(!savedProfile.contains("residencyCleanupEnabled"), "Build profiles should not own loaded-resource cleanup.");
			context.Expect(!savedProfile.contains("residencyIdleSecondsThreshold"), "Build profiles should not own residency idle thresholds.");
			context.Expect(!savedProfile.contains("residencyMemoryBudgetMb"), "Build profiles should not own residency memory budgets.");
			context.Expect(!savedProfile.contains("residencyCleanupIntervalSeconds"), "Build profiles should not own cleanup cadence.");
			context.Expect(!savedProfile.contains("residencyReloadGraceSeconds"), "Build profiles should not own reload grace.");
			context.Expect(!savedProfile.contains("residencyStrictBudgetMode"), "Build profiles should not own strict residency budgeting.");
		});
	}
}
