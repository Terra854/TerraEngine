#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Diagnostics/VirtualGeometryEvidence.h"
#include "VirtualGeometrySchemas.h"

#include <algorithm>
#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryEvidenceSchemaTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.EvidenceSchema.EmitsEveryRequiredSectionInCanonicalOrder",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkCapture capture{};
				capture.identity.captureId = "capture-001";
				capture.identity.workloadVersion = "workload-v1";
				capture.environment.backend = "vulkan";
				capture.environment.adapter = "test-adapter";
				capture.workload.name = "static-scene";
				capture.integrityHash = "sha256:test";

				const std::string serialized = TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture);
				std::size_t previousPosition = 0u;
				for (const std::string_view section : TerraEngine::Renderer::kVirtualGeometryBenchmarkRequiredSections)
				{
					const std::size_t position = serialized.find(std::string("\"") + std::string(section) + "\"");
					context.Expect(position != std::string::npos, "The evidence serializer must emit every required benchmark section.");
					context.Expect(position >= previousPosition, "The evidence serializer must keep required sections in canonical order.");
					previousPosition = position;
				}
			});

		registry.Add("VirtualGeometry.EvidenceSchema.SerializationIsDeterministicAndEscapesStrings",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkCapture capture{};
				capture.identity.captureId = "capture-\"quoted\"";
				capture.identity.workloadVersion = "v1";
				capture.environment.backend = "vulkan";
				capture.environment.adapter = "adapter";
				capture.workload.name = "scene";
				capture.integrityHash = "integrity";
				capture.statistics.medianNanoseconds = 100u;
				capture.statistics.p95Nanoseconds = 125u;
				capture.statistics.p99Nanoseconds = 150u;
				capture.statistics.sampleCount = 6u;
				capture.statistics.rejectedCount = 1u;
				capture.diagnostics = { { "z-last", "value" }, { "a-first", "line\nbreak" } };

				const std::string first = TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture);
				const std::string second = TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture);
				context.ExpectEqual(first, second, "Equivalent evidence captures must serialize to byte-identical JSON.");
				context.Expect(first.find("capture-\\\"quoted\\\"") != std::string::npos, "Evidence strings must escape quotation marks.");
				context.Expect(first.find("line\\nbreak") != std::string::npos, "Evidence strings must escape control characters.");
				context.Expect(first.find("\"a-first\"") < first.find("\"z-last\""), "Diagnostic map keys must serialize in sorted order.");
				context.Expect(first.find("medianNanoseconds") != std::string::npos
					&& first.find("p95Nanoseconds") != std::string::npos
					&& first.find("p99Nanoseconds") != std::string::npos
					&& first.find("rejectedCount") != std::string::npos,
					"Evidence serialization must retain robust statistics and rejected-sample counts.");
			});

		registry.Add("VirtualGeometry.EvidenceSchema.SerializerOutputPassesInstalledValidator",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkCapture capture{};
				capture.identity.captureId = "capture-validated";
				capture.identity.workloadVersion = "workload-v1";
				capture.environment.backend = "vulkan";
				capture.workload.name = "foundation";
				capture.integrityHash = "sha256:test";

				const auto validated =
					TerraEngine::Tools::VirtualGeometry::ValidateSchemaDocument(TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture),
						TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::BenchmarkCapture);
				context.Expect(validated.valid, "The benchmark writer and installed validator must share one accepted schema.");
				context.Expect(!validated.canonicalJson.empty(), "Accepted captures must provide deterministic canonical JSON for hashing.");
			});

		registry.Add("VirtualGeometry.EvidenceSchema.FeatureManifestsValidateFromSourceTree",
			[](TestContext &context)
			{
				using namespace TerraEngine::Tools::VirtualGeometry;
				const std::filesystem::path root = TERRAENGINE_SOURCE_DIR;
				const auto workloads = LoadAndValidateSchemaDocument(root / "Tests/Tools/VirtualGeometry/workloads.json", SchemaDocumentKind::Workloads);
				const auto hardware = LoadAndValidateSchemaDocument(root / "Tests/Tools/VirtualGeometry/hardware-matrix.json", SchemaDocumentKind::HardwareMatrix);
				const auto quality = LoadAndValidateSchemaDocument(root / "Tests/Tools/VirtualGeometry/quality-profiles.json", SchemaDocumentKind::QualityProfiles);
				context.Expect(workloads.valid, "The checked-in workload manifest must satisfy the installed schema.");
				context.Expect(hardware.valid, "The checked-in hardware matrix must satisfy the installed schema.");
				context.Expect(quality.valid, "The checked-in quality profiles must satisfy the installed schema.");
			});

		registry.Add("VirtualGeometry.EvidenceSchema.MissingSectionsFailWithPaths",
			[](TestContext &context)
			{
				const auto rejected = TerraEngine::Tools::VirtualGeometry::ValidateSchemaDocument(
					R"({"identity":{"schemaVersion":"1"},"rawSamples":[]})", TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::BenchmarkCapture);
				context.Expect(!rejected.valid, "A partial benchmark capture must not satisfy a release evidence gate.");
				context.Expect(
					!rejected.issues.empty() && rejected.issues.front().path.starts_with("$."), "Schema failures must name machine-readable JSON paths.");
			});

		registry.Add("VirtualGeometry.EvidenceSchema.MeasuredRowsRequireCompletedQueryProvenance",
			[](TestContext& context)
			{
				TerraEngine::Renderer::VirtualGeometryBenchmarkCapture capture{};
				capture.identity.captureId = "capture-provenance";
				capture.identity.workloadVersion = "workload-v1";
				capture.environment.backend = "vulkan";
				capture.workload.name = "provenance";
				capture.integrityHash = "sha256:test";
				TerraEngine::Renderer::VirtualGeometryMeasurementRecord sample{};
				sample.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
				sample.durationNanoseconds = 100u;
				sample.queryIdentity = "query-1";
				sample.executionEvidenceId = "completed-submit-1";
				sample.identity = { 1u, 0u, 1u };
				capture.rawSamples.push_back(sample);
				const auto accepted = TerraEngine::Tools::VirtualGeometry::ValidateSchemaDocument(
					TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture),
					TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::BenchmarkCapture);
				context.Expect(accepted.valid,
					"A measured benchmark row must retain completed frame, query, and execution provenance.");
				capture.rawSamples.front().executionEvidenceId.clear();
				const auto rejected = TerraEngine::Tools::VirtualGeometry::ValidateSchemaDocument(
					TerraEngine::Renderer::SerializeVirtualGeometryBenchmarkCapture(capture),
					TerraEngine::Tools::VirtualGeometry::SchemaDocumentKind::BenchmarkCapture);
				context.Expect(!rejected.valid,
					"A measured duration without completed execution proof must fail closed.");
			});
	}
} // namespace TerraEngine::Tests
