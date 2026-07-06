#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RenderSystem.h"
#include "Resource/ResourceStreaming.h"

#include <algorithm>
#include <array>
#include <limits>
#include <utility>

namespace TerraEngine::Tests {
	namespace {
		UUID MakeTestUuid(std::uint64_t lowBits)
		{
			UUID id{};
			id[15] = static_cast<std::uint8_t>(lowBits & 0xffu);
			id[14] = static_cast<std::uint8_t>((lowBits >> 8u) & 0xffu);
			return id;
		}

		VirtualGeometryAssetRuntime BuildFallbackOnlyResidentAsset()
		{
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = MakeTestUuid(11u);
			asset.clusterCoverageValidated = true;
			asset.cullableGroups.resize(4u);
			for (std::uint32_t index = 0u; index < static_cast<std::uint32_t>(asset.cullableGroups.size()); ++index)
			{
				asset.cullableGroups[index].meshletIndex = index;
			}

			GeometryPageRuntime fallbackPage{};
			fallbackPage.pageId = 0u;
			fallbackPage.fallbackPageId = 0u;
			fallbackPage.state = GeometryPageState::Pinned;
			fallbackPage.fallbackPage = true;
			fallbackPage.pinnedFallback = true;
			fallbackPage.payloadByteOffset = 256u;
			fallbackPage.payloadByteSize = 1024u;
			fallbackPage.byteSize = 1024u;
			asset.pages.push_back(fallbackPage);

			VirtualGeometryClusterRuntime fallbackCluster{};
			fallbackCluster.clusterId = 0u;
			fallbackCluster.lodIndex = 1u;
			fallbackCluster.firstGroupIndex = 0u;
			fallbackCluster.groupCount = 4u;
			fallbackCluster.pageId = fallbackPage.pageId;
			fallbackCluster.fallbackClusterId = fallbackCluster.clusterId;
			fallbackCluster.boundsRadius = 1.0f;
			asset.clusters.push_back(fallbackCluster);
			return asset;
		}

		VirtualGeometryAssetRuntime BuildFailedHighDetailWithFallbackAsset()
		{
			VirtualGeometryAssetRuntime asset = BuildFallbackOnlyResidentAsset();
			asset.detailLevels = {
				{ 0u, 4u, 4u, 2048u, 0.0f, 1.0f, glm::vec3(-1.0f), glm::vec3(1.0f) },
				{ 1u, 0u, 4u, 512u, 0.1f, 0.25f, glm::vec3(-1.0f), glm::vec3(1.0f) }
			};
			for (std::uint32_t index = 0u; index < static_cast<std::uint32_t>(asset.cullableGroups.size()); ++index)
			{
				asset.cullableGroups[index].meshletIndex = index;
			}

			GeometryPageRuntime highDetailPage{};
			highDetailPage.pageId = 1u;
			highDetailPage.fallbackPageId = 0u;
			highDetailPage.state = GeometryPageState::Failed;
			highDetailPage.failureReason = "upload validation failed";
			highDetailPage.payloadByteOffset = 4096u;
			highDetailPage.payloadByteSize = 2048u;
			highDetailPage.byteSize = 2048u;
			asset.pages.push_back(highDetailPage);

			asset.clusters.front().firstChildClusterId = 1u;
			asset.clusters.front().childCount = 1u;
			asset.clusters.front().geometricError = 1.0f;

			VirtualGeometryClusterRuntime highDetailCluster{};
			highDetailCluster.clusterId = 1u;
			highDetailCluster.parentClusterId = 0u;
			highDetailCluster.firstChildClusterId = std::numeric_limits<std::uint32_t>::max();
			highDetailCluster.childCount = 0u;
			highDetailCluster.lodIndex = 0u;
			highDetailCluster.firstGroupIndex = 0u;
			highDetailCluster.groupCount = 4u;
			highDetailCluster.pageId = 1u;
			highDetailCluster.fallbackClusterId = 0u;
			highDetailCluster.boundsRadius = 0.5f;
			asset.clusters.push_back(highDetailCluster);
			return asset;
		}

		TerraEngine::Renderer::RenderFrameSnapshot BuildPageStreamingSnapshot(VirtualGeometryPagePayloadSource payloadSource,
			VirtualGeometryEmittedPageSource emittedSource,
			std::uint64_t payloadOffset,
			std::uint64_t payloadBytes)
		{
			using namespace TerraEngine::Renderer;

			RenderFrameSnapshot snapshot;
			snapshot.frameId = 44u;
			snapshot.valid = true;

			VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 7u;

			// The resident payload record is the renderer-side contract for page-local
			// geometry bytes. Tests keep it tiny, but the fields mirror the runtime
			// data that Vulkan and diagnostics consume later in the feature.
			VirtualGeometryPagePayloadRecord payload;
			payload.meshId = MakeTestUuid(1u);
			payload.pageId = 3u;
			payload.fallbackPageId = 1u;
			payload.source = payloadSource;
			payload.selectedLod = 2u;
			payload.meshletOffset = 8u;
			payload.meshletCount = 12u;
			payload.payloadByteOffset = payloadOffset;
			payload.payloadByteSize = payloadBytes;
			payload.resident = true;
			payload.valid = true;
			state.residentPagePayloads.push_back(payload);

			VirtualGeometryEmittedPageSourceRecord emitted;
			emitted.viewType = state.viewType;
			emitted.viewId = state.outputId;
			emitted.frameId = snapshot.frameId;
			emitted.objectId = MakeTestUuid(2u);
			emitted.meshId = payload.meshId;
			emitted.materialId = MakeTestUuid(3u);
			emitted.clusterId = 9u;
			emitted.pageId = payload.pageId;
			emitted.fallbackPageId = payload.fallbackPageId;
			emitted.payloadSource = payloadSource;
			emitted.emittedSource = emittedSource;
			emitted.selectedLod = payload.selectedLod;
			emitted.meshletOffset = payload.meshletOffset;
			emitted.meshletCount = payload.meshletCount;
			emitted.payloadByteOffset = payloadOffset;
			emitted.payloadByteSize = payloadBytes;
			emitted.reason = ToString(emittedSource);
			state.emittedPageSources.push_back(emitted);

			VirtualGeometryPageRequestRecord request;
			request.meshId = payload.meshId;
			request.objectId = emitted.objectId;
			request.pageId = payload.pageId;
			request.fallbackPageId = payload.fallbackPageId;
			request.desiredLodIndex = payload.selectedLod;
			request.priority = 10;
			request.screenImportance = 1.0f;
			request.payloadSource = payloadSource;
			request.expectedEmission = emittedSource;
			request.reason = "foundation request";
			state.pageRequests.push_back(request);

			snapshot.virtualGeometryViewSelections.push_back(std::move(state));
			SortSnapshotDeterministically(snapshot);
			return snapshot;
		}
	}

	void RegisterVirtualGeometryPageStreamingTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometryPageStreaming.Foundation.PagePayloadSourceRecordsSurviveVisibilitySnapshot", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			RenderFrameSnapshot snapshot = BuildPageStreamingSnapshot(VirtualGeometryPagePayloadSource::ResidentPagePayload,
				VirtualGeometryEmittedPageSource::HighDetailPage,
				4096u,
				2048u);

			const VirtualGeometryViewVisibilitySnapshot visibility = BuildVirtualGeometryViewVisibilitySnapshot(snapshot,
				snapshot.virtualGeometryViewSelections.front(),
				VirtualGeometryCounterSource::Prepared);
			const VirtualGeometryViewDiagnosticsSnapshot diagnostics = BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(visibility.residentPagePayloads.size() == 1u, "Resident page payload records should copy into the immutable visibility snapshot.");
			context.Expect(visibility.emittedPageSources.size() == 1u, "Emitted page source records should be view-scoped and snapshot-owned.");
			context.Expect(visibility.pageRequests.size() == 1u, "Page requests should remain visible to diagnostics and streaming tests.");
			context.Expect(visibility.counters.residentPagePayloads == 1u, "Prepared counters should report resident page payload count.");
			context.Expect(visibility.counters.emittedPageSources == 1u, "Prepared counters should report emitted page source count.");
			context.Expect(diagnostics.residentPagePayloadCount == 1u, "Diagnostics should expose resident page payload count.");
			context.Expect(diagnostics.emittedPageSourceCount == 1u, "Diagnostics should expose emitted page source count.");
			context.Expect(diagnostics.pageSources.highDetailPayloads == 1u, "High-detail page payloads should be labeled separately from fallback payloads.");
			context.Expect(diagnostics.pageSources.emittedHighDetailSources == 1u, "High-detail emitted sources should be counted for page-sourced rendering gates.");
			context.Expect(diagnostics.embeddedFallbackPayloadCount == 0u, "Normal high-detail page records must not look like conservative embedded fallback.");
		});

		registry.Add("VirtualGeometryPageStreaming.Foundation.EmbeddedFallbackIsLabeledAsConservative", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			RenderFrameSnapshot snapshot = BuildPageStreamingSnapshot(VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback,
				VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback,
				0u,
				512u);

			const VirtualGeometryViewVisibilitySnapshot visibility = BuildVirtualGeometryViewVisibilitySnapshot(snapshot,
				snapshot.virtualGeometryViewSelections.front(),
				VirtualGeometryCounterSource::Prepared);
			const VirtualGeometryViewDiagnosticsSnapshot diagnostics = BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.embeddedFallbackPayloadCount == 1u, "Conservative embedded fallback use must stay visible to gates.");
			context.Expect(diagnostics.pageSources.embeddedFallbackPayloads == 1u, "Embedded fallback payloads should have their own source counter.");
			context.Expect(diagnostics.pageSources.emittedEmbeddedFallbackSources == 1u, "Emitted embedded fallback sources should be counted independently.");
			context.Expect(!diagnostics.warnings.empty(), "Conservative embedded fallback should produce an actionable diagnostic warning.");
		});

		registry.Add("VirtualGeometryPageStreaming.Foundation.PayloadSourceChangesSnapshotHash", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			RenderFrameSnapshot highDetail = BuildPageStreamingSnapshot(VirtualGeometryPagePayloadSource::ResidentPagePayload,
				VirtualGeometryEmittedPageSource::HighDetailPage,
				1024u,
				1024u);
			RenderFrameSnapshot fallback = BuildPageStreamingSnapshot(VirtualGeometryPagePayloadSource::ResidentFallbackPayload,
				VirtualGeometryEmittedPageSource::FallbackPage,
				2048u,
				1024u);

			const std::uint64_t highDetailHash = BuildSnapshotDeterministicHash(highDetail);
			const std::uint64_t fallbackHash = BuildSnapshotDeterministicHash(fallback);

			context.Expect(highDetailHash != fallbackHash, "Snapshot hashes should include payload source and page-local byte identity.");
		});

		registry.Add("VirtualGeometryPageStreaming.US1.FallbackOnlyResidentPagesEmitFallbackSources", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			VirtualGeometryAssetRuntime asset = BuildFallbackOnlyResidentAsset();
			VirtualGeometrySelectedClusterRecord selected;
			selected.objectId = MakeTestUuid(22u);
			selected.meshId = asset.assetId;
			selected.clusterId = 0u;
			selected.lodIndex = 1u;
			selected.pageId = 0u;
			selected.fallbackClusterId = 0u;
			selected.usingFallback = true;

			const std::vector<VirtualGeometryClusterPacket> packets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, { selected }, MakeTestUuid(33u), RenderViewType::Game, 2u);

			context.ExpectEqual(static_cast<std::size_t>(1u), packets.size(), "Fallback-only resident assets should still produce one renderable cluster packet.");
			context.Expect(packets.front().payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload, "Pinned fallback pages should be labeled as resident fallback payloads.");
			context.Expect(packets.front().emittedSource == VirtualGeometryEmittedPageSource::FallbackPage, "Fallback-only rendering should emit fallback page geometry.");
			context.ExpectEqual(256ull, packets.front().pagePayloadByteOffset, "Fallback packets should preserve page-local payload offsets.");
			context.ExpectEqual(1024ull, packets.front().pagePayloadByteSize, "Fallback packets should preserve page-local payload byte size.");

			RenderFrameSnapshot snapshot;
			snapshot.frameId = 91u;
			VirtualGeometryViewSelectionState state;
			state.viewType = RenderViewType::Game;
			state.outputId = 2u;
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
			payload.pinnedFallback = true;
			payload.valid = true;
			state.residentPagePayloads.push_back(payload);
			VirtualGeometryEmittedPageSourceRecord emitted;
			emitted.viewType = state.viewType;
			emitted.viewId = state.outputId;
			emitted.frameId = snapshot.frameId;
			emitted.objectId = selected.objectId;
			emitted.meshId = asset.assetId;
			emitted.pageId = packets.front().pageId;
			emitted.fallbackPageId = packets.front().fallbackPageId;
			emitted.payloadSource = packets.front().payloadSource;
			emitted.emittedSource = packets.front().emittedSource;
			emitted.fallbackSubstitution = true;
			state.emittedPageSources.push_back(emitted);
			snapshot.virtualGeometryViewSelections.push_back(state);

			const VirtualGeometryViewVisibilitySnapshot visibility = BuildVirtualGeometryViewVisibilitySnapshot(snapshot,
				snapshot.virtualGeometryViewSelections.front(),
				VirtualGeometryCounterSource::Prepared);
			const VirtualGeometryViewDiagnosticsSnapshot diagnostics = BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(1u, diagnostics.pageSources.fallbackPayloads, "Diagnostics should count fallback payload records separately from high-detail pages.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedFallbackSources, "Diagnostics should report that the visible source was fallback page geometry.");
			context.ExpectEqual(0u, diagnostics.pageSources.highDetailPayloads, "Fallback-only visibility must not imply high-detail page residency.");
		});

		registry.Add("VirtualGeometryPageStreaming.US2.FailedHighDetailPageSubstitutesResidentFallback", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			VirtualGeometryAssetRuntime asset = BuildFailedHighDetailWithFallbackAsset();
			const TerraEngine::UUID meshId = asset.assetId;
			const TerraEngine::UUID objectId = MakeTestUuid(44u);
			VirtualGeometryQualityProfile quality = MakeDefaultVirtualGeometryQualityProfile();
			quality.lodErrorScale = 8.0f;

			const VirtualGeometryClusterSelectionResult selection = RenderSystem::SelectVirtualGeometryClusterCut(
				asset,
				meshId,
				objectId,
				quality,
				512.0f,
				glm::mat4(1.0f),
				glm::vec3(0.0f, 0.0f, 5.0f),
				glm::uvec2(1280u, 720u),
				true,
				true);

			context.ExpectEqual(static_cast<std::size_t>(1u), selection.selectedClusters.size(), "Failed high-detail selection should fall back to one resident coverage cluster.");
			context.Expect(selection.selectedClusters.front().usingFallback, "The selected cluster should explicitly report fallback substitution.");
			context.ExpectEqual(0u, selection.selectedClusters.front().pageId, "Fallback substitution should select the resident fallback page.");
			context.ExpectEqual(static_cast<std::size_t>(1u), selection.pageNeeds.size(), "The missing/failed high-detail page should still create one streaming need.");
			context.ExpectEqual(1u, selection.pageNeeds.front().pageId, "The page need should identify the failed high-detail page.");
			context.ExpectEqual(0u, selection.pageNeeds.front().fallbackPageId, "The page need should preserve the resident fallback page id.");
			context.Expect(selection.pageNeeds.front().desiredPayloadSource == VirtualGeometryPagePayloadSource::Missing, "Failed detail should be reported as unavailable desired page payload.");
			context.Expect(selection.pageNeeds.front().fallbackPayloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload, "Fallback substitution should name the resident fallback payload source.");

			const std::vector<VirtualGeometryClusterPacket> packets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, selection.selectedClusters, MakeTestUuid(45u), RenderViewType::Game, 3u);
			context.ExpectEqual(static_cast<std::size_t>(1u), packets.size(), "Fallback-substituted selection should remain renderable.");
			context.Expect(packets.front().payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload, "The emitted packet should come from resident fallback coverage.");
			context.Expect(packets.front().emittedSource == VirtualGeometryEmittedPageSource::FallbackPage, "Failed high-detail pages should emit fallback page geometry, not holes.");
		});

		registry.Add("VirtualGeometryPageStreaming.US2.MissingPendingFailedAndEvictedPagesKeepFallbackCoverage", [](TestContext& context) {
			using namespace TerraEngine::Renderer;

			const std::array<GeometryPageState, 5u> unavailableStates = {
				GeometryPageState::NotResident,
				GeometryPageState::PendingIO,
				GeometryPageState::PendingUpload,
				GeometryPageState::Failed,
				GeometryPageState::Evicted
			};

			for (GeometryPageState state : unavailableStates)
			{
				VirtualGeometryAssetRuntime asset = BuildFailedHighDetailWithFallbackAsset();
				asset.pages[1].state = state;
				asset.pages[0].payloadBufferIndex = 70u;
				asset.pages[0].payloadDescriptorIndex = 1u;
				const VirtualGeometryClusterSelectionResult selection = RenderSystem::SelectVirtualGeometryClusterCut(
					asset,
					asset.assetId,
					MakeTestUuid(80u + static_cast<std::uint32_t>(state)),
					MakeDefaultVirtualGeometryQualityProfile(),
					512.0f,
					glm::mat4(1.0f),
					glm::vec3(0.0f, 0.0f, 5.0f),
					glm::uvec2(1280u, 720u),
					true,
					true);

				context.ExpectEqual(static_cast<std::size_t>(1u), selection.selectedClusters.size(), "Unavailable high-detail pages should pick one fallback coverage cluster.");
				context.Expect(selection.selectedClusters.front().usingFallback, "Unavailable high-detail pages should report fallback substitution.");
				context.ExpectEqual(0u, selection.selectedClusters.front().pageId, "Fallback coverage should use the resident fallback page.");
				context.ExpectEqual(static_cast<std::size_t>(1u), selection.pageNeeds.size(), "Unavailable high-detail pages should still queue one detail page need.");

				const std::vector<VirtualGeometryClusterPacket> packets =
					RenderSystem::BuildVirtualGeometryClusterPackets(asset, selection.selectedClusters, MakeTestUuid(81u), RenderViewType::Game, 4u);
				context.ExpectEqual(static_cast<std::size_t>(1u), packets.size(), "Fallback-selected clusters should remain renderable.");
				if (!packets.empty())
				{
					context.Expect(packets.front().payloadSource == VirtualGeometryPagePayloadSource::ResidentFallbackPayload, "Fallback-selected packets should use resident fallback payloads.");
					context.ExpectEqual(70u, packets.front().payloadBufferIndex, "Fallback packets should preserve the physical fallback payload buffer binding.");
					context.ExpectEqual(1u, packets.front().payloadDescriptorIndex, "Fallback packets should preserve the physical fallback payload descriptor index.");
				}
			}
		});

		registry.Add("VirtualGeometryPageStreaming.US3.EvictedHighDetailPageRequestsAgainWithFallbackVisible", [](TestContext& context) {
			const UUID meshId = MakeTestUuid(60u);
			const UUID objectId = MakeTestUuid(61u);

			Resource::VirtualGeometryStreamingPage fallbackPage;
			fallbackPage.pageId = 0u;
			fallbackPage.firstLodIndex = 0u;
			fallbackPage.lodCount = 1u;
			fallbackPage.fallbackPageId = 0u;
			fallbackPage.byteSize = 1024u;
			fallbackPage.resident = true;
			fallbackPage.pinnedFallback = true;
			fallbackPage.payloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;

			Resource::VirtualGeometryStreamingPage evictedHighDetail;
			evictedHighDetail.pageId = 6u;
			evictedHighDetail.firstLodIndex = 2u;
			evictedHighDetail.lodCount = 1u;
			evictedHighDetail.fallbackPageId = 0u;
			evictedHighDetail.byteSize = 8192u;
			evictedHighDetail.resident = false;
			evictedHighDetail.failed = false;
			evictedHighDetail.payloadSource = VirtualGeometryPagePayloadSource::Missing;

			Resource::VirtualGeometryPageNeedInput input;
			input.assetId = meshId;
			input.objectId = objectId;
			input.viewId = "Game:1";
			input.desiredLodIndex = 2u;
			input.desiredClusterId = 17u;
			input.screenImportance = 0.75f;
			input.predictedNeed = 0.75f;
			input.fallbackRequired = true;
			input.desiredPayloadSource = VirtualGeometryPagePayloadSource::Missing;
			input.fallbackPayloadSource = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			input.pages = { fallbackPage, evictedHighDetail };

			const Resource::VirtualGeometryPageRequestPlan plan =
				Resource::BuildVirtualGeometryPageRequestPlan({ input }, Resource::MakeDefaultStreamingPolicy());

			context.ExpectEqual(static_cast<std::size_t>(1u), plan.packets.size(), "Evicted high-detail pages should be requested again.");
			context.ExpectEqual(6u, plan.packets.front().pageId, "The desired high-detail page should be the queued IO request.");
			context.Expect(!plan.packets.front().fallbackRequest, "Resident fallback coverage should stay visible without queuing duplicate fallback IO.");
			context.Expect(plan.failures.empty(), "Evicted-but-retryable pages should not be treated as permanent request failures.");
		});
	}
}
