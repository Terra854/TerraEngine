#include "TestHarness.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	namespace
	{
		std::string ReadProductionFile(const char* relativePath)
		{
			std::ifstream input(
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath,
				std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}

		void ExpectContains(
			TestContext& context,
			const std::string& contents,
			std::string_view token,
			std::string_view message)
		{
			context.Expect(contents.find(token) != std::string::npos, message);
		}
	}

	void RegisterVirtualGeometryProductionScaffoldingTests(TestRegistry& registry)
	{
		// This neutral registration point is intentionally tiny. Capability-specific
		// editor tests extend the production contract without relying on static initialization
		// or silently disappearing from the editor test executable.
		registry.Add("VirtualGeometry.Editor.Production.ScaffoldingRegistered", [](TestContext& context)
		{
			context.Expect(true,
				"The production virtual-geometry editor-test registration point must execute.");
		});

		registry.Add("VirtualGeometry.Editor.Production.LiveHarnessSuppressesAutomaticSceneOpen", [](TestContext& context)
		{
			const std::string editorSource = ReadProductionFile(
				"Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp");
			const std::string harnessSource = ReadProductionFile(
				"Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			constexpr std::string_view flag =
				"TERRAENGINE_SUPPRESS_AUTOMATIC_SCENE_OPEN";
			context.Expect(
				editorSource.find(flag) != std::string::npos,
				"The editor must expose the process-only automatic-scene suppression flag.");
			context.Expect(
				editorSource.find("if (!suppressAutomaticSceneOpen)") != std::string::npos,
				"The LastScene and fallback scan must remain behind the suppression decision.");
			context.Expect(
				harnessSource.find(flag) != std::string::npos,
				"The installed live harness must opt into disposable-scene startup.");
		});

		registry.Add("VirtualGeometry.Editor.Production.LiveHarnessLocksValidationRoutes", [](TestContext& context)
		{
			const std::string harnessSource = ReadProductionFile(
				"Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			const std::string comparisonSource = ReadProductionFile(
				"Tools/VirtualGeometry/CompareVisibilityImages.ps1");
			ExpectContains(context, harnessSource,
				"renderer.virtualGeometry.validationRoute.set",
				"Route-parity captures must set a transient renderer validation route before capture.");
			ExpectContains(context, harnessSource,
				"renderer.virtualGeometry.validationRoute.reset",
				"Route-parity captures must reset their transient renderer validation route.");
			ExpectContains(context, comparisonSource,
				"baselineVisibilityRoute",
				"Cross-route comparisons must preserve baseline route provenance.");
			ExpectContains(context, comparisonSource,
				"candidateVisibilityRoute",
				"Cross-route comparisons must preserve candidate route provenance.");
		});

		registry.Add("VirtualGeometry.Editor.Production.LiveHarnessUsesDurableProfilerArtifactIdentity", [](TestContext& context)
		{
			const std::string harnessSource = ReadProductionFile(
				"Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1");
			const std::string physicsHarnessSource = ReadProductionFile(
				"Tools/Physics/RunInstalledEditorPhysicsStress.ps1");
			const std::string automationSource = ReadProductionFile(
				"Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp");
			ExpectContains(context, harnessSource,
				"$profilerArtifactId = [string](Get-PropertyValue $profilerJobResult 'artifactId')",
				"The live harness must select the artifact identity returned by the durable stop job.");
			ExpectContains(context, harnessSource,
				"$profilerStarted -and -not $profilerStopRequested",
				"Error cleanup must not stop and persist an already-finalized capture again.");
			context.Expect(
				harnessSource.find("$profilerBeforeIds -notcontains") == std::string::npos,
				"The live harness must not infer artifact identity from an asynchronous list delta.");
			ExpectContains(context, physicsHarnessSource,
				"$_.artifactId -eq $profilerCaptureArtifactId",
				"The installed physics harness must resolve the exact durable profiler artifact.");
			ExpectContains(context, automationSource,
				"Profiler capture is not recording or paused.",
				"Profiler automation must reject a second stop after finalization begins.");
		});

		registry.Add("VirtualGeometry.Editor.Production.InstallProducersListRequiredArtifacts", [](TestContext& context)
		{
			const std::string rootCmake = ReadProductionFile("CMakeLists.txt");
			const std::string toolsCmake = ReadProductionFile(
				"Tools/VirtualGeometry/CMakeLists.txt");

			// These assertions protect the direct install producers without
			// requiring a feature-specific post-install verification script.
			ExpectContains(context, rootCmake,
				"Tests/Fixtures/VirtualGeometry/hardening-fixtures.json",
				"The deterministic hardening fixture manifest must be installed.");
			ExpectContains(context, rootCmake,
				"Tests/Fixtures/AnimatedVirtualGeometry/",
				"The deterministic animated virtual-geometry fixture must be installed.");
			ExpectContains(context, rootCmake,
				"VirtualGeometryTraversal.comp.spv",
				"The accelerated-route shader must be produced in the install tree.");
			ExpectContains(context, toolsCmake,
				"${CMAKE_BINARY_DIR}/Build/Tools",
				"Every production virtual-geometry executable must use the canonical runnable install tree.");
			ExpectContains(context, toolsCmake,
				"TerraEngineVirtualGeometryBenchmark",
				"The benchmark executable must have an installed producer.");
		});

		registry.Add("VirtualGeometry.Editor.Production.CapabilityScenariosCoverRequiredContracts", [](TestContext& context)
		{
			const std::filesystem::path scenarioRoot =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) /
				"Tools/VirtualGeometry/CapabilityScenarios";
			std::size_t scenarioCount = 0;
			std::string combined;
			for (const std::filesystem::directory_entry& entry :
				std::filesystem::directory_iterator(scenarioRoot))
			{
				if (!entry.is_regular_file() ||
					entry.path().extension() != ".json")
				{
					continue;
				}
				++scenarioCount;
				std::ifstream input(entry.path(), std::ios::binary);
				std::ostringstream contents;
				contents << input.rdbuf();
				combined += contents.str();
			}
			context.ExpectEqual(
				static_cast<std::size_t>(10),
				scenarioCount,
				"The installed gate must define the complete production capability set.");
			for (const std::string_view scenarioId : {
				"capability-contract", "codec-storage", "device-submission",
				"visibility-parity", "streaming-publication", "hybrid-rasterization",
				"multi-view-rendering", "animated-geometry", "vendor-portability",
				"production-readiness"})
			{
				ExpectContains(context, combined, "\"id\": \"" + std::string(scenarioId) + "\"",
					"Every required production capability scenario must be present.");
			}
			context.Expect(
				combined.find("D3D12") == std::string::npos &&
					combined.find("DX12") == std::string::npos &&
					combined.find("DXIL") == std::string::npos,
				"Production capability scenarios must stay Vulkan-only.");
		});

		registry.Add("VirtualGeometry.Editor.Production.ReleaseGateHashesRealEvidence", [](TestContext& context)
		{
			const std::string scenarioGate = ReadProductionFile(
				"Tools/VirtualGeometry/RunVirtualGeometryCapabilityGate.ps1");
			const std::string releaseGate = ReadProductionFile(
				"Tools/VirtualGeometry/InvokeVirtualGeometryReleaseGates.ps1");
			const std::string matrixGate = ReadProductionFile(
				"Tools/VirtualGeometry/RunBackendParityMatrix.ps1");
			ExpectContains(context, scenarioGate,
				"InvokeVirtualGeometryReleaseGates.ps1",
				"The installed editor runner must invoke the release-gate orchestrator.");
			ExpectContains(context, releaseGate,
				"Get-FileHash -LiteralPath",
				"Release evidence must bind files to SHA-256 identities.");
			ExpectContains(context, releaseGate,
				"selectedPassRecords",
				"Quickstart tests must count selected pass records instead of registry totals.");
			ExpectContains(context, releaseGate,
				"foreach($route in @('compute', 'hybrid', 'indexed'))",
				"The release gate must run explicit hybrid route parity.");
			ExpectContains(context, releaseGate,
				"'full-reference'",
				"The release gate must validate independent full-reference evidence.");
			ExpectContains(context, releaseGate,
				"'million-decision'",
				"The release gate must validate independent million-decision evidence.");
			ExpectContains(context, releaseGate,
				"backendScope = @('Vulkan')",
				"The release gate must declare a Vulkan-only backend scope.");
			ExpectContains(context, releaseGate,
				"'-BaselineDirectory'",
				"The benchmark comparison must use the comparator's directory contract.");
			ExpectContains(context, releaseGate,
				"'-CandidateDirectory'",
				"The benchmark comparison must supply the candidate capture directory.");
			ExpectContains(context, releaseGate,
				"VirtualGeometry.Editor.Automation.ValidationRoutes",
				"The visibility-parity editor gate must select the current route-validation tests.");
			ExpectContains(context, releaseGate,
				"VirtualGeometry.Editor.Diagnostics.Foundation.ShadowSelectionsRemainSeparate",
				"The multi-view editor gate must select the current auxiliary-view tests.");
			ExpectContains(context, releaseGate,
				"VirtualGeometry.Editor.Export.Benchmark",
				"The production-readiness editor gate must select the current benchmark-export tests.");
			ExpectContains(context, scenarioGate,
				"$declaredOutputs",
				"Every operation and capture output declared by a capability manifest must be audited.");
			ExpectContains(context, matrixGate,
				"'--runtime-adapter' $RuntimeAdapter",
				"The backend matrix must forward its runtime adapter to the native benchmark.");
			ExpectContains(context, matrixGate,
				"currentHostVulkan",
				"Current-host Vulkan validation must be reported separately from future vendor coverage.");
			ExpectContains(context, matrixGate,
				"crossVendorCoverage",
				"Unavailable required vendor lanes must remain visible as incomplete cross-vendor coverage.");
		});
	}
}
