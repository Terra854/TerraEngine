#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

#include <string>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryDiagnosticsTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Diagnostics.SeparatesFrustumConeAndHiZCullReasons", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryHiZDiagnosticsInput input{};
			input.viewType = RenderViewType::Game;
			input.frameId = 91u;
			input.qualityRequested = true;
			input.historyValid = false;
			input.hiZActive = false;
			input.invalidReason = "projection-changed";
			input.culledByFrustum = 3u;
			input.culledByCone = 2u;
			input.culledByHiZ = 1u;
			input.falseOcclusionRiskWarnings = 1u;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryHiZDiagnostics(input);

			context.ExpectEqual(3u, snapshot.culledByFrustum, "Frustum cull counts should stay separate from cone and Hi-Z counts.");
			context.ExpectEqual(2u, snapshot.culledByCone, "Cone cull counts should stay separate from frustum and Hi-Z counts.");
			context.ExpectEqual(1u, snapshot.culledByHiZ, "Hi-Z cull counts should stay separate from geometric cull counts.");
			context.ExpectEqual(6u, snapshot.culledMeshlets, "Total culled meshlets should sum the separated cull domains.");
			context.Expect(!snapshot.hiZActive, "Inactive Hi-Z diagnostics should mark Hi-Z inactive.");
			context.ExpectEqual(static_cast<std::uint32_t>(1u), snapshot.hiZInactiveFrames, "Inactive Hi-Z should increment the inactive-frame counter.");
			context.Expect(snapshot.hiZInvalidReason.find("projection-changed") != std::string::npos, "Hi-Z diagnostics should preserve the invalid reason.");
			context.ExpectEqual(1u, snapshot.hiZFalseOcclusionRiskWarnings, "False-occlusion risk should have its own counter.");
		});

		registry.Add("VirtualGeometry.Diagnostics.KeepsSelectionDispatchAndResidencyCountersConsistent", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.detailLevels = {
				{ 0u, 0u, 6u, 300u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(10u);
			GeometryPageRuntime fallback{};
			fallback.pageId = 0u;
			fallback.state = GeometryPageState::Resident;
			fallback.byteSize = 256u;
			fallback.fallbackPage = true;
			GeometryPageRuntime requested{};
			requested.pageId = 1u;
			requested.state = GeometryPageState::Loading;
			requested.byteSize = 512u;
			asset.pages = { fallback, requested };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.geometryBudgetBytes = 512u;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 101u, { asset }, quality);

			context.ExpectEqual(10u, snapshot.sourceMeshlets, "Source meshlet counter should reflect cullable groups.");
			context.ExpectEqual(6u, snapshot.selectedMeshlets, "Selected meshlet counter should reflect the selected fallback detail.");
			context.ExpectEqual(snapshot.selectedMeshlets, snapshot.dispatchedMeshlets, "CPU diagnostics should dispatch the selected meshlet count before GPU cull readback is available.");
			context.ExpectEqual(1u, snapshot.residentPages, "Residency diagnostics should count resident fallback pages.");
			context.ExpectEqual(1u, snapshot.requestedPages, "Residency diagnostics should count loading/requested high-detail pages.");
			context.Expect(snapshot.budgetPressureBytes > 0u, "Budget pressure should be visible when estimated bytes exceed the geometry budget.");
			context.Expect(!snapshot.budgetContributors.empty(), "Diagnostics should include top budget contributors.");
		});

		registry.Add("VirtualGeometry.Diagnostics.InactiveReasonsRemainExplicit", [](TestContext& context) {
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildInactiveVirtualGeometryDiagnostics(RenderViewType::Scene, 33u, VirtualGeometryInactiveReason::UnsupportedView);

			context.Expect(!snapshot.active, "Inactive diagnostics should mark the view inactive.");
			context.Expect(snapshot.inactiveReason == VirtualGeometryInactiveReason::UnsupportedView, "Inactive diagnostics should preserve the exact reason.");
			context.Expect(std::string(ToString(snapshot.inactiveReason)).find("Unsupported") != std::string::npos, "Inactive reason should have a user-facing label.");
			context.Expect(!snapshot.warnings.empty(), "Inactive diagnostics should include a warning explaining fallback rendering.");
		});

		registry.Add("VirtualGeometry.Diagnostics.PublishesSchedulerBoundaryMetadata", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary{};
			summary.detailed = true;
			std::vector<TerraEngine::Renderer::VirtualGeometrySchedulerDiagnostics> schedulers;
			schedulers.push_back(TerraEngine::Renderer::BuildVirtualGeometrySchedulerDiagnostics(
				"RenderSystem",
				"Render",
				{ "VirtualGeometryQualityProfile", "VirtualGeometryResidencySnapshots" },
				{ "VirtualGeometryDiagnostics" },
				"Immutable render snapshots and virtual geometry LOD/page preparation.",
				"Protected RHI submission and diagnostics publication.",
				"SceneMutationQueue flush before render snapshots become visible."));

			TerraEngine::Renderer::ApplyVirtualGeometryDiagnostics(summary, {}, std::move(schedulers));

			context.ExpectEqual(static_cast<std::size_t>(1), summary.virtualGeometrySchedulers.size(), "Virtual geometry diagnostics should publish one scheduler boundary record.");
			const TerraEngine::Renderer::VirtualGeometrySchedulerDiagnostics& scheduler = summary.virtualGeometrySchedulers.front();
			context.ExpectEqual(std::string("RenderSystem"), scheduler.owningSystem, "Scheduler diagnostics should name the owning system.");
			context.ExpectEqual(std::string("Render"), scheduler.schedulerPhase, "Scheduler diagnostics should name the frame phase.");
			context.Expect(!scheduler.readResources.empty(), "Scheduler diagnostics should expose read resources.");
			context.Expect(!scheduler.writeResources.empty(), "Scheduler diagnostics should expose write resources.");
			context.Expect(scheduler.workerSafePreparation.find("LOD/page") != std::string::npos, "Scheduler diagnostics should describe worker-safe virtual geometry preparation.");
			context.Expect(scheduler.protectedBoundary.find("Protected") != std::string::npos, "Scheduler diagnostics should describe protected work.");
			context.Expect(scheduler.visibilityBoundary.find("SceneMutationQueue") != std::string::npos, "Scheduler diagnostics should describe the visibility boundary.");
		});
	}
}
