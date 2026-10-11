#include "../TestHarness.h"
#include "Tests/Tools/RendererEfficiency/StaticShadowFixture.h"
#include "Tests/Tools/RendererEfficiency/ShadowInvalidationFixture.h"
#include "Utilities/PlatformUtils.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include <fstream>
#include <algorithm>
namespace TerraEngine::EditorTests
{
	namespace
	{
		nlohmann::json StaticFixtureInput(const std::filesystem::path& path)
		{
			// Installed tests must consume installed closure; explicit source fallback
			// exists only for standalone authoring tests outside an installed harness.
			for(auto root=std::filesystem::path(PlatformUtils::GetExecutablePath()).parent_path();!root.empty();)
			{
				for(const auto& candidate:{root,root/"Build"}) if(std::filesystem::exists(candidate/"Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json")) {
					std::ifstream input(candidate/path); if(!input) throw std::runtime_error("Installed static fixture input is missing.");return nlohmann::json::parse(input);
				}
				const auto parent=root.parent_path();if(parent==root)break;root=parent;
			}
			std::ifstream input(std::filesystem::path(TERRAENGINE_SOURCE_DIR)/path);if(!input)throw std::runtime_error("Static fixture authoring input is missing.");return nlohmann::json::parse(input);
		}
	}
	void RegisterRendererEfficiencyStaticShadowFixtureTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.Editor.StaticFixture.TypedCategoryAndActualLoopingTicks",[](TestContext& context){
			using namespace TerraEngine::Editor::Automation;
			using namespace TerraEngine::Animation;
			const auto id=TerraEngine::GenerateUUID();TerraEngine::Scene::Transaction transaction;std::string error;
			const nlohmann::json request=nlohmann::json::array({{{"operation","SetStatic"},{"objectId",TerraEngine::ToHexString(id)},{"isStatic",true}}});
			context.Expect(PrepareRendererEfficiencyStaticShadowTransaction(request,{},transaction,error) && transaction.writeSet.operations.size()==1
				&& transaction.writeSet.operations[0].kind==TerraEngine::Scene::TransactionWriteKind::SetStatic && transaction.writeSet.operations[0].boolValue,"Category authoring must decode into actual typed SetStatic writes.");
			auto invalid=request;invalid[0]["isStatic"]=1;const auto original=transaction.writeSet.operations[0];
			context.Expect(!PrepareRendererEfficiencyStaticShadowTransaction(invalid,{},transaction,error) && transaction.writeSet.operations.size()==1 && transaction.writeSet.operations[0].boolValue==original.boolValue,"Malformed category cannot partially replace a prepared transaction.");
			Renderer::RendererEfficiencyStaticShadowAllocation owner;owner.sessionGeneration=2;owner.backendGeneration=3;owner.allocationIdentity=4;owner.allocationGeneration=5;
			Renderer::RendererEfficiencyStaticShadowExecution live;live.available=true;live.backendGeneration=3;live.allocations.push_back(owner);live.allocations[0].uses.push_back({6,7,1});live.acceptedSubmissions.push_back({6,7,1});
			context.Expect(RendererEfficiencyStaticOwnerHasOutstandingUse(live,owner),"Only a current exact accepted-not-completed native owner may precede in-flight lowering.");
			live.completedSubmissions.push_back(7);context.Expect(!RendererEfficiencyStaticOwnerHasOutstandingUse(live,owner),"Completed native use is not in-flight proof.");
			live.completedSubmissions.clear();live.allocations[0].retiring=true;context.Expect(!RendererEfficiencyStaticOwnerHasOutstandingUse(live,owner),"Already retired logical owner cannot authorize lowering.");
			live.allocations[0].retiring=false;live.allocations[0].allocationGeneration=9;context.Expect(!RendererEfficiencyStaticOwnerHasOutstandingUse(live,owner),"Different allocation generation cannot stand in for the accepted qualified owner.");
			AnimatorPlayback playback;AnimatorPlaybackConfiguration configuration;configuration.durationTicks=60;configuration.loop=true;
			context.Expect(playback.Configure(configuration)==AnimationStatus::Ok,"Actual looping playback must be configured for its real one-second clip.");
			for(std::uint64_t frame=0;frame<360;++frame) {
				const auto tick=RendererEfficiencyStaticSampleTick(frame);
				context.Expect(tick==31+static_cast<std::int64_t>(frame) && playback.Seek(tick)==AnimationStatus::Ok,"Every accepted frame must request its own actual tick rather than repeatSeek30.");
				const auto snapshot=playback.Snapshot();context.Expect(snapshot.absoluteTick==tick && snapshot.loopCount==tick/60 && snapshot.resolvedTick==tick%60 && snapshot.runState!=AnimatorPlaybackRunState::Playing,"Actual native loop count/time must preserve deterministic absolute tick while paused or stopped.");
			}
			bool rejected=false;try{(void)RendererEfficiencyStaticSampleTick(360);}catch(const std::invalid_argument&){rejected=true;}context.Expect(rejected,"No extra acceptance sample may be manufactured beyond the frozen window.");
		});
		registry.Add("Renderer.Efficiency.Editor.StaticFixture.RealStaticPoolAndLoopingAnimator",[](TestContext& context){
			const auto manifest=StaticFixtureInput("Tests/Fixtures/RendererEfficiency/workloads.json");
			const auto definition=*std::find_if(manifest.at("workloads").begin(),manifest.at("workloads").end(),[](const auto& value){return value.at("id")=="static-dynamic";});
			const auto hero=StaticFixtureInput("Tests/Fixtures/Animation/ImportValidation/Assets/Models/ImportedHero.gltf");
			const nlohmann::json foundation={{"objects",nlohmann::json::array({{{"uuid","65000000000000000000000000000001"},{"components",nlohmann::json::object()}},{{"uuid","65000000000000000000000000000002"},{"active",true},{"components",{{"LightComponent",{{"castsShadows",true}}}}}}})}};
			const auto fixture=TerraEngine::Tests::RendererEfficiency::BuildStaticShadowFixture(foundation,hero,definition,std::string(64,'a'),65007);
			context.Expect(fixture.steps.at("staticShadowFixture")==true && fixture.steps.at("steps").size()==definition.at("steps").size(),"Every frozen and appended step must retain its original identity.");
			std::size_t statics=0,looping=0;
			for(const auto& object:fixture.scene.at("objects")) {
				if(object.at("uuid").template get<std::string>().starts_with("6560000000000000")) {++statics;context.Expect(object.at("isStatic")==true && object.at("components").contains("MeshRenderComponent"),"Every static actor must be a real authored native mesh owner.");}
				if(object.at("components").contains("AnimatorComponent")) {++looping;context.Expect(object.at("components").at("AnimatorComponent").at("loopDefaultClip")==true,"Repeated actual Seek ticks must loop instead of freezing at the clip end.");}
			}
			context.Expect(statics==1000 && looping>=1,"Static1000 and real imported animator inputs must exist.");
			const auto& first=fixture.steps.at("steps").at(0);context.Expect(first.at("nativePayload").at("animatePerSample")==true && first.at("nativePayload").at("seekTick")==30,"Stable static test must use actual per-sample native animation, not a metadata tick.");
			const auto& category=fixture.steps.at("steps").at(2).at("nativePayload").at("transactionRequests");
			context.Expect(std::any_of(category.begin(),category.end(),[](const auto& row){return row.at("operation")=="SetStatic" && row.at("isStatic")==false;}),"Static-to-dynamic must be an actual typed category edit.");
			const auto& cube=*std::find_if(fixture.steps.at("steps").begin(),fixture.steps.at("steps").end(),[](const auto& row){return row.at("action")=="whole-cube-admission";});
			context.Expect(cube.at("requiredBundle").at("views").at(0).at("faceIndices").size()==6,"Whole cube must retain all six native faces.");
			for(const auto* name:{"multiview-lights","recovery-capacity"}) {
				const auto sharedDefinition=*std::find_if(manifest.at("workloads").begin(),manifest.at("workloads").end(),[&](const auto& value){return value.at("id")==name;});
				const auto shared=Tests::RendererEfficiency::BuildShadowInvalidationFixture(foundation,hero,sharedDefinition,std::string(64,'a'),sharedDefinition.at("seed"),"P5");
				context.Expect(shared.steps.at("steps").size()==(std::string(name)=="recovery-capacity"?14U:5U),"P5 shared stage must preserve all original and appended actions.");
				for(const auto& row:shared.steps.at("steps")) {
					const auto& payload=row.at("nativePayload");context.Expect(payload.at("baselineCandidateOrder").size()==5 && payload.at("candidateOrder").size()==(row.at("action")=="buffer-overflow"?1025U:5U),"P5 light pressure stays independent of static caster count.");
					for(const auto& view:row.at("requiredBundle").at("views"))context.Expect(view.contains("lightingPurposes") && !view.contains("depthPurposes"),"P5 shared union preserves shadow and two actual lighting purposes only.");
					if(row.at("action")=="group-overflow")context.Expect(payload.at("casterObjectIds").size()==1000,"Only explicit group overflow selects the real static1000 pool.");
				}
			}
		});
	}
}
