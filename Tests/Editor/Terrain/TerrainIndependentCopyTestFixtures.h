// Shared genuine durable terrain/scene fixture; no live writes occur on workers.
#pragma once
#include "TestHarness.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainIndependentCopyService.h"
#include "AssetDatabase.h"
#include "Resource/ResourceManager.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Object/ComponentRegistry.h"
#include "Components/TerrainComponent.h"
#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainLayerStack.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include "Utilities/UUID.h"
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <nlohmann/json.hpp>

namespace TerraEngine::EditorTests::TerrainCopyFixtures {
	namespace Copy = TerraEngine::Editor::Terrain;
	namespace Authored = TerraEngine::Terrain;
	inline std::string Read(const std::filesystem::path& path) { std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}}; }
	inline std::shared_ptr<Authored::TerrainAssetStore> Open(const std::filesystem::path& descriptor) {
		const auto data = std::filesystem::path(descriptor.string() + ".data");
		std::istringstream current(Read(data / "CURRENT")); std::string hash; current >> hash;
		const auto decoded = Authored::DecodeTerrainRootManifest(Read(data / "roots" / (hash + ".root")));
		if (!decoded.Succeeded() || !decoded.root) throw std::runtime_error("Fixture root failed to decode.");
		auto store = std::make_shared<Authored::TerrainAssetStore>(std::make_shared<const Authored::TerrainAssetRoot>(*decoded.root));
		if (!store->OpenPersistedRoot(data).Succeeded()) throw std::runtime_error("Fixture private root failed durable reopening.");
		return store;
	}
	struct Fixture {
		std::filesystem::path project = std::filesystem::temp_directory_path() / ("TerraIndependentTerrain-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
		std::filesystem::path source = project / "Assets" / "Authored.teterrain";
		TerraEngine::UUID sourceId = TerraEngine::GenerateUUID(), materialId = TerraEngine::GenerateUUID();
		ObjectManager& objects = ObjectManager::instance();
		Object selected, other;
		std::unique_ptr<Copy::TerrainIndependentCopyService> service;
		std::uint64_t epoch = 1U;
		bool rejectRegistration = false, rejectValidation = false, retryRollback = false, loseOwnership = false;
		unsigned registrations = 0U, rollbacks = 0U, finalizations = 0U;
		Fixture() {
			AssetDatabase::Instance().Shutdown(); TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
			ResourceManager::Instance().BeginRequestSession();
			ComponentRegistry::RegisterAllComponents(); objects.clear();
			if (ThreadPool::instance().getThreadCount() == 0U) ThreadPool::instance().initialize(2U);
			std::filesystem::create_directories(source.parent_path());
			auto root = std::make_shared<Authored::TerrainAssetRoot>(); root->assetUuid = sourceId;
			root->publicationGeneration = root->persistedRootGeneration = 1U; root->rootContentHash = {1U, 2U};
			root->authoredBounds = {0, 0, 256, 256}; root->sampleSpacingMetres = 2.5; root->defaultHeightMm = -13;
			root->defaultLayerId = 1U; root->layerStackGeneration = 3U;
			auto layers = std::make_shared<Authored::TerrainLayerStack>(); layers->generation = 3U;
			Authored::TerrainLayer layer; layer.stableId = 1U; layer.name = "Rock"; layer.materialAssetUuid = materialId;
			layer.mapping.tileSizeXMetres = 9.0; layer.mapping.offsetZMetres = 1.25; layer.mapping.mode = Authored::TerrainLayerMappingMode::Triplanar;
			layers->layers.push_back(layer); layer.stableId = 2U; layer.name = "Moss"; layer.mapping.tileSizeZMetres = 6.0; layers->layers.push_back(layer); root->layerStack = layers;
			Authored::TerrainAreaDescriptor area; area.coordinate = {0, 0}; area.editGeneration = 7U;
			area.minHeightMm = -100; area.maxHeightMm = 100; area.layerPresence = {1U, 2U}; area.authoredContentHash = {3U, 4U};
			std::vector<std::pair<Authored::ContentHash, std::vector<std::uint8_t>>> blobs;
			const auto encode = [&](Authored::TerrainChannelKind kind, unsigned group, std::vector<std::uint8_t> payload) {
				Authored::Format::TerrainAreaEnvelopeHeader header; header.assetUuid = sourceId; header.area = area.coordinate;
				header.channelKind = kind; header.weightGroup = static_cast<std::uint16_t>(group);
				Authored::EncodedTerrainArea encoded; if (!Authored::EncodeTerrainArea(header, payload, encoded).Succeeded()) throw std::runtime_error("Fixture channel encoding failed.");
				const auto hash = encoded.header.envelopeHash; blobs.emplace_back(hash, std::move(encoded.bytes)); return hash;
			};
			std::vector<std::uint8_t> heights(256U * 256U * 4U, 0U); heights[0] = 71U; heights[100] = 25U;
			area.channels.heightHash = encode(Authored::TerrainChannelKind::Height, 0U, std::move(heights));
			std::vector<std::uint8_t> holes(256U * 256U / 8U, 0U); holes[8] = 0x41U;
			area.channels.holeHash = encode(Authored::TerrainChannelKind::Holes, 0U, std::move(holes));
			const std::array<Authored::TerrainWeightContribution, 2U> contributions{{{1U, 175U}, {2U, 80U}}};
			std::vector<Authored::TerrainWeightSample> samples(256U * 256U, Authored::NormalizeTerrainWeights(contributions));
			std::vector<std::uint8_t> weights; if (!Authored::MaterializeTerrainWeightGroup(samples, weights).Succeeded()) throw std::runtime_error("Fixture weight encoding failed.");
			area.channels.weightGroupHashes[1U] = encode(Authored::TerrainChannelKind::WeightGroup, 1U, std::move(weights));
			Authored::TerrainDirectoryShard shard; shard.areaBounds = {0, 0, 1, 1}; shard.identity = {5U, 6U}; shard.descriptors.emplace(area.coordinate, area); root->directoryShards.push_back(shard);
			Authored::TerrainAssetStore store(root); for (auto& [hash, bytes] : blobs) if (!store.Blobs().PutVerified(hash, std::move(bytes))) throw std::runtime_error("Fixture blob verification failed.");
			if (!store.PersistAtomically({source.string() + ".data"}).Succeeded()) throw std::runtime_error("Fixture persistence failed.");
			nlohmann::json descriptor{{"format", "TerraEngineTerrain"}, {"rootFormat", "TERRAROOT1"}, {"version", 1U}, {"assetId", TerraEngine::ToHexString(sourceId)},
				{"width", 256U}, {"height", 256U}, {"sampleSpacingMetres", 2.5}, {"preservedCustomMetadata", {{"artist", "terrain-copy-test"}}}};
			std::ofstream(source) << descriptor.dump(4);
			AssetDatabase::Instance().Initialize(project); AssetDatabase::Instance().PauseWatcherForTests(); AssetDatabase::Instance().ImportAsset(source, true);
			selected = objects.createObject("Selected terrain"); other = objects.createObject("Other terrain");
			objects.AddComponent<TerrainComponent>(selected)->terrainAssetId = sourceId;
			objects.AddComponent<TerrainComponent>(other)->terrainAssetId = sourceId;
			Copy::TerrainCopyCallbacks callbacks;
			callbacks.identityAvailable = [](TerraEngine::UUID id) { return AssetDatabase::Instance().FindAssetByGuid(id) == nullptr; };
			callbacks.validateSource = [this](const auto& captured, std::string& error) {
				if (rejectValidation || captured.projectEpoch != epoch || captured.sourceAssetId != sourceId) { error = "Observed terrain source changed."; return false; }
				std::istringstream current(Read(source.string() + ".data/CURRENT")); std::string hash; current >> hash;
				return hash == Authored::ToHexString(captured.sourceRootHash);
			};
			callbacks.registerProvisional = [this](const auto&, const auto& ownership, std::string& error) {
				++registrations; if (rejectRegistration) { error = "Controlled terrain import rejection."; return false; }
				AssetDatabase::Instance().ImportAsset(ownership.descriptorPath, true);
				const auto* metadata = AssetDatabase::Instance().FindAssetByGuid(ownership.freshAssetId);
				return metadata && std::filesystem::absolute(AssetDatabase::Instance().GetAssetRoot() / metadata->sourcePath) == ownership.descriptorPath;
			};
			callbacks.finalizeProvisional = [this](const auto&) { ++finalizations; };
			callbacks.rollbackProvisional = [this](const auto& ownership) {
				++rollbacks;
				if (loseOwnership) return Copy::TerrainCopyCleanupResult{Copy::TerrainCopyCleanupDisposition::OwnershipLost, "Controlled ownership loss."};
				if (retryRollback) return Copy::TerrainCopyCleanupResult{Copy::TerrainCopyCleanupDisposition::Retry, "Controlled cleanup retry."};
				std::string error; const bool clean = AssetDatabase::Instance().UnregisterProvisionalTerrainCopy(ownership.descriptorPath, ownership.freshAssetId, error);
				return Copy::TerrainCopyCleanupResult{clean ? Copy::TerrainCopyCleanupDisposition::Complete : Copy::TerrainCopyCleanupDisposition::OwnershipLost, error};
			};
			service = std::make_unique<Copy::TerrainIndependentCopyService>(std::move(callbacks));
		}
		~Fixture() {
			if (service) { service->RetireProject(epoch); Flush(); (void)service->DrainAtShutdown(); service.reset(); }
			TerraEngine::Scene::SceneMutationQueue::Instance().Clear(); objects.clear(); AssetDatabase::Instance().Shutdown();
			std::error_code ignored; std::filesystem::remove_all(project, ignored);
		}
		Copy::TerrainCopyCapture Capture() const {
			TerraEngine::Scene::TransactionalMutationBuilder observation("terrain-copy-observation", TerraEngine::Scene::TransactionOrigin::Editor, "terrain");
			observation.ObserveScene(objects).ObserveObject(objects, objects.getUUID(selected));
			return {epoch, objects.getUUID(selected), sourceId, source, project / "Assets", project / ".terrain-copy-staging",
				Open(source)->CaptureRoot().root->rootContentHash, observation.Build().readSet, Read(source)};
		}
		bool Await(TerraEngine::UUID operation) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			do { service->PollPrepared(epoch); service->RetryCleanup(epoch); for (const auto& status : service->SnapshotStatus()) if (status.operationId == operation && status.state != Copy::TerrainCopyState::Preparing) return true; std::this_thread::yield(); }
			while (std::chrono::steady_clock::now() < deadline); return false;
		}
		Copy::TerrainCopyStatus Status(TerraEngine::UUID operation) const { for (const auto& status : service->SnapshotStatus()) if (status.operationId == operation) return status; throw std::runtime_error("Terrain copy status missing."); }
		void Flush() {
			(void)TerraEngine::Scene::SceneMutationQueue::Instance().DrainTransactions(objects);
			for (const auto& completion : TerraEngine::Scene::SceneMutationQueue::Instance().DrainCompletedEditorTransactions()) (void)service->HandleTransactionResult(completion.result);
		}
		std::filesystem::path CopyPath(TerraEngine::UUID operation) const {
			const auto* metadata = AssetDatabase::Instance().FindAssetByGuid(Status(operation).freshAssetId);
			return metadata ? project / "Assets" / metadata->sourcePath : std::filesystem::path{};
		}
	};
}
