#include "StaticShadowFixture.h"
#include "ShadowBatchingFixture.h"
#include <algorithm>
#include <array>
#include <stdexcept>
namespace TerraEngine::Tests::RendererEfficiency
{
	StaticShadowFixture BuildStaticShadowFixture(const nlohmann::json& foundation,
		const nlohmann::json& hero,const nlohmann::json& definition,std::string_view hash,std::uint64_t seed)
	{
		using Json=nlohmann::json;
		if(!foundation.contains("objects") || !foundation.at("objects").is_array() || foundation.at("objects").size()<2) throw std::invalid_argument("P5 requires the actual foundation camera and light.");
		constexpr std::array<std::string_view,15> actions={"animate-dynamic-stable-static","overlapping-occlusion","static-to-dynamic","dynamic-to-static","remove-static","remove-dynamic","cutout-revision",
			"zero-budget","too-small-budget","whole-cube-admission","unsupported-depth-transfer","allocation-failure","lower-budget-in-flight","retirement-view-churn","morph-animation"};
		if(definition.at("id")!="static-dynamic" || definition.at("stage")!="P5" || seed!=65007 || definition.at("seed")!=seed || definition.at("steps").size()!=actions.size()) throw std::invalid_argument("P5 fixture identity/window contract drift.");
		// Reuse actual independently cooked source packages and the compatible
		// pool. The detached bootstrap is never executed as a P4 operation.
		Json bootstrap={{"id","shadow-batching"},{"stage","P4"},{"seed",65006},{"cameraIds",{"scene-fixed"}},{"parameters",{{"compatibleCasterCount",1000}}},{"steps",Json::array()}};
		constexpr std::array<std::string_view,7> batchActions={"compatible-1000","mixed-route","deformed","cutout","off-camera","dispatch-limit-split","upload-failure-fallback"};
		for(unsigned i=0;i<batchActions.size();++i)bootstrap["steps"].push_back({{"id","bootstrap-"+std::to_string(i)},{"ordinal",i},{"action",batchActions[i]},{"warmupFrames",60},{"measuredFrames",300}});
		auto inherited=BuildShadowBatchingFixture(foundation,hero,bootstrap,hash,65006);
		StaticShadowFixture output{std::move(inherited.scene),std::move(inherited.steps),std::move(inherited.files)};
		Json pool=Json::array();
		for(auto& object:output.scene.at("objects")) {
			const auto id=object.at("uuid").get<std::string>();
			if(id.starts_with("6560000000000000")) {pool.push_back(id);object["isStatic"]=true;}
			if(object.at("components").contains("AnimatorComponent")) object["components"]["AnimatorComponent"]["loopDefaultClip"]=true;
		}
		if(pool.size()!=1000)throw std::runtime_error("P5 requires1000 actual imported static actors.");
		const std::string pose="65000000000000000000000000000021",morph="65000000000000000000000000000031";
		auto& script=output.steps;script["fixtureId"]="static-dynamic";script["seed"]=seed;script["stage"]="P5";script["selectedStage"]="P5";script["manifestStage"]="P5";
		script.erase("shadowBatchFixture");script["staticShadowFixture"]=true;script["fullManifestStepCount"]=actions.size();script["steps"]=Json::array();script["selectedManifestStepIds"]=Json::array();script["animationAuthoritySetup"]["appliesTo"]="AllP5Steps";
		Json baseline=Json::array();
		const auto firstStatic=std::find_if(output.scene.at("objects").begin(),output.scene.at("objects").end(),[&](const auto& object){return object.at("uuid")==pool[0];});
		const Json originalPose=firstStatic->at("components").at("Transform").at("WorldPose");
		for(const auto& id:pool) {
			baseline.push_back({{"operation","SetActive"},{"objectId",id},{"active",true}});baseline.push_back({{"operation","SetStatic"},{"objectId",id},{"isStatic",true}});
			for(const char* property:{"meshId","materialId"})baseline.push_back({{"operation","BindPublishedResource"},{"objectId",id},{"propertyName",property},{"bindingKey",std::string("static")+(std::string(property)=="meshId"?".mesh":".material")}});
		}
		for(const auto& id:{pose,morph}) {baseline.push_back({{"operation","SetActive"},{"objectId",id},{"active",true}});baseline.push_back({{"operation","SetStatic"},{"objectId",id},{"isStatic",false}});}
		for(unsigned light:{40U,41U,42U,43U})baseline.push_back({{"operation","SetActive"},{"objectId",std::string("650000000000000000000000000000")+std::to_string(light)},{"active",light==41}});
		baseline.push_back({{"operation","SetWorldTransform"},{"objectId",pool[0]},{"worldPose",originalPose}});
		for(unsigned i=0;i<actions.size();++i) {
			Json step=definition.at("steps").at(i);const auto action=actions[i];if(step.at("ordinal")!=i || step.at("action").get<std::string>()!=action || step.at("warmupFrames")!=60 || step.at("measuredFrames")!=300)throw std::invalid_argument("P5 step identity/count drift.");
			Json selected=pool,requests=Json::array();selected.push_back(i==14?morph:pose);
			if(i==1) {auto overlapping=originalPose;overlapping["position"]["offsetMetres"]={-1,0,0};overlapping["scale"]={1,1,1};requests.push_back({{"operation","SetWorldTransform"},{"objectId",pool[0]},{"worldPose",overlapping}});}
			if(i==2 || i==3)requests.push_back({{"operation","SetStatic"},{"objectId",i==2?pool[0]:Json(pose)},{"isStatic",i==3}});
			if(i==4 || i==5) {const Json removed=i==4?pool[0]:Json(pose);requests.push_back({{"operation","SetActive"},{"objectId",removed},{"active",false}});selected.erase(std::find(selected.begin(),selected.end(),removed));}
			if(i==6)requests.push_back({{"operation","BindPublishedResource"},{"objectId",pool[0]},{"propertyName","materialId"},{"bindingKey","cutout.material"}});
			const bool cube=i==9; if(cube) {requests.push_back({{"operation","SetActive"},{"objectId","65000000000000000000000000000040"},{"active",true}});requests.push_back({{"operation","SetActive"},{"objectId","65000000000000000000000000000041"},{"active",false}});}
			Json views=Json::array();for(unsigned view=1;view<=(i==13?2U:1U);++view)views.push_back({{"viewType",view==1?"Scene":"Game"},{"viewId",view},{"lightId",cube?"65000000000000000000000000000040":"65000000000000000000000000000041"},{"lightType",cube?1:0},{"faceIndices",cube?Json{0,1,2,3,4,5}:Json{0}}});
			step["requiredBundle"]={{"views",views}};
			step["nativePayload"]={{"baselineTransactionRequests",baseline},{"transactionRequests",requests},{"casterObjectIds",selected},{"seekTick",30},{"animatePerSample",i!=5},
				{"staticShadowCacheBudgetBytes",i==7?0:i==8?1:67108864},{"staticShadowControls",{{"forceTransferUnavailable",i==10},{"failAllocation",i==11}}}};
			// Command ownership belongs to the actual prefab animator root; renderer
			// palette/weight ownership belongs to its selected mesh child21/31.
			step["nativePayload"]["animationTargetObjectId"]=i==14?"65000000000000000000000000000030":"65000000000000000000000000000020";
			if(i==12)step["nativePayload"]["lowerBudgetWhilePendingBytes"]=0;
			if(i==6) {step["nativePayload"]["sourcePath"]="Models/RendererEfficiency_cutout.gltf";step["nativePayload"]["replacementPath"]="renderer-efficiency-variants/cutout-cutout.json";}
			step["support"]={{"status","RequiresRuntimeCapability"},{"reason","Actual static allocation/transfer, CPU animation publications and complete native final depth owners must prove every requested frame."}};
			script["selectedManifestStepIds"].push_back(step.at("id"));script["steps"].push_back(std::move(step));
		}
		output.files.erase(std::remove_if(output.files.begin(),output.files.end(),[](const auto& file){return file.first=="renderer-efficiency-steps.json";}),output.files.end());output.files.emplace_back("renderer-efficiency-steps.json",script);return output;
	}
}
