// Actual production ingress/admission policies are tested with copied CPU
// owners. These tests cannot establish GPU shader, timestamp or pixel proof.
#include "../TestHarness.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Tests/Support/RendererEfficiency/Renderer/RendererEfficiencyArtifacts.h"
#include "Utilities/UUID.h"
#include <fstream>
namespace TerraEngine::EditorTests
{
	namespace
	{
		using namespace TerraEngine; using namespace Editor::Automation; using Json=nlohmann::json;
		struct SealedRecipe
		{
			std::filesystem::path root=std::filesystem::temp_directory_path()/("terra-depth-"+ToHexString(GenerateUUID()));
			Json script,step;
			SealedRecipe()
			{
				std::filesystem::create_directories(root/"renderer-efficiency-depth");
				const Json recipe={{"extent",{{"width",7},{"height",5}}},{"fillBits",0x3f800000U},{"overrides",Json::array({{{"x",6},{"y",4},{"bits",0x7fc00000U}}})}};
				script={{"fixtureId","depth-coverage"},{"fixtureHash",std::string(64,'a')},{"seed",65004},{"cameras",Json::array({{{"viewType","Scene"},{"outputId",1},{"position",{0,1,8}}}})}};
				const Json file={{"schemaVersion",1},{"kind","LightingDepthInputRecipe"},{"fixtureId",script["fixtureId"]},{"fixtureHash",script["fixtureHash"]},{"seed",65004},{"fixtureStepId","depth-coverage-05"},{"viewType","Scene"},{"viewId",1},{"grid",{16,9,24}},{"recipe",recipe}};
				const std::string path="renderer-efficiency-depth/depth-coverage-05.json";
				{std::ofstream stream(root/path);stream<<file.dump();}
				std::string hash,error; if(!HashRendererEfficiencyOwnedInput(root,path,hash,error)) throw std::runtime_error(error);
				{std::ofstream stream(root/"renderer-efficiency-project.json");stream<<Json{{"generatedFiles",Json::array({{{"projectPath",path},{"sha256",hash}}})}}.dump();}
				step={{"id","depth-coverage-05"},{"nativeExtent",recipe["extent"]},{"cameraTransforms",{{"cameras",script["cameras"]},{"nativeExtent",recipe["extent"]}}},{"nativePayload",{{"depthRecipePath",path},{"depthRecipeSha256",hash},{"depthInputRecipe",recipe}}}};
			}
			~SealedRecipe(){std::error_code error;std::filesystem::remove_all(root,error);}
		};
		Renderer::RendererEfficiencyFixtureIntent Applied()
		{
			Renderer::RendererEfficiencyFixtureIntent intent;intent.controlsApplied=true;intent.controlGeneration=9;intent.correlation={"op","depth-coverage-05",0,2,4,5};
			Renderer::RendererEfficiencyFixtureCamera camera;camera.viewType=RenderViewType::Scene;camera.outputId=1;camera.nativeExtent={7,5};intent.cameras.push_back(camera);intent.depthControls.emplace();return intent;
		}
		Renderer::RendererEfficiencyEvidence Row(const Renderer::RendererEfficiencyFixtureIntent& intent)
		{
			Renderer::RendererEfficiencyEvidence row;row.kind=Renderer::RendererEfficiencyEvidenceKind::LightingDepth;row.status=Renderer::RendererEfficiencyEvidenceStatus::Valid;row.fixtureCorrelation=intent.correlation;row.fixtureControlGeneration=intent.controlGeneration;
			row.viewType=RenderViewType::Scene;row.viewId=1;row.frameSerial=10;row.commandSerial=11;row.submitSerial=12;row.completionSerial=13;row.effectiveOptions.optionsSessionGeneration=6;row.effectiveOptions.settingsGeneration=4;row.effectiveOptions.rendererSessionGeneration=5;
			auto depth=std::make_shared<Renderer::RendererEfficiencyLightingDepthArtifacts>();depth->productionWidth=7;depth->productionHeight=5;depth->productionBounds.resize(144*8);
			auto& query=depth->productionQuery;query.computeStageMarkers=query.noWaitRead=true;auto& result=query.result;result.status=Renderer::VulkanLightingDepthTiming::Status::Valid;result.submitSerial=12;result.completionSerial=13;result.owner={5,7,8,10,11,0,0,1,Renderer::VulkanLightingDepthTiming::Purpose::Production,Renderer::VulkanLightingDepthTiming::View::Scene,Renderer::VulkanLightingDepthTiming::Scope::IsolatedDepth};
			auto artifacts=std::make_shared<Renderer::RendererEfficiencyArtifacts>();artifacts->lightingDepth=depth;artifacts->frameColor=std::make_shared<Renderer::RendererEfficiencyFrameColor>();row.artifacts=artifacts;return row;
		}
		Json Bundle(){return {{"views",Json::array({{{"viewType","Scene"},{"viewId",1},{"depthPurposes",{"Production"}}}})}};}
	}
	void RegisterRendererEfficiencyDepthFixtureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.DepthFixture.SealedRecipeRejectsChangedAuthority",[](TestContext& context){
			SealedRecipe fixture;Renderer::RendererEfficiencyFixtureDepthControls output;std::string error;
			context.Expect(ParseRendererEfficiencyDepthControls(fixture.root,fixture.script,fixture.step,output,error)&&output.oracleRecipe&&output.oracleRecipe->overrides[0].bits==0x7fc00000U,"Exact sealed uint32 NaN payload remains bit-preserved without a JSON NaN literal.");const auto before=output;
			for(unsigned change=0;change<4;++change){auto step=fixture.step;auto script=fixture.script;if(change==0)step["nativePayload"]["depthInputRecipe"]["fillBits"]=0;if(change==1)step["nativePayload"]["depthRecipePath"]="../escape.json";if(change==2)script["seed"]=65005;if(change==3)step["nativePayload"]["depthRecipeSha256"]=std::string(64,'b');context.Expect(!ParseRendererEfficiencyDepthControls(fixture.root,script,step,output,error)&&output==before,"Changed hash, owner, inline bits or escaped path leaves output unchanged.");}
		});
		registry.Add("Renderer.Efficiency.DepthFixture.PerStepCamerasPreserveSealedProjection",[](TestContext& context){
			SealedRecipe fixture;auto primary=Applied().cameras;primary[0].nativeExtent={1920,1080};std::vector<Renderer::RendererEfficiencyFixtureCamera> output;std::string error;
			for(const auto extent: {glm::uvec2{1920,1080},glm::uvec2{1919,1079},glm::uvec2{7,5}}){auto step=fixture.step;step["nativeExtent"]={{"width",extent.x},{"height",extent.y}};step["cameraTransforms"]["nativeExtent"]=step["nativeExtent"];context.Expect(ParseRendererEfficiencyDepthCameras(fixture.script,step,primary,output,error)&&output[0].nativeExtent==extent&&output[0].cameraPosition==primary[0].cameraPosition,"Only sealed native extent changes; original projection/world pose retained.");}
			auto step=fixture.step;step["cameraTransforms"]["cameras"][0]["position"][0]=99;const auto before=output;context.Expect(!ParseRendererEfficiencyDepthCameras(fixture.script,step,primary,output,error)&&output==before,"Foreign camera pose cannot authorize a resize.");
		});
		registry.Add("Renderer.Efficiency.DepthFixture.CompletedDepthRequiresExactOwnerAndClosure",[](TestContext& context){
			const auto intent=Applied();const auto original=Row(intent);std::vector<Renderer::RendererEfficiencyEvidence> output;std::string error;
			context.Expect(!SelectRendererEfficiencyDepthBundle(Bundle(),intent,original.effectiveOptions,{},output,error)&&output.empty(),"Earlier purpose batch cannot authorize a missing asynchronous depth owner.");
			std::vector rows{original};context.Expect(SelectRendererEfficiencyDepthBundle(Bundle(),intent,original.effectiveOptions,rows,output,error)&&output.size()==1,"Current completed depth owner joins after its query becomes available.");
			for(unsigned change=0;change<6;++change){auto row=original;auto artifacts=std::make_shared<Renderer::RendererEfficiencyArtifacts>(*original.artifacts);auto depth=std::make_shared<Renderer::RendererEfficiencyLightingDepthArtifacts>(*artifacts->lightingDepth);artifacts->lightingDepth=depth;row.artifacts=artifacts;if(change==0)--row.fixtureControlGeneration;if(change==1)++depth->productionQuery.result.owner.commandSerial;if(change==2)depth->productionBounds.pop_back();if(change==3)depth->productionWidth=8;if(change==4)depth->productionQuery.noWaitRead=false;if(change==5)++row.effectiveOptions.optionsSessionGeneration;std::vector invalid{row};context.Expect(!SelectRendererEfficiencyDepthBundle(Bundle(),intent,original.effectiveOptions,invalid,output,error)&&output.size()==1,"Stale generation, query owner, incomplete bytes, extent and epoch fail without changing output.");}
			auto unavailable=original;unavailable.status=Renderer::RendererEfficiencyEvidenceStatus::Unavailable;unavailable.completionSerial=0;
			context.Expect(RendererEfficiencyDepthObservationUnavailable(unavailable,intent,original.effectiveOptions,0),"Exact terminal depth decision remains unavailable even without a successful completion.");
			for(unsigned change=0;change<5;++change){auto stale=unavailable;if(change==0)--stale.fixtureControlGeneration;if(change==1)stale.fixtureCorrelation.fixtureStepId="prior";if(change==2)++stale.effectiveOptions.optionsSessionGeneration;if(change==3)stale.kind=Renderer::RendererEfficiencyEvidenceKind::Shadow;if(change==4){stale.viewType=RenderViewType::Game;stale.viewId=2;}context.Expect(!RendererEfficiencyDepthObservationUnavailable(stale,intent,original.effectiveOptions,0),"Prior token, epoch, unexpected view and intentional shadow faults cannot terminate the current depth observation.");}
			rows.push_back(original);context.Expect(!SelectRendererEfficiencyDepthBundle(Bundle(),intent,original.effectiveOptions,rows,output,error),"Duplicate completed depth owners cannot replace closure.");
		});
		registry.Add("Renderer.Efficiency.DepthFixture.DelayedDepthJoinsCompletePurposeAndShadowUnion",[](TestContext& context){
			auto intent=Applied();intent.lightingControls.emplace();intent.lightingControls->snapshotSlot=0;
			auto depth=Row(intent);auto options=depth.effectiveOptions;Json bundle={{"views",Json::array({{{"viewType","Scene"},{"viewId",1},{"lightingPurposes",{"FinalLights","Analysis"}},{"depthPurposes",{"Production"}}}})}};
			std::vector<Renderer::RendererEfficiencyEvidence> rows;
			for(unsigned index=0;index<2;++index){auto row=depth;row.kind=Renderer::RendererEfficiencyEvidenceKind::Lighting;row.analysisPurpose=index==1;auto& upload=row.lightingUploads[index];upload.available=upload.succeeded=true;upload.analysisPurpose=index==1;upload.allocationId=20+index;upload.allocationGeneration=30;rows.push_back(row);}
			std::vector<Renderer::RendererEfficiencyEvidence> output;std::string error;
			context.Expect(!SelectRendererEfficiencyControlTransition(bundle,intent,options,rows,output,error),"Earlier lighting batch alone stays pending until exact delayed query owner arrives.");rows.push_back(depth);
			context.Expect(SelectRendererEfficiencyControlTransition(bundle,intent,options,rows,output,error)&&output.size()==3,"Actual complete pure Scene union has two purposes and one depth row.");
			auto sceneRows=rows;auto both=bundle;auto game=both["views"].front();game["viewType"]="Game";game["viewId"]=2;both["views"].push_back(game);auto camera=intent.cameras.front();camera.viewType=RenderViewType::Game;camera.outputId=2;intent.cameras.push_back(camera);
			for(auto row:sceneRows){row.viewType=RenderViewType::Game;row.viewId=2;row.commandSerial=14;if(row.kind==Renderer::RendererEfficiencyEvidenceKind::LightingDepth){auto artifacts=std::make_shared<Renderer::RendererEfficiencyArtifacts>(*row.artifacts);auto data=std::make_shared<Renderer::RendererEfficiencyLightingDepthArtifacts>(*artifacts->lightingDepth);data->productionQuery.result.owner.view=Renderer::VulkanLightingDepthTiming::View::Game;data->productionQuery.result.owner.outputId=2;data->productionQuery.result.owner.commandSerial=14;artifacts->lightingDepth=data;row.artifacts=artifacts;}rows.push_back(row);}
			context.Expect(SelectRendererEfficiencyControlTransition(both,intent,options,rows,output,error)&&output.size()==6,"Two views retain distinct real commands while sharing one completion.");
			// Repeated per-light view entries preserve every original shadow face;
			// normalized lighting/depth purposes are still present only once/view.
			intent.cameras.resize(1);rows=sceneRows;auto shared=bundle;shared["views"]=Json::array();
			for(unsigned light=1;light<=4;++light){const auto id=ParseUUID("6500000000000000000000000000000"+std::to_string(light)).value();Json faces=Json::array();for(unsigned face=0;face<(light==1?6U:1U);++face){auto row=depth;row.kind=Renderer::RendererEfficiencyEvidenceKind::Shadow;row.lightId=id;row.faceIndex=face;rows.push_back(row);faces.push_back(face);}shared["views"].push_back({{"viewType","Scene"},{"viewId",1},{"lightId",ToHexString(id)},{"faceIndices",faces},{"lightingPurposes",{"FinalLights","Analysis"}},{"depthPurposes",{"Production"}}});}
			context.Expect(SelectRendererEfficiencyControlTransition(shared,intent,options,rows,output,error)&&output.size()==12,"One shared view retains nine shadow faces plus both lighting purposes and depth.");
			auto sharedSceneRows=rows;auto sharedBoth=shared;for(auto view:shared["views"]){view["viewType"]="Game";view["viewId"]=2;sharedBoth["views"].push_back(view);}intent.cameras.push_back(camera);
			for(auto row:sharedSceneRows){row.viewType=RenderViewType::Game;row.viewId=2;row.commandSerial=14;if(row.kind==Renderer::RendererEfficiencyEvidenceKind::LightingDepth){auto artifacts=std::make_shared<Renderer::RendererEfficiencyArtifacts>(*row.artifacts);auto data=std::make_shared<Renderer::RendererEfficiencyLightingDepthArtifacts>(*artifacts->lightingDepth);data->productionQuery.result.owner.view=Renderer::VulkanLightingDepthTiming::View::Game;data->productionQuery.result.owner.outputId=2;data->productionQuery.result.owner.commandSerial=14;artifacts->lightingDepth=data;row.artifacts=artifacts;}rows.push_back(row);}
			context.Expect(SelectRendererEfficiencyControlTransition(sharedBoth,intent,options,rows,output,error)&&output.size()==24,"Two shared views preserve all eighteen shadow faces and six supplementary rows.");
			both=sharedBoth;
			const auto before=output.size();rows.back().completionSerial++;context.Expect(!SelectRendererEfficiencyControlTransition(both,intent,options,rows,output,error)&&output.size()==before,"A delayed query from another completion cannot substitute for the pinned cohort.");
		});
	}
}
