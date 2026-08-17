// TerrainTransactionStress.cpp -- Real typed terrain publication stress.
//
// Workers prepare transactions against one immutable root observation and
// enqueue them through the production TerrainMutationQueue. The protected
// TerrainPublication path validates conflicts, injected failures, and
// copy-on-write root replacement. Failed batches must leave the root identity
// unchanged.

#include "Terrain/TerrainAssetStore.h"
#include "Terrain/TerrainMutationQueue.h"
#include "Terrain/TerrainPublication.h"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	using namespace TerraEngine::Terrain;

	std::shared_ptr<const TerrainAssetRoot> MakeInitialRoot()
	{
		auto root = std::make_shared<TerrainAssetRoot>();
		root->assetUuid = TerraEngine::GenerateUUID();
		root->publicationGeneration = 1u;
		root->rootContentHash = {1u, 2u};
		root->authoredBounds = {0, 0, 256, 256};
		TerrainDirectoryShard shard{};
		shard.areaBounds = {0, 0, 1, 1};
		shard.identity = {3u, 4u};
		root->directoryShards.push_back(std::move(shard));
		return root;
	}

	TerrainEditTransaction MakeTransaction(const TerrainRootCapture& capture, std::uint32_t worker, std::uint32_t round, std::uint32_t seed)
	{
		TerrainEditTransaction transaction{};
		transaction.transactionId = TerraEngine::GenerateUUID();
		transaction.origin = TerrainEditOrigin::Test;
		transaction.targetAssetUuid = capture.root->assetUuid;
		transaction.historyGroup = "TerrainTransactionStress";
		transaction.inputRootHash = capture.root->rootContentHash;
		transaction.affectedBounds = {0, 0, 256, 256};
		transaction.budget.maximumAreas = 1u;
		const TerrainAreaCoordinate coordinate{0, 0};
		TerrainAreaDescriptor current{};
		current.coordinate = coordinate;
		if (!capture.root->directoryShards.empty())
		{
			const auto found = capture.root->directoryShards.front().descriptors.find(coordinate);
			if (found != capture.root->directoryShards.front().descriptors.end()) current = found->second;
			else current.editGeneration = capture.root->publicationGeneration;
		}
		transaction.readSet.push_back({coordinate, current.editGeneration, current.authoredContentHash});
		TerrainPatch patch{};
		patch.kind = TerrainPatchKind::ReplaceDescriptorMetadata;
		patch.replacement.coordinate = coordinate;
		patch.replacement.editGeneration = current.editGeneration + 1u;
		patch.replacement.authoredContentHash = {
			static_cast<std::uint64_t>(worker + 5u) ^ seed,
			static_cast<std::uint64_t>(round + 11u) ^ (static_cast<std::uint64_t>(seed) << 32U)};
		patch.replacement.minHeightMm = static_cast<std::int32_t>(worker);
		patch.replacement.maxHeightMm = static_cast<std::int32_t>(worker + 1u);
		transaction.patches.push_back(std::move(patch));
		return transaction;
	}
}

int main(int argc, char** argv)
{
	std::uint32_t workers = 4u;
	std::uint32_t rounds = 250u;
	std::uint32_t seed = 0x7e11a9u;
	for (int index = 1; index + 1 < argc; index += 2)
	{
		const std::string option(argv[index]);
		const auto value = static_cast<std::uint32_t>(std::stoul(argv[index + 1]));
		if (option == "--workers") workers = value;
		else if (option == "--rounds" || option == "--iterations") rounds = value;
		else if (option == "--seed") seed = value;
	}
	if (workers == 0u || workers > 64u || rounds == 0u) return 2;

	TerrainAssetStore store(MakeInitialRoot());
	// The bounded ring keeps one slot empty to distinguish full from empty. Give
	// every producer a deterministic admission slot plus explicit headroom so the
	// stress measures publication conflicts, not scheduler timing at ingress.
	TerrainMutationQueue queue(static_cast<std::size_t>(workers) * 2U + 8U);
	TerrainPublication publication(store);
	std::uint64_t committed = 0u;
	std::uint64_t rejected = 0u;
	std::uint64_t injectedFailures = 0u;
	std::atomic<std::uint64_t> admissionFailures{0u};
	bool valid = true;
	for (std::uint32_t round = 0u; round < rounds && valid; ++round)
	{
		const TerrainRootCapture before = store.CaptureRoot();
		std::vector<std::thread> producers;
		producers.reserve(workers);
		for (std::uint32_t worker = 0u; worker < workers; ++worker)
		{
			producers.emplace_back([&, worker, round, before] {
				if(!queue.Enqueue(MakeTransaction(before, worker, round, seed)).Succeeded())
					admissionFailures.fetch_add(1u, std::memory_order_relaxed);
			});
		}
		for (auto& producer : producers) producer.join();

		const bool injectFailure = (round % 7u) == 0u;
		publication.SetInjectFailureForTests(injectFailure);
		if (injectFailure) ++injectedFailures;
		const auto receipts = publication.PublishQueued(queue);
		std::uint64_t roundCommits = 0u;
		for (const auto& receipt : receipts)
		{
			if (receipt.outcome == TerrainEditOutcome::Committed) ++roundCommits;
			else ++rejected;
		}
		committed += roundCommits;
		const TerrainRootCapture after = store.CaptureRoot();
		if (injectFailure)
			valid = after.root->rootContentHash == before.root->rootContentHash && after.publicationGeneration == before.publicationGeneration;
		else
			valid = roundCommits <= 1u && after.publicationGeneration == before.publicationGeneration + roundCommits;
	}
	const auto finalRoot = store.CaptureRoot();
	valid = valid && admissionFailures.load(std::memory_order_relaxed) == 0u;
	std::cout << "terrain_transaction_stress workers=" << workers << " rounds=" << rounds
		<< " committed=" << committed << " rejected=" << rejected
		<< " injected_failures=" << injectedFailures
		<< " admission_failures=" << admissionFailures.load(std::memory_order_relaxed)
		<< " generation=" << finalRoot.publicationGeneration
		<< " valid=" << (valid ? "true" : "false") << '\n';
	return valid ? 0 : 1;
}
