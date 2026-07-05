#include "TestHarness.h"

#include "Renderer/RenderSystem.h"
#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderGraph.h"
#include "Renderer/RHI/Vulkan/VulkanUploadPlanning.h"
#include "File Formats/TerraEngineMeshFile.h"

#include <algorithm>

namespace TerraEngine::Tests
{
	namespace
	{
		VirtualGeometryAssetRuntime MakeMixedClusterAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.clusterCoverageValidated = true;
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.02f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 128u, 0.03f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			asset.cullableGroups.resize(28u);

			GeometryPageRuntime rootPage{};
			rootPage.pageId = 0u;
			rootPage.firstLodIndex = 1u;
			rootPage.lodCount = 1u;
			rootPage.groupOffset = 16u;
			rootPage.groupCount = 8u;
			rootPage.state = GeometryPageState::Resident;
			rootPage.fallbackPageId = 0u;
			rootPage.fallbackPage = true;
			GeometryPageRuntime nearPage{};
			nearPage.pageId = 1u;
			nearPage.firstLodIndex = 0u;
			nearPage.lodCount = 1u;
			nearPage.groupOffset = 0u;
			nearPage.groupCount = 16u;
			nearPage.state = GeometryPageState::Resident;
			nearPage.fallbackPageId = 0u;
			GeometryPageRuntime farPage{};
			farPage.pageId = 2u;
			farPage.firstLodIndex = 2u;
			farPage.lodCount = 1u;
			farPage.groupOffset = 24u;
			farPage.groupCount = 4u;
			farPage.state = GeometryPageState::Resident;
			farPage.fallbackPageId = 0u;
			asset.pages = { rootPage, nearPage, farPage };

			VirtualGeometryClusterRuntime root{};
			root.clusterId = 0u;
			root.parentClusterId = kVirtualGeometryInvalidIndex;
			root.firstChildClusterId = 1u;
			root.childCount = 2u;
			root.lodIndex = 1u;
			root.firstGroupIndex = 16u;
			root.groupCount = 8u;
			root.pageId = 0u;
			root.fallbackClusterId = 0u;
			root.geometricError = 0.02f;
			root.boundsRadius = 2.0f;
			VirtualGeometryClusterRuntime nearChild{};
			nearChild.clusterId = 1u;
			nearChild.parentClusterId = 0u;
			nearChild.firstChildClusterId = kVirtualGeometryInvalidIndex;
			nearChild.childCount = 0u;
			nearChild.lodIndex = 0u;
			nearChild.firstGroupIndex = 0u;
			nearChild.groupCount = 16u;
			nearChild.pageId = 1u;
			nearChild.fallbackClusterId = 0u;
			nearChild.geometricError = 0.0f;
			nearChild.boundsRadius = 1.0f;
			VirtualGeometryClusterRuntime farChild{};
			farChild.clusterId = 2u;
			farChild.parentClusterId = 0u;
			farChild.firstChildClusterId = kVirtualGeometryInvalidIndex;
			farChild.childCount = 0u;
			farChild.lodIndex = 2u;
			farChild.firstGroupIndex = 24u;
			farChild.groupCount = 4u;
			farChild.pageId = 2u;
			farChild.fallbackClusterId = 0u;
			farChild.geometricError = 0.01f;
			farChild.boundsRadius = 0.25f;
			asset.clusters = { root, nearChild, farChild };
			return asset;
		}

		bool SelectionContainsCluster(const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult& result, std::uint32_t clusterId)
		{
			return std::any_of(result.selectedClusters.begin(), result.selectedClusters.end(), [clusterId](const TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord& record) {
				return record.clusterId == clusterId;
			});
		}

		VirtualGeometryAssetRuntime MakeTwoRegionClusterAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.clusterCoverageValidated = true;
			asset.detailLevels = {
				{ 0u, 0u, 8u, 512u, 0.0f, 1.0f, glm::vec3(-2.0f), glm::vec3(2.0f) },
				{ 1u, 8u, 8u, 256u, 0.02f, 0.5f, glm::vec3(-2.0f), glm::vec3(2.0f) }
			};
			asset.cullableGroups.resize(16u);

			for (std::uint32_t pageId = 0u; pageId < 2u; ++pageId)
			{
				GeometryPageRuntime page{};
				page.pageId = pageId;
				page.firstLodIndex = pageId;
				page.lodCount = 1u;
				page.groupOffset = pageId == 0u ? 0u : 8u;
				page.groupCount = 8u;
				page.state = GeometryPageState::Resident;
				page.fallbackPageId = 0u;
				page.fallbackPage = pageId == 0u;
				asset.pages.push_back(page);
			}

			VirtualGeometryClusterRuntime nearRoot{};
			nearRoot.clusterId = 0u;
			nearRoot.parentClusterId = kVirtualGeometryInvalidIndex;
			nearRoot.firstChildClusterId = 2u;
			nearRoot.childCount = 1u;
			nearRoot.lodIndex = 1u;
			nearRoot.firstGroupIndex = 8u;
			nearRoot.groupCount = 4u;
			nearRoot.pageId = 1u;
			nearRoot.fallbackClusterId = 0u;
			nearRoot.geometricError = 0.02f;
			nearRoot.boundsCenter = glm::vec3(-1.0f, 0.0f, -5.0f);
			nearRoot.boundsRadius = 1.0f;

			VirtualGeometryClusterRuntime farRoot = nearRoot;
			farRoot.clusterId = 1u;
			farRoot.firstChildClusterId = 3u;
			farRoot.firstGroupIndex = 12u;
			farRoot.boundsCenter = glm::vec3(1.0f, 0.0f, -200.0f);

			VirtualGeometryClusterRuntime nearChild{};
			nearChild.clusterId = 2u;
			nearChild.parentClusterId = 0u;
			nearChild.firstChildClusterId = kVirtualGeometryInvalidIndex;
			nearChild.childCount = 0u;
			nearChild.lodIndex = 0u;
			nearChild.firstGroupIndex = 0u;
			nearChild.groupCount = 4u;
			nearChild.pageId = 0u;
			nearChild.fallbackClusterId = 0u;
			nearChild.geometricError = 0.0f;
			nearChild.boundsCenter = glm::vec3(-1.0f, 0.0f, -5.0f);
			nearChild.boundsRadius = 1.0f;

			VirtualGeometryClusterRuntime farChild = nearChild;
			farChild.clusterId = 3u;
			farChild.parentClusterId = 1u;
			farChild.firstGroupIndex = 4u;
			farChild.boundsCenter = glm::vec3(1.0f, 0.0f, -200.0f);
			asset.clusters = { nearRoot, farRoot, nearChild, farChild };
			return asset;
		}
	}

	void RegisterVirtualGeometryRenderTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.Render.US1.GpuPacketDescriptorsPreserveMaterialAndFallbackRuns", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialA = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialB = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> cpuPackets;
			for (std::uint32_t index = 0u; index < 6u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.viewId = 1u;
				packet.viewType = RenderViewType::Game;
				packet.objectId = TerraEngine::GenerateUUID();
				packet.meshId = meshId;
				packet.materialId = index < 2u ? materialA : materialB;
				packet.selectedLod = 1u;
				packet.clusterId = index;
				packet.meshletOffset = index * 8u;
				packet.meshletCount = 8u;
				packet.pageId = index == 5u ? 99u : 4u;
				packet.fallbackPageId = index == 5u ? 4u : 0u;
				packet.fallbackPacket = index == 5u;
				cpuPackets.push_back(packet);
			}

			const std::vector<GpuVirtualGeometryPacketRecord> gpuPackets =
				RenderSystem::BuildGpuVirtualGeometryPacketRecords(
					cpuPackets,
					9000u,
					VirtualGeometryDebugVisualizationMode::Residency);
			const std::vector<GpuVirtualGeometryBatchDescriptor> batches =
				RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(
					gpuPackets,
					9000u,
					VirtualGeometryDebugVisualizationMode::Residency);

			context.ExpectEqual(static_cast<std::size_t>(3u), batches.size(), "Multi-material GPU packet descriptors should split into material A, material B, and fallback runs.");
			context.ExpectEqual(2u, batches[0].packetCount, "Material A packets should remain one contiguous run.");
			context.ExpectEqual(3u, batches[1].packetCount, "Material B resident packets should remain one contiguous run.");
			context.ExpectEqual(1u, batches[2].packetCount, "Fallback packet coverage should split from resident packets even with the same material.");
			context.Expect(materialA == batches[0].materialId, "First GPU batch should keep material A.");
			context.Expect(materialB == batches[1].materialId, "Second GPU batch should keep material B.");
			context.Expect((batches[2].flags & 1u) != 0u, "Fallback GPU batch should carry fail-open packet flags.");
		});

		registry.Add("VirtualGeometry.Render.US2.GpuPacketsValidateAgainstCpuClusterOracle", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			std::vector<TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord> selectedClusters;
			std::vector<VirtualGeometryClusterPacket> cpuPackets;
			for (std::uint32_t index = 0u; index < 3u; ++index)
			{
				TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord selected;
				selected.objectId = objectId;
				selected.meshId = meshId;
				selected.clusterId = 10u + index;
				selected.lodIndex = index % 2u;
				selected.pageId = index + 1u;
				selected.fallbackClusterId = 0u;
				selected.usingFallback = index == 2u;
				selectedClusters.push_back(selected);

				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.viewId = 4u;
				packet.viewType = RenderViewType::Game;
				packet.objectId = objectId;
				packet.meshId = meshId;
				packet.materialId = materialId;
				packet.selectedLod = selected.lodIndex;
				packet.clusterId = selected.clusterId;
				packet.meshletOffset = index * 12u;
				packet.meshletCount = 12u;
				packet.pageId = selected.pageId;
				packet.fallbackPageId = selected.usingFallback ? 1u : 0u;
				packet.fallbackPacket = selected.usingFallback;
				packet.debugIdentity = 900u + index;
				cpuPackets.push_back(packet);
			}

			std::vector<GpuVirtualGeometryPacketRecord> gpuPackets =
				RenderSystem::BuildGpuVirtualGeometryPacketRecords(cpuPackets, 8800u, VirtualGeometryDebugVisualizationMode::Cluster);
			std::string validationReason;
			context.Expect(
				RenderSystem::ValidateGpuVirtualGeometryPacketRecords(cpuPackets, selectedClusters, gpuPackets, 8800u, VirtualGeometryDebugVisualizationMode::Cluster, &validationReason),
				"GPU packet mirrors should validate when every record maps back to the CPU-selected cluster oracle.");
			context.Expect(validationReason.empty(), "Successful GPU packet validation should not leave a stale reason string.");

			gpuPackets[1].clusterId += 100u;
			context.Expect(
				!RenderSystem::ValidateGpuVirtualGeometryPacketRecords(cpuPackets, selectedClusters, gpuPackets, 8800u, VirtualGeometryDebugVisualizationMode::Cluster, &validationReason),
				"Changing GPU packet cluster identity should fail validation before Vulkan publication.");
			context.Expect(validationReason.find("cluster") != std::string::npos, "Validation failure should explain that cluster identity drifted.");
		});

		registry.Add("VirtualGeometry.Render.US2.SnapshotPreservesGpuPacketMaterialFallbackAndRangeIdentity", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot;
			snapshot.frameId = 8801u;
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 12u;
			state.targetSize = glm::uvec2(1920u, 1080u);
			state.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::Residency;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			VirtualGeometryClusterPacket packet;
			packet.packetId = 0u;
			packet.viewId = state.outputId;
			packet.viewType = state.viewType;
			packet.objectId = objectId;
			packet.meshId = meshId;
			packet.materialId = materialId;
			packet.selectedLod = 2u;
			packet.clusterId = 77u;
			packet.meshletOffset = 144u;
			packet.meshletCount = 9u;
			packet.pageId = 5u;
			packet.fallbackPageId = 1u;
			packet.fallbackPacket = true;
			state.clusterPackets.push_back(packet);
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(state.clusterPackets, snapshot.frameId, state.debugVisualizationMode);
			state.gpuBatchDescriptors = RenderSystem::BuildGpuVirtualGeometryBatchDescriptors(state.gpuPacketRecords, snapshot.frameId, state.debugVisualizationMode);
			state.gpuPacketFrameSets.push_back(RenderSystem::BuildGpuVirtualGeometryPacketFrameSet(state.viewType, state.outputId, snapshot.frameId, state.targetSize, state.debugVisualizationMode, 0u, 1u, 0u, 1u));
			snapshot.virtualGeometryViewSelections.push_back(state);

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);

			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().gpuPacketRecords.size(), "Visibility snapshot should preserve one GPU packet record.");
			context.Expect(materialId == visibility.front().gpuPacketRecords.front().materialId, "GPU packet snapshot should preserve material identity.");
			context.ExpectEqual(144u, visibility.front().gpuPacketRecords.front().meshletOffset, "GPU packet snapshot should preserve meshlet offset.");
			context.ExpectEqual(9u, visibility.front().gpuPacketRecords.front().meshletCount, "GPU packet snapshot should preserve meshlet count.");
			context.Expect((visibility.front().gpuPacketRecords.front().flags & 1u) != 0u, "GPU packet snapshot should preserve fallback substitution state.");
			context.ExpectEqual(2u, visibility.front().gpuPacketRecords.front().selectedLod, "GPU packet snapshot should preserve selected LOD.");
		});

		registry.Add("VirtualGeometry.Render.US1.HighDetailResidentPagesBecomeEmittedSources", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			for (std::uint32_t index = 0u; index < static_cast<std::uint32_t>(asset.cullableGroups.size()); ++index)
			{
				asset.cullableGroups[index].meshletIndex = index;
			}
			asset.pages[1].payloadByteOffset = 4096u;
			asset.pages[1].payloadByteSize = 2048u;
			asset.pages[1].byteSize = 2048u;

			VirtualGeometrySelectedClusterRecord selected;
			selected.objectId = TerraEngine::GenerateUUID();
			selected.meshId = asset.assetId;
			selected.clusterId = 1u;
			selected.lodIndex = 0u;
			selected.pageId = 1u;
			selected.fallbackClusterId = 0u;
			selected.usingFallback = false;

			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const std::vector<VirtualGeometryClusterPacket> packets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, { selected }, materialId, RenderViewType::Game, 5u);
			context.ExpectEqual(static_cast<std::size_t>(1u), packets.size(), "A resident high-detail selected cluster should produce one packet.");
			context.Expect(packets.front().payloadSource == VirtualGeometryPagePayloadSource::ResidentPagePayload, "High-detail resident pages should be labeled as resident page payloads.");
			context.Expect(packets.front().emittedSource == VirtualGeometryEmittedPageSource::HighDetailPage, "High-detail resident pages should emit high-detail source records.");
			context.ExpectEqual(4096ull, packets.front().pagePayloadByteOffset, "High-detail packet should preserve page-local byte offset.");
			context.ExpectEqual(2048ull, packets.front().pagePayloadByteSize, "High-detail packet should preserve page-local byte size.");

			RenderFrameSnapshot snapshot;
			snapshot.frameId = 8802u;
			VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 5u;
			state.clusterPackets = packets;
			state.gpuPacketRecords = RenderSystem::BuildGpuVirtualGeometryPacketRecords(packets, snapshot.frameId, VirtualGeometryDebugVisualizationMode::Residency);
			VirtualGeometryPagePayloadRecord payload;
			payload.meshId = asset.assetId;
			payload.pageId = packets.front().pageId;
			payload.fallbackPageId = packets.front().fallbackPageId;
			payload.source = packets.front().payloadSource;
			payload.payloadByteOffset = packets.front().pagePayloadByteOffset;
			payload.payloadByteSize = packets.front().pagePayloadByteSize;
			payload.resident = true;
			payload.valid = true;
			state.residentPagePayloads.push_back(payload);
			VirtualGeometryEmittedPageSourceRecord emitted;
			emitted.viewType = state.viewType;
			emitted.viewId = state.outputId;
			emitted.frameId = snapshot.frameId;
			emitted.objectId = selected.objectId;
			emitted.meshId = asset.assetId;
			emitted.materialId = materialId;
			emitted.clusterId = selected.clusterId;
			emitted.selectedLod = selected.lodIndex;
			emitted.pageId = packets.front().pageId;
			emitted.fallbackPageId = packets.front().fallbackPageId;
			emitted.payloadSource = packets.front().payloadSource;
			emitted.emittedSource = packets.front().emittedSource;
			emitted.payloadByteOffset = packets.front().pagePayloadByteOffset;
			emitted.payloadByteSize = packets.front().pagePayloadByteSize;
			state.emittedPageSources.push_back(emitted);
			snapshot.virtualGeometryViewSelections.push_back(state);

			const VirtualGeometryViewVisibilitySnapshot visibility = BuildVirtualGeometryViewVisibilitySnapshot(snapshot,
				snapshot.virtualGeometryViewSelections.front(),
				VirtualGeometryCounterSource::Prepared);
			const VirtualGeometryViewDiagnosticsSnapshot diagnostics = BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);
			context.ExpectEqual(1u, diagnostics.pageSources.highDetailPayloads, "Diagnostics should count the resident high-detail page payload.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedHighDetailSources, "Diagnostics should report high-detail page emission.");
			context.ExpectEqual(0u, diagnostics.embeddedFallbackPayloadCount, "High-detail page emission should not be reported as conservative embedded fallback.");
		});

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

		registry.Add("VirtualGeometry.Render.Residency.ChoosesBestResidentCoarserPage", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.detailLevels = {
				{ 0u, 0u, 16u, 1024u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 16u, 8u, 512u, 0.01f, 0.5f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 2u, 24u, 4u, 256u, 0.02f, 0.25f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 3u, 28u, 2u, 128u, 0.03f, 0.125f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			GeometryPageRuntime fallback{};
			fallback.pageId = 0u;
			fallback.firstLodIndex = 0u;
			fallback.lodCount = 1u;
			fallback.state = GeometryPageState::Resident;
			fallback.fallbackPage = true;
			GeometryPageRuntime mid{};
			mid.pageId = 2u;
			mid.firstLodIndex = 2u;
			mid.lodCount = 1u;
			mid.state = GeometryPageState::Resident;
			GeometryPageRuntime missingHigh{};
			missingHigh.pageId = 3u;
			missingHigh.firstLodIndex = 3u;
			missingHigh.lodCount = 1u;
			missingHigh.state = GeometryPageState::Failed;
			missingHigh.fallbackPageId = 99u;
			asset.pages = { fallback, mid, missingHigh };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.maxLodDrop = 3u;
			const TerraEngine::Renderer::VirtualGeometryDrawRange range =
				RenderSystem::SelectVirtualGeometryResidentLodRange(asset, quality, 64.0f, true, true);

			context.Expect(range.failOpen, "Missing desired page should fail open through resident fallback data.");
			context.ExpectEqual(2u, range.selectedLodIndex, "Renderer should choose the best resident coarser page instead of always dropping to LOD0.");
			context.Expect(range.fallbackReason.find("best resident fallback") != std::string::npos, "Fallback reason should identify best resident fallback selection.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.SelectsMixedDetailWithinOneObject", [](TestContext& context) {
			const VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(asset, meshId, objectId, quality, 512.0f, true, true);

			context.ExpectEqual(static_cast<std::size_t>(2u), result.selectedClusters.size(), "Near projected size should refine the root into its child cluster cut.");
			context.Expect(SelectionContainsCluster(result, 1u), "The near child should keep high-detail LOD0 inside the object.");
			context.Expect(SelectionContainsCluster(result, 2u), "The far/small child should keep a lower-detail cluster inside the same object.");
			context.Expect(!SelectionContainsCluster(result, 0u), "A refined cluster cut should not also include the parent root.");
			context.ExpectEqual(2u, result.range.selectedClusterCount, "Draw range should expose the selected cluster count for shader payload binding.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.FailsOpenWhenCoverageProofIsMissing", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			asset.clusterCoverageValidated = false;

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					asset.assetId,
					TerraEngine::GenerateUUID(),
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					true,
					true);

			context.Expect(result.range.failOpen, "Unsafe mixed-detail metadata should fail open instead of selecting partial child coverage.");
			context.Expect(result.selectedClusters.empty(), "Missing coverage proof should prevent sparse mixed-detail cluster dispatch.");
			context.ExpectEqual(0u, result.range.selectedLodIndex, "Fail-open coverage safety should use the complete highest-detail range.");
			context.ExpectEqual(asset.detailLevels.front().meshletCount, result.range.meshletCount, "Fail-open coverage safety should draw every meshlet in the complete highest-detail range.");
			context.Expect(result.range.fallbackReason.find("cluster coverage validation missing") != std::string::npos, "Fail-open reason should identify the missing coverage proof.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.UsesPerClusterScreenErrorWithinOneObject", [](TestContext& context) {
			const VirtualGeometryAssetRuntime asset = MakeTwoRegionClusterAsset();
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					asset.assetId,
					TerraEngine::GenerateUUID(),
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(1000u, 1000u),
					true,
					true);

			context.Expect(SelectionContainsCluster(result, 2u), "The nearby region should refine to its high-detail child cluster.");
			context.Expect(SelectionContainsCluster(result, 1u), "The far region should remain at its coarser root cluster.");
			context.Expect(!SelectionContainsCluster(result, 0u), "The refined nearby parent should not overlap its child.");
			context.Expect(!SelectionContainsCluster(result, 3u), "The distant child should stay unrefined because its own screen error is small.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.DoesNotPublishContiguousEnvelopeRange", [](TestContext& context) {
			const VirtualGeometryAssetRuntime asset = MakeTwoRegionClusterAsset();
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					asset.assetId,
					TerraEngine::GenerateUUID(),
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(1000u, 1000u),
					true,
					true);

			context.ExpectEqual(static_cast<std::size_t>(2u), result.selectedClusters.size(), "Fixture should select two sparse clusters before checking the dispatched range.");
			context.Expect(result.range.meshletCount != 16u, "Cluster selection must not publish the old min/max whole-object envelope.");
			context.Expect(result.range.selectedClusterCount == 2u, "Selection metadata should still expose the selected sparse cluster count.");
		});

		registry.Add("VirtualGeometry.Render.Diagnostics.CountsSparseClusterDrawRanges", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			snapshot.frameId = 123u;
			snapshot.renderSettings.virtualGeometry = MakeDefaultVirtualGeometryQualityProfile();
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.targetSize = glm::uvec2(1000u, 1000u);
			state.hiZOcclusionAllowed = true;
			state.selectedClusters = {
				{ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), 2u, 0u, 0u, 0u, false },
				{ TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), 1u, 1u, 1u, 0u, false }
			};
			TerraEngine::Renderer::VirtualGeometryDrawRange nearRange{};
			nearRange.selectedLodIndex = 0u;
			nearRange.meshletOffset = 0u;
			nearRange.meshletCount = 4u;
			nearRange.virtualGeometryActive = true;
			nearRange.selectedClusterOffset = 0u;
			nearRange.selectedClusterCount = 1u;
			TerraEngine::Renderer::VirtualGeometryDrawRange farRange = nearRange;
			farRange.selectedLodIndex = 1u;
			farRange.meshletOffset = 12u;
			farRange.selectedClusterOffset = 1u;
			state.draws.push_back({ TerraEngine::GenerateUUID(), state.selectedClusters[0].meshId, nearRange, true });
			state.draws.push_back({ TerraEngine::GenerateUUID(), state.selectedClusters[1].meshId, farRange, true });
			snapshot.virtualGeometryViewSelections.push_back(std::move(state));

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot> diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryViewDiagnosticsFromSnapshot(
					snapshot,
					snapshot.renderSettings.virtualGeometry,
					VirtualGeometryInactiveReason::None);

			context.ExpectEqual(static_cast<std::size_t>(1u), diagnostics.size(), "Explicit sparse selection state should produce one diagnostics snapshot.");
			context.ExpectEqual(8u, diagnostics.front().selectedMeshlets, "Diagnostics should sum per-cluster draw ranges, not a contiguous envelope.");
			context.ExpectEqual(1u, diagnostics.front().selectedLodHistogram[0], "Sparse draw diagnostics should count the high-detail cluster draw.");
			context.ExpectEqual(1u, diagnostics.front().selectedLodHistogram[1], "Sparse draw diagnostics should count the coarse cluster draw.");
		});

		registry.Add("VirtualGeometry.Render.US1.MultiMaterialPacketsRemainSeparateBatches", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialA = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialB = TerraEngine::GenerateUUID();
			std::vector<VirtualGeometryClusterPacket> packets;
			for (std::uint32_t index = 0u; index < 4u; ++index)
			{
				VirtualGeometryClusterPacket packet;
				packet.packetId = index;
				packet.meshId = meshId;
				packet.materialId = index < 2u ? materialA : materialB;
				packet.selectedLod = 0u;
				packet.clusterId = index;
				packet.meshletOffset = index * 6u;
				packet.meshletCount = 6u;
				packets.push_back(packet);
			}

			const std::vector<ClusterWorkBatch> batches = RenderSystem::BuildVirtualGeometryClusterBatches(packets);

			context.ExpectEqual(static_cast<std::size_t>(2u), batches.size(), "Batching must split contiguous packets when material bindings differ.");
			context.ExpectEqual(2u, batches[0].packetCount, "First material should keep its contiguous packet run.");
			context.ExpectEqual(2u, batches[1].packetCount, "Second material should keep its contiguous packet run.");
			context.Expect(batches[0].materialId != batches[1].materialId, "Submitted batches should retain separate material ownership.");
		});

		registry.Add("VirtualGeometry.Render.US2.CompactionRejectsOffscreenButFailsOpenMissingResources", [](TestContext& context) {
			VirtualGeometryClusterPacket offscreen;
			offscreen.packetId = 0u;
			offscreen.clusterId = 7u;
			offscreen.meshletOffset = 0u;
			offscreen.meshletCount = 4u;
			offscreen.projectedBounds = glm::vec4(-0.4f, 0.1f, -0.1f, 0.3f);
			std::vector<VirtualGeometryClusterPacket> compacted;
			const CompactedVisibilitySet offscreenResult =
				RenderSystem::CompactVirtualGeometryClusterPackets({ offscreen }, compacted, RenderViewType::Game, 2u);

			context.ExpectEqual(0u, offscreenResult.visiblePackets, "Fully offscreen packets should be rejected before submission.");
			context.ExpectEqual(1u, offscreenResult.rejectedByFrustum, "Offscreen packet rejection should be classified as frustum culling.");
			context.Expect(compacted.empty(), "Frustum-rejected packets should not be submitted.");

			VirtualGeometryClusterPacket missingResource = offscreen;
			missingResource.packetId = 1u;
			missingResource.projectedBounds = glm::vec4(0.1f, 0.1f, 0.2f, 0.2f);
			missingResource.meshletCount = 0u;
			const CompactedVisibilitySet failOpenResult =
				RenderSystem::CompactVirtualGeometryClusterPackets({ missingResource }, compacted, RenderViewType::Game, 2u);

			context.ExpectEqual(1u, failOpenResult.visiblePackets, "Missing-resource-only packets should fail open to avoid missing geometry.");
			context.ExpectEqual(1u, failOpenResult.failOpenPackets, "Fail-open packet count should explain the restored submission.");
			context.ExpectEqual(static_cast<std::size_t>(1u), compacted.size(), "Fail-open packets should be submitted after restoration.");
		});

		registry.Add("VirtualGeometry.Render.US3.FallbackCoverageChainsPublishThroughSnapshots", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameSnapshot snapshot{};
			TerraEngine::Renderer::VirtualGeometryViewSelectionState state{};
			state.viewType = RenderViewType::Game;
			state.outputId = 5u;
			FallbackCoverageChain chain;
			chain.meshId = TerraEngine::GenerateUUID();
			chain.desiredClusterId = 8u;
			chain.fallbackClusterId = 2u;
			chain.fallbackPageId = 0u;
			chain.coverageStatus = VirtualGeometryFallbackCoverageStatus::Complete;
			chain.substitutedMeshlets = 6u;
			chain.reason = "fixture fallback coverage";
			state.fallbackCoverage.push_back(chain);
			snapshot.virtualGeometryViewSelections.push_back(state);

			const std::vector<TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot> visibility =
				TerraEngine::Renderer::BuildVirtualGeometryViewVisibilitySnapshots(snapshot);

			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.size(), "Fallback coverage selection should publish one visibility snapshot.");
			context.ExpectEqual(static_cast<std::size_t>(1u), visibility.front().fallbackCoverage.size(), "Fallback coverage chains should survive immutable snapshot copy.");
			context.ExpectEqual(8u, visibility.front().fallbackCoverage.front().desiredClusterId, "Fallback coverage should identify the missing desired cluster.");
			context.ExpectEqual(6u, visibility.front().fallbackCoverage.front().substitutedMeshlets, "Fallback coverage should report substituted meshlets.");
		});

		registry.Add("VirtualGeometry.Render.US4.MixedDetailClusterCutBuildsPageLocalPackets", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeTwoRegionClusterAsset();
			asset.pages[0].payloadByteOffset = 4096u;
			asset.pages[0].byteSize = 1536u;
			asset.pages[1].payloadByteOffset = 16384u;
			asset.pages[1].byteSize = 2048u;
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					asset.assetId,
					TerraEngine::GenerateUUID(),
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f),
					glm::uvec2(1000u, 1000u),
					true,
					true);
			const std::vector<VirtualGeometryClusterPacket> packets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, result.selectedClusters, materialId, RenderViewType::Game, 2u);

			const auto findPacket = [&packets](std::uint32_t clusterId) {
				return std::find_if(packets.begin(), packets.end(), [clusterId](const VirtualGeometryClusterPacket& packet) {
					return packet.clusterId == clusterId;
				});
			};
			const auto nearPacket = findPacket(2u);
			const auto farPacket = findPacket(1u);

			context.ExpectEqual(static_cast<std::size_t>(2u), result.selectedClusters.size(), "Mixed-detail selection should keep one sparse cluster per visible region.");
			context.Expect(nearPacket != packets.end(), "Nearby region should emit its high-detail child packet.");
			context.Expect(farPacket != packets.end(), "Distant region should emit its lower-detail root packet.");
			context.Expect(nearPacket != packets.end() && farPacket != packets.end() && nearPacket->selectedLod != farPacket->selectedLod, "Packets should preserve per-cluster detail selection instead of substituting one whole-object LOD.");
			context.Expect(nearPacket != packets.end() && farPacket != packets.end() && nearPacket->pageId != farPacket->pageId, "Packets should keep page-local identity for each selected cluster.");
			context.Expect(nearPacket != packets.end() && nearPacket->pagePayloadByteOffset == 4096u, "High-detail packet should point at the resident high-detail page byte range.");
			context.Expect(farPacket != packets.end() && farPacket->pagePayloadByteOffset == 16384u, "Coarse packet should point at its own resident page byte range.");
		});

		registry.Add("VirtualGeometry.Render.US4.PagePayloadIdentityInvalidatesPreparedSignatures", [](TestContext& context) {
			TerraEngine::Renderer::RenderFrameRenderableRecord record{};
			record.objectId = TerraEngine::GenerateUUID();
			record.meshId = TerraEngine::GenerateUUID();
			record.materialId = TerraEngine::GenerateUUID();
			record.virtualGeometry.selectedLodIndex = 0u;
			record.virtualGeometry.meshletOffset = 0u;
			record.virtualGeometry.meshletCount = 8u;
			record.virtualGeometry.virtualGeometryActive = true;
			record.virtualGeometry.pageId = 1u;
			record.virtualGeometry.fallbackPageId = 0u;
			record.virtualGeometry.payloadSource = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			record.virtualGeometry.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			record.virtualGeometry.pagePayloadByteOffset = 2048u;
			record.virtualGeometry.pagePayloadByteSize = 1024u;

			const std::uint64_t highDetailHash = TerraEngine::Renderer::HashPreparedDrawPacketSignature(
				TerraEngine::Renderer::BuildPreparedDrawPacketSignature(record));
			record.virtualGeometry.payloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			record.virtualGeometry.emittedSource = VirtualGeometryEmittedPageSource::FallbackPage;
			record.virtualGeometry.pageId = 0u;
			record.virtualGeometry.pagePayloadByteOffset = 8192u;
			const std::uint64_t fallbackHash = TerraEngine::Renderer::HashPreparedDrawPacketSignature(
				TerraEngine::Renderer::BuildPreparedDrawPacketSignature(record));

			context.Expect(highDetailHash != fallbackHash, "Prepared visible-command signatures should change when a draw swaps resident page payload identity.");
		});

		registry.Add("VirtualGeometry.Render.US4.DiagnosticsCountMaterialPageBoundaries", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.valid = true;
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;
			visibility.frameId = 333u;
			VirtualGeometryEmittedPageSourceRecord materialA{};
			materialA.materialId = TerraEngine::GenerateUUID();
			materialA.pageId = 4u;
			materialA.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			VirtualGeometryEmittedPageSourceRecord materialB = materialA;
			materialB.materialId = TerraEngine::GenerateUUID();
			visibility.emittedPageSources = { materialA, materialB };

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(2u, diagnostics.pageSources.emittedMaterialPagePairs, "Diagnostics should preserve both material/page pairs for page-local clusters.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedMultiMaterialPages, "Diagnostics should identify a page that emitted multiple material sections.");
		});

		registry.Add("VirtualGeometry.Render.US4.ZeroSurvivorGeneratedSubmissionsRemainValidZeroWork", [](TestContext& context) {
			const std::vector<GpuVirtualGeometryBatchDescriptor> batches;
			const std::vector<GpuVirtualGeometryPacketRecord> packets;
			const std::vector<GeneratedVirtualGeometrySubmissionRecord> submissions =
				RenderSystem::BuildGeneratedVirtualGeometrySubmissionRecords(batches, packets, 10002u);

			std::string validationReason;
			context.Expect(submissions.empty(), "Zero-survivor compaction should produce no generated submission records.");
			context.Expect(
				RenderSystem::ValidateGeneratedVirtualGeometrySubmissionRecords(submissions, batches, 10002u, RenderViewType::Game, 2u, 0u, &validationReason),
				"Zero-survivor generated submissions should validate as explicit zero work when packet capacity is also zero.");
			context.Expect(validationReason.empty(), "Valid zero-work generated submissions should not publish a fallback reason.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.EnforcesParentChildExclusivity", [](TestContext& context) {
			const VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult farResult =
				RenderSystem::SelectVirtualGeometryClusterCut(asset, TerraEngine::GenerateUUID(), TerraEngine::GenerateUUID(), MakeDefaultVirtualGeometryQualityProfile(), 16.0f, true, true);

			context.ExpectEqual(static_cast<std::size_t>(1u), farResult.selectedClusters.size(), "Small projected size should keep the coarse parent cluster.");
			context.Expect(SelectionContainsCluster(farResult, 0u), "Coarse selection should use the root cluster.");
			context.Expect(!SelectionContainsCluster(farResult, 1u), "Coarse selection should not overlap root coverage with child cluster 1.");
			context.Expect(!SelectionContainsCluster(farResult, 2u), "Coarse selection should not overlap root coverage with child cluster 2.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.UsesResidentFallbackAncestorForMissingChildPage", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			asset.pages[1].state = GeometryPageState::Failed;
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(asset, meshId, objectId, MakeDefaultVirtualGeometryQualityProfile(), 512.0f, true, true);

			context.ExpectEqual(static_cast<std::size_t>(1u), result.selectedClusters.size(), "A fallback ancestor should suppress overlapping sibling child selections.");
			context.Expect(SelectionContainsCluster(result, 0u), "Missing high-detail child page should fall back to the resident root cluster.");
			context.Expect(result.selectedClusters.front().usingFallback, "Fallback ancestor selection should be visible in diagnostics.");
			context.ExpectEqual(static_cast<std::size_t>(1u), result.pageNeeds.size(), "Desired non-resident child page should generate one page need request.");
			context.ExpectEqual(1u, result.pageNeeds.front().clusterId, "Page need should name the desired child cluster.");
			context.Expect(result.pageNeeds.front().fallbackRequired, "Page need should mark that fallback is currently preserving visibility.");
		});

		registry.Add("VirtualGeometry.Render.ClusterSelection.UsesFallbackAncestorWhileChildPageIsEvicting", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset = MakeMixedClusterAsset();
			asset.pages[1].state = GeometryPageState::Evicting;
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID objectId = TerraEngine::GenerateUUID();

			const TerraEngine::Renderer::VirtualGeometryClusterSelectionResult result =
				RenderSystem::SelectVirtualGeometryClusterCut(asset, meshId, objectId, MakeDefaultVirtualGeometryQualityProfile(), 512.0f, true, true);

			context.ExpectEqual(static_cast<std::size_t>(1u), result.selectedClusters.size(), "Evicting detail pages should be treated as unavailable during render preparation.");
			context.Expect(SelectionContainsCluster(result, 0u), "Resident fallback ancestor should keep the object visible while high-detail page retirement is pending.");
			context.Expect(result.selectedClusters.front().usingFallback, "Fallback substitution should be visible to diagnostics while the child page is evicting.");
			context.ExpectEqual(static_cast<std::size_t>(1u), result.pageNeeds.size(), "Evicting desired page should publish a page need so streaming can reload detail later.");
			context.ExpectEqual(1u, result.pageNeeds.front().pageId, "Reload request should target the evicting high-detail child page.");
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
			GeometryPageRuntime evicting{};
			evicting.pageId = 3u;
			evicting.state = GeometryPageState::Evicting;
			evicting.byteSize = 512u;
			asset.pages = { resident, requested, failed, evicting };

			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryLodQualityDiagnostics(RenderViewType::Game, 99u, { asset }, quality);

			context.ExpectEqual(1u, snapshot.residentPages, "Diagnostics should count resident virtual geometry pages.");
			context.ExpectEqual(1u, snapshot.requestedPages, "Diagnostics should count requested or loading virtual geometry pages.");
			context.ExpectEqual(1u, snapshot.failedPages, "Diagnostics should count failed virtual geometry pages.");
			context.ExpectEqual(1u, snapshot.evictingPages, "Diagnostics should count pages waiting for protected GPU retirement.");
			context.ExpectEqual(2u, snapshot.missingPages, "Diagnostics should count missing, failed, and evicting virtual geometry pages.");
			context.Expect(snapshot.estimatedGeometryBytes >= 1792u, "Diagnostics should include page byte estimates in geometry memory.");
			context.Expect(!snapshot.warnings.empty(), "Missing pages should surface a visible diagnostics warning.");
		});

		registry.Add("VirtualGeometry.Render.Vulkan.PageUploadAndRetirementHooksAreConservative", [](TestContext& context) {
			GeometryPageRuntime fallbackUpload{};
			fallbackUpload.pageId = 0u;
			fallbackUpload.state = GeometryPageState::PendingUpload;
			fallbackUpload.byteSize = 256u;
			fallbackUpload.fallbackPage = true;
			const TerraEngine::Renderer::Vulkan::VirtualGeometryPageUploadPublication fallbackPublication =
				TerraEngine::Renderer::Vulkan::PlanVirtualGeometryPageUploadPublication(fallbackUpload);

			GeometryPageRuntime highDetail{};
			highDetail.pageId = 4u;
			highDetail.state = GeometryPageState::Evicting;
			highDetail.byteSize = 1024u;
			const TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan highDetailRetirement =
				TerraEngine::Renderer::Vulkan::PlanVirtualGeometryPageRetirement(highDetail, 40u);

			GeometryPageRuntime protectedFallback = highDetail;
			protectedFallback.pageId = 0u;
			protectedFallback.fallbackPage = true;
			const TerraEngine::Renderer::Vulkan::VirtualGeometryPageRetirementPlan fallbackRetirement =
				TerraEngine::Renderer::Vulkan::PlanVirtualGeometryPageRetirement(protectedFallback, 40u);

			context.Expect(fallbackPublication.accepted, "Pending upload fallback pages should be publishable after protected GPU upload.");
			context.Expect(fallbackPublication.publishedState == GeometryPageState::Pinned, "Uploaded fallback pages should publish as pinned.");
			context.Expect(highDetailRetirement.canRetire, "Evicting high-detail pages should be eligible for deferred retirement.");
			context.ExpectEqual(42u, highDetailRetirement.retireAfterFrameId, "Retirement should wait for the configured completed-frame lag.");
			context.Expect(!fallbackRetirement.canRetire, "Fallback pages should not retire while they protect visibility.");
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
			context.Expect(contains(declaration.reads, TerraEngine::Scheduling::kResourceVirtualGeometryQualityProfile), "RenderSystem should declare virtual geometry quality reads.");
			context.Expect(contains(declaration.reads, TerraEngine::Scheduling::kResourceVirtualGeometryResidencySnapshots), "RenderSystem should keep the compatibility residency snapshot read.");
			context.Expect(contains(declaration.reads, TerraEngine::Scheduling::kResourceVirtualGeometryPageResidencySnapshots), "RenderSystem should declare explicit virtual geometry page residency reads.");
			context.Expect(contains(declaration.reads, TerraEngine::Scheduling::kResourceVirtualGeometryHiZValidationState), "RenderSystem should declare virtual geometry Hi-Z validation reads.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryClusterPackets), "RenderSystem should declare virtual geometry cluster packet writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryBatchPlans), "RenderSystem should declare virtual geometry batch plan writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryCompactionResults), "RenderSystem should declare virtual geometry compaction result writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryPageNeedPackets), "RenderSystem should declare virtual geometry page-need packet writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryVisibilitySnapshots), "RenderSystem should declare virtual geometry visibility snapshot writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryCounterSnapshots), "RenderSystem should declare virtual geometry counter snapshot writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryCullingDiagnostics), "RenderSystem should declare virtual geometry culling diagnostic writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryPerformanceCounters), "RenderSystem should declare virtual geometry performance counter writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryShadowVisibilitySnapshots), "RenderSystem should declare virtual geometry shadow visibility writes.");
			context.Expect(contains(declaration.writes, TerraEngine::Scheduling::kResourceVirtualGeometryDiagnostics), "RenderSystem should declare virtual geometry diagnostic writes.");
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
