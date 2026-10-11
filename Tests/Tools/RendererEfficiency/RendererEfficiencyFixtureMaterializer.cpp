// RendererEfficiencyFixtureMaterializer.cpp — installed-only disposable bootstrap.
// Usage: TerraEngine-RendererEfficiencyFixtureMaterializer --install-root <Build>
//   --work-root <fresh directory> --fixture <manifest ID> [--manifest <installed JSON>]
//   [--dry-run]. Link Windows bcrypt.lib and the existing committed JSON/GLM headers.
// P1 additionally authors deterministic sources and a sealed step recipe. Import,
// resource publication, and GPU observation remain mandatory runtime gates.

#include <windows.h>
#include <bcrypt.h>
#include <nlohmann/json.hpp>
#include <glm/gtc/quaternion.hpp>
#include "ShadowInvalidationFixture.h"
#include "LightingDirtyFixture.h"
#include "LightingDepthFixture.h"
#include "ShadowBatchingFixture.h"
#include "StaticShadowFixture.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
	using Json = nlohmann::json;
	namespace fs = std::filesystem;
	constexpr std::uintmax_t kMaximumInputBytes = 64ULL * 1024ULL * 1024ULL;
	constexpr std::size_t kMaximumInputFiles = 256U;

	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	// Cryptographic handles are process-local and always released on exceptions.
	// Hashing is streamed so an installed asset never requires an unbounded buffer.
	struct Sha256
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hash = nullptr;
		std::vector<unsigned char> object;
		Sha256()
		{
			Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
				"Cannot open Windows SHA256 provider.");
			DWORD bytes = 0, returned = 0;
			const auto result = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&bytes), sizeof(bytes), &returned, 0);
			if (result < 0)
			{
				BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr;
				throw std::runtime_error("Cannot query SHA256 state size.");
			}
			try
			{
				object.resize(bytes);
				Require(BCryptCreateHash(algorithm, &hash, object.data(), bytes, nullptr, 0, 0) >= 0,
					"Cannot create SHA256 state.");
			}
			catch (...)
			{
				BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; throw;
			}
		}
		~Sha256()
		{
			if (hash) BCryptDestroyHash(hash);
			if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
		}
		Sha256(const Sha256&) = delete;
		Sha256& operator=(const Sha256&) = delete;
	};

	std::string HashFile(const fs::path& path)
	{
		Require(fs::is_regular_file(path) && fs::file_size(path) <= kMaximumInputBytes,
			"Input is missing, not a regular file, or exceeds the 64-MiB bootstrap bound.");
		std::ifstream input(path, std::ios::binary);
		Require(input.good(), "Cannot read input for SHA256.");
		Sha256 state;
		std::array<unsigned char, 65536U> buffer{};
		while (input)
		{
			input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
			if (input.gcount() > 0)
				Require(BCryptHashData(state.hash, buffer.data(), static_cast<ULONG>(input.gcount()), 0) >= 0,
					"Cannot hash installed input bytes.");
		}
		Require(input.eof(), "Read failed before reaching input EOF.");
		std::array<unsigned char, 32U> digest{};
		Require(BCryptFinishHash(state.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
			"Cannot finish input SHA256.");
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		for (const auto byte : digest) { result += hex[byte >> 4U]; result += hex[byte & 15U]; }
		return result;
	}

	bool IsWithin(const fs::path& child, const fs::path& root)
	{
		auto c = child.begin();
		for (auto r = root.begin(); r != root.end(); ++r, ++c)
			if (c == child.end() || _wcsicmp(c->c_str(), r->c_str()) != 0) return false;
		return true;
	}

	// Reject junctions as well as symlinks. Canonical containment alone would
	// allow an installed input or output ancestor to redirect outside its owner.
	void RejectRedirects(const fs::path& path)
	{
		fs::path current;
		for (const auto& component : fs::absolute(path))
		{
			current /= component;
			if (!fs::exists(current)) continue;
			const auto attributes = GetFileAttributesW(current.c_str());
			Require(attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
				"Reparse points are not permitted in fixture input/output paths.");
		}
	}

	fs::path InstalledPath(const fs::path& root, const fs::path& relative)
	{
		Require(!relative.is_absolute(), "Installed input must be relative to InstallRoot.");
		auto path = root / relative;
		// Metadata uses portable slash-separated paths; normalize before Win32
		// canonical checks, which otherwise reject mixed separator spellings.
		path.make_preferred();
		RejectRedirects(path);
		Require(IsWithin(fs::canonical(path), root), "Installed input escaped InstallRoot.");
		return fs::canonical(path);
	}

	Json ReadJson(const fs::path& path)
	{
		Require(fs::file_size(path) <= kMaximumInputBytes, "JSON input exceeds the bootstrap bound.");
		std::ifstream input(path, std::ios::binary);
		Require(input.good(), "Cannot read JSON input.");
		return Json::parse(input);
	}

	void WriteJson(const fs::path& path, const Json& value)
	{
		fs::create_directories(path.parent_path());
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		Require(output.good(), "Cannot create generated JSON.");
		output << value.dump(1, '\t') << '\n';
		output.flush();
		Require(output.good(), "Cannot finish generated JSON.");
	}

	Json Pose(const Json& position, const Json& rotation = Json::array({1, 0, 0, 0}),
		const Json& scale = Json::array({1, 1, 1}))
	{
		// Cells use decimal strings, matching the canonical unbounded scene codec.
		return {{"WorldPose", {{"position", {{"cell", {{"x", "0"}, {"y", "0"}, {"z", "0"}}},
			{"offsetMetres", position}}}, {"rotation", rotation}, {"scale", scale}}},
			{"ParentRelativePose", {{"translationMetres", position}, {"rotation", rotation},
			{"scale", scale}, {"transformSpaceId", 0}}}};
	}

	void CanonicalizePrefabPoses(Json& object)
	{
		if (object.contains("components") && object["components"].contains("Transform"))
		{
			const auto old = object["components"]["Transform"];
			object["components"]["Transform"] = Pose(old.value("Position", Json::array({0, 0, 0})),
				old.value("Rotation", Json::array({1, 0, 0, 0})), old.value("Scale", Json::array({1, 1, 1})));
		}
		if (object.contains("children"))
			for (auto& child : object["children"]) CanonicalizePrefabPoses(child);
	}

	Json BuildScene(const Json& prefab, const Json& camera)
	{
		Require(prefab.contains("objects") && !prefab["objects"].empty(), "Imported hero prefab has no caster.");
		const auto position = camera.at("position");
		const auto target = camera.at("target");
		const glm::vec3 p(position[0].get<float>(), position[1].get<float>(), position[2].get<float>());
		const glm::vec3 t(target[0].get<float>(), target[1].get<float>(), target[2].get<float>());
		Require(glm::length(t - p) > 0.01F, "Camera target must differ from position.");
		const auto q = glm::quatLookAtRH(glm::normalize(t - p), glm::vec3(0, 1, 0));
		Json scene = {{"format", "TerraSceneUnboundedV1"}, {"name", "Renderer Efficiency Foundation"},
			{"sceneSettings", {{"ambientLight", {0.08, 0.09, 0.11}}, {"exposure", 1.0}}}, {"objects", Json::array()}};
		scene["objects"].push_back({{"uuid", "65000000000000000000000000000001"}, {"name", "Foundation Camera"},
			{"components", {{"Transform", Pose(position, {q.w, q.x, q.y, q.z})},
			{"CameraComponent", {{"verticalFovDegrees", camera.at("verticalFovDegrees")}, {"aspectRatio", 1920.0 / 1080.0},
			{"nearPlane", camera.at("nearPlane")}, {"farPlane", camera.at("farPlane")}, {"up", {0, 1, 0}}}}}}, {"children", Json::array()}});
		scene["objects"].push_back({{"uuid", "65000000000000000000000000000002"}, {"name", "Foundation Shadow Light"},
			{"components", {{"Transform", Pose({3, 5, 4})}, {"LightComponent", {{"type", 1}, {"unit", 0},
			{"color", {1, 1, 1}}, {"intensity", 1000.0}, {"range", 30.0}, {"castsShadows", true}}}}}, {"children", Json::array()}});
		for (auto object : prefab["objects"])
		{
			CanonicalizePrefabPoses(object);
			scene["objects"].push_back(std::move(object));
		}
		// A bind-pose triangle below the hero gives the minimal scene real receiver
		// geometry without inventing a mesh codec or depending on built-in aliases.
		scene["objects"].push_back({{"uuid", "65000000000000000000000000000003"}, {"name", "Foundation Receiver"},
			{"isStatic", true}, {"components", {{"Transform", Pose({0, -0.1, 0}, {0.70710678, 0.70710678, 0, 0}, {10, 10, 10})},
			{"MeshRenderComponent", {{"meshId", "3ce0c4c32b4c190edd4cb4371e9b055b"},
			{"materialId", "2b611b72eec7bdb66b0da4fe40360f82"}, {"taskShaderCullingEnabled", false}}}}}, {"children", Json::array()}});
		return scene;
	}

	struct CopyInput { fs::path source, destination; std::string hash; std::uintmax_t bytes = 0; };

	int Run(int argc, char** argv)
	{
		fs::path installArgument, workArgument, manifestArgument;
		std::string fixtureId;
		bool dryRun = false;
		std::string requestedStage;
		for (int i = 1; i < argc; ++i)
		{
			const std::string option = argv[i];
			if (option == "--dry-run") { Require(!dryRun, "Duplicate --dry-run."); dryRun = true; continue; }
			Require(i + 1 < argc, "CLI option requires a value.");
			const std::string value = argv[++i];
			if (option == "--install-root") { Require(installArgument.empty(), "Duplicate install root."); installArgument = value; }
			else if (option == "--work-root") { Require(workArgument.empty(), "Duplicate work root."); workArgument = value; }
			else if (option == "--manifest") { Require(manifestArgument.empty(), "Duplicate manifest."); manifestArgument = value; }
			else if (option == "--fixture") { Require(fixtureId.empty(), "Duplicate fixture ID."); fixtureId = value; }
			else if (option == "--stage") { Require(requestedStage.empty(), "Duplicate renderer stage."); requestedStage = value; }
			else throw std::runtime_error("Unknown materializer option: " + option);
		}
		Require(!installArgument.empty() && !workArgument.empty() && !fixtureId.empty(),
			"Required: --install-root <Build> --work-root <fresh> --fixture <id>.");
		RejectRedirects(installArgument); RejectRedirects(workArgument);
		const auto install = fs::canonical(installArgument);
		const auto work = fs::weakly_canonical(fs::absolute(workArgument));
		Require(!IsWithin(work, install) && !IsWithin(install, work), "WorkRoot and InstallRoot must not overlap.");
		Require(!fs::exists(work) || (fs::is_directory(work) && fs::is_empty(work)), "WorkRoot must be fresh or empty.");
		const auto manifest = manifestArgument.empty()
			? InstalledPath(install, "Tests/Fixtures/RendererEfficiency/workloads.json")
			: InstalledPath(install, fs::relative(fs::absolute(manifestArgument), install));
		const auto manifestHash = HashFile(manifest);
		const auto workloads = ReadJson(manifest);
		Require(workloads.at("schemaVersion") == 1 && workloads.at("workloads").size() == 9,
			"Expected frozen version-1 nine-workload manifest.");
		constexpr std::array<std::string_view, 9> frozenIds{"shadow-invalidation", "lighting-dirty", "lighting-thresholds",
			"depth-coverage", "depth-heavy", "shadow-batching", "static-dynamic", "multiview-lights", "recovery-capacity"};
		for (std::size_t i = 0; i < frozenIds.size(); ++i)
			Require(workloads["workloads"][i].at("id").get<std::string>() == frozenIds[i]
				&& workloads["workloads"][i].at("seed").get<std::uint64_t>() == 65001U + i,
				"Installed workload identity/order/seed differs from the frozen matrix.");
		const auto selected = std::find_if(workloads["workloads"].begin(), workloads["workloads"].end(),
			[&](const Json& value) { return value.at("id") == fixtureId; });
		Require(selected != workloads["workloads"].end(), "Fixture ID is not in the installed manifest.");
		// Shared recipes are materialized for the concrete requested stage. A
		// standalone invocation retains the original P1 shared-fixture default.
		if (requestedStage.empty()) requestedStage = selected->at("stage") == "All" ? "P1" : selected->at("stage").get<std::string>();
		Require(requestedStage == "P1" || requestedStage == "P2" || requestedStage == "P3" || requestedStage == "P4" || requestedStage == "P5",
			"A concrete renderer stage P1 through P5 is required.");
		Require(selected->at("stage") == "All" || selected->at("stage") == requestedStage, "Fixture belongs to a different requested stage.");
		Require(selected->at("definitionStatus") == "DefinitionOnly", "Unexpected fixture implementation receipt.");
		const auto seed = selected->at("seed").get<std::uint64_t>();
		Require(seed >= 65001 && seed <= 65009, "Fixture seed is outside the frozen range.");
		const auto inputRoot = InstalledPath(install, "Tests/Fixtures/RendererEfficiency/Inputs/AnimationImportValidation");
		const auto project = work / "project";
		std::vector<CopyInput> inputs;
		const auto append = [&](const fs::path& source, const fs::path& destination) {
			Require(inputs.size() < kMaximumInputFiles, "Input file count exceeds bounded bootstrap closure.");
			RejectRedirects(source);
			Require(IsWithin(fs::canonical(source), install), "Input closure escaped InstallRoot.");
			inputs.push_back({source, destination, HashFile(source), fs::file_size(source)});
		};
		for (const auto& relative : {fs::path("Assets/Models"), fs::path("Build/Assets")})
		{
			auto root = inputRoot / relative;
			root.make_preferred();
			Require(fs::is_directory(root), "Installed animation source/cooked closure is missing.");
			for (const auto& entry : fs::recursive_directory_iterator(root))
			{
				RejectRedirects(entry.path());
				if (entry.is_regular_file()) append(entry.path(), project / fs::relative(entry.path(), inputRoot));
			}
		}
		for (const auto* layout : {"EditorLayout.terra", "EditorLayout.ini"})
			append(InstalledPath(install, fs::path("Editor/Project/Templates/UserSettings") / layout), project / "UserSettings" / layout);
		std::sort(inputs.begin(), inputs.end(), [](const auto& a, const auto& b) { return a.destination < b.destination; });
		const auto importMetadata = ReadJson(inputRoot / "Assets/Models/ImportedHero.gltf.import.json");
		const auto& animationManifest = importMetadata.at("animationManifest");
		Require(animationManifest.at("publishedAtomically") == true && animationManifest.at("publishedGeneration").get<unsigned>() > 0,
			"Animation publication manifest is not complete.");
		for (const auto& child : animationManifest.at("subAssets"))
		{
			// Prefab children carry no cooked payload; their authored source is copied
			// separately. Morph/deformable children share the exact same mesh file.
			const auto compiledPath = child.at("compiledPath").get<std::string>();
			if (compiledPath.empty())
			{
				Require(child.at("kind") == "Prefab", "Required animation child has no compiled payload.");
				continue;
			}
			const auto path = InstalledPath(install, fs::path("Tests/Fixtures/RendererEfficiency/Inputs/AnimationImportValidation/Build/Assets") / compiledPath);
			Require(fs::file_size(path) > 0, "Animation publication child is empty.");
		}
		const auto prefab = ReadJson(inputRoot / "Assets/Models/ImportedHero_Generated/ImportedHero.teprefab");
		auto scene = BuildScene(prefab, workloads.at("cameras").at(0));
		std::vector<std::pair<fs::path, Json>> stageFiles;
		const bool hasP1Recipe = fixtureId == "shadow-invalidation" || fixtureId == "multiview-lights" || fixtureId == "recovery-capacity";
		const bool hasP2Recipe = fixtureId == "lighting-dirty" || fixtureId == "lighting-thresholds";
		const bool hasP3Recipe = fixtureId == "depth-coverage" || fixtureId == "depth-heavy";
		const bool hasP4Recipe = fixtureId == "shadow-batching";
		const bool hasP5Recipe = fixtureId == "static-dynamic";
		if(hasP1Recipe)
		{
			// Prepare pure detached values, including in dry-run. Writing remains
			// below the no-write return and ownership is always published last.
			auto fixture = TerraEngine::Tests::RendererEfficiency::BuildShadowInvalidationFixture(scene,
				ReadJson(inputRoot / "Assets/Models/ImportedHero.gltf"), *selected, manifestHash, seed, requestedStage);
			scene = std::move(fixture.scene);
			stageFiles = std::move(fixture.files);
		}
		else if (hasP2Recipe)
		{
			// P2 uses the same installed foundation geometry and ownership marker.
			// The pure producer authors actual light membership and typed steps.
			auto fixture = TerraEngine::Tests::RendererEfficiency::BuildLightingDirtyFixture(scene, *selected, manifestHash, seed);
			scene = std::move(fixture.scene); stageFiles = std::move(fixture.files);
		}
		else if(hasP3Recipe)
		{
			// Authored panels and isolated oracle inputs remain definition-only until
			// the native importer, protected controller and GPU collector prove them.
			auto fixture = TerraEngine::Tests::RendererEfficiency::BuildLightingDepthFixture(scene,*selected,manifestHash,seed);
			scene=std::move(fixture.scene); stageFiles=std::move(fixture.files);
		}
		else if(hasP4Recipe)
		{
			// P4 shares actual cooked inputs; no authored compatibility or route
			// flag can substitute for later native execution and readback receipts.
			auto fixture=TerraEngine::Tests::RendererEfficiency::BuildShadowBatchingFixture(scene,
				ReadJson(inputRoot/"Assets/Models/ImportedHero.gltf"),*selected,manifestHash,seed);
			scene=std::move(fixture.scene);stageFiles=std::move(fixture.files);
		}
		else if(hasP5Recipe)
		{
			// P5 shares real cooked static owners and CPU animation inputs. The
			// sealed definition never substitutes for completed native cache proof.
			auto fixture=TerraEngine::Tests::RendererEfficiency::BuildStaticShadowFixture(scene,
				ReadJson(inputRoot/"Assets/Models/ImportedHero.gltf"),*selected,manifestHash,seed);
			scene=std::move(fixture.scene);stageFiles=std::move(fixture.files);
		}
		// Seal both actual output cameras and the native render extent together.
		// The runtime copies this recipe into typed frame intent rather than reading
		// mutable editor orbit/panel settings or repairing author camera components.
		Json selectedCameras = Json::array();
		for(const auto& id : selected->at("cameraIds"))
		{
			const auto found = std::find_if(workloads.at("cameras").begin(),workloads.at("cameras").end(),[&](const auto& camera){return camera.at("id")==id;});
			Require(found!=workloads.at("cameras").end(),"Frozen workload selects an unknown camera.");selectedCameras.push_back(*found);
		}
		const Json cameraRecipe = {{"schemaVersion",1}, {"cameras",selectedCameras},
			{"nativeExtent",workloads.at("measurementPolicy").at("primaryResolution")}};
		for(auto& [path,value] : stageFiles) if(path == "renderer-efficiency-steps.json")
		{
			value["cameras"] = cameraRecipe.at("cameras");
			value["nativeExtent"] = cameraRecipe.at("nativeExtent");
			if(hasP3Recipe) for(auto& step : value.at("steps"))
			{
				// Every selected sealed camera keeps its pose/projection. Only this
				// step's native output size differs; panel/UI settings are irrelevant.
				const auto& extent=step.at("nativeExtent");
				Require(extent.at("width").get<unsigned>()>0 && extent.at("width").get<unsigned>()<=1920
					&& extent.at("height").get<unsigned>()>0 && extent.at("height").get<unsigned>()<=1080,"Depth native extent exceeds recipe bounds.");
				step["cameraTransforms"]={{"cameras",cameraRecipe.at("cameras")},{"nativeExtent",extent}};
			}
			if(hasP4Recipe) for(auto& step:value.at("steps"))
			{
				Json cameras=Json::array();const bool offCamera=step.at("action")=="off-camera";
				for(auto camera:cameraRecipe.at("cameras")) {
					if(camera.at("viewType")=="Game" && !offCamera) continue;
					// Scene aims across the enlarged receiver, placing part of the
					// caster pool outside its frustum without producing an empty image.
					if(offCamera && camera.at("viewType")=="Scene") {camera["target"][0]=camera.at("target")[0].get<double>()+12;camera["target"][1]=-0.1;}
					cameras.push_back(std::move(camera));
				}
				step["cameraTransforms"]={{"cameras",std::move(cameras)},{"nativeExtent",cameraRecipe.at("nativeExtent")}};
			}
			if(hasP5Recipe) for(auto& step:value.at("steps"))
			{
				Json cameras=Json::array();for(const auto& camera:cameraRecipe.at("cameras"))
					if(camera.at("viewType")=="Scene" || step.at("action")=="retirement-view-churn")cameras.push_back(camera);
				step["cameraTransforms"]={{"cameras",std::move(cameras)},{"nativeExtent",cameraRecipe.at("nativeExtent")}};
			}
		}
		Json receipt = {{"schemaVersion", 1}, {"kind", "RendererEfficiencyValidationProject"},
			{"projectRoot", project.generic_string()}, {"canonicalProjectRoot", project.generic_string()}, {"fixtureId", fixtureId},
			{"fixtureHash", manifestHash}, {"manifestHash", manifestHash}, {"fixtureManifestSha256", manifestHash}, {"seed", seed},
			{"ownerId", "renderer-efficiency-" + manifestHash.substr(0, 16) + "-" + fixtureId},
			{"foundationStatus", "FoundationOnly"}, {"workloadStatus", "DefinitionOnly"}, {"dryRun", dryRun},
			{"installRoot", install.generic_string()}, {"inputClosure", Json::array()}, {"generatedFiles", Json::array()}};
		for (const auto& input : inputs)
			receipt["inputClosure"].push_back({{"installedPath", fs::relative(input.source, install).generic_string()},
				{"projectPath", fs::relative(input.destination, project).generic_string()}, {"sha256", input.hash}, {"sizeBytes", input.bytes}});
		if (dryRun) { std::cout << receipt.dump(1, '\t') << '\n'; return 0; }
		fs::create_directories(project);
		// This bounded bootstrap copies a few small files once, outside frame work.
		// Sequential copy+verification preserves failure diagnostics and avoids a
		// DLL/task-pool runtime dependency for the standalone installed utility.
		for (const auto& input : inputs)
		{
			Require(HashFile(input.source) == input.hash, "Installed input changed during materialization.");
			fs::create_directories(input.destination.parent_path());
			fs::copy_file(input.source, input.destination, fs::copy_options::none);
			Require(HashFile(input.destination) == input.hash, "Copied input SHA256 does not match.");
		}
		std::vector<std::pair<fs::path, Json>> generated = {
			{"Assets/Scenes/Scene.tescene", scene},
			{"renderer-efficiency-camera.json", cameraRecipe},
			{"project.json", {{"name", "Renderer Efficiency Foundation"}, {"version", "1.0.0"}, {"startupScene", "Scenes/Scene.tescene"}}},
			{"game.json", {{"name", "Renderer Efficiency Foundation"}, {"version", "1.0.0"}, {"startupScene", "Scenes/Scene.tescene"}}},
			{"ProjectSettings/ProjectSettings.terra", {{"projectDisplayName", "Renderer Efficiency Foundation"}}},
			{"ProjectSettings/RenderSettings.json", {{"preset", 4}, {"validationEnabled", false}, {"debugEnabled", false},
				{"settings", {{"maxUploadedLights", 1024}, {"maxDynamicLights", 1024}, {"maxShadowLights", 4}, {"shadowResolution", 512}}}}},
			{"Assets/Models/RendererEfficiency.animation-manifest.json", animationManifest}
		};
		generated.insert(generated.end(), stageFiles.begin(), stageFiles.end());
		for (auto& [relative, value] : generated)
		{
			if(hasP3Recipe && relative=="renderer-efficiency-steps.json") for(auto& step:value.at("steps"))
			{
				if(!step.at("nativePayload").contains("depthRecipePath")) continue;
				const fs::path recipePath=step.at("nativePayload").at("depthRecipePath").get<std::string>();
				// Recipes precede the steps file in detached output. Seal actual disk
				// bytes and check inline equality before marker authority is published.
				Require(!recipePath.is_absolute() && recipePath.generic_string().starts_with("renderer-efficiency-depth/")
					&& recipePath.lexically_normal()==recipePath,"Depth recipe path is not owned/portable.");
				RejectRedirects(project/recipePath);
				Require(ReadJson(project/recipePath).at("recipe")==step.at("nativePayload").at("depthInputRecipe"),"Inline depth recipe differs from sealed input.");
				step["nativePayload"]["depthRecipeSha256"]=HashFile(project/recipePath);
			}
			WriteJson(project / relative, value);
			receipt["generatedFiles"].push_back({{"projectPath", relative.generic_string()}, {"sha256", HashFile(project / relative)}});
		}
		if(hasP1Recipe || hasP2Recipe || hasP3Recipe || hasP4Recipe || hasP5Recipe)
		{
			receipt["stepsPath"] = "renderer-efficiency-steps.json";
			receipt["stepsHash"] = HashFile(project / "renderer-efficiency-steps.json");
			receipt["fixtureStatus"] = "AuthoredAwaitingRuntimePublication";
		}
		receipt["cameraHash"] = HashFile(project / "renderer-efficiency-camera.json");
		// Normal editor startup reads Library/LastScene.txt as an Assets-relative
		// plain-text path (MainEditorSubsystem), independent of project.json.
		// Select the generated scene before the editor opens this owned project.
		fs::create_directories(project / "Library");
		{
			std::ofstream lastScene(project / "Library/LastScene.txt", std::ios::binary);
			lastScene << "Scenes/Scene.tescene";
			lastScene.flush();
			Require(lastScene.good(), "Cannot write owned startup scene selection.");
		}
		receipt["generatedFiles"].push_back({{"projectPath", "Library/LastScene.txt"},
			{"sha256", HashFile(project / "Library/LastScene.txt")}});
		// Publish ownership last: partial materialization cannot authorize the editor
		// fixture controller. Failed work is preserved for diagnosis, never deleted.
		WriteJson(project / "renderer-efficiency-project.json", receipt);
		WriteJson(work / "materialization.json", receipt);
		std::cout << receipt.dump(1, '\t') << '\n';
		return 0;
	}
}

int main(int argc, char** argv)
{
	try { return Run(argc, argv); }
	catch (const std::exception& error)
	{
		std::cerr << "Renderer efficiency materialization rejected: " << error.what() << '\n';
		return 3;
	}
}
