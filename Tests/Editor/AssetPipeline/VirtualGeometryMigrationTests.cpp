#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryMigrationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Editor.AssetPipeline.Migration.RejectsMissingLegacyArtifact",
			[](TestContext &c)
			{
				// Migration inputs are repository contracts, so resolve
				// them independently of CTest's build-tree working folder.
				const auto p = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tools/VirtualGeometry/legacy-virtual-geometry.json";
				c.Expect(!std::filesystem::exists(p), "Missing legacy artifact must remain unavailable, not appear migrated.");
			});
		registry.Add("VirtualGeometry.Editor.AssetPipeline.Migration.RepairManifestIsReadOnlyContract",
			[](TestContext &c)
			{
				const auto path = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tools/VirtualGeometry/PhaseScenarios/phase-9-production.json";
				std::ifstream in(path);
				c.Expect(static_cast<bool>(in), "The installed migration scenario source manifest must exist.");
				if (!in)
				{
					return;
				}
				nlohmann::json j;
				in >> j;
				c.Expect(j.value("unavailablePolicy", "") == "fail-closed", "Repair/migration scenarios must fail closed when provenance is unavailable.");
			});
	}
} // namespace TerraEngine::EditorTests
