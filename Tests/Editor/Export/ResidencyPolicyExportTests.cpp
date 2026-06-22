#include "TestHarness.h"

#include "BuildProfile.h"

#include <filesystem>

namespace TerraEngine::EditorTests {
	void RegisterResidencyPolicyExportTests(TestRegistry& registry)
	{
		registry.Add("Editor.Export.Residency.ProfileFieldsRoundTrip", [](TestContext& context) {
			BuildProfile profile;
			profile.name = "Residency";
			profile.id = "residency-test";
			profile.residencyCleanupEnabled = false;
			profile.residencyIdleSecondsThreshold = 12.25;
			profile.residencyMemoryBudgetMb = 456u;
			profile.residencyCleanupIntervalSeconds = 1.5;
			profile.residencyReloadGraceSeconds = 0.75;
			profile.residencyStrictBudgetMode = true;
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineResidencyProfileTest.json";

			context.Expect(SaveBuildProfile(profile, path), "Build profiles should save residency policy fields.");
			const BuildProfile loaded = LoadBuildProfile(path);
			std::filesystem::remove(path);

			context.ExpectEqual(false, loaded.residencyCleanupEnabled, "Cleanup enable should round trip.");
			context.ExpectEqual(12.25, loaded.residencyIdleSecondsThreshold, "Idle threshold should round trip in seconds.");
			context.ExpectEqual(std::uint64_t{456u}, loaded.residencyMemoryBudgetMb, "Memory budget should round trip in MB.");
			context.ExpectEqual(1.5, loaded.residencyCleanupIntervalSeconds, "Cleanup interval should round trip in seconds.");
			context.ExpectEqual(0.75, loaded.residencyReloadGraceSeconds, "Reload grace should round trip in seconds.");
			context.Expect(loaded.residencyStrictBudgetMode, "Strict budget mode should round trip.");
		});
	}
}
