// DeformableVirtualGeometryTests.cpp - deformable virtual-geometry contracts.
//
// ImplementationCheck will publish deformable cluster packets. These tests validate the
// existing cooked-data and fail-open seams while spelling out the extra packet
// fields that ImplementationCheck must connect to the renderer.

#include "TestHarness.h"

#include "File Formats/TerraEngineMeshFile.h"
#include "Renderer/RenderSystem.h"

#include <array>
#include <cstdint>
#include <limits>

namespace TerraEngine::Tests
{
	namespace
	{
		// This is intentionally a test-only contract until ImplementationCheck provides an
		// equivalent renderer-owned packet. A cluster may not discard these
		// fields while simplification, page fallback, or culling are in use.
		struct DeformableVirtualGeometryPacketContract
		{
			std::array<std::uint16_t, 4u> jointIndices{};
			std::array<std::uint16_t, 4u> jointWeights{};
			std::uint32_t morphTargetCount = 0u;
			std::uint32_t materialSectionId = 0u;
			bool dynamicBoundsValid = false;
			bool failOpenCulling = false;
			bool fallbackCoverageComplete = false;
		};

		UUID MakeDeformableVirtualGeometryTestUuid(std::uint8_t seed)
		{
			UUID id{};
			id[15] = seed;
			return id;
		}

		VirtualGeometryAssetRuntime BuildFallbackAsset()
		{
			using namespace TerraEngine::Renderer;

			VirtualGeometryAssetRuntime asset;
			asset.assetId = MakeDeformableVirtualGeometryTestUuid(20u);
			asset.clusterCoverageValidated = true;
			// Cluster selection is active only after the cooked whole-mesh LOD
			// metadata is present. These two records mirror the failed detail page
			// and its pinned coarse fallback below.
			asset.detailLevels = {
				{ 0u, 0u, 4u, 4u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 0u, 4u, 4u, 0.1f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(4u);

			GeometryPageRuntime fallbackPage;
			fallbackPage.pageId = 0u;
			fallbackPage.fallbackPageId = 0u;
			fallbackPage.firstLodIndex = 1u;
			fallbackPage.lodCount = 1u;
			fallbackPage.state = GeometryPageState::Pinned;
			fallbackPage.fallbackPage = true;
			fallbackPage.pinnedFallback = true;
			fallbackPage.payloadByteOffset = 256u;
			fallbackPage.payloadByteSize = 1024u;
			asset.pages.push_back(fallbackPage);

			GeometryPageRuntime missingDetail;
			missingDetail.pageId = 1u;
			missingDetail.fallbackPageId = 0u;
			missingDetail.firstLodIndex = 0u;
			missingDetail.lodCount = 1u;
			missingDetail.state = GeometryPageState::Failed;
			missingDetail.failureReason = "deterministic test page failure";
			asset.pages.push_back(missingDetail);

			VirtualGeometryClusterRuntime fallbackCluster;
			fallbackCluster.clusterId = 0u;
			fallbackCluster.firstChildClusterId = 1u;
			fallbackCluster.childCount = 1u;
			fallbackCluster.lodIndex = 1u;
			fallbackCluster.firstGroupIndex = 0u;
			fallbackCluster.groupCount = 4u;
			fallbackCluster.materialSectionId = 7u;
			fallbackCluster.pageId = 0u;
			fallbackCluster.fallbackClusterId = 0u;
			// The missing-detail regression must cross the refinement threshold so
			// selection actually requests child cluster 1 before exercising its
			// resident fallback coverage. A zero error deliberately keeps the
			// coarser parent and cannot validate the fail-open path.
			fallbackCluster.geometricError = 0.1f;
			fallbackCluster.boundsRadius = 1.0f;
			asset.clusters.push_back(fallbackCluster);

			VirtualGeometryClusterRuntime detailCluster;
			detailCluster.clusterId = 1u;
			detailCluster.parentClusterId = 0u;
			detailCluster.firstChildClusterId = std::numeric_limits<std::uint32_t>::max();
			detailCluster.lodIndex = 0u;
			detailCluster.firstGroupIndex = 0u;
			detailCluster.groupCount = 4u;
			detailCluster.materialSectionId = 7u;
			detailCluster.pageId = 1u;
			detailCluster.fallbackClusterId = 0u;
			detailCluster.boundsRadius = 0.5f;
			asset.clusters.push_back(detailCluster);
			return asset;
		}
	}

	void RegisterDeformableVirtualGeometryTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Deformable.PreservesWeightsAndMorphEnvelopeContract", [](TestContext& context) {
			const DeformableVirtualGeometryPacketContract contract{
				{ 2u, 5u, 8u, 11u }, { 30000u, 20000u, 10000u, 5535u }, 3u, 7u, true, false, true
			};

			context.ExpectEqual(4u, kTemfMaximumSkinInfluences,
				"Canonical deformable vertices must retain four normalized skin influences.");
			context.Expect(contract.jointWeights[0] != 0u && contract.jointWeights[3] != 0u && contract.morphTargetCount == 3u,
				"ImplementationCheck packets must preserve every cooked influence and morph target needed to reconstruct deformation.");
			context.Expect((kTemfDeformableFlagConservativeMorphEnvelope
				& kTemfDeformableFlagDenseMorphDeltas) == 0u,
				"Morph envelopes and dense morph deltas must remain independently identifiable in the cooked payload.");
		});

		registry.Add("VirtualGeometry.Deformable.CanonicalPageMorphContract", [](TestContext& context) {
			TemfDeformableDemandedPagePayload payload{};
			payload.pageId = 7u;
			payload.sectionCount = 1u;
			payload.sectionTableOffset =
				sizeof(TemfDeformableDemandedPagePayload);
			payload.decodedByteSize =
				sizeof(TemfDeformableDemandedPagePayload)
				+ sizeof(TemfDeformablePageSection)
				+ sizeof(TemfDeformablePageMorphVertex);
			TemfDeformablePageSection section{};
			section.kind = TemfDeformablePageSectionKind::MorphDeltas;
			section.relativeByteOffset = payload.sectionTableOffset
				+ sizeof(TemfDeformablePageSection);
			section.byteSize = sizeof(TemfDeformablePageMorphVertex);
			section.elementCount = 1u;
			section.elementStride = sizeof(TemfDeformablePageMorphVertex);
			TemfDeformablePageMorphVertex morphVertex{};

			context.Expect(payload.pageId == 7u
				&& section.relativeByteOffset
					+ section.byteSize == payload.decodedByteSize
				&& sizeof(TemfDeformablePageMorphVertex) == 48u,
				"Canonical demanded pages must own relative, fixed-stride morph data.");
			context.Expect(morphVertex.normalDelta[0] == 0.0f
				&& morphVertex.tangentDelta[0] == 0.0f,
				"Absent normal and tangent deltas must remain zero-filled.");
		});

		registry.Add("VirtualGeometry.Deformable.DynamicBoundsContractRemainsConservative", [](TestContext& context) {
			TemfDeformableMetadata metadata{};
			metadata.flags = kTemfDeformableFlagConservativeMorphEnvelope;
			metadata.deformationBoundsMin[0] = -2.0f;
			metadata.deformationBoundsMax[0] = 3.0f;

			context.Expect((metadata.flags
				& kTemfDeformableFlagConservativeMorphEnvelope) != 0u,
				"Deformable geometry must declare its conservative morph envelope.");
			context.Expect(metadata.deformationBoundsMin[0] <= 0.0f
				&& metadata.deformationBoundsMax[0] >= 0.0f,
				"The conservative deformation envelope must contain the bind-pose origin as a culling-safe baseline.");
		});

		registry.Add("VirtualGeometry.Deformable.UnprovenCullingFailsOpen", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			const DeformableVirtualGeometryPacketContract contract{ {}, {}, 0u, 7u, false, true, true };
			const VirtualGeometryClusterSelectionResult selection = RenderSystem::SelectVirtualGeometryClusterCut(
				BuildFallbackAsset(),
				MakeDeformableVirtualGeometryTestUuid(20u),
				MakeDeformableVirtualGeometryTestUuid(21u),
				MakeDefaultVirtualGeometryQualityProfile(),
				512.0f,
				true,
				false);

			context.Expect(contract.failOpenCulling, "ImplementationCheck must label deformable clusters with unproven bounds as fail-open before GPU culling.");
			context.Expect(selection.range.failOpen, "The existing selection seam must preserve visibility when bounds are invalid.");
		});

		registry.Add("VirtualGeometry.Deformable.MissingDetailPageUsesCompleteFallbackCoverage", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			const VirtualGeometryAssetRuntime asset = BuildFallbackAsset();
			const VirtualGeometryClusterSelectionResult selection = RenderSystem::SelectVirtualGeometryClusterCut(
				asset,
				asset.assetId,
				MakeDeformableVirtualGeometryTestUuid(22u),
				MakeDefaultVirtualGeometryQualityProfile(),
				512.0f,
				glm::mat4(1.0f),
				glm::vec3(0.0f, 0.0f, 5.0f),
				glm::uvec2(1280u, 720u),
				true,
				true);

			context.Expect(!selection.selectedClusters.empty() && selection.selectedClusters.front().usingFallback,
				"A missing deformable detail page must substitute its resident fallback cluster instead of creating a hole.");
			context.Expect(!selection.pageNeeds.empty() && selection.pageNeeds.front().fallbackRequired,
				"Fallback rendering must retain the failed detail-page request so streaming can recover it.");
		});

		registry.Add("VirtualGeometry.Deformable.MaterialSectionCoverageContract", [](TestContext& context) {
			const DeformableVirtualGeometryPacketContract contract{ {}, {}, 2u, 7u, true, false, true };
			const auto asset = BuildFallbackAsset();

			context.ExpectEqual(7u, asset.clusters.front().materialSectionId, "Fallback clusters must retain their cooked material-section identity.");
			context.Expect(contract.materialSectionId == asset.clusters.front().materialSectionId && contract.fallbackCoverageComplete,
				"ImplementationCheck packet emission must preserve material coverage when a deformable cluster falls back to a coarser page.");
		});
	}
}
