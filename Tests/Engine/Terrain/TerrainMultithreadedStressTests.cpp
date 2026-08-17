// TerrainMultithreadedStressTests.cpp -- Typed terrain boundary thrash checks.

#include "TestHarness.h"

#include "Physics/Terrain/TerrainCollisionSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Terrain/TerrainMutationQueue.h"
#include "Terrain/TerrainStreamingSystem.h"

#include <atomic>
#include <thread>

namespace TerraEngine::Tests
{
	void RegisterTerrainMultithreadedStressTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Stress.BoundedTypedIngressAndCancellationRemainAtomic", [](TestContext& context) {
			Terrain::TerrainMutationQueue queue(2u); std::atomic_uint accepted{0u}; std::vector<std::jthread> workers;
			for(std::uint32_t index = 0u; index < 8u; ++index) workers.emplace_back([&] { Terrain::TerrainEditTransaction transaction; transaction.transactionId = TerraEngine::GenerateUUID(); if(queue.Enqueue(std::move(transaction)).Succeeded()) ++accepted; });
			workers.clear();
			std::atomic_bool cancelled = true; Terrain::TerrainResidencyBudget budget{};
			const auto cancelledPlan = Terrain::PrepareTerrainResidencyPlan(4u, {}, {}, budget, &cancelled);
			const auto declarations = Terrain::TerrainStreamingSystem::Instance().SchedulerDeclaration();
			context.Expect(accepted <= 2u && queue.PendingCount() == accepted && cancelledPlan.cancelled && !declarations.reads.empty() && !declarations.writes.empty(),
				"Concurrent typed terrain ingress must remain bounded, cancelled streaming must not publish, and the streaming system must declare its scheduler resources.");
		});

		registry.Add("Terrain.Stress.CookAndCollisionCandidatesAreTypedAndCancellable", [](TestContext& context) {
			std::vector<std::int32_t> heights(Terrain::kTerrainAreaSampleCount, 17); Renderer::Terrain::TerrainCookInput input{}; input.assetId = TerraEngine::GenerateUUID(); input.authoredHash = {7u, 8u}; input.authoredGeneration = 2u; input.heightMillimetres = heights;
			const auto cook = Renderer::Terrain::CookTerrainPage(input); std::atomic_bool cancelled = true;
			Physics::TerrainHeightFieldRecipe recipe{}; recipe.terrainAssetId = input.assetId; recipe.authoredGeneration = 2u; recipe.authoredHash = 5u; recipe.heightMillimetres.assign(Physics::TerrainHeightFieldRecipe::kSampleCount, 0); recipe.holeBits.assign(Physics::TerrainHeightFieldRecipe::kHoleByteCount, 0u);
			const auto candidate = Physics::Terrain::PrepareTerrainCollisionCandidate(std::move(recipe), input.authoredHash, 2u, &cancelled);
			context.Expect(cook.complete && candidate.complete && candidate.cancellation->load(), "Cook and collision preparation must use immutable typed values; cancellation is observed before a protected collision publication.");
		});
	}
}
