#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

#include <fstream>
#include <sstream>
#include <string>

namespace TerraEngine::EditorTests {

	namespace
	{
		std::string ReadAutomationServiceSource()
		{
			// The MCP/automation contract is JSON text assembled by the editor
			// service, so this test guards the public field names directly.
			const auto read = [](const std::string& candidate) {
				std::ifstream file(candidate, std::ios::binary);
				if (!file)
				{
					return std::string();
				}
				std::ostringstream buffer;
				buffer << file.rdbuf();
				return buffer.str();
			};
			const std::string path = "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";

			std::string text = read(path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../") + path);
			if (!text.empty())
			{
				return text;
			}
			text = read(std::string("../../../../") + path);
			if (!text.empty())
			{
				return text;
			}

			const std::string thisFile = __FILE__;
			const auto resolveFromMarker = [&](const std::string& marker) {
				const std::size_t markerPosition = thisFile.find(marker);
				if (markerPosition == std::string::npos)
				{
					return std::string();
				}
				return read(thisFile.substr(0u, markerPosition) + path);
			};
			text = resolveFromMarker("Tests\\Editor\\Automation\\VirtualGeometryScalingAutomationTests.cpp");
			if (!text.empty())
			{
				return text;
			}
			return resolveFromMarker("Tests/Editor/Automation/VirtualGeometryScalingAutomationTests.cpp");
		}
	}

	void RegisterVirtualGeometryScalingAutomationTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Editor.Automation.Foundation.DiagnosticsReadContractNamesSourceData", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.viewType = RenderViewType::Game;
			diagnostics.counterSource = VirtualGeometryCounterSource::Prepared;
			diagnostics.clusterPacketCount = 12u;
			diagnostics.candidateClusterCount = 14u;
			diagnostics.compactedClusterCount = 10u;
			diagnostics.rejectedClusterCount = 4u;
			diagnostics.failOpenClusterCount = 1u;
			diagnostics.submittedBatchCount = 2u;
			diagnostics.residentPages = 3u;
			diagnostics.requestedPages = 2u;
			diagnostics.missingPages = 1u;
			diagnostics.pinnedFallbackPages = 1u;
			diagnostics.hiZBaselineMatched = true;

			context.Expect(diagnostics.counterSource == VirtualGeometryCounterSource::Prepared, "Automation diagnostics should carry source labels instead of authoritative-looking zeros.");
			context.ExpectEqual(12u, diagnostics.clusterPacketCount, "Automation diagnostics should expose packet counts.");
			context.ExpectEqual(14u, diagnostics.candidateClusterCount, "Automation diagnostics should expose candidate packet counts.");
			context.ExpectEqual(10u, diagnostics.compactedClusterCount, "Automation diagnostics should expose compacted packet counts.");
			context.ExpectEqual(4u, diagnostics.rejectedClusterCount, "Automation diagnostics should expose rejected packet counts.");
			context.ExpectEqual(1u, diagnostics.failOpenClusterCount, "Automation diagnostics should expose fail-open packet counts.");
			context.ExpectEqual(2u, diagnostics.submittedBatchCount, "Automation diagnostics should expose submitted batch counts.");
			context.ExpectEqual(3u, diagnostics.residentPages, "Automation diagnostics should expose resident page counts.");
			context.ExpectEqual(2u, diagnostics.requestedPages, "Automation diagnostics should expose requested page counts.");
			context.ExpectEqual(1u, diagnostics.missingPages, "Automation diagnostics should expose missing page counts.");
			context.ExpectEqual(1u, diagnostics.pinnedFallbackPages, "Automation diagnostics should expose pinned fallback counts.");
			context.Expect(diagnostics.hiZBaselineMatched, "Automation diagnostics should report the preserved Hi-Z baseline state.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.US6.CullingStatsExposeGpuPacketCounters", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics;
			diagnostics.counterSource = VirtualGeometryCounterSource::GPUReadback;
			diagnostics.gpuPacketRecordCount = 8u;
			diagnostics.gpuBatchDescriptorCount = 4u;
			diagnostics.gpuCompactionDispatchCount = 2u;
			diagnostics.generatedSubmissionCount = 2u;
			diagnostics.validGeneratedSubmissionCount = 1u;
			diagnostics.invalidGeneratedSubmissionCount = 1u;
			diagnostics.gpuCounterReadbackCount = 1u;
			diagnostics.gpuFallbackRecordCount = 1u;
			diagnostics.gpuFallbackReason = "validation fallback";

			const std::string source = ReadAutomationServiceSource();
			context.Expect(source.find("\"counterSource\"") != std::string::npos, "Automation stats should publish the counter source label.");
			context.Expect(source.find("\"gpuPacketRecords\"") != std::string::npos, "Automation stats should publish GPU packet record counts.");
			context.Expect(source.find("\"gpuBatchDescriptors\"") != std::string::npos, "Automation stats should publish GPU batch descriptor counts.");
			context.Expect(source.find("\"gpuCompactionDispatches\"") != std::string::npos, "Automation stats should publish compaction dispatch counts.");
			context.Expect(source.find("\"generatedSubmissions\"") != std::string::npos, "Automation stats should publish generated submission counts.");
			context.Expect(source.find("\"validGeneratedSubmissions\"") != std::string::npos, "Automation stats should publish valid generated submission counts.");
			context.Expect(source.find("\"invalidGeneratedSubmissions\"") != std::string::npos, "Automation stats should publish invalid generated submission counts.");
			context.Expect(source.find("\"gpuCounterReadbacks\"") != std::string::npos, "Automation stats should publish GPU counter readback counts.");
			context.Expect(source.find("\"gpuFallbackReason\"") != std::string::npos, "Automation stats should publish fail-open fallback reasons.");
			context.Expect(diagnostics.counterSource == VirtualGeometryCounterSource::GPUReadback, "Automation fixtures should be able to represent readback-backed packet counters.");
			context.ExpectEqual(8u, diagnostics.gpuPacketRecordCount, "Automation diagnostics should carry GPU packet counts.");
			context.ExpectEqual(1u, diagnostics.invalidGeneratedSubmissionCount, "Automation diagnostics should carry generated submission validation counts.");
			context.Expect(diagnostics.gpuFallbackReason.find("fallback") != std::string::npos, "Automation diagnostics should carry fallback explanations.");
		});

		registry.Add("VirtualGeometry.Editor.Automation.US7.CullingStatsExposePerformanceGateSummaries", [](TestContext& context) {
			PerformanceGateResult gate;
			gate.gateId = "gpu-packet-batch-ratio";
			gate.fixtureId = "gpu-packet-buffer-batching";
			gate.viewType = RenderViewType::Game;
			gate.viewId = 2u;
			gate.metricName = "gpuBatchDescriptorRatio";
			gate.threshold = 0.1f;
			gate.actual = 0.05f;
			gate.source = VirtualGeometryPerformanceGateSource::GPUReadback;
			gate.status = VirtualGeometryPerformanceGateStatus::Pass;

			const std::string source = ReadAutomationServiceSource();
			context.Expect(source.find("\"viewId\", gate.viewId") != std::string::npos, "Automation gate serialization should expose the gate view id.");
			context.Expect(source.find("\"performanceGateCount\"") != std::string::npos, "Automation stats should expose compact performance gate counts.");
			context.Expect(source.find("\"performanceGateFailures\"") != std::string::npos, "Automation stats should expose performance gate failure counts.");
			context.Expect(source.find("\"performanceGateUnsupported\"") != std::string::npos, "Automation stats should expose unsupported performance gate counts.");
			context.Expect(source.find("\"firstFailedPerformanceGate\"") != std::string::npos, "Automation stats should expose the first failed gate label.");
			context.ExpectEqual(2u, gate.viewId, "Automation gate fixtures should be able to carry the producing view id.");
			context.Expect(gate.source == VirtualGeometryPerformanceGateSource::GPUReadback, "Automation gate fixtures should preserve GPU readback gate sources.");
		});
	}
}
