// Synthetic completed bytes test the exporter contract, never GPU acceptance.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <fstream>
#include <iterator>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Renderer;
		using namespace Editor::Automation;
		struct LightingFixture
		{
			RendererEfficiencyEvidenceExportContext context;
			RendererEfficiencyEvidence row;
			LightingFixture()
			{
				context.projectRoot = std::filesystem::temp_directory_path() / ("TerraLighting-" + ToHexString(GenerateUUID()));
				context.operationId = ToHexString(GenerateUUID()); context.fixtureId = "lighting-dirty";
				context.fixtureHash = std::string(64, 'a'); context.fixtureStepId = "lighting-dirty-00";
				context.runId = "lighting-unit"; context.mode = "Candidate"; context.stage = "P2";
				context.seed = 65001; context.settingsGeneration = 2; context.mutationGeneration = 3; context.rendererSessionGeneration = 4;
				context.outputDirectory = context.projectRoot / "renderer-efficiency-evidence" / context.operationId / "step0";
				std::filesystem::create_directories(context.projectRoot);
				std::ofstream(context.projectRoot / "renderer-efficiency-project.json") << nlohmann::json{{"schemaVersion",1}, {"kind","RendererEfficiencyValidationProject"},
					{"projectRoot",context.projectRoot.generic_string()}, {"fixtureId",context.fixtureId}, {"fixtureHash",context.fixtureHash}};
				row.kind = RendererEfficiencyEvidenceKind::Lighting; row.status = RendererEfficiencyEvidenceStatus::Valid;
				row.viewType = RenderViewType::Scene; row.viewId = 1; row.frameSerial = 5; row.commandSerial = 6;
				row.submitSerial = row.completionSerial = 7; row.fixtureControlGeneration = 8;
				row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4; row.effectiveOptions.optionsSessionGeneration = 9;
				row.fixtureCorrelation = {context.operationId, context.fixtureStepId, 60, 3, 2, 4};
				for (unsigned index = 0; index < 2; ++index)
				{
					auto& upload = row.lightingUploads[index]; upload.available = upload.succeeded = upload.coherent = true;
					upload.analysisPurpose = index != 0; upload.allocationId = 100 + index; upload.allocationGeneration = 2;
					upload.allocationSizeBytes = 8ULL * 176 * 1024; upload.regionSizeBytes = 176 * 1024; upload.nonCoherentAtomSize = 256;
					upload.payloadLightIds = {GenerateUUID(),GenerateUUID()}; upload.activeRecordCount = 2; upload.mirrorCpuDurationNanoseconds = 11;
				}
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(); artifacts->lightingPayload.resize(176 * 1024);
				artifacts->fullSourceCandidateCount = 1025; // Actual uncapped producer metadata, separate from two uploaded records.
				artifacts->lightingPayload[0] = 1;
				auto color = std::make_shared<RendererEfficiencyFrameColor>(); color->width = color->height = 1;
				color->sourceFormat = 44; color->bytes = {10,20,30,255}; artifacts->frameColor = color; row.artifacts = artifacts;
			}
			~LightingFixture() { std::error_code ignored; std::filesystem::remove_all(context.projectRoot, ignored); }
		};
	}
	void RegisterRendererEfficiencyLightingEvidenceTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.LightingEvidence.RealShapedPayloadHasNoInventedShadow", [](TestContext& t) {
			LightingFixture fixture; auto analysis = fixture.row; analysis.analysisPurpose = true;
			const std::array rows{ fixture.row, analysis }; RendererEfficiencyEvidenceExportResult result; std::string error;
			t.Expect(ExportRendererEfficiencyEvidence(fixture.context, rows, result, error), "Both completed purposes must export without fake depth/light identities: " + error);
			t.Expect(result.frames.size() == 2 && result.artifacts.size() == 6, "Each purpose retains full payload, native color and counters.");
			t.Expect(result.frames[0].contains("fullSourceCandidateCount")
				&& result.frames[0].at("fullSourceCandidateCount").at("value") == 1025,
				"Actual uncapped source count must survive export independently of capped GPU records.");
			for (const auto& artifact : result.artifacts) if (artifact.at("kind") == "Counters")
			{
				std::ifstream input(fixture.context.projectRoot / artifact.at("path").get<std::string>());
				const auto counters = nlohmann::json::parse(input);
				t.Expect(counters.contains("fullSourceCandidateCount") && counters.at("fullSourceCandidateCount") == 1025,
					"Raw native counters retain the same uncapped producer count.");
			}
			for (const auto& frame : result.frames)
				t.Expect(frame.at("evidenceKind") == "Lighting" && frame.at("lightId").at("status") == "Unavailable"
					&& frame.at("lighting")[0].at("hostWriteStatus") == "Succeeded", "Lighting names its actual purpose and does not invent a shadow light.");
			for (const auto& artifact : result.artifacts) if (artifact.at("kind") == "LightingPayload")
			{
				std::ifstream input(fixture.context.projectRoot / artifact.at("path").get<std::string>(), std::ios::binary);
				const std::vector<std::uint8_t> bytes{ std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>() };
				t.Expect(bytes == fixture.row.artifacts->lightingPayload && artifact.at("sha256").get<std::string>().size() == 64,
					"Full GPU-shaped payload bytes remain exact and content-addressed.");
			}
			// Noncoherent writes report their actual atom-expanded allocation range.
			for(auto& upload : fixture.row.lightingUploads) {
				upload.coherent = false; upload.logicalWriteBytes = 176; upload.logicalWriteRecords = 1;
				upload.logicalWriteRanges = {{0,176}}; upload.physicalFlushBytes = 256; upload.physicalFlushRanges = {{0,256}};
			}
			analysis.lightingUploads = fixture.row.lightingUploads;
			fixture.context.outputDirectory /= "noncoherent";
			t.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::array{fixture.row,analysis}, result, error)
				&& result.frames[0].at("lighting")[0].at("physicalFlushRanges").at("value")[0].at("lengthBytes") == 256,
				"Actual covering atom flushes export without replacing their native range shape.");
			// Baseline artifacts retain their real pre-edit native token; they do not
			// become warmup/measured rows or require a synthetic shadow identity.
			fixture.context.measurementPhase = "LightingBaseline";
			fixture.context.outputDirectory /= "baseline";
			fixture.row.fixtureCorrelation.stepFrame = analysis.fixtureCorrelation.stepFrame = 0;
			const std::array baseline{fixture.row, analysis};
			t.Expect(ExportRendererEfficiencyEvidence(fixture.context, baseline, result, error)
				&& result.frames[0].at("measurementPhase") == "LightingBaseline", "Actual baseline purposes retain token zero and their separate phase.");
		});
		registry.Add("Renderer.Efficiency.Editor.LightingEvidence.InvalidPayloadOrReceiptsRejectAtomically", [](TestContext& t) {
			for (unsigned fault = 0; fault < 21; ++fault)
			{
				LightingFixture fixture; auto bytes = std::make_shared<RendererEfficiencyArtifacts>(*fixture.row.artifacts); fixture.row.artifacts = bytes;
				auto& upload = fixture.row.lightingUploads[0];
				if (fault == 0) upload.available = false;
				if (fault == 1) upload.succeeded = false;
				if (fault == 2) upload.allocationGeneration = 0;
				if (fault == 3) upload.activeRecordCount = 1025;
				if (fault == 4) { upload.logicalWriteBytes = 176; upload.logicalWriteRecords = 1; }
				if (fault == 5) bytes->lightingPayload.pop_back();
				if (fault == 6) bytes->lightingPayload[176 * 2] = 1;
				if (fault == 7) bytes->frameColor.reset();
				if (fault == 8) fixture.row.effectiveOptions.rendererSessionGeneration = 5;
				if (fault == 9) fixture.row.completionSerial = 0;
				if (fault == 10) upload.analysisPurpose = true;
				if (fault == 11) { upload.physicalFlushBytes = 176; upload.physicalFlushRanges.push_back({0,176}); }
				if (fault == 12) upload.regionSizeBytes = 176;
				if (fault == 13) upload.allocationSizeBytes = 1;
				if (fault == 14) upload.nonCoherentAtomSize = 3;
				if (fault == 15) { upload.coherent = false; upload.logicalWriteBytes = 176; upload.logicalWriteRecords = 1;
					upload.logicalWriteRanges.push_back({0,176}); upload.physicalFlushBytes = 176; upload.physicalFlushRanges.push_back({0,176}); }
				if (fault == 16) upload.payloadLightIds.clear();
				if (fault == 17) upload.payloadLightIds[1] = upload.payloadLightIds[0];
				if (fault == 18) bytes->fullSourceCandidateCount.reset();
				if (fault == 19) bytes->fullSourceCandidateCount = 4097;
				if (fault == 20) bytes->fullSourceCandidateCount = 1;
				RendererEfficiencyEvidenceExportResult result; result.frames.push_back("retained-output"); std::string error;
				t.Expect(!ExportRendererEfficiencyEvidence(fixture.context, std::array{fixture.row, [&] { auto other = fixture.row; other.analysisPurpose = true; return other; }()}, result, error)
					&& result.frames == nlohmann::json::array({"retained-output"}) && !std::filesystem::exists(fixture.context.outputDirectory),
					"Incomplete native payload/receipt/range proof rejects before publishing files or output.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.LightingEvidence.DuplicatePurposeOrCommandRejects", [](TestContext& t) {
			for (unsigned fault = 0; fault < 6; ++fault)
			{
				LightingFixture fixture; auto second = fixture.row; second.analysisPurpose = fault != 0;
				if (fault == 1) ++second.commandSerial;
				if (fault == 2) ++second.fixtureControlGeneration;
				if (fault == 3) ++second.effectiveOptions.optionsSessionGeneration;
				if (fault == 4) { fixture.context.measurementPhase = "LightingBaseline"; }
				if (fault == 5) second.lightingUploads[1].allocationSizeBytes = 1;
				const std::array rows{fixture.row,second}; RendererEfficiencyEvidenceExportResult result; std::string error;
				t.Expect(!ExportRendererEfficiencyEvidence(fixture.context, rows, result, error) && !std::filesystem::exists(fixture.context.outputDirectory),
					"Each purpose appears once and both purposes share their actual qualified-view command.");
			}
		});
	}
}
