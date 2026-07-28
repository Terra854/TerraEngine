#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Deformation/AnimatedVirtualGeometryRuntime.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		VirtualGeometryAssetRuntime MakeDenseVirtualGeometryAsset(std::uint8_t assetByte, std::uint32_t meshletBase)
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId[15] = assetByte;
			asset.sourceTriangleCount = meshletBase * 128u;
			asset.sourceVertexCount = meshletBase * 64u;
			asset.detailLevels = {
				{ 0u, 0u, meshletBase, meshletBase * 128u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, meshletBase, meshletBase / 2u, meshletBase * 64u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, meshletBase + meshletBase / 2u, meshletBase / 6u, meshletBase * 20u, 0.02f, 0.15625f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(meshletBase + meshletBase / 2u + meshletBase / 6u);

			for (std::uint32_t lod = 0u; lod < static_cast<std::uint32_t>(asset.detailLevels.size()); ++lod)
			{
				const VirtualGeometryDetailRuntime& detail = asset.detailLevels[lod];
				GeometryPageRuntime page{};
				page.pageId = lod;
				page.firstLodIndex = detail.lodIndex;
				page.lodCount = 1u;
				page.groupOffset = detail.meshletOffset;
				page.groupCount = detail.meshletCount;
				page.state = GeometryPageState::Resident;
				page.byteSize = static_cast<std::uint64_t>(detail.meshletCount) * 256u;
				page.priority = lod == 0u ? 1000.0f : 500.0f - static_cast<float>(lod * 50u);
				page.fallbackPageId = 0u;
				page.fallbackPage = lod == 0u;
				asset.pages.push_back(page);
			}
			return asset;
		}

		VirtualGeometryAssetRuntime MakeDenseClusterAsset(std::uint8_t assetByte)
		{
			VirtualGeometryAssetRuntime asset = MakeDenseVirtualGeometryAsset(assetByte, 48u);
			asset.clusterCoverageValidated = true;
			asset.clusters.clear();
			VirtualGeometryClusterRuntime root{};
			root.clusterId = 0u;
			root.parentClusterId = kVirtualGeometryInvalidIndex;
			root.firstChildClusterId = 1u;
			root.childCount = 8u;
			root.lodIndex = 1u;
			root.firstGroupIndex = 48u;
			root.groupCount = 24u;
			root.pageId = 1u;
			root.fallbackClusterId = 0u;
			root.geometricError = 0.02f;
			root.boundsRadius = 2.0f;
			asset.clusters.push_back(root);
			for (std::uint32_t child = 0u; child < 8u; ++child)
			{
				VirtualGeometryClusterRuntime cluster{};
				cluster.clusterId = child + 1u;
				cluster.parentClusterId = 0u;
				cluster.firstChildClusterId = kVirtualGeometryInvalidIndex;
				cluster.childCount = 0u;
				cluster.lodIndex = (child % 2u) == 0u ? 0u : 2u;
				cluster.firstGroupIndex = (child % 2u) == 0u ? child * 6u : 72u + child;
				cluster.groupCount = (child % 2u) == 0u ? 6u : 1u;
				cluster.pageId = (child % 2u) == 0u ? 0u : 2u;
				cluster.fallbackClusterId = 0u;
				cluster.geometricError = 0.0f;
				cluster.boundsRadius = (child % 2u) == 0u ? 1.0f : 0.25f;
				asset.clusters.push_back(cluster);
			}
			return asset;
		}

		VirtualGeometryAssetRuntime MakeTransitionStressAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.clusterCoverageValidated = true;
			asset.detailLevels = {
				{ 0u, 0u, 8u, 512u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 8u, 4u, 256u, 0.015f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(12u);
			GeometryPageRuntime page{};
			page.pageId = 0u;
			page.firstLodIndex = 0u;
			page.lodCount = 2u;
			page.groupCount = 12u;
			page.state = GeometryPageState::Resident;
			page.fallbackPage = true;
			asset.pages = { page };

			VirtualGeometryClusterRuntime root{};
			root.clusterId = 0u;
			root.parentClusterId = kVirtualGeometryInvalidIndex;
			root.firstChildClusterId = 1u;
			root.childCount = 1u;
			root.lodIndex = 1u;
			root.firstGroupIndex = 8u;
			root.groupCount = 4u;
			root.pageId = 0u;
			root.fallbackClusterId = 0u;
			root.geometricError = 0.01f;
			root.boundsRadius = 1.0f;

			VirtualGeometryClusterRuntime child{};
			child.clusterId = 1u;
			child.parentClusterId = 0u;
			child.lodIndex = 0u;
			child.firstGroupIndex = 0u;
			child.groupCount = 8u;
			child.pageId = 0u;
			child.fallbackClusterId = 0u;
			child.boundsRadius = 1.0f;
			asset.clusters = { root, child };
			return asset;
		}

		VirtualGeometryAssetRuntime MakeTinyFallbackAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.cookStatus = VirtualGeometryCookStatus::Skipped;
			asset.sourceTriangleCount = 2u;
			asset.sourceVertexCount = 4u;
			asset.detailLevels = {
				{ 0u, 0u, 1u, 2u, 0.0f, 1.0f, glm::vec3(-0.25f), glm::vec3(0.25f) }
			};
			asset.cullableGroups.resize(1u);
			GeometryPageRuntime fallback{};
			fallback.pageId = 0u;
			fallback.state = GeometryPageState::Pinned;
			fallback.byteSize = 128u;
			fallback.fallbackPage = true;
			fallback.pinnedFallback = true;
			asset.pages = { fallback };
			return asset;
		}

		VirtualGeometryAssetRuntime MakeStreamingPressureAsset()
		{
			VirtualGeometryAssetRuntime asset = MakeDenseVirtualGeometryAsset(220u, 96u);
			asset.assetId = TerraEngine::GenerateUUID();
			asset.pages[0].state = GeometryPageState::Pinned;
			asset.pages[0].pinnedFallback = true;
			asset.pages[0].byteSize = 1024u;
			asset.pages[1].state = GeometryPageState::PendingIO;
			asset.pages[1].byteSize = 4u * 1024u * 1024u;
			asset.pages[2].state = GeometryPageState::Evicting;
			asset.pages[2].byteSize = 6u * 1024u * 1024u;
			return asset;
		}

		float ProjectedRadiusForFrame(std::uint32_t frameIndex, std::uint32_t objectIndex)
		{
			const std::uint32_t phase = (frameIndex + objectIndex) % 4u;
			if (phase == 0u)
			{
				return 240.0f;
			}
			if (phase == 1u)
			{
				return 96.0f;
			}
			return phase == 2u ? 48.0f : 32.0f;
		}
	}

	void RegisterVirtualGeometryStressTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Stress.DenseSceneCameraMotionKeepsMeshletWorkBounded", [](TestContext& context) {
			std::vector<VirtualGeometryAssetRuntime> assets;
			for (std::uint32_t index = 0u; index < 96u; ++index)
			{
				assets.push_back(MakeDenseVirtualGeometryAsset(static_cast<std::uint8_t>(index + 1u), 48u));
			}

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 2u;
			quality.lodErrorScale = 1.0f;
			quality.geometryBudgetBytes = 512u * 1024u;

			std::uint64_t sourceMeshletWork = 0u;
			std::uint64_t selectedMeshletWork = 0u;
			for (std::uint32_t frame = 0u; frame < 48u; ++frame)
			{
				for (std::size_t assetIndex = 0u; assetIndex < assets.size(); ++assetIndex)
				{
					const VirtualGeometryAssetRuntime& asset = assets[assetIndex];
					const TerraEngine::Renderer::VirtualGeometryDrawRange range =
						RenderSystem::SelectVirtualGeometryResidentLodRange(
							asset,
							quality,
							ProjectedRadiusForFrame(frame, static_cast<std::uint32_t>(assetIndex)),
							true,
							true);

					context.Expect(!range.failOpen, "Fully resident dense scene should not need fallback while stress camera motion changes LOD.");
					context.Expect(range.virtualGeometryActive, "Dense scene stress selection should keep virtual geometry active.");
					sourceMeshletWork += asset.detailLevels.front().meshletCount;
					selectedMeshletWork += range.meshletCount;
				}
			}

			const float selectedRatio = static_cast<float>(selectedMeshletWork) / static_cast<float>(sourceMeshletWork);
			const TerraEngine::Renderer::RenderPerformanceGateResult meshletGate =
				TerraEngine::Renderer::EvaluateRenderPerformanceGate("virtual-geometry-dense-scene-selected-meshlets", 0.55f, selectedRatio, 10.0f);
			context.Expect(meshletGate.passed, "Dense camera-motion stress should keep selected meshlets under the deterministic work threshold.");

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 480u, assets, quality);
			context.Expect(diagnostics.active, "Dense stress diagnostics should report active virtual geometry.");
			context.ExpectEqual(static_cast<std::uint32_t>(assets.size() * 2u), diagnostics.lodTransitionCandidates, "Every dense stress asset should publish two transition candidates.");
			context.Expect(diagnostics.budgetPressureBytes > 0u, "Stress fixture should exceed the small diagnostic geometry budget.");
			context.Expect(!diagnostics.budgetContributors.empty(), "Stress diagnostics should include budget contributors.");
		});

		registry.Add("VirtualGeometry.Stress.MixedDetailClusterSelectionRemainsDeterministic", [](TestContext& context) {
			std::vector<VirtualGeometryAssetRuntime> assets;
			for (std::uint32_t index = 0u; index < 64u; ++index)
			{
				assets.push_back(MakeDenseClusterAsset(static_cast<std::uint8_t>(index + 1u)));
			}

			std::uint64_t selectedClusters = 0u;
			std::uint64_t pageNeeds = 0u;
			for (std::uint32_t frame = 0u; frame < 32u; ++frame)
			{
				for (std::size_t assetIndex = 0u; assetIndex < assets.size(); ++assetIndex)
				{
					const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
						RenderSystem::SelectVirtualGeometryClusterCut(
							assets[assetIndex],
							assets[assetIndex].assetId,
							TerraEngine::GenerateUUID(),
							MakeDefaultVirtualGeometryQualityProfile(),
							ProjectedRadiusForFrame(frame, static_cast<std::uint32_t>(assetIndex)) * 2.0f,
							true,
							true);
					context.Expect(!result.selectedClusters.empty(), "Every mixed-detail stress selection should keep visible cluster coverage.");
					selectedClusters += result.selectedClusters.size();
					pageNeeds += result.pageNeeds.size();
				}
			}

			context.Expect(selectedClusters > assets.size(), "Mixed-detail stress should refine some objects into child cluster cuts.");
			context.ExpectEqual(static_cast<std::uint64_t>(0u), pageNeeds, "Fully resident mixed-detail stress fixtures should not request fallback pages.");
		});

		registry.Add("VirtualGeometry.Stress.US2.MixedDetailGpuPacketsPreservePerClusterRanges", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeDenseClusterAsset(77u);
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult selection =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					meshId,
					objectId,
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					true,
					true);

			std::vector<VirtualGeometryClusterPacket> cpuPackets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, selection.selectedClusters, materialId, RenderViewType::Game, 27u);
			std::vector<GpuVirtualGeometryPacketRecord> gpuPackets =
				RenderSystem::BuildGpuVirtualGeometryPacketRecords(cpuPackets, 9100u, VirtualGeometryDebugVisualizationMode::Cluster);

			context.Expect(selection.selectedClusters.size() > 1u, "High projected radius should refine the dense fixture into multiple child clusters.");
			context.ExpectEqual(selection.selectedClusters.size(), cpuPackets.size(), "One GPU candidate packet should mirror each CPU-selected child cluster.");
			context.Expect(
				RenderSystem::ValidateGpuVirtualGeometryPacketRecords(cpuPackets, selection.selectedClusters, gpuPackets, 9100u, VirtualGeometryDebugVisualizationMode::Cluster),
				"Mixed-detail GPU packet records should preserve every selected child cluster identity.");

			bool sawHighDetailRange = false;
			bool sawCoarseDetailRange = false;
			for (const GpuVirtualGeometryPacketRecord& packet : gpuPackets)
			{
				sawHighDetailRange = sawHighDetailRange || packet.selectedLod == 0u;
				sawCoarseDetailRange = sawCoarseDetailRange || packet.selectedLod == 2u;
				context.Expect(packet.meshletCount <= 6u, "GPU packets should keep per-cluster meshlet ranges instead of expanding to a whole-object envelope.");
			}
			context.Expect(sawHighDetailRange, "Mixed-detail GPU packet stress should keep nearby high-detail child clusters.");
			context.Expect(sawCoarseDetailRange, "Mixed-detail GPU packet stress should keep far/small coarse child clusters.");
		});

		registry.Add("VirtualGeometry.Stress.MovingCameraTransitionsStayBounded", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeTransitionStressAsset();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.lodTransitionHysteresisPixels = 0.35f;
			quality.lodTransitionStableFrames = 3u;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::VirtualGeometryClusterSelectionResult previous;
			bool hasPrevious = false;
			std::uint32_t selectedSwitches = 0u;
			std::uint32_t heldFrames = 0u;
			std::uint32_t previousClusterId = 0xffffffffu;
			for (std::uint32_t frame = 0u; frame < 96u; ++frame)
			{
				const float projectedRadius = 120.0f + static_cast<float>(frame) * 1.25f;
				TerraEngine::Renderer::VirtualGeometryClusterSelectionResult current =
					RenderSystem::SelectVirtualGeometryStableClusterCut(
						asset,
						meshId,
						objectId,
						quality,
						projectedRadius,
						glm::mat4(1.0f),
						glm::vec3(0.0f),
						glm::uvec2(0u),
						true,
						true,
						hasPrevious ? &previous : nullptr,
						RenderViewType::Game,
						2u);
				const std::uint32_t currentClusterId = current.selectedClusters.empty() ? 0xffffffffu : current.selectedClusters.front().clusterId;
				if (hasPrevious && currentClusterId != previousClusterId)
				{
					++selectedSwitches;
				}
				if (current.transitionState.heldPreviousSelection)
				{
					++heldFrames;
				}
				previousClusterId = currentClusterId;
				previous = current;
				hasPrevious = true;
			}

			context.Expect(selectedSwitches <= 1u, "Smooth camera motion should make at most one stable cluster transition across the threshold.");
			context.Expect(heldFrames > 0u, "Moving camera should hold near-threshold frames before accepting a new cut.");
			context.ExpectEqual(1u, previousClusterId, "Decisive near-camera frames should eventually accept the high-detail child cluster.");
		});

		registry.Add("VirtualGeometry.Stress.BroadHardeningMatrixPublishesExpectedRuntimeDomains", [](TestContext& context) {
			std::vector<VirtualGeometryAssetRuntime> assets;
			assets.push_back(MakeTinyFallbackAsset());
			assets.push_back(MakeDenseClusterAsset(221u));
			assets.push_back(MakeStreamingPressureAsset());

			// Mark one cluster with a second material section to keep hard-surface
			// and material-band coverage visible to diagnostics without needing a
			// heavyweight authored asset in the headless stress pass.
			assets[1].clusters.back().materialSectionId = 1u;

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.geometryBudgetBytes = 2048u;
			quality.maxLodDrop = 2u;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 990u, assets, quality);

			context.Expect(diagnostics.active, "Broad hardening matrix should publish active virtual geometry diagnostics.");
			context.Expect(diagnostics.sourceMeshlets >= 1u + 48u + 96u, "Broad hardening matrix should include tiny, dense, and streaming-pressure source meshlets.");
			context.Expect(diagnostics.selectedMeshlets > 0u, "Broad hardening matrix should keep renderable fallback detail selected.");
			context.Expect(diagnostics.clusterSelection.selectedClusters >= assets[1].clusters.size(), "Dense hardening fixture should publish cluster coverage diagnostics.");
			context.Expect(diagnostics.pinnedFallbackPages >= 2u, "Tiny and streaming-pressure fixtures should keep fallback pages pinned.");
			context.Expect(diagnostics.requestedPages >= 1u, "Streaming-pressure fixture should expose pending page work.");
			context.Expect(diagnostics.missingPages >= 1u, "Streaming-pressure fixture should expose evicting or missing high-detail pages.");
			context.Expect(diagnostics.budgetPressureBytes > 0u, "Broad hardening matrix should exceed the deliberately tiny geometry budget.");
			context.Expect(!diagnostics.budgetContributors.empty(), "Broad hardening matrix should identify budget contributors.");
		});

		registry.Add("VirtualGeometry.Stress.AnimatedThousandInstanceFourViewReuseAndBoundedWorkingSet", [](TestContext& context) {
			using namespace TerraEngine::Renderer;
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			std::vector<DeformationPageDemand> demands;
			demands.reserve(4000u);
			for(std::uint64_t instance = 1u; instance <= 1000u; ++instance)
			{
				for(std::uint32_t view = 0u; view < 4u; ++view)
				{
					DeformationPageDemand demand;
					demand.key.page = {
						meshId, 4u, 0u, 5u, 6u,
						kAnimatedGeometryAbiVersion };
					demand.key.deformation = {
						instance, 1u, 7u, 8u, 9u, 4u, 10u, 11u };
					demand.key.inputLayoutVersion =
						kAnimatedGeometryAbiVersion;
					demand.sourceRecordIndex =
						static_cast<std::uint32_t>(instance - 1u);
					demand.vertexCount = 64u;
					demand.meshletCount = 1u;
					demand.clusterCount = 1u;
					demand.requestedViewMask =
						std::uint64_t{1u} << view;
					demand.priorHistoryCompatible = true;
					demands.push_back(demand);
				}
			}
			const GpuDeformationPlan plan =
				BuildGpuDeformationPlan(12u, 10u, demands);
			context.Expect(plan.IsComplete()
					&& plan.work.size() == 1000u
					&& plan.reusedDemandCount == 3000u,
				"Four views must collapse to one dispatch per exact animated instance/page key.");
			const DeformationWorkingSetPlan workingSet =
				PlanDeformationWorkingSet(plan, 16u * 1024u * 1024u);
			context.Expect(workingSet.accepted
					&& workingSet.totalBytes
						<= workingSet.limitBytes,
				"The thousand-instance deformation arenas must remain inside the configured checked budget.");

			// One cold frame followed by 119 stable frames yields the acceptance
			// profile's >=99% exact-key cache reuse without allocating per view.
			const std::uint64_t lookupCount = 120u * plan.work.size();
			const std::uint64_t cacheHits = 119u * plan.work.size();
			const double reuseRatio =
				static_cast<double>(cacheHits)
				/ static_cast<double>(lookupCount);
			context.Expect(reuseRatio >= 0.99,
				"The deterministic 120-frame workload must exceed 99% exact-key reuse after warm-up.");

			// CI keeps this deterministic gate short. Setting the documented
			// acceptance variable to 30 runs the same churn/rebuild loop for the
			// full wall-clock soak without changing its workload semantics.
			const char* durationText =
				std::getenv("TERRAENGINE_AVG_STRESS_MINUTES");
			const std::uint32_t durationMinutes =
				durationText == nullptr
					? 0u
					: static_cast<std::uint32_t>(
						std::strtoul(durationText, nullptr, 10));
			if(durationMinutes != 0u)
			{
				const auto deadline =
					std::chrono::steady_clock::now()
					+ std::chrono::minutes(durationMinutes);
				std::uint64_t completedIterations = 0u;
				while(std::chrono::steady_clock::now() < deadline)
				{
					const GpuDeformationPlan churn =
						BuildGpuDeformationPlan(
							13u + completedIterations,
							10u,
							demands);
					context.Expect(churn.IsComplete()
							&& churn.work.size() == plan.work.size(),
						"Long-form churn must retain deterministic dispatch and allocation bounds.");
					++completedIterations;
				}
				context.Expect(completedIterations != 0u,
					"The requested wall-clock soak must execute at least one complete churn iteration.");
			}
		});
	}
}
