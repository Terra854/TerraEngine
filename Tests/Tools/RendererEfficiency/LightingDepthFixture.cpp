#include "LightingDepthFixture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace TerraEngine::Tests::RendererEfficiency
{
	namespace
	{
		using Json = nlohmann::json;
		std::string Id(unsigned ordinal, bool asset = false)
		{
			std::ostringstream text;
			text << (asset ? "6540000000000000" : "6530000000000000") << std::hex << std::setfill('0') << std::setw(16) << ordinal;
			return text.str();
		}
		Json Pose(const Json& offset, const Json& scale = Json::array({1,1,1}), const Json& rotation = Json::array({1,0,0,0}))
		{
			return {{"WorldPose", {{"position", {{"cell", {{"x","0"},{"y","0"},{"z","0"}}}, {"offsetMetres",offset}}}, {"rotation",rotation},{"scale",scale}}},
				{"ParentRelativePose", {{"translationMetres",offset},{"rotation",rotation},{"scale",scale},{"transformSpaceId",0}}}};
		}
		void PrepareFoundation(Json& objects)
		{
			// Preserve installed geometry but freeze animation and remove inherited
			// illumination so actual collected candidate counts are deterministic.
			for(auto& object : objects)
			{
				auto& components = object.at("components"); components.erase("LightComponent");
				if(components.contains("AnimatorComponent")) components["AnimatorComponent"]["playAutomatically"] = false;
				if(object.contains("children")) PrepareFoundation(object["children"]);
			}
			objects.erase(std::remove_if(objects.begin(),objects.end(),[](const Json& object) { return object.value("uuid","") == "65000000000000000000000000000002"; }),objects.end());
		}
		std::string Base64(const std::vector<std::uint8_t>& bytes)
		{
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string result;
			for(std::size_t index=0;index<bytes.size();index+=3)
			{
				const auto count=std::min<std::size_t>(3,bytes.size()-index);
				const unsigned word=(unsigned(bytes[index])<<16)|(count>1?unsigned(bytes[index+1])<<8:0)|(count>2?unsigned(bytes[index+2]):0);
				result+=alphabet[(word>>18)&63]; result+=alphabet[(word>>12)&63]; result+=count>1?alphabet[(word>>6)&63]:'='; result+=count>2?alphabet[word&63]:'=';
			}
			return result;
		}
		Json Panel()
		{
			// Cookable opaque indexed glTF quad, with embedded little-endian data.
			// Actual import determines named child UUIDs; startup placeholders below
			// cannot authorize runtime evidence until those owners are published.
			std::vector<std::uint8_t> bytes;
			const auto append=[&]<typename T>(T value) { const auto start=bytes.size(); bytes.resize(start+sizeof(T)); std::memcpy(bytes.data()+start,&value,sizeof(T)); };
			for(float value : std::array<float,12>{-1,-1,0,1,-1,0,-1,1,0,1,1,0}) append(value);
			for(unsigned vertex=0;vertex<4;++vertex) { append(0.F); append(0.F); append(1.F); }
			for(float value : std::array<float,8>{0,0,1,0,0,1,1,1}) append(value);
			for(std::uint16_t index : std::array<std::uint16_t,6>{0,1,2,1,3,2}) append(index);
			return {{"asset",{{"version","2.0"},{"generator","TerraEngine owned lighting depth fixture"}}},
				{"buffers",Json::array({{{"byteLength",bytes.size()},{"uri","data:application/octet-stream;base64,"+Base64(bytes)}}})},
				{"bufferViews",Json::array({{{"buffer",0},{"byteOffset",0},{"byteLength",48}},{{"buffer",0},{"byteOffset",48},{"byteLength",48}},{{"buffer",0},{"byteOffset",96},{"byteLength",32}},{{"buffer",0},{"byteOffset",128},{"byteLength",12},{"target",34963}}})},
				{"accessors",Json::array({{{"bufferView",0},{"componentType",5126},{"count",4},{"type","VEC3"},{"min",{-1,-1,0}},{"max",{1,1,0}}},{{"bufferView",1},{"componentType",5126},{"count",4},{"type","VEC3"}},{{"bufferView",2},{"componentType",5126},{"count",4},{"type","VEC2"}},{{"bufferView",3},{"componentType",5123},{"count",6},{"type","SCALAR"}}})},
				{"materials",Json::array({{{"name","DepthPanelMaterial"},{"alphaMode","OPAQUE"},{"doubleSided",true},{"pbrMetallicRoughness",{{"baseColorFactor",{0.4,0.6,0.5,1}},{"metallicFactor",0},{"roughnessFactor",0.8}}}}})},
				{"meshes",Json::array({{{"name","DepthPanel"},{"primitives",Json::array({{{"attributes",{{"POSITION",0},{"NORMAL",1},{"TEXCOORD_0",2}}},{"indices",3},{"material",0}}})}}})},
				{"nodes",Json::array({{{"name","DepthPanel"},{"mesh",0}}})},{"scenes",Json::array({{{"nodes",{0}}}})},{"scene",0}};
		}
		Json Recipe(unsigned ordinal,unsigned width,unsigned height)
		{
			// Numeric uint32 preserves NaN/Inf input payloads in legal JSON. Sparse
			// coordinates follow the frozen float-derived original tile rectangles.
			Json result={{"extent",{{"width",width},{"height",height}}},{"fillBits",ordinal==3?0xbf800000U:ordinal==0||ordinal==4?0x3f000000U:0x3f800000U},{"overrides",Json::array()}};
			const auto pixel=[&](unsigned x,unsigned y,std::uint32_t bits) { result["overrides"].push_back({{"x",x},{"y",y},{"bits",bits}}); };
			if(ordinal==2) return result;
			if(ordinal==5)
			{
				for(unsigned y=0;y<height;++y) for(unsigned x=0;x<width;++x) pixel(x,y,(x+y)%3==0?0x3f800000U:(x+y)%3==1?0U:0x3f7fffffU);
				return result;
			}
			constexpr std::array<std::uint32_t,7> invalid{0xbf800000U,0x3fc00000U,0x7fc00000U,0x7f800000U,0xff800000U,0U,0x3f7fffffU};
			const float tileWidth=float(width)/16.F, tileHeight=float(height)/9.F;
			for(unsigned ty=0;ty<9;++ty) for(unsigned tx=0;tx<16;++tx)
			{
				const unsigned x0=static_cast<unsigned>(float(tx)*tileWidth), x1=std::min(width,static_cast<unsigned>(float(tx+1)*tileWidth));
				const unsigned y0=static_cast<unsigned>(float(ty)*tileHeight), y1=std::min(height,static_cast<unsigned>(float(ty+1)*tileHeight));
				if(x0==x1 || y0==y1) continue;
				// Odd relative positions or final pixels expose stride-two omissions.
				const unsigned x=ordinal==1 && tx%2==0?x0+1:x1-1, y=ordinal==1 && ty%2==0?y0+1:y1-1;
				pixel(x,y,ordinal==3?invalid[(ty*16+tx)%invalid.size()]:(tx+ty)%2==0?0x3a83126fU:0x3f7fffffU);
			}
			return result;
		}
	}
	LightingDepthFixture BuildLightingDepthFixture(const Json& foundationScene,const Json& definition,std::string_view manifestHash,std::uint64_t seed)
	{
		const std::string id=definition.at("id"); const bool coverage=id=="depth-coverage";
		if((!coverage && id!="depth-heavy") || definition.at("stage")!="P3" || seed!=(coverage?65004U:65005U) || definition.at("seed")!=seed
			|| definition.at("cameraIds")!=Json::array({"scene-fixed"}) || manifestHash.size()!=64
			|| !std::all_of(manifestHash.begin(),manifestHash.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');})) throw std::invalid_argument("Depth fixture identity contract drift.");
		const unsigned lightCount=coverage?17:1000;
		if(definition.at("parameters").at("lightCount")!=lightCount || definition.at("parameters").at("grid")!=Json::array({16,9,24})) throw std::invalid_argument("Depth fixture light/grid contract drift.");
		const std::vector<std::string> actions=coverage?std::vector<std::string>{"full-coverage","single-pixel-skipped-sample-positions","clear-only","invalid-depth","resize-odd","resize-small"}:std::vector<std::string>{"stable-depth-heavy"};
		if(definition.at("steps").size()!=actions.size()) throw std::invalid_argument("Depth fixture step count drift.");
		LightingDepthFixture output{foundationScene,Json::object(),{}}; PrepareFoundation(output.scene.at("objects")); Json order=Json::array();
		for(unsigned ordinal=1;ordinal<=lightCount;++ordinal)
		{
			order.push_back(Id(ordinal+1000));
			output.scene["objects"].push_back({{"uuid",Id(ordinal+1000)},{"name","Depth Light "+std::to_string(ordinal)},{"active",true},{"isStatic",false},{"tag","Untagged"},{"layer","Default"},{"children",Json::array()},
				{"components",{{"Transform",Pose({(int(ordinal%10)-5)*0.5,3.0,(int((ordinal/10)%10)-5)*0.5})},{"LightComponent",{{"type",1},{"unit",0},{"color",{1,1,1}},{"intensity",0.25},{"range",30.0},{"castsShadows",false},{"authoredPriority",lightCount+1-ordinal},{"stabilityPreference",0.0}}}}}});
		}
		Json panels=Json::object();
		const auto panel=[&](unsigned ordinal,const char* role,Json position,Json scale) {
			const double angle=std::atan(0.25)/2.0; const Json rotation={std::cos(angle),-std::sin(angle),0,0};
			output.scene["objects"].push_back({{"uuid",Id(ordinal)},{"name",role},{"active",true},{"isStatic",true},{"tag","Untagged"},{"layer","Default"},{"children",Json::array()},
				{"components",{{"Transform",Pose(position,scale,rotation)},{"MeshRenderComponent",{{"meshId","3ce0c4c32b4c190edd4cb4371e9b055b"},{"materialId","2b611b72eec7bdb66b0da4fe40360f82"},{"taskShaderCullingEnabled",true}}}}}});
			panels[role]={{"objectId",Id(ordinal)},{"componentName","MeshRenderComponent"},{"assetBindings",{{"meshId","depthPanel.mesh"},{"materialId","depthPanel.material"}}}};
		};
		panel(10,"background",{0,1,-4},{20,12,1}); panel(11,"nearLayer",{-3,1,0},{3,4,1}); panel(12,"farLayer",{3,2,-2},{2,3,1}); panel(13,"thinDistant",{0,1,-1},{0.0125,3,1});
		const std::string source="Models/RendererEfficiency_DepthPanel.gltf";
		output.files.emplace_back("Assets/"+source,Panel()); output.files.emplace_back("Assets/"+source+".meta",Json{{"guid",Id(1,true)},{"sourcePath",source},{"importerName","GltfPackageImporter"},{"lastModified",0},{"compiledPath",""},{"settings",Json::object()}});
		output.steps={{"schemaVersion",1},{"fixtureId",id},{"fixtureHash",manifestHash},{"seed",seed},{"stage","P3"},{"selectedStage","P3"},{"manifestStage","P3"},{"depthFixture",true},
			{"fullManifestStepCount",actions.size()},{"selectedManifestStepIds",Json::array()},{"excludedManifestStepIds",Json::array()},{"resetBaselineBeforeStep",true},
			{"targets",{{"lightingPool",order},{"depthPanels",panels},{"foundationReceiver",{{"objectId","65000000000000000000000000000003"},{"componentName","MeshRenderComponent"}}}}},
			{"assets",Json::array({{{"sourcePath",source},{"packageGuid",Id(1,true)},{"requiredSubAssets",Json::array({{{"kind","Mesh"},{"name","DepthPanel"},{"bindingKey","depthPanel.mesh"}},{{"kind","Material"},{"name","DepthPanelMaterial"},{"bindingKey","depthPanel.material"}}})},{"requiredCook",{{"minimumLods",1},{"minimumMeshlets",1}}}}})},{"steps",Json::array()}};
		for(unsigned ordinal=0;ordinal<actions.size();++ordinal)
		{
			auto step=definition.at("steps").at(ordinal); std::ostringstream suffix; suffix<<std::setfill('0')<<std::setw(2)<<ordinal; const std::string stepId=id+"-"+suffix.str();
			if(step.at("id")!=stepId || step.at("action")!=actions[ordinal] || step.at("ordinal")!=ordinal || step.at("warmupFrames")!=60 || step.at("measuredFrames")!=300) throw std::invalid_argument("Depth fixture frozen window/action drift.");
			const unsigned width=ordinal==4?1919:ordinal==5?7:1920, height=ordinal==4?1079:ordinal==5?5:1080; const Json extent={{"width",width},{"height",height}};
			if(step.contains("nativeExtent") && step.at("nativeExtent")!=extent) throw std::invalid_argument("Depth fixture extent contract drift.");
			step["nativeExtent"]=extent;
			Json baseline=Json::array(); for(const auto& objectId:order) baseline.push_back({{"operation","SetActive"},{"objectId",objectId},{"active",true}});
			step["nativePayload"]={{"baselineTransactionRequests",baseline},{"transactionRequests",Json::array()},{"baselineCandidateOrder",order},{"candidateOrder",order},
				{"lightingBudgets",{{"maxUploadedLights",1024},{"maxDynamicLights",lightCount},{"maxShadowLights",0}}},{"expectedRecipe",{{"sourceCount",lightCount},{"analysisCount",lightCount},{"retainedCount",lightCount}}}};
			if(coverage)
			{
				const auto recipe=Recipe(ordinal,width,height); const std::string path="renderer-efficiency-depth/"+stepId+".json";
				output.files.emplace_back(path,Json{{"schemaVersion",1},{"kind","LightingDepthInputRecipe"},{"fixtureId",id},{"fixtureHash",manifestHash},{"seed",seed},{"fixtureStepId",stepId},{"viewType","Scene"},{"viewId",1},{"grid",{16,9,24}},{"recipe",recipe}});
				step["nativePayload"]["depthRecipePath"]=path; step["nativePayload"]["depthInputRecipe"]=recipe;
			}
			step["requiredBundle"]={{"views",Json::array({{{"viewType","Scene"},{"viewId",1},{"lightingPurposes",{"FinalLights","Analysis"}},{"depthPurposes",coverage?Json::array({"Production","Oracle"}):Json::array({"Production"})}}})}};
			step["requiredCapabilities"]={"OwnedSceneTransactionReceipt","PublishedGeometryOwners","TypedLightingCandidateOrder","CompletionQualifiedLightingDepthEvidence","NativePerStepCameraExtent"};
			step["support"]={{"status","RequiresRuntimeCapability"},{"reason","Recipe only; actual cooked bindings, production depth, isolated oracle and GPU completion must be observed."}};
			output.steps["selectedManifestStepIds"].push_back(stepId); output.steps["steps"].push_back(std::move(step));
		}
		output.files.emplace_back("renderer-efficiency-steps.json",output.steps); return output;
	}
}
