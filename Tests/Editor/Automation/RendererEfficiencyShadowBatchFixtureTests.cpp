// CPU authoring/ingress tests. Actual Vulkan grouping and pixel parity remain
// independent installed native acceptance requirements.
#include "../TestHarness.h"
#include "Tests/Tools/RendererEfficiency/ShadowBatchingFixture.h"
#include "Tests/Tools/RendererEfficiency/ShadowInvalidationFixture.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Utilities/UUID.h"
#include "Utilities/PlatformUtils.h"
#include <fstream>
#include <set>
namespace TerraEngine::EditorTests
{
	namespace
	{
		using Json=nlohmann::json;
		Json Read(const char* relativePath)
		{
			// Installed harnesses use their own sealed fixture closure. Match the
			// executable-relative ancestor discovery used by editor export tests;
			// once a seal is found, missing files must fail rather than use source.
			std::filesystem::path path;
			for(auto root=PlatformUtils::GetExecutablePath().parent_path();!root.empty();root=root.parent_path()) {
				for(const auto& candidate:{root,root/"Build"}) {
					if(!std::filesystem::is_regular_file(candidate/"Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json")) continue;
					path=candidate/relativePath;break;
				}
				if(!path.empty() || root==root.parent_path()) break;
			}
			// An explicitly compiled standalone authoring test may use committed
			// source inputs when its executable has no installed runtime ancestor.
			if(path.empty()) path=std::filesystem::path(TERRAENGINE_SOURCE_DIR)/relativePath;
			std::ifstream stream(path,std::ios::binary);
			if(!stream) throw std::runtime_error("Cannot read renderer fixture: "+path.generic_string());
			Json value;stream>>value;return value;
		}
		auto Fixture()
		{
			const auto manifest=Read("Tests/Fixtures/RendererEfficiency/workloads.json");
			const auto found=std::find_if(manifest.at("workloads").begin(),manifest.at("workloads").end(),[](const auto& value){return value.at("id")=="shadow-batching";});
			Json foundation={{"objects",Json::array({{{"uuid","65000000000000000000000000000001"},{"components",Json::object()}},{{"uuid","65000000000000000000000000000002"},{"active",true},{"components",{{"LightComponent",{{"castsShadows",true}}}}}}})}};
			return TerraEngine::Tests::RendererEfficiency::BuildShadowBatchingFixture(foundation,Read("Tests/Fixtures/Animation/ImportValidation/Assets/Models/ImportedHero.gltf"),*found,std::string(64,'a'),65006);
		}
		void SharedStagesPreserveRealPressureAndUnsupportedOwners(TestContext& context)
		{
			const auto manifest=Read("Tests/Fixtures/RendererEfficiency/workloads.json");
			const auto definition=*std::find_if(manifest.at("workloads").begin(),manifest.at("workloads").end(),[](const auto& value){return value.at("id")=="recovery-capacity";});
			Json foundation={{"objects",Json::array({{{"uuid","65000000000000000000000000000001"},{"components",Json::object()}},{{"uuid","65000000000000000000000000000002"},{"active",true},{"components",{{"LightComponent",{{"castsShadows",true}}}}}}})}};
			const auto fixture=TerraEngine::Tests::RendererEfficiency::BuildShadowInvalidationFixture(foundation,Read("Tests/Fixtures/Animation/ImportValidation/Assets/Models/ImportedHero.gltf"),definition,std::string(64,'a'),65009,"P4");
			context.Expect(fixture.steps.at("steps").size()==8,"P4 retains eight actual recovery actions.");
			for(const auto& step:fixture.steps.at("steps")) {
				const auto& payload=step.at("nativePayload");const auto action=step.at("action").get<std::string>();
				context.Expect(payload.at("baselineCandidateOrder").size()==5,"P4 uses five real baseline lights.");
				context.Expect(payload.at("candidateOrder").size()==(action=="buffer-overflow"?1025U:5U),"Buffer pressure is lights, not caster count.");
				for(const auto& view:step.at("requiredBundle").at("views")) context.Expect(view.contains("lightingPurposes") && !view.contains("depthPurposes"),"P4 shared shadow/lighting union retains no synthetic depth.");
				if(action=="group-overflow") context.Expect(payload.at("casterObjectIds").size()==1000 && payload.at("shadowBatchControls").at("maxInstancesPerGroup")==64,"Real pool exceeds actual group limit.");
				if(action=="unsupported-grouped-route") context.Expect(payload.at("casterObjectIds").size()==2 && payload.at("seekTick")==30,"Actual CPU pose and morph owners supply conservative route.");
				TerraEngine::Renderer::RendererEfficiencyFixtureIntent controls;std::string error;
				context.Expect(TerraEngine::Editor::Automation::ParseRendererEfficiencyShadowBatchControls(step,controls,error,false),"Shared controls use real bounded selected targets.");
				if(action=="unsupported-grouped-route") {
					context.Expect(!TerraEngine::Editor::Automation::ParseRendererEfficiencyShadowBatchControls(step,controls,error),"Dedicated1000 contract cannot be weakened by shared two-owner recipe.");
					auto invalid=step;invalid["nativePayload"]["casterObjectIds"].push_back(invalid["nativePayload"]["casterObjectIds"][0]);
					context.Expect(!TerraEngine::Editor::Automation::ParseRendererEfficiencyShadowBatchControls(invalid,controls,error,false),"Shared duplicate identities reject atomically.");
				}
			}
		}

		void RealSharedOwnersAndWindows(TestContext& context)
		{
			const auto fixture=Fixture(); const auto repeated=Fixture();
			context.Expect(fixture.scene==repeated.scene && fixture.files==repeated.files,"Detached bytes must be deterministic.");
			context.Expect(fixture.steps.at("steps").size()==7,"All frozen steps survive.");
			std::set<std::string> ids;
			for(const auto& object:fixture.scene.at("objects")) context.Expect(ids.insert(object.at("uuid")).second,"Actual root object UUIDs are unique.");
			for(const auto& step:fixture.steps.at("steps")) {
				context.Expect(step.at("warmupFrames")==60 && step.at("measuredFrames")==300,"Frozen windows remain exact.");
				context.Expect(step.at("nativePayload").at("casterObjectIds").size()==1000,"Exactly1000 actual caster targets.");
				std::set<std::string> targets;for(const auto& id:step.at("nativePayload").at("casterObjectIds")) context.Expect(targets.insert(id).second,"No duplicate selected caster.");
			}
			for(const auto& file:fixture.files) if(file.first.extension()==".meta") context.Expect(file.second.at("lastModified")==0 && file.second.at("compiledPath")=="","Normal first-cook metadata preserves native package identity.");
		}
		void RealRoutesCutoutAndAllRegions(TestContext& context)
		{
			const auto fixture=Fixture(); const auto& steps=fixture.steps.at("steps");
			context.Expect(steps[4].at("requiredBundle").at("views").size()==8,"Both actual views retain all four real light kinds.");
			unsigned regions=0;for(const auto& view:steps[4].at("requiredBundle").at("views")) regions+=static_cast<unsigned>(view.at("faceIndices").size());
			context.Expect(regions==18,"All point faces plus three2D regions per view.");
			context.Expect(steps[5].at("nativePayload").at("shadowBatchControls").at("maxInstancesPerGroup")==64,"Real bounded split request.");
			context.Expect(steps[6].at("nativePayload").at("shadowBatchControls").at("failInstanceUpload")==true,"Only actual upload seam requested.");
			bool mask=false,dense=false;for(const auto& file:fixture.files) {
				if(file.first.generic_string()=="Assets/Models/RendererEfficiency_cutout.gltf") mask=file.second.at("materials")[0].at("alphaMode")=="MASK" && file.second.at("textures").size()==1;
				if(file.first.generic_string()=="Assets/Models/RendererEfficiency_grid.gltf") dense=file.second.at("accessors")[0].at("count")==4225;
			}
			context.Expect(mask && dense,"Actual cookable alpha texture and dense source, not route flags.");
		}
		void TypedControlsRejectDuplicateAndUnownedOutput(TestContext& context)
		{
			using namespace TerraEngine::Editor::Automation;
			const auto fixture=Fixture();auto step=fixture.steps.at("steps")[4];TerraEngine::Renderer::RendererEfficiencyFixtureIntent intent;std::string error;
			context.Expect(ParseRendererEfficiencyShadowBatchControls(step,intent,error),"Actual selected1000 control ingress must succeed.");
			context.Expect(intent.casterSelections.size()==2 && intent.casterSelections[0].objectIds.size()==1000,"Both real views retain full UUIDs.");
			const auto prior=intent;step["nativePayload"]["casterObjectIds"][999]=step["nativePayload"]["casterObjectIds"][0];
			context.Expect(!ParseRendererEfficiencyShadowBatchControls(step,intent,error) && intent.casterSelections==prior.casterSelections,"Duplicate rejection is atomic.");
			step=fixture.steps.at("steps")[4];step["requiredBundle"]["views"][0]["viewId"]=7;
			context.Expect(!ParseRendererEfficiencyShadowBatchControls(step,intent,error),"No invented native output IDs.");
			step=fixture.steps.at("steps")[5];step["nativePayload"]["shadowBatchControls"]["maxInstancesPerGroup"]=65537;
			context.Expect(!ParseRendererEfficiencyShadowBatchControls(step,intent,error),"Physical admission bounds remain strict.");
			step=fixture.steps.at("steps")[6];context.Expect(ParseRendererEfficiencyShadowBatchControls(step,intent,error),"Native one-shot request is retained for transition.");
			const auto measured=RendererEfficiencyShadowBatchMeasurementIntent(intent);
			context.Expect(intent.shadowBatchControls->failInstanceUpload && !measured.shadowBatchControls->failInstanceUpload && measured.casterSelections==intent.casterSelections,"Only subsequent measurement disarms seam; original receipt remains unchanged.");
			const auto manifest=Read("Tests/Fixtures/RendererEfficiency/workloads.json");Json script={{"cameras",manifest.at("cameras")},{"nativeExtent",{{"width",1920},{"height",1080}}}};
			step=fixture.steps.at("steps")[4];auto cameras=script.at("cameras");cameras[0]["target"][0]=12;cameras[0]["target"][1]=-0.1;step["cameraTransforms"]={{"cameras",cameras},{"nativeExtent",script["nativeExtent"]}};
			std::vector<TerraEngine::Renderer::RendererEfficiencyFixtureCamera> primary(2),selected;
			primary[0].viewType=RenderViewType::Scene;primary[0].outputId=1;primary[0].cameraPosition.offsetMetres={0,4,12};primary[0].lookAtTarget.offsetMetres={0,1,0};primary[1].viewType=RenderViewType::Game;primary[1].outputId=2;
			context.Expect(ParseRendererEfficiencyShadowBatchCameras(script,step,primary,selected,error) && selected.size()==2 && selected[0].lookAtTarget.offsetMetres.x==12,"Actual selected camera recipe preserves real view identity.");
			step["cameraTransforms"]["cameras"][0]["up"][0]=1;
			context.Expect(!ParseRendererEfficiencyShadowBatchCameras(script,step,primary,selected,error),"Unsealed perstep camera mutation rejects.");
		}
		void DetachedPublishedBindingsRejectMissingOwner(TestContext& context)
		{
			using namespace TerraEngine;using namespace Editor::Automation;
			const auto fixture=Fixture();const auto& requests=fixture.steps.at("steps")[3].at("nativePayload").at("transactionRequests");
			std::map<std::string,UUID> bindings={{"cutout.mesh",ParseUUID(std::string(32,'a')).value()},{"cutout.material",ParseUUID(std::string(32,'b')).value()}};
			Scene::Transaction transaction;std::string error;
			context.Expect(PrepareRendererEfficiencyShadowBatchTransaction(requests,bindings,transaction,error),"Copied actual named owners become typed writes.");
			context.Expect(transaction.writeSet.operations.size()==requests.size(),"Every authored active/binding write retained.");
			const auto count=transaction.writeSet.operations.size();bindings.erase("cutout.material");
			context.Expect(!PrepareRendererEfficiencyShadowBatchTransaction(requests,bindings,transaction,error) && transaction.writeSet.operations.size()==count,"Missing actual owner cannot partially replace output.");
		}
	}
	void RegisterRendererEfficiencyShadowBatchFixtureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShadowBatchFixture.SharedStagesPreserveRealPressureAndUnsupportedOwners",SharedStagesPreserveRealPressureAndUnsupportedOwners);
		registry.Add("Renderer.Efficiency.ShadowBatchFixture.RealSharedOwnersAndWindows",RealSharedOwnersAndWindows);
		registry.Add("Renderer.Efficiency.ShadowBatchFixture.RealRoutesCutoutAndAllRegions",RealRoutesCutoutAndAllRegions);
		registry.Add("Renderer.Efficiency.ShadowBatchFixture.TypedControlsRejectDuplicateAndUnownedOutput",TypedControlsRejectDuplicateAndUnownedOutput);
		registry.Add("Renderer.Efficiency.ShadowBatchFixture.DetachedPublishedBindingsRejectMissingOwner",DetachedPublishedBindingsRejectMissingOwner);
	}
}
