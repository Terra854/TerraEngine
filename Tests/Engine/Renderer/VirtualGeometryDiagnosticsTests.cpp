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
			input.historyValid = true;
			input.hiZActive = true;
			input.invalidReason = "projection-changed-diagnostic";
			input.ownerViewType = RenderViewType::Game;
			input.sameViewHistory = true;
			input.sameTargetHistory = true;
			input.projectionMatches = false;
			input.failOpen = false;
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
			context.Expect(snapshot.hiZActive, "Projection metadata should not mark Hi-Z inactive once the pyramid exists.");
			context.ExpectEqual(static_cast<std::uint32_t>(0u), snapshot.hiZInactiveFrames, "Diagnostic metadata should not increment the inactive-frame counter.");
			context.Expect(snapshot.hiZInvalidReason.empty(), "Active Hi-Z diagnostics should not publish an inactive reason.");
			context.ExpectEqual(1u, snapshot.hiZFalseOcclusionRiskWarnings, "False-occlusion risk should have its own counter.");
			context.Expect(!snapshot.hiZValidation.failOpen, "Active Hi-Z diagnostics should not publish fail-open validation state.");
			context.Expect(snapshot.hiZValidation.sameViewHistory, "Diagnostics should expose whether the history belongs to the same view.");
			context.Expect(snapshot.hiZValidation.sameTargetHistory, "Diagnostics should expose whether the history belongs to the same render target.");
			context.Expect(!snapshot.hiZValidation.projectionMatches, "Diagnostics should expose projection mismatch separately from target ownership.");
			context.Expect(snapshot.hiZValidation.inactiveReason.empty(), "Validation diagnostics should only carry inactive reasons for true missing-resource failures.");
		});

		registry.Add("VirtualGeometry.Diagnostics.PublishesHiZInactiveReasonAndFailOpenState", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryHiZDiagnosticsInput input{};
			input.viewType = RenderViewType::Scene;
			input.frameId = 92u;
			input.qualityRequested = true;
			input.historyValid = true;
			input.hiZActive = true;
			input.invalidReason = "thin-geometry-risk";
			input.ownerViewType = RenderViewType::Game;
			input.sameViewHistory = false;
			input.sameTargetHistory = false;
			input.projectionMatches = true;
			input.failOpen = false;
			input.falseOcclusionRiskWarnings = 2u;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryHiZDiagnostics(input);

			context.Expect(snapshot.hiZActive, "Thin-geometry metadata should remain diagnostic-only for always-on Hi-Z.");
			context.Expect(snapshot.hiZHistoryValid, "Fresh history and metadata diagnostics should be reported separately.");
			context.ExpectEqual(0u, snapshot.hiZInactiveFrames, "Diagnostic-only metadata should not increment the inactive frame counter.");
			context.ExpectEqual(2u, snapshot.hiZFalseOcclusionRiskWarnings, "Guard-driven false-occlusion risks should be counted.");
			context.Expect(snapshot.hiZValidation.ownerViewType == RenderViewType::Game, "Diagnostics should expose the pyramid owner view.");
			context.Expect(!snapshot.hiZValidation.sameViewHistory, "Scene view diagnostics should reveal that the camera history came from another view.");
			context.Expect(!snapshot.hiZValidation.sameTargetHistory, "Scene view diagnostics should reveal target mismatch separately.");
			context.Expect(snapshot.hiZValidation.projectionMatches, "Projection match should stay true when ownership is the only problem.");
			context.Expect(!snapshot.hiZValidation.failOpen, "Diagnostic-only guard metadata should not force fail-open state.");
			context.Expect(snapshot.hiZValidation.inactiveReason.empty(), "Inactive reason should stay empty while Hi-Z remains active.");
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

		registry.Add("AdaptiveVirtualGeometry.Diagnostics.DefaultProfileIsSafeAndExplainable", [](TestContext& context) {
			AdaptiveVirtualGeometryCookProfile profile = MakeDefaultAdaptiveVirtualGeometryCookProfile();
			profile.targetRatios[1] = 2.0f;
			profile.minimumUsefulTriangleReduction = -1.0f;

			const AdaptiveVirtualGeometryCookProfile sanitized = SanitizeAdaptiveVirtualGeometryCookProfile(profile);
			const TerraEngine::Renderer::VirtualGeometryAdaptiveSettingsDiagnostics settings =
				TerraEngine::Renderer::BuildVirtualGeometryAdaptiveSettingsDiagnostics(sanitized);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildDefaultAdaptiveVirtualGeometryDiagnostics(RenderViewType::Scene, 2027u, sanitized);

			context.ExpectEqual(kVirtualGeometryAdaptiveTargetCapacity, settings.requestedTargetCount, "Default adaptive virtual geometry should publish the full half-octave target list.");
			context.Expect(sanitized.targetRatios[1] <= sanitized.targetRatios[0], "Sanitization should keep target ratios monotonic.");
			context.Expect(settings.minimumUsefulTriangleReduction >= 0.0f, "Unsafe reduction thresholds should clamp to safe values.");
			context.Expect(snapshot.active, "Default adaptive diagnostics should be publishable before any cooked asset exists.");
			context.Expect(!snapshot.hiZValidation.failOpen, "Foundation diagnostics should not synthesize a Hi-Z disable path.");
			context.Expect(snapshot.hiZValidation.inactiveReason.empty(), "Default Hi-Z validation should leave inactive reason empty until a real missing-resource failure.");
		});

		registry.Add("AdaptiveVirtualGeometry.Diagnostics.CountsPendingPinnedAndCookOutputDefaults", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.adaptiveCookProfile = MakeDefaultAdaptiveVirtualGeometryCookProfile();
			asset.detailLevels = {
				{ 0u, 0u, 5u, 300u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.generatedLevels = {
				{ 0u, 0u, 0u, 1.0f, 300u, 5u, 0.0f }
			};
			asset.skippedDetails = {
				{ 1u, 0.70710677f, VirtualGeometryDetailTargetStatus::Skipped, VirtualGeometrySkippedDetailReason::InsufficientTriangleReduction, 0u, 0u, 0u, 0.0f, "not enough reduction" }
			};
			GeometryPageRuntime pinned{};
			pinned.pageId = 0u;
			pinned.state = GeometryPageState::Pinned;
			pinned.byteSize = 256u;
			pinned.fallbackPage = true;
			pinned.pinnedFallback = true;
			GeometryPageRuntime pendingIo{};
			pendingIo.pageId = 1u;
			pendingIo.state = GeometryPageState::PendingIO;
			GeometryPageRuntime pendingUpload{};
			pendingUpload.pageId = 2u;
			pendingUpload.state = GeometryPageState::PendingUpload;
			asset.pages = { pinned, pendingIo, pendingUpload };

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 2028u, { asset }, MakeDefaultVirtualGeometryQualityProfile());

			context.ExpectEqual(1u, snapshot.cookOutput.generatedLevelCount, "Generated-level diagnostics should use explicit adaptive generated records when present.");
			context.ExpectEqual(1u, snapshot.cookOutput.skippedDetailCount, "Skipped adaptive targets should publish a count even before Inspector UI exists.");
			context.ExpectEqual(1u, snapshot.residentPages, "Pinned fallback pages should count as resident for rendering.");
			context.ExpectEqual(2u, snapshot.requestedPages, "Pending IO and pending upload pages should count as requested work.");
			context.ExpectEqual(1u, snapshot.pinnedFallbackPages, "Pinned fallback pages should have a dedicated diagnostics counter.");
			context.ExpectEqual(1u, snapshot.pageResidency.pendingIoPages, "Pending IO pages should be counted separately.");
			context.ExpectEqual(1u, snapshot.pageResidency.pendingUploadPages, "Pending upload pages should be counted separately.");
		});
	}
}
