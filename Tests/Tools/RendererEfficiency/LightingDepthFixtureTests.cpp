// CPU authoring regression only; run this standalone tool with workloads.json.
// Its recipes and glTF bytes do not prove that shaders or native imports ran.
#include "LightingDepthFixture.h"
#include "Renderer/LightingDepthReduction.h"
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>

namespace
{
	using Json = nlohmann::json;
	void Require(bool value, const char* message) { if(!value) throw std::runtime_error(message); }
	const Json& File(const TerraEngine::Tests::RendererEfficiency::LightingDepthFixture& fixture, const std::string& path)
	{
		for(const auto& item : fixture.files) if(item.first.generic_string() == path) return item.second;
		throw std::runtime_error("Required generated file is absent.");
	}
}
int main(int argc, char** argv)
{
	if(argc != 2) return 3;
	unsigned passed = 0, failed = 0;
	const auto test = [&](const char* name, auto body) {
		try { body(); ++passed; std::cout << "[PASS] " << name << '\n'; }
		catch(const std::exception& error) { ++failed; std::cout << "[FAIL] " << name << ": " << error.what() << '\n'; }
	};
	Json manifest; std::ifstream input(argv[1]); input >> manifest;
	Json foundation = {{"objects", Json::array({{{"uuid", "65000000000000000000000000000002"},
		{"components", {{"LightComponent", Json::object()}}}}, {{"uuid", "65000000000000000000000000000003"},
		{"components", {{"MeshRenderComponent", {{"meshId", "3ce0c4c32b4c190edd4cb4371e9b055b"}, {"materialId", "2b611b72eec7bdb66b0da4fe40360f82"}}}}}}})}};
	for(const auto& definition : manifest.at("workloads"))
	{
		const auto id = definition.at("id").get<std::string>(); if(id != "depth-coverage" && id != "depth-heavy") continue;
		test(id.c_str(), [&] {
			using namespace TerraEngine::Tests::RendererEfficiency;
			const auto result = BuildLightingDepthFixture(foundation, definition, std::string(64, 'a'), definition.at("seed"));
			const auto repeated = BuildLightingDepthFixture(foundation, definition, std::string(64, 'a'), definition.at("seed"));
			Require(result.scene == repeated.scene && result.steps == repeated.steps && result.files == repeated.files, "Producer is deterministic.");
			const unsigned count = id == "depth-coverage" ? 17 : 1000; std::set<std::string> identities; unsigned lights = 0, geometry = 0;
			for(const auto& object : result.scene.at("objects"))
			{
				Require(identities.insert(object.at("uuid")).second, "Object IDs are unique.");
				if(object.at("components").contains("LightComponent")) { ++lights; Require(object.at("active") == true && object.at("components").at("LightComponent").at("castsShadows") == false, "Actual authored active lights are nonshadowing."); }
				if(object.at("components").contains("MeshRenderComponent")) ++geometry;
				Require(!object.at("components").contains("TransformComponent"), "Canonical Transform component spelling.");
			}
			Require(lights == count && geometry >= 5, "Actual light count and layered/thin geometry.");
			Require(result.steps.at("assets").size() == 1 && result.steps.at("targets").at("lightingPool").size() == count, "Native binding contract retains actual identities.");
			const auto& gltf = File(result, "Assets/Models/RendererEfficiency_DepthPanel.gltf");
			Require(gltf.at("meshes").at(0).at("name") == "DepthPanel" && gltf.at("materials").at(0).at("alphaMode") == "OPAQUE", "Cookable opaque named panel.");
			const auto& meta = File(result, "Assets/Models/RendererEfficiency_DepthPanel.gltf.meta");
			Require(meta.at("lastModified") == 0 && meta.at("compiledPath") == "", "Complete native metadata preserves package identity.");
			Require(result.steps.at("steps").size() == (count == 17 ? 6 : 1), "Original frozen step count.");
			for(unsigned ordinal = 0; ordinal < result.steps.at("steps").size(); ++ordinal)
			{
				const auto& step = result.steps.at("steps").at(ordinal); const unsigned width = ordinal == 4 ? 1919 : ordinal == 5 ? 7 : 1920;
				const unsigned height = ordinal == 4 ? 1079 : ordinal == 5 ? 5 : 1080;
				Require(step.at("nativeExtent") == Json{{"width",width},{"height",height}} && step.at("warmupFrames") == 60 && step.at("measuredFrames") == 300, "Sealed extents and unchanged windows.");
				Require(step.at("nativePayload").at("candidateOrder").size() == count, "All real candidates are selected above fallback threshold.");
				Require(step.at("requiredBundle").at("views").at(0).at("depthPurposes").size() == (count == 17 ? 2 : 1), "Production depth is mandatory; oracle is separate.");
				if(count != 17) continue;
				const auto& recipe = File(result, step.at("nativePayload").at("depthRecipePath").get<std::string>());
				Require(recipe.at("recipe").at("extent") == Json{{"width",width},{"height",height}} && recipe.at("recipe").at("fillBits").is_number_unsigned(), "Typed uint32 input extent and bits.");
				std::set<std::pair<unsigned,unsigned>> pixels;
				for(const auto& pixel : recipe.at("recipe").at("overrides")) { const unsigned x = pixel.at("x"), y = pixel.at("y"); Require(x < width && y < height && pixels.emplace(x,y).second && pixel.at("bits").is_number_unsigned(), "Sparse overrides are bounded, unique uint32 values."); }
				if(ordinal == 1) Require(pixels.size() == 144, "One skipped-position pixel in every nonempty tile.");
				if(ordinal == 2) Require(pixels.empty() && recipe.at("recipe").at("fillBits") == 0x3f800000U, "True empty clear-only oracle.");
				if(ordinal == 3) { std::set<std::uint32_t> bits; for(const auto& pixel : recipe.at("recipe").at("overrides")) bits.insert(pixel.at("bits").get<std::uint32_t>()); Require(bits.contains(0x7fc00000U) && bits.contains(0x7f800000U) && bits.contains(0xff800000U) && bits.contains(0U) && bits.contains(0x3f7fffffU), "Actual invalid and edge bit patterns preserved."); }
				if(ordinal == 5) Require(pixels.size() == 35, "Small recipe authors only its actual 35 pixels.");
				// Exercise the real independent numerical APIs using generated inputs.
				// This is CPU consistency only, never native shader/readback acceptance.
				using namespace TerraEngine::Renderer::LightingDepthReduction;
				DepthRecipe typed; typed.extent={width,height}; typed.fillBits=recipe.at("recipe").at("fillBits");
				for(const auto& pixel:recipe.at("recipe").at("overrides")) typed.overrides.push_back({pixel.at("x"),pixel.at("y"),pixel.at("bits")});
				std::vector<std::uint32_t> dense(std::size_t(width)*height); TileResults complete{},cooperative{};
				Require(MaterializeDepthRecipe(typed,dense) && ReduceCompleteScan(typed.extent,dense,complete) && ReduceCooperativeOracle(typed.extent,dense,cooperative), "Actual typed recipe materialization/reductions succeed.");
				std::uint64_t visited=0,eligible=0; unsigned emptyRectangles=0;
				for(unsigned tile=0;tile<144;++tile) { Require(complete[tile].bounds==cooperative[tile].bounds && complete[tile].coverage==cooperative[tile].coverage,"Independent complete/cooperative CPU results match."); visited+=complete[tile].coverage.visited; eligible+=complete[tile].coverage.eligible; if(!complete[tile].coverage.visited) { ++emptyRectangles; Require(complete[tile].bounds==TileBounds{},"Empty rectangle has genuine neutral bounds."); } }
				Require(visited==std::uint64_t(width)*height,"All actual input pixels visited exactly once.");
				if(ordinal==1) Require(eligible==144,"Exactly one genuine eligible sample per tile.");
				if(ordinal==2) Require(eligible==0,"Clear-one input has zero eligible samples.");
				if(ordinal==5) Require(emptyRectangles==109 && visited==35,"Small viewport retains 109 empty tile rectangles.");
			}
			auto drift = definition; drift["steps"][0]["warmupFrames"] = 1; bool rejected = false;
			try { (void)BuildLightingDepthFixture(foundation, drift, std::string(64,'a'), definition.at("seed")); } catch(const std::invalid_argument&) { rejected = true; }
			Require(rejected, "Window drift fails closed.");
			drift=definition; drift["steps"][0]["nativeExtent"]={{"width",1},{"height",1}}; rejected=false;
			try { (void)BuildLightingDepthFixture(foundation,drift,std::string(64,'a'),definition.at("seed")); } catch(const std::invalid_argument&) { rejected=true; }
			Require(rejected,"Native extent drift fails closed.");
		});
	}
	std::cout << passed << " passed, " << failed << " failed\n"; return failed ? 1 : 0;
}
