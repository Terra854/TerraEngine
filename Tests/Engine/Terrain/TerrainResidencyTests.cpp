// TerrainResidencyTests.cpp -- demand, protection, and stale publication checks.

#include "TestHarness.h"

#include "Terrain/TerrainStreamingSystem.h"
#include "Renderer/Terrain/TerrainPageCooker.h"
#include "Renderer/Terrain/TerrainRenderPackets.h"

#include <atomic>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterTerrainResidencyTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Residency.ProtectedDemandSurvivesBudgetAndStalePlansReject", [](TestContext& context) {
			Terrain::TerrainResidencySnapshot prior{};
			const auto asset = TerraEngine::GenerateUUID();
			Terrain::TerrainResidencyDemand edit{};
			edit.assetId = asset; edit.area = {0, 0}; edit.rootGeneration = 4u;
			edit.source = Terrain::TerrainDemandSource::ActiveEdit; edit.priority = 100u; edit.authoredBytes = 80u;
			Terrain::TerrainResidencyDemand scene{};
			scene.assetId = asset; scene.area = {1, 0}; scene.rootGeneration = 4u;
			scene.source = Terrain::TerrainDemandSource::SceneView; scene.priority = 1u; scene.authoredBytes = 80u;
			Terrain::TerrainResidencyBudget budget{};
			budget.authoredBytes = 100u; budget.renderBytes = budget.controlBytes = budget.collisionBytes = 1024u;
			const auto plan = Terrain::PrepareTerrainResidencyPlan(4u, prior, {edit, scene}, budget);
			context.Expect(plan.evictAreas.size() == 1u && plan.candidate.areas.size() == 1u && plan.candidate.areas.front().area == edit.area,
				"Budget pressure must evict ordinary detail before an active edit area.");
			Terrain::TerrainResidencySnapshot published{};
			context.Expect(!Terrain::PublishTerrainResidencyPlan(published, plan, 5u) && published.areas.empty(),
				"A plan prepared from a stale root must leave the previous residency snapshot untouched.");
			context.Expect(Terrain::PublishTerrainResidencyPlan(published, plan, 4u) && published.areas.size() == 1u,
				"A current immutable plan must publish all selected residency at one boundary.");
		});

		registry.Add("Terrain.Residency.AggregatesEveryDemandClassAndRetainsOnlyMatchingLkg", [](TestContext& context) {
			const auto asset = TerraEngine::GenerateUUID();
			Terrain::TerrainResidencyBudget budget{};
			budget.authoredBytes = 1'000u; budget.renderBytes = budget.controlBytes = budget.collisionBytes = 1'000u;
			std::vector<Terrain::TerrainResidencyDemand> demands;
			for (const auto source : {Terrain::TerrainDemandSource::SceneView, Terrain::TerrainDemandSource::GameView, Terrain::TerrainDemandSource::Shadow,
				Terrain::TerrainDemandSource::Reflection, Terrain::TerrainDemandSource::Capture, Terrain::TerrainDemandSource::Selection,
				Terrain::TerrainDemandSource::ActiveEdit, Terrain::TerrainDemandSource::Play, Terrain::TerrainDemandSource::ExplicitPin})
			{
				Terrain::TerrainResidencyDemand demand{};
				demand.assetId = asset; demand.area = {static_cast<std::int64_t>(demands.size()), 0}; demand.rootGeneration = 8u;
				demand.source = source; demand.priority = static_cast<std::uint32_t>(demands.size()); demand.authoredBytes = 10u;
				demands.push_back(demand);
			}
			Terrain::TerrainResidencyDemand duplicate = demands.front(); duplicate.priority = 99u; demands.push_back(duplicate);
			const auto plan = Terrain::PrepareTerrainResidencyPlan(8u, {}, demands, budget);
			const auto protectedCount = std::count_if(plan.candidate.areas.begin(), plan.candidate.areas.end(), [](const auto& area) { return area.protectedFromEviction; });
			context.Expect(plan.candidate.areas.size() == 9u && plan.candidate.coalescedDemands == 1u && protectedCount == 4u,
				"Scene, game, capture, shadow, reflection, selection, edit, play, and pin demand must coalesce while preserving all protected sources.");

			std::atomic_bool cancelled{true};
			const auto cancelledPlan = Terrain::PrepareTerrainResidencyPlan(8u, {}, demands, budget, &cancelled);
			context.Expect(cancelledPlan.cancelled, "Cancelled IO/planning work must not produce a publishable residency candidate.");

			std::vector<std::int32_t> heights(Terrain::kTerrainAreaSampleCount, 50);
			TerraEngine::Renderer::Terrain::TerrainCookInput input{};
			input.assetId = asset; input.authoredHash = {71u, 72u}; input.authoredGeneration = 8u; input.heightMillimetres = heights;
			const auto cook = TerraEngine::Renderer::Terrain::CookTerrainPage(input);
			TerraEngine::Renderer::Terrain::TerrainViewPacket view{};
			view.assetId = asset; view.authoredHash = input.authoredHash; view.authoredGeneration = 8u; view.pageSourceIdentity = cook.sourceIdentity; view.frameId = 1u;
			const auto fresh = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, cook, {});
			TerraEngine::Resource::VirtualGeometryPagePublication lkg{};
			lkg.candidateId = fresh->candidate.candidateId; lkg.publicationGeneration = fresh->candidate.publicationGeneration; lkg.key = fresh->candidate.key;
			lkg.payload = fresh->candidate.payload; lkg.hierarchy = fresh->candidate.hierarchy; lkg.descriptor = fresh->candidate.descriptor;
			lkg.state = TerraEngine::Resource::VirtualGeometryPagePublicationState::Resident; lkg.publicationFrame = 1u;
			const std::vector<TerraEngine::Resource::VirtualGeometryPagePublication> visible = {lkg};
			const auto fallback = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, {}, visible);
			view.authoredGeneration = 9u;
			const auto rejected = TerraEngine::Renderer::Terrain::PrepareTerrainRenderPacket(view, {}, visible);
			context.Expect(fallback.has_value() && fallback->usesLastKnownGood && !rejected.has_value(),
				"Last-known-good residency must cover only the exact authored generation, never stale IO output.");
		});
	}
}
