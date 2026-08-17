// TerrainSchedulerContractTests.cpp — terrain worker and protected-boundary declarations.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Terrain/TerrainDerivedSystem.h"
#include "Terrain/TerrainStreamingSystem.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterTerrainSchedulerContractTests(TestRegistry& registry)
	{
		registry.Add("Terrain.Foundation.SchedulerDeclarationsExposeWorkerAndPublicationBoundaries", [](TestContext& context) {
			auto& streaming = TerraEngine::Terrain::TerrainStreamingSystem::Instance();
			auto& derived = TerraEngine::Terrain::TerrainDerivedSystem::Instance();
			streaming.Initialize();
			derived.Initialize();
			const auto streamingDeclaration = streaming.SchedulerDeclaration();
			const auto derivedDeclaration = derived.SchedulerDeclaration();
			context.Expect(streamingDeclaration.phase == "Render" && !streamingDeclaration.mainThread
				&& streamingDeclaration.before.end() != std::find(streamingDeclaration.before.begin(), streamingDeclaration.before.end(), "TerrainDerivedSystem"),
				"Terrain streaming must use the canonical Render phase and publish before derived terrain work.");
			context.Expect(derivedDeclaration.phase == "Render" && derivedDeclaration.before.end()
				!= std::find(derivedDeclaration.before.begin(), derivedDeclaration.before.end(), "RenderSystem"),
				"Terrain derived publication must precede RenderSystem capture.");
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			for(const char* required : { TerraEngine::Scheduling::kResourceTerrainAssetRoots,
				TerraEngine::Scheduling::kResourceTerrainPublishedResidency,
				TerraEngine::Scheduling::kResourceTerrainPublishedDerived,
				TerraEngine::Scheduling::kResourceTerrainPreparedCollision,
				TerraEngine::Scheduling::kResourceTerrainPublishedCollision,
				TerraEngine::Scheduling::kResourceTerrainDiagnostics }) {
				context.Expect(std::any_of(resources.begin(), resources.end(), [required](const auto& resource) {
					return resource.name == required;
				}), "Every terrain handoff must be visible in scheduler diagnostics.");
			}
			const auto workloads = derived.DeclareFrameWorkloads(0.0F);
			context.Expect(workloads.size() == 2U && workloads[0].workerEligible && !workloads[1].workerEligible
				&& workloads[1].sameFrameVisibilityRequired,
				"Terrain derived work must separate worker preparation from protected same-frame publication.");
			derived.Shutdown();
			streaming.Shutdown();
		});
	}
}
