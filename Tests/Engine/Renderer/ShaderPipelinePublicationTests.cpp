// ShaderPipelinePublicationTests.cpp — exact native pipeline publication identity.
#include "TestHarness.h"
#include "Renderer/RHI/Vulkan/VulkanContextImpl.h"
#include "../Runtime/CorrectnessTestSupport.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"
#include <cstring>
#include <fstream>
#include <future>

namespace TerraEngine::Tests
{
	namespace
	{
		// This parser-valid container exercises real resource publication only;
		// no device is asked to execute its small, deliberately synthetic code.
		void WritePublicationStage(const std::filesystem::path& path, std::uint32_t marker)
		{
			const std::string source = "Engine/Shaders/PublicationFixture.vert";
			ShaderHeader header{};
			std::memcpy(header.magic, &kShaderMagicValue, sizeof(kShaderMagicValue));
			header.version = kShaderCurrentVersion;
			header.headerSize = static_cast<std::uint16_t>(sizeof(header));
			header.reflectionOffset = sizeof(header);
			header.compiledBinaryOffset = header.reflectionOffset + sizeof(ShaderReflection);
			header.stringTableOffset = header.compiledBinaryOffset + sizeof(CompiledStageBinary);
			header.stringTableSize = static_cast<std::uint32_t>(source.size() + 1U);
			header.compiledBinarySize = 2U * sizeof(std::uint32_t);
			ShaderReflection reflection{};
			reflection.source.stage = static_cast<std::uint32_t>(ShaderStage::Vertex);
			reflection.source.entryPointOffset = kInvalidStringOffset;
			reflection.source.targetProfileOffset = kInvalidStringOffset;
			CompiledStageBinary binary{};
			binary.format = static_cast<std::uint32_t>(ShaderBinaryFormat::SPIRV);
			binary.entryPointOffset = kInvalidStringOffset;
			binary.targetProfileOffset = kInvalidStringOffset;
			binary.bytecodeOffset = header.stringTableOffset + header.stringTableSize;
			binary.bytecodeSize = header.compiledBinarySize;
			const std::array<std::uint32_t, 2U> code{0x07230203U, marker};
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(&header), sizeof(header));
			file.write(reinterpret_cast<const char*>(&reflection), sizeof(reflection));
			file.write(reinterpret_cast<const char*>(&binary), sizeof(binary));
			file.write(source.c_str(), static_cast<std::streamsize>(source.size() + 1U));
			file.write(reinterpret_cast<const char*>(code.data()), sizeof(code));
			file.close();
			if (!file) throw std::runtime_error("Could not write shader publication fixture.");
		}

		// Join physical jobs before removing their files, and restore process-wide
		// lifecycle state so subsequent engine tests can open a fresh session.
		class PublicationSession final
		{
		public:
			PublicationSession() : previousRoot(ResourceManager::Instance().GetAssetRoot())
			{
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().BeginRequestSession();
				ResourceManager::Instance().ClearCaches();
				ThreadPool::instance().initialize(1U);
			}
			~PublicationSession()
			{
				auto& resources = ResourceManager::Instance();
				resources.SetRequestTestHooks({});
				ThreadPool::instance().shutdown();
				resources.ClearCaches();
				resources.SetAssetRoot(previousRoot);
				resources.BeginRequestSession();
			}
		private:
			std::filesystem::path previousRoot;
		};

		UUID AwaitPublication(TestContext& context, const std::shared_future<UUID>& future)
		{
			context.Expect(future.valid() && future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"The real resource request must settle within the watchdog deadline.");
			return future.get();
		}
	}

	void RegisterShaderPipelinePublicationTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.ShaderPipeline.OverrideRestoresExactOwner", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationSession session;
			auto& resources = ResourceManager::Instance();
			const auto baselinePath = fixture.Child("baseline.tspf");
			const auto overridePath = fixture.Child("override.tspf");
			WritePublicationStage(baselinePath, 1U); WritePublicationStage(overridePath, 2U);
			const auto id = AwaitPublication(context, resources.LoadShaderAsync(baselinePath));
			const auto baseline = resources.AcquireShaderSnapshot(id);
			context.Expect(id != EMPTY_UUID && baseline != nullptr, "Baseline must be an actual accepted shader owner.");
			const auto receipt = resources.BeginRendererEfficiencyShaderOverride(id, overridePath);
			context.Expect(AwaitPublication(context, receipt.completion) == id, "Override must preserve durable identity.");
			const auto overrideOwner = resources.GetRendererEfficiencyShaderOverrideOwner(receipt);
			context.Expect(overrideOwner && overrideOwner != baseline && overrideOwner->publicationGeneration > baseline->publicationGeneration,
				"Override must publish a distinct immutable owner with an actual accepted revision.");
			context.Expect(resources.AcquireShaderSnapshot(id) == overrideOwner, "Receipt must identify the visible owner.");
			context.Expect(resources.RestoreRendererEfficiencyShaderOverride(receipt), "Accepted current override must restore.");
			context.Expect(resources.AcquireShaderSnapshot(id) == baseline, "Restore must preserve the exact previous owner, not decode a replacement.");
			context.Expect(!resources.RestoreRendererEfficiencyShaderOverride(receipt), "A consumed restore receipt must not be reusable.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.InflightReloadSupersedesRestore", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationSession session;
			Correctness::ControlledGate gate;
			auto& resources = ResourceManager::Instance();
			const auto baselinePath = fixture.Child("baseline.tspf"), overridePath = fixture.Child("override.tspf"), newerPath = fixture.Child("newer.tspf");
			WritePublicationStage(baselinePath, 1U); WritePublicationStage(overridePath, 2U); WritePublicationStage(newerPath, 3U);
			const auto id = AwaitPublication(context, resources.LoadShaderAsync(baselinePath));
			const auto receipt = resources.BeginRendererEfficiencyShaderOverride(id, overridePath);
			context.Expect(AwaitPublication(context, receipt.completion) == id && id != EMPTY_UUID, "Override must publish before supersession.");
			const auto overrideOwner = resources.AcquireShaderSnapshot(id);
			ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [wait = gate.MakeWaiter()](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks));
			const auto newer = resources.ReloadShaderFromAuthoredPathAsync(id, newerPath);
			context.Expect(gate.WaitForArrivals(1U), "Newer reload must reach its real publication boundary.");
			context.Expect(newer.wait_for(std::chrono::seconds(0)) != std::future_status::ready,
				"The superseding request must still be in flight when cleanup is tested.");
			context.Expect(resources.AcquireShaderSnapshot(id) == overrideOwner, "Visible owner intentionally remains the old override.");
			context.Expect(!resources.RestoreRendererEfficiencyShaderOverride(receipt), "New request authority must block old cleanup before publication.");
			context.Expect(resources.AcquireShaderSnapshot(id) == overrideOwner, "Rejected cleanup must be atomic.");
			resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(AwaitPublication(context, newer) == id, "Rejected cleanup must leave newer publication alive.");
			context.Expect(resources.AcquireShaderSnapshot(id) != overrideOwner, "Newer immutable owner must become visible.");
			context.Expect(!resources.RestoreRendererEfficiencyShaderOverride(receipt), "Superseded receipt stays invalid after completion.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.ProjectEpochRejectsRestore", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationSession session;
			auto& resources = ResourceManager::Instance();
			const auto baselinePath = fixture.Child("baseline.tspf"), overridePath = fixture.Child("override.tspf");
			WritePublicationStage(baselinePath, 1U); WritePublicationStage(overridePath, 2U);
			const auto id = AwaitPublication(context, resources.LoadShaderAsync(baselinePath));
			const auto receipt = resources.BeginRendererEfficiencyShaderOverride(id, overridePath);
			context.Expect(AwaitPublication(context, receipt.completion) == id && id != EMPTY_UUID, "Override must publish before project change.");
			const auto before = resources.AcquireShaderSnapshot(id);
			resources.SetAssetRoot(fixture.Child("next-project/Assets"));
			const auto afterEpoch = resources.AcquireShaderSnapshot(id);
			context.Expect(!resources.RestoreRendererEfficiencyShaderOverride(receipt), "Actual project-root epoch must revoke the receipt.");
			context.Expect(resources.AcquireShaderSnapshot(id) == afterEpoch, "Rejected old-project cleanup must preserve current cache state.");
			context.Expect(before != nullptr, "The test must start with an accepted owner rather than an unavailable receipt.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.PendingAndStoppedRestoreReject", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationSession session;
			Correctness::ControlledGate gate;
			auto& resources = ResourceManager::Instance();
			const auto baselinePath = fixture.Child("baseline.tspf"), overridePath = fixture.Child("override.tspf");
			WritePublicationStage(baselinePath, 1U); WritePublicationStage(overridePath, 2U);
			const auto id = AwaitPublication(context, resources.LoadShaderAsync(baselinePath));
			const auto baseline = resources.AcquireShaderSnapshot(id);
			context.Expect(id != EMPTY_UUID && baseline != nullptr, "Pending cleanup requires a genuine previous owner.");
			ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [wait = gate.MakeWaiter()](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks));
			const auto receipt = resources.BeginRendererEfficiencyShaderOverride(id, overridePath);
			context.Expect(gate.WaitForArrivals(1U), "Override must be held before actual publication.");
			context.Expect(!resources.RestoreRendererEfficiencyShaderOverride(receipt), "Pending receipt cannot restore a not-yet-published owner.");
			context.Expect(resources.AcquireShaderSnapshot(id) == baseline, "Pending cleanup must leave baseline unchanged.");
			// Stop settles logical requests first; its physical drain is released
			// before any assertion can unwind and wait on the asynchronous drain.
			auto drain = std::async(std::launch::async, [&resources] { resources.StopAdmissionsAndDrain(); });
			const bool settled = receipt.completion.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready;
			const bool restored = resources.RestoreRendererEfficiencyShaderOverride(receipt);
			const auto stoppedOwner = resources.AcquireShaderSnapshot(id);
			resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(drain.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "Canceled physical request must drain after gate release.");
			drain.get();
			context.Expect(settled && receipt.completion.get() == EMPTY_UUID, "Stopped override must settle as canceled, not published.");
			context.Expect(!restored && stoppedOwner == baseline && resources.AcquireShaderSnapshot(id) == baseline,
				"Stopped cleanup and later worker completion must not replace the previous owner.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.SameIdentitySameSizeNewPublication", [](TestContext& context) {
			// Equal UUIDs and bytecode lengths describe a normal in-place reload.
			// Only the accepted immutable publication distinguishes these modules.
			PipelineKey previous{};
			previous.shaderCollectionId[0] = 1U;
			previous.collectionGeneration = 7U;
			PipelineKey::StagePublication stage;
			stage.stage = ShaderStage::Mesh; stage.shaderId[0] = 2U; stage.generation = 11U;
			previous.stagePublications.push_back(stage);
			auto replacement = previous;
			replacement.stagePublications[0].generation = 12U;
			context.Expect(!(previous == replacement), "Same-size shader replacement must change the exact pipeline key.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.StageOrderAndIdentityExact", [](TestContext& context) {
			PipelineKey first{};
			PipelineKey::StagePublication mesh;
			mesh.stage = ShaderStage::Mesh; mesh.shaderId[0] = 1U; mesh.generation = 2U;
			PipelineKey::StagePublication fragment;
			fragment.stage = ShaderStage::Fragment; fragment.shaderId[0] = 2U; fragment.generation = 3U;
			first.stagePublications = {mesh, fragment};
			auto reordered = first;
			std::reverse(reordered.stagePublications.begin(), reordered.stagePublications.end());
			context.Expect(!(first == reordered), "Ordered actual stage publications must participate in equality.");
			auto other = first; other.stagePublications[0].shaderId[0] = 3U;
			context.Expect(!(first == other), "Actual shader identity must participate in equality.");
			context.Expect(first == first, "Unchanged accepted stage tuple must remain a cache hit.");
		});
		registry.Add("Renderer.Efficiency.ShaderPipeline.CollectionRetainsReflectedOwner", [](TestContext& context) {
			// Replacing a cache owner must not change the bytes used to create the
			// module whose reflection and key were prepared from the old owner.
			auto oldShader = std::make_shared<Shader>();
			oldShader->publicationGeneration = 1U; oldShader->storage = {1U, 2U, 3U, 4U};
			oldShader->bytecode = oldShader->storage;
			ShaderCollection collection;
			ShaderCollection::StageBinding stage;
			stage.shader = oldShader; stage.publicationGeneration = 1U;
			collection.stages.push_back(stage);
			oldShader = std::make_shared<Shader>();
			oldShader->publicationGeneration = 2U; oldShader->storage = {4U, 3U, 2U, 1U};
			context.Expect(collection.stages[0].shader->publicationGeneration == 1U,
				"A collection must retain its reflected immutable publication.");
			context.Expect(collection.stages[0].shader->bytecode[0] == 1U,
				"Replacement must not alter retained module bytes.");
		});
	}
}
