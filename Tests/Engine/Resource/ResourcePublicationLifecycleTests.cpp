// Fresh cooked textures and controlled pool dispatch exercise the production
// reload/publication paths. No renderer device or ignored cooked asset is used.
#include "../TestHarness.h"
#include "../Runtime/CorrectnessTestSupport.h"

#include "File Formats/TerraEngineTextureFile.h"
#include "File Formats/TerraEngineMaterialFile.h"
#include "File Formats/TerraEngineMeshFile.h"
#include "File Formats/TerraEngineAudioFile.h"
#include "Audio/Assets/AudioClipAsset.h"
#include "Animation/Assets/SkeletonAsset.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Resource/ResourceManager.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		// The vendored Basis Universal solid-color UASTC mode writes the five-bit
		// mode code 0x17 followed by RGBA bytes. Zero hints are legal for this mode.
		// A real one-block, one-mip texture therefore decodes to a known red byte.
		void WriteTinyTexture(const std::filesystem::path& path, std::uint8_t red)
		{
			TETFHeader header{};
			header.magic = kMagic;
			header.version = kVersion;
			header.headerSize = sizeof(TETFHeader);
			header.alignment = 1U;
			header.chunkCount = 2U;
			header.tocOffset = sizeof(TETFHeader);
			const std::uint64_t payloadBase = sizeof(TETFHeader) + 2U * sizeof(TETFChunkEntry);
			const std::uint64_t descriptorBytes = sizeof(TETFDesc) + sizeof(TETFLevelEntry);
			header.totalSize = payloadBase + descriptorBytes + 16U;
			const std::array<TETFChunkEntry, 2U> chunks{{
				{kChunkDESC, 0U, payloadBase, descriptorBytes},
				{kChunkUAST, 0U, payloadBase + descriptorBytes, 16U}
			}};
			TETFDesc descriptor{};
			descriptor.width = 4U;
			descriptor.height = 4U;
			descriptor.depth = 1U;
			descriptor.layers = 1U;
			descriptor.mipCount = 1U;
			descriptor.faces = 1U;
			descriptor.blockWidth = 4U;
			descriptor.blockHeight = 4U;
			descriptor.blockBytes = 16U;
			descriptor.semantic = static_cast<std::uint8_t>(TextureFileSemantic::Data);
			descriptor.payloadChunkIndex = 1U;
			descriptor.userMetadataChunk = TETF_INVALID_CHUNK_INDEX;
			descriptor.thumbnailChunk = TETF_INVALID_CHUNK_INDEX;
			descriptor.vkFormat = 37U; // Linear RGBA8 gives an exact decoded marker.
			const TETFLevelEntry level{0U, 16U};
			std::array<std::uint8_t, 16U> block{};
			unsigned bit = 0U;
			const auto append = [&block, &bit](std::uint32_t value, unsigned count) {
				for (unsigned index = 0U; index < count; ++index, ++bit) {
					block[bit / 8U] |= static_cast<std::uint8_t>(((value >> index) & 1U) << (bit % 8U));
				}
			};
			append(0x17U, 5U);
			append(red, 8U);
			append(31U, 8U);
			append(47U, 8U);
			append(255U, 8U);
			std::vector<std::uint8_t> bytes(static_cast<std::size_t>(header.totalSize));
			std::memcpy(bytes.data(), &header, sizeof(header));
			std::memcpy(bytes.data() + header.tocOffset, chunks.data(), sizeof(chunks));
			std::memcpy(bytes.data() + payloadBase, &descriptor, sizeof(descriptor));
			std::memcpy(bytes.data() + payloadBase + sizeof(descriptor), &level, sizeof(level));
			std::memcpy(bytes.data() + payloadBase + descriptorBytes, block.data(), block.size());
			// The file checksum is computed with its own header field still zero.
			std::uint32_t checksum = 0xFFFFFFFFU;
			for (const auto byte : bytes) {
				checksum ^= byte;
				for (unsigned index = 0U; index < 8U; ++index) checksum = (checksum >> 1U) ^ ((checksum & 1U) ? 0xEDB88320U : 0U);
			}
			header.fileCrc32 = ~checksum;
			std::memcpy(bytes.data(), &header, sizeof(header));
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			file.close();
			if (!file) throw std::runtime_error("Could not write the tiny publication texture fixture.");
		}

		template <typename Byte>
		void WriteBytes(const std::filesystem::path& path, const std::vector<Byte>& bytes) {
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
			if (!output) throw std::runtime_error("Could not write a detached publication fixture.");
		}

		void WriteTinyMaterial(const std::filesystem::path& path, float marker) {
			MaterialHeader header{};
			std::memcpy(header.magic, "TMAT", 4U);
			header.version = kMaterialCurrentVersion;
			header.headerSize = sizeof(header);
			header.baseColorFactor[0] = marker;
			header.baseColorFactor[3] = 1.0F;
			header.textureArrayOffset = sizeof(header);
			header.floatParameterOffset = sizeof(header);
			std::vector<std::uint8_t> bytes(sizeof(header));
			std::memcpy(bytes.data(), &header, sizeof(header));
			WriteBytes(path, bytes);
		}

		void WriteTinySkeleton(TestContext& context, const std::filesystem::path& path, const TerraEngine::UUID& id) {
			using namespace TerraEngine::Animation;
			SkeletonAsset asset;
			asset.assetId.value = id;
			asset.compatibilityFingerprint = 12345U;
			asset.referencePoseHash = 54321U;
			asset.rootJointIndex = 0U;
			asset.joints.push_back({100U, -1, AnimationTransform{}, true, "Root", "/root", "hips"});
			AnimationMatrix4x4 matrix{};
			matrix.values[0U] = matrix.values[5U] = matrix.values[10U] = matrix.values[15U] = 1.0F;
			asset.inverseBindMatrices.push_back(matrix);
			std::vector<std::uint8_t> bytes;
			context.Expect(SerializeSkeletonAsset(asset, bytes) == AnimationStatus::Ok, "Fresh skeleton bytes must serialize through the real cooked format.");
			WriteBytes(path, bytes);
		}

		void WriteTinyNavigation(TestContext& context, const std::filesystem::path& path, float width) {
			TerraEngine::Navigation::NavigationGeometryDocument document;
			document.kind = TerraEngine::Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{0.0F, 0.0F, 0.0F}, {width, 0.0F, 0.0F}, {0.0F, 0.0F, width}};
			document.triangles = {{0U, 1U, 2U}};
			const auto encoded = TerraEngine::Navigation::EncodeNavigationGeometry(document);
			context.Expect(encoded.accepted, "Fresh navigation bytes must use the real companion encoder.");
			WriteBytes(path, encoded.bytes);
		}

		void WriteTinyAudio(TestContext& context, const std::filesystem::path& path, const TerraEngine::UUID& id, std::uint8_t marker) {
			TerraEngine::Audio::AudioFileMetadata metadata;
			metadata.assetId = id;
			metadata.encoding = TerraEngine::Audio::AudioEncoding::Wave;
			metadata.channelCount = 1U; metadata.sampleRate = 48000U; metadata.frameCount = 4U;
			const std::vector<std::uint8_t> payload{marker, 1U, 2U, 3U};
			metadata.sourceChecksum = TerraEngine::Audio::ComputeAudioChecksum(payload);
			metadata.payloadChecksum = metadata.sourceChecksum;
			std::vector<std::uint8_t> bytes;
			context.Expect(TerraEngine::Audio::SerializeAudioFile(metadata, payload, bytes) == TerraEngine::Audio::AudioFileStatus::Ok,
				"The provider-neutral fixture must pass its production envelope writer.");
			WriteBytes(path, bytes);
		}

		class PublicationPoolSession final {
		public:
			PublicationPoolSession(std::size_t workers = 1U)
			{
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().BeginRequestSession();
				ResourceManager::Instance().ClearCaches();
				ThreadPool::instance().initialize(workers);
			}
			~PublicationPoolSession()
			{
				ResourceManager::Instance().SetRequestTestHooks({});
				ThreadPool::instance().shutdown();
				ResourceManager::Instance().ClearCaches();
				// Cases may intentionally stop their owner session. Leave a fresh
				// explicit session for later resource consumers in the shared process.
				ResourceManager::Instance().BeginRequestSession();
			}
		};

		std::future<Correctness::GateResult> BlockLoaderPool(Correctness::ControlledGate& gate)
		{
			auto task = ThreadPool::instance().createTask(gate.MakeWaiter(std::chrono::seconds(30)));
			auto future = task->getFuture();
			ThreadPool::instance().enqueue(task);
			return future;
		}

		// A FIFO marker proves the previously queued physical reload has returned,
		// even when the repaired public adapter terminalizes early on invalidation.
		void WaitForPoolFence(TestContext& context)
		{
			auto task = ThreadPool::instance().createTask([] {});
			auto future = task->getFuture();
			ThreadPool::instance().enqueue(task);
			context.Expect(future.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Physical resource work must settle before inspecting its publication effects.");
			future.get();
		}

		std::uint8_t LoadedRed(TestContext& context, ResourceManager& manager, const TerraEngine::UUID& id)
		{
			const auto* texture = manager.GetTexture(id);
			context.Expect(texture != nullptr && texture->transcodedData.size() == 64U,
				"The fresh one-block fixture must pass the production texture decoder.");
			return texture->transcodedData.front();
		}
	}

	void RegisterResourcePublicationLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.ReloadAfterClearCannotResurrect", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationPoolSession session;
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("clear.tetf");
			WriteTinyTexture(path, 17U);
			const auto identity = resources.LoadTexture(path);
			context.Expect(LoadedRed(context, resources, identity) == 17U, "Initial cooked fixture content must decode exactly.");
			Correctness::ControlledGate gate;
			auto blocker = BlockLoaderPool(gate);
			context.Expect(gate.WaitForArrivals(1U), "The worker must stop before reload dispatch.");
			const auto reload = resources.ReloadTexture(identity);
			resources.ClearCaches();
			const auto generation = resources.GetRendererResourcePublicationGeneration();
			std::cout << "Texture reload admitted before clear; releasing physical work.\n" << std::flush;
			gate.Release();
			context.Expect(reload.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Invalidated reload adapters must settle.");
			reload.get();
			WaitForPoolFence(context);
			context.Expect(resources.GetTexture(identity) == nullptr, "An old reload must not resurrect a cleared production cache entry.");
			context.Expect(resources.GetRendererResourcePublicationGeneration() == generation,
				"An obsolete reload must not advance renderer publication generations.");
			context.Expect(!resources.IsResourceResident(identity) && !resources.GetStreamableAsset(identity).has_value(),
				"Cleared residency and streaming accounting must not be recreated by obsolete work.");
			context.Expect(blocker.get() == Correctness::GateResult::Released, "The dispatch gate must release normally.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.NewOwnerBeforeOlderReloadWins", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationPoolSession session;
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("latest.tetf");
			WriteTinyTexture(path, 17U);
			const auto identity = resources.LoadTexture(path);
			context.Expect(LoadedRed(context, resources, identity) == 17U, "The old content marker must load before admission.");
			Correctness::ControlledGate gate;
			auto blocker = BlockLoaderPool(gate);
			context.Expect(gate.WaitForArrivals(1U), "The worker must stop before the older reload executes.");
			const auto older = resources.ReloadTexture(identity);
			resources.ClearCaches();
			WriteTinyTexture(path, 92U);
			resources.RegisterAssetPath(identity, path, AssetType::Texture);
			context.Expect(resources.LoadTexture(path) == identity && LoadedRed(context, resources, identity) == 92U,
				"The new owner must publish its accepted content before old physical work runs.");
			const auto generation = resources.GetRendererResourcePublicationGeneration();
			// Restore old disk bytes only after the new owner's detached decode has
			// published. The still-queued old request then finishes with old content.
			WriteTinyTexture(path, 17U);
			std::cout << "New texture owner published before older reload; releasing old work.\n" << std::flush;
			gate.Release();
			context.Expect(older.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "The old reload must terminalize.");
			older.get();
			WaitForPoolFence(context);
			context.Expect(LoadedRed(context, resources, identity) == 92U, "New accepted ownership must win when old work completes later.");
			context.Expect(resources.GetRendererResourcePublicationGeneration() == generation,
				"Old completion must preserve the new owner's publication generation.");
			context.Expect(resources.IsResourceResident(identity) && resources.GetStreamableAsset(identity).has_value(),
				"The current owner's residency and streaming records must remain intact.");
			context.Expect(blocker.get() == Correctness::GateResult::Released, "The ordering gate must release normally.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.ControlledOwnershipInterleavings", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationPoolSession session;
			auto& resources = ResourceManager::Instance();
			const auto oldRoot = resources.GetEngineRoot();
			struct RootRestore { ResourceManager& manager; std::filesystem::path root; ~RootRestore() { manager.SetEngineRoot(root); } } restore{resources, oldRoot};
			const auto path = fixture.Child("ownership.tetf");
			const auto replacement = fixture.Child("replacement.tetf");
			WriteTinyTexture(path, 17U);
			WriteTinyTexture(replacement, 92U);
			for (unsigned iteration = 0U; iteration < 100U; ++iteration) {
				resources.SetEngineRoot(oldRoot);
				resources.ClearCaches();
				const auto identity = resources.LoadTexture(path);
				context.Expect(LoadedRed(context, resources, identity) == 17U, "Each ownership interleaving must begin with freshly decoded content.");
				Correctness::ControlledGate gate;
				auto blocker = BlockLoaderPool(gate);
				context.Expect(gate.WaitForArrivals(1U), "Physical reload dispatch must be controlled for every interleaving.");
				const auto before = resources.GetRequestDiagnostics();
				const auto reload = resources.ReloadTexture(identity);
				// Cover global and identity-specific ownership loss with the same
				// production reload. Registration preserves last-good bytes until
				// current content publishes; it still withdraws the old worker's right.
				switch (iteration % 5U) {
				case 0U: resources.ClearCaches(); break;
				case 1U: resources.DeleteResource(identity, AssetType::Texture); break;
				case 2U: (void)resources.ReleaseStreamableAssetForRuntime(identity, AssetType::Texture, iteration + 1U); break;
				case 3U: resources.RegisterAssetPath(identity, replacement, AssetType::Texture); break;
				default: resources.SetEngineRoot(fixture.Child("replacement-engine")); break;
				}
				const auto generation = resources.GetRendererResourcePublicationGeneration();
				gate.Release();
				context.Expect(reload.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
					"Every obsolete reload adapter must terminalize within the bounded fixture deadline.");
				reload.get();
				WaitForPoolFence(context);
				const auto after = resources.GetRequestDiagnostics();
				context.Expect(resources.GetRendererResourcePublicationGeneration() == generation,
					"Ownership loss must prevent all late renderer-generation publication.");
				const auto countBefore = before.published + before.failed + before.canceled + before.obsolete;
				const auto countAfter = after.published + after.failed + after.canceled + after.obsolete;
				context.Expect(countAfter == countBefore + 1U && after.pending == 0U && after.physicalTasks == 0U,
					"Each invalidated reload must record exactly one terminal result and leave no unresolved physical/request work.");
				if (iteration % 5U <= 1U) {
					context.Expect(resources.GetTexture(identity) == nullptr && !resources.IsResourceResident(identity)
						&& !resources.GetStreamableAsset(identity).has_value(),
						"Cleared/deleted production caches and accounting must stay absent after old completion.");
				}
				if (iteration % 5U == 2U) context.Expect(!resources.IsResourceResident(identity), "Released residency must remain released after old completion.");
				context.Expect(blocker.get() == Correctness::GateResult::Released, "Every ownership interleaving must release its dispatch gate normally.");
			}
		});

		registry.Add("Engine.ResourceAsyncLifecycle.Publication.NewerTextureReloadWinsInBothCompletionOrders", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture;
			PublicationPoolSession session(2U);
			auto& resources = ResourceManager::Instance();
			const auto path = fixture.Child("ordered.tetf");
			for (unsigned order = 0U; order < 2U; ++order) {
				resources.BeginRequestSession(); resources.ClearCaches();
				WriteTinyTexture(path, 17U);
				const auto identity = resources.LoadTexture(path);
				Correctness::ControlledGate oldGate, newGate;
				std::atomic_uint calls{0U};
				ResourceManager::RequestTestHooks hooks;
				hooks.beforePublication = [&](const std::string&) {
					if (calls.fetch_add(1U) == 0U) (void)oldGate.MakeWaiter(std::chrono::seconds(30))();
					else (void)newGate.MakeWaiter(std::chrono::seconds(30))();
				};
				resources.SetRequestTestHooks(std::move(hooks));
				const auto older = resources.ReloadTexture(identity);
				context.Expect(oldGate.WaitForArrivals(1U), "Older reload must finish detached decoding first.");
				WriteTinyTexture(path, 92U);
				const auto newer = resources.ReloadTexture(identity);
				context.Expect(newGate.WaitForArrivals(1U), "Newer reload must own a distinct detached payload.");
				resources.SetRequestTestHooks({});
				if (order == 0U) { oldGate.Release(); WaitForPoolFence(context); newGate.Release(); }
				else { newGate.Release(); context.Expect(newer.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready, "New owner must publish before old completion."); oldGate.Release(); }
				older.get(); newer.get(); resources.StopAdmissionsAndDrain();
				context.Expect(LoadedRed(context, resources, identity) == 92U, "Admission order must win under either physical completion order.");
			}
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.FailedTextureReloadRetainsPayloadAndAccounting", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto path = fixture.Child("last-good.tetf");
			WriteTinyTexture(path, 17U); const auto id = resources.LoadTexture(path);
			const auto generation = resources.GetRendererResourcePublicationGeneration();
			const auto accounting = resources.GetStreamableAsset(id);
			ResourceManager::RequestTestHooks hooks;
			hooks.beforeDecode = [](const std::string&) { throw std::runtime_error("Controlled reload decode fault."); };
			resources.SetRequestTestHooks(std::move(hooks));
			resources.ReloadTexture(id).get(); resources.SetRequestTestHooks({});
			WriteBytes(path, std::vector<std::uint8_t>{0U}); resources.ReloadTexture(id).get();
			resources.StopAdmissionsAndDrain();
			const auto after = resources.GetStreamableAsset(id);
			context.Expect(LoadedRed(context, resources, id) == 17U && resources.GetRendererResourcePublicationGeneration() == generation,
				"Exceptional and malformed reloads must retain the current accepted generation.");
			context.Expect(accounting && after && accounting->payloadState == after->payloadState
				&& accounting->estimatedCpuBytes == after->estimatedCpuBytes && resources.IsResourceResident(id),
				"Last-good accounting must stay truthful when a replacement fails.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.AuthoredMaterialSupersedesOlderPreview", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto path = fixture.Child("runtime.tmat"), authored = fixture.Child("authored.tmat");
			WriteTinyMaterial(path, 0.1F); WriteTinyMaterial(authored, 0.4F); const auto id = resources.LoadMaterial(path);
			context.Expect(resources.GetMaterial(id) != nullptr, "Minimal TMAT must pass the production material decoder.");
			Correctness::ControlledGate gate; std::atomic_uint calls{0U};
			ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [&](const std::string&) { if (calls.fetch_add(1U) == 0U) (void)gate.MakeWaiter(std::chrono::seconds(30))(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto older = std::async(std::launch::async, [&] { return resources.ReloadMaterialFromAuthoredPath(id, authored); });
			context.Expect(gate.WaitForArrivals(1U), "Older authored preview must be detached before publication.");
			WriteTinyMaterial(authored, 0.9F);
			context.Expect(resources.ReloadMaterialFromAuthoredPath(id, authored), "Later authored preview must publish through current runtime identity.");
			resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(!older.get() && resources.GetMaterial(id)->header.baseColorFactor[0] == 0.9F,
				"Late old preview completion must not overwrite the newer material.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.GeneratedMeshCannotPublishAfterClear", [](TestContext& context) {
			PublicationPoolSession session; auto& resources = ResourceManager::Instance();
			const auto id = resources.RegisterMesh("publication-mesh", std::make_unique<Mesh>());
			context.Expect(resources.GetMesh(id) != nullptr && resources.IsResourceResident(id), "Generated mesh registration must publish cache and accounting together.");
			Correctness::ControlledGate gate; ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [wait = gate.MakeWaiter(std::chrono::seconds(30))](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto replacement = std::async(std::launch::async, [&] { return resources.RegisterMesh("publication-mesh", std::make_unique<Mesh>()); });
			context.Expect(gate.WaitForArrivals(1U), "Generated payload must reach the real publication boundary.");
			resources.ClearCaches(); const auto generation = resources.GetRendererResourcePublicationGeneration();
			resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(replacement.get() == TerraEngine::EMPTY_UUID && resources.GetMesh(id) == nullptr && !resources.IsResourceResident(id)
				&& resources.GetRendererResourcePublicationGeneration() == generation, "Generated replacement authority must be withdrawn by clear.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.UnknownCookedIdentityCannotResurrectDeletion", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto id = TerraEngine::GenerateUUID(); const auto path = fixture.Child("identity.teskel");
			WriteTinySkeleton(context, path, id);
			Correctness::ControlledGate gate; ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [wait = gate.MakeWaiter(std::chrono::seconds(30))](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto load = std::async(std::launch::async, [&] { return resources.LoadSkeleton(path); });
			context.Expect(gate.WaitForArrivals(1U), "Self-identifying first load must reveal identity after admission.");
			resources.DeleteResource(id, AssetType::Skeleton); resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(load.get() == TerraEngine::EMPTY_UUID && resources.GetSkeleton(id) == nullptr && !resources.IsResourceResident(id),
				"An identity tombstone must reject a request that did not know its UUID when admitted.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.PreparedAnimationRejectsReplacedSession", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto id = TerraEngine::GenerateUUID(); const auto path = fixture.Child("prepared.teskel");
			const auto initialRevision = resources.GetAnimationBindingResourceRevision();
			WriteTinySkeleton(context, path, id); context.Expect(resources.LoadSkeleton(path) == id, "Prepared reload needs a real live skeleton publication.");
			context.Expect(resources.GetAnimationBindingResourceRevision() == initialRevision + 1U,
				"First animation payload publication must wake binding reconciliation exactly once.");
			const auto initialGeneration = resources.GetAnimationAssetRuntimeGeneration(id, AssetType::Skeleton);
			context.Expect(resources.ReloadSkeleton(id) && resources.GetAnimationBindingResourceRevision() == initialRevision + 2U
				&& resources.GetAnimationAssetRuntimeGeneration(id, AssetType::Skeleton) == initialGeneration + 1U,
				"Direct hot reload must publish both asset generation and binding-visible revision together.");
			const AnimationAssetReloadRequest request{id, AssetType::Skeleton};
			auto oldBatch = resources.PrepareAnimationAssetReloadBatch(std::span(&request, 1U));
			context.Expect(oldBatch.Accepted(), "Valid cooked replacement must prepare through production parsing.");
			resources.ClearCaches(); context.Expect(resources.LoadSkeleton(path) == id, "New ownership must publish before old commit.");
			const auto generation = resources.GetAnimationAssetRuntimeGeneration(id, AssetType::Skeleton);
			context.Expect(!resources.CommitPreparedAnimationAssetReloadBatch(oldBatch), "A prepared transaction cannot cross ownership epochs.");
			context.Expect(resources.GetAnimationAssetRuntimeGeneration(id, AssetType::Skeleton) == generation && resources.IsResourceResident(id), "Rejected old commit must preserve new cache and accounting.");
			auto current = resources.PrepareAnimationAssetReloadBatch(std::span(&request, 1U));
			context.Expect(current.Accepted() && resources.CommitPreparedAnimationAssetReloadBatch(current), "Current prepared work must still commit allocation-free.");
			resources.FinalizeCommittedAnimationAssetReloadBatch(current);
			context.Expect(resources.GetAnimationAssetRuntimeGeneration(id, AssetType::Skeleton) == generation + 1U && resources.GetRequestDiagnostics().pending == 0U,
				"One current prepared commit must advance generation and terminalize ownership once.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.NavigationReplacementPreservesCurrentAndPriorLeases", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto id = TerraEngine::GenerateUUID(); const auto path = fixture.Child("old.tenavgeom"), newer = fixture.Child("new.tenavgeom");
			WriteTinyNavigation(context, path, 2.0F); WriteTinyNavigation(context, newer, 4.0F);
			TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor descriptor;
			descriptor.sourceUuid.value = id; descriptor.sourceRevision = 1U; descriptor.canonicalPlacementRevision = 1U; descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = TerraEngine::Navigation::NavigationSourceKind::Mesh;
			const auto initial = resources.LoadNavigationGeometryCompanion(id, path, descriptor);
			context.Expect(initial.accepted && initial.snapshot, "Minimal navigation companion must pass production decoding.");
			Correctness::ControlledGate gate; std::atomic_uint calls{0U}; ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [&](const std::string&) { if (calls.fetch_add(1U) == 0U) (void)gate.MakeWaiter(std::chrono::seconds(30))(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto oldLoad = std::async(std::launch::async, [&] { return resources.LoadNavigationGeometryCompanion(id, path, descriptor); });
			context.Expect(gate.WaitForArrivals(1U), "Old navigation decode must stop before its protected exchange.");
			resources.RegisterNavigationGeometryPath(id, newer);
			const auto current = resources.LoadNavigationGeometryCompanion(id, newer, descriptor);
			resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(!oldLoad.get().accepted && current.accepted && resources.GetNavigationGeometrySnapshot(id) == current.snapshot,
				"Old companion completion cannot overwrite the new registered snapshot.");
			context.Expect(initial.snapshot->ContentFingerprint() != current.snapshot->ContentFingerprint(), "Prior immutable geometry lease must retain its own content.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.NavigationRootChangeInvalidatesAdmittedRelativeWork", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto id = TerraEngine::GenerateUUID();
			const auto oldRoot = resources.GetAssetRoot();
			struct RootRestore { ResourceManager& manager; std::filesystem::path root; ~RootRestore() { manager.SetAssetRoot(root); } } restore{resources, oldRoot};
			const auto firstRoot = fixture.Child("first"), secondRoot = fixture.Child("second");
			std::filesystem::create_directories(firstRoot); std::filesystem::create_directories(secondRoot);
			const std::filesystem::path relative = "relative.tenavgeom";
			WriteTinyNavigation(context, firstRoot / relative, 2.0F); WriteTinyNavigation(context, secondRoot / relative, 4.0F);
			TerraEngine::Navigation::NavigationGeometrySnapshotDescriptor descriptor;
			descriptor.sourceUuid.value = id; descriptor.sourceRevision = 1U; descriptor.canonicalPlacementRevision = 1U; descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = TerraEngine::Navigation::NavigationSourceKind::Mesh;
			resources.SetAssetRoot(firstRoot);
			const auto initial = resources.LoadNavigationGeometryCompanion(id, relative, descriptor);
			context.Expect(initial.accepted && initial.snapshot, "Relative companion must resolve against the first project root.");
			// Hold real admitted work before decoding, replace its owning root, then
			// retry the same authored relative path through fresh production admission.
			Correctness::ControlledGate gate; ResourceManager::RequestTestHooks hooks;
			hooks.beforeDecode = [wait = gate.MakeWaiter(std::chrono::seconds(30))](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto oldLoad = std::async(std::launch::async, [&] { return resources.LoadNavigationGeometryCompanion(id, relative, descriptor); });
			context.Expect(gate.WaitForArrivals(1U), "Old relative companion must already own its captured admission before root replacement.");
			resources.SetAssetRoot(secondRoot); resources.SetRequestTestHooks({}); gate.Release();
			context.Expect(!oldLoad.get().accepted, "Root replacement must reject the older admitted companion.");
			const auto current = resources.LoadNavigationGeometryCompanion(id, relative, descriptor);
			context.Expect(current.accepted && current.snapshot && current.resolvedPath == (secondRoot / relative).lexically_normal()
				&& resources.GetNavigationGeometrySnapshot(id) == current.snapshot,
				"Fresh relative admission must use and publish the replacement project root.");
			if (initial.snapshot && current.snapshot) context.Expect(initial.snapshot->ContentFingerprint() != current.snapshot->ContentFingerprint(),
				"Old immutable geometry leases must preserve first-root content after replacement.");
		});
		registry.Add("Engine.ResourceAsyncLifecycle.Publication.AudioReloadPreservesLeasesAndRejectsOlderWork", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto id = TerraEngine::GenerateUUID(); const auto path = fixture.Child("lease.teaudio");
			WriteTinyAudio(context, path, id, 17U); context.Expect(resources.LoadAudioClip(path) == id, "Fresh TEAF must pass production clip loading.");
			const auto initial = resources.GetAudioClip(id);
			Correctness::ControlledGate gate; std::atomic_uint calls{0U}; ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [&](const std::string&) { if (calls.fetch_add(1U) == 0U) (void)gate.MakeWaiter(std::chrono::seconds(30))(); };
			resources.SetRequestTestHooks(std::move(hooks));
			auto oldReload = std::async(std::launch::async, [&] { return resources.ReloadAudioClip(id); });
			context.Expect(gate.WaitForArrivals(1U), "Old audio payload must be detached before publication.");
			WriteTinyAudio(context, path, id, 92U); context.Expect(resources.ReloadAudioClip(id), "Later audio reload must own the current generation.");
			resources.SetRequestTestHooks({}); gate.Release(); const auto current = resources.GetAudioClip(id);
			context.Expect(!oldReload.get() && initial && current && initial->EncodedPayload().front() == 17U && current->EncodedPayload().front() == 92U
				&& current->Generation() > initial->Generation(), "Audio leases and newer authority must survive old completion.");
			WriteBytes(path, std::vector<std::uint8_t>{0U}); context.Expect(!resources.ReloadAudioClip(id) && resources.GetAudioClip(id) == current,
				"Malformed replacement must preserve current immutable clip ownership.");
		});

		registry.Add("Engine.ResourceAsyncLifecycle.Publication.TextureShutdownDrainsPhysicalReload", [](TestContext& context) {
			Correctness::DisposableFixturePath fixture; PublicationPoolSession session;
			auto& resources = ResourceManager::Instance(); const auto path = fixture.Child("shutdown.tetf");
			WriteTinyTexture(path, 17U); const auto id = resources.LoadTexture(path);
			Correctness::ControlledGate gate; ResourceManager::RequestTestHooks hooks;
			hooks.beforePublication = [wait = gate.MakeWaiter(std::chrono::seconds(30))](const std::string&) { (void)wait(); };
			resources.SetRequestTestHooks(std::move(hooks)); const auto reload = resources.ReloadTexture(id);
			context.Expect(gate.WaitForArrivals(1U), "Shutdown fixture must retain an actual decoded texture producer.");
			auto drain = std::async(std::launch::async, [&] { resources.StopAdmissionsAndDrain(); });
			context.Expect(reload.wait_for(Correctness::kDefaultTimeout) == std::future_status::ready,
				"Admission shutdown must terminalize the compatibility receipt before waiting for physical work.");
			context.Expect(drain.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout,
				"Shutdown must still retain and wait for the held physical producer.");
			const auto generation = resources.GetRendererResourcePublicationGeneration();
			resources.SetRequestTestHooks({}); gate.Release(); drain.get();
			context.Expect(LoadedRed(context, resources, id) == 17U && resources.GetRendererResourcePublicationGeneration() == generation,
				"Closed ownership must prevent held reload publication while retaining last-good cache content.");
			context.Expect(resources.LoadTextureAsync(fixture.Child("closed.tetf")).get() == TerraEngine::EMPTY_UUID,
				"Stopping producers must leave admission closed until explicit new-session initialization.");
			resources.BeginRequestSession();
			const auto nextOwnerPath = fixture.Child("next-owner.tetf");
			WriteTinyTexture(nextOwnerPath, 92U);
			const auto nextOwnerId = resources.LoadTexture(nextOwnerPath);
			context.Expect(nextOwnerId != TerraEngine::EMPTY_UUID && LoadedRed(context, resources, nextOwnerId) == 92U,
				"An explicit new owner after shutdown must serve a later synchronous consumer in the same process.");
		});

	}
}
