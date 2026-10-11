#include "ShadowBatchingFixture.h"
#include "ShadowInvalidationFixture.h"
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace TerraEngine::Tests::RendererEfficiency
{
	ShadowBatchingFixture BuildShadowBatchingFixture(const nlohmann::json& foundation,
		const nlohmann::json& hero, const nlohmann::json& definition, std::string_view hash, std::uint64_t seed)
	{
		using Json=nlohmann::json;
		constexpr std::array<std::string_view,7> actions={"compatible-1000","mixed-route","deformed","cutout","off-camera","dispatch-limit-split","upload-failure-fallback"};
		if(definition.at("id")!="shadow-batching" || definition.at("stage")!="P4" || seed!=65006 || definition.at("seed")!=seed
			|| definition.at("parameters").at("compatibleCasterCount")!=1000 || definition.at("steps").size()!=7 || hash.size()!=64
			|| !std::all_of(hash.begin(),hash.end(),[](char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');})) throw std::invalid_argument("P4 fixture identity/count contract drift.");
		// Reuse the real independently cooked P1 sources and animation bindings.
		// This detached bootstrap never runs P1 steps or claims their evidence.
		Json bootstrap={{"id","shadow-invalidation"},{"stage","P1"},{"cameraIds",{"scene-fixed"}},{"steps",Json::array()}};
		for(unsigned i=0;i<17;++i) bootstrap["steps"].push_back({{"id","bootstrap-"+std::to_string(i)},{"ordinal",i},{"action","stable"}});
		auto inherited=BuildShadowInvalidationFixture(foundation,hero,bootstrap,hash,seed);
		ShadowBatchingFixture output{std::move(inherited.scene),std::move(inherited.steps),std::move(inherited.files)};
		output.files.erase(std::remove_if(output.files.begin(),output.files.end(),[](const auto& item){return item.first=="renderer-efficiency-steps.json";}),output.files.end());
		Json dense;
		for(const auto& item:output.files) if(item.first=="renderer-efficiency-variants/grid-page-baseline.json") dense=item.second;
		for(auto& item:output.files) {
			if(item.first=="Assets/Models/RendererEfficiency_grid.gltf") item.second=dense;
			// A single actual primitive/material gives compatible actors one segment,
			// rather than falsely asserting1000 submissions for2000 source sections.
			if(item.first=="Assets/Models/RendererEfficiency_static.gltf" || item.first=="Assets/Models/RendererEfficiency_cutout.gltf") {
				item.second["meshes"][0]["primitives"].erase(item.second["meshes"][0]["primitives"].begin()+1,item.second["meshes"][0]["primitives"].end());
				item.second["materials"].erase(item.second["materials"].begin()+1,item.second["materials"].end());
			}
		}
		Json pool=Json::array(); auto& roles=output.steps["targets"];
		const auto source=std::find_if(output.scene["objects"].begin(),output.scene["objects"].end(),[](const auto& object){return object.at("uuid")=="65000000000000000000000000000010";});
		const Json model=*source;
		for(auto& object:output.scene["objects"]) {
			const auto id=object.at("uuid").get<std::string>();
			object["active"]=id=="65000000000000000000000000000001" || id=="65000000000000000000000000000003" || id=="65000000000000000000000000000041"
				|| id=="65000000000000000000000000000020" || id=="65000000000000000000000000000030";
			if(object.at("components").contains("LightComponent")) {object["components"]["LightComponent"]["range"]=100;object["components"]["LightComponent"]["castsShadows"]=true;}
			if(id=="65000000000000000000000000000003") {
				object["components"]["Transform"]["WorldPose"]["scale"]={20,1,20};
				object["components"]["Transform"]["ParentRelativePose"]["scale"]={20,1,20};
			}
		}
		for(unsigned i=0;i<1000;++i) {
			std::ostringstream id;id<<"6560000000000000"<<std::hex<<std::setw(16)<<std::setfill('0')<<i+1;
			const std::string key="batch-"+std::to_string(i); Json object=model;object["uuid"]=id.str();object["name"]="Batch Caster "+std::to_string(i);object["active"]=true;
			const Json position={(int(i%32)-16)*0.18,0.0,(int(i/32)-16)*0.18};
			auto& transform=object["components"]["Transform"];transform["WorldPose"]["position"]["offsetMetres"]=position;transform["ParentRelativePose"]["translationMetres"]=position;
			transform["WorldPose"]["scale"]={0.025,0.025,0.025};transform["ParentRelativePose"]["scale"]={0.025,0.025,0.025};
			output.scene["objects"].push_back(std::move(object));pool.push_back(id.str());
			roles[key]={{"objectId",id.str()},{"componentName","MeshRenderComponent"},{"assetBindings",{{"meshId","static.mesh"},{"materialId","static.material"}}}};
		}
		output.steps["fixtureId"]="shadow-batching";output.steps["stage"]="P4";output.steps["selectedStage"]="P4";output.steps["manifestStage"]="P4";output.steps["fullManifestStepCount"]=7;output.steps["shadowBatchFixture"]=true;
		output.steps["selectedManifestStepIds"]=Json::array();output.steps["excludedManifestStepIds"]=Json::array();output.steps["steps"]=Json::array();
		output.steps["animationAuthoritySetup"]["appliesTo"]="AllP4Steps";
		for(unsigned i=0;i<7;++i) {
			Json step=definition.at("steps")[i];const auto action=actions[i];
			if(step.at("action").get<std::string>()!=action || step.at("ordinal")!=i || step.at("warmupFrames")!=60 || step.at("measuredFrames")!=300) throw std::invalid_argument("P4 action/window contract drift.");
			Json selected=pool,requests=Json::array();
			for(unsigned index=0;index<1000;++index) {
				const bool active=i!=2 || index<998;
				requests.push_back({{"operation","SetActive"},{"objectId",pool[index]},{"active",active}});
				const std::string role=i==3?"cutout":i==1 && index>=500?"grid":"static";
				for(const char* property:{"meshId","materialId"}) requests.push_back({{"operation","BindPublishedResource"},{"objectId",pool[index]},{"propertyName",property},{"bindingKey",role+(std::string(property)=="meshId"?".mesh":".material")}});
			}
			if(i==2) {selected.erase(selected.end()-2,selected.end());selected.push_back("65000000000000000000000000000021");selected.push_back("65000000000000000000000000000031");}
			for(const unsigned light:{40U,41U,42U,43U}) requests.push_back({{"operation","SetActive"},{"objectId",std::string("650000000000000000000000000000")+std::to_string(light)},{"active",i==4 || light==41}});
			for(const unsigned root:{20U,30U}) requests.push_back({{"operation","SetActive"},{"objectId",std::string("650000000000000000000000000000")+std::to_string(root)},{"active",true}});
			Json bundle={{"views",Json::array()}};
			for(unsigned view=1;view<=(i==4?2U:1U);++view) for(unsigned light:{41U,40U,42U,43U}) {
				if(i!=4 && light!=41) continue;
				bundle["views"].push_back({{"viewType",view==1?"Scene":"Game"},{"viewId",view},{"lightId",std::string("650000000000000000000000000000")+std::to_string(light)},
					{"lightType",light==41?0:light==40?1:light==42?2:3},{"faceIndices",light==40?Json{0,1,2,3,4,5}:Json{0}}});
			}
			step["nativePayload"]={{"transactionRequests",requests},{"casterObjectIds",selected},{"shadowBatchControls",{{"maxInstancesPerGroup",i==5?64:0},{"maxTaskGroupsX",0},{"failInstanceUpload",i==6}}}};
			if(i==2) step["nativePayload"]["seekTick"]=30;
			step["requiredBundle"]=bundle;step["requiredCapabilities"]={"PublishedGeometryOwners","OwnedSceneTransactionReceipt","NativeShadowBatchExecutionReceipt","CompletedShadowEvidence"};
			step["support"]={{"status","RequiresRuntimeCapability"},{"reason","Real light spatial queries, route owners and recorded group/singleton commands must independently prove the recipe."}};
			output.steps["selectedManifestStepIds"].push_back(step.at("id"));output.steps["steps"].push_back(std::move(step));
		}
		output.files.emplace_back("renderer-efficiency-steps.json",output.steps);return output;
	}
}
