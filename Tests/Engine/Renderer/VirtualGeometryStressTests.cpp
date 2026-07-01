#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <cstdint>
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
	}
}
