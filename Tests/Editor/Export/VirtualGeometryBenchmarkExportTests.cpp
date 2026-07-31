#include "TestHarness.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryBenchmarkExportTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Editor.Export.BenchmarkManifestHasVersionedSchema",
			[](TestContext &context)
			{
				// The manifest is a source-controlled export contract, so resolve it
				// from the source root instead of CTest's build-tree working folder.
				const auto manifestPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tools/VirtualGeometry/workloads.json";
				std::ifstream input(manifestPath);
				nlohmann::json manifest;
				context.Expect(static_cast<bool>(input), "Workload manifest is required for installed benchmark export.");
				if (input)
				{
					input >> manifest;
					context.Expect(manifest.value("schemaVersion", 0) == 1, "Benchmark manifest schema must be version 1.");
					context.Expect(manifest.contains("workloads") && !manifest["workloads"].empty(), "Benchmark manifest must contain workloads.");
				}
			});
		registry.Add("VirtualGeometry.Editor.Export.MissingCaptureCannotValidateClaim",
			[](TestContext &context)
			{
				context.Expect(
					!std::filesystem::exists("out/evidence/045/comparison.json"), "Absent comparison evidence must not validate a published benchmark claim.");
			});
		registry.Add("VirtualGeometry.Editor.Export.MeasuredClaimsRequireExecutionIdentity",
			[](TestContext &context)
			{
				const auto sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Tools/VirtualGeometry/TerraEngineVirtualGeometryBenchmark.cpp";
				std::ifstream source(sourcePath);
				const std::string text{ std::istreambuf_iterator<char>(source), {} };
				context.Expect(text.find("executionId") != std::string::npos, "Measured samples must carry an execution identity.");
				context.Expect(text.find("values.size() < 5u") != std::string::npos, "Aggregates must reject fewer than five measured samples.");
				context.Expect(
					text.find("runtime-adapter-reported") == std::string::npos,
					"Measured identity fields must preserve concrete runtime SHA-256 values instead of placeholders.");
				context.Expect(
					text.find("runtime-adapter-pending-hash") == std::string::npos,
					"Measured captures must publish a reproducible integrity hash instead of a pending marker.");
				context.Expect(
					text.find("stable-sha256-values-preserved-from-runtime-adapter") != std::string::npos,
					"The benchmark must document that adapter identity hashes are preserved rather than invented.");
				context.Expect(
					text.find("sha256-of-canonical-json-with-integrity-hash-empty") != std::string::npos,
					"The benchmark must document a reproducible aggregate-integrity policy.");
			});
	}
} // namespace TerraEngine::EditorTests
