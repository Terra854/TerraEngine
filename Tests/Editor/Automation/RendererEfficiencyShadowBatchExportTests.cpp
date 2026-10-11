// Actual exporter policies consume copied native-shaped receipts. These CPU
// tests cannot establish GPU grouping, visibility or completed pixel parity.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <bit>
#include <fstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Renderer;
		using namespace Editor::Automation;
		struct Fixture
		{
			RendererEfficiencyEvidenceExportContext context;
			RendererEfficiencyEvidence row;
			Fixture()
			{
				context.projectRoot = std::filesystem::temp_directory_path() / ("terra-batch-export-" + ToHexString(GenerateUUID()));
				context.fixtureId = "shadow-batching"; context.fixtureHash = std::string(64, 'a');
				context.operationId = ToHexString(GenerateUUID()); context.fixtureStepId = "batch-00";
				context.stage = "P4"; context.runId = "cpu-contract"; context.mode = "Candidate";
				context.seed = 65004; context.settingsGeneration = 2;
				context.mutationGeneration = 3; context.rendererSessionGeneration = 4;
				context.outputDirectory = context.projectRoot / "renderer-efficiency-evidence" / context.operationId / "step0";
				std::filesystem::create_directories(context.projectRoot);
				std::ofstream(context.projectRoot / "renderer-efficiency-project.json") << nlohmann::json{
					{"schemaVersion",1}, {"kind","RendererEfficiencyValidationProject"},
					{"projectRoot",context.projectRoot.generic_string()}, {"fixtureId",context.fixtureId}, {"fixtureHash",context.fixtureHash}};
				row.status = RendererEfficiencyEvidenceStatus::Valid; row.kind = RendererEfficiencyEvidenceKind::Shadow;
				row.viewType = RenderViewType::Scene; row.viewId = 1; row.lightId = GenerateUUID(); row.resolution = {1,1};
				row.frameSerial = 5; row.commandSerial = 6; row.submitSerial = row.completionSerial = 7;
				row.contentOwner = "actual-shaped-owner"; row.fixtureControlGeneration = 8;
				row.effectiveOptions.settingsGeneration = 2; row.effectiveOptions.rendererSessionGeneration = 4;
				row.effectiveOptions.optionsSessionGeneration = 9;
				row.fixtureCorrelation = {context.operationId, context.fixtureStepId, 60, 3, 2, 4};
				auto bytes = std::make_shared<RendererEfficiencyArtifacts>();
				bytes->shadowKey.rendererSessionGeneration = 4; bytes->shadowKey.viewType = row.viewType;
				bytes->shadowKey.viewId = 1; bytes->shadowKey.lightId = row.lightId;
				bytes->shadowKey.resourceKind = ShadowResourceKind::SpotAtlas; bytes->shadowKey.allocationGeneration = 1;
				bytes->shadowKey.extent = {1,1}; bytes->shadowKey.depthFormat = bytes->depthSourceFormat = 126; bytes->contentToken = 10;
				const auto bits = std::bit_cast<std::uint32_t>(0.5F);
				for(unsigned byte = 0; byte < 4; ++byte) bytes->depthF32LE.push_back(static_cast<std::uint8_t>(bits >> (byte * 8)));
				bytes->coverageU8 = {1}; auto color = std::make_shared<RendererEfficiencyFrameColor>();
				color->width = color->height = 1; color->sourceFormat = 44; color->bytes = {10,20,30,255}; bytes->frameColor = color;
				auto& execution = row.shadowBatchExecution;
				execution.available = execution.recordedComplete = execution.frameSlotSafe = true;
				execution.uploadAttempted = execution.uploadSucceeded = true;
				execution.allocationIdentity = 11; execution.allocationGeneration = 12;
				execution.backendGeneration = 13; execution.instanceBytes = 352;
				execution.frameSlot = 1; execution.snapshotSlot = 7;
				RendererEfficiencyShadowBatchGroup group; group.instanceCount = group.taskGroupsX = 2;
				group.pipelineIdentity = 14; group.layoutIdentity = 15;
				for(unsigned index = 0; index < 3; ++index)
				{
					ShadowCasterStamp stamp; stamp.objectId[0] = index + 1; stamp.lifetimeGeneration = 20 + index;
					stamp.segmentId = index + 1; stamp.meshletCount = 1;
					bytes->eligibleMembership.rows.push_back(stamp); bytes->submittedSegments.push_back(stamp);
					execution.sources.push_back({stamp.objectId, stamp.lifetimeGeneration, index, 0, 1, true, false});
					// Visibility comes from an independent captured camera selection,
					// not from whether this source contributes to the light's shadows.
					execution.sources.back().cameraVisible = index != 2;
					RendererEfficiencyShadowBatchMember member{stamp.objectId, stamp.lifetimeGeneration, stamp.segmentId};
					if(index < 2) group.members.push_back(member); else execution.singletons.push_back(member);
				}
				execution.groups.push_back(group); row.artifacts = bytes;
				for(auto* counter : {&row.eligibleCasterSegments, &row.submittedCasterSegments, &row.actualDrawSubmissions, &row.groups, &row.singletons})
				{ counter->status = RendererEfficiencyEvidenceStatus::Valid; counter->reason.clear(); }
				row.eligibleCasterSegments.value = row.submittedCasterSegments.value = 3;
				row.actualDrawSubmissions.value = 2; row.groups.value = row.singletons.value = 1;
			}
			~Fixture() { std::error_code ignored; std::filesystem::remove_all(context.projectRoot, ignored); }
			void StaticExecution(bool reuse)
			{
				context.stage = "P5";
				auto bytes = std::make_shared<RendererEfficiencyArtifacts>(*row.artifacts);
				for (auto& stamp : bytes->eligibleMembership.rows) {
					stamp.meshId[0] = 1; stamp.evidenceMask = kCompleteShadowCasterEvidence;
					stamp.staticSupport = {true, true, true, true, true, true};
				}
				bytes->eligibleMembership.rows[2].staticSupport.authoredStatic = false;
				bytes->submittedSegments = bytes->eligibleMembership.rows;
				if (reuse) bytes->submittedSegments.erase(bytes->submittedSegments.begin(), bytes->submittedSegments.begin() + 2);
				row.artifacts = bytes; row.shadowBatchExecution.available = false;
				auto& execution = row.staticShadowExecution;
				execution.available = execution.partitionComplete = execution.copyOverlay = execution.recordedComplete = true;
				execution.staticReused = reuse; execution.requiredFaceMask = execution.copiedFaceMask = 1;
				execution.staticRecordedFaceMask = reuse ? 0 : 1;
				execution.staticView = bytes->shadowKey; execution.staticView.allocationGeneration = 22;
				execution.staticContentToken = 23; execution.sourceCommandSerial = reuse ? 2 : row.commandSerial;
				execution.sourceSubmitSerial = reuse ? 3 : row.submitSerial;
				execution.allocationIdentity = 24; execution.allocationGeneration = 22; execution.backendGeneration = 25;
				execution.staticView.regionId = execution.allocationIdentity;
				execution.actualBytes = 4096; execution.staticDraws = reuse ? 0 : 2; execution.dynamicDraws = 1;
				execution.staticMembership.assign(bytes->eligibleMembership.rows.begin(), bytes->eligibleMembership.rows.begin() + 2);
				execution.dynamicMembership = {bytes->eligibleMembership.rows[2]};
				row.submittedCasterSegments.value = reuse ? 1 : 3; row.actualDrawSubmissions.value = reuse ? 1 : 3;
				row.staticReservedBytes = {RendererEfficiencyEvidenceStatus::Valid, 0, ""};
				row.staticActiveBytes = {RendererEfficiencyEvidenceStatus::Valid, 4096, ""};
				row.staticRetiringBytes = {RendererEfficiencyEvidenceStatus::Valid, 8192, ""}; row.staticCacheDraining = true;
				RendererEfficiencyStaticShadowAllocation owner;
				owner.sessionGeneration = row.effectiveOptions.rendererSessionGeneration; owner.backendGeneration = execution.backendGeneration;
				owner.allocationIdentity = execution.allocationIdentity; owner.allocationGeneration = execution.allocationGeneration;
				owner.actualBytes = 4096; owner.depthFormat = 126; owner.layers = 1; owner.extent = {1, 1};
				owner.uses.push_back({execution.sourceCommandSerial, execution.sourceSubmitSerial, 1});
				if (reuse) owner.uses.push_back({row.commandSerial, 0, 1}); // Actual recorded use before submit promotion.
				execution.allocations.push_back(owner);
				owner.allocationIdentity = 26; owner.allocationGeneration = 27; owner.actualBytes = 8192; owner.retiring = true; owner.uses.clear();
				execution.allocations.push_back(owner); // Completion-qualified release awaits its protected pump.
				execution.completedSubmissions = {1}; execution.discardedCommands = {4};
				execution.acceptedSubmissions.push_back({execution.sourceCommandSerial, execution.sourceSubmitSerial, 1});
				if (reuse) execution.acceptedSubmissions.push_back({row.commandSerial, row.submitSerial, 1});
			}
		};
		nlohmann::json Counters(const Fixture& fixture, const RendererEfficiencyEvidenceExportResult& result)
		{
			for(const auto& artifact : result.artifacts) if(artifact.at("kind") == "Counters")
			{ std::ifstream stream(fixture.context.projectRoot / artifact.at("path").get<std::string>()); return nlohmann::json::parse(stream); }
			throw std::runtime_error("Missing raw counters.");
		}
	}
	void RegisterRendererEfficiencyShadowBatchExportTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.StaticShadowExport.RawActualCopyAndMemory", [](TestContext& context) {
			Fixture fixture; fixture.StaticExecution(false); RendererEfficiencyEvidenceExportResult output; std::string error;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error), "Completed fresh static copy and dynamic overlay export.");
			const auto raw = Counters(fixture, output);
			context.Expect(raw.contains("staticShadowExecution") && raw.at("staticShadowExecution").at("staticContentToken") == "23"
				&& raw.at("staticShadowExecution").at("staticMembership").size() == 2
				&& raw.at("staticShadowExecution").at("staticView").at("allocationGeneration") == "22", "Actual static owner and full semantic rows remain distinct from final allocation.");
			context.Expect(output.frames.at(0).at("memory").at("draining") == true
				&& output.frames.at(0).at("memory").at("activeBytes").at("value") == 4096,
				"Available native memory and draining values become measured frame fields.");
			Fixture cube; cube.StaticExecution(true);
			auto cubeBytes = std::make_shared<RendererEfficiencyArtifacts>(*cube.row.artifacts);
			cube.row.faceIndex = cubeBytes->shadowKey.faceIndex = 5;
			cubeBytes->shadowKey.resourceKind = ShadowResourceKind::PointCube;
			for (unsigned face = 0; face < 6; ++face) cubeBytes->shadowKey.lightProjectionStamp.cubeViewProjections[face][0] = face + 1;
			cubeBytes->shadowKey.lightProjectionStamp.cubeProjectionEvidenceComplete = true;
			cubeBytes->shadowKey.lightProjectionStamp.viewProjection = cubeBytes->shadowKey.lightProjectionStamp.cubeViewProjections[5];
			cube.row.artifacts = cubeBytes;
			auto& e = cube.row.staticShadowExecution; e.staticView = cubeBytes->shadowKey;
			e.staticView.faceIndex = 0; e.staticView.regionId = e.allocationIdentity; e.staticView.allocationGeneration = e.allocationGeneration;
			e.staticView.lightProjectionStamp.viewProjection = e.staticView.lightProjectionStamp.cubeViewProjections[0];
			e.requiredFaceMask = e.copiedFaceMask = 63;
			e.allocations[0].layers = 6;
			std::vector<RendererEfficiencyEvidence> faces;
			for (unsigned face = 0; face < 6; ++face) {
				auto row = cube.row; auto bytes = std::make_shared<RendererEfficiencyArtifacts>(*cubeBytes);
				row.faceIndex = bytes->shadowKey.faceIndex = face;
				bytes->shadowKey.lightProjectionStamp.viewProjection = bytes->shadowKey.lightProjectionStamp.cubeViewProjections[face];
				row.artifacts = bytes; faces.push_back(std::move(row));
			}
			context.Expect(ExportRendererEfficiencyEvidence(cube.context, faces, output, error),
				"Whole-cube source owner and per-face matrix differ only through the exact six-face projection set.");
			faces[5].staticShadowExecution.staticView.lightProjectionStamp.cubeViewProjections[2][0]++;
			context.Expect(!ExportRendererEfficiencyEvidence(cube.context, faces, output, error),
				"An unobserved cube-face projection mismatch cannot authorize a static copy.");
		});
		registry.Add("Renderer.Efficiency.Editor.StaticShadowExport.MalformedReceiptAtomic", [](TestContext& context) {
			for (unsigned fault = 0; fault < 33; ++fault) {
				Fixture fixture; fixture.StaticExecution(false); auto& e = fixture.row.staticShadowExecution;
				if (fault == 0) e.partitionComplete = false;
				if (fault == 1) e.recordedComplete = false;
				if (fault == 2) e.staticContentToken = 0;
				if (fault == 3) e.sourceSubmitSerial = 0;
				if (fault == 4) e.copiedFaceMask = 0;
				if (fault == 5) e.staticMembership[0].staticSupport.deformationAbsent = false;
				if (fault == 6) e.dynamicMembership.clear();
				if (fault == 7) e.staticView.viewId++;
				if (fault == 8) e.staticMembership[1] = e.staticMembership[0];
				if (fault == 9) e.staticReused = true;
				if (fault == 10) e.actualBytes = 0;
				if (fault == 11) fixture.row.staticActiveBytes.status = RendererEfficiencyEvidenceStatus::Unavailable;
				if (fault == 12) e.staticView.regionId++;
				if (fault == 13) e.sourceSubmitSerial++;
				if (fault == 14) e.staticView.allocationGeneration++;
				if (fault == 15) e.dynamicMembership[0].scale[0]++;
				if (fault == 16) e.allocations.clear();
				if (fault == 17) e.allocations[0].sessionGeneration++;
				if (fault == 18) e.allocations[0].uses[0].commandSerial = 0;
				if (fault == 19) e.allocations.push_back(e.allocations[0]);
				if (fault == 20) e.completedSubmissions.push_back(0);
				if (fault == 21) e.discardedCommands.push_back(e.discardedCommands[0]);
				if (fault == 22) e.allocations[0].retiring = true;
				if (fault == 23) e.allocations[0].actualBytes++;
				if (fault == 24) e.backendGeneration = 0;
				if (fault == 25) e.allocations[0].backendGeneration++;
				if (fault == 26) e.acceptedSubmissions[0].commandSerial = 0;
				if (fault == 27) e.acceptedSubmissions[0].submitSerial = 0;
				if (fault == 28) e.acceptedSubmissions[0].frameSlot = 3;
				if (fault == 29) e.acceptedSubmissions.push_back(e.acceptedSubmissions[0]);
				if (fault == 30) e.acceptedSubmissions[0].submitSerial++;
				if (fault == 31) e.acceptedSubmissions[0].frameSlot++;
				if (fault == 32) { e.allocations[0].uses[0].submitSerial = 0; e.acceptedSubmissions[0].submitSerial++; }
				RendererEfficiencyEvidenceExportResult output; output.colorEncodingCount = 99; std::string error;
				context.Expect(!ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
					&& output.colorEncodingCount == 99 && output.frames.empty() && !std::filesystem::exists(fixture.context.outputDirectory),
					"Forged static owner, partition, masks or missing native memory rejects before publication.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.StaticShadowExport.ReuseAndUnavailable", [](TestContext& context) {
			Fixture fixture; fixture.StaticExecution(true); RendererEfficiencyEvidenceExportResult output; std::string error;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error), "Cached static rows stay eligible but only dynamic rows are newly submitted.");
			auto raw = Counters(fixture, output);
			context.Expect(raw.contains("staticShadowExecution") && raw.at("staticShadowExecution").at("staticReused").get<bool>()
				&& raw.at("submittedCasterIdentityRows").size() == 1
				&& output.frames.at(0).at("shadow").at("staticReuseRegionCount").at("value") == 1,
				"Reuse never fabricates cached static draw submissions and reports the actual reused static region.");
			context.Expect(raw.at("staticShadowExecution").at("allocations").at(0).at("uses").at(1).at("submitSerial") == "0"
				&& raw.at("staticShadowExecution").at("completedSubmissions").at(0) == "1"
				&& raw.at("staticShadowExecution").contains("acceptedSubmissions")
				&& raw.at("staticShadowExecution").at("acceptedSubmissions").at(1).at("submitSerial") == "7",
				"Recorded pending uses and actual bounded completion history retain their exact serial types.");
			fixture.row.commandSerial = 800;
			fixture.row.staticShadowExecution.allocations[0].uses[1].commandSerial = 800;
			fixture.row.staticShadowExecution.acceptedSubmissions[1].commandSerial = 800;
			fixture.row.staticShadowExecution.acceptedSubmissions.push_back({801, 7, 1});
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error),
				"Independent command/submit clocks and multiple actual commands in one submit remain valid.");
			fixture.row.staticShadowExecution.available = false;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
				&& output.frames.at(0).at("memory").at("activeBytes").at("status") == "Unavailable",
				"Absent native static execution cannot authorize measured memory from leftover values.");
			// A combined fallback does not claim a reusable static source. Unknown
			// dynamic proof must remain observable rather than being invented.
			fixture.row.staticShadowExecution.available = true;
			fixture.row.staticShadowExecution.copyOverlay = fixture.row.staticShadowExecution.partitionComplete = false;
			fixture.row.staticShadowExecution.staticReused = false;
			fixture.row.staticShadowExecution.staticMembership.clear(); fixture.row.staticShadowExecution.dynamicMembership.clear();
			fixture.row.staticShadowExecution.staticContentToken = fixture.row.staticShadowExecution.sourceCommandSerial = fixture.row.staticShadowExecution.sourceSubmitSerial = 0;
			fixture.row.staticShadowExecution.fallbackReason = "Unknown dynamic semantic proof";
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
				&& output.frames.at(0).at("memory").at("admissionReason").at("value") == "Unknown dynamic semantic proof",
				"Native fallback memory remains available without a fabricated partition or static source.");
			fixture.row.staticShadowExecution.completedSubmissions.push_back(3);
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error),
				"A later actual completion receipt may coexist with its earlier captured allocation-use snapshot.");
			auto empty = std::make_shared<RendererEfficiencyArtifacts>(*fixture.row.artifacts);
			empty->eligibleMembership.rows.clear(); empty->submittedSegments.clear(); fixture.row.artifacts = empty;
			fixture.row.eligibleCasterSegments.value = fixture.row.submittedCasterSegments.value = fixture.row.actualDrawSubmissions.value = 0;
			fixture.row.staticShadowExecution.partitionComplete = true; fixture.row.staticShadowExecution.allocations.clear();
			fixture.row.staticActiveBytes.value = fixture.row.staticRetiringBytes.value = 0;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
				&& output.frames.at(0).at("memory").at("activeBytes").at("value") == 0,
				"Completed empty native regions retain measured zero allocation charges.");
		});
		registry.Add("Renderer.Efficiency.Editor.ShadowBatchExport.RawActualReceipt", [](TestContext& context) {
			Fixture fixture; RendererEfficiencyEvidenceExportResult output; std::string error;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error), "Complete actual-shaped group and singleton receipt exports.");
			const auto raw = Counters(fixture, output);
			context.Expect(raw.contains("shadowBatchExecution") && raw.at("shadowBatchExecution").at("allocationIdentity") == "11"
				&& raw.at("shadowBatchExecution").at("groups").at(0).at("members").at(1).at("lifetimeGeneration") == "21"
				&& raw.at("shadowBatchExecution").at("sources").at(2).at("sourceOrdinal") == 2
				&& raw.at("shadowBatchExecution").at("sources").at(0).at("cameraVisible").is_boolean()
				&& raw.at("shadowBatchExecution").at("sources").at(0).at("cameraVisible").get<bool>()
				&& raw.at("shadowBatchExecution").at("sources").at(2).at("cameraVisible").is_boolean()
				&& !raw.at("shadowBatchExecution").at("sources").at(2).at("cameraVisible").get<bool>(),
				"Raw receipt preserves full uint64 strings and typed source ordinals without inferred success.");
			fixture.row.shadowBatchExecution.groups[0].firstInstance = 1;
			fixture.row.shadowBatchExecution.instanceBytes = 528;
			fixture.row.shadowBatchExecution.sources.push_back({GenerateUUID(), 23, 3, 0, 0, true, true});
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error),
				"Independent light prefix and genuinely resolved material-excluded source remain valid.");
		});
		registry.Add("Renderer.Efficiency.Editor.ShadowBatchExport.MalformedReceiptAtomic", [](TestContext& context) {
			for(unsigned lane = 0; lane < 14; ++lane)
			{
				Fixture fixture; auto& execution = fixture.row.shadowBatchExecution;
				if(lane == 0) execution.recordedComplete = false;
				if(lane == 1) execution.frameSlotSafe = false;
				if(lane == 2) execution.uploadSucceeded = false;
				if(lane == 3) execution.allocationGeneration = 0;
				if(lane == 4) execution.groups[0].firstInstance = 2;
				if(lane == 5) execution.groups[0].members[1] = execution.groups[0].members[0];
				if(lane == 6) execution.singletons.clear();
				if(lane == 7) execution.sources[2].resolved = false;
				if(lane == 8) execution.sources[2].ownerMeshletCount = 2;
				if(lane == 9) execution.sources[2].excludedByMaterial = true;
				if(lane == 10) execution.snapshotSlot = 8;
				if(lane == 11) fixture.row.actualDrawSubmissions.value = 3;
				if(lane == 12) execution.groups[0].pipelineIdentity = 0;
				if(lane == 13) execution.sources[1].sourceOrdinal = execution.sources[0].sourceOrdinal;
				RendererEfficiencyEvidenceExportResult output; output.colorEncodingCount = 99; std::string error;
				context.Expect(!ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
					&& output.colorEncodingCount == 99 && output.frames.empty() && !std::filesystem::exists(fixture.context.outputDirectory),
					"Incomplete execution, source range, counters or group identity rejects before artifact creation.");
			}
		});
		registry.Add("Renderer.Efficiency.Editor.ShadowBatchExport.FailedUploadRetainsSingles", [](TestContext& context) {
			Fixture fixture; auto& execution = fixture.row.shadowBatchExecution;
				execution.singletons.insert(execution.singletons.begin(), execution.groups[0].members.begin(), execution.groups[0].members.end());
				execution.groups.clear(); execution.uploadSucceeded = false; execution.allocationIdentity = execution.allocationGeneration = execution.instanceBytes = 0;
				fixture.row.groups.value = 0; fixture.row.singletons.value = fixture.row.actualDrawSubmissions.value = 3;
				RendererEfficiencyEvidenceExportResult output; std::string error;
				context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error), "Recorded complete singleton fallback does not require a grouped allocation.");
				const auto raw = Counters(fixture, output);
				context.Expect(raw.contains("shadowBatchExecution") && !raw.at("shadowBatchExecution").at("uploadSucceeded").get<bool>()
					&& raw.at("shadowBatchExecution").at("singletons").size() == 3, "Failed native upload remains explicit beside every actual singleton.");
			// Reuse retains complete eligible content/source proof but issues no new
			// draw commands. Its empty submitted partition must remain honest.
			auto artifacts = std::make_shared<RendererEfficiencyArtifacts>(*fixture.row.artifacts);
			artifacts->submittedSegments.clear(); fixture.row.artifacts = artifacts;
			execution.singletons.clear(); execution.uploadAttempted = false;
			fixture.row.submittedCasterSegments.value = fixture.row.singletons.value = fixture.row.actualDrawSubmissions.value = 0;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error),
				"Native reuse validates source spans against eligible content rather than fabricated submitted draws.");
			execution.available = false; execution.recordedComplete = false; execution.sources[2].resolved = false;
			context.Expect(ExportRendererEfficiencyEvidence(fixture.context, std::span(&fixture.row, 1), output, error)
				&& !Counters(fixture, output).at("shadowBatchExecution").at("available").get<bool>(),
				"Earlier P1 observations without available batching proof remain explicitly unavailable.");
		});
	}
}
