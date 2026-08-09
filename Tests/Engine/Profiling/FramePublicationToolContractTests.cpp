#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace TerraEngine::Tests
{
	namespace
	{
		// CTest launches the shared engine-test executable from the build tree,
		// while developers often launch it from the repository root. Resolve tool
		// contracts from the source directory baked into the test target so both
		// entry points inspect the same authored files.
		std::filesystem::path FramePublicationToolRoot()
		{
			return std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Tools" / "FramePublication";
		}
	}

	void RegisterFramePublicationToolContractTests(TestRegistry& registry)
	{
		registry.Add("Engine.FramePublicationTools.ManifestDefinesCanonicalScenarios", [](TestContext& context)
		{
			const std::filesystem::path path =
				FramePublicationToolRoot() / "workloads.json";
			std::ifstream input(path);
			context.Expect(input.good(), "Frame publication workload manifest must be present.");
			if (!input) return;
			nlohmann::json manifest; input >> manifest;
			context.Expect(manifest.value("schemaVersion", 0) == 1, "Workload schema version must be supported.");
			context.Expect(manifest.value("measuredRuns", 0) == 5, "Comparable captures require five runs.");
			context.Expect(manifest["scenarios"].is_array() && manifest["scenarios"].size() == 6, "Exactly six canonical scenarios are required.");
			for (const auto& scenario : manifest["scenarios"])
			{
				context.Expect(scenario.contains("id") && scenario.contains("population")
					&& scenario.contains("routeMix") && scenario.contains("animationPolicy")
					&& scenario.contains("seed"), "Every scenario must carry immutable identity fields.");
			}
			context.Expect(manifest.value("durationSeconds", 0) == 60, "Measured runs must last 60 seconds.");
			context.Expect(manifest["warmup"].value("minimumSeconds", 0) >= 30
				&& manifest["warmup"].value("minimumFrames", 0) >= 120, "Warmup must be at least 30 seconds and 120 frames.");
		});

		registry.Add("Engine.FramePublicationTools.FailClosedEvidenceAndInstalledContract", [](TestContext& context)
		{
			const std::filesystem::path root = FramePublicationToolRoot();
			for (const char* name : {"CaptureFramePublicationPerformance.ps1", "CompareFramePublicationPerformance.ps1", "RunFramePublicationContracts.ps1", "RunFramePublicationSoak.ps1"})
				context.Expect(std::filesystem::exists(root / name), std::string("Installed tooling source must include ") + name);
			std::ifstream compare(root / "CompareFramePublicationPerformance.ps1");
			std::string source((std::istreambuf_iterator<char>(compare)), {});
			context.Expect(source.find("Identity mismatch") != std::string::npos
				&& source.find("exactly five measured runs") != std::string::npos,
				"Comparison must reject identity mismatch and incorrect run count.");
			context.Expect(source.find("unavailable") == std::string::npos,
				"Unavailable placeholder evidence must not be treated as comparable output.");
			std::ifstream capture(root / "CaptureFramePublicationPerformance.ps1");
			std::string captureSource((std::istreambuf_iterator<char>(capture)), {});
			context.Expect(captureSource.find("postCaptureStats.data") != std::string::npos
				&& captureSource.find("profilerEvidence.summary.data.summary") != std::string::npos
				&& captureSource.find("geometryRouteCalibrationIdentity") != std::string::npos
				&& captureSource.find("run-$run-metrics.json") != std::string::npos,
				"Capture must extract route diagnostics and profiler timing streams per run.");
			context.Expect(captureSource.find("sparseMutationOperation") != std::string::npos,
				"Static sparse-change capture must require an explicit one-percent mutation operation.");
			context.Expect(captureSource.find("variantSharedClipLists") != std::string::npos
				&& captureSource.find("shared looping clip") != std::string::npos,
				"Shared animation captures must select a looping clip that remains visibly active after warmup.");
			std::ifstream liveVisual(FramePublicationToolRoot().parent_path()
				/ "VirtualGeometry" / "CaptureVirtualGeometryLiveVisual.ps1");
			std::string liveVisualSource((std::istreambuf_iterator<char>(liveVisual)), {});
			context.Expect(liveVisual.good()
				&& liveVisualSource.find("retryDelaysMilliseconds") != std::string::npos
				&& liveVisualSource.find("uniqueCaptureHashCount") != std::string::npos
				&& liveVisualSource.find("Sort-Object -Unique") != std::string::npos
				&& liveVisualSource.find("FramePublication-[^/]+\\.tescene") != std::string::npos
				&& liveVisualSource.find("Refusing to clean a validation scene outside Assets/Scenes") != std::string::npos,
				"Animated visual validation must tolerate loop-phase aliasing while still requiring distinct retained frame hashes.");
			context.Expect(liveVisualSource.find("postPresentIdentity") != std::string::npos
				&& liveVisualSource.find("healthyAnimatedPresentation") != std::string::npos
				&& liveVisualSource.find("changingCaptureHashes") != std::string::npos
				&& liveVisualSource.find("safeFallbackPacketCount") != std::string::npos,
				"Animated captures must prove changing post-present hashes and healthy animation presentation packets.");
			context.Expect(captureSource.find("SparseTransformMutation") != std::string::npos
				&& captureSource.find("committedFrameCount") != std::string::npos
				&& captureSource.find("batchSize") != std::string::npos,
				"Static sparse-change capture must prove protected lifecycle coverage and exact batch cardinality.");
			context.Expect(captureSource.find("[switch]$Resume") != std::string::npos
				&& captureSource.find("Test-ReusableScenarioCapture") != std::string::npos
				&& captureSource.find("@($capture.runs).Count -ne $requiredRuns") != std::string::npos,
				"Resume must reuse only complete identity-matched captures with the exact run count.");
			std::ifstream soak(root / "RunFramePublicationSoak.ps1");
			std::string soakSource((std::istreambuf_iterator<char>(soak)), {});
			context.Expect(soakSource.find("$adapterSucceeded = $?") != std::string::npos
				&& soakSource.find("$LASTEXITCODE") == std::string::npos,
				"The PowerShell soak wrapper must preserve a successful script result without consulting a stale native exit code.");
			std::ifstream captureSchema(root / "Schemas" / "capture.schema.json");
			std::string schema((std::istreambuf_iterator<char>(captureSchema)), {});
			context.Expect(schema.find("projectHash") != std::string::npos
				&& schema.find("cameraHash") != std::string::npos
				&& schema.find("timingSources") != std::string::npos,
				"Capture schema must require project/camera/timing identity.");
			std::ifstream liveSchema(root / "Schemas" / "live-visual.schema.json");
			std::string liveSchemaText((std::istreambuf_iterator<char>(liveSchema)), {});
			context.Expect(liveSchema.good()
				&& liveSchemaText.find("uniqueCaptureHashCount") != std::string::npos
				&& liveSchemaText.find("\"minItems\": 2") != std::string::npos,
				"Live visual schema must require multiple retained captures and hash evidence.");
			context.Expect(source.find("Capture manifest missing") != std::string::npos,
				"Comparison must fail closed when capture.json is missing.");
		});
	}
}
