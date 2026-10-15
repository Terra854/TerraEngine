// Matching-ABI normal-renderer validation host. See README for prerequisites.
// Usage: --runtime-root <installed Build> --project-root <isolated fixture>
// --output-root <fresh directory> --archive-receipt <immutable reference.json>.
// --materialization <fresh runtime-memory-safety materialization.json> and
// --qualification <approved full post-import qualification.json> and
// --support-manifest <installed common support declaration pins> are mandatory.
// The shared source is compiled separately against archived/current engine ABI.
#include "Core/MainEngine.h"
#include "Renderer/RenderSystem.h"
#include "Renderer/RendererEfficiencyArtifacts.h"
#include "Renderer/RHI/Vulkan/VulkanReadbackConversion.h"
#include "Renderer/RHI/Vulkan/VulkanContext.h"
#include "Resource/ResourceManager.h"
#include "Resource/VirtualGeometryCodec.h"
#include "Scene/SceneManager.h"
#include "Animation/AnimationSystem.h"
#include "Audio/AudioSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scripting/ScriptingSystem.h"
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
#include "Renderer/Graph/RenderFrameScratchValidation.h"
#include <bit>
#include <tuple>
#endif
#include <nlohmann/json.hpp>
#include <bcrypt.h>
#include <array>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <map>
#include <set>
#include <thread>

namespace
{
	using Json = nlohmann::json;
	using namespace TerraEngine::Renderer;
	using Clock = std::chrono::steady_clock;
	void Require(bool condition, const std::string& reason)
	{
		if(!condition) throw std::runtime_error(reason);
	}
	std::filesystem::path Path(std::string_view text)
	{
		return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
	}
	std::string Hash(const std::filesystem::path& path)
	{
		// SHA256 uses the Windows SDK, already required by the engine build. Input
		// bytes are streamed; this never reads a GPU handle or runtime object layout.
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider unavailable");
		struct CloseAlgorithm { BCRYPT_ALG_HANDLE value; ~CloseAlgorithm() { BCryptCloseAlgorithmProvider(value, 0); } } closeAlgorithm{algorithm};
		BCRYPT_HASH_HANDLE hash = nullptr;
		Require(BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0, "SHA256 context unavailable");
		struct CloseHash { BCRYPT_HASH_HANDLE value; ~CloseHash() { BCryptDestroyHash(value); } } closeHash{hash};
		std::ifstream input(path, std::ios::binary);
		Require(input.is_open(), "Cannot hash input: " + path.string());
		std::array<unsigned char, 65536> buffer{};
		while(input) {
			input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
			const auto size = input.gcount();
			if(size) Require(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(size), 0) >= 0, "SHA256 update failed");
		}
		Require(input.eof(), "Hash input read failed");
		std::array<unsigned char, 32> digest{};
		Require(BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0, "SHA256 finish failed");
		constexpr char hex[] = "0123456789abcdef";
		std::string result;
		for(auto byte : digest) { result.push_back(hex[byte >> 4]); result.push_back(hex[byte & 15]); }
		return result;
	}
	Json Pin(const std::filesystem::path& path)
	{
		return {{"path", path.generic_string()}, {"bytes", std::filesystem::file_size(path)}, {"sha256", Hash(path)}};
	}
	Json ReadJson(const std::filesystem::path& path)
	{
		std::ifstream input(path);
		Require(input.is_open(), "Cannot read JSON: " + path.string());
		Json value; input >> value; return value;
	}
	std::filesystem::path ContainedFile(const std::filesystem::path& root, const std::string& relative)
	{
		// Receipt paths are data, never permission to leave the isolated project.
		const auto child = Path(relative);
		Require(!child.is_absolute() && !child.empty(), "Expected relative closure path");
		const auto resolved = std::filesystem::weakly_canonical(root / child);
		const auto suffix = resolved.lexically_relative(std::filesystem::weakly_canonical(root));
		Require(!suffix.empty() && *suffix.begin() != ".." && std::filesystem::is_regular_file(resolved),
			"Missing or escaping closure file: " + relative);
		return resolved;
	}
	void VerifyFilePin(const Json& value)
	{
		const auto path = Path(value.at("path").get<std::string>());
		Require(path.is_absolute() && std::filesystem::is_regular_file(path) && std::filesystem::file_size(path) == value.at("bytes")
			&& Hash(path) == value.at("sha256"), "Qualification source/payload pin changed: " + path.string());
	}
	std::vector<std::uint8_t> DecodeCaptureBase64(std::string_view text)
	{
		// Decode only the engine's completed PNG transport, with bounded strict
		// padding. Pixel decoding/comparison remains an independent Python check.
		Require(!text.empty() && text.size() % 4 == 0 && text.size() <= 128ULL * 1024 * 1024, "Invalid capture base64 extent");
		const std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::vector<std::uint8_t> bytes; bytes.reserve(text.size() / 4 * 3);
		for(std::size_t i = 0; i < text.size(); i += 4) {
			std::uint32_t value = 0; unsigned padding = 0;
			for(unsigned n = 0; n < 4; ++n) {
				const char ch = text[i + n];
				if(ch == '=') { Require(i + 4 == text.size() && n >= 2, "Invalid capture base64 padding"); ++padding; value <<= 6; }
				else { const auto index = alphabet.find(ch); Require(index != std::string_view::npos && padding == 0, "Invalid capture base64 character"); value = (value << 6) | static_cast<unsigned>(index); }
			}
			Require(padding <= 2, "Invalid capture base64 padding count");
			bytes.push_back(static_cast<std::uint8_t>(value >> 16));
			if(padding < 2) bytes.push_back(static_cast<std::uint8_t>(value >> 8));
			if(padding < 1) bytes.push_back(static_cast<std::uint8_t>(value));
		}
		return bytes;
	}
	Json PreflightProject(const std::filesystem::path& project, const std::filesystem::path& materialization, const std::filesystem::path& qualification)
	{
		const auto document = ReadJson(materialization);
		const auto qualified = ReadJson(qualification);
		Require(qualified.at("schemaVersion") == "runtime-memory-safety-qualified-runtime-v1" && qualified.at("status") == "Qualified"
			&& qualified.at("policy") == "FullActualPostImportReferenceRuntimeClosure" && qualified.at("scale") == document.at("scale")
			&& std::filesystem::equivalent(Path(qualified.at("projectRoot").get<std::string>()), project), "Require explicit approved full post-import qualification");
		for(const auto* key : {"originalMaterialization", "referenceClosureReceipt", "referenceCapture", "referenceLifecycle", "originalPackageManifest"}) VerifyFilePin(qualified.at(key));
		Require(qualified.at("originalMaterialization").at("sha256") == Hash(materialization), "Original materialization changed during qualification");
		// Exact paths prevent an unrelated file from replacing a required payload.
		const std::set<std::string> runtimePaths{"Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json",
			"Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_493d9bc3.teanim",
			"Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroIdle_fad19fd5.teanim",
			"Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_9beae902.teskel",
			"Build/Assets/Models/ImportedHero_Generated/Animation/ImportedHeroSkeleton_b7d2176c.teskel",
			"Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf",
			"Build/Assets/Models/ImportedHero_Generated/ImportedHeroMaterial.tmat",
			"ProjectSettings/RenderSettings.json",
			"Assets/Scenes/Scene.tescene"};
		std::map<std::string, Json> overlays;
		Require(qualified.at("files").size() == 9U, "Qualified runtime closure must contain all seven cooked files, settings and scene");
		for(const auto& row : qualified.at("files")) {
			const auto relative = row.at("relative").get<std::string>();
			Require(runtimePaths.contains(relative), "Unapproved qualification target");
			const auto file = ContainedFile(project, relative);
			VerifyFilePin(row.at("source")); VerifyFilePin(row.at("qualified"));
			Require(std::filesystem::equivalent(file, Path(row.at("qualified").at("path").get<std::string>()))
				&& row.at("source").at("sha256") == row.at("qualified").at("sha256")
				&& overlays.emplace(relative, row.at("qualified")).second, "Qualified runtime target/source differs");
		}
		Require(document.at("kind") == "RuntimeMemorySafetyValidationProject" && document.at("seed") == 66007,
			"Require runtime-memory-safety materialization, not bootstrap or arbitrary project");
		Require(std::filesystem::equivalent(Path(document.at("projectRoot").get<std::string>()), project), "Materialization project root differs");
		const auto scale = document.at("scale").get<std::string>();
		const std::map<std::string, std::pair<unsigned, unsigned>> counts{{"small", {1, 16}}, {"normal", {4, 256}}, {"stress", {16, 4096}}};
		Require(counts.contains(scale), "Unsupported fixture scale");
		Require(document.at("expectedCameraCount") == counts.at(scale).first && document.at("expectedLightCount") == counts.at(scale).second,
			"Frozen fixture cohort differs");
		Require(document.at("inputClosure").size() == 14, "Incomplete frozen input closure");
		for(const auto& row : document.at("inputClosure")) {
			const auto relative = row.at("projectPath").get<std::string>();
			if(relative == "Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json")
				Require(qualified.at("originalPackageManifest").at("sha256") == row.at("sha256"), "Original GUID alias manifest was not preserved");
			const auto file = ContainedFile(project, relative);
			const auto expectedHash = overlays.contains(relative) ? overlays.at(relative).at("sha256") : row.at("sha256");
			const auto expectedBytes = overlays.contains(relative) ? overlays.at(relative).at("bytes") : row.at("sizeBytes");
			Require(Hash(file) == expectedHash && std::filesystem::file_size(file) == expectedBytes,
				"Initial/explicitly qualified input closure changed: " + file.string());
		}
		for(const auto& [relative, expected] : document.at("generatedHashes").items())
			Require(Hash(ContainedFile(project, relative)) == (overlays.contains(relative) ? overlays.at(relative).at("sha256") : expected), "Generated/qualified fixture input changed: " + relative);
		Require(overlays.at("Assets/Scenes/Scene.tescene").at("sha256") == document.at("generatedHashes").at("Assets/Scenes/Scene.tescene"), "Qualification changed frozen authored scene");
		Require(!std::filesystem::exists(project / "ProjectSettings/PhysicsSettings.json"), "Frozen fixture expects default physics settings");
		const auto profile = ReadJson(ContainedFile(project, "ProjectSettings/RenderSettings.json"));
		Require(profile.at("preset") == 4 && profile.at("validationEnabled") == false && profile.at("debugEnabled") == false,
			"Unexpected frozen renderer profile");
		return document;
	}
	Json PreflightRuntime(const std::filesystem::path& runtime)
	{
		// Loading occurs before main. Verify both staged bytes and already loaded
		// native modules; never select a different ABI with PATH or a late DLL call.
		std::array<wchar_t, 32768> executable{};
		const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		Require(length > 0 && length < executable.size(), "Executable path unavailable");
		const auto staging = std::filesystem::path(executable.data()).parent_path();
		Require(std::filesystem::equivalent(staging, std::filesystem::current_path()), "Working directory must be the isolated host staging directory");
		Json files = Json::array();
		const auto check = [&](const std::filesystem::path& original, const std::filesystem::path& staged) {
			Require(std::filesystem::is_regular_file(staged) && Hash(original) == Hash(staged), "Missing or mismatched staged runtime: " + staged.string());
			files.push_back({{"original", Pin(original)}, {"staged", Pin(staged)}});
		};
		for(const auto& entry : std::filesystem::directory_iterator(runtime / "Editor")) {
			if(!entry.is_regular_file() || entry.path().extension() != ".dll") continue;
			check(entry.path(), staging / entry.path().filename());
			if(const auto module = GetModuleHandleW(entry.path().filename().c_str())) {
				std::array<wchar_t, 32768> loaded{};
				const auto size = GetModuleFileNameW(module, loaded.data(), static_cast<DWORD>(loaded.size()));
				Require(size > 0 && size < loaded.size() && std::filesystem::equivalent(loaded.data(), staging / entry.path().filename()),
					"Native module resolved outside matching staging closure");
			}
		}
		Require(std::filesystem::is_regular_file(runtime / "Editor/Managed/hostfxr.dll") &&
			std::filesystem::is_regular_file(runtime / "Editor/Managed/TerraEngine.CSharp.runtimeconfig.json"), "Self-contained managed runtime absent");
		for(const auto& entry : std::filesystem::recursive_directory_iterator(runtime / "Editor/Managed"))
			if(entry.is_regular_file()) check(entry.path(), staging / "Managed" / entry.path().lexically_relative(runtime / "Editor/Managed"));
		return {{"stagingRoot", staging.generic_string()}, {"matchingClosure", std::move(files)}};
	}
	Json RegisterCookedAssets(const std::filesystem::path& project, const std::filesystem::path& originalManifest)
	{
		const std::map<std::string, AssetType> types{{"DeformableMesh", AssetType::Mesh}, {"Material", AssetType::Material},
			{"Skeleton", AssetType::Skeleton}, {"AnimationClip", AssetType::AnimationClip}};
		Json registered = Json::array(); std::set<std::string> ids;
		// Preserve authored sibling GUIDs, then make the post-import package GUID
		// preferred for each path. Existing ResourceManager aliases return that
		// canonical payload without rewriting the GUID stored in scene components.
		for(const auto& manifestPath : {originalManifest, ContainedFile(project, "Build/Assets/.terra-animation-manifests/da35202f311bd5ea56e23beb0aafbd9a.json")}) {
			const auto manifest = ReadJson(manifestPath); std::set<std::string> kinds;
			for(const auto& row : manifest.at("subAssets")) {
				const auto kind = row.at("kind").get<std::string>();
				if(!types.contains(kind)) { Require(kind == "MorphTarget" || kind == "Prefab", "Unexpected package asset kind"); continue; }
				Require(kinds.insert(kind).second, "Duplicate cooked fixture kind");
				if(!ids.insert(row.at("guid").get<std::string>()).second) continue;
				const auto id = TerraEngine::ParseUUID(row.at("guid").get<std::string>());
				Require(id.has_value(), "Invalid cooked resource UUID");
				const auto file = ContainedFile(project / "Build/Assets", row.at("compiledPath").get<std::string>());
				ResourceManager::Instance().RegisterAssetPath(*id, file, types.at(kind));
				registered.push_back({{"kind", kind}, {"id", row.at("guid")}, {"payload", Pin(file)}});
			}
			Require(kinds.size() == 4, "Missing cooked fixture resources");
		}
		return registered;
	}
	Json ValidateQualifiedGeometry(const std::filesystem::path& project)
	{
		const auto path = ContainedFile(project, "Build/Assets/Models/ImportedHero_Generated/ImportedHeroBody.temf");
		std::ifstream input(path, std::ios::binary);
		const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
		TerraEngine::Resource::TemfProductionValidationLimits limits;
		limits.availableCapabilities = kVirtualGeometryCodecCapabilityMeshoptimizer;
		const auto result = TerraEngine::Resource::ValidateTemfProductionFile(bytes, limits);
		Require(result.IsComplete(), "Qualified geometry failed matching-runtime production validation: " + result.diagnostic);
		return {{"complete", result.IsComplete()}, {"code", static_cast<unsigned>(result.code)}, {"diagnostic", result.diagnostic},
			{"headerBytes", result.directory.header.headerBytes}, {"formatContractOffset", 168}, {"formatContractId", "TEMFPRD1"}, {"payload", Pin(path)}};
	}
	void EnterPausedPlay()
	{
		// This uses common native lifecycle APIs, not editor restore snapshots.
		// Frozen fixtures have no project physics document, so normal editor entry
		// resolves the same defaults. Pause before any host frame or explicit step.
		auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
		Require(physics.QueueSettingsUpdate(TerraEngine::Physics::PhysicsSettings::Defaults()) && physics.StartWorld(), "Paused fixture physics startup failed");
		auto& animation = TerraEngine::Animation::AnimationSystem::Instance();
		(void)animation.NotifyLifecycleInvalidation(TerraEngine::Animation::AnimationLifecycleBoundary::PlayMode);
		animation.SetAuthoredComponentPlaybackEnabled(true); animation.SetAuthoredComponentPlaybackAdvancing(true);
		auto& audio = TerraEngine::Audio::AudioSystem::Instance(); audio.SetRuntimePlaybackEnabled(true);
		ScriptingSystem::Instance().SetRunning(true);
		Require(physics.SetPaused(true), "Physics pause rejected");
		ScriptingSystem::Instance().SetPaused(true); audio.SetRuntimePlaybackPaused(true);
	}
	void RequirePausedPlay()
	{
		const auto time = ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
		Require(time.playSessionActive && time.playSessionPaused && !time.advancesSimulation &&
			TerraEngine::Physics::PhysicsSystem::Instance().IsPaused(), "Frame did not preserve paused play without explicit step");
	}
	void Write(const std::filesystem::path& path, std::span<const std::uint8_t> bytes)
	{
		Require(!std::filesystem::exists(path), "Output must be fresh");
		std::ofstream output(path, std::ios::binary);
		Require(output.is_open(), "Cannot create output");
		output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		output.close(); Require(output.good(), "Cannot persist output");
	}
	void WriteJson(const std::filesystem::path& path, const Json& value)
	{
		const auto text = value.dump(2);
		Write(path, {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
	}
	Json Position(const TerraEngine::Scene::WorldPosition& position)
	{
		return {{"cell", {position.cell.x, position.cell.y, position.cell.z}},
			{"offsetMetres", {position.offsetMetres.x, position.offsetMetres.y, position.offsetMetres.z}}};
	}
	Json Pose(const Transform& transform)
	{
		const auto& pose = transform.worldPose;
		return {{"position", Position(pose.position)}, {"rotation", {pose.rotation.w, pose.rotation.x, pose.rotation.y, pose.rotation.z}},
			{"scale", {pose.scale.x, pose.scale.y, pose.scale.z}}};
	}
	Json Inputs(std::vector<TerraEngine::UUID>& lightIds)
	{
		Json cameras = Json::array(), lights = Json::array();
		auto& objects = ObjectManager::instance();
		// This is explicitly the protected authored-input projection, not a claim
		// that an old DLL exposes its private coroutine collection arrays.
		objects.forEachObjectWithComponents<Transform, CameraComponent>([&](Object object, Transform& transform, CameraComponent& camera) {
			cameras.push_back({{"ordinal", cameras.size()}, {"objectId", TerraEngine::ToHexString(objects.getUUID(object))},
				{"pose", Pose(transform)}, {"verticalFovDegrees", camera.verticalFovDegrees}, {"aspectRatio", camera.aspectRatio},
				{"nearPlane", camera.nearPlane}, {"farPlane", camera.farPlane}, {"up", {camera.up.x, camera.up.y, camera.up.z}}});
		});
		objects.forEachObjectWithComponents<Transform, LightComponent>([&](Object object, Transform& transform, LightComponent& light) {
			if(static_cast<std::uint32_t>(light.type) > static_cast<std::uint32_t>(LightType::Area)) return;
			const auto id = objects.getUUID(object); lightIds.push_back(id);
			lights.push_back({{"ordinal", lights.size()}, {"objectId", TerraEngine::ToHexString(id)}, {"pose", Pose(transform)},
				{"type", static_cast<std::uint32_t>(light.type)}, {"unit", static_cast<std::uint32_t>(light.unit)},
				{"appearance", static_cast<std::uint32_t>(light.appearance)}, {"color", {light.color.x, light.color.y, light.color.z}},
				{"temperature", light.temperature}, {"intensity", light.intensity}, {"range", light.range},
				{"innerConeAngle", light.innerConeAngle}, {"outerConeAngle", light.outerConeAngle}, {"width", light.width}, {"height", light.height},
				{"castsShadows", light.castsShadows}, {"shadowStrength", light.shadowStrength}, {"shadowBias", light.shadowBias},
				{"indirectMultiplier", light.indirectMultiplier}, {"authoredPriority", light.authoredPriority}, {"stabilityPreference", light.stabilityPreference}});
		});
		return {{"observationKind", "protectedAuthoredInputProjection"}, {"cameras", std::move(cameras)}, {"lights", std::move(lights)}};
	}
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
	Json ScratchEpisode(const RenderFrameScratchDiagnostics& value)
	{
		return {{"lifetimeId", value.lifetimeId}, {"episodeId", value.episodeId}, {"frameId", value.frameId},
			{"capacityBytes", value.capacityBytes}, {"usedBytes", value.usedBytes}, {"peakBytes", value.peakBytes},
			{"lifetimeHighWaterBytes", value.lifetimeHighWaterBytes}, {"reuseAllocationCount", value.reuseAllocationCount},
			{"fallbackAllocationCount", value.fallbackAllocationCount}, {"resetCount", value.resetCount}, {"outcomeCode", static_cast<unsigned>(value.outcome)}};
	}
	Json ScratchReclamation(const RenderFrameScratchReclamationDiagnostics& value)
	{
		return {{"lifetimeId", value.lifetimeId}, {"totalFallbackAllocationCount", value.totalFallbackAllocationCount},
			{"totalFallbackDeallocationCount", value.totalFallbackDeallocationCount},
			{"outstandingFallbackAllocationCount", value.outstandingFallbackAllocationCount},
			{"outstandingFallbackBytes", value.outstandingFallbackBytes}, {"resetCount", value.resetCount}};
	}
	auto CameraBits(const CameraComponent& value)
	{
		const auto bits = [](float item) { return std::bit_cast<std::uint32_t>(item); };
		return std::tuple{bits(value.verticalFovDegrees), bits(value.aspectRatio), bits(value.nearPlane), bits(value.farPlane),
			bits(value.up.x), bits(value.up.y), bits(value.up.z)};
	}
	auto LightBits(const LightComponent& value)
	{
		const auto bits = [](float item) { return std::bit_cast<std::uint32_t>(item); };
		return std::tuple{value.type, value.unit, value.appearance, bits(value.color.x), bits(value.color.y), bits(value.color.z),
			bits(value.temperature), bits(value.intensity), bits(value.range), bits(value.innerConeAngle), bits(value.outerConeAngle),
			bits(value.width), bits(value.height), value.castsShadows, bits(value.shadowStrength), bits(value.shadowBias),
			bits(value.indirectMultiplier), bits(value.authoredPriority), bits(value.stabilityPreference)};
	}
	RenderFrameScratchCapturedInputs IndependentExpectedInputs()
	{
		// This query runs before the hook is installed. It copies authored fields
		// directly; no renderer-produced captured array or digest supplies the oracle.
		RenderFrameScratchCapturedInputs result;
		auto& objects = ObjectManager::instance();
		objects.forEachObjectWithComponents<Transform, CameraComponent>([&](const Object& object, Transform&, CameraComponent& camera) {
			result.cameras.push_back({objects.getUUID(object), camera});
		});
		objects.forEachObjectWithComponents<Transform, LightComponent>([&](const Object& object, Transform&, LightComponent& light) {
			if(static_cast<unsigned>(light.type) <= static_cast<unsigned>(LightType::Area)) result.lights.push_back({objects.getUUID(object), light});
		});
		return result;
	}
	Json CapturedInputProjection(const RenderFrameScratchCapturedInputs& inputs, const char* kind)
	{
		Json cameras = Json::array(), lights = Json::array();
		for(const auto& row : inputs.cameras) {
			const auto& camera = row.camera;
			cameras.push_back({{"ordinal", cameras.size()}, {"objectId", TerraEngine::ToHexString(row.objectId)},
				{"verticalFovDegrees", camera.verticalFovDegrees}, {"aspectRatio", camera.aspectRatio}, {"nearPlane", camera.nearPlane},
				{"farPlane", camera.farPlane}, {"up", {camera.up.x, camera.up.y, camera.up.z}}});
		}
		for(const auto& row : inputs.lights) {
			const auto& light = row.light;
			lights.push_back({{"ordinal", lights.size()}, {"objectId", TerraEngine::ToHexString(row.objectId)},
				{"type", static_cast<unsigned>(light.type)}, {"unit", static_cast<unsigned>(light.unit)}, {"appearance", static_cast<unsigned>(light.appearance)},
				{"color", {light.color.x, light.color.y, light.color.z}}, {"temperature", light.temperature}, {"intensity", light.intensity},
				{"range", light.range}, {"innerConeAngle", light.innerConeAngle}, {"outerConeAngle", light.outerConeAngle}, {"width", light.width},
				{"height", light.height}, {"castsShadows", light.castsShadows}, {"shadowStrength", light.shadowStrength}, {"shadowBias", light.shadowBias},
				{"indirectMultiplier", light.indirectMultiplier}, {"authoredPriority", light.authoredPriority}, {"stabilityPreference", light.stabilityPreference}});
		}
		return {{"observationKind", kind}, {"cameras", std::move(cameras)}, {"lights", std::move(lights)}};
	}
	class CandidateScratchObserver final {
	public:
		explicit CandidateScratchObserver(RenderFrameScratchCapturedInputs expected) : expected_(std::move(expected)) {}
		Json ExpectedProjection() const { return CapturedInputProjection(expected_, "independentProtectedQuery"); }
		void Observe(const RenderFrameScratchValidationEvent& event)
		{
			if(event.checkpoint != RenderFrameScratchValidationCheckpoint::Captured) return;
			Require(event.capturedInputs != nullptr, "Candidate actual capture inputs unavailable");
			const auto& actual = *event.capturedInputs;
			Require(actual.cameras.size() == expected_.cameras.size() && actual.lights.size() == expected_.lights.size(), "Candidate actual capture cardinality differs");
			for(std::size_t index = 0; index < actual.cameras.size(); ++index)
				Require(actual.cameras[index].objectId == expected_.cameras[index].objectId && CameraBits(actual.cameras[index].camera) == CameraBits(expected_.cameras[index].camera), "Candidate actual camera identity/order/component bits differ");
			for(std::size_t index = 0; index < actual.lights.size(); ++index)
				Require(actual.lights[index].objectId == expected_.lights[index].objectId && LightBits(actual.lights[index].light) == LightBits(expected_.lights[index].light), "Candidate actual light identity/order/component bits differ");
			// Captured runs on the protected coordinator. Worker read events return
			// above without touching this map; no active resource is queried here.
			Require(frames_.size() < 256U, "Candidate native capture observation bound exceeded");
			Require(frames_.emplace(event.episode.frameId, Frame{event.episode, event.capturedInputs}).second, "Candidate repeated captured frame identity");
		}
		void AfterFrame(const RenderFrameScratchValidationState& state)
		{
			Require(!state.active, "Candidate frame returned with an active scratch lease");
			const auto found = frames_.find(state.latest.frameId);
			if(found == frames_.end()) return;
			Require(state.latest.lifetimeId == found->second.capture.lifetimeId && state.latest.episodeId == found->second.capture.episodeId
				&& state.latest.outcome == RenderFrameScratchOutcome::Completed && state.reclamation.lifetimeId == state.latest.lifetimeId,
				"Candidate terminal scratch does not join actual captured frame");
			found->second.terminal = state.latest; found->second.reclamation = state.reclamation; found->second.terminalObserved = true;
		}
		Json WriteSelected(const std::filesystem::path& root, std::uint64_t frameId) const
		{
			const auto found = frames_.find(frameId);
			Require(found != frames_.end() && found->second.terminalObserved, "Selected GPU frame lacks matching candidate native scratch episode");
			const auto& frame = found->second;
			const auto path = root / "candidate-captured-inputs.json";
			WriteJson(path, CapturedInputProjection(*frame.inputs, "actualNativeCapturedArrays"));
			return {{"capturedInputs", Pin(path)}, {"capturedEpisode", ScratchEpisode(frame.capture)}, {"terminalEpisode", ScratchEpisode(frame.terminal)},
				{"selectedFrameReclamation", ScratchReclamation(frame.reclamation)}, {"typedIdentityOrderComponentBitsEqual", true},
				{"observationPoint", "Actual Captured callback plus copied terminal/reclamation immediately after Engine.Update for the selected output frame"}};
		}
	private:
		struct Frame {
			RenderFrameScratchDiagnostics capture;
			std::shared_ptr<const RenderFrameScratchCapturedInputs> inputs;
			RenderFrameScratchDiagnostics terminal{};
			RenderFrameScratchReclamationDiagnostics reclamation{};
			bool terminalObserved = false;
		};
		RenderFrameScratchCapturedInputs expected_;
		std::map<std::uint64_t, Frame> frames_;
	};
#endif
	Json Reference(const RenderReferenceFrame& reference)
	{
		return {{"frameId", reference.frameId}, {"viewId", reference.viewId}, {"outputId", reference.outputId},
			{"canonicalOrigin", Position(reference.canonicalOrigin)}, {"originGeneration", reference.originGeneration},
			{"spatialGeneration", reference.spatialGeneration}, {"residencyGeneration", reference.residencyGeneration}};
	}
	Json View(RenderSystem& renderer)
	{
		const auto stats = renderer.GetLatestRenderStats(RenderViewType::Game);
		const auto reference = renderer.GetLatestRenderReferenceFrame(RenderViewType::Game);
		const auto snapshots = renderer.GetLatestRenderSnapshotDiagnostics();
		Json owners = Json::array();
		for(const auto& owner : snapshots.ownerSummaries) owners.push_back({{"outputId", owner.outputId}, {"kind", static_cast<unsigned>(owner.kind)},
			{"state", static_cast<unsigned>(owner.state)}, {"extent", {owner.targetSize.x, owner.targetSize.y}},
			{"lightingIsolated", owner.lightingIsolated}, {"cullingIsolated", owner.cullingIsolated}});
		return {{"valid", stats.valid}, {"outputKind", static_cast<unsigned>(stats.outputKind)}, {"outputId", stats.outputId},
			{"drawCalls", stats.drawCalls}, {"triangleCount", stats.triangleCount},
			{"dedicatedCaptureOutput", stats.dedicatedCaptureOutput}, {"publicationFrameId", stats.publicationFrameId},
			{"publicationGeneration", stats.publicationGeneration}, {"rendererSessionGeneration", stats.rendererSessionGeneration},
			{"renderSize", {stats.renderSize.x, stats.renderSize.y}}, {"referenceFrame", reference ? Reference(*reference) : Json(nullptr)},
			{"snapshotOwners", std::move(owners)}};
	}
}

int main(int argc, char** argv)
{
	std::filesystem::path outputRoot;
	Json receipt{{"schemaVersion", "runtime-memory-safety-parity-prototype-v3"}, {"variant", RMS_HOST_VARIANT},
		{"status", "Unverified"}, {"nativeAcceptance", false}, {"performanceAcceptance", false}};
	try {
		std::map<std::string, std::filesystem::path> arguments;
		for(int index = 1; index < argc; index += 2) {
			Require(index + 1 < argc, "Every argument needs a value");
			const std::string key = argv[index];
			Require(key == "--runtime-root" || key == "--project-root" || key == "--output-root" || key == "--archive-receipt" || key == "--materialization" || key == "--renderer-source-proof" || key == "--qualification" || key == "--support-manifest", "Unknown argument");
			Require(arguments.emplace(key, Path(argv[index + 1])).second, "Duplicate argument");
		}
		Require(arguments.size() == 8, "Require all eight path arguments including qualification and support manifest");
		for(const auto& [key, path] : arguments) Require(path.is_absolute(), "Paths must be absolute");
		const auto runtime = arguments.at("--runtime-root"), project = arguments.at("--project-root");
		outputRoot = arguments.at("--output-root");
		Require(std::filesystem::is_directory(runtime) && std::filesystem::is_directory(project), "Missing runtime/project root");
		Require(!std::filesystem::exists(outputRoot), "Output root must be fresh");
		std::filesystem::create_directories(outputRoot);
		receipt["archiveReceipt"] = Pin(arguments.at("--archive-receipt"));
		receipt["rendererSourceProof"] = Pin(arguments.at("--renderer-source-proof"));
		const auto sourceProof = ReadJson(arguments.at("--renderer-source-proof"));
		Require(sourceProof.at("backendSnapshotCapacity") == 8U, "Unexpected inherited backend snapshot capacity");
		VerifyFilePin(sourceProof.at("referenceCompatibilityCapacityHeader"));
		VerifyFilePin(sourceProof.at("referenceSource")); VerifyFilePin(sourceProof.at("candidateSource"));
		for(const auto& source : sourceProof.at("additionalSources")) { VerifyFilePin(source.at("referenceSource")); VerifyFilePin(source.at("candidateSource")); }
		Require(sourceProof.at("sourceRevision") == ReadJson(arguments.at("--archive-receipt")).at("sourceRevision"), "Renderer source proof does not join archived revision");
		receipt["supportManifest"] = Pin(arguments.at("--support-manifest"));
		const auto materialization = PreflightProject(project, arguments.at("--materialization"), arguments.at("--qualification"));
		receipt["materialization"] = Pin(arguments.at("--materialization"));
		receipt["qualification"] = Pin(arguments.at("--qualification"));
		receipt["fixtureScale"] = materialization.at("scale");
		receipt["assetClosurePolicy"] = "Explicit full actual post-import reference runtime closure in a fresh derivative; original materialization retained";
		receipt["timedPostImportAssetClosureParityVerified"] = false;
		receipt["runtimeClosure"] = PreflightRuntime(runtime);
		std::array<wchar_t, 32768> hostPath{};
		const auto hostLength = GetModuleFileNameW(nullptr, hostPath.data(), static_cast<DWORD>(hostPath.size()));
		Require(hostLength > 0 && hostLength < hostPath.size(), "Actual host path unavailable");
		receipt["hostExecutable"] = Pin(std::filesystem::path(hostPath.data()));
		receipt["imageNormalization"] = "Matching-ABI VulkanReadback::ConvertToRgba8, tightly packed RGBA8; no excluded regions";
		std::array<wchar_t, 32768> modulePath{};
		Require(GetModuleFileNameW(GetModuleHandleW(L"TerraEngine.dll"), modulePath.data(), static_cast<DWORD>(modulePath.size())) > 0,
			"Loaded engine DLL path unavailable");
		receipt["loadedEngine"] = Pin(std::filesystem::path(modulePath.data()));
		receipt["expectedEngine"] = Pin(runtime / "Editor/TerraEngine.dll");
		Require(receipt["loadedEngine"]["sha256"] == receipt["expectedEngine"]["sha256"], "Wrong runtime DLL resolved");
		receipt["qualifiedGeometryValidation"] = ValidateQualifiedGeometry(project);
		// Parent stages the matching local DLL closure beside each host. Never use
		// PATH to select between old and new runtimes or load candidate-only APIs.
		ResourceManager::Instance().SetEngineRoot(runtime / "Editor");
		ResourceManager::Instance().SetAssetRoot(project / "Build/Assets");
		Engine engine;
		struct Shutdown { Engine& value; bool completed = false; ~Shutdown() { if(!completed) value.Shutdown(); } } shutdown{engine};
		engine.Initialize();
		// Native Engine omits the editor AssetDatabase; register existing cooked
		// payloads without importing or changing the fresh fixture input closure.
		const auto qualified = ReadJson(arguments.at("--qualification"));
		receipt["registeredCookedAssets"] = RegisterCookedAssets(project, Path(qualified.at("originalPackageManifest").at("path").get<std::string>()));
		auto& scenes = SceneManager::Instance();
		const auto scene = scenes.LoadScene(project / "Assets/Scenes/Scene.tescene");
		Require(scenes.SetActiveScene(scene), "Fixture scene activation failed");
		auto& renderer = RenderSystem::Instance();
		// Use the same normal Game View offscreen path as the editor workload.
		// No camera, view-order or Scene camera override is installed.
		renderer.SetEditorModeEnabled(true);
		auto* vulkan = dynamic_cast<VulkanContext*>(renderer.GetContext());
		Require(vulkan != nullptr, "Matching Vulkan editor output context unavailable");
		vulkan->EnableEditorMode();
		Json lastPresentation;
		receipt["editorPresentationAttempts"] = {{"count", 0U}, {"retainedLimit", 32U}, {"attempts", Json::array()}};
		const auto updateNormalFrame = [&](bool requireQualifiedPresentation = false) {
			renderer.SetGameViewTargetSize({1280U, 720U});
			if(!engine.Update(0.0F)) return false;
			const auto presented = vulkan->PresentEditor({}, {});
			const auto progress = vulkan->GetEditorSubmissionProgress();
			lastPresentation = {{"submitted", presented.Submitted()}, {"nativeResult", presented.nativeResult}, {"submissionSerial", presented.submissionSerial},
				{"outcome", static_cast<unsigned>(presented.outcome)}, {"stage", static_cast<unsigned>(presented.stage)},
				{"qualificationRequired", requireQualifiedPresentation}, {"presentationGeneration", progress.presentationGeneration},
				{"lastSubmittedSerial", progress.lastSubmittedSerial}, {"completedSerial", progress.completedSerial}, {"deviceLost", progress.deviceLost}};
			if(presented.acquiredImage) lastPresentation["acquiredImage"] = {{"frameSlot", presented.acquiredImage->frameSlot},
				{"imageIndex", presented.acquiredImage->imageIndex}, {"presentationGeneration", presented.acquiredImage->presentationGeneration}};
			else lastPresentation["acquiredImage"] = nullptr;
			// Retain the actual result before checking it, including on startup or
			// failure. A successful queue present can request swapchain recreation;
			// that warm-up outcome is not a qualified selected-frame receipt.
			auto& history = receipt["editorPresentationAttempts"];
			history["count"] = history["count"].get<std::uint64_t>() + 1U;
			history["attempts"].push_back(lastPresentation);
			if(history["attempts"].size() > 32U) history["attempts"].erase(history["attempts"].begin());
			receipt["lastEditorPresentation"] = lastPresentation;
			if(requireQualifiedPresentation) {
				Require(presented.Submitted() && presented.submissionSerial != 0U && presented.nativeResult == VK_SUCCESS,
					"Qualified normal editor frame lacks successful presentation receipt");
			} else {
				// The normal editor deliberately retries acquire timeouts and startup
				// resize/recreation. Permit only these bounded warm-up states; actual
				// capture below still requires an unchanged successful queue receipt.
				const bool retryableOutcome = presented.outcome == TerraEngine::Renderer::Vulkan::EditorPresentOutcome::Skipped
					|| presented.outcome == TerraEngine::Renderer::Vulkan::EditorPresentOutcome::RecreationRequested;
				const bool retryableResult = presented.nativeResult == VK_SUCCESS || presented.nativeResult == VK_TIMEOUT
					|| presented.nativeResult == VK_NOT_READY || presented.nativeResult == VK_SUBOPTIMAL_KHR || presented.nativeResult == VK_ERROR_OUT_OF_DATE_KHR;
				Require(!progress.deviceLost && (presented.Submitted() || (retryableOutcome && retryableResult)),
					"Normal editor startup presentation failed outside its retryable lifecycle");
			}
			return true;
		};
		const auto initialSettings = renderer.GetRuntimeRenderSettingsSnapshot();
		Require(initialSettings.maxUploadedLights == 1024 && initialSettings.maxDynamicLights == 1024 && initialSettings.maxShadowLights == 4 &&
			!ResourceManager::Instance().IsRenderValidationEnabled() && !ResourceManager::Instance().IsRenderDebugEnabled(), "Effective frozen render settings differ");
		EnterPausedPlay();
		const auto deadline = Clock::now() + std::chrono::seconds(120);
		bool ready = false;
		for(std::size_t frame = 0; frame < 1200 && Clock::now() < deadline; ++frame) {
			Require(updateNormalFrame(), "Engine stopped before readiness");
			RequirePausedPlay();
			const auto stats = renderer.GetLatestRenderStats(RenderViewType::Game);
			// A completed extent alone can describe a clear-only frame. Require real
			// geometry counters too; unsupported counters remain Unverified, never a
			// reason to silently accept an empty reference image.
			if(stats.valid && stats.renderSize.x && stats.renderSize.y && stats.drawCalls && stats.triangleCount && frame >= 8
				&& lastPresentation.at("submitted").get<bool>() && lastPresentation.at("nativeResult") == VK_SUCCESS) { ready = true; break; }
			std::this_thread::yield();
		}
		Require(ready, "No bounded normal-renderer readiness");
		std::vector<TerraEngine::UUID> lightIds;
		const auto inputs = Inputs(lightIds);
		Require(inputs["cameras"].size() == materialization.at("expectedCameraCount") && inputs["lights"].size() == materialization.at("expectedLightCount"),
			"Activated authored fixture cohort differs");
		WriteJson(outputRoot / "authored-inputs.json", inputs);
		receipt["authoredInputs"] = Pin(outputRoot / "authored-inputs.json");
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
		// Query and persist the independent oracle before enabling the native hook.
		// It observes the same isolated authored scene, not a pressure-only fixture.
		auto candidateScratch = std::make_shared<CandidateScratchObserver>(IndependentExpectedInputs());
		WriteJson(outputRoot / "candidate-expected-inputs.json", candidateScratch->ExpectedProjection());
		const auto expectedCandidateInputs = Pin(outputRoot / "candidate-expected-inputs.json");
		auto candidateCallbacks = std::make_shared<RenderFrameScratchValidationCallbacks>();
		candidateCallbacks->onEvent = [candidateScratch](const RenderFrameScratchValidationEvent& event) { candidateScratch->Observe(event); };
		renderer.SetFrameScratchValidationCallbacks(candidateCallbacks);
#endif
		receipt["viewBefore"] = View(renderer);
		// Startup defaults have generation zero. Native readback correctly rejects
		// that identity, so publish the same values through the normal protected
		// options queue. This changes provenance only, not renderer behavior.
		const auto priorOptions = renderer.GetRendererEfficiencyOptionsSnapshot();
		const auto optionsAdmission = renderer.QueueRendererEfficiencyOptions(priorOptions.options);
		Require(optionsAdmission.accepted && optionsAdmission.requestedGeneration != 0U,
			"Same-value options publication rejected: " + optionsAdmission.error);
		bool optionsPublished = false;
		for(std::size_t frame = 0; frame < 64 && Clock::now() < deadline; ++frame) {
			Require(updateNormalFrame(), "Engine stopped before options provenance publication");
			RequirePausedPlay();
			const auto observed = renderer.GetRendererEfficiencyOptionsSnapshot();
			Require(observed.options == priorOptions.options && observed.rendererSessionGeneration == priorOptions.rendererSessionGeneration,
				"Same-value options publication changed effective values or renderer lifetime");
			if(observed.settingsGeneration == optionsAdmission.requestedGeneration && observed.settingsGeneration != 0U) { optionsPublished = true; break; }
		}
		Require(optionsPublished, "No bounded protected publication of same-value options provenance");
		const auto options = renderer.GetRendererEfficiencyOptionsSnapshot();
		receipt["optionsProvenancePublication"] = {{"requestedGeneration", optionsAdmission.requestedGeneration},
			{"observedGeneration", options.settingsGeneration}, {"optionsSessionGeneration", options.optionsSessionGeneration},
			{"rendererSessionGeneration", options.rendererSessionGeneration}, {"effectiveValuesUnchanged", options.options == priorOptions.options}};
		const auto settings = renderer.GetRuntimeRenderSettingsSnapshot();
		RendererEfficiencyFixtureIntent intent;
		intent.correlation = {"runtime-memory-safety-parity", "normal-all-camera-output", 1, 0,
			options.settingsGeneration, options.rendererSessionGeneration};
		intent.lightingControls.emplace();
		intent.lightingControls->candidateOrder = lightIds;
		intent.lightingControls->maxUploadedLights = settings.maxUploadedLights;
		intent.lightingControls->maxDynamicLights = settings.maxDynamicLights;
		intent.lightingControls->maxShadowLights = settings.maxShadowLights;
		// No camera, view order, origin, frame-slot or snapshot-slot override.
		const auto admitted = renderer.QueueRendererEfficiencyFixtureIntent(intent);
		Require(admitted.accepted, "Lighting readback intent rejected: " + admitted.reason);
		receipt["fixtureControlAdmission"] = {{"operationId", intent.correlation.operationId}, {"controlGeneration", admitted.controlGeneration},
			{"settingsGeneration", options.settingsGeneration}, {"rendererSessionGeneration", options.rendererSessionGeneration}};
		std::uint64_t cursor = renderer.GetRendererEfficiencyEvidenceBatch(0).batchSequence;
		Json rows = Json::array();
		std::map<std::uint64_t, std::set<std::uint64_t>> completeCommands;
		std::map<std::uint64_t, std::set<std::pair<std::uint64_t, bool>>> seenPurposes;
		const auto expectedAdmittedCommands = inputs["cameras"].size();
		std::uint64_t selectedFrame = 0;
		std::map<std::uint64_t, std::map<std::pair<std::uint64_t, bool>, Json>> commandMetadata;
		std::map<std::uint64_t, Json> normalOutputs;
		// Preserve actual rejection reasons and physical identities on failure.
		// Retain only the latest 32 batches/64 rows each; these are observations,
		// never replacement acceptance evidence for a missing native artifact.
		receipt["collectorFrameProof"] = {{"observedBatchCount", 0U}, {"retainedBatchLimit", 32U},
			{"rowLimitPerBatch", 64U}, {"requiredPreparedCommandCount", expectedAdmittedCommands}, {"batches", Json::array()}};
		for(std::size_t frame = 0; frame < 1200 && Clock::now() < deadline && !selectedFrame; ++frame) {
			Require(updateNormalFrame(true), "Engine stopped before completed parity outputs");
			RequirePausedPlay();
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
			candidateScratch->AfterFrame(renderer.GetFrameScratchValidationState());
#endif
			// Capture the completed final shared Game View for the same frame before
			// advancing. Lighting staging receipts can arrive several frames later.
			// Sixteen pinned candidates bound memory/disk while preserving exact joins.
			if(normalOutputs.size() < 16U) {
				RenderCaptureRequest request; request.sourceKind = RenderCaptureSourceKind::GameView;
				request.viewType = RenderViewType::Game; request.sourceName = "runtime-memory-safety-normal-final";
				request.targetSize = {1280U, 720U}; request.freshness = "latestCompleted";
				request.dedicatedRenderTarget = false; request.allowBackendFallback = false;
				const auto captured = renderer.CaptureImage(request);
				Require(captured.ok && captured.backendReadback && !captured.deterministicFallback && !captured.pendingAsync
					&& captured.size == request.targetSize && captured.frameId != 0U && captured.mimeType == "image/png"
					&& captured.freshness == "latestCompleted" && !captured.overlaysIncluded, "Final normal Game View lacks completed native readback: " + captured.message);
				const auto snapshots = renderer.GetLatestRenderSnapshotDiagnostics();
				Json fallbackEvents = Json::array();
				for(const auto& event : snapshots.fallbackEvents) fallbackEvents.push_back({{"kind", static_cast<unsigned>(event.kind)},
					{"reason", static_cast<unsigned>(event.reason)}, {"result", static_cast<unsigned>(event.result)}, {"outputId", event.outputId}, {"required", event.required}});
				Json snapshotProof{{"valid", snapshots.valid}, {"maxSnapshotsPerFrame", snapshots.maxSnapshotsPerFrame},
					{"peakSnapshotCount", snapshots.peakSnapshotCount}, {"activeSnapshotCount", snapshots.activeSnapshotCount},
					{"completedSnapshotCount", snapshots.completedSnapshotCount}, {"fallbackEvents", std::move(fallbackEvents)}};
				const auto pngPath = outputRoot / ("normal-frame-" + std::to_string(captured.frameId) + ".png");
				Write(pngPath, DecodeCaptureBase64(captured.base64Data));
				Require(normalOutputs.emplace(captured.frameId, Json{{"policy", "EditorNormalGameViewAfterAllAuthoredCameraSubmissions"},
					{"frameId", captured.frameId}, {"ok", captured.ok}, {"backendReadback", captured.backendReadback},
					{"deterministicFallback", captured.deterministicFallback}, {"pendingAsync", captured.pendingAsync},
					{"freshness", captured.freshness}, {"dedicatedRenderTarget", false}, {"allowBackendFallback", false},
					{"overlaysIncluded", captured.overlaysIncluded}, {"size", {captured.size.x, captured.size.y}}, {"mimeType", captured.mimeType},
					{"png", Pin(pngPath)}, {"presentation", lastPresentation}, {"snapshotDiagnostics", std::move(snapshotProof)}}).second, "Repeated normal Game View frame capture");
			}
			const auto batch = renderer.GetRendererEfficiencyEvidenceBatch(cursor);
			if(batch.batchSequence <= cursor) continue;
			cursor = batch.batchSequence;
			auto& collectorProof = receipt["collectorFrameProof"];
			collectorProof["observedBatchCount"] = collectorProof["observedBatchCount"].get<std::uint64_t>() + 1U;
			Json observedRows = Json::array();
			for(const auto& row : batch.rows) {
				if(observedRows.size() >= 64U) break;
				observedRows.push_back({{"kind", static_cast<unsigned>(row.kind)}, {"status", static_cast<unsigned>(row.status)},
					{"reason", row.unavailableReason}, {"operationId", row.fixtureCorrelation.operationId},
					{"settingsGeneration", row.effectiveOptions.settingsGeneration}, {"optionsSessionGeneration", row.effectiveOptions.optionsSessionGeneration},
					{"controlGeneration", row.fixtureControlGeneration}, {"frameSerial", row.frameSerial}, {"commandSerial", row.commandSerial},
					{"submitSerial", row.submitSerial}, {"completionSerial", row.completionSerial}, {"viewType", static_cast<unsigned>(row.viewType)},
					{"viewId", row.viewId}, {"analysisPurpose", row.analysisPurpose}, {"hasArtifacts", static_cast<bool>(row.artifacts)}});
			}
			collectorProof["batches"].push_back({{"batchSequence", batch.batchSequence}, {"actualRowCount", batch.rows.size()}, {"rows", std::move(observedRows)}});
			if(collectorProof["batches"].size() > 32U) collectorProof["batches"].erase(collectorProof["batches"].begin());
			for(const auto& row : batch.rows) {
				if(row.kind != RendererEfficiencyEvidenceKind::Lighting || row.fixtureCorrelation.operationId != intent.correlation.operationId) continue;
				Require(row.preparationSettled && row.commandSerial && row.frameSerial && row.fixtureControlGeneration == admitted.controlGeneration,
					"Native command metadata lacks prepared control/frame identity");
				const auto metadataKey = std::make_pair(row.commandSerial, row.analysisPurpose);
				Json metadata{{"frameSerial", row.frameSerial}, {"commandSerial", row.commandSerial}, {"analysisPurpose", row.analysisPurpose},
					{"status", static_cast<unsigned>(row.status)}, {"reason", row.unavailableReason}, {"submitSerial", row.submitSerial},
					{"completionSerial", row.completionSerial}, {"preparationSettled", row.preparationSettled}, {"viewType", static_cast<unsigned>(row.viewType)},
					{"viewId", row.viewId}, {"fixtureControlGeneration", row.fixtureControlGeneration}, {"settingsGeneration", row.effectiveOptions.settingsGeneration},
					{"optionsSessionGeneration", row.effectiveOptions.optionsSessionGeneration}, {"hasArtifacts", static_cast<bool>(row.artifacts)}};
				Require(commandMetadata[row.frameSerial].emplace(metadataKey, std::move(metadata)).second, "Repeated native command metadata purpose");
				if(row.status != RendererEfficiencyEvidenceStatus::Valid || !row.artifacts) continue;
				Require(row.preparationSettled && row.submitSerial && row.completionSerial && row.commandSerial
					&& row.fixtureControlGeneration == admitted.controlGeneration, "Incomplete native submission proof");
				const auto& artifacts = *row.artifacts;
				Require(artifacts.fullSourceCandidateCount && *artifacts.fullSourceCandidateCount == lightIds.size(), "Full source light membership differs");
				Require(artifacts.lightingPayload.size() == 176ULL * 1024 && artifacts.frameColor && !artifacts.frameColor->bytes.empty(),
					"Full lighting or same-command color bytes absent");
				const auto purpose = row.analysisPurpose ? 1U : 0U;
				const auto& upload = row.lightingUploads[purpose];
				Require(upload.available && upload.succeeded && upload.payloadLightIds.size() == upload.activeRecordCount,
					"Actual selected light IDs missing");
				Require(seenPurposes[row.frameSerial].emplace(row.commandSerial, row.analysisPurpose).second, "Repeated actual command/purpose");
				const auto prefix = "row-" + std::to_string(rows.size());
				const auto payload = outputRoot / (prefix + ".lighting.bin"), color = outputRoot / (prefix + ".color.bin");
				const auto rgbaPath = outputRoot / (prefix + ".rgba8.bin");
				// Use this executable's matching runtime conversion, including its HDR
				// gamma/rounding behavior. The comparator never approximates native
				// normalization and retains source format/raw hashes for every image.
				const auto rgba = TerraEngine::Renderer::VulkanReadback::ConvertToRgba8(artifacts.frameColor->bytes,
					artifacts.frameColor->width, artifacts.frameColor->height, static_cast<VkFormat>(artifacts.frameColor->sourceFormat));
				Write(payload, artifacts.lightingPayload); Write(color, artifacts.frameColor->bytes);
				Write(rgbaPath, rgba);
				Json ids = Json::array(); for(const auto& id : upload.payloadLightIds) ids.push_back(TerraEngine::ToHexString(id));
				rows.push_back({{"observedOrdinal", rows.size()}, {"batchSequence", batch.batchSequence}, {"frameSerial", row.frameSerial},
					{"commandSerial", row.commandSerial}, {"submitSerial", row.submitSerial}, {"completionSerial", row.completionSerial},
					{"preparationSettled", row.preparationSettled}, {"fixtureControlGeneration", row.fixtureControlGeneration},
					{"fixtureOperationId", row.fixtureCorrelation.operationId},
					{"viewType", static_cast<unsigned>(row.viewType)}, {"viewId", row.viewId}, {"analysisPurpose", row.analysisPurpose},
					{"payloadLightIds", std::move(ids)}, {"activeRecordCount", upload.activeRecordCount}, {"payload", Pin(payload)},
					{"upload", {{"available", upload.available}, {"succeeded", upload.succeeded}, {"analysisPurpose", upload.analysisPurpose},
						{"allocationId", upload.allocationId}, {"allocationGeneration", upload.allocationGeneration},
						{"allocationSizeBytes", upload.allocationSizeBytes}, {"regionOffsetBytes", upload.regionOffsetBytes}, {"regionSizeBytes", upload.regionSizeBytes}}},
					{"color", Pin(color)}, {"normalizedRgba8", Pin(rgbaPath)}, {"colorExtent", {artifacts.frameColor->width, artifacts.frameColor->height}},
					{"colorSourceFormat", artifacts.frameColor->sourceFormat}, {"fullSourceCandidateCount", *artifacts.fullSourceCandidateCount}});
				if(seenPurposes[row.frameSerial].contains({row.commandSerial, false}) && seenPurposes[row.frameSerial].contains({row.commandSerial, true}))
					completeCommands[row.frameSerial].insert(row.commandSerial);

			}
			for(const auto& [nativeFrame, metadata] : commandMetadata) {
				if(metadata.size() == 2U * expectedAdmittedCommands && !completeCommands[nativeFrame].empty() && completeCommands[nativeFrame].size() <= expectedAdmittedCommands && normalOutputs.contains(nativeFrame)) {
					selectedFrame = nativeFrame; break;
				}
			}
		}
		// Retain the accepted artifacts and last actual view even on an incomplete
		// cohort, so a failed lane explains missing purposes/commands precisely.
		receipt["actualRows"] = rows;
		Json attemptedMetadata = Json::array();
		for(const auto& [nativeFrame, metadata] : commandMetadata) {
			Json copied = Json::array(); for(const auto& [identity, row] : metadata) copied.push_back(row);
			attemptedMetadata.push_back({{"frameSerial", nativeFrame}, {"rows", std::move(copied)}});
		}
		receipt["collectorFrameProof"]["observedCommandFrames"] = std::move(attemptedMetadata);
		receipt["collectorFrameProof"]["lastView"] = View(renderer);
		receipt["collectorFrameProof"]["acceptedArtifactRowCount"] = rows.size();
		Require(selectedFrame != 0, "No complete all-camera normal output frame before deadline");
		receipt["selectedFrameSerial"] = selectedFrame;
		receipt["finalNormalOutput"] = normalOutputs.at(selectedFrame);
		Json orderedMetadata = Json::array(); std::uint64_t previousCommand = 0U, commandOrdinal = 0U;
		for(const auto& [identity, metadata] : commandMetadata.at(selectedFrame)) {
			if(previousCommand && previousCommand != identity.first) ++commandOrdinal;
			previousCommand = identity.first; auto copied = metadata; copied["commandOrdinal"] = commandOrdinal; orderedMetadata.push_back(std::move(copied));
		}
		Require(commandOrdinal + 1U == expectedAdmittedCommands, "Incomplete ordered authored-camera command metadata");
		receipt["normalCommandMetadata"] = std::move(orderedMetadata);
		receipt["normalOutputCoverage"] = {{"authoredCameraCount", inputs["cameras"].size()}, {"actualNormalPublishedViewCount", 1},
			{"actualCommandCount", commandOrdinal + 1U}, {"normalQueuedCameraCommands", inputs["cameras"].size()},
			{"backendSnapshotCapacity", 8}, {"expectedAdmittedCameraCommands", expectedAdmittedCommands}, {"actualRecordedCameraCommands", commandOrdinal + 1U},
			{"capacityLimit", "Editor required-output overflow submits and completes a prefix, then reuses the same eight physical snapshot slices"},
			{"capacityPolicy", "EditorRequiredOutputOverflowSerializesAndReusesEightPhysicalSlices"},
			{"lightingArtifactCommandCount", completeCommands.at(selectedFrame).size()}, {"lightingPurposeArtifactCount", 2U * completeCommands.at(selectedFrame).size()},
			{"perCameraImageCoverage", false}, {"privateReferenceArraysObserved", false},
			{"queuePolicy", "AllAuthoredCamerasInCapturedOrderSharedGameOutput2"},
			{"readbackLimit", "Lighting payload readback deduplicates frame/view/purpose; final Game View covers the completed normal shared target"}};
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
		receipt["candidateScratch"] = candidateScratch->WriteSelected(outputRoot, selectedFrame);
		receipt["candidateScratch"]["expectedInputs"] = expectedCandidateInputs;
		renderer.SetFrameScratchValidationCallbacks(nullptr);
#endif
		receipt["actualRows"] = std::move(rows);
		receipt["viewAfter"] = View(renderer);
		receipt["pausedPlayWithoutExplicitStep"] = true;
		// Recheck all immutable inputs before closure so incidental runtime writes
		// cannot be mistaken for the fresh authored fixture we admitted above.
		(void)PreflightProject(project, arguments.at("--materialization"), arguments.at("--qualification"));
		renderer.ClearRendererEfficiencyFixtureIntent(intent.correlation.operationId);
		engine.Shutdown(); shutdown.completed = true;
#if defined(RMS_CANDIDATE_SCRATCH) && RMS_CANDIDATE_SCRATCH
		const auto retiredScratch = renderer.GetFrameScratchValidationState();
		Require(!retiredScratch.active && retiredScratch.reclamation.lifetimeId == receipt["candidateScratch"]["terminalEpisode"]["lifetimeId"]
			&& retiredScratch.reclamation.outstandingFallbackAllocationCount == 0U && retiredScratch.reclamation.outstandingFallbackBytes == 0U
			&& retiredScratch.reclamation.totalFallbackAllocationCount == retiredScratch.reclamation.totalFallbackDeallocationCount,
			"Candidate actual native shutdown reclamation is incomplete");
		receipt["candidateScratch"]["retirementReclamation"] = ScratchReclamation(retiredScratch.reclamation);
#endif
		receipt["normalShutdownCompleted"] = true;
		receipt["status"] = "Captured";
		receipt["scope"] = "Controlled actual GPU payload, ID order, native color and view evidence; candidate collection arrays require separate regression";
		WriteJson(outputRoot / "parity-receipt.json", receipt);
		return 0;
	} catch(const std::exception& error) {
		receipt["reason"] = error.what();
		if(!outputRoot.empty() && std::filesystem::is_directory(outputRoot) && !std::filesystem::exists(outputRoot / "parity-receipt.json"))
			try { WriteJson(outputRoot / "parity-receipt.json", receipt); } catch(...) {}
		std::cerr << error.what() << '\n';
		return 2;
	}
}
