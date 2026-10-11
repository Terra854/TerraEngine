// Installed-only deterministic input generation. Import/cooking and typed runtime
// publication remain engine responsibilities: authored JSON is never GPU evidence.
#include "ShadowInvalidationFixture.h"
#include "Animation/AnimationTypes.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

namespace TerraEngine::Tests::RendererEfficiency
{
	namespace
	{
		using Json = nlohmann::json;
		using Bytes = std::vector<std::uint8_t>;
		std::string Id(unsigned value, bool asset = false)
		{
			const auto digits = std::to_string(value);
			return std::string(asset ? "651" : "650") + std::string(29 - digits.size(), '0') + digits;
		}
		std::string Base64(const Bytes& bytes)
		{
			constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string result;
			for(std::size_t index = 0; index < bytes.size(); index += 3)
			{
				const unsigned count = static_cast<unsigned>(std::min<std::size_t>(3, bytes.size() - index));
				const unsigned word = (unsigned(bytes[index]) << 16) | (count > 1 ? unsigned(bytes[index + 1]) << 8 : 0)
					| (count > 2 ? unsigned(bytes[index + 2]) : 0);
				result += alphabet[(word >> 18) & 63]; result += alphabet[(word >> 12) & 63];
				result += count > 1 ? alphabet[(word >> 6) & 63] : '='; result += count > 2 ? alphabet[word & 63] : '=';
			}
			return result;
		}
		void BigEndian(Bytes& bytes, std::uint32_t value)
		{
			for(int shift = 24; shift >= 0; shift -= 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
		}
		void Chunk(Bytes& png, std::string_view type, const Bytes& data)
		{
			BigEndian(png, static_cast<std::uint32_t>(data.size()));
			const auto start = png.size(); png.insert(png.end(), type.begin(), type.end()); png.insert(png.end(), data.begin(), data.end());
			std::uint32_t crc = 0xffffffffU;
			for(std::size_t index = start; index < png.size(); ++index)
			{
				crc ^= png[index];
				for(int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
			}
			BigEndian(png, ~crc);
		}
		std::string CutoutTexture(bool reverse)
		{
			// A 2x2 RGBA checker with genuine transparent texels, encoded using a
			// standard zlib stored block so the standalone tool needs no PNG library.
			Bytes scanlines;
			for(unsigned row = 0; row < 2; ++row)
			{
				scanlines.push_back(0);
				for(unsigned column = 0; column < 2; ++column)
				{
					scanlines.insert(scanlines.end(), {255, 255, 255, static_cast<std::uint8_t>(((row + column + unsigned(reverse)) % 2) ? 0 : 128)});
				}
			}
			Bytes compressed{0x78, 0x01, 0x01, static_cast<std::uint8_t>(scanlines.size()), 0,
				static_cast<std::uint8_t>(~scanlines.size()), 0xff};
			compressed.insert(compressed.end(), scanlines.begin(), scanlines.end());
			std::uint32_t a = 1, b = 0;
			for(auto value : scanlines) { a = (a + value) % 65521; b = (b + a) % 65521; }
			BigEndian(compressed, (b << 16) | a);
			Bytes png{137, 80, 78, 71, 13, 10, 26, 10}, header;
			BigEndian(header, 2); BigEndian(header, 2); header.insert(header.end(), {8, 6, 0, 0, 0});
			Chunk(png, "IHDR", header); Chunk(png, "IDAT", compressed); Chunk(png, "IEND", {});
			return "data:image/png;base64," + Base64(png);
		}
		template<typename T> void Append(Bytes& bytes, T value)
		{
			// The installed Windows utility writes glTF's little-endian scalar format.
			const auto start = bytes.size(); bytes.resize(start + sizeof(value)); std::memcpy(bytes.data() + start, &value, sizeof(value));
		}
		Json Grid(std::string name, unsigned divisions, bool horizontal, bool cutout, float displacement = 0)
		{
			Bytes bytes;
			const unsigned vertices = (divisions + 1) * (divisions + 1);
			for(unsigned row = 0; row <= divisions; ++row) for(unsigned col = 0; col <= divisions; ++col)
			{
				const float x = float(col) / divisions * 4 - 2 + displacement, v = float(row) / divisions * 4;
				Append(bytes, x); Append(bytes, horizontal ? 0.F : v); Append(bytes, horizontal ? v - 2 : 0.F);
			}
			const auto normalOffset = bytes.size();
			for(unsigned vertex = 0; vertex < vertices; ++vertex) { Append(bytes, 0.F); Append(bytes, horizontal ? 1.F : 0.F); Append(bytes, horizontal ? 0.F : 1.F); }
			const auto uvOffset = bytes.size();
			for(unsigned row = 0; row <= divisions; ++row) for(unsigned col = 0; col <= divisions; ++col) { Append(bytes, float(col) / divisions); Append(bytes, float(row) / divisions); }
			std::array<std::size_t, 2> indexOffsets{}; std::array<unsigned, 2> indexCounts{};
			// Two material sections keep emitted meshlet ranges distinguishable. The
			// real importer must still prove at least two cooked detail levels/meshlets.
			for(unsigned section = 0; section < 2; ++section)
			{
				indexOffsets[section] = bytes.size();
				for(unsigned row = 0; row < divisions; ++row) for(unsigned col = 0; col < divisions; ++col)
				{
					if((col * 2 / divisions) != section) continue;
					const unsigned a = row * (divisions + 1) + col, b = a + 1, c = a + divisions + 1, d = c + 1;
					for(unsigned value : (horizontal ? std::array<unsigned, 6>{a, c, b, b, c, d} : std::array<unsigned, 6>{a, b, c, b, d, c})) Append(bytes, static_cast<std::uint16_t>(value));
					indexCounts[section] += 6;
				}
			}
			Json materials = Json::array();
			for(unsigned section = 0; section < 2; ++section)
			{
				Json material = {{"name", name + "Material" + std::to_string(section)}, {"doubleSided", true},
					{"pbrMetallicRoughness", {{"baseColorFactor", {0.3 + section * 0.3, 0.6, 0.4, 1}}, {"metallicFactor", 0}, {"roughnessFactor", 0.8}}}};
				if(cutout) { material["alphaMode"] = "MASK"; material["alphaCutoff"] = 0.25; material["pbrMetallicRoughness"]["baseColorTexture"] = {{"index", 0}}; }
				materials.push_back(std::move(material));
			}
			Json result = {{"asset", {{"version", "2.0"}, {"generator", "TerraEngine renderer efficiency owned fixture"}}},
				{"buffers", Json::array({{{"byteLength", bytes.size()}, {"uri", "data:application/octet-stream;base64," + Base64(bytes)}}})},
				{"bufferViews", Json::array({{{"buffer", 0}, {"byteOffset", 0}, {"byteLength", normalOffset}},
					{{"buffer", 0}, {"byteOffset", normalOffset}, {"byteLength", uvOffset - normalOffset}},
					{{"buffer", 0}, {"byteOffset", uvOffset}, {"byteLength", indexOffsets[0] - uvOffset}}})},
				{"accessors", Json::array({{{"bufferView", 0}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"},
					{"min", horizontal ? Json{-2 + displacement, 0, -2} : Json{-2 + displacement, 0, 0}}, {"max", horizontal ? Json{2 + displacement, 0, 2} : Json{2 + displacement, 4, 0}}},
					{{"bufferView", 1}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC3"}},
					{{"bufferView", 2}, {"componentType", 5126}, {"count", vertices}, {"type", "VEC2"}}})}, {"materials", materials},
				{"nodes", Json::array({{{"name", name}, {"mesh", 0}}})}, {"scenes", Json::array({{{"nodes", {0}}}})}, {"scene", 0}};
			Json primitives = Json::array();
			for(unsigned section = 0; section < 2; ++section)
			{
				if(indexCounts[section] == 0) continue;
				const auto view = result["bufferViews"].size(), accessor = result["accessors"].size();
				result["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", indexOffsets[section]}, {"byteLength", indexCounts[section] * 2}, {"target", 34963}});
				result["accessors"].push_back({{"bufferView", view}, {"componentType", 5123}, {"count", indexCounts[section]}, {"type", "SCALAR"}});
				primitives.push_back({{"attributes", {{"POSITION", 0}, {"NORMAL", 1}, {"TEXCOORD_0", 2}}}, {"indices", accessor}, {"material", section}});
			}
			result["meshes"] = Json::array({{{"name", name}, {"primitives", primitives}}});
			if(cutout) { result["images"] = Json::array({{{"name", "CutoutAlpha"}, {"uri", CutoutTexture(false)}}}); result["textures"] = Json::array({{{"source", 0}}}); }
			return result;
		}
		Json Pose(Json offset)
		{
			return {{"WorldPose", {{"position", {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}}, {"offsetMetres", offset}}},
				{"rotation", {1, 0, 0, 0}}, {"scale", {1, 1, 1}}}}, {"ParentRelativePose", {{"translationMetres", offset}, {"rotation", {1, 0, 0, 0}}, {"scale", {1, 1, 1}}, {"transformSpaceId", 0}}}};
		}
		Json Object(unsigned id, std::string name, Json position, Json components, bool isStatic = false)
		{
			components["Transform"] = Pose(position);
			return {{"uuid", Id(id)}, {"name", name}, {"active", true}, {"isStatic", isStatic}, {"tag", "Untagged"}, {"layer", "Default"}, {"components", components}, {"children", Json::array()}};
		}
	}

	ShadowInvalidationFixture BuildShadowInvalidationFixture(const Json& foundationScene, const Json& heroGltf,
		const Json& definition, std::string_view manifestHash, std::uint64_t seed, std::string_view requestedStage)
	{
		// P1 remains the default, byte-identical recipe. Shared P2 adds authored
		// lighting candidates and typed controls, never measured success claims.
		if(requestedStage != "P1" && requestedStage != "P2" && requestedStage != "P3" && requestedStage != "P4" && requestedStage != "P5") throw std::invalid_argument("Shared fixture stage must be P1 through P5.");
		const bool sharedStatic = requestedStage == "P5";
		const bool sharedBatch = requestedStage == "P4" || sharedStatic;
		const bool sharedDepth = requestedStage == "P3";
		const bool sharedLighting = requestedStage == "P2" || sharedDepth || sharedBatch;
		ShadowInvalidationFixture output{foundationScene, Json::object(), {}};
		const std::string fixtureId = definition.at("id");
		if(fixtureId != "shadow-invalidation" && fixtureId != "multiview-lights" && fixtureId != "recovery-capacity")
			throw std::runtime_error("No P1 authored recipe for requested fixture.");
		if(sharedLighting && fixtureId == "shadow-invalidation") throw std::invalid_argument("Shadow invalidation belongs to P1.");
		output.scene["name"] = "Renderer Efficiency " + fixtureId;
		// Keep the canonical camera/primary light, replace the generic imported hero
		// and triangle receiver with stable native target identities.
		output.scene["objects"].erase(output.scene["objects"].begin() + 2, output.scene["objects"].end());
		Json assets = Json::array(), targets = Json::object();
		const auto addAsset = [&](unsigned id, std::string role, std::string name, Json gltf, Json children, unsigned minimumLods = 0, unsigned minimumMeshlets = 0) {
			const std::string path = "Models/RendererEfficiency_" + role + ".gltf";
			output.files.emplace_back("Assets/" + path, gltf);
			// LoadMetadata requires the native file-clock ticks and compiled path.
			// Zero ticks and an empty target keep first-cook discovery deterministic
			// while preserving this package GUID; normal import fills both fields.
			output.files.emplace_back("Assets/" + path + ".meta", Json{{"guid", Id(id, true)}, {"sourcePath", path}, {"importerName", "GltfPackageImporter"},
				{"lastModified", 0}, {"compiledPath", ""}, {"settings", Json::object()}});
			assets.push_back({{"sourcePath", path}, {"packageGuid", Id(id, true)}, {"requiredSubAssets", children},
				{"requiredCook", {{"minimumLods", minimumLods}, {"minimumMeshlets", minimumMeshlets}}}});
		};
		const auto addMesh = [&](unsigned id, unsigned assetId, std::string role, std::string name, Json position, unsigned divisions, bool horizontal, bool mask) {
			Json children = Json::array({
				{{"kind", "Mesh"}, {"name", name}, {"bindingKey", role + ".mesh"}},
				{{"kind", "Material"}, {"name", name + "Material0"}, {"bindingKey", role + ".material"}}});
			if(mask) children.push_back({{"kind", "Texture"}, {"name", "CutoutAlpha"}, {"bindingKey", role + ".texture"}});
			addAsset(assetId, role, name, Grid(name, divisions, horizontal, mask), children, divisions > 1 ? 2 : 0, divisions > 1 ? 2 : 0);
			// Existing installed bindings keep startup valid. Host replaces them only
			// after the generated package publishes; these placeholders cannot pass SC.
			output.scene["objects"].push_back(Object(id, name, position, {{"MeshRenderComponent", {{"meshId", "3ce0c4c32b4c190edd4cb4371e9b055b"}, {"materialId", "2b611b72eec7bdb66b0da4fe40360f82"}, {"taskShaderCullingEnabled", true}}}}, true));
			targets[role] = {{"objectId", Id(id)}, {"componentName", "MeshRenderComponent"}, {"assetBindings", {{"meshId", role + ".mesh"}, {"materialId", role + ".material"}}}};
		};
		addMesh(10, 1, "static", "StaticCaster", {0, 0, 0}, 1, false, false);
		addMesh(3, 2, "receiver", "Receiver", {0, -0.1, 0}, 1, true, false);
		addMesh(11, 3, "grid", "SelectedGeometry", {-4, 0, 0}, 32, false, false);
		addMesh(12, 4, "cutout", "CutoutCaster", {3, 0, 0}, 1, false, true);
		// Variant files are outside Assets, so the editor cannot import them before
		// the owned host atomically replaces the matching source and awaits reimport.
		output.files.emplace_back("renderer-efficiency-variants/static-mesh-revision.json", Grid("StaticCaster", 1, false, false, 0.25F));
		// Keep package/node/material names and the source .meta GUID unchanged.
		// Moving real grid vertices changes cooked page bytes under the same asset
		// identity; only native reimport/publication receipts can prove completion.
		// The native compiler emits authored pages only above 4096 source
		// triangles. Establish this same-UUID dense baseline before changing its
		// vertices; the direct 32-grid remains the baseline for owner-range steps.
		output.files.emplace_back("renderer-efficiency-variants/grid-page-baseline.json", Grid("SelectedGeometry", 64, false, false));
		output.files.emplace_back("renderer-efficiency-variants/grid-page-revision.json", Grid("SelectedGeometry", 64, false, false, 0.5F));
		Json materialVariant = Grid("StaticCaster", 1, false, false);
		materialVariant["materials"][0]["pbrMetallicRoughness"]["baseColorFactor"] = {0.8, 0.1, 0.2, 1};
		output.files.emplace_back("renderer-efficiency-variants/static-material-revision.json", materialVariant);
		Json textureVariant = Grid("CutoutCaster", 1, false, true);
		textureVariant["images"][0]["uri"] = CutoutTexture(true);
		output.files.emplace_back("renderer-efficiency-variants/cutout-texture-revision.json", textureVariant);
		Json cutoutVariant = Grid("CutoutCaster", 1, false, true);
		for(auto& material : cutoutVariant["materials"]) material["alphaCutoff"] = 0.75;
		output.files.emplace_back("renderer-efficiency-variants/cutout-cutout.json", cutoutVariant);
		for(unsigned index = 0; index < 2; ++index)
		{
			const std::string role = index == 0 ? "pose" : "morph", clipName = index == 0 ? "PoseOnly" : "MorphOnly";
			Json gltf = heroGltf, animation = heroGltf.at("animations").at(0), channels = Json::array();
			for(const auto& channel : animation.at("channels")) if((channel.at("target").at("path") == "weights") == (index == 1)) channels.push_back(channel);
			animation["name"] = clipName; animation["channels"] = channels; animation.erase("extras"); gltf["animations"] = Json::array({animation});
			addAsset(5 + index, role, "ImportedHero", gltf, Json::array({
				{{"kind", "AnimationClip"}, {"name", clipName}, {"bindingKey", role + ".clip"}},
				{{"kind", "Skeleton"}, {"name", "ImportedHeroSkeleton"}, {"bindingKey", role + ".skeleton"}},
				{{"kind", "DeformableMesh"}, {"name", "ImportedHeroBody"}, {"bindingKey", role + ".mesh"}},
				{{"kind", "Material"}, {"name", "ImportedHeroMaterial"}, {"bindingKey", role + ".material"}}}));
			const unsigned rootId = index == 0 ? 20 : 30;
			// Active P1 casters retain exact CPU pose/morph authority for reuse proof.
			// Rendering and readback still use the real GPU; missing GPU-only proof stays conservative.
			Json animator = {{"schemaVersion", 3}, {"requestedMode", static_cast<std::uint32_t>(Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback)}, {"defaultClipAssetId", "9dae9362137785aea86bbc574f64d671"},
				{"skeletonOverrideAssetId", "38c018a909d627cc2df445e0b5c99cdd"}, {"playAutomatically", false}, {"loopDefaultClip", sharedStatic}, {"defaultClipStartFrame", 0}, {"rootMotionPolicy", 0}};
			Json root = Object(rootId, clipName, {index == 0 ? -1 : 1, 0, 0}, {{"AnimatorComponent", animator}});
			root["children"].push_back(Object(rootId + 1, role + "Body", {0, 0, 0}, {{"MeshRenderComponent", {{"meshId", "3ce0c4c32b4c190edd4cb4371e9b055b"}, {"materialId", "2b611b72eec7bdb66b0da4fe40360f82"}, {"taskShaderCullingEnabled", true}}}}));
			output.scene["objects"].push_back(root);
			targets[role] = {{"objectId", Id(rootId)}, {"componentName", "AnimatorComponent"}, {"assetBindings", {{"defaultClipAssetId", role + ".clip"}, {"skeletonOverrideAssetId", role + ".skeleton"}}}};
			targets[role + "Body"] = {{"objectId", Id(rootId + 1)}, {"componentName", "MeshRenderComponent"}, {"assetBindings", {{"meshId", role + ".mesh"}, {"materialId", role + ".material"}}}};
		}
		for(unsigned id = 40; id <= 43; ++id)
		{
			const unsigned type = id == 40 ? 1 : id == 41 ? 0 : id == 42 ? 2 : 3;
			Json light = Object(id, "Validation Light " + std::to_string(type), {3, 5, 4}, {{"LightComponent", {{"type", type}, {"intensity", 1000}, {"range", 30}, {"castsShadows", id != 40}}}});
			// Non-cube projections point at the caster group, rather than retaining
			// an identity orientation whose spotlight cone can miss every caster.
			const double yaw = std::atan2(3.0, 4.0), pitch = -std::atan2(4.0, 5.0);
			const Json rotation = {std::cos(yaw / 2) * std::cos(pitch / 2), std::cos(yaw / 2) * std::sin(pitch / 2),
				std::sin(yaw / 2) * std::cos(pitch / 2), -std::sin(yaw / 2) * std::sin(pitch / 2)};
			light["components"]["Transform"]["WorldPose"]["rotation"] = rotation;
			light["components"]["Transform"]["ParentRelativePose"]["rotation"] = rotation;
			output.scene["objects"].push_back(std::move(light));
		}
		Json sharedOrder = Json::array(), sharedPool = Json::array(), sharedBaseline = Json::array();
		if(sharedLighting)
		{
			// Four existing shadow casters remain ahead of the nonshadowing pool.
			// The fifth existing light is nonshadowing, so maxShadowLights=4 retains
			// precisely the original point/directional/spot/area adjunct bundle.
			for(const unsigned id : {2U,40U,41U,42U,43U}) sharedOrder.push_back(Id(id));
			for(auto& object : output.scene["objects"]) if(object.contains("components") && object["components"].contains("LightComponent"))
				object["components"]["LightComponent"]["authoredPriority"] = 10000 - static_cast<unsigned>(std::find(sharedOrder.begin(), sharedOrder.end(), object.at("uuid")) - sharedOrder.begin());
			const unsigned poolCount=fixtureId == "recovery-capacity" ? 1020U : sharedDepth ? 27U : 0U;
			for(unsigned ordinal = 0; ordinal < poolCount; ++ordinal)
			{
				const unsigned id = 10000 + ordinal;
				Json light = Object(id, "Shared Lighting Candidate " + std::to_string(ordinal), {0,3,0},
					{{"LightComponent", {{"type",1},{"intensity",0.01},{"range",30},{"castsShadows",false},
						{"authoredPriority",1020 - ordinal},{"stabilityPreference",0.0}}}});
				const bool baselineActive=sharedDepth && ordinal<27;
				light["active"] = baselineActive; output.scene["objects"].push_back(std::move(light)); sharedPool.push_back(Id(id));
				sharedBaseline.push_back({{"operation","SetActive"},{"objectId",Id(id)},{"active",baselineActive}});
				if(baselineActive) sharedOrder.push_back(Id(id));
			}
			// These are only fixture-owned existing transaction requests. The host
			// publishes membership at its ordinary protected transaction boundary.
			for(unsigned index=0;index<5;++index) sharedBaseline.push_back({{"operation","SetActive"},{"objectId",sharedOrder[index]},{"active",true}});
		}
		Json batchPool=Json::array(),batchBaseline=Json::array();
		if(sharedBatch && fixtureId=="recovery-capacity") {
			// These inactive actors share actual imported static owners. Only the
			// group-overflow transaction activates them; no native route is authored.
			const auto source=std::find_if(output.scene["objects"].begin(),output.scene["objects"].end(),[](const auto& object){return object.at("uuid")==Id(10);});
			const Json model=*source;
			for(unsigned index=0;index<1000;++index) {
				const auto id=Id(20000+index);Json object=model;object["uuid"]=id;object["name"]="Shared Batch Caster "+std::to_string(index);object["active"]=false; if(sharedStatic) object["isStatic"]=true;
				const Json position={(int(index%32)-16)*0.12,0.0,(int(index/32)-16)*0.12};
				object["components"]["Transform"]["WorldPose"]["position"]["offsetMetres"]=position;object["components"]["Transform"]["ParentRelativePose"]["translationMetres"]=position;
				object["components"]["Transform"]["WorldPose"]["scale"]={0.02,0.02,0.02};object["components"]["Transform"]["ParentRelativePose"]["scale"]={0.02,0.02,0.02};
				output.scene["objects"].push_back(std::move(object));batchPool.push_back(id);batchBaseline.push_back({{"operation","SetActive"},{"objectId",id},{"active",false}});
				targets["batch-"+std::to_string(index)]={{"objectId",id},{"componentName","MeshRenderComponent"},{"assetBindings",{{"meshId","static.mesh"},{"materialId","static.material"}}}};
			}
		}
		Json bundle = {{"views", Json::array()}};
		for(unsigned view = 1; view <= 2; ++view) for(const unsigned light : {2U, 41U, 42U, 43U})
		{
			const std::string cameraId = view == 1 ? "scene-fixed" : "game-fixed";
			if(std::find(definition.at("cameraIds").begin(),definition.at("cameraIds").end(),cameraId)==definition.at("cameraIds").end()) continue;
			bundle["views"].push_back({{"viewType", view == 1 ? "Scene" : "Game"}, {"viewId", view}, {"lightId", Id(light)},
				{"lightType", light == 2 ? 1 : light == 41 ? 0 : light == 42 ? 2 : 3}, {"faceIndices", light == 2 ? Json{0, 1, 2, 3, 4, 5} : Json{0}}});
		}
		// All recipes share the same real asset closure. A stage-qualified script
		// records its intentional subset without rewriting the frozen All manifest.
		output.steps = {{"schemaVersion", 1}, {"fixtureId", fixtureId}, {"fixtureHash", manifestHash}, {"seed", seed},
			{"stage", requestedStage}, {"selectedStage", requestedStage}, {"manifestStage", definition.at("stage")},
			{"fullManifestStepCount", definition.at("steps").size()}, {"selectedManifestStepIds", Json::array()}, {"excludedManifestStepIds", Json::array()},
			{"animationAuthoritySetup", {{"policy", "ForceCpuDeterministicFallback"}, {"requestedMode", static_cast<std::uint32_t>(Animation::AnimationExecutionPolicy::ForceCpuDeterministicFallback)},
				{"appliesTo", "AllP1Steps"}, {"reason", "Exact retained CPU pose and morph authority; active casters and native GPU rendering remain enabled."}}},
			{"resetBaselineBeforeStep", true}, {"assets", assets}, {"targets", targets}, {"steps", Json::array()}};
		if(sharedLighting)
		{
			output.steps["sharedLighting"] = true;
			if(sharedDepth) output.steps["sharedDepth"] = true;
			if(sharedStatic) output.steps["sharedStaticShadow"] = true;
			output.steps["targets"]["sharedLightingPool"] = sharedPool;
			output.steps["animationAuthoritySetup"]["appliesTo"] = "AllSelectedSharedSteps";
		}
		for(const auto& definitionStep : definition.at("steps"))
		{
			const std::string action = definitionStep.at("action");
			const bool selected = fixtureId != "recovery-capacity" || action == "recorded-submit-abort" || action == "incomplete-point-faces"
				|| action == "resource-recreation" || action == "device-recovery" || action == "transaction-conflict-rollback"
				|| sharedStatic || (sharedLighting && action == "buffer-overflow") || (sharedBatch && (action == "group-overflow" || action == "unsupported-grouped-route"));
			output.steps[selected ? "selectedManifestStepIds" : "excludedManifestStepIds"].push_back(definitionStep.at("id"));
			if(!selected) continue;
			std::string role = action == "pose" ? "pose" : action == "morph" ? "morph" : action == "cutout" || action == "texture-revision" || action == "shader-revision" ? "cutout" : action == "selected-lod" || action == "selected-meshlet" || action == "page-revision" || action == "residency" ? "grid" : "static";
			Json payload = Json::object(), requirements = Json::array({"AssetBindingsPublished", "CompletedShadowEvidence"});
			if(action == "transform") payload = {{"worldPose", Pose({1, 0, 0}).at("WorldPose")}};
			else if(action == "pose" || action == "morph") { payload = {{"seekTick", 30}, {"clipBinding", role + ".clip"}}; requirements.push_back("AnimationCommandPublication"); }
			else if(action == "membership-removal") payload = {{"active", false}};
			else if(action == "light-assignment") payload = {{"componentName", "LightComponent"}, {"propertyName", "castsShadows"}, {"value", true}, {"pairedDisableTargetId", Id(2)}};
			else if(action == "lifetime-replacement") payload = {{"replacementRole", "static"}, {"name", "Replacement Caster"}, {"worldPose", Pose({0, 0, 0}).at("WorldPose")}};
			else if(action != "stable") { payload = {{"bindingKey", role + ".mesh"}, {"variant", 1}}; requirements.push_back("OwnedResourceOrGeometryMutation:" + action); }
			if(action == "mesh-revision" || action == "page-revision" || action == "material-revision" || action == "texture-revision" || action == "cutout")
			{
				payload = {{"sourcePath", "Models/RendererEfficiency_" + role + ".gltf"},
					{"replacementPath", "renderer-efficiency-variants/" + role + "-" + action + ".json"}};
				if(action == "page-revision") payload["denseBaselineReplacementPath"] = "renderer-efficiency-variants/grid-page-baseline.json";
			}
			if(fixtureId == "multiview-lights")
			{
				requirements.push_back("OwnedMultiViewMutation:" + action);
				if(action == "distinct-origins") payload = {{"views", Json::array({
					{{"viewType", "Scene"}, {"viewId", 1}, {"origin", {{"cell", {0, 0, 0}}, {"offsetMetres", {0, 0, 0}}}}},
					{{"viewType", "Game"}, {"viewId", 2}, {"origin", {{"cell", {0, 0, 0}}, {"offsetMetres", {100, 0, 0}}}}}})}};
				else if(action == "different-caster-selections") payload = {{"views", Json::array({
					{{"viewType", "Scene"}, {"viewId", 1}, {"casterObjectIds", {Id(10), Id(11), Id(12)}}},
					{{"viewType", "Game"}, {"viewId", 2}, {"casterObjectIds", {Id(21), Id(31), Id(3)}}}})}};
				else if(action == "alternate-view-order") payload = {{"viewOrder", Json::array({{{"viewType", "Game"}, {"viewId", 2}}, {{"viewType", "Scene"}, {"viewId", 1}}})}};
				// Reorder real outputs to change their native snapshot slots; no
				// synthetic slot number participates in renderer proof.
				else if(action == "alternate-snapshot-slot") payload = {{"viewOrder", Json::array({{{"viewType", "Game"}, {"viewId", 2}}, {{"viewType", "Scene"}, {"viewId", 1}}})}};
				// The renderer submits each actual output once. Distinct B/Scene and
				// A/Game origins exercise ownership overwrite in that real order.
				else if(action == "B-record-overwrite-before-A-old-owner-hit") payload = {{"viewOrder", Json::array({
					{{"viewType", "Scene"}, {"viewId", 1}}, {{"viewType", "Game"}, {"viewId", 2}}})}, {"views", Json::array({
					{{"viewType", "Scene"}, {"viewId", 1}, {"origin", {{"cell", {0, 0, 0}}, {"offsetMetres", {100, 0, 0}}}}},
					{{"viewType", "Game"}, {"viewId", 2}, {"origin", {{"cell", {0, 0, 0}}, {"offsetMetres", {0, 0, 0}}}}}})}, {"requireOldOwnerMiss", true}};
				else throw std::runtime_error("Unknown frozen multiview P1 action.");
			}
			else if(fixtureId == "recovery-capacity")
			{
				requirements.push_back("OwnedRecoveryMutation:" + action);
				payload = {{"faultKind", action}, {"afterRecordedFrame", true}, {"requireFreshCompletionAfterFault", true}};
				if(action == "incomplete-point-faces") payload["faceIndices"] = {0, 1, 2, 3, 4};
				if(action == "transaction-conflict-rollback") payload = {{"worldPose", Pose({1, 0, 0}).at("WorldPose")},
					{"deleteTarget", true}, {"writeAfterDelete", true}, {"expectedTransactionStatus", "Rejected"}};
			}
			Json target = {{"objectId", targets.at(role).at("objectId")}, {"componentName", targets.at(role).at("componentName")}, {"expectedLifetime", nullptr}, {"expectedComponentGeneration", nullptr}};
			if(action == "light-assignment") { target["objectId"] = Id(40); target["componentName"] = "LightComponent"; }
			Json stepBundle = bundle;
			if(action == "light-assignment") for(auto& view : stepBundle["views"]) if(view["lightId"] == Id(2)) view["lightId"] = Id(40);
			if(sharedLighting)
			{
				// Preserve the original shadow/camera/fault payload fields. Lighting
				// order and membership are additional typed inputs for this same step.
				Json order = sharedOrder, mutations = Json::array();
				if(action == "buffer-overflow") for(const auto& id : sharedPool) {
					if(std::find(order.begin(),order.end(),id)!=order.end()) continue;
					order.push_back(id); mutations.push_back({{"operation","SetActive"},{"objectId",id},{"active",true}});
				}
				if(sharedBatch) {
					payload["shadowBatchControls"]={{"maxInstancesPerGroup",action=="group-overflow"?64:0},{"maxTaskGroupsX",0},{"failInstanceUpload",false}};
					if(action=="group-overflow") {payload["casterObjectIds"]=batchPool;for(const auto& id:batchPool) mutations.push_back({{"operation","SetActive"},{"objectId",id},{"active",true}});}
					if(action=="unsupported-grouped-route") {payload["casterObjectIds"]={Id(21),Id(31)};payload["seekTick"]=30;requirements.push_back("ActualCpuConservativeDeformation");}
				}
				Json baseline=sharedBaseline;for(const auto& request:batchBaseline) baseline.push_back(request);
				if(sharedStatic) {
					// Earlier stages keep their original membership. P5 records actual
					// static category writes through the same protected queue boundary.
					for(const unsigned id:{10U,11U,12U}) baseline.push_back({{"operation","SetStatic"},{"objectId",Id(id)},{"isStatic",true}});
					for(const unsigned id:{21U,31U}) baseline.push_back({{"operation","SetStatic"},{"objectId",Id(id)},{"isStatic",false}});
					for(const auto& id:batchPool) baseline.push_back({{"operation","SetStatic"},{"objectId",id},{"isStatic",true}});
					payload["staticShadowControls"]={{"forceTransferUnavailable",action=="unsupported-depth-transfer"},{"failAllocation",false}};
					payload["staticShadowCacheBudgetBytes"]=action=="zero-budget"?0:action=="too-small-budget"?1:67108864;
					if(action=="lower-budget-in-flight")payload["lowerBudgetWhilePendingBytes"]=0;
				}
				payload["baselineTransactionRequests"] = baseline; payload["transactionRequests"] = mutations;
				payload["baselineCandidateOrder"] = sharedOrder; payload["candidateOrder"] = order;
				payload["lightingBudgets"] = {{"maxUploadedLights",1024},{"maxDynamicLights",1024},{"maxShadowLights",4}};
				payload["expectedRecipe"] = {{"baselineSourceCount",sharedOrder.size()},{"sourceCount",order.size()},
					{"analysisCount",std::min<std::size_t>(order.size(),1024)},{"retainedCount",std::min<std::size_t>(order.size(),1024)},
					{"selectionOrder","authored-priority-descending-stable-id"}};
				for(auto& view : stepBundle["views"])
				{
					view["lightingPurposes"] = {"FinalLights","Analysis"};
					if(sharedDepth) view["depthPurposes"] = {"Production"};
				}
				if(sharedDepth) requirements.push_back("CompletionQualifiedLightingDepthEvidence");
				requirements.push_back("TypedLightingBudgetControl"); requirements.push_back("TypedLightingCandidateOrder");
				requirements.push_back("CompletionQualifiedLightingBufferEvidence"); requirements.push_back("ActualPhysicalSlotCoverage");
			}
			Json resourceGenerations = Json::array();
			if(action != "stable") resourceGenerations.push_back(action);
			output.steps["steps"].push_back({{"id", definitionStep.at("id")}, {"ordinal", definitionStep.at("ordinal")}, {"action", action}, {"target", target},
				{"nativePayload", payload}, {"requiredCapabilities", requirements}, {"requiredBundle", stepBundle}, {"warmupFrames", 60}, {"measuredFrames", 300},
				{"observation", {{"requireSettingsGeneration", true}, {"requireMutationGeneration", true}, {"requiredResourceGenerations", resourceGenerations}}},
				{"support", {{"status", "RequiresRuntimeCapability"}, {"reason", "Require published asset bindings and actual completion-qualified native observation; missing hooks are Unavailable."}}}});
		}
		const std::size_t required = fixtureId == "shadow-invalidation" ? 17 : fixtureId == "recovery-capacity" && sharedStatic ? 14 : fixtureId == "recovery-capacity" && sharedBatch ? 8 : fixtureId == "recovery-capacity" && sharedLighting ? 6 : 5;
		if(output.steps["steps"].size() != required) throw std::runtime_error("Stage-qualified recipe is missing frozen applicable steps.");
		output.files.emplace_back("renderer-efficiency-steps.json", output.steps);
		return output;
	}
}
