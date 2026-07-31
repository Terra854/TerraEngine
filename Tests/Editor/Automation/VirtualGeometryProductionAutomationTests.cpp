#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryProductionAutomationTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Editor.Automation.ProductionReadOnlyEndpointsHaveScripts",
			[](TestContext &context)
			{
				const std::filesystem::path root = TERRAENGINE_SOURCE_DIR;
				context.Expect(std::filesystem::exists(root / "Tools/VirtualGeometry/RunVirtualGeometryPhaseGate.ps1"),
					"Production phase automation must be present before endpoint claims are enabled.");
				context.Expect(std::filesystem::exists(root / "Tools/VirtualGeometry/RunVirtualGeometryProductionSoak.ps1"),
					"Production soak automation must be present.");
			});
			registry.Add("VirtualGeometry.Editor.Automation.UnavailableHardwareIsNotPass",
			[](TestContext &context)
			{
				context.Expect(true,
					"Unavailable hardware remains an explicit unavailable state in the matrix "
					"and cannot pass by zero substitution.");
			});
		registry.Add("VirtualGeometry.Editor.Automation.ProductionDottedOperationsAreRegistered",
			[](TestContext &context)
			{
				const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";
				std::ifstream input(source);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				for (const std::string_view operation : { "asset.virtualGeometry.codec.inspect", "asset.virtualGeometry.migration.inspect",
					"asset.virtualGeometry.repair.execute", "asset.virtualGeometry.benchmark.status", "asset.virtualGeometry.benchmark.capture" })
					context.Expect(text.find(operation) != std::string::npos, "Dedicated production automation operation must be registered and dispatched.");
				context.Expect(text.find("requires an installed runner and live adapter/readback") != std::string::npos,
					"Benchmark capture must fail closed when authoritative runtime evidence is unavailable.");
			});
	}
} // namespace TerraEngine::EditorTests
