// TerrainAutomationDiagnosticTests.cpp -- Immutable automation diagnostic facts.

#include "TestHarness.h"

#include "Terrain/TerrainDiagnostics.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterTerrainAutomationDiagnosticTests(TestRegistry& registry)
	{
		registry.Add("Terrain.AutomationDiagnostics.ReportsActionableStaleConflictAndPercentiles", [](TestContext& context) {
			using namespace TerraEngine::Terrain;
			TerrainDiagnosticBuildInput input; input.snapshot.revision = 9u; input.snapshot.authoredGeneration = 9u; input.snapshot.savedGeneration = 7u; input.snapshot.visibleGeneration = 8u; input.snapshot.renderGeneration = 8u; input.snapshot.controlGeneration = 8u; input.snapshot.collisionGeneration = 6u; input.snapshot.dirty = true; input.snapshot.historyDepth = 3u; input.snapshot.historyBytes = 8192u; input.snapshot.residency.push_back({{-2, 7}, 4096u, true}); input.snapshot.conflicts.push_back({TerraEngine::GenerateUUID(), TerrainResultCode::Conflict, "Observed generation changed."});
			input.snapshot.resourcePressure.push_back({"Terrain GPU pages", 120u, 100u, true}); input.snapshot.layerFallbacks.push_back({4u, TerraEngine::GenerateUUID(), TerrainLayerFallbackState::MissingSource, "Layer source is unavailable."}); input.snapshot.seams.push_back({{-2, 7}, {-1, 7}, false, "Neighbour cook generation differs."});
			TerrainCollisionRecord collision; collision.coordinate = {-2, 7}; collision.status = TerrainCollisionStatus::Stale; collision.sourceRootHash = {3u, 4u}; collision.sourcePublicationGeneration = 8u; collision.lastKnownGoodActive = true; collision.message = "Cook observed an older root."; input.snapshot.collisions.push_back(collision);
			TerrainCollisionRecord unavailable; unavailable.coordinate = {12, -4}; unavailable.status = TerrainCollisionStatus::Unavailable; unavailable.message = "Collision area is not resident."; input.snapshot.collisions.push_back(unavailable);
			input.snapshot.jobs.push_back({TerraEngine::GenerateUUID(), TerrainDerivedStatus::RejectedStale, 11u}); input.stageSamples.push_back({"TerrainCook", {10u, 20u, 30u, 40u, 50u}});
			const TerrainDiagnosticSnapshot snapshot = BuildTerrainDiagnosticSnapshot(std::move(input));
			const bool hasConflict = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Conflict && item.action == TerrainDiagnosticAction::ResolveConflict; });
			const bool hasCollision = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Collision && item.location == TerrainAreaCoordinate{-2, 7} && item.impact == TerrainDiagnosticImpact::LastKnownGoodActive && item.action == TerrainDiagnosticAction::Rebuild; });
			const bool hasPressure = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Residency && item.severity == TerrainDiagnosticSeverity::Error && item.impact == TerrainDiagnosticImpact::Unavailable && item.action == TerrainDiagnosticAction::IncreaseBudget; });
			const bool hasUnavailable = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Collision && item.location == TerrainAreaCoordinate{12, -4} && item.severity == TerrainDiagnosticSeverity::Error && item.impact == TerrainDiagnosticImpact::Unavailable && item.action == TerrainDiagnosticAction::Retry; });
			const bool hasSource = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Source && item.action == TerrainDiagnosticAction::ReloadSource; });
			const bool hasSeam = std::any_of(snapshot.items.begin(), snapshot.items.end(), [](const TerrainDiagnosticItem& item) { return item.category == TerrainDiagnosticCategory::Seam && item.location == TerrainAreaCoordinate{-2, 7} && item.action == TerrainDiagnosticAction::Rebuild; });
			context.Expect(snapshot.revision == 9u && snapshot.authoredGeneration == 9u && snapshot.savedGeneration == 7u && snapshot.visibleGeneration == 8u && snapshot.renderGeneration == 8u && snapshot.controlGeneration == 8u && snapshot.collisionGeneration == 6u && snapshot.dirty && snapshot.historyDepth == 3u && snapshot.historyBytes == 8192u && snapshot.residency[0].residentBytes == 4096u && hasConflict && hasCollision && hasPressure && hasUnavailable && hasSource && hasSeam && snapshot.stagePercentiles.size() == 1u && snapshot.stagePercentiles[0].p50Nanoseconds == 30u && snapshot.stagePercentiles[0].p95Nanoseconds == 50u,
				"Automation diagnostics must preserve supplied generation/history/residency facts, expose actionable pressure/source/seam/stale states, and calculate percentiles only from reported stage samples.");
		});
	}
}
