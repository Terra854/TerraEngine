#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderGraph.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryRenderTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Render.Diagnostics.ReportsLodTransitionPoppingRisk", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.sourceTriangleCount = 1024u;
			asset.sourceVertexCount = 512u;
			asset.detailLevels = {
				{ 0u, 0u, 8u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 8u, 4u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 12u, 2u, 128u, 0.03f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(14u);

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 3u;
			quality.lodErrorScale = 8.0f;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
					TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(
					RenderViewType::Game,
					77u,
					{ asset },
					quality);

			const bool hasPoppingWarning = std::any_of(snapshot.warnings.begin(), snapshot.warnings.end(), [](const std::string& warning) {
				return warning.find("popping") != std::string::npos || warning.find("LOD drop") != std::string::npos;
			});

			context.Expect(snapshot.active, "Enabled virtual geometry quality diagnostics should mark the view active.");
			context.ExpectEqual(2u, snapshot.lodTransitionCandidates, "Three detail levels should expose two transition candidates.");
			context.Expect(snapshot.poppingRiskWarnings > 0u, "Aggressive max LOD drop and error scale should publish popping-risk counters.");
			context.Expect(hasPoppingWarning, "Popping-risk diagnostics should include actionable warning text.");
		});

		registry.Add("VirtualGeometry.Render.LodSelection.ChoosesWholeMeshRangesFromProjectedSize", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 128u, 0.02f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 2u;
			quality.lodErrorScale = 1.0f;

			const TerraEngine::Renderer::VirtualGeometryDrawRange nearRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 512.0f, true, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange farRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 32.0f, true, true);

			context.ExpectEqual(0u, nearRange.selectedLodIndex, "Large projected meshes should keep LOD0.");
			context.ExpectEqual(2u, farRange.selectedLodIndex, "Small projected meshes should select the lower-detail range.");
			context.ExpectEqual(24u, farRange.meshletOffset, "Selected LOD range should preserve cooked meshlet offset.");
			context.ExpectEqual(4u, farRange.meshletCount, "Selected LOD range should preserve cooked meshlet count.");
		});

		registry.Add("VirtualGeometry.Render.LodSelection.ReachesEveryCookedDetailLevel", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 256u, 0.02f, 0.25f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 3u, 28u, 2u, 128u, 0.03f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 0u;
			quality.lodErrorScale = 1.0f;

			const TerraEngine::Renderer::VirtualGeometryDrawRange nearRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 640.0f, true, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange firstDropRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 320.0f, true, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange secondDropRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 160.0f, true, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange finalDropRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 80.0f, true, true);

			context.ExpectEqual(0u, nearRange.selectedLodIndex, "Large projected meshes should remain at the highest detail.");
			context.ExpectEqual(1u, firstDropRange.selectedLodIndex, "Crossing the first screen-size band should select LOD1.");
			context.ExpectEqual(2u, secondDropRange.selectedLodIndex, "Crossing the second screen-size band should select LOD2.");
			context.ExpectEqual(3u, finalDropRange.selectedLodIndex, "The final cooked detail level should be reachable for tiny projected meshes.");
			context.ExpectEqual(28u, finalDropRange.meshletOffset, "Final LOD selection should preserve the cooked meshlet offset.");
			context.ExpectEqual(2u, finalDropRange.meshletCount, "Final LOD selection should preserve the cooked meshlet count.");
		});

		registry.Add("VirtualGeometry.Render.LodSelection.KeepsResidentEmbeddedDetailPages", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 256u, 0.02f, 0.25f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			for (std::uint32_t lodIndex = 0u; lodIndex < 3u; ++lodIndex)
			{
				GeometryPageRuntime page{};
				page.pageId = lodIndex;
				page.firstLodIndex = lodIndex;
				page.lodCount = 1u;
				page.state = GeometryPageState::Resident;
				page.fallbackPageId = 0u;
				page.fallbackPage = lodIndex == 0u;
				asset.pages.push_back(page);
			}

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 0u;

			const TerraEngine::Renderer::VirtualGeometryDrawRange range =
				RenderSystem::SelectVirtualGeometryResidentLodRange(asset, quality, 160.0f, true, true);

			context.Expect(!range.failOpen, "Embedded resident detail pages should not fall back to the highest-detail page.");
			context.ExpectEqual(2u, range.selectedLodIndex, "A resident lower-detail page should remain selected.");
			context.ExpectEqual(24u, range.meshletOffset, "Resident lower-detail pages should preserve the cooked meshlet offset.");
			context.ExpectEqual(4u, range.meshletCount, "Resident lower-detail pages should preserve the cooked meshlet count.");
		});

		registry.Add("VirtualGeometry.Render.LodSelection.FailsOpenForDisabledCullingOrInvalidBounds", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 1u;

			const TerraEngine::Renderer::VirtualGeometryDrawRange noTaskRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 16.0f, false, true);
			const TerraEngine::Renderer::VirtualGeometryDrawRange invalidBoundsRange =
				RenderSystem::SelectVirtualGeometryLodRange(asset, quality, 16.0f, true, false);

			context.Expect(noTaskRange.failOpen, "Disabled task culling should fail open.");
			context.ExpectEqual(0u, noTaskRange.selectedLodIndex, "Disabled task culling should keep highest-detail fallback.");
			context.Expect(invalidBoundsRange.failOpen, "Invalid meshlet bounds should fail open.");
			context.ExpectEqual(0u, invalidBoundsRange.selectedLodIndex, "Invalid bounds should keep highest-detail fallback.");
		});

		registry.Add("VirtualGeometry.Render.Diagnostics.ReportsUnsupportedMeshShaderRoute", [](TestContext& context) {
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryInactiveReason::MissingMeshShaderRoute),
				static_cast<std::uint32_t>(ResolveVirtualGeometryInactiveReason(quality, false)),
				"Always-on virtual geometry should report unsupported when the backend lacks a mesh shader route.");

			context.ExpectEqual(
				static_cast<std::uint32_t>(VirtualGeometryInactiveReason::None),
				static_cast<std::uint32_t>(ResolveVirtualGeometryInactiveReason(quality, true)),
				"Virtual geometry should be active when the backend exposes the mesh shader route.");
		});

		registry.Add("VirtualGeometry.Render.LodSelection.ResidencyUsesBestResidentFallbackDetail", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			GeometryPageRuntime highDetailPage{};
			highDetailPage.pageId = 0u;
			highDetailPage.firstLodIndex = 0u;
			highDetailPage.lodCount = 1u;
			highDetailPage.state = GeometryPageState::Failed;
			highDetailPage.fallbackPageId = 1u;
			GeometryPageRuntime residentPage{};
			residentPage.pageId = 1u;
			residentPage.firstLodIndex = 1u;
			residentPage.lodCount = 1u;
			residentPage.state = GeometryPageState::Resident;
			residentPage.fallbackPageId = 1u;
			residentPage.fallbackPage = true;
			asset.pages = { highDetailPage, residentPage };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();

			const TerraEngine::Renderer::VirtualGeometryDrawRange range =
				RenderSystem::SelectVirtualGeometryResidentLodRange(asset, quality, 512.0f, true, true);

			context.Expect(range.failOpen, "Always-on virtual geometry should fail open when the desired page is not resident.");
			context.Expect(range.virtualGeometryActive, "Stale disabled settings should not turn off the mesh-shader virtual geometry route.");
			context.ExpectEqual(1u, range.selectedLodIndex, "Residency fallback should choose the best resident detail when LOD0 is not resident.");
			context.ExpectEqual(16u, range.meshletOffset, "Residency fallback should preserve the resident LOD meshlet offset.");
			context.ExpectEqual(8u, range.meshletCount, "Residency fallback should preserve the resident LOD meshlet count.");
			context.Expect(range.fallbackReason.find("resident") != std::string::npos, "Fallback reason should explain resident fallback selection.");
		});

		registry.Add("VirtualGeometry.Render.PreparationCache.InvalidatesWhenSelectedLodRangeChanges", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameRenderableRecord record{};
			record.objectId = TerraEngine::GenerateUUID();
			record.meshId = TerraEngine::GenerateUUID();
			record.materialId = TerraEngine::GenerateUUID();
			record.modelMatrix = glm::mat4(1.0f);
			record.virtualGeometry = { 0u, 0u, 12u, true, false, {} };

			const std::uint64_t firstHash = TerraEngine::Renderer::HashPreparedDrawPacketSignature(
				TerraEngine::Renderer::BuildPreparedDrawPacketSignature(record));
			record.virtualGeometry = { 1u, 12u, 6u, true, false, {} };
			const std::uint64_t secondHash = TerraEngine::Renderer::HashPreparedDrawPacketSignature(
				TerraEngine::Renderer::BuildPreparedDrawPacketSignature(record));

			context.Expect(firstHash != secondHash, "Changing selected virtual geometry LOD ranges should invalidate immutable draw packet preparation.");
		});

		registry.Add("VirtualGeometry.Render.Diagnostics.RecordsFailOpenFallbackReason", [](TestContext& context) {
			TerraEngine::Renderer::RenderDiagnosticsSummary summary{};
			TerraEngine::Renderer::VirtualGeometryDrawRange range{};
			range.failOpen = true;
			range.fallbackReason = "invalid meshlet bounds";

			TerraEngine::Renderer::AddVirtualGeometryFailOpenDiagnostic(summary, TerraEngine::GenerateUUID(), range);

			context.ExpectEqual(static_cast<std::size_t>(1), summary.backendFallbacks.size(), "A fail-open virtual geometry range should publish one backend fallback diagnostic.");
			context.Expect(summary.backendFallbacks.front().find("invalid meshlet bounds") != std::string::npos, "Fail-open diagnostics should preserve the exact fallback reason.");
			context.Expect(summary.backendFallbacks.front().find("highest-detail") != std::string::npos, "Fail-open diagnostics should explain that visibility is preserved by fallback rendering.");
		});

		registry.Add("VirtualGeometry.Render.Residency.UsesResidentFallbackWhenHighDetailPageMissing", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 128u, 0.02f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			GeometryPageRuntime fallbackPage{};
			fallbackPage.pageId = 0u;
			fallbackPage.firstLodIndex = 0u;
			fallbackPage.lodCount = 1u;
			fallbackPage.state = GeometryPageState::Resident;
			fallbackPage.fallbackPageId = 0u;
			fallbackPage.fallbackPage = true;
			GeometryPageRuntime missingPage{};
			missingPage.pageId = 2u;
			missingPage.firstLodIndex = 2u;
			missingPage.lodCount = 1u;
			missingPage.state = GeometryPageState::Failed;
			missingPage.fallbackPageId = 0u;
			asset.pages = { fallbackPage, missingPage };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 2u;

			const TerraEngine::Renderer::VirtualGeometryDrawRange range =
				RenderSystem::SelectVirtualGeometryResidentLodRange(asset, quality, 16.0f, true, true);

			context.Expect(range.failOpen, "Missing high-detail pages should fail open through a resident fallback.");
			context.ExpectEqual(0u, range.selectedLodIndex, "Resident fallback page should select the fallback LOD.");
			context.Expect(range.fallbackReason.find("resident fallback") != std::string::npos, "Fallback reason should explain page residency.");
		});

		registry.Add("VirtualGeometry.Render.Diagnostics.CountsResidencyPageStates", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 4u, 256u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			GeometryPageRuntime resident{};
			resident.pageId = 0u;
			resident.state = GeometryPageState::Resident;
			resident.byteSize = 256u;
			GeometryPageRuntime requested{};
			requested.pageId = 1u;
			requested.state = GeometryPageState::Requested;
			requested.byteSize = 512u;
			GeometryPageRuntime failed{};
			failed.pageId = 2u;
			failed.state = GeometryPageState::Failed;
			failed.byteSize = 1024u;
			asset.pages = { resident, requested, failed };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 99u, { asset }, quality);

			context.ExpectEqual(1u, snapshot.residentPages, "Diagnostics should count resident virtual geometry pages.");
			context.ExpectEqual(1u, snapshot.requestedPages, "Diagnostics should count requested or loading virtual geometry pages.");
			context.ExpectEqual(1u, snapshot.missingPages, "Diagnostics should count missing or failed virtual geometry pages.");
			context.Expect(snapshot.estimatedGeometryBytes >= 1792u, "Diagnostics should include page byte estimates in geometry memory.");
			context.Expect(!snapshot.warnings.empty(), "Missing pages should surface a visible diagnostics warning.");
		});

		registry.Add("VirtualGeometry.Render.WorkerPreparation.DeclaresImmutableInputsAndProtectedBoundaries", [](TestContext& context) {
			const TerraEngine::Scheduling::SystemDeclaration declaration = RenderSystem::Instance().SchedulerDeclaration();

			const auto contains = [](const std::vector<std::string>& values, const char* expected) {
				return std::find(values.begin(), values.end(), expected) != values.end();
			};

			// Virtual geometry render preparation is allowed to run on workers
			// only because it consumes snapshots and writes diagnostics packets.
			// Any live scene/object/component mutation must stay outside this
			// worker-safe region and pass through protected mutation boundaries.
			context.ExpectEqual(std::string("Render"), declaration.phase, "RenderSystem should declare its scheduler phase.");
			context.Expect(contains(declaration.reads, "VirtualGeometryQualityProfile"), "RenderSystem should declare virtual geometry quality reads.");
			context.Expect(contains(declaration.reads, "VirtualGeometryResidencySnapshots"), "RenderSystem should declare virtual geometry residency snapshot reads.");
			context.Expect(contains(declaration.writes, "VirtualGeometryDiagnostics"), "RenderSystem should declare virtual geometry diagnostic writes.");
			context.Expect(declaration.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare, "Virtual geometry render preparation should be classified as worker preparation.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::PartiallyWorkerSafe, "RenderSystem should expose the protected GPU and diagnostic boundary.");
			context.Expect(declaration.safeSubwork.find("snapshots") != std::string::npos, "Worker-safe render preparation should name immutable snapshot inputs.");
			context.Expect(declaration.safeSubwork.find("virtual geometry LOD/page preparation") != std::string::npos, "Worker-safe subwork should include virtual geometry LOD/page planning.");
			context.Expect(declaration.protectedCommit.find("virtual geometry diagnostics publication") != std::string::npos, "Protected boundary should name diagnostics publication.");
			context.Expect(declaration.protectedCommit.find("Swapchain") != std::string::npos, "Protected boundary should include live GPU ownership.");
			context.Expect(declaration.requiresFlushedSceneMutations, "RenderSystem should require queued scene mutations to flush before render snapshots are read.");
		});
	}
}
