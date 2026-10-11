// CPU serialization tests use native-shaped bytes and receipts, never GPU claims.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <bit>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Renderer;
		using namespace Editor::Automation;
		struct ExportFixture
		{
			RendererEfficiencyEvidenceExportContext context;
			RendererEfficiencyEvidence row;
			ExportFixture()
			{
				// Each test owns an isolated marker and content-addressed output root.
				context.projectRoot = std::filesystem::temp_directory_path() / ("TerraTransition-" + ToHexString(GenerateUUID()));
				context.operationId = ToHexString(GenerateUUID()); context.fixtureId = "shadow-invalidation";
				context.fixtureHash = std::string(64, 'a'); context.fixtureStepId = "shadow-invalidation-09";
				context.runId = "transition-unit"; context.mode = "Candidate"; context.measurementPhase = "Transition";
				context.seed = 65001; context.settingsGeneration = 2; context.mutationGeneration = 3; context.rendererSessionGeneration = 4;
				context.outputDirectory = context.projectRoot / "renderer-efficiency-evidence" / context.operationId / "step9" / "transition";
				std::filesystem::create_directories(context.projectRoot);
				std::ofstream(context.projectRoot / "renderer-efficiency-project.json") << nlohmann::json{{"schemaVersion",1}, {"kind","RendererEfficiencyValidationProject"},
					{"projectRoot",context.projectRoot.generic_string()}, {"fixtureId",context.fixtureId}, {"fixtureHash",context.fixtureHash}};
				row.status = RendererEfficiencyEvidenceStatus::Valid; row.viewType = RenderViewType::Scene; row.viewId = 1;
				row.lightId = GenerateUUID(); row.resolution = {1,1}; row.frameSerial = 5; row.commandSerial = 6; row.submitSerial = row.completionSerial = 7;
				row.contentOwner = "retained-native-shaped-owner"; row.fixtureControlGeneration = 8;
				row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4; row.effectiveOptions.optionsSessionGeneration = 9;
				row.fixtureCorrelation = {context.operationId, context.fixtureStepId, 0, 3, 2, 4};
				for(auto* counter : {&row.eligibleCasterSegments, &row.submittedCasterSegments, &row.actualDrawSubmissions})
				{ counter->status = RendererEfficiencyEvidenceStatus::Valid; counter->value = 0; counter->reason.clear(); }
				auto bytes = std::make_shared<RendererEfficiencyArtifacts>(); bytes->shadowKey.rendererSessionGeneration = 4;
				bytes->shadowKey.viewType = row.viewType; bytes->shadowKey.viewId = 1; bytes->shadowKey.lightId = row.lightId;
				bytes->shadowKey.resourceKind = ShadowResourceKind::SpotAtlas; bytes->shadowKey.allocationGeneration = 1;
				bytes->shadowKey.extent = {1,1}; bytes->shadowKey.depthFormat = bytes->depthSourceFormat = 126; bytes->contentToken = 10;
				const auto bits = std::bit_cast<std::uint32_t>(0.5F);
				for(unsigned byte = 0; byte < 4; ++byte) bytes->depthF32LE.push_back(static_cast<std::uint8_t>(bits >> (byte * 8)));
				bytes->coverageU8 = {1}; auto color = std::make_shared<RendererEfficiencyFrameColor>();
				color->width = color->height = 1; color->sourceFormat = 44; color->bytes = {10,20,30,255}; bytes->frameColor = color; row.artifacts = bytes;
			}
			~ExportFixture() { std::error_code ignored; std::filesystem::remove_all(context.projectRoot, ignored); }
		};
		std::string HashBytes(const std::vector<std::uint8_t>& bytes)
		{
			// Independently verify every referenced file with the host's bundled OS
			// cryptography API; comparison does not reuse the exporter's hash helper.
			struct Algorithm { BCRYPT_ALG_HANDLE value = nullptr; ~Algorithm() { if(value) BCryptCloseAlgorithmProvider(value,0); } } algorithm;
			struct Hash { BCRYPT_HASH_HANDLE value = nullptr; ~Hash() { if(value) BCryptDestroyHash(value); } };
			ULONG size = 0, returned = 0;
			if(BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_SHA256_ALGORITHM,nullptr,0) < 0
				|| BCryptGetProperty(algorithm.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&returned,0) < 0) throw std::runtime_error("Test SHA initialization failed.");
			std::vector<std::uint8_t> object(size); Hash hash; std::array<std::uint8_t,32> digest{};
			if(BCryptCreateHash(algorithm.value,&hash.value,object.data(),size,nullptr,0,0) < 0
				|| BCryptHashData(hash.value,const_cast<PUCHAR>(bytes.data()),static_cast<ULONG>(bytes.size()),0) < 0
				|| BCryptFinishHash(hash.value,digest.data(),static_cast<ULONG>(digest.size()),0) < 0) throw std::runtime_error("Test SHA failed.");
			constexpr char hex[] = "0123456789abcdef"; std::string text;
			for(const auto byte : digest) { text += hex[byte >> 4]; text += hex[byte & 15]; } return text;
		}
		nlohmann::json Counters(const ExportFixture& fixture, const RendererEfficiencyEvidenceExportResult& result)
		{
			for(const auto& artifact : result.artifacts) if(artifact.at("kind") == "Counters")
			{ std::ifstream input(fixture.context.projectRoot / artifact.at("path").get<std::string>()); return nlohmann::json::parse(input); }
			throw std::runtime_error("Export omitted native counters.");
		}
	}
	void RegisterRendererEfficiencyControlTransitionExportTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.TransitionExport.ResidencyPurposePreservesNativeReuploadToken", [](TestContext& t) {
			ExportFixture fixture;
			fixture.context.transitionPurpose = RendererEfficiencyTransitionPurpose::ResidencyReupload;
			fixture.context.residencyBaselineStepFrame = 360;
			fixture.context.fixtureStepId = fixture.row.fixtureCorrelation.fixtureStepId = "shadow-invalidation-12";
			fixture.context.outputDirectory = fixture.context.projectRoot / "renderer-efficiency-evidence" / fixture.context.operationId / "step12" / "residency-transition";
			fixture.row.fixtureCorrelation.stepFrame = 361;
			RendererEfficiencyEvidenceExportResult result; std::string error;
			t.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), result, error),
				"A completed native residency reupload must retain its positive original step-frame token: " + error);
			const auto counters = Counters(fixture, result);
			t.Expect(counters.at("stepFrame") == "361" && counters.at("measurementPhase") == "Transition"
				&& counters.at("readbackProvenance").at("mutationGeneration") == "3"
				&& counters.at("readbackProvenance").at("fixtureControlGeneration") == "8"
				&& counters.at("readbackProvenance").at("optionsSessionGeneration") == "9"
				&& result.frames[0].at("stepFrame") == 361,
				"Residency serialization must not synthesize control frame zero or change original mutation correlation.");
			ExportFixture baseline; baseline.context.transitionPurpose = RendererEfficiencyTransitionPurpose::ResidencyBaseline;
			baseline.context.residencyBaselineStepFrame = 360; baseline.row.fixtureCorrelation.stepFrame = 360;
			RendererEfficiencyEvidenceExportResult baselineResult;
			t.Expect(ExportRendererEfficiencyEvidence(baseline.context, std::span(&baseline.row, 1), baselineResult, error)
				&& Counters(baseline, baselineResult).at("stepFrame") == "360", "The completed residency baseline retains its actual frozen-window native token.");
			// All failures exercise the production writer before any file output,
			// including mixed reupload tokens and missing current publication proof.
			for(unsigned failure = 0; failure < 14; ++failure) {
				ExportFixture invalid; invalid.context.transitionPurpose = RendererEfficiencyTransitionPurpose::ResidencyReupload;
				invalid.context.residencyBaselineStepFrame = 360;
				invalid.row.fixtureCorrelation.stepFrame = 361;
				auto second = invalid.row; second.lightId = GenerateUUID();
				auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(*second.artifacts);
				artifacts->shadowKey.lightId = second.lightId; second.artifacts = artifacts;
				switch(failure) {
				case 0: invalid.context.transitionPurpose = RendererEfficiencyTransitionPurpose::Control; invalid.context.residencyBaselineStepFrame = 0; break;
				case 1: invalid.context.transitionPurpose = RendererEfficiencyTransitionPurpose::ResidencyBaseline; break;
				case 2: invalid.row.fixtureCorrelation.stepFrame = second.fixtureCorrelation.stepFrame = 0; break;
				case 3: invalid.context.measurementPhase = "Measured"; break;
				case 4: invalid.context.measurementPhase = "Warmup"; invalid.context.transitionPurpose = RendererEfficiencyTransitionPurpose::ResidencyBaseline; break;
				case 5: invalid.row.fixtureControlGeneration = 0; break;
				case 6: invalid.row.effectiveOptions.optionsSessionGeneration = 0; break;
				case 7: second.fixtureCorrelation.stepFrame = 362; break;
				case 8: second.fixtureControlGeneration = 11; break;
				case 9: second.effectiveOptions.optionsSessionGeneration = 12; break;
				case 10: invalid.context.residencyBaselineStepFrame = 0; break;
				case 11: invalid.row.fixtureCorrelation.stepFrame = second.fixtureCorrelation.stepFrame = 360; break;
				case 12: invalid.context.transitionPurpose = RendererEfficiencyTransitionPurpose::Control;
					invalid.context.measurementPhase = "Warmup"; invalid.row.fixtureCorrelation.stepFrame = second.fixtureCorrelation.stepFrame = 0; break;
				case 13: second.fixtureCorrelation.mutationGeneration = 4; break;
				}
				const std::array rows{invalid.row, second}; RendererEfficiencyEvidenceExportResult rejected;
				rejected.frames.push_back("retained-output"); error.clear();
				t.Expect(!ExportRendererEfficiencyEvidence(invalid.context, rows, rejected, error)
					&& rejected.frames == nlohmann::json::array({"retained-output"}) && !error.empty()
					&& !std::filesystem::exists(invalid.context.outputDirectory), "Wrong residency purpose or incoherent native completion must reject atomically.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.TransitionExport.PreservesActualCorrelationAndArtifactClosure", [](TestContext& t) {
			ExportFixture fixture; RendererEfficiencyEvidenceExportResult result; std::string error;
			const bool exported = ExportRendererEfficiencyEvidence(fixture.context,std::span(&fixture.row,1),result,error);
			t.Expect(exported, "Actual exporter must accept a nested transition without relabeling receipts: " + error);
			t.Expect(result.frames.size() == 1 && result.artifacts.size() == 4 && result.colorEncodingCount == 1, "One complete native-shaped observation must retain all four artifacts.");
			const auto counters = Counters(fixture,result); const auto& provenance = counters.at("readbackProvenance");
			t.Expect(counters.at("measurementPhase") == "Transition" && counters.at("stepFrame") == "0" && provenance.at("mutationGeneration") == "3"
				&& provenance.at("fixtureControlGeneration") == "8" && provenance.at("optionsSessionGeneration") == "9",
				"Transition must preserve the old mutation correlation and serialize actual control/options epochs as exact decimal strings.");
			t.Expect(provenance.at("frameSerial") == "5" && provenance.at("commandSerial") == "6" && provenance.at("submitSerial") == "7" && provenance.at("completionSerial") == "7"
				&& counters.at("sourceContentOwner").at("contentToken") == "10", "CPU export must preserve genuine supplied receipt and content-owner identity.");
			for(const auto& artifact : result.artifacts) {
				const auto relative = std::filesystem::path(artifact.at("path").get<std::string>()); std::ifstream input(fixture.context.projectRoot / relative,std::ios::binary);
				const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
				t.Expect(!relative.is_absolute() && !bytes.empty() && artifact.at("status") == "Valid" && HashBytes(bytes) == artifact.at("sha256")
					&& relative.filename().string().starts_with(artifact.at("sha256").get<std::string>()), "Every referenced artifact must exist with its independently verified content-addressed SHA.");
			}
			// Independent views record distinct primaries in one actual submit.
			// Their commands must differ legally while all common receipt fields agree.
			auto game = fixture.row; game.viewType = RenderViewType::Game; game.viewId = 2; game.commandSerial = 11;
			auto gameBytes = std::make_shared<RendererEfficiencyArtifacts>(*game.artifacts);
			gameBytes->shadowKey.viewType = game.viewType; gameBytes->shadowKey.viewId = game.viewId; game.artifacts = gameBytes;
			const std::array views{fixture.row,game}; RendererEfficiencyEvidenceExportResult viewResult;
			const bool viewsExported = ExportRendererEfficiencyEvidence(fixture.context,views,viewResult,error);
			t.Expect(viewsExported && viewResult.frames.size() == 2 && viewResult.frames[0].at("commandSerial").at("value") == 6
				&& viewResult.frames[1].at("commandSerial").at("value") == 11, "Scene and Game must retain distinct genuine supplied command identities in the same submit cohort: " + error);
		});
		registry.Add("Renderer.Efficiency.Editor.TransitionExport.RejectsUnsupportedPhaseAndUnknownControlCohort", [](TestContext& t) {
			for(unsigned failure = 0; failure < 7; ++failure) {
				ExportFixture fixture; auto second = fixture.row; second.lightId = GenerateUUID(); auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(*second.artifacts);
				artifacts->shadowKey.lightId = second.lightId; second.artifacts = artifacts;
				switch(failure) {
				case 0: fixture.context.measurementPhase = "FalsePhase"; break;
				case 1: fixture.row.fixtureControlGeneration = 0; break;
				case 2: fixture.row.effectiveOptions.optionsSessionGeneration = 0; break;
				case 3: second.fixtureControlGeneration = 11; break;
				case 4: second.effectiveOptions.optionsSessionGeneration = 12; break;
				case 5: fixture.row.fixtureCorrelation.stepFrame = 1; break;
				case 6: second.commandSerial = 11; break;
				}
				const std::array rows{fixture.row,second}; RendererEfficiencyEvidenceExportResult result; result.frames.push_back("retained-output"); std::string error;
				t.Expect(!ExportRendererEfficiencyEvidence(fixture.context,rows,result,error) && result.frames == nlohmann::json::array({"retained-output"})
					&& !error.empty() && !std::filesystem::exists(fixture.context.outputDirectory), "Invalid phase/zero/mixed transition proof must reject atomically before artifact output.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.TransitionExport.NormalPhasesPreserveTruthfulZeroProvenance", [](TestContext& t) {
			for(const auto* phase : {"Warmup","Measured"}) {
				ExportFixture fixture; fixture.context.measurementPhase = phase; fixture.row.fixtureControlGeneration = 0; fixture.row.effectiveOptions.optionsSessionGeneration = 0;
				RendererEfficiencyEvidenceExportResult result; std::string error;
				const bool exported = ExportRendererEfficiencyEvidence(fixture.context,std::span(&fixture.row,1),result,error);
				t.Expect(exported, "Existing normal serialization must remain supported: " + error);
				const auto counters = Counters(fixture,result); const auto& provenance = counters.at("readbackProvenance");
				t.Expect(counters.at("measurementPhase") == phase && provenance.at("fixtureControlGeneration") == "0" && provenance.at("optionsSessionGeneration") == "0",
					"Normal CPU fixtures must serialize raw zero provenance honestly, never manufacture production proof.");
			}
		});
	}
}
