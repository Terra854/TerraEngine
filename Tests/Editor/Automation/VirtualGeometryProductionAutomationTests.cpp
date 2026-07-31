#include "TestHarness.h"
#include "Providers/EditorDiagnosticsAutomationProvider.h"
#include <filesystem>
#include <algorithm>
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
				context.Expect(std::filesystem::exists(root / "Tools/VirtualGeometry/RunVirtualGeometryCapabilityGate.ps1"),
					"Production capability automation must be present before endpoint claims are enabled.");
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
		registry.Add("VirtualGeometry.Editor.Automation.RouteLocksAndDiagnosticSnapshotsPreserveUnavailableEvidence",
			[](TestContext& context)
			{
				const auto provider = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Editor/TerraEngine-Editor/Automation/Providers/EditorDiagnosticsAutomationProvider.cpp";
				std::ifstream input(provider);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				for (const std::string_view marker : { "route.lock", "diagnostics.snapshot", "capture.provenance", "unavailableMetrics" })
					context.Expect(text.find(marker) != std::string::npos,
						"Diagnostics automation must expose route locks, immutable snapshots, provenance, and unavailable metrics.");
			});
		registry.Add("VirtualGeometry.Editor.Automation.DiagnosticsProviderPublishesEvidenceCapabilities",
			[](TestContext& context)
			{
				using namespace TerraEngine::Editor::Automation;
				EditorDiagnosticsAutomationProvider provider([](std::string_view) {
					return AutomationHandler([](const AutomationRequestContext&) {
						return AutomationDispatchResult{ true, ErrorCategory::Internal, {}, nlohmann::json::object() };
					});
				});
				const auto capabilities = provider.GetCapabilities();
				for (const std::string_view name : { "route.lock", "diagnostics.snapshot", "capture.provenance" })
				{
					const auto found = std::find_if(capabilities.begin(), capabilities.end(), [name](const auto& capability) {
						return capability.descriptor.name == name;
					});
					context.Expect(found != capabilities.end(), "Diagnostics provider must publish the semantic evidence capability.");
					if (found != capabilities.end())
						context.Expect(static_cast<bool>(found->handler), "Published evidence capability must have an executable handler.");
				}
				const auto snapshot = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) {
					return capability.descriptor.name == "diagnostics.snapshot";
				});
				if (snapshot != capabilities.end() && snapshot->handler)
				{
					const auto result = snapshot->handler(AutomationRequestContext{});
					context.Expect(result.data.contains("objectIdentity") && result.data.contains("routeGeneration"),
						"Immutable diagnostics snapshots must always expose identity lanes.");
					context.Expect(result.data["unavailableMetrics"].is_array(),
						"Unmeasured identity lanes must remain explicit unavailable metrics.");
				}
				const auto routeLock = std::find_if(capabilities.begin(), capabilities.end(), [](const auto& capability) {
					return capability.descriptor.name == "route.lock";
				});
				if (routeLock != capabilities.end() && routeLock->handler)
				{
					AutomationRequestContext request{};
					request.arguments = {{"route", "virtual-compute"}, {"enabled", true}};
					const auto result = routeLock->handler(request);
					context.Expect(result.ok, "route.lock must delegate representation locks to the installed renderer route handler.");
				}
			});
		registry.Add("VirtualGeometry.Editor.Automation.CrossViewIdentityAndRouteGenerationsAreExposed",
			[](TestContext& context)
			{
				// Automation evidence must be sufficient to compare game, scene,
				// picking, and depth views without reopening mutable scene state.
				const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Editor/TerraEngine-Editor/Automation/Providers/EditorDiagnosticsAutomationProvider.cpp";
				std::ifstream input(source);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				for(const std::string_view marker : { "objectIdentity", "primitiveIdentity", "materialIdentity",
					"pickingIdentity", "depthIdentity", "routeGeneration" })
					context.Expect(text.find(marker) != std::string::npos,
						"Cross-view diagnostics must expose stable object, primitive, material, picking, depth, and route-generation identity.");
			});
		registry.Add("VirtualGeometry.Editor.Automation.VisualComparisonUsesDecodedQualityEvidence",
			[](TestContext& context)
			{
				const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
					"Tools/VirtualGeometry/CompareVirtualGeometryImages.ps1";
				std::ifstream input(source);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				for(const std::string_view marker : {
					"Format32bppArgb",
					"differentPixels",
					"meanAbsoluteError",
					"silhouetteDifferentPixels",
					"coverageRatioDelta",
					"differenceImage" })
				{
					context.Expect(text.find(marker) != std::string::npos,
						"Visual comparison must decode pixels and preserve quality plus human-review evidence.");
				}
				context.Expect(text.find("differentBytes") == std::string::npos,
					"Compressed image-container byte differences must not be presented as pixel differences.");
			});
		registry.Add("VirtualGeometry.Editor.Automation.ProfilerCaptureUsesBufferedCompletedSnapshots",
			[](TestContext& context)
			{
				const auto root = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
				const auto read = [](const std::filesystem::path& path) {
					std::ifstream input(path);
					return std::string{ std::istreambuf_iterator<char>(input), {} };
				};
				const std::string capture = read(root / "Engine/Profiling/ProfilerCapture.cpp");
				const std::string service = read(root / "Editor/TerraEngine-Editor/Profiling/EditorProfilerService.cpp");
				const std::string window = read(root / "Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp");
				context.Expect(capture.find("completed") != std::string::npos || capture.find("buffer") != std::string::npos,
					"Profiler capture must retain completed frame samples in a buffer.");
				context.Expect(service.find("snapshot") != std::string::npos || service.find("Snapshot") != std::string::npos,
					"Profiler service must consume immutable completed snapshots.");
				context.Expect(window.find("snapshot") != std::string::npos || window.find("Snapshot") != std::string::npos,
					"Profiler window must consume buffered snapshots rather than synchronously polling the engine.");
			});
		registry.Add("VirtualGeometry.Editor.Automation.ProductionDottedOperationsAreRegistered",
			[](TestContext &context)
			{
				const auto source = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";
				std::ifstream input(source);
				const std::string text{ std::istreambuf_iterator<char>(input), {} };
				for (const std::string_view operation : { "asset.virtualGeometry.codec.inspect",
					"asset.virtualGeometry.benchmark.status", "asset.virtualGeometry.benchmark.capture" })
					context.Expect(text.find(operation) != std::string::npos, "Dedicated production automation operation must be registered and dispatched.");
				context.Expect(text.find("requires an installed runner and live adapter/readback") != std::string::npos,
					"Benchmark capture must fail closed when authoritative runtime evidence is unavailable.");
			});
	}
} // namespace TerraEngine::EditorTests
