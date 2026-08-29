// TerrainMultithreadedStressTests.cpp -- Typed terrain boundary thrash checks.

#include "TestHarness.h"

#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Renderer/Terrain/TerrainLodSelection.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Resource/VirtualGeometryPagePublication.h"
#include "Terrain/TerrainMutationQueue.h"
#include "Terrain/TerrainResize.h"
#include "Terrain/TerrainStreamingSystem.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		constexpr std::uint32_t kTerrainHaloExtent = 258u;
		constexpr std::size_t kTerrainHaloSampleCount =
			static_cast<std::size_t>(kTerrainHaloExtent) * kTerrainHaloExtent;
		constexpr std::size_t kConcurrentPageCount = 4u;

		Resource::VirtualGeometryPagePublicationCandidate MakeCompletePageCandidate(
			const TerraEngine::UUID& assetId,
			std::uint32_t pageId,
			std::uint64_t publicationGeneration,
			std::uint64_t allocationId)
		{
			Resource::VirtualGeometryPagePublicationCandidate candidate;
			candidate.candidateId = 1000u + publicationGeneration * 100u + pageId;
			candidate.publicationGeneration = publicationGeneration;
			candidate.key.asset.assetId = assetId;
			candidate.key.asset.sourceHash = 11u;
			candidate.key.asset.buildRecipeHash = 22u;
			candidate.key.pageId = pageId;
			candidate.key.generations = {
				publicationGeneration,
				publicationGeneration,
				publicationGeneration,
				publicationGeneration,
				publicationGeneration
			};
			candidate.key.pageContentGeneration = publicationGeneration;
			candidate.payload = {64u, 128u, 0xabcdu + pageId, true, true};
			candidate.hierarchy = {{pageId}, 0u, true, true};
			candidate.descriptor = {allocationId, pageId, pageId, true};
			candidate.dependenciesVerified = true;
			candidate.residencyTransitionVerified = true;
			return candidate;
		}

		// Publication snapshots are copied values. Matching their visibility-critical
		// fields proves a rejected batch did not leak a partial descriptor or generation.
		bool SameVisiblePages(
			const std::vector<Resource::VirtualGeometryPagePublication>& expected,
			const std::vector<Resource::VirtualGeometryPagePublication>& actual)
		{
			if(expected.size() != actual.size()) return false;
			for(std::size_t index = 0u; index < expected.size(); ++index)
			{
				const auto& left = expected[index];
				const auto& right = actual[index];
				if(left.candidateId != right.candidateId
					|| left.publicationGeneration != right.publicationGeneration
					|| left.key != right.key
					|| left.descriptor.allocationId != right.descriptor.allocationId
					|| left.descriptor.descriptorIndex != right.descriptor.descriptorIndex
					|| left.state != right.state
					|| left.publicationFrame != right.publicationFrame) return false;
			}
			return true;
		}

		std::vector<Renderer::Terrain::TerrainPageResidencyEntry> BuildSelectionSnapshot(
			const std::vector<Resource::VirtualGeometryPagePublication>& pages)
		{
			std::vector<Renderer::Terrain::TerrainPageResidencyEntry> entries;
			entries.reserve(pages.size());
			for(const auto& page : pages)
			{
				Renderer::Terrain::TerrainPageResidencyEntry entry;
				entry.assetId = page.key.asset.assetId;
				entry.area = {static_cast<std::int64_t>(page.key.pageId), 0};
				entry.pageId = page.key.pageId;
				entry.state = page.state;
				entry.uploadCompleted = page.descriptor.uploadCompleted;
				entry.published = page.IsVisible();
				entry.pinnedCoarseCoverage = true;
				entries.push_back(entry);
			}
			return entries;
		}

		Terrain::ContentHash HashExpandedSamples(
			const Terrain::TerrainAssetRoot& root,
			const Terrain::TerrainResizeDelta& delta,
			std::size_t workerCount)
		{
			const std::int64_t width = delta.proposedBounds.maxXExclusive - delta.proposedBounds.minX;
			const std::int64_t depth = delta.proposedBounds.maxZExclusive - delta.proposedBounds.minZ;
			std::vector<std::int32_t> values(static_cast<std::size_t>(width * depth), root.defaultHeightMm);
			const auto source = [](Terrain::TerrainSampleCoordinate sample) -> std::optional<std::int32_t> {
				return static_cast<std::int32_t>(12'000 + sample.x * 31 + sample.z * 17);
			};
			workerCount = std::max<std::size_t>(1u, workerCount);
			std::vector<std::jthread> workers;
			workers.reserve(workerCount);
			for(std::size_t worker = 0u; worker < workerCount; ++worker)
			{
				workers.emplace_back([&, worker] {
					// Static index partitioning varies the scheduling width but retains one
					// canonical output slot per global sample coordinate.
					for(std::size_t index = worker; index < values.size(); index += workerCount)
					{
						const Terrain::TerrainSampleCoordinate sample{
							delta.proposedBounds.minX + static_cast<std::int64_t>(index % static_cast<std::size_t>(width)),
							delta.proposedBounds.minZ + static_cast<std::int64_t>(index / static_cast<std::size_t>(width))};
						values[index] = delta.previousBounds.Contains(sample)
							? source(sample).value_or(root.defaultHeightMm)
							: Terrain::InitializeExpandedTerrainHeight(root, delta, sample, source);
					}
				});
			}
			workers.clear();
			std::vector<std::uint8_t> bytes(values.size() * sizeof(std::int32_t));
			std::memcpy(bytes.data(), values.data(), bytes.size());
			return Terrain::HashTerrainBytes(bytes);
		}
	}

	void RegisterTerrainMultithreadedStressTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Stress.ResizeShapeMatrixRemainsBoundedAndResponsive", [](TestContext& context) {
			const auto started = std::chrono::steady_clock::now();
			bool valid = true;
			for(std::int64_t extent = 1; extent <= 8; ++extent)
			{
				const Terrain::TerrainSampleBounds square{-extent * 256, -extent * 256, 0, 0};
				const auto areas = Terrain::EnumerateTerrainAreas(square, 64u);
				valid = valid && areas && areas->size() == static_cast<std::size_t>(extent * extent)
					&& areas->front() == Terrain::TerrainAreaCoordinate{-extent, -extent}
					&& areas->back() == Terrain::TerrainAreaCoordinate{-1, -1};
			}
			const Terrain::TerrainSampleBounds longNarrow{-2048, -3, 2048, 260};
			const auto longAreas = Terrain::EnumerateTerrainAreas(longNarrow, 128u);
			const auto negativeTail = Terrain::TerrainActiveRectForArea(longNarrow, {-8, -1});
			const auto positiveTail = Terrain::TerrainActiveRectForArea(longNarrow, {7, 1});
			const auto leftOwner = Terrain::OwnerArea({-1, 0});
			const auto rightOwner = Terrain::OwnerArea({0, 0});
			const auto elapsed = std::chrono::steady_clock::now() - started;
			context.Expect(valid && longAreas && longAreas->size() == 48u
				&& negativeTail == Terrain::TerrainAreaActiveRect{0u, 253u, 256u, 256u}
				&& positiveTail == Terrain::TerrainAreaActiveRect{0u, 0u, 256u, 4u}
				&& leftOwner == Terrain::TerrainAreaCoordinate{-1, 0}
				&& rightOwner == Terrain::TerrainAreaCoordinate{0, 0}
				&& elapsed < std::chrono::seconds(2),
				"The 1x1 through 8x8, long-narrow, signed, partial-tail, and cross-boundary matrix must enumerate within a bounded interactive interval.");
		});

		registry.Add("Terrain.Stress.ResizeExpansionHashesAreWorkerCountIndependent", [](TestContext& context) {
			Terrain::TerrainAssetRoot root{};
			root.authoredBounds = {0, 0, 64, 64};
			root.defaultHeightMm = -250;
			root.sampleSpacingMetres = 1.0;
			const std::array<Terrain::TerrainSampleBounds, 3u> proposals{{
				{0, 0, 96, 64}, {-32, 0, 96, 64}, {-32, -32, 96, 96}}};
			const std::size_t maximumWorkers = std::max<std::size_t>(4u,
				std::min<std::size_t>(16u, std::thread::hardware_concurrency()));
			const std::array<std::size_t, 4u> workerCounts{{1u, 2u, 4u, maximumWorkers}};
			bool deterministic = true;
			for(const auto proposed : proposals)
			{
				Terrain::TerrainResizeDelta delta{};
				delta.kind = Terrain::TerrainResizeKind::Expand;
				delta.previousBounds = root.authoredBounds;
				delta.proposedBounds = proposed;
				delta.expansionPolicy = Terrain::TerrainExpansionPolicy::EdgeBlend;
				delta.transitionSamples = 24u;
				const auto expected = HashExpandedSamples(root, delta, workerCounts.front());
				for(const std::size_t count : workerCounts)
					deterministic = deterministic && HashExpandedSamples(root, delta, count) == expected;
			}
			context.Expect(deterministic,
				"One-side, opposite-side, and four-side Edge Blend hashes must match at 1, 2, 4, and maximum workers.");
		});

		registry.Add("Terrain.Stress.BoundedTypedIngressAndCancellationRemainAtomic", [](TestContext& context) {
			Terrain::TerrainMutationQueue queue(2u);
			std::atomic_uint accepted{0u};
			std::vector<std::jthread> workers;
			for(std::uint32_t index = 0u; index < 8u; ++index)
			{
				workers.emplace_back([&] {
					Terrain::TerrainEditTransaction transaction;
					transaction.transactionId = TerraEngine::GenerateUUID();
					if(queue.Enqueue(std::move(transaction)).Succeeded()) ++accepted;
				});
			}
			workers.clear();

			std::atomic_bool cancelled = true;
			Terrain::TerrainResidencyBudget budget{};
			const auto cancelledPlan = Terrain::PrepareTerrainResidencyPlan(4u, {}, {}, budget, &cancelled);
			const auto declarations = Terrain::TerrainStreamingSystem::Instance().SchedulerDeclaration();
			context.Expect(accepted <= 2u && queue.PendingCount() == accepted
				&& cancelledPlan.cancelled && !declarations.reads.empty() && !declarations.writes.empty(),
				"Concurrent typed terrain ingress must remain bounded, cancelled streaming must not publish, and the streaming system must declare its scheduler resources.");
		});

		registry.Add("Terrain.Stress.CookAndCollisionCandidatesAreTypedAndCancellable", [](TestContext& context) {
			// A terrain cook owns 256 cells but requires one normal-only halo sample
			// around the complete 257-by-257 owned vertex grid.
			std::vector<std::int32_t> heights(kTerrainHaloSampleCount, 17);
			Renderer::Terrain::TerrainCookInput input{};
			input.assetId = TerraEngine::GenerateUUID();
			input.authoredHash = {7u, 8u};
			input.authoredGeneration = 2u;
			input.heightMillimetres = heights;
			const auto cook = Renderer::Terrain::CookTerrainPage(input);
			std::atomic_bool cancelled = true;
			Physics::TerrainHeightFieldRecipe recipe{};
			recipe.terrainAssetId = input.assetId;
			recipe.authoredGeneration = 2u;
			recipe.authoredHash = 5u;
			recipe.heightMillimetres.assign(Physics::TerrainHeightFieldRecipe::kSampleCount, 0);
			recipe.holeBits.assign(Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			const auto candidate = Physics::Terrain::PrepareTerrainCollisionCandidate(
				std::move(recipe), input.authoredHash, 2u, &cancelled);
			context.Expect(cook.complete && candidate.complete && candidate.cancellation->load(),
				"Cook and collision preparation must use immutable typed values; cancellation is observed before a protected collision publication.");
		});

		registry.Add("Terrain.Stress.ConcurrentCookUploadEvictSelectRollsBackUntilProtectedVisibility", [](TestContext& context) {
			const auto assetId = TerraEngine::GenerateUUID();
			Resource::VirtualGeometryPagePublicationStore publicationStore;
			std::vector<Resource::VirtualGeometryPagePublicationCandidate> initialCandidates;
			initialCandidates.reserve(kConcurrentPageCount);
			for(std::uint32_t pageId = 0u; pageId < kConcurrentPageCount; ++pageId)
				initialCandidates.push_back(MakeCompletePageCandidate(assetId, pageId, 1u, 100u + pageId));
			const auto initialPublication = publicationStore.PublishAtProtectedBoundary(initialCandidates, 1u);
			context.Expect(initialPublication.accepted && initialPublication.visible.size() == kConcurrentPageCount,
				"The fixture must begin with one complete protected page generation.");
			const auto priorVisible = publicationStore.Snapshot();

			// Each slot is written by exactly one worker. The barrier aligns the heavy
			// cooks so thread scheduling cannot accidentally turn this into a serial test.
			std::array<Renderer::Terrain::TerrainPageCookResult, kConcurrentPageCount> cooks;
			std::array<Resource::VirtualGeometryPagePublicationCandidate, kConcurrentPageCount> replacements;
			std::array<Renderer::Terrain::TerrainPageResolution, kConcurrentPageCount> priorSelections;
			const auto priorSelectionSnapshot = BuildSelectionSnapshot(priorVisible);
			std::barrier startGate(static_cast<std::ptrdiff_t>(kConcurrentPageCount));
			std::vector<std::jthread> workers;
			workers.reserve(kConcurrentPageCount);
			for(std::uint32_t pageId = 0u; pageId < kConcurrentPageCount; ++pageId)
			{
				workers.emplace_back([&, pageId] {
					std::vector<std::int32_t> halo(kTerrainHaloSampleCount,
						static_cast<std::int32_t>(pageId * 100));
					Renderer::Terrain::TerrainCookInput input{};
					input.assetId = assetId;
					input.area = {static_cast<std::int64_t>(pageId), 0};
					input.authoredHash = {90u + pageId, 180u + pageId};
					input.authoredGeneration = 2u;
					input.heightMillimetres = halo;
					startGate.arrive_and_wait();
					cooks[pageId] = Renderer::Terrain::CookTerrainPage(input);
					replacements[pageId] = MakeCompletePageCandidate(assetId, pageId, 2u, 200u + pageId);
					// Replacing a page retires only the allocation owned by that same
					// identity; cross-page retirement is rejected by the coordinator.
					replacements[pageId].retirements = {{100u + pageId, 50u}};
					Renderer::Terrain::TerrainPageSelectionRequest selection;
					selection.frameId = 2u;
					selection.viewId = pageId;
					selection.assetId = assetId;
					selection.area = {static_cast<std::int64_t>(pageId), 0};
					selection.desiredPageId = pageId;
					priorSelections[pageId] = Renderer::Terrain::ResolveTerrainOwnAncestorPage(
						selection, priorSelectionSnapshot);
				});
			}
			workers.clear();

			bool allWorkersComplete = true;
			for(std::size_t index = 0u; index < kConcurrentPageCount; ++index)
			{
				allWorkersComplete = allWorkersComplete && cooks[index].complete
					&& priorSelections[index].completeCoverage
					&& priorSelections[index].actualPageId == index;
			}
			context.Expect(allWorkersComplete && SameVisiblePages(priorVisible, publicationStore.Snapshot()),
				"Concurrent cooks and selectors may prepare private values, but no page generation becomes visible before the protected callback.");

			// One worker reports an unfinished upload and attempts to retire another
			// page's allocation. Either defect must reject every replacement atomically.
			replacements[1u].descriptor.uploadCompleted = false;
			replacements[1u].retirements = {{100u, 50u}};
			const std::vector<Resource::VirtualGeometryPagePublicationCandidate> rejectedCandidates(
				replacements.begin(), replacements.end());
			const auto rejected = publicationStore.PublishAtProtectedBoundary(rejectedCandidates, 2u);
			context.Expect(!rejected.accepted && !rejected.rejectionReason.empty()
				&& publicationStore.LastPublicationFrame() == 1u
				&& SameVisiblePages(priorVisible, publicationStore.Snapshot()),
				"An incomplete upload or conflicting eviction must roll back the whole protected batch and preserve last-known-good visibility.");

			// Completing the upload and restoring same-page retirement makes the exact
			// same private candidate generation eligible at the next declared boundary.
			replacements[1u].descriptor.uploadCompleted = true;
			replacements[1u].retirements = {{101u, 50u}};
			const std::vector<Resource::VirtualGeometryPagePublicationCandidate> acceptedCandidates(
				replacements.begin(), replacements.end());
			const auto accepted = publicationStore.PublishAtProtectedBoundary(acceptedCandidates, 3u);
			bool completeNewGeneration = accepted.accepted
				&& accepted.visible.size() == kConcurrentPageCount
				&& publicationStore.LastPublicationFrame() == 3u;
			for(const auto& page : accepted.visible)
				completeNewGeneration = completeNewGeneration && page.IsVisible()
					&& page.publicationGeneration == 2u && page.publicationFrame == 3u;
			const auto currentSelectionSnapshot = BuildSelectionSnapshot(accepted.visible);
			Renderer::Terrain::TerrainPageSelectionRequest currentSelection;
			currentSelection.frameId = 3u;
			currentSelection.assetId = assetId;
			currentSelection.area = {2, 0};
			currentSelection.desiredPageId = 2u;
			const auto currentResolution = Renderer::Terrain::ResolveTerrainOwnAncestorPage(
				currentSelection, currentSelectionSnapshot);
			context.Expect(completeNewGeneration && currentResolution.desiredResident
				&& currentResolution.actualPageId == 2u,
				"A valid protected boundary must expose the complete new generation together so selection never observes a mixed cut.");
		});
	}
}
