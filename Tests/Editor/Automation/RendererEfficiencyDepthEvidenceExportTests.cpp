// These tests serialize supplied native-shaped CPU bytes. They do not execute
// shaders or establish GPU correctness, completion, illumination parity or speed.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/RendererEfficiencyEvidenceExport.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include <bit>
#include <fstream>
#include <iterator>
#include <limits>

namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace Renderer;
		using namespace Editor::Automation;
		namespace Timing = Renderer::VulkanLightingDepthTiming;
		void Word(std::vector<std::uint8_t>& bytes, std::uint32_t value)
		{
			for(unsigned i=0;i<4;++i) bytes.push_back(static_cast<std::uint8_t>(value>>(i*8)));
		}
		void OraclePayloadExtent(RendererEfficiencyLightingDepthArtifacts& depth,unsigned width,unsigned height)
		{
			// Complete CPU-native-shaped bytes isolate extent/recipe admission tests.
			depth.oracleRecipe->extent={width,height};depth.oracleInput.clear();depth.oracleCoverage.clear();
			for(unsigned i=0;i<width*height;++i)Word(depth.oracleInput,0x3f000000U);
			const float dx=float(width)/16,dy=float(height)/9;
			for(unsigned y=0;y<9;++y)for(unsigned x=0;x<16;++x){const auto count=(std::uint32_t(float(x+1)*dx)-std::uint32_t(float(x)*dx))*(std::uint32_t(float(y+1)*dy)-std::uint32_t(float(y)*dy));Word(depth.oracleCoverage,count);Word(depth.oracleCoverage,count);}
		}
		struct DepthExportFixture
		{
			RendererEfficiencyEvidenceExportContext context;
			RendererEfficiencyEvidence row;
			std::shared_ptr<RendererEfficiencyArtifacts> artifacts;
			std::shared_ptr<RendererEfficiencyLightingDepthArtifacts> depth;
			DepthExportFixture()
			{
				context.projectRoot=std::filesystem::temp_directory_path()/ ("TerraDepthExport-"+ToHexString(GenerateUUID()));
				context.operationId=ToHexString(GenerateUUID());context.fixtureId="depth-coverage";context.fixtureHash=std::string(64,'a');context.fixtureStepId="depth-coverage-00";
				context.stage="P3";context.runId="depth-export-unit";context.mode="Candidate";context.measurementPhase="Warmup";context.seed=65004;
				context.settingsGeneration=2;context.mutationGeneration=3;context.rendererSessionGeneration=4;
				context.outputDirectory=context.projectRoot/"renderer-efficiency-evidence"/context.operationId/"step0";
				std::filesystem::create_directories(context.projectRoot);
				std::ofstream(context.projectRoot/"renderer-efficiency-project.json")<<nlohmann::json{{"schemaVersion",1},{"kind","RendererEfficiencyValidationProject"},{"projectRoot",context.projectRoot.generic_string()},{"fixtureId",context.fixtureId},{"fixtureHash",context.fixtureHash}};
				row.kind=RendererEfficiencyEvidenceKind::LightingDepth;row.status=RendererEfficiencyEvidenceStatus::Valid;row.viewType=RenderViewType::Scene;row.viewId=1;row.resolution={16,9};
				row.frameSerial=5;row.commandSerial=6;row.submitSerial=row.completionSerial=7;row.fixtureControlGeneration=8;
				row.effectiveOptions.settingsGeneration=2;row.effectiveOptions.rendererSessionGeneration=4;row.effectiveOptions.optionsSessionGeneration=9;
				row.fixtureCorrelation={context.operationId,context.fixtureStepId,0,3,2,4};row.preparationInlineTasks=4;row.preparationSettled=true;
				artifacts=std::make_shared<RendererEfficiencyArtifacts>();depth=std::make_shared<RendererEfficiencyLightingDepthArtifacts>();artifacts->lightingDepth=depth;row.artifacts=artifacts;
				depth->productionWidth=16;depth->productionHeight=9;depth->oracleRecipe=LightingDepthReduction::DepthRecipe{{16,9},0x3f000000U,{}};
				for(unsigned i=0;i<144;++i){Word(depth->productionBounds,0x3f000000U);Word(depth->productionBounds,0x3f000000U);Word(depth->oracleInput,0x3f000000U);Word(depth->oracleBounds,0x3f000000U);Word(depth->oracleBounds,0x3f000000U);Word(depth->oracleCoverage,1);Word(depth->oracleCoverage,1);}
				auto color=std::make_shared<RendererEfficiencyFrameColor>();color->width=16;color->height=9;color->sourceFormat=44;color->bytes.resize(16*9*4,255);artifacts->frameColor=color;
				auto& q=depth->productionQuery;q.cooperative=true;q.computeStageMarkers=q.noWaitRead=true;q.queryReadFlags=5;q.beginStageMask=q.endStageMask=2048;
				q.result.owner={4,2,3,5,6,0,1,1,Timing::Purpose::Production,Timing::View::Scene,Timing::Scope::IsolatedDepth};q.range={4,2};
				q.result.status=Timing::Status::Valid;q.result.submitSerial=q.result.completionSerial=7;q.result.timestampValidBits=8;q.result.timestampPeriodNanoseconds=.25;
				q.result.beginTicks=0;q.result.endTicks=q.result.rawTicks=5;q.result.durationNanoseconds=1.25;q.result.beginAvailable=q.result.endAvailable=q.result.nonemptyWork=true;
				depth->oracleQuery=q;depth->oracleQuery->result.owner.purpose=Timing::Purpose::Oracle;depth->oracleQuery->range={6,2};
			}
			~DepthExportFixture(){std::error_code ignored;std::filesystem::remove_all(context.projectRoot,ignored);}
		};
		nlohmann::json DepthCounters(const DepthExportFixture& f,const RendererEfficiencyEvidenceExportResult& r)
		{
			for(const auto& a:r.artifacts) if(a.at("kind")=="Counters"){std::ifstream input(f.context.projectRoot/a.at("path").get<std::string>());return nlohmann::json::parse(input);}
			throw std::runtime_error("Missing depth counters");
		}
	}
	void RegisterRendererEfficiencyDepthEvidenceExportTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.DepthExport.CompletedOraclePreservesActualBytesAndUnroundedTiming",[](TestContext& t){
			DepthExportFixture f;RendererEfficiencyEvidenceExportResult result;std::string error;
			t.Expect(ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"Actual export accepts complete production and isolated oracle: "+error);
			if(result.frames.empty()) return;
			const auto counters=DepthCounters(f,result);
			t.Expect(result.artifacts.size()==6&&result.frames[0].at("evidenceKind")=="LightingDepth"&&result.frames[0].at("payloadPurpose")=="Production","Depth exports all distinct native artifacts without shadow identities.");
			t.Expect(counters.at("productionQuery").at("beginTicks")=="0"&&counters.at("productionQuery").at("durationNanoseconds")==1.25&&result.frames[0].at("depthStageTimingNanoseconds").at("value")==1.25,"Zero begin is legal; native fractional nanoseconds are not rounded or replaced by whole-view duration.");
			t.Expect(counters.at("oracleRecipe").at("fillBits")==0x3f000000U&&counters.at("productionNativeExtent")==nlohmann::json::array({16,9}),"Exact bit recipe and actual per-view extent remain portable.");
			for(const auto& artifact:result.artifacts){const auto path=f.context.projectRoot/artifact.at("path").get<std::string>();t.Expect(std::filesystem::exists(path)&&artifact.at("status")=="Valid","Every content-addressed artifact references a published native-shaped payload.");
				std::ifstream input(path,std::ios::binary);const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
				if(artifact.at("kind")=="ProductionDepthBounds")t.Expect(bytes==f.depth->productionBounds,"Production bounds bytes are not replaced by CPU estimates.");
				if(artifact.at("kind")=="DepthInput")t.Expect(bytes==f.depth->oracleInput,"Exact GPU-shaped input bit payload survives export.");
				if(artifact.at("kind")=="DepthBounds")t.Expect(bytes==f.depth->oracleBounds,"Exact oracle extrema survive export.");
				if(artifact.at("kind")=="DepthCoverage")t.Expect(bytes==f.depth->oracleCoverage,"Actual-load coverage bytes survive export.");}
			// Masked rollover and a zero begin counter are distinct legal cases.
			f.depth->productionQuery.result.beginTicks=250;f.depth->productionQuery.result.endTicks=3;f.depth->productionQuery.result.rawTicks=9;f.depth->productionQuery.result.durationNanoseconds=2.25;
			RendererEfficiencyEvidenceExportResult rollover;t.Expect(ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),rollover,error)&&DepthCounters(f,rollover).at("productionQuery").at("durationNanoseconds")==2.25,"Native valid-bit rollover preserves fractional duration.");
		});
		registry.Add("Renderer.Efficiency.Editor.DepthExport.UnavailableTimingRemainsExplicit",[](TestContext& t){
			DepthExportFixture f;f.depth->productionQuery.result.status=Timing::Status::Unavailable;f.depth->productionQuery.result.reason=Timing::Reason::ReadFailed;f.depth->productionQuery.result.durationNanoseconds=0;f.depth->productionQuery.result.rawTicks=0;
			f.depth->oracleQuery.reset();f.depth->oracleRecipe.reset();f.depth->oracleInput.clear();f.depth->oracleBounds.clear();f.depth->oracleCoverage.clear();
			RendererEfficiencyEvidenceExportResult result;std::string error;t.Expect(ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"Completed payload with explicit unavailable timing remains honest: "+error);
			if(!result.frames.empty())t.Expect(result.frames[0].at("depthStageTimingNanoseconds").at("status")=="Unavailable"&&DepthCounters(f,result).at("productionQuery").at("reason")=="ReadFailed","Exporter does not turn an unavailable query into speed proof.");
			// An abandoned completed pair has real configuration/owner receipts but
			// no marker/read calls. Their truthful zero flags remain unavailable.
			auto& q=f.depth->productionQuery;q.result.reason=Timing::Reason::Abandoned;q.result.beginAvailable=q.result.endAvailable=false;q.computeStageMarkers=q.noWaitRead=false;q.queryReadFlags=q.beginStageMask=q.endStageMask=0;
			RendererEfficiencyEvidenceExportResult abandoned;t.Expect(ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),abandoned,error)&&DepthCounters(f,abandoned).at("productionQuery").at("queryReadFlags")==0,"Missing calls remain explicit zero metadata, not fabricated available timing.");
		});
		registry.Add("Renderer.Efficiency.Editor.DepthExport.ReferenceFallbackRetainsRequestedAndActualMode",[](TestContext& t){
			DepthExportFixture f;f.row.fallbackReason="Actual cooperative pipeline unavailable; native complete-scan pipeline recorded.";f.depth->productionQuery.cooperative=f.depth->oracleQuery->cooperative=false;
			RendererEfficiencyEvidenceExportResult result;std::string error;t.Expect(ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"A truthful native reference fallback remains numeric evidence: "+error);
			if(!result.frames.empty()){const auto counters=DepthCounters(f,result);t.Expect(counters.at("effectiveOptions").at("lightingDepth")=="Cooperative"&&!counters.at("productionQuery").at("cooperative").get<bool>()&&counters.at("depthFallbackReason")==f.row.fallbackReason,"No fallback may relabel the requested option or manufacture cooperative speed proof.");}
			f.row.fallbackReason.clear();t.Expect(!ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"Wrong mode without native fallback reason rejects.");f.row.fallbackReason="fallback";f.depth->oracleQuery->cooperative=true;t.Expect(!ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"Oracle and production must use the same actual numerical pipeline mode.");
			f.row.effectiveOptions.options.lightingDepth=RendererEfficiencyLightingDepth::CompleteScan;f.depth->productionQuery.cooperative=true;t.Expect(!ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error),"CompleteScan request cannot silently execute cooperative mode.");
		});
		registry.Add("Renderer.Efficiency.Editor.DepthExport.RejectsMalformedPayloadAndQueryAtomically",[](TestContext& t){
			for(unsigned failure=0;failure<30;++failure){DepthExportFixture f;auto& q=f.depth->productionQuery;switch(failure){
				case 0:f.depth->productionBounds.pop_back();break;case 1:f.depth->oracleInput.pop_back();break;case 2:f.depth->oracleCoverage.pop_back();break;
				case 3:q.result.owner.commandSerial++;break;case 4:q.result.owner.rendererSessionGeneration++;break;case 5:q.result.owner.outputId++;break;
				case 6:q.result.owner.scope=Timing::Scope::WholeView;break;case 7:q.range.firstQuery++;break;case 8:q.queryReadFlags=7;break;case 9:q.beginStageMask=1;break;
				case 10:q.result.durationNanoseconds=2;break;case 11:q.result.rawTicks++;break;case 12:q.result.endAvailable=false;break;case 13:q.result.timestampValidBits=0;break;
				case 14:q.result.timestampPeriodNanoseconds=std::numeric_limits<double>::infinity();break;case 15:q.cooperative=false;break;
				case 16:f.depth->oracleRecipe->overrides={{0,0,0},{0,0,1}};break;case 17:f.depth->oracleInput[3]=0;break;case 18:f.depth->oracleCoverage[0]=0;break;
				case 19:f.depth->oracleQuery->result.owner.snapshotSlot++;break;
				case 20:f.row.resolution.x++;break;
				case 21:OraclePayloadExtent(*f.depth,32,9);break;
				case 22:{
					f.depth->productionWidth=64;f.depth->productionHeight=32;f.row.resolution={64,32};
					auto color=std::make_shared<RendererEfficiencyFrameColor>(*f.artifacts->frameColor);color->width=64;color->height=32;color->bytes.resize(64*32*4,255);f.artifacts->frameColor=color;
					OraclePayloadExtent(*f.depth,64,32);for(unsigned i=0;i<1025;++i)f.depth->oracleRecipe->overrides.push_back({i%64,i/64,0x3f000000U});break;
				}
				case 23:q.result.status=Timing::Status::Unavailable;q.result.reason=Timing::Reason::ReadFailed;q.result.timestampPeriodNanoseconds=-1;break;
				case 24:q.result.status=Timing::Status::Unavailable;q.result.reason=Timing::Reason::ReadFailed;q.result.timestampValidBits=65;break;
				case 25:f.depth->oracleCoverage[4]=2;break;
				case 26:{
					auto& u=f.row.lightingUploads[0];u.available=u.succeeded=u.coherent=true;u.allocationId=u.allocationGeneration=1;u.snapshotSlot=1;u.regionOffsetBytes=u.regionSizeBytes=176*1024;u.allocationSizeBytes=u.regionSizeBytes*8;u.nonCoherentAtomSize=256;break;
				}
				case 27:q.result.status=Timing::Status::Unavailable;q.result.reason=Timing::Reason::ReadFailed;q.queryReadFlags=7;break;
				case 28:q.result.status=Timing::Status::Unavailable;q.result.reason=Timing::Reason::ReadFailed;q.beginStageMask=1;break;
				case 29:{
					for(unsigned i=0;i<2;++i){auto& u=f.row.lightingUploads[i];u.available=u.succeeded=u.coherent=true;u.analysisPurpose=i!=0;u.allocationId=i+1;u.allocationGeneration=1;u.frameSlot=1;u.snapshotSlot=1;u.regionOffsetBytes=u.regionSizeBytes=176*1024;u.allocationSizeBytes=u.regionSizeBytes*8;u.nonCoherentAtomSize=256;}break;
				}
			} RendererEfficiencyEvidenceExportResult result;result.frames.push_back("retained");std::string error;t.Expect(!ExportRendererEfficiencyEvidence(f.context,std::span(&f.row,1),result,error)&&result.frames==nlohmann::json::array({"retained"})&&!std::filesystem::exists(f.context.outputDirectory),"Bad current native proof rejects before any output publication, lane "+std::to_string(failure)+": "+error);}
		});
		registry.Add("Renderer.Efficiency.Editor.DepthExport.MixedEvidenceRetainsShadowAndBothLightingPurposes",[](TestContext& t){
			DepthExportFixture f;auto final=f.row,analysis=f.row,shadow=f.row;final.kind=analysis.kind=RendererEfficiencyEvidenceKind::Lighting;analysis.analysisPurpose=true;
			auto lightBytes=std::make_shared<RendererEfficiencyArtifacts>(*f.artifacts);lightBytes->fullSourceCandidateCount=0;lightBytes->lightingPayload.resize(176*1024);final.artifacts=analysis.artifacts=lightBytes;
			for(unsigned i=0;i<2;++i){auto& u=final.lightingUploads[i];u.available=u.succeeded=u.coherent=true;u.analysisPurpose=i!=0;u.allocationId=i+1;u.allocationGeneration=1;u.snapshotSlot=1;u.regionSizeBytes=176*1024;u.regionOffsetBytes=u.regionSizeBytes;u.allocationSizeBytes=u.regionSizeBytes*8;u.nonCoherentAtomSize=256;}analysis.lightingUploads=final.lightingUploads;
			shadow.kind=RendererEfficiencyEvidenceKind::Shadow;shadow.lightId=GenerateUUID();shadow.resolution={1,1};shadow.contentOwner="native-shaped-shadow";
			for(auto* counter:{&shadow.eligibleCasterSegments,&shadow.submittedCasterSegments,&shadow.actualDrawSubmissions}){counter->status=RendererEfficiencyEvidenceStatus::Valid;counter->value=0;}
			auto shadowBytes=std::make_shared<RendererEfficiencyArtifacts>();shadowBytes->frameColor=f.artifacts->frameColor;shadowBytes->shadowKey.rendererSessionGeneration=4;shadowBytes->shadowKey.viewType=RenderViewType::Scene;shadowBytes->shadowKey.viewId=1;shadowBytes->shadowKey.lightId=shadow.lightId;shadowBytes->shadowKey.resourceKind=ShadowResourceKind::SpotAtlas;shadowBytes->shadowKey.extent={1,1};shadowBytes->shadowKey.allocationGeneration=1;shadowBytes->shadowKey.depthFormat=shadowBytes->depthSourceFormat=126;shadowBytes->contentToken=1;Word(shadowBytes->depthF32LE,0x3f000000U);shadowBytes->coverageU8={1};shadow.artifacts=shadowBytes;
			std::array rows{shadow,final,analysis,f.row};RendererEfficiencyEvidenceExportResult result;std::string error;t.Expect(ExportRendererEfficiencyEvidence(f.context,rows,result,error)&&result.frames.size()==4,"The mixed bundle keeps original shadow and both typed lighting purposes: "+error);
			const std::array partial{shadow,final,f.row};RendererEfficiencyEvidenceExportResult rejected;rejected.frames.push_back("retained");t.Expect(!ExportRendererEfficiencyEvidence(f.context,partial,rejected,error)&&rejected.frames==nlohmann::json::array({"retained"}),"Adding depth cannot weaken the original two-purpose lighting completeness gate.");
		});
		registry.Add("Renderer.Efficiency.Editor.DepthExport.DuplicateViewRejectedDistinctCommandsAllowed",[](TestContext& t){
			DepthExportFixture f;auto game=f.row;game.viewType=RenderViewType::Game;game.viewId=0;game.commandSerial=11;
			auto artifacts=std::make_shared<RendererEfficiencyArtifacts>(*f.artifacts);auto depth=std::make_shared<RendererEfficiencyLightingDepthArtifacts>(*f.depth);artifacts->lightingDepth=depth;game.artifacts=artifacts;
			for(auto* q:{&depth->productionQuery,&*depth->oracleQuery}){q->result.owner.view=Timing::View::Game;q->result.owner.outputId=0;q->result.owner.commandSerial=11;q->result.owner.snapshotSlot=2;q->range.firstQuery+=4;}
			std::array rows{f.row,game};RendererEfficiencyEvidenceExportResult result;std::string error;t.Expect(ExportRendererEfficiencyEvidence(f.context,rows,result,error)&&result.frames.size()==2,"Distinct same-submit Scene/Game command owners remain qualified: "+error);
			rows[1]=f.row;result.frames=nlohmann::json::array({"retained"});t.Expect(!ExportRendererEfficiencyEvidence(f.context,rows,result,error)&&result.frames==nlohmann::json::array({"retained"}),"Duplicate LightingDepth view cannot manufacture a complete cohort.");
		});
	}
}
