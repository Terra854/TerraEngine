#include "TestHarness.h"
#include "VulkanSourceTestUtils.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"
#include "Renderer/RHI/Vulkan/VulkanHiZ.h"
#include "Renderer/RenderSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <glm/glm.hpp>
#include <sstream>
#include <string>
#include <utility>

namespace TerraEngine::Tests
{
	namespace
	{
		constexpr std::uint64_t kStableDepthSceneSignature = 0x5eed1234ull;

		VulkanHiZHistoryMetadata MakeValidHistory(std::uint32_t viewId, std::uint32_t width, std::uint32_t height, const glm::mat4& viewProjection)
		{
			VulkanHiZHistoryMetadata history{};
			history.viewId = viewId;
			history.viewClass = VulkanHiZViewClass::Game;
			history.targetId = 41u;
			history.width = width;
			history.height = height;
			history.projectionSignature = BuildVulkanHiZProjectionSignature(viewProjection);
			history.depthSceneSignature = kStableDepthSceneSignature;
			history.frameId = 11u;
			history.valid = true;
			history.invalidReason = VulkanHiZInvalidReason::None;
			return history;
		}

		VulkanHiZValidationInput MakeValidationInput(std::uint32_t viewId, std::uint32_t width, std::uint32_t height, const glm::mat4& viewProjection)
		{
			VulkanHiZValidationInput input{};
			input.viewId = viewId;
			input.viewClass = VulkanHiZViewClass::Game;
			input.targetId = 41u;
			input.width = width;
			input.height = height;
			input.projectionSignature = BuildVulkanHiZProjectionSignature(viewProjection);
			input.depthSceneSignature = kStableDepthSceneSignature;
			input.pyramidReady = true;
			input.qualityAllowsOcclusion = true;
			input.viewSupported = true;
			input.selfOcclusionGuardPassed = true;
			input.nearPlaneGuardPassed = true;
			input.thinGeometryGuardPassed = true;
			return input;
		}

		std::string ReadSourceFile(const std::filesystem::path& relativePath)
		{
			// The Hi-Z page-transition tests inspect shader/backend source
			// contracts so they can run on CI machines without a Vulkan device.
			const std::filesystem::path sourcePath =
				std::filesystem::path(TERRAENGINE_SOURCE_DIR) / relativePath;
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		bool ContainsText(const std::string& text, const std::string& token)
		{
			return text.find(token) != std::string::npos;
		}
	}

	void RegisterVirtualGeometryHiZTests(TestRegistry& registry)
	{
		registry.Add("VirtualGeometry.HiZ.GpuPacketsAndGeneratedSubmissionsPreserveBaselineMatch", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 6u;
			visibility.frameId = 7105u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.pyramidSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.renderSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.cameraSignature = 0x70050011u;
			visibility.hiZValidationState.depthSceneSignature = 0x70050022u;
			visibility.hiZValidationState.warmupFrames = 4u;
			visibility.hiZValidationState.failOpen = false;

			GpuVirtualGeometryPacketRecord packet{};
			packet.packetId = 0u;
			packet.viewId = visibility.outputId;
			packet.viewType = visibility.viewType;
			packet.frameId = visibility.frameId;
			packet.objectId = TerraEngine::GenerateUUID();
			packet.meshId = TerraEngine::GenerateUUID();
			packet.materialId = TerraEngine::GenerateUUID();
			packet.clusterId = 12u;
			packet.meshletOffset = 48u;
			packet.meshletCount = 24u;
			visibility.gpuPacketRecords.push_back(packet);

			GpuVirtualGeometryPacketFrameSet frameSet{};
			frameSet.viewId = visibility.outputId;
			frameSet.viewType = visibility.viewType;
			frameSet.frameId = visibility.frameId;
			frameSet.targetSize = visibility.hiZValidationState.renderSize;
			frameSet.packetCount = 1u;
			frameSet.batchCount = 1u;
			frameSet.generatedSubmissionCount = 1u;
			frameSet.validity = VirtualGeometryGpuFrameSetValidity::Valid;
			visibility.gpuPacketFrameSets.push_back(frameSet);

			GeneratedVirtualGeometrySubmissionRecord submission{};
			submission.recordId = 0u;
			submission.viewId = visibility.outputId;
			submission.viewType = visibility.viewType;
			submission.frameId = visibility.frameId;
			submission.packetCount = 1u;
			submission.indirectTaskCount = 1u;
			submission.materialId = packet.materialId;
			submission.meshId = packet.meshId;
			submission.source = VirtualGeometryCounterSource::GPUReadback;
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::Valid;
			visibility.generatedSubmissions.push_back(submission);

			HiZRegressionSnapshot baseline{};
			baseline.viewId = visibility.outputId;
			baseline.viewType = visibility.viewType;
			baseline.frameIndex = visibility.frameId;
			baseline.state = VirtualGeometryHiZState::Active;
			baseline.testedMeshlets = packet.meshletCount;
			baseline.occludedMeshlets = 8u;
			baseline.visibleMeshlets = 16u;
			baseline.recoveryFrames = visibility.hiZValidationState.warmupFrames;
			baseline.baselineMatched = true;
			visibility.hiZRegressionSnapshots.push_back(baseline);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.hiZBaselineMatched, "GPU packet and generated-submission diagnostics should preserve the accepted Hi-Z baseline.");
			context.Expect(diagnostics.hiZActive, "The GPU packet path should not turn an active current pyramid into a fail-open state.");
			context.ExpectEqual(1u, diagnostics.gpuPacketRecordCount, "The diagnostics snapshot should still expose the GPU packet table while checking Hi-Z.");
			context.ExpectEqual(1u, diagnostics.validGeneratedSubmissionCount, "Generated submissions should remain valid while Hi-Z baseline records are matched.");
			context.ExpectEqual(0u, diagnostics.hiZFalseOcclusionRiskWarnings, "Matched GPU packet Hi-Z baselines should not publish false-occlusion risk warnings.");
		});

		registry.Add("VirtualGeometry.HiZ.PageSourceTransitionsStayDiagnosticOnly", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 9u;
			visibility.frameId = 7106u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.pyramidSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.renderSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.cameraSignature = 0x71060010u;
			visibility.hiZValidationState.depthSceneSignature = 0x71060020u;
			visibility.hiZValidationState.warmupFrames = 5u;
			visibility.hiZValidationState.failOpen = false;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			VirtualGeometryPagePayloadRecord highDetailPayload{};
			highDetailPayload.meshId = meshId;
			highDetailPayload.pageId = 5u;
			highDetailPayload.fallbackPageId = 1u;
			highDetailPayload.source = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			highDetailPayload.payloadByteOffset = 4096u;
			highDetailPayload.payloadByteSize = 2048u;
			highDetailPayload.resident = true;
			highDetailPayload.valid = true;
			visibility.residentPagePayloads.push_back(highDetailPayload);

			VirtualGeometryPagePayloadRecord fallbackPayload = highDetailPayload;
			fallbackPayload.pageId = 1u;
			fallbackPayload.fallbackPageId = 1u;
			fallbackPayload.source = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			fallbackPayload.payloadByteOffset = 0u;
			fallbackPayload.payloadByteSize = 1024u;
			fallbackPayload.pinnedFallback = true;
			visibility.residentPagePayloads.push_back(fallbackPayload);

			VirtualGeometryEmittedPageSourceRecord highDetailEmission{};
			highDetailEmission.viewId = visibility.outputId;
			highDetailEmission.viewType = visibility.viewType;
			highDetailEmission.frameId = visibility.frameId;
			highDetailEmission.meshId = meshId;
			highDetailEmission.materialId = materialId;
			highDetailEmission.pageId = highDetailPayload.pageId;
			highDetailEmission.fallbackPageId = highDetailPayload.fallbackPageId;
			highDetailEmission.payloadSource = highDetailPayload.source;
			highDetailEmission.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			highDetailEmission.payloadByteOffset = highDetailPayload.payloadByteOffset;
			highDetailEmission.payloadByteSize = highDetailPayload.payloadByteSize;
			visibility.emittedPageSources.push_back(highDetailEmission);

			VirtualGeometryEmittedPageSourceRecord fallbackEmission = highDetailEmission;
			fallbackEmission.pageId = fallbackPayload.pageId;
			fallbackEmission.fallbackPageId = fallbackPayload.fallbackPageId;
			fallbackEmission.payloadSource = fallbackPayload.source;
			fallbackEmission.emittedSource = VirtualGeometryEmittedPageSource::FallbackPage;
			fallbackEmission.fallbackSubstitution = true;
			fallbackEmission.payloadByteOffset = fallbackPayload.payloadByteOffset;
			fallbackEmission.payloadByteSize = fallbackPayload.payloadByteSize;
			fallbackEmission.reason = "resident fallback substituted while detail page streams";
			visibility.emittedPageSources.push_back(fallbackEmission);

			HiZRegressionSnapshot baseline{};
			baseline.viewId = visibility.outputId;
			baseline.viewType = visibility.viewType;
			baseline.frameIndex = visibility.frameId;
			baseline.state = VirtualGeometryHiZState::Active;
			baseline.testedMeshlets = 48u;
			baseline.visibleMeshlets = 48u;
			baseline.recoveryFrames = visibility.hiZValidationState.warmupFrames;
			baseline.baselineMatched = true;
			visibility.hiZRegressionSnapshots.push_back(baseline);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.hiZActive, "Resident/fallback page-source transitions should not disable an active current Hi-Z pyramid.");
			context.Expect(diagnostics.hiZBaselineMatched, "Page-source transitions should preserve the accepted Hi-Z baseline.");
			context.ExpectEqual(0u, diagnostics.hiZFalseOcclusionRiskWarnings, "Matched page-source transitions should not raise false-occlusion risk warnings.");
			context.ExpectEqual(1u, diagnostics.pageSources.highDetailPayloads, "The high-detail resident payload should remain visible to diagnostics.");
			context.ExpectEqual(1u, diagnostics.pageSources.fallbackPayloads, "The fallback resident payload should remain visible to diagnostics.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedHighDetailSources, "High-detail emission should be counted independently of fallback emission.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedFallbackSources, "Fallback emission should be counted independently of high-detail emission.");
		});

		registry.Add("VirtualGeometry.HiZ.PagePayloadSourcesDoNotChangeAcceptedBaseline", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 12u;
			visibility.frameId = 7303u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.pyramidSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.renderSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.cameraSignature = 0x73030010u;
			visibility.hiZValidationState.depthSceneSignature = 0x73030020u;
			visibility.hiZValidationState.warmupFrames = 6u;
			visibility.hiZValidationState.failOpen = false;

			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			VirtualGeometryPagePayloadRecord highDetail{};
			highDetail.meshId = meshId;
			highDetail.pageId = 8u;
			highDetail.fallbackPageId = 1u;
			highDetail.source = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			highDetail.payloadByteOffset = 4096u;
			highDetail.payloadByteSize = 3072u;
			highDetail.resident = true;
			highDetail.valid = true;
			visibility.residentPagePayloads.push_back(highDetail);

			VirtualGeometryPagePayloadRecord residentFallback = highDetail;
			residentFallback.pageId = 1u;
			residentFallback.fallbackPageId = 1u;
			residentFallback.source = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			residentFallback.payloadByteOffset = 0u;
			residentFallback.payloadByteSize = 1024u;
			residentFallback.pinnedFallback = true;
			visibility.residentPagePayloads.push_back(residentFallback);

			VirtualGeometryPagePayloadRecord embeddedFallback = highDetail;
			embeddedFallback.pageId = 0u;
			embeddedFallback.fallbackPageId = 0u;
			embeddedFallback.source = VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			embeddedFallback.payloadByteOffset = 0u;
			embeddedFallback.payloadByteSize = 512u;
			embeddedFallback.pinnedFallback = true;
			visibility.residentPagePayloads.push_back(embeddedFallback);

			auto makeEmission = [&](const VirtualGeometryPagePayloadRecord& payload, VirtualGeometryEmittedPageSource source, std::string reason) {
				VirtualGeometryEmittedPageSourceRecord emission{};
				emission.viewId = visibility.outputId;
				emission.viewType = visibility.viewType;
				emission.frameId = visibility.frameId;
				emission.meshId = meshId;
				emission.materialId = materialId;
				emission.pageId = payload.pageId;
				emission.fallbackPageId = payload.fallbackPageId;
				emission.payloadSource = payload.source;
				emission.emittedSource = source;
				emission.payloadByteOffset = payload.payloadByteOffset;
				emission.payloadByteSize = payload.payloadByteSize;
				emission.fallbackSubstitution = source != VirtualGeometryEmittedPageSource::HighDetailPage;
				emission.conservativeEmbeddedFallback = source == VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
				emission.reason = std::move(reason);
				return emission;
			};
			visibility.emittedPageSources.push_back(makeEmission(highDetail, VirtualGeometryEmittedPageSource::HighDetailPage, {}));
			visibility.emittedPageSources.push_back(makeEmission(residentFallback, VirtualGeometryEmittedPageSource::FallbackPage, "resident fallback covered a missing detail page"));
			visibility.emittedPageSources.push_back(makeEmission(embeddedFallback, VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback, "owning-mesh fallback remained conservative"));

			HiZRegressionSnapshot baseline{};
			baseline.viewId = visibility.outputId;
			baseline.viewType = visibility.viewType;
			baseline.frameIndex = visibility.frameId;
			baseline.state = VirtualGeometryHiZState::Active;
			baseline.testedMeshlets = 72u;
			baseline.occludedMeshlets = 12u;
			baseline.visibleMeshlets = 60u;
			baseline.recoveryFrames = visibility.hiZValidationState.warmupFrames;
			baseline.baselineMatched = true;
			visibility.hiZRegressionSnapshots.push_back(baseline);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.hiZActive, "Resident, fallback, and owning-mesh fallback payload sources should keep the accepted Hi-Z state active.");
			context.Expect(diagnostics.hiZBaselineMatched, "Payload-source changes should not become a hidden Hi-Z baseline mismatch.");
			context.ExpectEqual(0u, diagnostics.hiZFalseOcclusionRiskWarnings, "Matched payload-source transitions should not create false-occlusion risks.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedHighDetailSources, "High-detail page emission should stay visible beside fallback coverage.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedFallbackSources, "Resident fallback page emission should stay visible beside high-detail coverage.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedEmbeddedFallbackSources, "Owning-mesh conservative fallback emission should be explicit instead of mutating Hi-Z policy.");
		});

		registry.Add("VirtualGeometry.HiZ.PageIdentityIsHashedButTaskPathHasNoQualityTier", [](TestContext& context) {
			const std::string vulkanSource = VulkanSourceContracts::ReadContextSources();
			const std::string taskShader = ReadSourceFile("Engine/Shaders/Default/Default.task.hlsl");

			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryPageId"), "Vulkan Hi-Z depth signatures should include the resident page id.");
			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryFallbackPageId"), "Vulkan Hi-Z depth signatures should include fallback page identity.");
			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryPayloadSource"), "Vulkan Hi-Z depth signatures should include payload source identity.");
			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryEmittedSource"), "Vulkan Hi-Z depth signatures should include emitted source identity.");
			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryPagePayloadByteOffset"), "Vulkan Hi-Z depth signatures should include page-local payload byte offsets.");
			context.Expect(ContainsText(vulkanSource, "command.virtualGeometryPagePayloadByteSize"), "Vulkan Hi-Z depth signatures should include page-local payload byte sizes.");
			context.Expect(ContainsText(taskShader, "pagePayloadSeed"), "Task shader packet metadata should still read page payload source identity.");
			context.Expect(ContainsText(taskShader, "does not alter the Hi-Z projection"), "Task shader comments should state that page payload identity does not change Hi-Z projection.");
			context.Expect(!ContainsText(taskShader, "HiZQuality"), "Task shader should still avoid selectable Hi-Z quality tiers.");
		});

		registry.Add("VirtualGeometry.HiZ.PagePayloadSizeDoesNotBroadenClusterPacketBounds", [](TestContext& context) {
			VirtualGeometryAssetRuntime asset{};
			asset.assetId = TerraEngine::GenerateUUID();
			asset.cullableGroups.resize(4u);
			for (std::uint32_t groupIndex = 0u; groupIndex < 4u; ++groupIndex)
			{
				asset.cullableGroups[groupIndex].groupIndex = groupIndex;
				asset.cullableGroups[groupIndex].lodIndex = 0u;
				asset.cullableGroups[groupIndex].meshletIndex = 20u + groupIndex;
				asset.cullableGroups[groupIndex].pageId = 3u;
			}

			GeometryPageRuntime page{};
			page.pageId = 3u;
			page.firstLodIndex = 0u;
			page.lodCount = 1u;
			page.groupOffset = 0u;
			page.groupCount = 4u;
			page.state = GeometryPageState::Resident;
			page.byteSize = 2048u;
			page.payloadByteOffset = 512u;
			page.payloadByteSize = 2048u;
			page.fallbackPageId = 0u;
			asset.pages.push_back(page);

			VirtualGeometryClusterRuntime cluster{};
			cluster.clusterId = 11u;
			cluster.lodIndex = 0u;
			cluster.firstGroupIndex = 0u;
			cluster.groupCount = 4u;
			cluster.pageId = page.pageId;
			cluster.boundsCenter = glm::vec3(2.0f, -1.0f, 5.0f);
			cluster.boundsRadius = 0.75f;
			asset.clusters.push_back(cluster);

			TerraEngine::Renderer::VirtualGeometrySelectedClusterRecord selected{};
			selected.meshId = TerraEngine::GenerateUUID();
			selected.objectId = TerraEngine::GenerateUUID();
			selected.clusterId = cluster.clusterId;
			selected.lodIndex = cluster.lodIndex;
			selected.pageId = page.pageId;

			const TerraEngine::UUID materialId = TerraEngine::GenerateUUID();
			const std::vector<VirtualGeometryClusterPacket> firstPackets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, { selected }, materialId, RenderViewType::Game, 2u);
			asset.pages.front().payloadByteSize = 8192u;
			asset.pages.front().byteSize = 8192u;
			const std::vector<VirtualGeometryClusterPacket> republishedPackets =
				RenderSystem::BuildVirtualGeometryClusterPackets(asset, { selected }, materialId, RenderViewType::Game, 2u);

			context.ExpectEqual(static_cast<std::size_t>(1u), firstPackets.size(), "The test cluster should build one Hi-Z-compatible packet.");
			context.ExpectEqual(static_cast<std::size_t>(1u), republishedPackets.size(), "Republishing the page payload should still build one packet.");
			context.ExpectEqual(2048ull, firstPackets.front().pagePayloadByteSize, "The first packet should expose the first resident page byte size.");
			context.ExpectEqual(8192ull, republishedPackets.front().pagePayloadByteSize, "The republished packet should expose the changed resident page byte size.");
			context.Expect(firstPackets.front().boundsMin == republishedPackets.front().boundsMin, "Page payload byte-size changes should not broaden packet minimum bounds.");
			context.Expect(firstPackets.front().boundsMax == republishedPackets.front().boundsMax, "Page payload byte-size changes should not broaden packet maximum bounds.");
			context.Expect(firstPackets.front().boundsMin == cluster.boundsCenter - glm::vec3(cluster.boundsRadius), "Packet minimum bounds should come from the selected cluster.");
			context.Expect(firstPackets.front().boundsMax == cluster.boundsCenter + glm::vec3(cluster.boundsRadius), "Packet maximum bounds should come from the selected cluster.");
		});

		registry.Add("VirtualGeometry.HiZ.BatchedPathPublishesBaselineMatch", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;
			visibility.frameId = 7001u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::Prepared;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.pyramidSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.renderSize = glm::uvec2(1920u, 1080u);
			visibility.hiZValidationState.warmupFrames = 3u;
			visibility.hiZValidationState.failOpen = false;

			VirtualGeometryClusterPacket packet{};
			packet.packetId = 0u;
			packet.viewId = visibility.outputId;
			packet.viewType = visibility.viewType;
			packet.meshletCount = 32u;
			visibility.clusterPackets.push_back(packet);

			ClusterWorkBatch batch{};
			batch.batchId = 0u;
			batch.viewId = visibility.outputId;
			batch.viewType = visibility.viewType;
			batch.packetCount = 1u;
			batch.selectedMeshlets = 32u;
			batch.submittedMeshlets = 32u;
			batch.batchReason = "batched Hi-Z baseline fixture";
			visibility.clusterBatches.push_back(batch);

			HiZRegressionSnapshot baseline{};
			baseline.viewId = visibility.outputId;
			baseline.viewType = visibility.viewType;
			baseline.frameIndex = visibility.frameId;
			baseline.state = VirtualGeometryHiZState::Active;
			baseline.testedMeshlets = 32u;
			baseline.visibleMeshlets = 32u;
			baseline.recoveryFrames = 3u;
			baseline.baselineMatched = true;
			visibility.hiZRegressionSnapshots.push_back(baseline);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.Expect(diagnostics.hiZBaselineMatched, "Batched virtual geometry should publish the accepted Hi-Z baseline as matched.");
			context.ExpectEqual(0u, diagnostics.hiZFalseOcclusionRiskWarnings, "Matched Hi-Z baselines should not raise false-occlusion risks.");
			context.ExpectEqual(1u, diagnostics.submittedBatchCount, "The Hi-Z baseline fixture should still carry batched packet diagnostics.");
			context.Expect(diagnostics.hiZActive, "Active validation state should keep Hi-Z active for the batched diagnostics path.");
		});

		registry.Add("VirtualGeometry.HiZ.BaselineMismatchFailsPerformanceGate", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;
			visibility.frameId = 7107u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.failOpen = false;

			HiZRegressionSnapshot baseline{};
			baseline.viewId = visibility.outputId;
			baseline.viewType = visibility.viewType;
			baseline.frameIndex = visibility.frameId;
			baseline.state = VirtualGeometryHiZState::Active;
			baseline.testedMeshlets = 64u;
			baseline.visibleMeshlets = 64u;
			baseline.baselineMatched = false;
			baseline.reason = "accepted baseline changed";
			visibility.hiZRegressionSnapshots.push_back(baseline);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);
			const auto gate = std::find_if(
				diagnostics.virtualGeometryPerformanceGates.begin(),
				diagnostics.virtualGeometryPerformanceGates.end(),
				[](const PerformanceGateResult& candidate) {
					return candidate.gateId == "virtual-geometry-hiz-baseline-match";
				});

			context.Expect(!diagnostics.hiZBaselineMatched, "A mismatched Hi-Z baseline should remain visible in diagnostics.");
			context.ExpectEqual(1u, diagnostics.hiZFalseOcclusionRiskWarnings, "A mismatched Hi-Z baseline should increment the false-occlusion risk counter.");
			context.Expect(gate != diagnostics.virtualGeometryPerformanceGates.end(), "Hi-Z baseline gate should be generated for visibility diagnostics.");
			context.Expect(gate->status == VirtualGeometryPerformanceGateStatus::Fail, "Hi-Z baseline gate should fail when accepted behavior changes.");
			context.ExpectEqual(2u, gate->viewId, "Hi-Z baseline gate should carry the producing Game View id.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.KeepsOcclusionActiveAcrossCameraAndDepthSceneMotion", [](TestContext& context) {
			glm::mat4 viewProjection(1.0f);
			VulkanHiZValidationInput input = MakeValidationInput(1u, 1920u, 1080u, viewProjection);

			const VulkanHiZValidationResult firstFrame = ValidateVulkanHiZHistory({}, input);
			context.Expect(firstFrame.occlusionAllowed, "A cleared Hi-Z pyramid should be usable immediately; first-frame metadata is diagnostic-only.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(firstFrame.invalidReason), "First-frame metadata should not disable always-on Hi-Z.");

			const VulkanHiZHistoryMetadata history = MakeValidHistory(1u, 1920u, 1080u, viewProjection);
			VulkanHiZValidationInput missingPyramidInput = input;
			missingPyramidInput.pyramidReady = false;
			const VulkanHiZValidationResult missingPyramid = ValidateVulkanHiZHistory(history, missingPyramidInput);
			context.Expect(!missingPyramid.occlusionAllowed, "Missing pyramid resources are the only validation path that should disable Hi-Z.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::MissingPyramid), static_cast<std::uint32_t>(missingPyramid.invalidReason), "Missing Hi-Z pyramid resources should have a distinct fail-open reason.");

			VulkanHiZValidationInput resizeInput = input;
			resizeInput.width = 1280u;
			const VulkanHiZValidationResult resize = ValidateVulkanHiZHistory(history, resizeInput);
			context.Expect(resize.occlusionAllowed, "Resize metadata should not disable Hi-Z once a same-frame pyramid resource is ready.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(resize.invalidReason), "Resize metadata is diagnostic-only for always-on Hi-Z.");

			VulkanHiZValidationInput targetInput = input;
			targetInput.targetId = 99u;
			const VulkanHiZValidationResult targetMismatch = ValidateVulkanHiZHistory(history, targetInput);
			context.Expect(targetMismatch.occlusionAllowed, "Editor target switches should keep Hi-Z active because the shared warm pyramid is diagnostic-gated, not ownership-gated.");
			context.Expect(!targetMismatch.sameTargetHistory, "Validation should still expose that the previous pyramid came from a different render target.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(targetMismatch.invalidReason), "Target mismatch is diagnostic-only for the always-active editor Hi-Z path.");

			glm::mat4 changedProjection = viewProjection;
			changedProjection[3][2] = 0.25f;
			VulkanHiZValidationInput projectionInput = MakeValidationInput(1u, 1920u, 1080u, changedProjection);
			const VulkanHiZValidationResult projectionChanged = ValidateVulkanHiZHistory(history, projectionInput);
			context.Expect(projectionChanged.occlusionAllowed, "Camera/view-projection motion should keep Hi-Z active so VG culling remains continuous while navigating.");
			context.Expect(!projectionChanged.projectionMatches, "Validation should still expose that the previous pyramid was generated from a different projection.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(projectionChanged.invalidReason), "Projection mismatch is diagnostic-only and should not become the active fail-open reason.");

			VulkanHiZValidationInput cameraCutInput = input;
			cameraCutInput.cameraCut = true;
			const VulkanHiZValidationResult cameraCut = ValidateVulkanHiZHistory(history, cameraCutInput);
			context.Expect(cameraCut.occlusionAllowed, "Camera-cut metadata should not disable Hi-Z while the editor is continuously updating the view.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(cameraCut.invalidReason), "Camera-cut metadata is diagnostic-only for the always-active Hi-Z path.");

			VulkanHiZValidationInput movedOccluderInput = input;
			movedOccluderInput.depthSceneSignature = kStableDepthSceneSignature + 1u;
			const VulkanHiZValidationResult movedOccluder = ValidateVulkanHiZHistory(history, movedOccluderInput);
			context.Expect(movedOccluder.occlusionAllowed, "Moving occluders should keep Hi-Z active so wall/blocker tests recover immediately after motion.");
			context.Expect(!movedOccluder.depthSceneMatches, "Validation should expose that the depth-producing draw scene changed.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(movedOccluder.invalidReason), "Depth-scene changes are diagnostic-only for the always-active Hi-Z path.");

			VulkanHiZValidationInput viewInput = input;
			viewInput.viewId = 2u;
			const VulkanHiZValidationResult viewMismatch = ValidateVulkanHiZHistory(history, viewInput);
			context.Expect(viewMismatch.occlusionAllowed, "Another editor camera view should not permanently disable the shared warm Hi-Z pyramid.");
			context.Expect(!viewMismatch.sameViewHistory, "Validation should still expose that the previous pyramid was produced by another view.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(viewMismatch.invalidReason), "View mismatch is diagnostic-only for the always-active editor Hi-Z path.");

			const VulkanHiZValidationResult valid = ValidateVulkanHiZHistory(history, input);
			context.Expect(valid.historyValid, "Ready Hi-Z resources should validate regardless of metadata history.");
			context.Expect(valid.occlusionAllowed, "Ready Hi-Z resources should allow occlusion while quality and guard metadata remain diagnostic-only.");
			context.Expect(valid.depthSceneMatches, "Matching draw-scene metadata should keep Hi-Z eligible for occlusion.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.IgnoresQualityAndGuardDisableSignals", [](TestContext& context) {
			const glm::mat4 viewProjection(1.0f);
			const VulkanHiZHistoryMetadata history = MakeValidHistory(3u, 1024u, 768u, viewProjection);
			VulkanHiZValidationInput input = MakeValidationInput(3u, 1024u, 768u, viewProjection);
			input.qualityAllowsOcclusion = false;
			input.selfOcclusionGuardPassed = false;
			input.nearPlaneGuardPassed = false;
			input.thinGeometryGuardPassed = false;
			input.viewSupported = false;

			const VulkanHiZValidationResult result = ValidateVulkanHiZHistory(history, input);

			context.Expect(result.historyValid, "Ready Hi-Z resources should stay valid when legacy disable metadata is present.");
			context.Expect(result.occlusionAllowed, "Quality, support, and guard metadata must not disable always-on Hi-Z.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(result.invalidReason), "Legacy disable metadata should not become active invalid reasons.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.KeepsThinAndNearPlaneSignalsDiagnosticOnly", [](TestContext& context) {
			const glm::mat4 viewProjection(1.0f);
			const VulkanHiZHistoryMetadata history = MakeValidHistory(5u, 1440u, 900u, viewProjection);

			VulkanHiZValidationInput thinGeometryInput = MakeValidationInput(5u, 1440u, 900u, viewProjection);
			thinGeometryInput.thinGeometryGuardPassed = false;
			const VulkanHiZValidationResult thinGeometry = ValidateVulkanHiZHistory(history, thinGeometryInput);

			VulkanHiZValidationInput nearPlaneInput = MakeValidationInput(5u, 1440u, 900u, viewProjection);
			nearPlaneInput.nearPlaneGuardPassed = false;
			const VulkanHiZValidationResult nearPlane = ValidateVulkanHiZHistory(history, nearPlaneInput);

			context.Expect(thinGeometry.historyValid, "Thin-geometry metadata should not disable a ready Hi-Z pyramid.");
			context.Expect(thinGeometry.occlusionAllowed, "Thin-geometry metadata must be diagnostic-only.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(thinGeometry.invalidReason), "Thin-geometry metadata should not become an active invalid reason.");
			context.Expect(nearPlane.historyValid, "Near-plane metadata should not disable a ready Hi-Z pyramid.");
			context.Expect(nearPlane.occlusionAllowed, "Near-plane metadata must be diagnostic-only.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(nearPlane.invalidReason), "Near-plane metadata should not become an active invalid reason.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.CameraAndEditorViewMotionStayActive", [](TestContext& context) {
			glm::mat4 previousViewProjection(1.0f);
			const VulkanHiZHistoryMetadata history = MakeValidHistory(9u, 1600u, 900u, previousViewProjection);

			glm::mat4 cameraCutProjection(1.0f);
			cameraCutProjection[3][0] = 4.0f;
			cameraCutProjection[3][2] = -2.0f;
			const VulkanHiZValidationInput cameraCutInput = MakeValidationInput(9u, 1600u, 900u, cameraCutProjection);
			const VulkanHiZValidationResult cameraCut = ValidateVulkanHiZHistory(history, cameraCutInput);

			VulkanHiZValidationInput sceneViewInput = MakeValidationInput(10u, 1600u, 900u, previousViewProjection);
			sceneViewInput.viewClass = VulkanHiZViewClass::Scene;
			const VulkanHiZValidationResult multiViewReuse = ValidateVulkanHiZHistory(history, sceneViewInput);

			VulkanHiZValidationInput shadowInput = MakeValidationInput(9u, 1600u, 900u, previousViewProjection);
			shadowInput.viewClass = VulkanHiZViewClass::Shadow;
			const VulkanHiZValidationResult shadowView = ValidateVulkanHiZHistory(history, shadowInput);

			context.Expect(cameraCut.occlusionAllowed, "Camera movement should keep Hi-Z active so virtual geometry does not lose occlusion while navigating.");
			context.Expect(!cameraCut.projectionMatches, "Diagnostics should still show that the sampled pyramid came from a different view-projection.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(cameraCut.invalidReason), "Projection movement is diagnostic-only in the always-active Hi-Z path.");
			context.Expect(multiViewReuse.occlusionAllowed, "Scene/Game view switches should keep Hi-Z active so editor interaction cannot permanently disable occlusion.");
			context.Expect(!multiViewReuse.sameViewHistory, "Diagnostics should still reveal that the previous pyramid owner was another view.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(multiViewReuse.invalidReason), "View mismatch is diagnostic-only for the always-active editor Hi-Z path.");
			context.Expect(shadowView.occlusionAllowed, "Shadow-view metadata should not latch the shared Hi-Z validation state off.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::None), static_cast<std::uint32_t>(shadowView.invalidReason), "Shadow-view metadata should stay diagnostic-only in validation.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.ReportsWarmCurrentPyramidAcrossResizeAndCameraCut", [](TestContext& context) {
			const glm::mat4 viewProjection(1.0f);
			VulkanHiZHistoryMetadata history = MakeValidHistory(15u, 1280u, 720u, viewProjection);
			history.warmupFrames = 2u;

			VulkanHiZValidationInput resizedInput = MakeValidationInput(15u, 1920u, 1080u, viewProjection);
			const VulkanHiZValidationResult resized = ValidateVulkanHiZHistory(history, resizedInput);
			context.Expect(resized.occlusionAllowed, "A resized view with a ready current pyramid should continue sampling Hi-Z.");
			context.ExpectEqual(1920u, resized.width, "Validation should publish the current pyramid width, not stale history width.");
			context.ExpectEqual(1080u, resized.height, "Validation should publish the current pyramid height, not stale history height.");
			context.Expect(resized.warmupFrames >= 1u, "A ready resized pyramid should publish at least one warm-up frame.");

			VulkanHiZValidationInput cameraCutInput = resizedInput;
			cameraCutInput.cameraCut = true;
			cameraCutInput.projectionSignature += 1u;
			const VulkanHiZValidationResult cameraCut = ValidateVulkanHiZHistory(history, cameraCutInput);
			context.Expect(cameraCut.occlusionAllowed, "Camera cuts should not hard-disable a ready current pyramid.");
			context.Expect(!cameraCut.projectionMatches, "Camera-cut diagnostics should retain projection mismatch information.");
			context.Expect(cameraCut.warmupFrames >= 1u, "Camera cuts should keep the current pyramid warm instead of reporting unavailable state.");
		});

		registry.Add("VirtualGeometry.HiZ.Validation.ProjectedBoundsFailOpenWhenUncertain", [](TestContext& context) {
			VulkanHiZProjectedBoundsInput missing{};
			missing.pyramidReady = false;
			missing.clipW = 2.0f;
			missing.uvCenter = glm::vec2(0.5f, 0.5f);
			missing.uvRadius = glm::vec2(0.1f, 0.1f);
			missing.viewportSize = glm::uvec2(1920u, 1080u);
			const VulkanHiZProjectedBoundsResult missingResult = ValidateVulkanHiZProjectedBounds(missing);
			context.Expect(!missingResult.sampleAllowed, "Missing pyramid resources should prevent task-shader Hi-Z samples.");
			context.Expect(missingResult.failOpen, "Missing pyramid resources should fail open per meshlet.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::MissingPyramid), static_cast<std::uint32_t>(missingResult.invalidReason), "Missing projected-bound resources should explain the fail-open reason.");

			VulkanHiZProjectedBoundsInput nearPlane = missing;
			nearPlane.pyramidReady = true;
			nearPlane.clipW = 0.0f;
			const VulkanHiZProjectedBoundsResult nearPlaneResult = ValidateVulkanHiZProjectedBounds(nearPlane);
			context.Expect(!nearPlaneResult.sampleAllowed, "Near-plane-crossing bounds should not sample Hi-Z.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::NearPlaneRisk), static_cast<std::uint32_t>(nearPlaneResult.invalidReason), "Near-plane fail-open should remain explainable.");

			VulkanHiZProjectedBoundsInput partial = missing;
			partial.pyramidReady = true;
			partial.clipW = 3.0f;
			partial.uvCenter = glm::vec2(0.98f, 0.5f);
			partial.uvRadius = glm::vec2(0.05f, 0.05f);
			const VulkanHiZProjectedBoundsResult partialResult = ValidateVulkanHiZProjectedBounds(partial);
			context.Expect(!partialResult.sampleAllowed, "A meshlet whose projected sphere crosses the view edge should fail open rather than cull from a clipped Hi-Z rectangle.");
			context.ExpectEqual(static_cast<std::uint32_t>(VulkanHiZInvalidReason::ProjectedBoundsRisk), static_cast<std::uint32_t>(partialResult.invalidReason), "Partial screen coverage should use the conservative projected-bounds fail-open reason.");

			VulkanHiZProjectedBoundsInput valid = missing;
			valid.pyramidReady = true;
			valid.clipW = 5.0f;
			valid.uvCenter = glm::vec2(0.5f, 0.5f);
			valid.uvRadius = glm::vec2(0.02f, 0.03f);
			const VulkanHiZProjectedBoundsResult validResult = ValidateVulkanHiZProjectedBounds(valid);
			context.Expect(validResult.sampleAllowed, "Fully projected meshlet bounds inside the view should be eligible for conservative Hi-Z sampling.");
			context.Expect(!validResult.failOpen, "Valid projected bounds should not report fail-open.");
			context.Expect(validResult.uvMin.x < valid.uvCenter.x && validResult.uvMax.x > valid.uvCenter.x, "Valid projected bounds should publish a padded sampling rectangle.");
		});
	}
}
