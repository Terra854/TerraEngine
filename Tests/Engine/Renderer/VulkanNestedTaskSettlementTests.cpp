// VulkanNestedTaskSettlementTests.cpp â€” Real installed Vulkan child ownership faults.
#include "Tests/Engine/TestHarness.h"
#include "Tests/Engine/Runtime/CorrectnessTestSupport.h"
#include "Core/MainEngine.h"
#include "Components/CameraComponent.h"
#include "Components/MeshRenderComponent.h"
#include "Components/Transform.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Renderer/Terrain/TerrainDirectMesh.h"
#include "Terrain/TerrainMaterial.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Utilities/PlatformUtils.h"
#include <GLFW/glfw3.h>
#include <glm/gtc/quaternion.hpp>
#include <atomic>
#include <cstdlib>
#include <fstream>

namespace TerraEngine::Tests {
	namespace {
		bool HasCompiledVulkanResources(const std::filesystem::path& root) {
			// Authored source directories also contain Shaders. Only an installed
			// compiled closure can initialize real GPU pipelines for these draws.
			for(const auto* relative : {"Shaders/Default.mesh.tspf", "Shaders/Default.frag.tspf",
				"Shaders/Default.task.tspf", "Shaders/UI/UI_Instanced.vert.spv", "Shaders/UI/UI_Instanced.frag.spv"}) {
				if(!std::filesystem::is_regular_file(root / relative) || std::filesystem::file_size(root / relative) == 0) return false;
			}
			return true;
		}
		std::filesystem::path VulkanEngineRoot() {
			// Installed acceptance supplies an exact root. Native test executables
			// can also discover the installed Editor while walking their ancestors.
			if(const char* root = std::getenv("TERRAENGINE_LIFECYCLE_INSTALL_ROOT")) {
				const auto editor = std::filesystem::absolute(root) / "Editor";
				if(!HasCompiledVulkanResources(editor)) throw std::runtime_error("Explicit installed compiled Vulkan resource closure is missing");
				return editor;
			}
			for(auto cursor = std::filesystem::path(PlatformUtils::GetExecutablePath()).parent_path(); !cursor.empty();) {
				if(HasCompiledVulkanResources(cursor / "Editor")) return cursor / "Editor";
				const auto parent = cursor.parent_path(); if(parent == cursor) break; cursor = parent;
			}
#ifdef TERRAENGINE_SOURCE_DIR
			const auto build = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "out" / "build" / "RelWithDebInfo" / "Build" / "Editor";
			if(HasCompiledVulkanResources(build)) return build;
#endif
			throw std::runtime_error("Real Vulkan fixture requires installed Editor shaders and runtime closure");
		}

		void PopulateVulkanDraws(const std::filesystem::path& assets, std::size_t count) {
			// Load an actual empty scene before protected owner-thread creation.
			const auto scenePath = assets / "Scene.tescene";
			std::ofstream(scenePath) << R"({"format":"TerraSceneUnboundedV1","objects":[]})";
			auto& scenes = SceneManager::Instance();
			if(!scenes.SetActiveScene(scenes.LoadScene(scenePath))) throw std::runtime_error("Vulkan fixture scene activation failed");
			auto& objects = ObjectManager::instance();
			auto camera = objects.createObject("Nested recording camera");
			objects.AddComponent<CameraComponent>(camera);
			// Canonical protected writes invalidate transform and spatial caches.
			auto pose = objects.GetCanonicalWorldPose(camera);
			if(!pose) throw std::runtime_error("Vulkan camera pose is missing");
			pose->position.offsetMetres = {0.5, 2.0, 2.0};
			pose->rotation = glm::angleAxis(glm::radians(-55.0f), glm::vec3(1, 0, 0));
			if(objects.SetCanonicalWorldPose(camera, *pose) != Scene::WorldCoordinateError::None) throw std::runtime_error("Vulkan camera pose rejected");
			std::vector<std::int32_t> heights(TerraEngine::Terrain::kTerrainAreaSampleCount, 0);
			Renderer::Terrain::TerrainDirectMeshInput input;
			input.assetId = GenerateUUID(); input.authoredHash = {1, 2}; input.authoredGeneration = 1;
			input.activeRect = {0, 0, 2, 2}; input.heightMillimetres = heights;
			auto mesh = Renderer::Terrain::BuildTerrainDirectMesh(input);
			if(!mesh.Succeeded()) throw std::runtime_error(mesh.diagnostic);
			auto& resources = ResourceManager::Instance();
			const auto meshId = resources.RegisterMesh("nested-vulkan-mesh", std::move(mesh.mesh));
			if(meshId == EMPTY_UUID) throw std::runtime_error("Vulkan generated mesh publication failed");
			for(std::size_t i = 0; i < count; ++i) {
				const auto materialId = resources.RegisterMaterial("nested-vulkan-material-" + std::to_string(i),
					TerraEngine::Terrain::CreateTerrainMaterial(resources, {}));
				if(materialId == EMPTY_UUID) throw std::runtime_error("Vulkan generated material publication failed");
				auto object = objects.createObject("Nested recording quad " + std::to_string(i));
				auto* render = objects.AddComponent<MeshRenderComponent>(object);
				render->meshId = meshId; render->materialId = materialId; render->taskShaderCullingEnabled = false;
			}
		}

		void RunVulkanFault(TestContext& context, Tasks::NestedTaskStage stage) {
			// Captured gates/counters outlive the Engine and its admitted callbacks.
			Correctness::DisposableFixturePath fixture;
			std::promise<void> frameCompletion;
			auto frameCompletionFuture = frameCompletion.get_future();
			Correctness::ControlledGate held, faultReached;
			std::atomic<bool> returned{false}, entered{false}, settled{false}, blocked{false}, driverObserved{false};
			std::atomic<std::size_t> checkpointCount{0}, lastCheckpointIndex{0};
			std::atomic<int> lastCheckpointStage{-1};
			const bool inlineCase = stage == Tasks::NestedTaskStage::Inline;
			const std::size_t heldIndex = stage == Tasks::NestedTaskStage::Result ? 1 : 0;
			// Join the async driver before any captured gates, atomics or futures die.
			std::future<void> driver;
			auto& pool = ThreadPool::instance();
			if(pool.getThreadCount() != 0) throw std::runtime_error("Vulkan lifetime fixture requires a quiescent frame pool");
			pool.initialize(inlineCase ? 1 : 3);
			struct PoolCleanup { ~PoolCleanup() { ThreadPool::instance().shutdown(); } } poolCleanup;
			auto& resources = ResourceManager::Instance();
			const auto priorEngine = resources.GetEngineRoot(), priorAssets = resources.GetAssetRoot();
			struct RootCleanup {
				std::filesystem::path engine, assets;
				~RootCleanup() { ResourceManager::Instance().SetEngineRoot(engine); ResourceManager::Instance().SetAssetRoot(assets); }
			} roots{priorEngine, priorAssets};
			resources.SetEngineRoot(VulkanEngineRoot()); resources.SetAssetRoot(fixture.Root());
			// The backend preserves GLFW hints. Initializing GLFW first makes the
			// hidden hint valid before the normal Engine creates its real surface.
			if(!glfwInit()) throw std::runtime_error("Real Vulkan fixture GLFW initialization failed");
			glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
			Engine engine;
			// Wake the test driver and admitted children before Engine unwinds.
			struct ReleaseBeforeEngineDrain {
				Correctness::ControlledGate& held;
				Correctness::ControlledGate& fault;
				~ReleaseBeforeEngineDrain() noexcept {
					try { held.Cancel(); } catch(...) {}
					try { fault.Cancel(); } catch(...) {}
				}
			} releaseBeforeDrain{held, faultReached};
			engine.Initialize();
			if(!resources.GetShaderCollection(resources.GetDefaultShaderCollectionID()))
				throw std::runtime_error("Installed default Vulkan shader collection failed normal engine initialization: " + resources.GetEngineRoot().string());
			PopulateVulkanDraws(fixture.Root(), 513);
			struct HookCleanup { ~HookCleanup() { Tasks::SetNestedTaskHookForTesting({}); } } hookCleanup;
			Tasks::SetNestedTaskHookForTesting([&, wait = held.MakeWaiter()](Tasks::NestedTaskCheckpoint point) {
				if(point.group != Tasks::NestedTaskGroup::VulkanSecondaryRecording) return;
				entered.store(true); ++checkpointCount;
				lastCheckpointStage.store(static_cast<int>(point.stage)); lastCheckpointIndex.store(point.childIndex);
				if(!inlineCase && point.stage == Tasks::NestedTaskStage::Child && point.childIndex == heldIndex) {
					const auto outcome = wait(); settled.store(true);
					if(outcome != Correctness::GateResult::Released) throw std::runtime_error("Vulkan fixture child did not receive release");
				}
				const auto failureIndex = stage == Tasks::NestedTaskStage::Result || inlineCase ? 0U : 1U;
				if(point.stage == stage && point.childIndex == failureIndex) {
					if(!inlineCase && !held.WaitForArrivals(1)) throw std::runtime_error("Vulkan sibling never entered production callback");
					faultReached.Release();
					throw std::runtime_error("Injected actual Vulkan recording failure");
				}
			});
			// This driver is outside the global pool, so sole-worker owner progress
			// and production child settlement cannot depend on test release capacity.
			driver = std::async(std::launch::async, [&] {
				const auto observation = faultReached.MakeWaiter()();
				driverObserved.store(observation == Correctness::GateResult::Released);
				// Observe the owner completion channel while the real child remains held.
				// A missing join would let Update publish completion before this release.
				blocked.store(frameCompletionFuture.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout && !returned.load());
				held.Release();
			});
			const bool accepted = engine.Update(1.0f / 60.0f); returned.store(true); frameCompletion.set_value();
			driver.get();
			context.Expect(entered.load() && driverObserved.load(), "Fault must enter actual RenderFrame stage=" + std::to_string(static_cast<int>(stage))
				+ "; observed checkpoints=" + std::to_string(checkpointCount.load())
				+ ", last stage=" + std::to_string(lastCheckpointStage.load()) + ", index=" + std::to_string(lastCheckpointIndex.load()));
			context.Expect(!accepted && engine.GetFrameFailure().has_value()
				&& engine.GetFrameFailure()->reason.find("Injected actual Vulkan recording failure") != std::string::npos,
				"The exact initiating recording failure must reject the frame through Engine.Update.");
			context.Expect(inlineCase || (blocked.load() && settled.load()), "Frame return must follow physical completion of the deliberately held production sibling.");
			engine.Shutdown();
		}
	}
	void VerifyCleanVulkanLifetime(TestContext& context) {
		// Failed engines are terminal. Recovery means a fresh normal GPU owner,
		// one actual accepted frame, and ordinary shutdown of that new lifetime.
		Correctness::DisposableFixturePath fixture;
		auto& resources = ResourceManager::Instance();
		const auto priorEngine = resources.GetEngineRoot(), priorAssets = resources.GetAssetRoot();
		struct Cleanup {
			std::filesystem::path engine, assets;
			~Cleanup() {
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().SetEngineRoot(engine);
				ResourceManager::Instance().SetAssetRoot(assets);
			}
		} cleanup{priorEngine, priorAssets};
		ThreadPool::instance().initialize(1);
		resources.SetEngineRoot(VulkanEngineRoot()); resources.SetAssetRoot(fixture.Root());
		if(!glfwInit()) throw std::runtime_error("Fresh Vulkan GLFW initialization failed");
		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		Engine engine;
		engine.Initialize(); PopulateVulkanDraws(fixture.Root(), 1);
		context.Expect(engine.Update(1.0f / 60.0f) && !engine.GetFrameFailure(),
			"A fresh real GPU engine lifetime must accept a clean frame after failed lifetime drain.");
		engine.Shutdown();
	}
	void RegisterVulkanNestedTaskSettlementTests(TestRegistry& registry) {
		registry.Add("Engine.NestedTaskSettlement.VulkanSecondaryRecording", [](TestContext& context) {
			for(const auto stage : {Tasks::NestedTaskStage::Creation, Tasks::NestedTaskStage::Submission,
				Tasks::NestedTaskStage::Child, Tasks::NestedTaskStage::Result, Tasks::NestedTaskStage::Inline}) RunVulkanFault(context, stage);
			VerifyCleanVulkanLifetime(context);
		});
	}
}
