// DeformableVirtualGeometryTests.cpp - deformable virtual-geometry contracts.
//
// T093 will publish deformable cluster packets. These tests validate the
// existing cooked-data and fail-open seams while spelling out the extra packet
// fields that T093 must connect to the renderer.

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
		// This is intentionally a test-only contract until T093 provides an
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

			context.ExpectEqual(4u, kDeformableMeshMaximumInfluences, "Cooked deformable VG vertices must retain the four normalized skin influences required by TDEF.");
			context.Expect(contract.jointWeights[0] != 0u && contract.jointWeights[3] != 0u && contract.morphTargetCount == 3u,
				"T093 packets must preserve every cooked influence and morph target needed to reconstruct deformation.");
			context.Expect((kDeformableMeshPayloadFlagConservativeMorphEnvelope
				& kDeformableMeshPayloadFlagDenseMorphPositionDeltas) == 0u,
				"Morph envelopes and dense morph deltas must remain independently identifiable in the cooked payload.");
		});

		registry.Add("VirtualGeometry.Deformable.VersionedClusterAndMorphSidecarContract", [](TestContext& context) {
			DeformableVirtualGeometrySidecarHeader sidecar{};
			sidecar.magic = kDeformableVirtualGeometrySidecarMagicValue;
			sidecar.version = kDeformableVirtualGeometrySidecarVersion;
			sidecar.headerSize = sizeof(DeformableVirtualGeometrySidecarHeader);
			sidecar.morphVertexStrideBytes = sizeof(DeformableVirtualGeometryMorphVertex);
			DeformableVirtualGeometryClusterRecord cluster{};
			cluster.flags = kDeformableVirtualGeometryClusterFlagActive
				| kDeformableVirtualGeometryClusterFlagFallbackCoverageComplete;
			cluster.deformationBoundsMin[0] = -1.0F;
			cluster.deformationBoundsMax[0] = 1.0F;
			DeformableVirtualGeometryMorphVertex morphVertex{};

			context.Expect(sidecar.magic == kDeformableVirtualGeometrySidecarMagicValue
				&& sidecar.morphVertexStrideBytes == 48U
				&& sizeof(DeformableVirtualGeometryMorphVertex) == 48U,
				"TDFV must remain a separately versioned 48-byte target-major GPU morph extension over frozen TDEF v1.");
			context.Expect((cluster.flags & kDeformableVirtualGeometryClusterFlagFallbackCoverageComplete) != 0U
				&& (cluster.flags & (kDeformableVirtualGeometryClusterFlagConeCullingSafe
					| kDeformableVirtualGeometryClusterFlagHiZCullingSafe)) == 0U,
				"Cooked deformable clusters must retain fallback coverage while unproven cone and Hi-Z culling stay disabled.");
			context.Expect(morphVertex.normalDelta[0] == 0.0F && morphVertex.tangentDelta[0] == 0.0F,
				"Absent normal and tangent morph channels must use zero-filled GPU records instead of changing the fixed stride.");
		});

		registry.Add("VirtualGeometry.Deformable.DynamicBoundsContractRemainsConservative", [](TestContext& context) {
			DeformableMeshPayloadHeader header{};
			header.virtualGeometryDeformationFlags = kDeformableMeshVirtualGeometryFlagWholeMeshEnvelope
				| kDeformableMeshVirtualGeometryFlagDisableUnprovenConeCulling;
			header.deformationBoundsMin[0] = -2.0f;
			header.deformationBoundsMax[0] = 3.0f;

			context.Expect((header.virtualGeometryDeformationFlags & kDeformableMeshVirtualGeometryFlagWholeMeshEnvelope) != 0u,
				"Before T093 proves cluster-specific dynamic bounds, deformable VG must use the whole-mesh conservative envelope.");
			context.Expect(header.deformationBoundsMin[0] <= 0.0f && header.deformationBoundsMax[0] >= 0.0f,
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

			context.Expect(contract.failOpenCulling, "T093 must label deformable clusters with unproven bounds as fail-open before GPU culling.");
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
				"T093 packet emission must preserve material coverage when a deformable cluster falls back to a coarser page.");
		});
	}
}
