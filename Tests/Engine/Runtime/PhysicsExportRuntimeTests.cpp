// PhysicsExportRuntimeTests.cpp - US7 exported runtime-content contracts.

#include "TestHarness.h"

#include "Physics/Assets/CookedColliderAsset.h"
#include "Physics/Assets/PhysicsMaterial.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "TEPAK/PackWriter.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <string_view>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class PhysicsExportWorkspace final {
		public:
			PhysicsExportWorkspace() : root_(std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "PhysicsExportRuntime") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(content_);
			}
			~PhysicsExportWorkspace() { ContentMount::Instance().Unmount(); std::error_code ignored; std::filesystem::remove_all(root_, ignored); }
			void Write(std::string_view path, std::string_view text) const {
				const auto fullPath = content_ / std::filesystem::path(path);
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream output(fullPath, std::ios::binary); output << text;
			}
			void Write(std::string_view path, const std::vector<std::byte>& bytes) const {
				const auto fullPath = content_ / std::filesystem::path(path);
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream output(fullPath, std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			}
			std::filesystem::path Pack() const {
				const auto pack = root_ / "Physics.tepak";
				tepak::PackagingRequest request; request.inputRoot = content_; request.outputPackPath = pack; request.workerCount = 1U;
				request.enableCompression = false; request.enableDictionaryTraining = false; request.overwriteExisting = true;
				(void)tepak::PackWriter::Create(request); return pack;
			}
		private:
			std::filesystem::path root_;
			std::filesystem::path content_ = root_ / "Content";
		};

		nlohmann::json SettingsDocument() {
			TerraEngine::Physics::PhysicsSettings settings = TerraEngine::Physics::PhysicsSettings::Defaults();
			// Exercise the runtime parser with the same intentionally non-default
			// values that the installed editor/export parity fixture records.
			settings.gravity.y = -15.5F;
			settings.fixedTimeStep = 0.02F;
			settings.debugVisualizationEnabled = true;
			settings.retainedDebugTicks = 3U;
			settings.maxDebugPrimitives = 256U;
			settings.layers[1U].name = "ParityDynamic";
			settings.layers[1U].broadPhaseGroup = 7U;
			nlohmann::json layers = nlohmann::json::array();
			for(const auto& layer : settings.layers) layers.push_back({{"id", layer.id}, {"name", layer.name}, {"collisionMask", layer.collisionMask}, {"broadPhaseGroup", layer.broadPhaseGroup}, {"enabled", layer.enabled}});
			return {{"schemaVersion", settings.schemaVersion}, {"gravity", {settings.gravity.x, settings.gravity.y, settings.gravity.z}}, {"fixedTimeStep", settings.fixedTimeStep},
				{"debugVisualizationEnabled", settings.debugVisualizationEnabled}, {"retainedDebugTicks", settings.retainedDebugTicks},
				{"maxDebugPrimitives", settings.maxDebugPrimitives}, {"layers", layers}};
		}
		std::string Hash(std::string_view text) {
			std::uint64_t value = 14695981039346656037ULL;
			for(const char byte : text) { value ^= static_cast<unsigned char>(byte); value *= 1099511628211ULL; }
			char output[17]{}; std::snprintf(output, sizeof(output), "%016llx", static_cast<unsigned long long>(value)); return output;
		}
		std::string Hash(const std::vector<std::byte>& bytes) {
			std::string text(bytes.size(), '\0');
			for(std::size_t index = 0U; index < bytes.size(); ++index) text[index] = static_cast<char>(std::to_integer<unsigned char>(bytes[index]));
			return Hash(text);
		}

		Runtime::ExportStartupCoordinatorInput MakeHeadlessStartupInput(const Runtime::PackagedPhysicsRuntime& packagedPhysics) {
			Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = true;
			input.projectName = "Physics Export Runtime";
			input.startupScene = "Scenes/Startup.tescene";
			input.renderProfile = "headless";
			input.packagedPhysics = packagedPhysics;
			// A headless export deliberately has no Renderer gate. Physics, scene,
			// and managed scripting must still become ready before entering frames.
			input.requiredSystems = {
				{"Scene", true, true, "Headless startup scene loaded"},
				{"Scripting", true, true, "Managed physics binding loaded"}
			};
			return input;
		}
	}

	void RegisterPhysicsExportRuntimeTests(TestRegistry& registry) {
		registry.Add("Engine.PhysicsExportRuntime.LoadsMountedPhysicsSettingsAndManagedBindings", [](TestContext& context) {
			PhysicsExportWorkspace workspace;
			const std::string settings = SettingsDocument().dump();
			const std::string layers = SettingsDocument().at("layers").dump();
			TerraEngine::Physics::CookedColliderEnvelope envelope;
			envelope.assetId[0] = 1U; envelope.sourceAssetId[0] = 2U; envelope.sourceContentHash = 3U; envelope.recipeHash = 4U;
			envelope.vertexCount = 3U; envelope.triangleCount = 1U; envelope.partCount = 1U;
			envelope.bounds.minimum = {-1.0F, -1.0F, -1.0F}; envelope.bounds.maximum = {1.0F, 1.0F, 1.0F};
			TerraEngine::Physics::CookedColliderDiagnostics diagnostics;
			const auto cooked = TerraEngine::Physics::CookedColliderAsset::Create(envelope, {std::byte{1}, std::byte{2}}, diagnostics);
			context.Expect(cooked.has_value(), "The runtime loader fixture needs one valid project-owned cooked collider envelope.");
			const std::vector<std::byte> cookedBytes = cooked ? cooked->Serialize() : std::vector<std::byte>{};
			workspace.Write("ProjectSettings/PhysicsSettings.json", settings);
			workspace.Write("Physics/Fixture.tepcoll", cookedBytes);
			workspace.Write("Managed/Assembly-CSharp.dll", "NativeAOT physics binding");
			workspace.Write("game.json", nlohmann::json{{"physics", {{"schemaVersion", 1U}, {"provider", {{"identifier", "Jolt"}, {"version", "5.6.0"}, {"revision", "e77f175595e64cb44218cc9d9d56fc365ad0e36a"}}}, {"cookFormatVersion", 1U}, {"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1U}, {"contentHash", Hash(settings)}}}, {"layersHash", Hash(layers)}, {"materials", nlohmann::json::array()}, {"colliders", nlohmann::json::array({{{"guid", TerraEngine::ToHexString(envelope.assetId)}, {"packagePath", "Physics/Fixture.tepcoll"}, {"checksum", Hash(cookedBytes)}, {"size", cookedBytes.size()}, {"sourceProvenanceHash", "0000000000000003"}, {"recipeHash", "0000000000000004"}}})}, {"validation", {{"status", "validated"}}}}}}.dump());
			context.Expect(ContentMount::Instance().Mount(workspace.Pack()), "The generated physics fixture should mount through the normal runtime content path.");
			const auto loaded = Runtime::LoadPackagedPhysicsRuntime(ContentMount::Instance());
			context.Expect(loaded.loaded && loaded.providerRuntimeReady && loaded.managedBindingsLoaded, "Mounted export content must load the approved Jolt contract and managed physics binding.");
			context.ExpectEqual(32U, static_cast<unsigned int>(loaded.settings.layers.size()), "Physics Settings must preserve all 32 collision layers at runtime.");
			context.Expect(loaded.settings.gravity.y == -15.5F && loaded.settings.fixedTimeStep == 0.02F
				&& loaded.settings.layers[1U].broadPhaseGroup == 7U,
				"Mounted runtime settings must preserve the fixture's non-default gravity, timestep, and layer routing.");
			context.Expect(loaded.settings.debugVisualizationEnabled && loaded.settings.retainedDebugTicks == 3U
				&& loaded.settings.maxDebugPrimitives == 256U,
				"Mounted runtime settings must preserve bounded debug-visualization limits instead of applying editor-only defaults.");
			context.ExpectEqual(static_cast<std::size_t>(1U), loaded.colliders.size(), "A valid cooked collider must load from the content mount before scene simulation uses it.");
			const Runtime::ExportStartupCoordinatorResult startup = Runtime::CoordinateExportStartup(MakeHeadlessStartupInput(loaded));
			context.Expect(startup.CanEnterRuntime(), "A valid physics package must enter a headless runtime without requiring a renderer or source asset access.");
			context.Expect(std::none_of(startup.summary.requiredSystems.begin(), startup.summary.requiredSystems.end(), [](const Runtime::BootSystemResult& gate) {
				return gate.name == "Renderer";
			}), "Headless startup must not synthesize a graphics dependency while validating packaged physics.");
		});

		registry.Add("Engine.PhysicsExportRuntime.RejectsManagedPlaceholderAndPreventsRuntimeEntry", [](TestContext& context) {
			PhysicsExportWorkspace workspace;
			const std::string settings = SettingsDocument().dump();
			const std::string layers = SettingsDocument().at("layers").dump();
			workspace.Write("ProjectSettings/PhysicsSettings.json", settings);
			workspace.Write("Managed/Assembly-CSharp.dll", "placeholder");
			workspace.Write("game.json", nlohmann::json{{"physics", {{"schemaVersion", 1U}, {"provider", {{"identifier", "Jolt"}, {"version", "5.6.0"}, {"revision", "e77f175595e64cb44218cc9d9d56fc365ad0e36a"}}}, {"cookFormatVersion", 1U}, {"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1U}, {"contentHash", Hash(settings)}}}, {"layersHash", Hash(layers)}, {"materials", nlohmann::json::array()}, {"colliders", nlohmann::json::array()}, {"validation", {{"status", "validated"}}}}}}.dump());
			context.Expect(ContentMount::Instance().Mount(workspace.Pack()), "The corrupt managed fixture should still be readable as an archive.");
			Runtime::ExportStartupCoordinatorInput input; input.exportRuntime = true; input.contentMounted = true; input.metadataLoaded = true; input.packagedPhysics = Runtime::LoadPackagedPhysicsRuntime(ContentMount::Instance());
			context.Expect(!Runtime::CoordinateExportStartup(input).CanEnterRuntime(), "A placeholder managed binding must fail closed before a headless exported runtime enters its frame loop.");
		});

		registry.Add("Engine.PhysicsExportRuntime.RejectsCorruptCookedColliderBeforePhysicsStartup", [](TestContext& context) {
			PhysicsExportWorkspace workspace;
			const std::string settings = SettingsDocument().dump();
			const std::string layers = SettingsDocument().at("layers").dump();
			const std::vector<std::byte> corruptCollider = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
			workspace.Write("ProjectSettings/PhysicsSettings.json", settings);
			workspace.Write("Physics/Corrupt.tepcoll", corruptCollider);
			workspace.Write("Managed/Assembly-CSharp.dll", "NativeAOT physics binding");
			workspace.Write("game.json", nlohmann::json{{"physics", {{"schemaVersion", 1U}, {"provider", {{"identifier", "Jolt"}, {"version", "5.6.0"}, {"revision", "e77f175595e64cb44218cc9d9d56fc365ad0e36a"}}}, {"cookFormatVersion", 1U}, {"settings", {{"packagePath", "ProjectSettings/PhysicsSettings.json"}, {"schemaVersion", 1U}, {"contentHash", Hash(settings)}}}, {"layersHash", Hash(layers)}, {"materials", nlohmann::json::array()}, {"colliders", nlohmann::json::array({{{"guid", "22222222222222223333333344444444"}, {"packagePath", "Physics/Corrupt.tepcoll"}, {"checksum", Hash(corruptCollider)}, {"size", corruptCollider.size()}, {"sourceProvenanceHash", "0000000000000011"}, {"recipeHash", "0000000000000017"}}})}, {"validation", {{"status", "validated"}}}}}}.dump());
			context.Expect(ContentMount::Instance().Mount(workspace.Pack()), "The corrupt cooked-collider fixture should mount as an archive before the physics gate inspects it.");
			const Runtime::PackagedPhysicsRuntime packaged = Runtime::LoadPackagedPhysicsRuntime(ContentMount::Instance());
			context.Expect(packaged.declared && !packaged.loaded, "A corrupt cooked collider must be rejected before a physics provider or world can initialize.");
			Runtime::ExportStartupCoordinatorInput input; input.exportRuntime = true; input.contentMounted = true; input.metadataLoaded = true; input.packagedPhysics = packaged;
			context.Expect(!Runtime::CoordinateExportStartup(input).CanEnterRuntime(), "The export startup coordinator must fail closed when the mounted cooked collider is corrupt.");
		});

		registry.Add("Engine.PhysicsExportRuntime.ShutdownDrainsHeadlessPhysicsWorkBeforeWorkersExit", [](TestContext& context) {
			ThreadPool& pool = ThreadPool::instance();
			pool.shutdown();
			pool.initialize(1U);
			std::atomic<bool> physicsPreparationCompleted = false;
			auto preparation = pool.createTask([&physicsPreparationCompleted]() {
				// This represents worker-safe packaged-physics preparation. The
				// protected runtime boundary may only tear down after it finishes.
				physicsPreparationCompleted.store(true, std::memory_order_release);
				return true;
			});
			std::future<bool> completion = preparation->getFuture();
			pool.enqueue(preparation);
			pool.shutdown();

			context.Expect(completion.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
				"Headless shutdown must drain an admitted physics preparation job before joining workers.");
			context.Expect(completion.get() && physicsPreparationCompleted.load(std::memory_order_acquire),
				"A drained physics preparation job must finish its engine-owned publication work before shutdown returns.");
			context.ExpectEqual(0ULL, static_cast<unsigned long long>(pool.getThreadCount()),
				"After headless shutdown returns, no worker can retain an active physics job.");
		});
	}
}
