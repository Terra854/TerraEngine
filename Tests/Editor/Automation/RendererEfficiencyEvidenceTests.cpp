#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"

#include <bit>
#include <filesystem>
#include <fstream>
#include <limits>

namespace TerraEngine::EditorTests
{
	using namespace Editor::Automation;
	using namespace Renderer;
	namespace
	{
		// This native-shaped CPU fixture exercises serialization only. It cannot
		// establish GPU execution, installed identity, or acceptance thresholds.
		struct EvidenceFixture
		{
			RendererEfficiencyEvidenceExportContext capture;
			RendererEfficiencyEvidence row;
			nlohmann::json marker;
			EvidenceFixture()
			{
				capture.operationId = ToHexString(GenerateUUID());
				capture.projectRoot = std::filesystem::temp_directory_path() / ("TerraEvidence-" + capture.operationId);
				std::filesystem::create_directories(capture.projectRoot);
				capture.fixtureId = "shadow-invalidation"; capture.fixtureHash = std::string(64, 'a');
				capture.fixtureStepId = "shadow-invalidation-00"; capture.runId = "evidence-unit";
				capture.mode = "Reference"; capture.seed = 65001;
				capture.settingsGeneration = 2; capture.mutationGeneration = 3; capture.rendererSessionGeneration = 4;
				capture.outputDirectory = capture.projectRoot / "renderer-efficiency-evidence" / capture.operationId / "step0";
				marker = {{"schemaVersion", 1}, {"kind", "RendererEfficiencyValidationProject"},
					{"projectRoot", capture.projectRoot.generic_string()}, {"fixtureId", capture.fixtureId}, {"fixtureHash", capture.fixtureHash}};
				WriteMarker();
				row.status = RendererEfficiencyEvidenceStatus::Valid; row.viewType = RenderViewType::Scene; row.viewId = 1;
				row.lightId = GenerateUUID(); row.resolution = {1, 1}; row.frameSerial = 5; row.commandSerial = 6;
				row.submitSerial = row.completionSerial = 7; row.contentOwner = "actual-shaped-unit-owner";
				row.fixtureCorrelation = {capture.operationId, capture.fixtureStepId, 0, 3, 2, 4};
				row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4;
				for (auto* counter : {&row.eligibleCasterSegments, &row.submittedCasterSegments, &row.actualDrawSubmissions}) {
					counter->status = RendererEfficiencyEvidenceStatus::Valid; counter->value = 0;
				}
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>();
				artifacts->shadowKey.rendererSessionGeneration = 4; artifacts->shadowKey.viewType = row.viewType;
				artifacts->shadowKey.viewId = 1; artifacts->shadowKey.lightId = row.lightId;
				artifacts->shadowKey.resourceKind = ShadowResourceKind::SpotAtlas; artifacts->shadowKey.allocationGeneration = 1;
				artifacts->shadowKey.extent = {1, 1}; artifacts->shadowKey.depthFormat = artifacts->depthSourceFormat = 126;
				artifacts->contentToken = 1; artifacts->coverageU8 = {1};
				const auto bits = std::bit_cast<std::uint32_t>(0.5F);
				for (unsigned byte = 0; byte < 4; ++byte) artifacts->depthF32LE.push_back(static_cast<std::uint8_t>(bits >> (byte * 8)));
				auto color = std::make_shared<RendererEfficiencyFrameColor>(); color->width = color->height = 1;
				color->sourceFormat = 37; color->bytes = {10, 20, 30, 255}; artifacts->frameColor = color; row.artifacts = artifacts;
			}
			~EvidenceFixture() { std::error_code ignored; std::filesystem::remove_all(capture.projectRoot, ignored); }
			void WriteMarker() const { std::ofstream(capture.projectRoot / "renderer-efficiency-project.json") << marker.dump(); }
			bool Export(RendererEfficiencyEvidenceExportResult& result, std::string& error) const
			{
				return ExportRendererEfficiencyEvidence(capture, std::span(&row, 1), result, error);
			}
		};
		void ExpectRejected(TestContext& test, const EvidenceFixture& fixture)
		{
			RendererEfficiencyEvidenceExportResult result; result.frames = {"retained-frame"}; result.artifacts = {"retained-artifact"}; result.colorEncodingCount = 17;
			const auto beforeFrames = result.frames; const auto beforeArtifacts = result.artifacts;
			std::string error;
			test.Expect(!fixture.Export(result, error) && !error.empty(), "Production export must reject malformed evidence.");
			test.Expect(result.frames == beforeFrames && result.artifacts == beforeArtifacts && result.colorEncodingCount == 17, "Rejection cannot publish a partial result.");
		}
	}
	void RegisterRendererEfficiencyEvidenceTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.Evidence.MissingMarkerFieldsRejectAtomically", [](TestContext& test) {
			for (const auto* field : {"schemaVersion", "kind", "projectRoot", "fixtureId", "fixtureHash"}) {
				EvidenceFixture fixture; fixture.marker.erase(field); fixture.WriteMarker(); ExpectRejected(test, fixture);
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Evidence.CrossFieldIdentityRejectsAtomically", [](TestContext& test) {
			for (unsigned fault = 0; fault < 10; ++fault) {
				EvidenceFixture fixture;
				if (fault == 0) fixture.capture.fixtureHash.assign(64, 'b');
				if (fault == 1) fixture.row.fixtureCorrelation.operationId = ToHexString(GenerateUUID());
				if (fault == 2) fixture.row.fixtureCorrelation.fixtureStepId += "-other";
				if (fault == 3) fixture.row.fixtureCorrelation.mutationGeneration++;
				if (fault == 4) fixture.row.effectiveOptions.settingsGeneration++;
				if (fault == 5) fixture.row.effectiveOptions.rendererSessionGeneration++;
				if (fault == 6) fixture.row.viewId++;
				if (fault == 7) fixture.row.completionSerial = fixture.row.submitSerial - 1;
				if (fault == 8) fixture.row.actualDrawSubmissions.value.reset();
				if (fault == 9) fixture.capture.outputDirectory = fixture.capture.projectRoot / "foreign-output";
				ExpectRejected(test, fixture);
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Evidence.EmptyAndInvalidTimingRejectAtomically", [](TestContext& test) {
			EvidenceFixture fixture; RendererEfficiencyEvidenceExportResult result; result.frames = {"retained"}; std::string error;
			test.Expect(!ExportRendererEfficiencyEvidence(fixture.capture, {}, result, error) && result.frames == nlohmann::json{"retained"}, "Zero observations cannot invent exported samples.");
			for (double duration : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
				fixture.row.lightingDepthTiming.status = RendererEfficiencyEvidenceStatus::Valid;
				fixture.row.lightingDepthTiming.durationMilliseconds = duration; ExpectRejected(test, fixture);
			}
		});
		registry.Add("Renderer.Efficiency.Editor.Evidence.CpuAndResourceOverheadNeverInventGpuTiming", [](TestContext& test) {
			EvidenceFixture fixture;
			// Zero-light successful writes and CPU work remain real observations,
			// independently of absent GPU timing. Coherent storage never flushes.
			auto& upload = fixture.row.lightingUploads[0]; upload.available = upload.succeeded = upload.coherent = true;
			upload.allocationId = 9; upload.allocationGeneration = 10;
			upload.allocationSizeBytes = upload.regionSizeBytes = 176ULL * 1024;
			upload.logicalWriteBytes = upload.regionSizeBytes; upload.logicalWriteRecords = 1024;
			upload.logicalWriteRanges = {{0, upload.regionSizeBytes}}; upload.mirrorCpuDurationNanoseconds = 1234;
			RendererEfficiencyEvidenceExportResult result; std::string error;
			test.Expect(fixture.Export(result, error), "Observed zero-light/coherent write must export: " + error);
			const auto& frame = result.frames.at(0);
			test.Expect(frame.at("depthStageTimingNanoseconds").at("status") == "Unavailable", "CPU overhead does not manufacture a GPU interval.");
			const auto& receipts = frame.at("lighting");
			test.Expect(receipts.at(0).at("mirrorCpuDurationNanoseconds").at("value") == 1234
				&& receipts.at(0).at("physicalFlushBytes").at("value") == 0
				&& receipts.at(0).at("logicalWriteBytes").at("value") == 176ULL * 1024, "CPU duration, actual bytes, and measured zero flush remain independent.");
			fixture.row.lightingDepthTiming.status = RendererEfficiencyEvidenceStatus::Pending;
			fixture.row.lightingDepthTiming.durationMilliseconds = 99;
			test.Expect(fixture.Export(result, error) && result.frames.at(0).at("depthStageTimingNanoseconds").at("status") == "Pending"
				&& result.frames.at(0).at("depthStageTimingNanoseconds").at("value").is_null(), "Pending timing cannot expose even a retained numeric value.");
		});
	}
}
