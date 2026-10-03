// Production resource requests run in isolated watchdog processes because a
// blocked collection can also prevent ThreadPool teardown from returning.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"

#include "File Formats/TerraEngineShaderFile.h"
#include "Resource/ResourceManager.h"
#include "Resource/ShaderCollection.h"
#include "ThreadPool/ThreadPool.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		// Build a parser-valid vertex container from the committed format types.
		// These four bytecode bytes exercise resource loading only; the fixture
		// never asks a graphics device to create a pipeline from them.
		void WriteTinyVertexStage(const std::filesystem::path& path, ShaderStage stage = ShaderStage::Vertex)
		{
			const std::string source = "Engine/Shaders/ResourceFixture.vert";
			ShaderHeader header{};
			std::memcpy(header.magic, &kShaderMagicValue, sizeof(kShaderMagicValue));
			header.version = kShaderCurrentVersion;
			header.headerSize = static_cast<std::uint16_t>(sizeof(header));
			header.reflectionOffset = sizeof(header);
			header.compiledBinaryOffset = header.reflectionOffset + sizeof(ShaderReflection);
			header.stringTableOffset = header.compiledBinaryOffset + sizeof(CompiledStageBinary);
			header.stringTableSize = static_cast<std::uint32_t>(source.size() + 1U);
			header.compiledBinarySize = sizeof(std::uint32_t);
			ShaderReflection reflection{};
			reflection.source.stage = static_cast<std::uint32_t>(stage);
			reflection.source.entryPointOffset = kInvalidStringOffset;
			reflection.source.targetProfileOffset = kInvalidStringOffset;
			CompiledStageBinary binary{};
			binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
			binary.entryPointOffset = kInvalidStringOffset;
			binary.targetProfileOffset = kInvalidStringOffset;
			binary.bytecodeOffset = header.stringTableOffset + header.stringTableSize;
			binary.bytecodeSize = header.compiledBinarySize;
			const std::uint32_t bytecode = 0x07230203U;
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(&header), sizeof(header));
			file.write(reinterpret_cast<const char*>(&reflection), sizeof(reflection));
			file.write(reinterpret_cast<const char*>(&binary), sizeof(binary));
			file.write(source.c_str(), static_cast<std::streamsize>(source.size() + 1U));
			file.write(reinterpret_cast<const char*>(&bytecode), sizeof(bytecode));
			file.close();
			if (!file) throw std::runtime_error("Could not write the tiny resource shader fixture.");
		}

		// The pool is owned for the entire request lifetime. A production deadlock
		// intentionally requires the external watchdog to terminate this process;
		// local gate cancellation cannot rescue a worker waiting inside the loader.
		class ResourcePoolSession final {
		public:
			explicit ResourcePoolSession(std::size_t workerCount)
			{
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().BeginRequestSession();
				ResourceManager::Instance().ClearCaches();
				ThreadPool::instance().initialize(workerCount);
			}
			~ResourcePoolSession()
			{
				ResourceManager::Instance().SetRequestTestHooks({});
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().ClearCaches();
				// A stopped request owner must not leak closed admission into the
				// next fixture sharing this process-global resource manager.
				ResourceManager::Instance().BeginRequestSession();
			}
		};

		void ExerciseCollectionBeforeAdmittedStages(TestContext& context, std::size_t requestedWorkers)
		{
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(requestedWorkers);
			auto& pool = ThreadPool::instance();
			auto& resources = ResourceManager::Instance();
			const std::size_t workerCount = pool.getThreadCount();
			context.Expect(workerCount > 0U, "The dependency fixture requires at least one worker.");
			std::vector<std::filesystem::path> stages;
			for (std::size_t index = 0U; index < workerCount; ++index) {
				const auto path = fixture.Child("stage-" + std::to_string(index) + ".tspf");
				WriteTinyVertexStage(path);
				context.Expect(resources.LoadShader(path) != TerraEngine::EMPTY_UUID,
					"A fresh tiny stage must pass the production decoder before testing scheduling.");
				stages.push_back(path);
			}
			resources.ClearCaches();

			// Occupy every worker before admitting requests so FIFO dispatch order,
			// rather than timing or sleeps, establishes the problematic dependency.
			Correctness::ControlledGate dispatchGate;
			std::vector<std::future<Correctness::GateResult>> blockerFutures;
			for (std::size_t index = 0U; index < workerCount; ++index) {
				auto task = pool.createTask(dispatchGate.MakeWaiter(std::chrono::seconds(30)));
				blockerFutures.push_back(task->getFuture());
				pool.enqueue(task);
			}
			context.Expect(dispatchGate.WaitForArrivals(workerCount), "Every worker must reach the admission gate.");
			std::vector<std::shared_future<TerraEngine::UUID>> collections;
			std::vector<std::shared_future<TerraEngine::UUID>> shaders;
			for (const auto& path : stages) {
				collections.push_back(resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}}));
			}
			for (const auto& path : stages) shaders.push_back(resources.LoadShaderAsync(path));
			std::cout << "Resource dependency admission complete; releasing " << workerCount << " workers.\n" << std::flush;
			dispatchGate.Release();
			const auto deadline = std::chrono::steady_clock::now() + Correctness::kDefaultTimeout;
			for (auto& future : collections) {
				context.Expect(future.wait_until(deadline) == std::future_status::ready,
					"Collections must settle within five seconds without blocking workers on queued stages.");
				const auto id = future.get();
				context.Expect(id != TerraEngine::EMPTY_UUID, "A collection with a valid vertex stage must publish.");
				const auto* collection = resources.GetShaderCollection(id);
				context.Expect(collection != nullptr && collection->reflectionComplete,
					"Successful completion must correspond to a production cache publication.");
			}
			for (auto& future : shaders) {
				context.Expect(future.wait_until(deadline) == std::future_status::ready,
					"Every directly admitted stage must settle within the same deadline.");
				context.Expect(future.get() != TerraEngine::EMPTY_UUID, "Valid stage requests must publish.");
			}
			for (auto& future : blockerFutures) {
				context.Expect(future.wait_until(deadline) == std::future_status::ready,
					"All admission blockers must return after release.");
				context.Expect(future.get() == Correctness::GateResult::Released,
					"The admission gate must release rather than time out.");
			}
		}
		TerraEngine::UUID ReadyResource(TestContext& context, const std::shared_future<TerraEngine::UUID>& future)
		{
			context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Every production request must reach a terminal outcome.");
			return future.get();
		}

		void ExerciseFailureAndRetry(TestContext& context, bool throwDecode, bool throwEnqueue)
		{
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto stage = fixture.Child("retry.tspf");
			WriteTinyVertexStage(stage);
			const auto fail = [](const std::string&) { throw std::runtime_error("Controlled resource failure."); };
			ResourceManager::RequestTestHooks hooks;
			if (throwDecode) hooks.beforeDecode = fail;
			if (throwEnqueue) hooks.beforeEnqueue = fail;
			resources.SetRequestTestHooks(std::move(hooks));
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, stage}})) == TerraEngine::EMPTY_UUID,
				"Exceptional dependencies must settle parents with the compatibility failure UUID.");
			context.Expect(resources.GetRequestDiagnostics().pending == 0U,
				"Exceptional admission or decoding must not poison the in-flight request table.");
			resources.SetRequestTestHooks({});
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, stage}})) != TerraEngine::EMPTY_UUID,
				"A later request must retry successfully after the controlled exception.");
		}

	}

	// Run each capacity with an exact filter in its own watchdog subprocess.
	// Default capacity follows the pool's usual environment/hardware selection.
	void RegisterResourceAsyncDependencyTests(TestRegistry& registry)
	{
		registry.Add("Engine.ResourceAsyncLifecycle.CollectionBeforeStage.OneWorker", [](TestContext& context) {
			ExerciseCollectionBeforeAdmittedStages(context, 1U);
		});
		registry.Add("Engine.ResourceAsyncLifecycle.CollectionBeforeStage.TwoWorkers", [](TestContext& context) {
			ExerciseCollectionBeforeAdmittedStages(context, 2U);
		});
		registry.Add("Engine.ResourceAsyncLifecycle.CollectionBeforeStage.DefaultWorkers", [](TestContext& context) {
			ExerciseCollectionBeforeAdmittedStages(context, 0U);
		});
		registry.Add("Engine.ResourceAsyncLifecycle.SharedAndDuplicateStages", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(2U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("shared.tspf");
			WriteTinyVertexStage(path);
			const auto before = resources.GetRequestDiagnostics();
			Correctness::ControlledGate gate;
			resources.SetRequestTestHooks({[wait = gate.MakeWaiter()](const std::string&) { (void)wait(); }, {}, {}});
			const auto first = resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}});
			context.Expect(gate.WaitForArrivals(1U), "The shared stage must reach decoding.");
			const auto second = resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}, {ShaderStage::Vertex, path}});
			const auto direct = resources.LoadShaderAsync(path.parent_path() / "." / path.filename());
			resources.SetRequestTestHooks({});
			gate.Release();
			const auto stageId = ReadyResource(context, direct);
			context.Expect(stageId != TerraEngine::EMPTY_UUID, "Direct demand must share the admitted dependency.");
			for (const auto& future : {first, second}) {
				const auto id = ReadyResource(context, future);
				const auto* collection = resources.GetShaderCollection(id);
				context.Expect(collection != nullptr, "Both consumers must publish collections.");
				for (const auto& binding : collection->stages) context.Expect(binding.shaderId == stageId, "Duplicate/shared dependencies must preserve one stage identity.");
			}
			context.Expect(resources.GetRequestDiagnostics().published == before.published + 3U,
				"One shared stage and two collections must each publish once.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.MissingAndInvalidDependencies", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("missing.tspf");
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}})) == TerraEngine::EMPTY_UUID,
				"A missing dependency must settle its parent without worker waits.");
			{ std::ofstream file(path, std::ios::binary); file << "invalid"; }
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}})) == TerraEngine::EMPTY_UUID,
				"Malformed compiled bytes must settle the dependency and collection.");
			WriteTinyVertexStage(path);
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}})) != TerraEngine::EMPTY_UUID,
				"Corrected compiled content must load on the next request.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.DecodeExceptionAndRetry", [](TestContext& context) { ExerciseFailureAndRetry(context, true, false); });
		registry.Add("Engine.ResourceAsyncLifecycle.EnqueueExceptionAndRetry", [](TestContext& context) { ExerciseFailureAndRetry(context, false, true); });
		registry.Add("Engine.ResourceAsyncLifecycle.AdmissionSetupExceptionAndRetry", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("setup.tspf");
			WriteTinyVertexStage(path);
			ResourceManager::RequestTestHooks hooks;
			hooks.afterAdmission = [](const std::string&) { throw std::bad_alloc(); };
			resources.SetRequestTestHooks(std::move(hooks));
			context.Expect(ReadyResource(context, resources.LoadShaderAsync(path)) == TerraEngine::EMPTY_UUID,
				"A failed admitted producer setup must settle its future.");
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}})) == TerraEngine::EMPTY_UUID,
				"Collection assembly setup failure must settle its parent.");
			context.Expect(resources.GetRequestDiagnostics().pending == 0U, "Setup failures must leave no pending ownership.");
			resources.SetRequestTestHooks({});
			context.Expect(ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}})) != TerraEngine::EMPTY_UUID,
				"A fresh producer must retry both failed keys successfully.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.MissingExecutorSettlesWithoutBlockingDrain", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			ThreadPool::instance().shutdown();
			const auto path = fixture.Child("executor.tspf");
			WriteTinyVertexStage(path);
			context.Expect(ReadyResource(context, resources.LoadShaderAsync(path)) == TerraEngine::EMPTY_UUID,
				"Async requests require a live executor and must fail promptly without one.");
			resources.StopAdmissionsAndDrain();
			context.Expect(resources.GetRequestDiagnostics().pending == 0U, "Missing executor admission must not poison teardown.");
			resources.BeginRequestSession();
			context.Expect(resources.LoadShader(path) != TerraEngine::EMPTY_UUID, "Synchronous production loading remains valid without workers.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.DefaultGeometryUsesCapturedEngineRoot", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto oldRoot = resources.GetEngineRoot();
			struct RootRestore { ResourceManager& manager; std::filesystem::path root; ~RootRestore() { manager.SetEngineRoot(root); } } restore{resources, oldRoot};
			std::filesystem::create_directory(fixture.Child("Shaders"));
			WriteTinyVertexStage(fixture.Child("Shaders/Default.task.tspf"), ShaderStage::Task);
			WriteTinyVertexStage(fixture.Child("Shaders/Default.mesh.tspf"), ShaderStage::Mesh);
			const auto fragment = fixture.Child("surface.tspf");
			WriteTinyVertexStage(fragment, ShaderStage::Fragment);
			resources.SetEngineRoot(fixture.Root());
			const auto id = ReadyResource(context, resources.LoadShaderCollectionAsync({{ShaderStage::Fragment, fragment}}));
			const auto* collection = resources.GetShaderCollection(id);
			context.Expect(collection != nullptr && collection->stages.size() == 3U,
				"Surface-only requests must subscribe to both implicit geometry stages.");
			context.Expect(collection->stages[0U].stage == ShaderStage::Task && collection->stages[1U].stage == ShaderStage::Mesh,
				"Implicit geometry must retain task/mesh ordering.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.CanceledCollectionDoesNotPublishPrivateStage", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("private.tspf");
			WriteTinyVertexStage(path);
			Correctness::ControlledGate gate;
			resources.SetRequestTestHooks({[wait = gate.MakeWaiter()](const std::string&) { (void)wait(); }, {}, {}});
			const auto parent = resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}});
			context.Expect(gate.WaitForArrivals(1U), "Private stage decoding must be controlled before cancellation.");
			TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID, stageId = TerraEngine::EMPTY_UUID;
			for (const auto& request : resources.GetRequestDiagnostics().requests) {
				if (request.key.ends_with(';')) parentId = request.assetId;
				else stageId = request.assetId;
			}
			context.Expect(resources.CancelStreamableLoad(parentId, AssetType::ShaderCollection), "The collection request must accept cancellation.");
			context.Expect(ReadyResource(context, parent) == TerraEngine::EMPTY_UUID, "Cancellation must settle the parent before physical stage work returns.");
			resources.SetRequestTestHooks({});
			gate.Release();
			ThreadPool::instance().shutdown();
			context.Expect(resources.AcquireShaderSnapshot(stageId) == nullptr, "An obsolete-parent-only stage must not enter the production cache.");
			context.Expect(resources.GetRequestDiagnostics().pending == 0U, "Canceled collection dependency records must all terminalize.");
		});

		registry.Add("Engine.ResourceAsyncLifecycle.ConcurrentAdmissionPublishesOnce", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(2U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("concurrent.tspf");
			WriteTinyVertexStage(path);
			Correctness::ControlledGate gate;
			resources.SetRequestTestHooks({[wait = gate.MakeWaiter()](const std::string&) { (void)wait(); }, {}, {}});
			const auto before = resources.GetRequestDiagnostics();
			std::vector<std::future<std::shared_future<TerraEngine::UUID>>> admissions;
			for (unsigned index = 0U; index < 16U; ++index) admissions.push_back(std::async(std::launch::async, [&resources, path] { return resources.LoadShaderAsync(path); }));
			std::vector<std::shared_future<TerraEngine::UUID>> requests;
			for (auto& admission : admissions) {
				context.Expect(admission.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
					"Concurrent callers must finish admission without waiting for decoding.");
				requests.push_back(admission.get());
			}
			context.Expect(gate.WaitForArrivals(1U), "One decoder must own the admitted key.");
			context.Expect(resources.GetRequestDiagnostics().pending == 1U, "All concurrent callers must share one pending record.");
			resources.SetRequestTestHooks({});
			gate.Release();
			const auto identity = ReadyResource(context, requests.front());
			for (const auto& request : requests) context.Expect(ReadyResource(context, request) == identity, "Concurrent requests must preserve one UUID.");
			context.Expect(resources.GetRequestDiagnostics().published == before.published + 1U, "Exclusive admission must publish exactly once.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.CancelingCollectionPreservesDirectStageDemand", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("direct-shared.tspf");
			WriteTinyVertexStage(path);
			Correctness::ControlledGate gate;
			resources.SetRequestTestHooks({[wait = gate.MakeWaiter()](const std::string&) { (void)wait(); }, {}, {}});
			const auto parent = resources.LoadShaderCollectionAsync({{ShaderStage::Vertex, path}});
			context.Expect(gate.WaitForArrivals(1U), "Shared stage work must reach the controlled decode boundary.");
			const auto direct = resources.LoadShaderAsync(path);
			TerraEngine::UUID parentId = TerraEngine::EMPTY_UUID;
			for (const auto& request : resources.GetRequestDiagnostics().requests) if (request.key.ends_with(';')) parentId = request.assetId;
			context.Expect(resources.CancelStreamableLoad(parentId, AssetType::ShaderCollection), "A collection consumer must cancel independently.");
			context.Expect(ReadyResource(context, parent) == TerraEngine::EMPTY_UUID, "The canceled consumer must settle immediately.");
			resources.SetRequestTestHooks({});
			gate.Release();
			const auto identity = ReadyResource(context, direct);
			context.Expect(identity != TerraEngine::EMPTY_UUID && resources.AcquireShaderSnapshot(identity) != nullptr,
				"Another current direct consumer must retain stage publication demand.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.StageLeaseSurvivesCacheInvalidation", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("leased.tspf");
			WriteTinyVertexStage(path);
			const auto identity = ReadyResource(context, resources.LoadShaderAsync(path));
			const auto lease = resources.AcquireShaderSnapshot(identity);
			context.Expect(lease != nullptr && lease->bytecode.size() == sizeof(std::uint32_t), "The lease must observe the exact published payload.");
			resources.ClearCaches();
			context.Expect(resources.AcquireShaderSnapshot(identity) == nullptr, "Invalidation must detach the current cache owner.");
			std::uint32_t magic = 0U;
			std::memcpy(&magic, lease->bytecode.data(), sizeof(magic));
			context.Expect(magic == 0x07230203U && lease->stringTable.starts_with("Engine/Shaders/"),
				"Leased spans and string views must retain their original backing storage after clear.");
		});

		registry.Add("Engine.ResourceAsyncLifecycle.TeardownDrainsPhysicalLoaderBeforeReopening", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			ResourcePoolSession session(1U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("drain.tspf");
			WriteTinyVertexStage(path);
			Correctness::ControlledGate gate;
			resources.SetRequestTestHooks({[wait = gate.MakeWaiter(std::chrono::seconds(30))](const std::string&) { (void)wait(); }, {}, {}});
			const auto request = resources.LoadShaderAsync(path);
			context.Expect(gate.WaitForArrivals(1U), "A physical loader must reach its controlled boundary.");
			auto drain = std::async(std::launch::async, [&resources] { resources.StopAdmissionsAndDrain(); });
			const bool requestSettled = request.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			const bool drainBlocked = drain.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
			const auto during = resources.GetRequestDiagnostics();
			// Release before assertions so an assertion failure cannot strand the
			// draining owner behind this test's deliberately blocked decoder.
			resources.SetRequestTestHooks({});
			gate.Release();
			context.Expect(requestSettled && request.get() == TerraEngine::EMPTY_UUID, "Stopping admission must terminalize the public request promptly.");
			context.Expect(drainBlocked && during.physicalTasks == 1U && !during.admitting,
				"Canceled request promises must not be mistaken for settled physical loader tasks.");
			context.Expect(drain.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "The protected owner must drain after physical work is released.");
			drain.get();
			context.Expect(ReadyResource(context, resources.LoadShaderAsync(path)) == TerraEngine::EMPTY_UUID, "A stopped session must reject new producer admission.");
			resources.BeginRequestSession();
			context.Expect(ReadyResource(context, resources.LoadShaderAsync(path)) != TerraEngine::EMPTY_UUID,
				"An explicit new session must reopen only after previous physical work settles.");
		});

	}
}
