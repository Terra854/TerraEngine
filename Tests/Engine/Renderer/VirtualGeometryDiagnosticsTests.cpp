#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"
#include "Renderer/Graph/RenderFrameSnapshot.h"

#include <algorithm>
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

		registry.Add("VirtualGeometry.Diagnostics.US2.PublishesCompactionSourceCounters", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 3u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::Prepared;
			CompactedVisibilitySet compaction;
			compaction.viewType = RenderViewType::Game;
			compaction.viewId = 3u;
			compaction.candidatePackets = 12u;
			compaction.visiblePackets = 7u;
			compaction.rejectedByFrustum = 2u;
			compaction.rejectedByResidency = 1u;
			compaction.rejectedByCone = 1u;
			compaction.failOpenPackets = 2u;
			compaction.submittedBatches = 3u;
			visibility.compactionResults.push_back(compaction);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(12u, diagnostics.candidateClusterCount, "Diagnostics should expose prepared candidate packet count.");
			context.ExpectEqual(7u, diagnostics.compactedClusterCount, "Diagnostics should expose prepared compacted packet count.");
			context.ExpectEqual(4u, diagnostics.rejectedClusterCount, "Diagnostics should aggregate all prepared rejection domains.");
			context.ExpectEqual(2u, diagnostics.failOpenClusterCount, "Diagnostics should expose prepared fail-open packet count.");
			context.ExpectEqual(static_cast<std::uint32_t>(VirtualGeometryCounterSource::Prepared), static_cast<std::uint32_t>(diagnostics.counterSource), "Compaction counters should keep their prepared-source label.");
		});

		registry.Add("VirtualGeometry.Diagnostics.US6.PublishesGpuPacketCompactionAndGeneratedCounters", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 4u;
			visibility.frameId = 9606u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;

			GpuVirtualGeometryPacketRecord packet{};
			packet.packetId = 0u;
			packet.viewId = visibility.outputId;
			packet.viewType = visibility.viewType;
			packet.frameId = visibility.frameId;
			packet.meshletCount = 18u;
			visibility.gpuPacketRecords.push_back(packet);

			GpuVirtualGeometryBatchDescriptor batch{};
			batch.batchId = 0u;
			batch.viewId = visibility.outputId;
			batch.viewType = visibility.viewType;
			batch.frameId = visibility.frameId;
			batch.packetCount = 1u;
			batch.submittedMeshlets = 18u;
			visibility.gpuBatchDescriptors.push_back(batch);

			GpuVirtualGeometryPacketFrameSet frameSet{};
			frameSet.viewId = visibility.outputId;
			frameSet.viewType = visibility.viewType;
			frameSet.frameId = visibility.frameId;
			frameSet.packetCount = 1u;
			frameSet.batchCount = 1u;
			frameSet.validity = VirtualGeometryGpuFrameSetValidity::Valid;
			visibility.gpuPacketFrameSets.push_back(frameSet);

			GpuPacketCompactionDispatch dispatch{};
			dispatch.dispatchId = 0u;
			dispatch.viewId = visibility.outputId;
			dispatch.viewType = visibility.viewType;
			dispatch.frameId = visibility.frameId;
			dispatch.candidatePacketCount = 2u;
			dispatch.outputCapacity = 4u;
			visibility.gpuCompactionDispatches.push_back(dispatch);

			GeneratedVirtualGeometrySubmissionRecord submission{};
			submission.recordId = 0u;
			submission.viewId = visibility.outputId;
			submission.viewType = visibility.viewType;
			submission.frameId = visibility.frameId;
			submission.packetCount = 1u;
			submission.validationState = VirtualGeometryGeneratedSubmissionValidationState::Valid;
			visibility.generatedSubmissions.push_back(submission);

			VirtualGeometryCounterReadbackRecord readback{};
			readback.viewId = visibility.outputId;
			readback.viewType = visibility.viewType;
			readback.frameId = visibility.frameId;
			readback.source = VirtualGeometryCounterSource::GPUReadback;
			readback.latencyFrames = 1u;
			readback.candidatePackets = 2u;
			readback.compactedPackets = 1u;
			visibility.gpuCounterReadbacks.push_back(readback);

			VirtualGeometryGpuFallbackRecord fallback{};
			fallback.viewId = visibility.outputId;
			fallback.viewType = visibility.viewType;
			fallback.frameId = visibility.frameId;
			fallback.reason = "generated validation fallback";
			fallback.affectedPackets = 2u;
			fallback.affectedBatches = 1u;
			visibility.gpuFallbackRecords.push_back(fallback);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(1u, diagnostics.gpuPacketRecordCount, "Diagnostics should expose GPU packet record count.");
			context.ExpectEqual(1u, diagnostics.gpuBatchDescriptorCount, "Diagnostics should expose GPU batch descriptor count.");
			context.ExpectEqual(1u, diagnostics.gpuValidFrameSetCount, "Diagnostics should count valid GPU packet frame sets.");
			context.ExpectEqual(1u, diagnostics.gpuCompactionDispatchCount, "Diagnostics should expose compaction dispatch count.");
			context.ExpectEqual(1u, diagnostics.validGeneratedSubmissionCount, "Diagnostics should count valid generated submissions.");
			context.ExpectEqual(1u, diagnostics.gpuCounterReadbackCount, "Diagnostics should expose counter readback records.");
			context.ExpectEqual(1u, diagnostics.gpuCounterReadbackLatencyFrames, "Diagnostics should preserve GPU counter latency.");
			context.ExpectEqual(2u, diagnostics.gpuFallbackAffectedPackets, "Diagnostics should expose fallback affected packet count.");
			context.Expect(diagnostics.gpuFallbackReason.find("fallback") != std::string::npos, "Diagnostics should preserve source-labeled fallback reasons.");
		});

		registry.Add("VirtualGeometry.Diagnostics.US6.PublishesPageSourcePayloadAndEmissionCounters", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Scene;
			visibility.outputId = 8u;
			visibility.frameId = 9607u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::GPUReadback;

			VirtualGeometryPagePayloadRecord highDetail{};
			highDetail.meshId = TerraEngine::GenerateUUID();
			highDetail.pageId = 12u;
			highDetail.source = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			highDetail.payloadByteSize = 4096u;
			highDetail.resident = true;
			visibility.residentPagePayloads.push_back(highDetail);

			VirtualGeometryPagePayloadRecord fallback{};
			fallback.meshId = highDetail.meshId;
			fallback.pageId = 2u;
			fallback.source = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			fallback.payloadByteSize = 1024u;
			fallback.resident = true;
			fallback.pinnedFallback = true;
			visibility.residentPagePayloads.push_back(fallback);

			VirtualGeometryPagePayloadRecord embedded{};
			embedded.meshId = highDetail.meshId;
			embedded.pageId = 0u;
			embedded.source = VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback;
			embedded.payloadByteSize = 512u;
			embedded.resident = true;
			visibility.residentPagePayloads.push_back(embedded);

			VirtualGeometryEmittedPageSourceRecord emittedHigh{};
			emittedHigh.viewId = visibility.outputId;
			emittedHigh.viewType = visibility.viewType;
			emittedHigh.frameId = visibility.frameId;
			emittedHigh.meshId = highDetail.meshId;
			emittedHigh.materialId = TerraEngine::GenerateUUID();
			emittedHigh.pageId = highDetail.pageId;
			emittedHigh.emittedSource = VirtualGeometryEmittedPageSource::HighDetailPage;
			visibility.emittedPageSources.push_back(emittedHigh);

			VirtualGeometryEmittedPageSourceRecord emittedFallback = emittedHigh;
			emittedFallback.pageId = fallback.pageId;
			emittedFallback.fallbackPageId = fallback.pageId;
			emittedFallback.emittedSource = VirtualGeometryEmittedPageSource::FallbackPage;
			emittedFallback.fallbackSubstitution = true;
			visibility.emittedPageSources.push_back(emittedFallback);

			VirtualGeometryEmittedPageSourceRecord emittedEmbedded = emittedHigh;
			emittedEmbedded.pageId = embedded.pageId;
			emittedEmbedded.emittedSource = VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback;
			emittedEmbedded.conservativeEmbeddedFallback = true;
			emittedEmbedded.reason = "legacy embedded fallback used";
			visibility.emittedPageSources.push_back(emittedEmbedded);

			VirtualGeometryEmittedPageSourceRecord emittedMissing = emittedHigh;
			emittedMissing.pageId = 99u;
			emittedMissing.emittedSource = VirtualGeometryEmittedPageSource::NoSubmittedWork;
			visibility.emittedPageSources.push_back(emittedMissing);

			TerraEngine::Renderer::VirtualGeometryPageRequestRecord pageRequest{};
			pageRequest.meshId = highDetail.meshId;
			pageRequest.pageId = 13u;
			pageRequest.fallbackPageId = fallback.pageId;
			pageRequest.reason = "detail page needed";
			visibility.pageRequests.push_back(pageRequest);

			VirtualGeometryPageRetirementRecord pageRetirement{};
			pageRetirement.meshId = highDetail.meshId;
			pageRetirement.pageId = 14u;
			pageRetirement.byteSize = 2048u;
			visibility.pageRetirements.push_back(pageRetirement);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(3u, diagnostics.pageSources.residentPayloads, "Diagnostics should count resident page payload records per view.");
			context.ExpectEqual(1u, diagnostics.pageSources.highDetailPayloads, "Diagnostics should count resident high-detail payloads separately.");
			context.ExpectEqual(1u, diagnostics.pageSources.fallbackPayloads, "Diagnostics should count resident fallback payloads separately.");
			context.ExpectEqual(1u, diagnostics.pageSources.embeddedFallbackPayloads, "Diagnostics should count conservative embedded fallback payloads separately.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedHighDetailSources, "Diagnostics should count emitted high-detail page sources.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedFallbackSources, "Diagnostics should count emitted fallback page sources.");
			context.ExpectEqual(1u, diagnostics.pageSources.emittedEmbeddedFallbackSources, "Diagnostics should count emitted embedded fallback sources.");
			context.ExpectEqual(1u, diagnostics.pageSources.missingSources, "Diagnostics should count no-work/invalid page-source records.");
			context.ExpectEqual(1u, diagnostics.pageSources.pageRequests, "Diagnostics should publish page request count.");
			context.ExpectEqual(1u, diagnostics.pageSources.pageRetirements, "Diagnostics should publish page retirement count.");
			context.ExpectEqual(static_cast<std::uint64_t>(5632u), diagnostics.pageSources.residentPayloadBytes, "Diagnostics should sum resident page-source bytes.");
			context.ExpectEqual(static_cast<std::uint64_t>(1024u), diagnostics.pageSources.fallbackPayloadBytes, "Diagnostics should sum fallback payload bytes.");
			context.ExpectEqual(static_cast<std::uint64_t>(512u), diagnostics.pageSources.embeddedFallbackBytes, "Diagnostics should sum embedded fallback bytes.");
			context.Expect(!diagnostics.warnings.empty(), "Embedded fallback source use should leave a visible diagnostic warning.");
		});

		registry.Add("VirtualGeometry.Diagnostics.US7.UnsupportedCounterSourceGatesDoNotPassSilently", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 2u;
			visibility.frameId = 9707u;
			visibility.valid = true;
			visibility.source = VirtualGeometryCounterSource::Unavailable;
			visibility.counters.source = visibility.source;
			visibility.counters.unavailableReason = "readback unavailable in fixture";

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);
			const auto sourceGate = std::find_if(
				diagnostics.virtualGeometryPerformanceGates.begin(),
				diagnostics.virtualGeometryPerformanceGates.end(),
				[](const PerformanceGateResult& gate) {
					return gate.gateId == "virtual-geometry-counter-source-available";
				});

			context.Expect(sourceGate != diagnostics.virtualGeometryPerformanceGates.end(), "Unavailable counter sources should publish an explicit performance gate.");
			context.Expect(sourceGate->status == VirtualGeometryPerformanceGateStatus::Unsupported, "Unavailable counter sources should be unsupported, not pass.");
			context.Expect(sourceGate->source == VirtualGeometryPerformanceGateSource::Unsupported, "Unavailable counter sources should map to the unsupported performance-gate source.");
			context.ExpectEqual(2u, sourceGate->viewId, "Unsupported counter-source gates should carry the producing view id.");
			context.Expect(sourceGate->diagnostic.find("readback unavailable") != std::string::npos, "Unsupported counter-source gates should carry the unavailable reason.");
		});

		registry.Add("VirtualGeometry.Diagnostics.US5.PublishesPerShadowViewCountersSeparately", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Shadow;
			visibility.outputId = 1002u;
			visibility.frameId = 9305u;
			visibility.valid = true;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Unsupported;
			visibility.hiZValidationState.reason = "shadow view does not sample camera Hi-Z";

			ShadowVisibilitySelection shadow{};
			shadow.shadowViewId = visibility.outputId;
			shadow.frameIndex = visibility.frameId;
			shadow.candidatePackets = 6u;
			shadow.selectedPackets = 4u;
			shadow.rejectedByLightFrustum = 2u;
			shadow.selectedMeshlets = 120u;
			shadow.dispatchedMeshlets = 72u;
			shadow.fallbackReasons.push_back("shadow light-frustum fixture");
			visibility.shadowVisibilitySelections.push_back(shadow);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot diagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(1u, diagnostics.shadowViewCount, "Shadow diagnostics should count per-shadow selections.");
			context.ExpectEqual(4u, diagnostics.shadowVisibilitySelections.front().selectedPackets, "Shadow diagnostics should expose selected packets.");
			context.ExpectEqual(2u, diagnostics.shadowVisibilitySelections.front().rejectedByLightFrustum, "Shadow diagnostics should expose light-frustum rejections.");
			context.ExpectEqual(72u, diagnostics.shadowVisibilitySelections.front().dispatchedMeshlets, "Shadow diagnostics should expose shadow-dispatched meshlets.");
			context.Expect(diagnostics.hiZState == VirtualGeometryHiZState::Unsupported, "Shadow diagnostics should keep camera Hi-Z separate from light-view visibility.");
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

		registry.Add("VirtualGeometry.Diagnostics.CarriesBoundedHiZExplanationSamples", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.frameId = 930u;
			visibility.valid = true;
			visibility.hiZValidationState.state = VirtualGeometryHiZState::Active;
			visibility.hiZValidationState.pyramidReady = true;
			visibility.hiZValidationState.warmupFrames = 3u;
			visibility.hiZValidationState.pyramidSize = glm::uvec2(1280u, 720u);
			visibility.hiZValidationState.renderSize = glm::uvec2(1280u, 720u);

			for (std::uint32_t index = 0u; index < TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity + 8u; ++index)
			{
				TerraEngine::Renderer::OcclusionDebugSample sample{};
				sample.meshletId = index;
				sample.clusterId = index / 2u;
				sample.outcome = index == 0u ? VirtualGeometryCullOutcome::HiZCulled : VirtualGeometryCullOutcome::Visible;
				sample.reason = index == 0u ? "Hi-Z rectangle proved full occlusion." : "Visible or fail-open sample.";
				visibility.occlusionDebugSamples.push_back(sample);
			}

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			context.ExpectEqual(TerraEngine::Renderer::kVirtualGeometryOcclusionDebugSampleCapacity, static_cast<std::uint32_t>(snapshot.occlusionDebugSamples.size()), "Diagnostics should keep a bounded representative Hi-Z explanation list.");
			context.Expect(snapshot.occlusionDebugSamples.front().outcome == VirtualGeometryCullOutcome::HiZCulled, "The first representative Hi-Z sample should preserve its outcome.");
			context.Expect(!snapshot.occlusionDebugSamples.front().reason.empty(), "Representative Hi-Z samples should carry user-facing explanation text.");
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

		registry.Add("VirtualGeometry.Diagnostics.VisibilityResidencyPublishesPerAssetContributors", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 44u;
			visibility.frameId = 8801u;
			visibility.valid = true;

			PageResidencyRecord resident{};
			resident.meshId = meshId;
			resident.pageId = 1u;
			resident.state = GeometryPageState::Resident;
			resident.byteCost = 1024u;
			visibility.pageResidency.push_back(resident);

			PageResidencyRecord pinnedFallback{};
			pinnedFallback.meshId = meshId;
			pinnedFallback.pageId = 0u;
			pinnedFallback.state = GeometryPageState::Pinned;
			pinnedFallback.isFallback = true;
			pinnedFallback.protectedFallback = true;
			pinnedFallback.byteCost = 256u;
			visibility.pageResidency.push_back(pinnedFallback);

			PageResidencyRecord pendingUpload{};
			pendingUpload.meshId = meshId;
			pendingUpload.pageId = 2u;
			pendingUpload.state = GeometryPageState::PendingUpload;
			pendingUpload.byteCost = 512u;
			visibility.pageResidency.push_back(pendingUpload);

			PageResidencyRecord missing{};
			missing.meshId = meshId;
			missing.pageId = 3u;
			missing.state = GeometryPageState::NotResident;
			missing.byteCost = 128u;
			visibility.pageResidency.push_back(missing);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			const auto contributor = std::find_if(
				snapshot.budgetContributors.begin(),
				snapshot.budgetContributors.end(),
				[&meshId](const TerraEngine::Renderer::VirtualGeometryBudgetContributor& value) {
					return value.assetId == meshId;
				});

			context.Expect(contributor != snapshot.budgetContributors.end(), "Visibility diagnostics should publish a per-asset contributor for virtual geometry page residency.");
			if (contributor != snapshot.budgetContributors.end())
			{
				context.ExpectEqual(2u, contributor->residentPages, "Resident and pinned fallback pages should both be renderable for the asset.");
				context.ExpectEqual(1280ull, contributor->residentBytes, "Renderable page bytes should include resident and pinned fallback pages.");
				context.ExpectEqual(1u, contributor->pinnedFallbackPages, "Pinned fallback pages should stay visible in per-asset diagnostics.");
				context.ExpectEqual(256ull, contributor->pinnedFallbackBytes, "Pinned fallback bytes should be attributed to the mesh asset.");
				context.ExpectEqual(512ull, contributor->requestedBytes, "Pending upload pages should be reported as requested per-asset work.");
				context.ExpectEqual(1u, contributor->missingPages, "Missing pages should be attributed to the mesh asset so editor diagnostics can explain fallback rendering.");
			}
		});

		registry.Add("VirtualGeometry.Diagnostics.ResidentPayloadsPublishPerAssetBytesWithoutPageRecords", [](TestContext& context) {
			const TerraEngine::UUID meshId = TerraEngine::GenerateUUID();
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot visibility{};
			visibility.viewType = RenderViewType::Game;
			visibility.outputId = 45u;
			visibility.frameId = 8802u;
			visibility.valid = true;

			VirtualGeometryPagePayloadRecord highDetail{};
			highDetail.meshId = meshId;
			highDetail.pageId = 4u;
			highDetail.source = VirtualGeometryPagePayloadSource::ResidentPagePayload;
			highDetail.payloadByteSize = 4096u;
			highDetail.resident = true;
			visibility.residentPagePayloads.push_back(highDetail);

			VirtualGeometryPagePayloadRecord fallback{};
			fallback.meshId = meshId;
			fallback.pageId = 0u;
			fallback.source = VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
			fallback.payloadByteSize = 1024u;
			fallback.resident = true;
			fallback.pinnedFallback = true;
			visibility.residentPagePayloads.push_back(fallback);

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);

			const auto contributor = std::find_if(
				snapshot.budgetContributors.begin(),
				snapshot.budgetContributors.end(),
				[&meshId](const TerraEngine::Renderer::VirtualGeometryBudgetContributor& value) {
					return value.assetId == meshId;
				});

			context.Expect(contributor != snapshot.budgetContributors.end(), "Renderable VG payloads should publish per-asset bytes even when no explicit page residency records were captured.");
			if (contributor != snapshot.budgetContributors.end())
			{
				context.ExpectEqual(2u, contributor->residentPages, "High-detail and fallback payloads should both count as renderable resident payloads.");
				context.ExpectEqual(5120ull, contributor->residentBytes, "Per-asset resident bytes should fall back to payload byte sizes from live render records.");
				context.ExpectEqual(4096ull, contributor->reclaimableBytes, "High-detail page payloads should be reported as reclaimable page memory.");
				context.ExpectEqual(1u, contributor->pinnedFallbackPages, "Fallback payloads should remain visible as pinned fallback coverage.");
				context.ExpectEqual(1024ull, contributor->pinnedFallbackBytes, "Pinned fallback bytes should use the payload byte size.");
			}
		});

		registry.Add("VirtualGeometry.Diagnostics.InactiveReasonsRemainExplicit", [](TestContext& context) {
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot snapshot =
				TerraEngine::Renderer::BuildInactiveVirtualGeometryDiagnostics(RenderViewType::Scene, 33u, VirtualGeometryInactiveReason::UnsupportedView);

			context.Expect(!snapshot.active, "Inactive diagnostics should mark the view inactive.");
			context.Expect(snapshot.inactiveReason == VirtualGeometryInactiveReason::UnsupportedView, "Inactive diagnostics should preserve the exact reason.");
			context.Expect(std::string(ToString(snapshot.inactiveReason)).find("Unsupported") != std::string::npos, "Inactive reason should have a user-facing label.");
			context.Expect(!snapshot.warnings.empty(), "Inactive diagnostics should include a warning explaining fallback rendering.");
		});

		registry.Add("VirtualGeometry.Diagnostics.VisualizerModeChangesPublishNextFrame", [](TestContext& context) {
			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot previousFrame{};
			previousFrame.viewType = RenderViewType::Game;
			previousFrame.frameId = 401u;
			previousFrame.valid = true;
			previousFrame.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::LOD;

			TerraEngine::Renderer::VirtualGeometryViewVisibilitySnapshot nextFrame = previousFrame;
			nextFrame.frameId = 402u;
			nextFrame.debugVisualizationMode = VirtualGeometryDebugVisualizationMode::HiZ;

			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot previousDiagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(previousFrame);
			const TerraEngine::Renderer::VirtualGeometryViewDiagnosticsSnapshot nextDiagnostics =
				TerraEngine::Renderer::BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(nextFrame);

			context.Expect(previousDiagnostics.debugVisualizationMode == VirtualGeometryDebugVisualizationMode::LOD, "The previous diagnostics snapshot should preserve its published visualizer mode.");
			context.Expect(nextDiagnostics.debugVisualizationMode == VirtualGeometryDebugVisualizationMode::HiZ, "The next diagnostics snapshot should publish the changed visualizer mode without waiting for camera movement.");

			VirtualGeometryQualityProfile previousProfile = MakeDefaultVirtualGeometryQualityProfile();
			previousProfile.debugVisualizationMode = previousDiagnostics.debugVisualizationMode;
			VirtualGeometryQualityProfile nextProfile = MakeDefaultVirtualGeometryQualityProfile();
			nextProfile.debugVisualizationMode = nextDiagnostics.debugVisualizationMode;
			context.Expect(
				BuildVirtualGeometryQualityProfileSignature(previousProfile) != BuildVirtualGeometryQualityProfileSignature(nextProfile),
				"Changing the visualizer mode should also change the visible-command cache signature for the next rendered frame.");
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
