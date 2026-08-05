#include "RenderFrameSnapshot.h"
#include "RenderDiagnostics.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace TerraEngine::Renderer {

	namespace {
		std::shared_ptr<const AnimatedObjectVisibilitySnapshot>
			gAnimatedObjectVisibilitySnapshot;

		void HashCombine(std::uint64_t& seed, std::uint64_t value)
		{
			seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
		}

		std::uint64_t HashUuid(const TerraEngine::UUID& id)
		{
			return static_cast<std::uint64_t>(std::hash<TerraEngine::UUID>{}(id));
		}

		template<typename Record>
		void SortByUuid(std::vector<Record>& records, TerraEngine::UUID Record::* member)
		{
			const auto orderedBefore = [member](const Record& left, const Record& right) {
				// Two-digit hexadecimal text preserves byte-array order. Compare UUID
				// bytes directly so sorting does not allocate two strings per comparison.
				return left.*member < right.*member;
			};
			// RenderSystem canonicalizes retained membership when structure changes.
			// Preserve that steady-state order with one linear check instead of an
			// unconditional O(N log N) sort on every frame.
			if(!std::is_sorted(records.begin(), records.end(), orderedBefore)) {
				std::sort(records.begin(), records.end(), orderedBefore);
			}
		}

		template<typename Record, typename Compare>
		void SortIfNeeded(std::vector<Record>& records, Compare compare)
		{
			if(!std::is_sorted(records.begin(), records.end(), compare)) {
				std::sort(records.begin(), records.end(), compare);
			}
		}
		void HashVirtualGeometryRange(std::uint64_t& hash, const VirtualGeometryDrawRange& range)
		{
			HashCombine(hash, range.selectedLodIndex);
			HashCombine(hash, range.meshletOffset);
			HashCombine(hash, range.meshletCount);
			HashCombine(hash, range.virtualGeometryActive ? 1u : 0u);
			HashCombine(hash, range.failOpen ? 1u : 0u);
			HashCombine(hash, range.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, range.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(range.hiZInactiveReason)));
			HashCombine(hash, range.selectedClusterOffset);
			HashCombine(hash, range.selectedClusterCount);
			HashCombine(hash, range.pageNeedOffset);
			HashCombine(hash, range.pageNeedCount);
			HashCombine(hash, range.pageId);
			HashCombine(hash, range.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(range.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(range.emittedSource));
			HashCombine(hash, range.pagePayloadByteOffset);
			HashCombine(hash, range.pagePayloadByteSize);
			HashCombine(hash, range.payloadBufferIndex);
			HashCombine(hash, range.payloadDescriptorIndex);
		}

		void HashVirtualGeometryCounters(std::uint64_t& hash, const VirtualGeometryCounterSnapshot& counters)
		{
			HashCombine(hash, static_cast<std::uint64_t>(counters.source));
			HashCombine(hash, counters.preparedFrameId);
			HashCombine(hash, counters.readbackFrameId);
			HashCombine(hash, counters.readbackLatencyFrames);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(counters.unavailableReason)));
			HashCombine(hash, counters.toleranceMeshlets);
			HashCombine(hash, static_cast<std::uint32_t>(std::round(counters.tolerancePercent * 1000.0f)));
			HashCombine(hash, counters.sourceMeshlets);
			HashCombine(hash, counters.selectedMeshlets);
			HashCombine(hash, counters.dispatchedMeshlets);
			HashCombine(hash, counters.culledMeshlets);
			HashCombine(hash, counters.culledByFrustum);
			HashCombine(hash, counters.culledByCone);
			HashCombine(hash, counters.culledByHiZ);
			HashCombine(hash, counters.fallbackMeshlets);
			HashCombine(hash, counters.residentPages);
			HashCombine(hash, counters.requestedPages);
			HashCombine(hash, counters.missingPages);
			HashCombine(hash, counters.fallbackSubstitutions);
			HashCombine(hash, counters.residentPagePayloads);
			HashCombine(hash, counters.fallbackPagePayloads);
			HashCombine(hash, counters.embeddedFallbackPayloads);
			HashCombine(hash, counters.emittedPageSources);
			HashCombine(hash, counters.authoritative ? 1u : 0u);
		}

		void HashVirtualGeometryHiZState(std::uint64_t& hash, const VirtualGeometryHiZValidationState& state)
		{
			HashCombine(hash, static_cast<std::uint64_t>(state.state));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(state.reason)));
			HashCombine(hash, state.pyramidSize.x);
			HashCombine(hash, state.pyramidSize.y);
			HashCombine(hash, state.renderSize.x);
			HashCombine(hash, state.renderSize.y);
			HashCombine(hash, state.cameraSignature);
			HashCombine(hash, state.depthSceneSignature);
			HashCombine(hash, state.warmupFrames);
			HashCombine(hash, state.pyramidReady ? 1u : 0u);
			HashCombine(hash, state.failOpen ? 1u : 0u);
		}

		void HashVirtualGeometryOcclusionSample(std::uint64_t& hash, const OcclusionDebugSample& sample)
		{
			HashCombine(hash, HashUuid(sample.objectId));
			HashCombine(hash, HashUuid(sample.meshId));
			HashCombine(hash, sample.clusterId);
			HashCombine(hash, sample.meshletId);
			HashCombine(hash, sample.sampledMip);
			HashCombine(hash, static_cast<std::uint64_t>(sample.outcome));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(sample.reason)));
		}

		void HashVirtualGeometryLODTransitionState(std::uint64_t& hash, const VirtualGeometryLODTransitionState& state)
		{
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.outputId);
			HashCombine(hash, HashUuid(state.objectId));
			HashCombine(hash, HashUuid(state.meshId));
			HashCombine(hash, state.viewStateKey);
			HashCombine(hash, state.lastStableSelectionSignature);
			HashCombine(hash, state.candidateSelectionSignature);
			HashCombine(hash, state.stableFrameCount);
			HashCombine(hash, state.heldFrameCount);
			HashCombine(hash, state.acceptedFrameCount);
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.hysteresisBandPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.transitionMarginPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(state.candidateMaxErrorPixels * 1000.0f)));
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, state.heldPreviousSelection ? 1u : 0u);
			HashCombine(hash, state.resetThisFrame ? 1u : 0u);
			HashCombine(hash, state.valid ? 1u : 0u);
		}

		void HashVirtualGeometryClusterPacket(std::uint64_t& hash, const VirtualGeometryClusterPacket& packet)
		{
			HashCombine(hash, packet.packetId);
			HashCombine(hash, packet.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(packet.viewType));
			HashCombine(hash, HashUuid(packet.objectId));
			HashCombine(hash, HashUuid(packet.meshId));
			HashCombine(hash, HashUuid(packet.materialId));
			HashCombine(hash, packet.selectedLod);
			HashCombine(hash, packet.clusterId);
			HashCombine(hash, packet.meshletOffset);
			HashCombine(hash, packet.meshletCount);
			HashCombine(hash, packet.pageId);
			HashCombine(hash, packet.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(packet.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(packet.emittedSource));
			HashCombine(hash, packet.pagePayloadByteOffset);
			HashCombine(hash, packet.pagePayloadByteSize);
			HashCombine(hash, packet.payloadBufferIndex);
			HashCombine(hash, packet.payloadDescriptorIndex);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(packet.fallbackReason)));
			HashCombine(hash, packet.debugIdentity);
			HashCombine(hash, packet.fallbackPacket ? 1u : 0u);
		}

		void HashClusterWorkBatch(std::uint64_t& hash, const ClusterWorkBatch& batch)
		{
			HashCombine(hash, batch.batchId);
			HashCombine(hash, batch.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(batch.viewType));
			HashCombine(hash, HashUuid(batch.meshId));
			HashCombine(hash, HashUuid(batch.materialId));
			HashCombine(hash, batch.packetOffset);
			HashCombine(hash, batch.packetCount);
			HashCombine(hash, batch.selectedMeshlets);
			HashCombine(hash, batch.submittedMeshlets);
			HashCombine(hash, static_cast<std::uint64_t>(batch.fallbackMode));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(batch.batchReason)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(batch.fallbackReason)));
		}

		void HashCompactedVisibilitySet(std::uint64_t& hash, const CompactedVisibilitySet& compaction)
		{
			HashCombine(hash, compaction.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(compaction.viewType));
			HashCombine(hash, compaction.candidatePackets);
			HashCombine(hash, compaction.visiblePackets);
			HashCombine(hash, compaction.rejectedByFrustum);
			HashCombine(hash, compaction.rejectedByHiZ);
			HashCombine(hash, compaction.rejectedByResidency);
			HashCombine(hash, compaction.rejectedByCone);
			HashCombine(hash, compaction.failOpenPackets);
			HashCombine(hash, compaction.submittedBatches);
			HashCombine(hash, static_cast<std::uint64_t>(compaction.source));
		}

		void HashGpuVirtualGeometryPacketRecord(std::uint64_t& hash, const GpuVirtualGeometryPacketRecord& packet)
		{
			// GPU packet records are immutable mirrors of what the backend may
			// upload. Hashing every compatibility field makes command reuse
			// invalidate when a packet would point at different geometry.
			HashCombine(hash, packet.packetId);
			HashCombine(hash, packet.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(packet.viewType));
			HashCombine(hash, packet.frameId);
			HashCombine(hash, HashUuid(packet.objectId));
			HashCombine(hash, HashUuid(packet.meshId));
			HashCombine(hash, HashUuid(packet.materialId));
			HashCombine(hash, packet.selectedLod);
			HashCombine(hash, packet.clusterId);
			HashCombine(hash, packet.meshletOffset);
			HashCombine(hash, packet.meshletCount);
			HashCombine(hash, packet.pageId);
			HashCombine(hash, packet.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(packet.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(packet.emittedSource));
			HashCombine(hash, packet.pagePayloadByteOffset);
			HashCombine(hash, packet.pagePayloadByteSize);
			HashCombine(hash, packet.payloadBufferIndex);
			HashCombine(hash, packet.payloadDescriptorIndex);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(packet.fallbackReason)));
			HashCombine(hash, packet.debugIdentity);
			HashCombine(hash, packet.flags);
		}

		void HashGpuVirtualGeometryBatchDescriptor(std::uint64_t& hash, const GpuVirtualGeometryBatchDescriptor& batch)
		{
			HashCombine(hash, batch.batchId);
			HashCombine(hash, batch.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(batch.viewType));
			HashCombine(hash, batch.frameId);
			HashCombine(hash, batch.packetOffset);
			HashCombine(hash, batch.packetCount);
			HashCombine(hash, HashUuid(batch.meshId));
			HashCombine(hash, HashUuid(batch.materialId));
			HashCombine(hash, batch.selectedMeshlets);
			HashCombine(hash, batch.submittedMeshlets);
			HashCombine(hash, static_cast<std::uint64_t>(batch.debugMode));
			HashCombine(hash, static_cast<std::uint64_t>(batch.fallbackMode));
			HashCombine(hash, batch.flags);
		}

		void HashGpuVirtualGeometryPacketFrameSet(std::uint64_t& hash, const GpuVirtualGeometryPacketFrameSet& frameSet)
		{
			HashCombine(hash, frameSet.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(frameSet.viewType));
			HashCombine(hash, frameSet.frameId);
			HashCombine(hash, frameSet.targetSize.x);
			HashCombine(hash, frameSet.targetSize.y);
			HashCombine(hash, static_cast<std::uint64_t>(frameSet.debugMode));
			HashCombine(hash, frameSet.packetOffset);
			HashCombine(hash, frameSet.packetCount);
			HashCombine(hash, frameSet.batchOffset);
			HashCombine(hash, frameSet.batchCount);
			HashCombine(hash, frameSet.compactedPacketOffset);
			HashCombine(hash, frameSet.compactedPacketCount);
			HashCombine(hash, frameSet.generatedSubmissionOffset);
			HashCombine(hash, frameSet.generatedSubmissionCount);
			HashCombine(hash, static_cast<std::uint64_t>(frameSet.validity));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(frameSet.fallbackReason)));
		}

		void HashGpuPacketCompactionDispatch(std::uint64_t& hash, const GpuPacketCompactionDispatch& dispatch)
		{
			HashCombine(hash, dispatch.dispatchId);
			HashCombine(hash, dispatch.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(dispatch.viewType));
			HashCombine(hash, dispatch.frameId);
			HashCombine(hash, dispatch.candidatePacketCount);
			HashCombine(hash, dispatch.candidateBatchCount);
			HashCombine(hash, dispatch.outputCapacity);
			HashCombine(hash, static_cast<std::uint64_t>(dispatch.hiZBaselineState));
			HashCombine(hash, static_cast<std::uint64_t>(dispatch.debugMode));
			HashCombine(hash, dispatch.failOpenOnInvalid ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(dispatch.fallbackReason)));
		}

		void HashGeneratedVirtualGeometrySubmissionRecord(std::uint64_t& hash, const GeneratedVirtualGeometrySubmissionRecord& submission)
		{
			HashCombine(hash, submission.recordId);
			HashCombine(hash, submission.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(submission.viewType));
			HashCombine(hash, submission.frameId);
			HashCombine(hash, submission.batchId);
			HashCombine(hash, submission.packetOffset);
			HashCombine(hash, submission.packetCount);
			HashCombine(hash, submission.indirectTaskCount);
			HashCombine(hash, HashUuid(submission.materialId));
			HashCombine(hash, HashUuid(submission.meshId));
			HashCombine(hash, submission.counterOffset);
			HashCombine(hash, static_cast<std::uint64_t>(submission.source));
			HashCombine(hash, static_cast<std::uint64_t>(submission.validationState));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(submission.fallbackReason)));
		}

		void HashVirtualGeometryCounterReadbackRecord(std::uint64_t& hash, const VirtualGeometryCounterReadbackRecord& readback)
		{
			HashCombine(hash, readback.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(readback.viewType));
			HashCombine(hash, readback.frameId);
			HashCombine(hash, static_cast<std::uint64_t>(readback.source));
			HashCombine(hash, readback.latencyFrames);
			HashCombine(hash, readback.candidatePackets);
			HashCombine(hash, readback.compactedPackets);
			HashCombine(hash, readback.submittedBatches);
			HashCombine(hash, readback.rejectedPackets);
			HashCombine(hash, readback.failOpenPackets);
			HashCombine(hash, readback.selectedMeshlets);
			HashCombine(hash, readback.dispatchedMeshlets);
			HashCombine(hash, readback.culledMeshlets);
			HashCombine(hash, readback.culledByHiZ);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(readback.unavailableReason)));
		}

		void HashVirtualGeometryGpuFallbackRecord(std::uint64_t& hash, const VirtualGeometryGpuFallbackRecord& fallback)
		{
			HashCombine(hash, fallback.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(fallback.viewType));
			HashCombine(hash, fallback.frameId);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(fallback.reason)));
			HashCombine(hash, fallback.affectedPackets);
			HashCombine(hash, fallback.affectedBatches);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(fallback.coveragePolicy)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(fallback.diagnostic)));
		}

		void HashPageResidencyRecord(std::uint64_t& hash, const PageResidencyRecord& page)
		{
			HashCombine(hash, HashUuid(page.meshId));
			HashCombine(hash, page.pageId);
			HashCombine(hash, page.coveredClusterOffset);
			HashCombine(hash, page.coveredClusterCount);
			HashCombine(hash, page.byteCost);
			HashCombine(hash, page.payloadByteOffset);
			HashCombine(hash, page.payloadByteSize);
			HashCombine(hash, static_cast<std::uint64_t>(page.state));
			HashCombine(hash, static_cast<std::uint64_t>(page.payloadSource));
			HashCombine(hash, page.fallbackPageId);
			HashCombine(hash, page.isFallback ? 1u : 0u);
			HashCombine(hash, page.protectedFallback ? 1u : 0u);
			HashCombine(hash, page.lastRequestedFrame);
			HashCombine(hash, page.lastUsedFrame);
			HashCombine(hash, page.retirementFrame);
			HashCombine(hash, static_cast<std::uint64_t>(page.priority));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(page.failureReason)));
			for (const std::uint32_t clusterId : page.coveredClusters)
			{
				HashCombine(hash, clusterId);
			}
		}

		void HashVirtualGeometryPageRequestRecord(std::uint64_t& hash, const VirtualGeometryPageRequestRecord& request)
		{
			HashCombine(hash, HashUuid(request.meshId));
			HashCombine(hash, HashUuid(request.objectId));
			HashCombine(hash, request.pageId);
			HashCombine(hash, request.fallbackPageId);
			HashCombine(hash, request.clusterId);
			HashCombine(hash, request.desiredLodIndex);
			HashCombine(hash, static_cast<std::uint64_t>(request.priority));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(request.screenImportance * 1000.0f)));
			HashCombine(hash, request.fallbackRequired ? 1u : 0u);
			HashCombine(hash, request.fallbackRequest ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(request.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(request.expectedEmission));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(request.reason)));
		}

		void HashVirtualGeometryResidentPageSetRecord(std::uint64_t& hash, const VirtualGeometryResidentPageSetRecord& set)
		{
			HashCombine(hash, HashUuid(set.meshId));
			HashCombine(hash, set.residentBytes);
			HashCombine(hash, set.pinnedFallbackBytes);
			HashCombine(hash, set.residentPageCount);
			HashCombine(hash, set.pinnedFallbackPageCount);
			for (const std::uint32_t pageId : set.residentPageIds)
			{
				HashCombine(hash, pageId);
			}
			for (const std::uint32_t pageId : set.fallbackPageIds)
			{
				HashCombine(hash, pageId);
			}
		}

		void HashVirtualGeometryPagePayloadRecord(std::uint64_t& hash, const VirtualGeometryPagePayloadRecord& payload)
		{
			HashCombine(hash, HashUuid(payload.meshId));
			HashCombine(hash, payload.pageId);
			HashCombine(hash, payload.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(payload.source));
			HashCombine(hash, payload.selectedLod);
			HashCombine(hash, payload.clusterOffset);
			HashCombine(hash, payload.clusterCount);
			HashCombine(hash, payload.meshletOffset);
			HashCombine(hash, payload.meshletCount);
			HashCombine(hash, payload.vertexOffset);
			HashCombine(hash, payload.vertexCount);
			HashCombine(hash, payload.primitiveOffset);
			HashCombine(hash, payload.primitiveCount);
			HashCombine(hash, payload.materialSectionId);
			HashCombine(hash, payload.payloadByteOffset);
			HashCombine(hash, payload.payloadByteSize);
			HashCombine(hash, payload.payloadBufferIndex);
			HashCombine(hash, payload.payloadDescriptorIndex);
			HashCombine(hash, payload.allocationId);
			HashCombine(hash, payload.resident ? 1u : 0u);
			HashCombine(hash, payload.pinnedFallback ? 1u : 0u);
			HashCombine(hash, payload.valid ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(payload.owningMeshFallbackReason)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(payload.diagnostic)));
		}

		void HashVirtualGeometryEmittedPageSourceRecord(std::uint64_t& hash, const VirtualGeometryEmittedPageSourceRecord& emitted)
		{
			HashCombine(hash, emitted.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(emitted.viewType));
			HashCombine(hash, emitted.frameId);
			HashCombine(hash, HashUuid(emitted.objectId));
			HashCombine(hash, HashUuid(emitted.meshId));
			HashCombine(hash, HashUuid(emitted.materialId));
			HashCombine(hash, emitted.clusterId);
			HashCombine(hash, emitted.selectedLod);
			HashCombine(hash, emitted.pageId);
			HashCombine(hash, emitted.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(emitted.payloadSource));
			HashCombine(hash, static_cast<std::uint64_t>(emitted.emittedSource));
			HashCombine(hash, emitted.meshletOffset);
			HashCombine(hash, emitted.meshletCount);
			HashCombine(hash, emitted.payloadByteOffset);
			HashCombine(hash, emitted.payloadByteSize);
			HashCombine(hash, emitted.payloadBufferIndex);
			HashCombine(hash, emitted.payloadDescriptorIndex);
			HashCombine(hash, emitted.fallbackSubstitution ? 1u : 0u);
			HashCombine(hash, emitted.conservativeEmbeddedFallback ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(emitted.reason)));
		}

		void HashVirtualGeometryPageRetirementRecord(std::uint64_t& hash, const VirtualGeometryPageRetirementRecord& retirement)
		{
			HashCombine(hash, HashUuid(retirement.meshId));
			HashCombine(hash, retirement.pageId);
			HashCombine(hash, retirement.frameId);
			HashCombine(hash, retirement.byteSize);
			HashCombine(hash, retirement.protectedFallback ? 1u : 0u);
			HashCombine(hash, retirement.retired ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(retirement.reason)));
		}

		void HashFallbackCoverageChain(std::uint64_t& hash, const FallbackCoverageChain& chain)
		{
			HashCombine(hash, HashUuid(chain.meshId));
			HashCombine(hash, chain.desiredClusterId);
			HashCombine(hash, chain.fallbackClusterId);
			HashCombine(hash, chain.fallbackPageId);
			HashCombine(hash, static_cast<std::uint64_t>(chain.coverageStatus));
			HashCombine(hash, chain.substitutedMeshlets);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(chain.reason)));
		}

		void HashHiZRegressionSnapshot(std::uint64_t& hash, const HiZRegressionSnapshot& snapshot)
		{
			HashCombine(hash, snapshot.viewId);
			HashCombine(hash, static_cast<std::uint64_t>(snapshot.viewType));
			HashCombine(hash, snapshot.frameIndex);
			HashCombine(hash, static_cast<std::uint64_t>(snapshot.state));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(snapshot.reason)));
			HashCombine(hash, snapshot.testedMeshlets);
			HashCombine(hash, snapshot.occludedMeshlets);
			HashCombine(hash, snapshot.visibleMeshlets);
			HashCombine(hash, snapshot.failOpenMeshlets);
			HashCombine(hash, snapshot.invalidDepthSamples);
			HashCombine(hash, snapshot.recoveryFrames);
			HashCombine(hash, snapshot.baselineMatched ? 1u : 0u);
		}

		void HashConeSafetyMetadata(std::uint64_t& hash, const ConeSafetyMetadata& cone)
		{
			HashCombine(hash, HashUuid(cone.meshId));
			HashCombine(hash, cone.clusterId);
			HashCombine(hash, static_cast<std::uint64_t>(cone.materialSidedness));
			HashCombine(hash, static_cast<std::uint64_t>(cone.windingState));
			HashCombine(hash, static_cast<std::uint64_t>(cone.alphaRisk));
			HashCombine(hash, static_cast<std::uint64_t>(cone.thinShellRisk));
			HashCombine(hash, cone.coneValid ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(cone.disabledReason)));
		}

		void HashShadowVisibilitySelection(std::uint64_t& hash, const ShadowVisibilitySelection& selection)
		{
			HashCombine(hash, selection.shadowViewId);
			HashCombine(hash, HashUuid(selection.lightId));
			HashCombine(hash, selection.frameIndex);
			HashCombine(hash, selection.candidatePackets);
			HashCombine(hash, selection.selectedPackets);
			HashCombine(hash, selection.rejectedByLightFrustum);
			HashCombine(hash, selection.selectedMeshlets);
			HashCombine(hash, selection.dispatchedMeshlets);
			HashCombine(hash, selection.reusedCameraHiZ ? 1u : 0u);
			for (const std::string& reason : selection.fallbackReasons)
			{
				HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(reason)));
			}
		}

		void HashPerformanceGateResult(std::uint64_t& hash, const PerformanceGateResult& gate)
		{
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(gate.gateId)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(gate.fixtureId)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(TerraEngine::ToHexString(gate.assetId))));
			HashCombine(hash, gate.pageId);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(gate.metricName)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(gate.threshold * 1000.0f)));
			HashCombine(hash, static_cast<std::uint32_t>(std::round(gate.actual * 1000.0f)));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(gate.unit)));
			HashCombine(hash, static_cast<std::uint64_t>(gate.status));
			HashCombine(hash, static_cast<std::uint64_t>(gate.source));
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(gate.diagnostic)));
		}

		template<typename State>
		void SortVirtualGeometryScalingRecords(State& state)
		{
			// These records can be produced by worker jobs in arbitrary order.
			// Sorting by stable ids keeps snapshot hashes, diagnostics, and tests
			// deterministic without forcing workers to serialize their output.
			std::sort(state.clusterPackets.begin(), state.clusterPackets.end(), [](const VirtualGeometryClusterPacket& left, const VirtualGeometryClusterPacket& right) {
				if (left.packetId != right.packetId)
				{
					return left.packetId < right.packetId;
				}
				return left.clusterId < right.clusterId;
			});
			std::sort(state.clusterBatches.begin(), state.clusterBatches.end(), [](const ClusterWorkBatch& left, const ClusterWorkBatch& right) {
				if (left.batchId != right.batchId)
				{
					return left.batchId < right.batchId;
				}
				return left.packetOffset < right.packetOffset;
			});
			std::sort(state.compactionResults.begin(), state.compactionResults.end(), [](const CompactedVisibilitySet& left, const CompactedVisibilitySet& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
			});
			std::sort(state.gpuPacketRecords.begin(), state.gpuPacketRecords.end(), [](const GpuVirtualGeometryPacketRecord& left, const GpuVirtualGeometryPacketRecord& right) {
				if (left.packetId != right.packetId)
				{
					return left.packetId < right.packetId;
				}
				return left.clusterId < right.clusterId;
			});
			std::sort(state.gpuBatchDescriptors.begin(), state.gpuBatchDescriptors.end(), [](const GpuVirtualGeometryBatchDescriptor& left, const GpuVirtualGeometryBatchDescriptor& right) {
				if (left.batchId != right.batchId)
				{
					return left.batchId < right.batchId;
				}
				return left.packetOffset < right.packetOffset;
			});
			std::sort(state.gpuPacketFrameSets.begin(), state.gpuPacketFrameSets.end(), [](const GpuVirtualGeometryPacketFrameSet& left, const GpuVirtualGeometryPacketFrameSet& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				return left.frameId < right.frameId;
			});
			std::sort(state.gpuCompactionDispatches.begin(), state.gpuCompactionDispatches.end(), [](const GpuPacketCompactionDispatch& left, const GpuPacketCompactionDispatch& right) {
				if (left.dispatchId != right.dispatchId)
				{
					return left.dispatchId < right.dispatchId;
				}
				return left.frameId < right.frameId;
			});
			std::sort(state.generatedSubmissions.begin(), state.generatedSubmissions.end(), [](const GeneratedVirtualGeometrySubmissionRecord& left, const GeneratedVirtualGeometrySubmissionRecord& right) {
				if (left.recordId != right.recordId)
				{
					return left.recordId < right.recordId;
				}
				return left.batchId < right.batchId;
			});
			std::sort(state.gpuCounterReadbacks.begin(), state.gpuCounterReadbacks.end(), [](const VirtualGeometryCounterReadbackRecord& left, const VirtualGeometryCounterReadbackRecord& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				return left.frameId < right.frameId;
			});
			std::sort(state.gpuFallbackRecords.begin(), state.gpuFallbackRecords.end(), [](const VirtualGeometryGpuFallbackRecord& left, const VirtualGeometryGpuFallbackRecord& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				return left.reason < right.reason;
			});
			std::sort(state.pageRequests.begin(), state.pageRequests.end(), [](const VirtualGeometryPageRequestRecord& left, const VirtualGeometryPageRequestRecord& right) {
				if (left.priority != right.priority)
				{
					return left.priority > right.priority;
				}
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.residentPageSets.begin(), state.residentPageSets.end(), [](const VirtualGeometryResidentPageSetRecord& left, const VirtualGeometryResidentPageSetRecord& right) {
				return left.meshId < right.meshId;
			});
			for (VirtualGeometryResidentPageSetRecord& set : state.residentPageSets)
			{
				std::sort(set.residentPageIds.begin(), set.residentPageIds.end());
				std::sort(set.fallbackPageIds.begin(), set.fallbackPageIds.end());
			}
			std::sort(state.residentPagePayloads.begin(), state.residentPagePayloads.end(), [](const VirtualGeometryPagePayloadRecord& left, const VirtualGeometryPagePayloadRecord& right) {
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.emittedPageSources.begin(), state.emittedPageSources.end(), [](const VirtualGeometryEmittedPageSourceRecord& left, const VirtualGeometryEmittedPageSourceRecord& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				if (left.clusterId != right.clusterId)
				{
					return left.clusterId < right.clusterId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.pageRetirements.begin(), state.pageRetirements.end(), [](const VirtualGeometryPageRetirementRecord& left, const VirtualGeometryPageRetirementRecord& right) {
				if (left.frameId != right.frameId)
				{
					return left.frameId < right.frameId;
				}
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.pageResidency.begin(), state.pageResidency.end(), [](const PageResidencyRecord& left, const PageResidencyRecord& right) {
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.fallbackCoverage.begin(), state.fallbackCoverage.end(), [](const FallbackCoverageChain& left, const FallbackCoverageChain& right) {
				if (left.desiredClusterId != right.desiredClusterId)
				{
					return left.desiredClusterId < right.desiredClusterId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.hiZRegressionSnapshots.begin(), state.hiZRegressionSnapshots.end(), [](const HiZRegressionSnapshot& left, const HiZRegressionSnapshot& right) {
				if (left.viewId != right.viewId)
				{
					return left.viewId < right.viewId;
				}
				return left.frameIndex < right.frameIndex;
			});
			std::sort(state.coneSafetyMetadata.begin(), state.coneSafetyMetadata.end(), [](const ConeSafetyMetadata& left, const ConeSafetyMetadata& right) {
				if (left.clusterId != right.clusterId)
				{
					return left.clusterId < right.clusterId;
				}
				return left.meshId < right.meshId;
			});
			std::sort(state.shadowVisibilitySelections.begin(), state.shadowVisibilitySelections.end(), [](const ShadowVisibilitySelection& left, const ShadowVisibilitySelection& right) {
				if (left.shadowViewId != right.shadowViewId)
				{
					return left.shadowViewId < right.shadowViewId;
				}
				return left.lightId < right.lightId;
			});
			std::sort(state.performanceGates.begin(), state.performanceGates.end(), [](const PerformanceGateResult& left, const PerformanceGateResult& right) {
				if (left.gateId != right.gateId)
				{
					return left.gateId < right.gateId;
				}
				return left.fixtureId < right.fixtureId;
			});
		}

		template<typename State>
		void HashVirtualGeometryScalingRecords(std::uint64_t& hash, const State& state)
		{
			for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
			{
				HashVirtualGeometryClusterPacket(hash, packet);
			}
			for (const ClusterWorkBatch& batch : state.clusterBatches)
			{
				HashClusterWorkBatch(hash, batch);
			}
			for (const CompactedVisibilitySet& compaction : state.compactionResults)
			{
				HashCompactedVisibilitySet(hash, compaction);
			}
			for (const GpuVirtualGeometryPacketRecord& packet : state.gpuPacketRecords)
			{
				HashGpuVirtualGeometryPacketRecord(hash, packet);
			}
			for (const GpuVirtualGeometryBatchDescriptor& batch : state.gpuBatchDescriptors)
			{
				HashGpuVirtualGeometryBatchDescriptor(hash, batch);
			}
			for (const GpuVirtualGeometryPacketFrameSet& frameSet : state.gpuPacketFrameSets)
			{
				HashGpuVirtualGeometryPacketFrameSet(hash, frameSet);
			}
			for (const GpuPacketCompactionDispatch& dispatch : state.gpuCompactionDispatches)
			{
				HashGpuPacketCompactionDispatch(hash, dispatch);
			}
			for (const GeneratedVirtualGeometrySubmissionRecord& submission : state.generatedSubmissions)
			{
				HashGeneratedVirtualGeometrySubmissionRecord(hash, submission);
			}
			for (const VirtualGeometryCounterReadbackRecord& readback : state.gpuCounterReadbacks)
			{
				HashVirtualGeometryCounterReadbackRecord(hash, readback);
			}
			for (const VirtualGeometryGpuFallbackRecord& fallback : state.gpuFallbackRecords)
			{
				HashVirtualGeometryGpuFallbackRecord(hash, fallback);
			}
			for (const VirtualGeometryPageRequestRecord& request : state.pageRequests)
			{
				HashVirtualGeometryPageRequestRecord(hash, request);
			}
			for (const VirtualGeometryResidentPageSetRecord& set : state.residentPageSets)
			{
				HashVirtualGeometryResidentPageSetRecord(hash, set);
			}
			for (const VirtualGeometryPagePayloadRecord& payload : state.residentPagePayloads)
			{
				HashVirtualGeometryPagePayloadRecord(hash, payload);
			}
			for (const VirtualGeometryEmittedPageSourceRecord& emitted : state.emittedPageSources)
			{
				HashVirtualGeometryEmittedPageSourceRecord(hash, emitted);
			}
			for (const VirtualGeometryPageRetirementRecord& retirement : state.pageRetirements)
			{
				HashVirtualGeometryPageRetirementRecord(hash, retirement);
			}
			for (const PageResidencyRecord& page : state.pageResidency)
			{
				HashPageResidencyRecord(hash, page);
			}
			for (const FallbackCoverageChain& chain : state.fallbackCoverage)
			{
				HashFallbackCoverageChain(hash, chain);
			}
			for (const HiZRegressionSnapshot& hiZ : state.hiZRegressionSnapshots)
			{
				HashHiZRegressionSnapshot(hash, hiZ);
			}
			for (const ConeSafetyMetadata& cone : state.coneSafetyMetadata)
			{
				HashConeSafetyMetadata(hash, cone);
			}
			for (const ShadowVisibilitySelection& shadow : state.shadowVisibilitySelections)
			{
				HashShadowVisibilitySelection(hash, shadow);
			}
			for (const PerformanceGateResult& gate : state.performanceGates)
			{
				HashPerformanceGateResult(hash, gate);
			}
		}

		std::uint64_t BuildVirtualGeometryCameraSignature(const RenderFrameSnapshot& snapshot, const VirtualGeometryViewSelectionState& state)
		{
			// The foundation stage has no live camera object in this value layer.
			// Hashing immutable view identity gives diagnostics a stable
			// placeholder until backend-specific camera signatures are published.
			std::uint64_t signature = snapshot.frameId;
			HashCombine(signature, snapshot.sceneRevision);
			HashCombine(signature, snapshot.resourceRevision);
			HashCombine(signature, state.outputId);
			HashCombine(signature, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(signature, static_cast<std::uint64_t>(state.viewType));
			HashCombine(signature, state.targetSize.x);
			HashCombine(signature, state.targetSize.y);
			HashCombine(signature, state.viewGeneration);
			return signature;
		}
	}

	void SortSnapshotDeterministically(RenderFrameSnapshot& snapshot)
	{
		// Stable sort points make repeated frames produce identical work packet
		// ids and pass ordering even when scene registry iteration order changes.
		SortIfNeeded(snapshot.viewRequests, [](const RenderFrameViewRequest& left, const RenderFrameViewRequest& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		SortByUuid(snapshot.renderables, &RenderFrameRenderableRecord::objectId);
		SortByUuid(snapshot.lights, &RenderFrameLightRecord::lightId);
		SortByUuid(snapshot.materials, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.meshes, &RenderFrameResourceRecord::resourceId);
		SortByUuid(snapshot.textures, &RenderFrameResourceRecord::resourceId);
		SortIfNeeded(snapshot.geometryRouteDecisions,
			[](const GeometryRouteDecision& left, const GeometryRouteDecision& right) {
				return left.decisionIdentity < right.decisionIdentity;
			});
		SortIfNeeded(snapshot.directMeshletBatches,
			[](const DirectMeshletBatch& left, const DirectMeshletBatch& right) {
				return left.geometryIdentity < right.geometryIdentity;
			});
		SortIfNeeded(snapshot.animatedGeometryPageDemands,
			[](const DeformationPageDemand& left, const DeformationPageDemand& right)
			{
				if(left.key != right.key)
				{
					return left.key < right.key;
				}
				return left.requestedViewMask < right.requestedViewMask;
			});
		SortIfNeeded(snapshot.virtualGeometryViewSelections, [](const VirtualGeometryViewSelectionState& left, const VirtualGeometryViewSelectionState& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		for (VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			SortIfNeeded(state.draws, [](const VirtualGeometryViewDrawRecord& left, const VirtualGeometryViewDrawRecord& right) {
				return left.objectId < right.objectId;
			});
			SortIfNeeded(state.selectedClusters, [](const VirtualGeometrySelectedClusterRecord& left, const VirtualGeometrySelectedClusterRecord& right) {
				if (left.clusterId != right.clusterId)
				{
					return left.clusterId < right.clusterId;
				}
				return left.objectId < right.objectId;
			});
			SortVirtualGeometryScalingRecords(state);
			SortIfNeeded(state.pageNeeds, [](const VirtualGeometryPageNeedRecord& left, const VirtualGeometryPageNeedRecord& right) {
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.objectId < right.objectId;
			});
			SortIfNeeded(state.lodTransitionStates, [](const VirtualGeometryLODTransitionState& left, const VirtualGeometryLODTransitionState& right) {
				if (left.outputId != right.outputId)
				{
					return left.outputId < right.outputId;
				}
				return left.objectId < right.objectId;
			});
		}
		SortIfNeeded(snapshot.virtualGeometryVisibilitySnapshots, [](const VirtualGeometryViewVisibilitySnapshot& left, const VirtualGeometryViewVisibilitySnapshot& right) {
			if (left.outputId != right.outputId)
			{
				return left.outputId < right.outputId;
			}
			return static_cast<std::uint8_t>(left.viewType) < static_cast<std::uint8_t>(right.viewType);
		});
		for (VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			SortIfNeeded(state.drawRanges, [](const VirtualGeometryViewDrawRecord& left, const VirtualGeometryViewDrawRecord& right) {
				return left.objectId < right.objectId;
			});
			SortIfNeeded(state.selectedClusters, [](const VirtualGeometrySelectedClusterRecord& left, const VirtualGeometrySelectedClusterRecord& right) {
				if (left.clusterId != right.clusterId)
				{
					return left.clusterId < right.clusterId;
				}
				return left.objectId < right.objectId;
			});
			SortVirtualGeometryScalingRecords(state);
			SortIfNeeded(state.pageNeeds, [](const VirtualGeometryPageNeedRecord& left, const VirtualGeometryPageNeedRecord& right) {
				if (left.pageId != right.pageId)
				{
					return left.pageId < right.pageId;
				}
				return left.objectId < right.objectId;
			});
			SortIfNeeded(state.lodTransitionStates, [](const VirtualGeometryLODTransitionState& left, const VirtualGeometryLODTransitionState& right) {
				if (left.outputId != right.outputId)
				{
					return left.outputId < right.outputId;
				}
				return left.objectId < right.objectId;
			});
		}
	}

	bool SnapshotUsesOnlyResolvedResources(const RenderFrameSnapshot& snapshot)
	{
		auto allResourcesResolved = [](const std::vector<RenderFrameResourceRecord>& records) {
			return std::all_of(records.begin(), records.end(), [](const RenderFrameResourceRecord& record) {
				return record.resourceId != TerraEngine::EMPTY_UUID && (record.resident || record.usingFallback);
			});
		};

		const bool animationGenerationsResolved = std::all_of(
			snapshot.renderables.begin(), snapshot.renderables.end(),
			[](const RenderFrameRenderableRecord& renderable)
			{
				return ValidateAnimationRenderGenerationRecord(renderable.animation);
			});
		return snapshot.valid
			&& !SnapshotContainsLiveMutableState(snapshot)
			&& animationGenerationsResolved
			&& allResourcesResolved(snapshot.materials)
			&& allResourcesResolved(snapshot.meshes)
			&& allResourcesResolved(snapshot.textures);
	}

	bool SnapshotContainsLiveMutableState(const RenderFrameSnapshot& snapshot)
	{
		return snapshot.containsLiveMutableState;
	}

	bool ValidateAnimationRenderGenerationRecord(const AnimationRenderGenerationRecord& record)
	{
		if (record.source == AnimationRenderEvaluationSource::None)
		{
			return record.stableInstanceId == 0U
				&& record.poseGeneration == 0U
				&& record.morphGeneration == 0U
				&& record.paletteGeneration == 0U
				&& record.deformationContentIdLow == 0U
				&& record.deformationContentIdHigh == 0U
				&& record.gpuWorkPacketCount == 0U
				&& !record.complete
				&& !record.safeFallback;
		}

		if (record.stableInstanceId == 0U
			|| record.deformablePayloadGeneration == 0U
			|| record.rendererGeneration == 0U)
		{
			return false;
		}
		if((record.deformationContentIdLow == 0U)
			!= (record.deformationContentIdHigh == 0U))
		{
			return false;
		}
		if (record.source == AnimationRenderEvaluationSource::SafeFallback)
		{
			return record.safeFallback && !record.complete
				&& record.gpuWorkPacketCount == 0U;
		}
		if (record.programGeneration == 0U
			|| !record.complete || record.safeFallback
			|| record.poseGeneration == 0U
			|| record.morphGeneration == 0U
			|| record.paletteGeneration == 0U)
		{
			return false;
		}
		if (record.source == AnimationRenderEvaluationSource::CpuPose)
		{
			return record.gpuWorkPacketCount == 0U && record.gpuStageMask == 0U;
		}
		return record.animationPlanGeneration != 0U
			&& record.gpuWorkPacketCount != 0U
			&& record.gpuStageMask != 0U;
	}

	void AnimatedObjectVisibilitySnapshot::BuildVisibleObjectLookup() noexcept
	{
		visibleObjectLookupIndices.clear();
		visibleObjectLookupMask = 0U;
		if(visibleObjectIds.empty()
			|| visibleObjectIds.size() >= std::numeric_limits<std::uint32_t>::max()) {
			return;
		}
		try {
			std::size_t capacity = 1U;
			const std::size_t required = visibleObjectIds.size() * 2U;
			while(capacity < required) {
				if(capacity > (std::numeric_limits<std::uint32_t>::max() >> 1U)) {
					return;
				}
				capacity <<= 1U;
			}
			const std::uint32_t invalidIndex =
				std::numeric_limits<std::uint32_t>::max();
			std::vector<std::uint32_t> lookup(capacity, invalidIndex);
			const std::uint32_t mask = static_cast<std::uint32_t>(capacity - 1U);
			for(std::uint32_t index = 0U;
				index < static_cast<std::uint32_t>(visibleObjectIds.size()); ++index) {
				std::uint32_t slot = static_cast<std::uint32_t>(
					HashUuid(visibleObjectIds[index])) & mask;
				for(std::size_t probe = 0U; probe < capacity; ++probe) {
					const std::uint32_t existing = lookup[slot];
					if(existing == invalidIndex) {
						lookup[slot] = index;
						break;
					}
					if(existing < visibleObjectIds.size()
						&& visibleObjectIds[existing] == visibleObjectIds[index]) {
						break;
					}
					slot = (slot + 1U) & mask;
				}
			}
			visibleObjectLookupIndices = std::move(lookup);
			visibleObjectLookupMask = mask;
		} catch(const std::bad_alloc&) {
			// The canonical sorted vector remains a complete exact fallback. Memory
			// pressure therefore affects only lookup speed, never culling correctness.
			visibleObjectLookupIndices.clear();
			visibleObjectLookupMask = 0U;
		}
	}

	bool AnimatedObjectVisibilitySnapshot::Contains(
		const TerraEngine::UUID& objectId) const noexcept
	{
		const std::uint32_t invalidIndex = std::numeric_limits<std::uint32_t>::max();
		if(!visibleObjectLookupIndices.empty()
			&& visibleObjectLookupIndices.size()
				== static_cast<std::size_t>(visibleObjectLookupMask) + 1U) {
			std::uint32_t slot = static_cast<std::uint32_t>(HashUuid(objectId))
				& visibleObjectLookupMask;
			for(std::size_t probe = 0U; probe < visibleObjectLookupIndices.size(); ++probe) {
				const std::uint32_t index = visibleObjectLookupIndices[slot];
				if(index == invalidIndex) return false;
				if(index < visibleObjectIds.size()
					&& visibleObjectIds[index] == objectId) {
					return true;
				}
				slot = (slot + 1U) & visibleObjectLookupMask;
			}
			return false;
		}
		return std::binary_search(
			visibleObjectIds.begin(), visibleObjectIds.end(), objectId);
	}

	void PublishAnimatedObjectVisibilitySnapshot(
		AnimatedObjectVisibilitySnapshot snapshot)
	{
		if(!snapshot.canonicalUnique)
		{
			// External/test producers may append the same object from several views.
			// Normalize those snapshots here; the live renderer avoids this O(N log N)
			// work by publishing from its canonical renderable-index visibility mask.
			std::sort(snapshot.visibleObjectIds.begin(), snapshot.visibleObjectIds.end());
			snapshot.visibleObjectIds.erase(
				std::unique(snapshot.visibleObjectIds.begin(), snapshot.visibleObjectIds.end()),
				snapshot.visibleObjectIds.end());
			snapshot.canonicalUnique = true;
		}
		snapshot.BuildVisibleObjectLookup();
		auto immutable = std::make_shared<const AnimatedObjectVisibilitySnapshot>(
			std::move(snapshot));
		std::atomic_store_explicit(
			&gAnimatedObjectVisibilitySnapshot, std::move(immutable),
			std::memory_order_release);
	}

	std::shared_ptr<const AnimatedObjectVisibilitySnapshot>
	LoadAnimatedObjectVisibilitySnapshot() noexcept
	{
		return std::atomic_load_explicit(
			&gAnimatedObjectVisibilitySnapshot, std::memory_order_acquire);
	}

	void ClearAnimatedObjectVisibilitySnapshot() noexcept
	{
		std::atomic_store_explicit(
			&gAnimatedObjectVisibilitySnapshot,
			std::shared_ptr<const AnimatedObjectVisibilitySnapshot>{},
			std::memory_order_release);
	}

	bool CanScheduleGpuAnimatedPageDeformation(
		const AnimationRenderGenerationRecord& record)
	{
		// CPU palettes live in a different frame resource from GPU palette output.
		// Treating packet zero as a CPU sentinel is unsafe whenever another
		// animator owns real GPU packet zero, because the page would skin with the
		// other character's matrices. Owner deformation is already the complete,
		// correct fallback for these CPU-authoritative renderables.
		return ValidateAnimationRenderGenerationRecord(record)
			&& record.complete
			&& !record.safeFallback
			&& record.gpuWorkPacketCount != 0U;
	}

	std::uint64_t BuildAnimationRenderGenerationHash(const AnimationRenderGenerationRecord& record)
	{
		std::uint64_t hash = 1469598103934665603ULL;
		HashCombine(hash, record.stableInstanceId);
		HashCombine(hash, record.poseGeneration);
		HashCombine(hash, record.morphGeneration);
		HashCombine(hash, record.paletteGeneration);
		HashCombine(hash, record.deformablePayloadGeneration);
		HashCombine(hash, record.programGeneration);
		HashCombine(hash, record.animationPlanGeneration);
		HashCombine(hash, record.rendererGeneration);
		HashCombine(hash, record.deformationContentIdLow);
		HashCombine(hash, record.deformationContentIdHigh);
		HashCombine(hash, record.firstGpuWorkPacket);
		HashCombine(hash, record.gpuWorkPacketCount);
		HashCombine(hash, record.gpuStageMask);
		HashCombine(hash, static_cast<std::uint64_t>(record.source));
		HashCombine(hash, record.complete ? 1U : 0U);
		HashCombine(hash, record.safeFallback ? 1U : 0U);
		return hash;
	}

	std::uint64_t BuildRenderInvalidationGenerationHash(const RenderInvalidationGenerationRecord& record)
	{
		std::uint64_t hash = 1469598103934665603ULL;
		HashCombine(hash, record.pose);
		HashCombine(hash, record.morph);
		HashCombine(hash, record.asset);
		HashCombine(hash, record.page);
		HashCombine(hash, record.view);
		HashCombine(hash, record.material);
		HashCombine(hash, record.renderer);
		HashCombine(hash, record.residency);
		return hash;
	}

	std::uint64_t BuildSnapshotDeterministicHash(const RenderFrameSnapshot& snapshot)
	{
		std::uint64_t hash = snapshot.frameId;
		HashCombine(hash, snapshot.sceneRevision);
		HashCombine(hash, snapshot.resourceRevision);
		HashCombine(hash, snapshot.valid ? 1u : 0u);
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			HashCombine(hash, view.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(view.viewType));
			HashCombine(hash, view.targetSize.x);
			HashCombine(hash, view.targetSize.y);
			HashCombine(hash, view.viewGeneration);
		}
		for (const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			HashCombine(hash, HashUuid(renderable.objectId));
			HashCombine(hash, HashUuid(renderable.meshId));
			HashCombine(hash, HashUuid(renderable.materialId));
			HashVirtualGeometryRange(hash, renderable.virtualGeometry);
			HashCombine(hash, renderable.geometryRouteDecision.decisionIdentity);
			HashCombine(hash, static_cast<std::uint64_t>(renderable.geometryRouteDecision.selected));
			HashCombine(hash, renderable.geometryRouteDecision.routeGeneration);
			HashCombine(hash, renderable.taskShaderCullingEnabled ? 1u : 0u);
			HashCombine(hash, renderable.usingFallback ? 1u : 0u);
			HashCombine(hash, BuildAnimationRenderGenerationHash(renderable.animation));
			HashCombine(hash, BuildRenderInvalidationGenerationHash(renderable.invalidation));
		}
		for(const GeometryRouteDecision& decision : snapshot.geometryRouteDecisions)
		{
			HashCombine(hash, decision.decisionIdentity);
			HashCombine(hash, static_cast<std::uint64_t>(decision.selected));
			HashCombine(hash, decision.policyIdentity);
			HashCombine(hash, decision.calibrationIdentity);
			HashCombine(hash, decision.routeGeneration);
		}
		for(const DirectMeshletBatch& batch : snapshot.directMeshletBatches)
		{
			HashCombine(hash, batch.geometryIdentity);
			HashCombine(hash, batch.geometryGeneration);
			HashCombine(hash, batch.materialIdentity);
			HashCombine(hash, batch.firstMeshlet);
			HashCombine(hash, batch.meshletCount);
			for(const std::uint64_t instance : batch.instanceIdentities) HashCombine(hash, instance);
		}
		for(const DeformationPageDemand& demand : snapshot.animatedGeometryPageDemands)
		{
			HashCombine(hash, HashUuid(demand.key.page.meshId));
			HashCombine(hash, demand.key.page.meshPayloadGeneration);
			HashCombine(hash, demand.key.page.pageId);
			HashCombine(hash, demand.key.page.pageContentGeneration);
			HashCombine(hash, demand.key.page.residencyGeneration);
			HashCombine(hash, demand.key.deformation.stableInstanceId);
			HashCombine(hash, demand.key.deformation.animationPublicationGeneration);
			HashCombine(hash, demand.key.deformation.rendererGeneration);
			HashCombine(hash, demand.key.deformation.sceneGeneration);
			HashCombine(hash, demand.key.deformation.contentIdLow);
			HashCombine(hash, demand.key.deformation.contentIdHigh);
			HashCombine(hash, demand.ownerStableInstanceId);
			HashCombine(hash, demand.requestedViewMask);
			HashCombine(hash, demand.vertexCount);
			HashCombine(hash, demand.meshletCount);
			HashCombine(hash, demand.clusterCount);
		}
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			HashCombine(hash, state.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.targetSize.x);
			HashCombine(hash, state.targetSize.y);
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, state.shadowPass ? 1u : 0u);
			HashCombine(hash, state.exportRuntime ? 1u : 0u);
			HashCombine(hash, state.hiZOcclusionAllowed ? 1u : 0u);
			HashCombine(hash, state.hiZSelfOcclusionGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZNearPlaneGuardPassed ? 1u : 0u);
			HashCombine(hash, state.hiZThinGeometryGuardPassed ? 1u : 0u);
			HashCombine(hash, static_cast<std::uint64_t>(std::hash<std::string>{}(state.hiZInactiveReason)));
			HashCombine(hash, state.hasHiZValidationState ? 1u : 0u);
			if (state.hasHiZValidationState)
			{
				HashVirtualGeometryHiZState(hash, state.hiZValidationState);
			}
			for (const VirtualGeometryViewDrawRecord& draw : state.draws)
			{
				HashCombine(hash, HashUuid(draw.objectId));
				HashCombine(hash, HashUuid(draw.meshId));
				HashVirtualGeometryRange(hash, draw.range);
				HashCombine(hash, draw.taskShaderCullingEnabled ? 1u : 0u);
			}
			for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
			{
				HashCombine(hash, HashUuid(cluster.objectId));
				HashCombine(hash, HashUuid(cluster.meshId));
				HashCombine(hash, cluster.clusterId);
				HashCombine(hash, cluster.lodIndex);
				HashCombine(hash, cluster.pageId);
				HashCombine(hash, cluster.fallbackClusterId);
				HashCombine(hash, cluster.usingFallback ? 1u : 0u);
			}
			HashVirtualGeometryScalingRecords(hash, state);
			for (const VirtualGeometryPageNeedRecord& need : state.pageNeeds)
			{
				HashCombine(hash, HashUuid(need.objectId));
				HashCombine(hash, HashUuid(need.meshId));
				HashCombine(hash, need.pageId);
				HashCombine(hash, need.fallbackPageId);
				HashCombine(hash, need.clusterId);
				HashCombine(hash, need.desiredLodIndex);
				HashCombine(hash, static_cast<std::uint32_t>(std::round(need.screenImportance * 1000.0f)));
				HashCombine(hash, need.fallbackRequired ? 1u : 0u);
				HashCombine(hash, static_cast<std::uint64_t>(need.desiredPayloadSource));
				HashCombine(hash, static_cast<std::uint64_t>(need.fallbackPayloadSource));
				HashCombine(hash, need.packetId);
				HashCombine(hash, need.batchId);
				HashCombine(hash, need.packetOffset);
				HashCombine(hash, need.packetCount);
			}
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				HashVirtualGeometryLODTransitionState(hash, transitionState);
			}
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			HashCombine(hash, state.frameId);
			HashCombine(hash, state.outputId);
			HashCombine(hash, static_cast<std::uint64_t>(state.outputKind));
			HashCombine(hash, static_cast<std::uint64_t>(state.viewType));
			HashCombine(hash, state.renderSize.x);
			HashCombine(hash, state.renderSize.y);
			HashCombine(hash, state.cameraSignature);
			HashCombine(hash, static_cast<std::uint64_t>(state.debugVisualizationMode));
			HashCombine(hash, static_cast<std::uint64_t>(state.source));
			HashCombine(hash, state.valid ? 1u : 0u);
			HashVirtualGeometryCounters(hash, state.counters);
			HashVirtualGeometryHiZState(hash, state.hiZValidationState);
			HashVirtualGeometryScalingRecords(hash, state);
			for (const OcclusionDebugSample& sample : state.occlusionDebugSamples)
			{
				HashVirtualGeometryOcclusionSample(hash, sample);
			}
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				HashVirtualGeometryLODTransitionState(hash, transitionState);
			}
			for (const VirtualGeometryViewDrawRecord& draw : state.drawRanges)
			{
				HashCombine(hash, HashUuid(draw.objectId));
				HashCombine(hash, HashUuid(draw.meshId));
				HashVirtualGeometryRange(hash, draw.range);
				HashCombine(hash, draw.taskShaderCullingEnabled ? 1u : 0u);
			}
			for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
			{
				HashCombine(hash, HashUuid(cluster.objectId));
				HashCombine(hash, HashUuid(cluster.meshId));
				HashCombine(hash, cluster.clusterId);
				HashCombine(hash, cluster.lodIndex);
				HashCombine(hash, cluster.pageId);
				HashCombine(hash, cluster.fallbackClusterId);
				HashCombine(hash, cluster.usingFallback ? 1u : 0u);
			}
			for (const VirtualGeometryPageNeedRecord& need : state.pageNeeds)
			{
				HashCombine(hash, HashUuid(need.objectId));
				HashCombine(hash, HashUuid(need.meshId));
				HashCombine(hash, need.pageId);
				HashCombine(hash, need.fallbackPageId);
				HashCombine(hash, need.clusterId);
				HashCombine(hash, need.desiredLodIndex);
				HashCombine(hash, static_cast<std::uint32_t>(std::round(need.screenImportance * 1000.0f)));
				HashCombine(hash, need.fallbackRequired ? 1u : 0u);
				HashCombine(hash, static_cast<std::uint64_t>(need.desiredPayloadSource));
				HashCombine(hash, static_cast<std::uint64_t>(need.fallbackPayloadSource));
				HashCombine(hash, need.packetId);
				HashCombine(hash, need.batchId);
				HashCombine(hash, need.packetOffset);
				HashCombine(hash, need.packetCount);
			}
		}
		for (const RenderFrameLightRecord& light : snapshot.lights)
		{
			HashCombine(hash, HashUuid(light.lightId));
			HashCombine(hash, light.lightType);
			HashCombine(hash, light.castsShadows ? 1u : 0u);
		}
		return hash;
	}

	std::uint64_t EstimateSnapshotWorkload(const RenderFrameSnapshot& snapshot)
	{
		// The estimate is intentionally backend-neutral. It reflects CPU work
		// units the graph can split, not GPU timings or API-specific commands.
		std::uint64_t virtualGeometryClusterWork = 0u;
		for (const VirtualGeometryViewSelectionState& state : snapshot.virtualGeometryViewSelections)
		{
			virtualGeometryClusterWork += static_cast<std::uint64_t>(
				state.selectedClusters.size()
				+ state.clusterPackets.size()
				+ state.clusterBatches.size()
				+ state.gpuPacketRecords.size()
				+ state.gpuBatchDescriptors.size()
				+ state.gpuPacketFrameSets.size()
				+ state.gpuCompactionDispatches.size()
				+ state.generatedSubmissions.size()
				+ state.gpuCounterReadbacks.size()
				+ state.gpuFallbackRecords.size()
				+ state.compactionResults.size()
				+ state.pageNeeds.size()
				+ state.pageRequests.size()
				+ state.residentPageSets.size()
				+ state.residentPagePayloads.size()
				+ state.emittedPageSources.size()
				+ state.pageRetirements.size()
				+ state.pageResidency.size()
				+ state.fallbackCoverage.size()
				+ state.hiZRegressionSnapshots.size()
				+ state.coneSafetyMetadata.size()
				+ state.shadowVisibilitySelections.size()
				+ state.performanceGates.size()
				+ state.lodTransitionStates.size());
		}
		for (const VirtualGeometryViewVisibilitySnapshot& state : snapshot.virtualGeometryVisibilitySnapshots)
		{
			virtualGeometryClusterWork += static_cast<std::uint64_t>(
				state.drawRanges.size()
				+ state.selectedClusters.size()
				+ state.clusterPackets.size()
				+ state.clusterBatches.size()
				+ state.gpuPacketRecords.size()
				+ state.gpuBatchDescriptors.size()
				+ state.gpuPacketFrameSets.size()
				+ state.gpuCompactionDispatches.size()
				+ state.generatedSubmissions.size()
				+ state.gpuCounterReadbacks.size()
				+ state.gpuFallbackRecords.size()
				+ state.compactionResults.size()
				+ state.pageNeeds.size()
				+ state.pageRequests.size()
				+ state.residentPageSets.size()
				+ state.residentPagePayloads.size()
				+ state.emittedPageSources.size()
				+ state.pageRetirements.size()
				+ state.pageResidency.size()
				+ state.fallbackCoverage.size()
				+ state.hiZRegressionSnapshots.size()
				+ state.coneSafetyMetadata.size()
				+ state.shadowVisibilitySelections.size()
				+ state.performanceGates.size()
				+ state.lodTransitionStates.size());
		}
		return static_cast<std::uint64_t>(snapshot.renderables.size())
			+ static_cast<std::uint64_t>(snapshot.lights.size())
			+ static_cast<std::uint64_t>(snapshot.viewRequests.size() * 8u)
			+ static_cast<std::uint64_t>(snapshot.virtualGeometryViewSelections.size() * 4u)
			+ virtualGeometryClusterWork;
	}

	std::vector<DeformationPageDemand>
	SelectAnimatedGeometryPageDemands(const RenderFrameSnapshot& snapshot)
	{
		// Resolve object ids through copied renderable publications. View
		// selections deliberately do not retain animation pointers or live
		// component handles, so this join is deterministic and worker-safe.
		std::unordered_map<TerraEngine::UUID, std::uint64_t> stableInstances;
		stableInstances.reserve(snapshot.renderables.size());
		for(const RenderFrameRenderableRecord& renderable : snapshot.renderables)
		{
			if(renderable.objectId != TerraEngine::EMPTY_UUID
				&& renderable.animation.stableInstanceId != 0u)
			{
				stableInstances.insert_or_assign(
					renderable.objectId,
					renderable.animation.stableInstanceId);
			}
		}

		using SelectedPageKey = std::pair<std::uint64_t, std::uint32_t>;
		std::map<SelectedPageKey, std::uint64_t> selectedViewMasks;
		std::unordered_map<std::uint64_t, std::unordered_set<std::uint32_t>>
			demandPagesByOwner;
		demandPagesByOwner.reserve(
			snapshot.animatedGeometryPageDemands.size());
		for(const DeformationPageDemand& demand :
			snapshot.animatedGeometryPageDemands)
		{
			const std::uint64_t owner = demand.ownerStableInstanceId != 0u
				? demand.ownerStableInstanceId
				: demand.key.deformation.stableInstanceId;
			demandPagesByOwner[owner].insert(
				demand.key.page.pageId);
		}

		// A page-local GPU packet is the preferred visibility proof. Some
		// protected fallback routes deliberately submit the complete embedded
		// owner mesh instead, so they have no packet page that can be joined to
		// the independently validated animated-page directory. Preserve a
		// separate owner-wide view mask for that conservative route rather than
		// dropping every deformation demand for a visibly submitted animator.
		std::unordered_map<std::uint64_t, std::uint64_t>
			ownerFallbackViewMasks;
		ownerFallbackViewMasks.reserve(stableInstances.size());
		for(std::size_t viewIndex = 0u;
			viewIndex < snapshot.virtualGeometryViewSelections.size();
			++viewIndex)
		{
			// The ABI exposes a 64-bit view union. Additional diagnostic/capture
			// views share the final overflow bit instead of silently losing their
			// page demand.
			const std::uint64_t viewMask =
				std::uint64_t{1u} << std::min<std::size_t>(viewIndex, 63u);
			const VirtualGeometryViewSelectionState& view =
				snapshot.virtualGeometryViewSelections[viewIndex];
			std::unordered_set<std::uint64_t> visibleOwners;
			visibleOwners.reserve(view.draws.size());
			std::unordered_set<std::uint64_t> ownersWithMatchingPagePackets;
			ownersWithMatchingPagePackets.reserve(view.gpuPacketRecords.size());
			for(const VirtualGeometryViewDrawRecord& draw : view.draws)
			{
				const auto stableInstance =
					stableInstances.find(draw.objectId);
				if(stableInstance == stableInstances.end()
					|| draw.range.meshletCount == 0u
					|| (!draw.range.virtualGeometryActive
						&& !draw.range.failOpen))
				{
					continue;
				}
				visibleOwners.insert(stableInstance->second);
			}
			// Embedded and conservative payloads still carry the exact cluster cut
			// selected for this view. Join those clusters to demanded page ids before
			// considering the expensive whole-owner fallback; otherwise a low LOD
			// draw needlessly deforms every page contained by the source asset.
			for(const VirtualGeometrySelectedClusterRecord& cluster :
				view.selectedClusters)
			{
				const auto stableInstance =
					stableInstances.find(cluster.objectId);
				if(stableInstance == stableInstances.end())
				{
					continue;
				}
				const auto demandedPages =
					demandPagesByOwner.find(stableInstance->second);
				if(demandedPages == demandPagesByOwner.end()
					|| !demandedPages->second.contains(cluster.pageId))
				{
					continue;
				}
				selectedViewMasks[{
					stableInstance->second,
					cluster.pageId
				}] |= viewMask;
				ownersWithMatchingPagePackets.insert(
					stableInstance->second);
			}
			for(const GpuVirtualGeometryPacketRecord& packet :
				view.gpuPacketRecords)
			{
				const bool residentPage =
					packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentPagePayload
					|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::ResidentFallbackPayload;
				const auto stableInstance =
					stableInstances.find(packet.objectId);
				if(stableInstance == stableInstances.end())
				{
					continue;
				}
				const auto demandedPages =
					demandPagesByOwner.find(stableInstance->second);
				const bool exactEmbeddedPage = packet.payloadSource
						== VirtualGeometryPagePayloadSource::EmbeddedMeshPayload
					|| packet.payloadSource
						== VirtualGeometryPagePayloadSource::
							ConservativeEmbeddedFallback;
				if((!residentPage && !exactEmbeddedPage)
					|| demandedPages == demandPagesByOwner.end()
					|| !demandedPages->second.contains(packet.pageId))
				{
					continue;
				}
				// One coarse draw can expand into several independently resident
				// page packets. Deformation must follow the emitted packet pages,
				// otherwise only the draw's first page moves while every other
				// page remains in bind pose.
				selectedViewMasks[{
					stableInstance->second,
					packet.pageId
				}] |= viewMask;
				ownersWithMatchingPagePackets.insert(
					stableInstance->second);
			}
			for(const std::uint64_t stableInstanceId : visibleOwners)
			{
				if(!ownersWithMatchingPagePackets.contains(stableInstanceId))
				{
					// An unmatched visible draw is a complete-owner fail-open
					// submission. Selecting all of that owner's validated demanded
					// pages is conservative and keeps owner identity intact; it
					// never aliases work between independently animated clones.
					ownerFallbackViewMasks[stableInstanceId] |= viewMask;
				}
			}
		}

		std::vector<DeformationPageDemand> selectedDemands;
		selectedDemands.reserve(std::min(
			snapshot.animatedGeometryPageDemands.size(),
			selectedViewMasks.size()));
		for(const DeformationPageDemand& demand :
			snapshot.animatedGeometryPageDemands)
		{
			const std::uint64_t owner = demand.ownerStableInstanceId != 0u
				? demand.ownerStableInstanceId
				: demand.key.deformation.stableInstanceId;
			const auto selected = selectedViewMasks.find({
				owner,
				demand.key.page.pageId
			});
			const auto ownerFallback = ownerFallbackViewMasks.find(
				owner);
			const std::uint64_t requestedViewMask =
				(selected != selectedViewMasks.end()
					? selected->second
					: 0u)
				| (ownerFallback != ownerFallbackViewMasks.end()
					? ownerFallback->second
					: 0u);
			if(requestedViewMask == 0u)
			{
				continue;
			}
			DeformationPageDemand selectedDemand = demand;
			selectedDemand.requestedViewMask = requestedViewMask;
			selectedDemands.push_back(std::move(selectedDemand));
		}
		return selectedDemands;
	}

	VirtualGeometryViewSelectionState BuildVirtualGeometryViewSelectionState(const RenderFrameViewRequest& view, const std::vector<RenderFrameRenderableRecord>& renderables)
	{
		VirtualGeometryViewSelectionState state;
		state.outputKind = view.outputKind;
		state.viewType = view.outputKind == RenderOutputKind::MainRuntimeView ? RenderViewType::ExportRuntime : view.viewType;
		state.targetSize = view.targetSize;
		state.outputId = view.outputId;
		state.viewGeneration = view.viewGeneration;
		state.shadowPass = view.viewType == RenderViewType::Shadow;
		state.exportRuntime = view.viewType == RenderViewType::ExportRuntime || view.outputKind == RenderOutputKind::MainRuntimeView;
		state.hiZOcclusionAllowed = true;
		state.hiZInactiveReason.clear();
		state.draws.reserve(renderables.size());
		for (const RenderFrameRenderableRecord& renderable : renderables)
		{
			if (!renderable.virtualGeometry.virtualGeometryActive
				&& renderable.virtualGeometry.meshletCount == 0u
				&& !renderable.virtualGeometry.failOpen)
			{
				continue;
			}

			state.draws.push_back({
				renderable.objectId,
				renderable.meshId,
				renderable.virtualGeometry,
				renderable.taskShaderCullingEnabled
			});
		}
		return state;
	}

	std::vector<VirtualGeometryViewSelectionState> BuildVirtualGeometryViewSelectionStates(const RenderFrameSnapshot& snapshot)
	{
		if (!snapshot.virtualGeometryViewSelections.empty())
		{
			// Explicit per-view selections are authored by RenderSystem after each
			// visibility pass. Returning a copy preserves immutability for editor
			// diagnostics and prevents one view from mutating another view's range.
			return snapshot.virtualGeometryViewSelections;
		}

		std::vector<VirtualGeometryViewSelectionState> states;
		states.reserve(snapshot.viewRequests.size());
		for (const RenderFrameViewRequest& view : snapshot.viewRequests)
		{
			states.push_back(BuildVirtualGeometryViewSelectionState(view, snapshot.renderables));
		}
		return states;
	}

	VirtualGeometryCounterSnapshot BuildVirtualGeometryCounterSnapshot(const VirtualGeometryViewSelectionState& state, VirtualGeometryCounterSource source)
	{
		VirtualGeometryCounterSnapshot counters;
		counters.source = source;
		counters.authoritative = source == VirtualGeometryCounterSource::GPUReadback;
		switch (source)
		{
		case VirtualGeometryCounterSource::Prepared:
			counters.unavailableReason = "prepared CPU estimate before GPU readback";
			break;
		case VirtualGeometryCounterSource::GPUReadback:
			counters.unavailableReason.clear();
			break;
		case VirtualGeometryCounterSource::DelayedGPUReadback:
			counters.readbackLatencyFrames = 1u;
			counters.unavailableReason = "GPU readback is delayed; showing prepared estimate";
			break;
		case VirtualGeometryCounterSource::Unsupported:
			counters.unavailableReason = "backend does not support virtual geometry counter readback";
			break;
		case VirtualGeometryCounterSource::Unavailable:
			counters.unavailableReason = "virtual geometry counters are unavailable for this view";
			break;
		}
		if (!state.gpuCounterReadbacks.empty())
		{
			// Backend readback packets are already copied into the immutable
			// frame snapshot. Aggregating them here gives lightweight stats a
			// truthful source label without touching live GPU buffers.
			counters.source = state.gpuCounterReadbacks.front().source;
			counters.authoritative = false;
			counters.unavailableReason.clear();
			for (const VirtualGeometryCounterReadbackRecord& readback : state.gpuCounterReadbacks)
			{
				counters.authoritative = counters.authoritative || readback.source == VirtualGeometryCounterSource::GPUReadback;
				counters.readbackFrameId = std::max(counters.readbackFrameId, readback.frameId);
				counters.readbackLatencyFrames = std::max(counters.readbackLatencyFrames, readback.latencyFrames);
				counters.sourceMeshlets += readback.selectedMeshlets;
				counters.selectedMeshlets += readback.selectedMeshlets;
				counters.dispatchedMeshlets += readback.dispatchedMeshlets;
				counters.culledMeshlets += readback.culledMeshlets;
				counters.culledByHiZ += readback.culledByHiZ;
				counters.fallbackSubstitutions += readback.failOpenPackets;
				if (!readback.unavailableReason.empty() && counters.unavailableReason.empty())
				{
					counters.unavailableReason = readback.unavailableReason;
				}
			}
			if (counters.source != VirtualGeometryCounterSource::GPUReadback && counters.unavailableReason.empty())
			{
				counters.unavailableReason = "GPU virtual geometry counter readback is not authoritative for this frame";
			}
		}
		else if (!state.clusterPackets.empty())
		{
			for (const VirtualGeometryClusterPacket& packet : state.clusterPackets)
			{
				counters.sourceMeshlets += packet.meshletCount;
				counters.selectedMeshlets += packet.meshletCount;
				counters.dispatchedMeshlets += packet.meshletCount;
				if (packet.fallbackPacket)
				{
					counters.fallbackMeshlets += packet.meshletCount;
					++counters.fallbackSubstitutions;
				}
			}
		}
		else
		{
			for (const VirtualGeometryViewDrawRecord& draw : state.draws)
			{
				counters.sourceMeshlets += draw.range.meshletCount;
				counters.selectedMeshlets += draw.range.meshletCount;
				counters.dispatchedMeshlets += draw.range.meshletCount;
				if (draw.range.failOpen)
				{
					counters.fallbackMeshlets += draw.range.meshletCount;
				}
			}
		}
		for (const PageResidencyRecord& page : state.pageResidency)
		{
			if (page.state == GeometryPageState::Resident
				|| page.state == GeometryPageState::Pinned
				|| page.state == GeometryPageState::PendingRetirement)
			{
				++counters.residentPages;
			}
		}
		for (const VirtualGeometryPagePayloadRecord& payload : state.residentPagePayloads)
		{
			if (!payload.resident && payload.source != VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback)
			{
				continue;
			}
			++counters.residentPagePayloads;
			if (payload.source == VirtualGeometryPagePayloadSource::ResidentFallbackPayload || payload.pinnedFallback)
			{
				++counters.fallbackPagePayloads;
			}
			if (payload.source == VirtualGeometryPagePayloadSource::ConservativeEmbeddedFallback)
			{
				++counters.embeddedFallbackPayloads;
			}
		}
		for (const VirtualGeometryEmittedPageSourceRecord& emitted : state.emittedPageSources)
		{
			++counters.emittedPageSources;
			if (emitted.emittedSource == VirtualGeometryEmittedPageSource::FallbackPage || emitted.fallbackSubstitution)
			{
				++counters.fallbackSubstitutions;
			}
			if (emitted.emittedSource == VirtualGeometryEmittedPageSource::ConservativeEmbeddedFallback || emitted.conservativeEmbeddedFallback)
			{
				++counters.embeddedFallbackPayloads;
			}
		}
		for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
		{
			if (cluster.usingFallback)
			{
				++counters.fallbackSubstitutions;
			}
		}
		counters.requestedPages = static_cast<std::uint32_t>(state.pageNeeds.size());
		counters.missingPages = counters.requestedPages;
		return counters;
	}

	std::vector<OcclusionDebugSample> BuildVirtualGeometryOcclusionDebugSamples(const VirtualGeometryViewSelectionState& state)
	{
		std::vector<OcclusionDebugSample> samples;
		samples.reserve(std::min<std::size_t>(state.draws.size(), kVirtualGeometryOcclusionDebugSampleCapacity));
		for (const VirtualGeometryViewDrawRecord& draw : state.draws)
		{
			if (samples.size() >= kVirtualGeometryOcclusionDebugSampleCapacity)
			{
				break;
			}
			if (!draw.range.virtualGeometryActive && !draw.range.failOpen)
			{
				continue;
			}

			OcclusionDebugSample sample;
			sample.objectId = draw.objectId;
			sample.meshId = draw.meshId;
			sample.meshletId = draw.range.meshletOffset;
			if (draw.range.selectedClusterCount > 0u
				&& draw.range.selectedClusterOffset < state.selectedClusters.size())
			{
				const VirtualGeometrySelectedClusterRecord& cluster = state.selectedClusters[draw.range.selectedClusterOffset];
				sample.clusterId = cluster.clusterId;
			}
			sample.outcome = draw.range.failOpen
				? VirtualGeometryCullOutcome::FailOpen
				: VirtualGeometryCullOutcome::Visible;
			sample.reason = draw.range.failOpen
				? (draw.range.fallbackReason.empty() ? "Virtual geometry failed open to preserve visibility." : draw.range.fallbackReason)
				: "Selected meshlet remained visible; Hi-Z may only cull after full projected-bounds proof.";
			samples.push_back(std::move(sample));
		}
		return samples;
	}

	VirtualGeometryHiZValidationState BuildDefaultVirtualGeometryHiZValidationState(const VirtualGeometryViewSelectionState& state, std::uint64_t cameraSignature)
	{
		VirtualGeometryHiZValidationState hiZ;
		hiZ.renderSize = state.targetSize;
		hiZ.cameraSignature = cameraSignature;
		hiZ.depthSceneSignature = cameraSignature ^ 0x517cc1b727220a95ull;
		hiZ.pyramidReady = false;
		hiZ.failOpen = true;
		if (state.shadowPass || state.viewType == RenderViewType::Shadow)
		{
			hiZ.state = VirtualGeometryHiZState::Unsupported;
			hiZ.reason = "Shadow virtual geometry does not sample camera Hi-Z.";
			return hiZ;
		}
		if (state.targetSize.x == 0u || state.targetSize.y == 0u)
		{
			hiZ.state = VirtualGeometryHiZState::MissingPyramid;
			hiZ.reason = "Render target size is unavailable, so no current Hi-Z pyramid can be sampled.";
			return hiZ;
		}
		if (!state.hiZOcclusionAllowed)
		{
			hiZ.state = VirtualGeometryHiZState::FailOpen;
			hiZ.reason = state.hiZInactiveReason.empty()
				? "Hi-Z occlusion is failing open for this view."
				: state.hiZInactiveReason;
			return hiZ;
		}

		hiZ.state = VirtualGeometryHiZState::Active;
		hiZ.pyramidSize = state.targetSize;
		hiZ.warmupFrames = 1u;
		hiZ.pyramidReady = true;
		hiZ.failOpen = false;
		return hiZ;
	}

	VirtualGeometryViewVisibilitySnapshot BuildVirtualGeometryViewVisibilitySnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryViewSelectionState& state,
		VirtualGeometryCounterSource source)
	{
		VirtualGeometryViewVisibilitySnapshot visibility;
		visibility.outputKind = state.outputKind;
		visibility.viewType = state.viewType;
		visibility.frameId = snapshot.frameId;
		visibility.renderSize = state.targetSize;
		visibility.outputId = state.outputId;
		visibility.cameraSignature = BuildVirtualGeometryCameraSignature(snapshot, state);
		visibility.debugVisualizationMode = state.debugVisualizationMode;
		visibility.counters = BuildVirtualGeometryCounterSnapshot(state, source);
		visibility.counters.preparedFrameId = snapshot.frameId;
		if (source == VirtualGeometryCounterSource::GPUReadback)
		{
			visibility.counters.readbackFrameId = snapshot.frameId;
		}
		else if (source == VirtualGeometryCounterSource::DelayedGPUReadback)
		{
			visibility.counters.readbackFrameId = snapshot.frameId > 0u ? snapshot.frameId - 1u : 0u;
			visibility.counters.readbackLatencyFrames = snapshot.frameId > 0u ? 1u : 0u;
		}
		visibility.hiZValidationState = state.hasHiZValidationState
			? state.hiZValidationState
			: BuildDefaultVirtualGeometryHiZValidationState(state, visibility.cameraSignature);
		if (visibility.hiZValidationState.cameraSignature == 0u)
		{
			visibility.hiZValidationState.cameraSignature = visibility.cameraSignature;
		}
		else
		{
			visibility.cameraSignature = visibility.hiZValidationState.cameraSignature;
		}
		if (visibility.hiZValidationState.renderSize == glm::uvec2(0u))
		{
			visibility.hiZValidationState.renderSize = state.targetSize;
		}
		visibility.drawRanges = state.draws;
		visibility.selectedClusters = state.selectedClusters;
		visibility.clusterPackets = state.clusterPackets;
		visibility.clusterBatches = state.clusterBatches;
		visibility.gpuPacketRecords = state.gpuPacketRecords;
		visibility.gpuBatchDescriptors = state.gpuBatchDescriptors;
		visibility.gpuPacketFrameSets = state.gpuPacketFrameSets;
		visibility.gpuCompactionDispatches = state.gpuCompactionDispatches;
		visibility.generatedSubmissions = state.generatedSubmissions;
		visibility.gpuCounterReadbacks = state.gpuCounterReadbacks;
		visibility.gpuFallbackRecords = state.gpuFallbackRecords;
		visibility.compactionResults = state.compactionResults;
		visibility.pageNeeds = state.pageNeeds;
		visibility.pageRequests = state.pageRequests;
		visibility.residentPageSets = state.residentPageSets;
		visibility.residentPagePayloads = state.residentPagePayloads;
		visibility.emittedPageSources = state.emittedPageSources;
		visibility.pageRetirements = state.pageRetirements;
		visibility.pageResidency = state.pageResidency;
		visibility.fallbackCoverage = state.fallbackCoverage;
		visibility.hiZRegressionSnapshots = state.hiZRegressionSnapshots;
		visibility.coneSafetyMetadata = state.coneSafetyMetadata;
		visibility.shadowVisibilitySelections = state.shadowVisibilitySelections;
		visibility.performanceGates = state.performanceGates;
		visibility.lodTransitionStates = state.lodTransitionStates;
		visibility.occlusionDebugSamples = BuildVirtualGeometryOcclusionDebugSamples(state);
		visibility.source = source;
		visibility.valid = snapshot.valid;
		return visibility;
	}

	std::vector<VirtualGeometryViewVisibilitySnapshot> BuildVirtualGeometryViewVisibilitySnapshots(const RenderFrameSnapshot& snapshot)
	{
		if (!snapshot.virtualGeometryVisibilitySnapshots.empty())
		{
			// RenderSystem can publish backend-populated visibility records. Tests
			// receive copies so they can verify independence without touching the
			// protected renderer-owned publication buffer.
			return snapshot.virtualGeometryVisibilitySnapshots;
		}

		std::vector<VirtualGeometryViewSelectionState> states = BuildVirtualGeometryViewSelectionStates(snapshot);
		std::vector<VirtualGeometryViewVisibilitySnapshot> visibility;
		visibility.reserve(states.size());
		for (const VirtualGeometryViewSelectionState& state : states)
		{
			visibility.push_back(BuildVirtualGeometryViewVisibilitySnapshot(snapshot, state, VirtualGeometryCounterSource::Prepared));
		}
		return visibility;
	}

	std::vector<VirtualGeometryViewDiagnosticsSnapshot> BuildVirtualGeometryViewDiagnosticsFromSnapshot(
		const RenderFrameSnapshot& snapshot,
		const VirtualGeometryQualityProfile& qualityProfile,
		VirtualGeometryInactiveReason inactiveReason,
		const GpuDeformationPlan* completedDeformationPlan)
	{
		const VirtualGeometryQualityProfile quality = SanitizeVirtualGeometryQualityProfile(qualityProfile);
		std::optional<GpuDeformationPlan> fallbackDeformationPlan;
		if(completedDeformationPlan == nullptr && !snapshot.animatedGeometryPageDemands.empty()) {
			// Standalone diagnostics callers may not own the backend work plan. Build
			// it at most once here; the live renderer passes its completed immutable
			// plan so Game, Scene, and Shadow diagnostics never repeat this work.
			fallbackDeformationPlan = BuildGpuDeformationPlan(
				snapshot.frameId,
				snapshot.animatedGeometryPageDemands.front().key.deformation.rendererGeneration,
				snapshot.animatedGeometryPageDemands);
			completedDeformationPlan = &*fallbackDeformationPlan;
		}
		const auto ApplyAnimatedGeometryDiagnostics =
			[&snapshot, completedDeformationPlan](VirtualGeometryViewDiagnosticsSnapshot& viewDiagnostics)
		{
			// Visibility snapshots are the preferred live path, while prepared
			// selection states are the fallback path. Both must be enriched from
			// the same frame-wide deformation demand list or MCP/editor views can
			// incorrectly report zero work after compute deformation was accepted.
			viewDiagnostics.animatedPageDemandCount =
				static_cast<std::uint32_t>(
					snapshot.animatedGeometryPageDemands.size());
			if(snapshot.animatedGeometryPageDemands.empty())
			{
				viewDiagnostics.animatedBoundsSource = "NoAnimatedPageWork";
				return;
			}
			if(completedDeformationPlan == nullptr) {
				viewDiagnostics.animatedFallbackCount =
					viewDiagnostics.animatedPageDemandCount;
				viewDiagnostics.animatedSkippedWorkCount =
					viewDiagnostics.animatedPageDemandCount;
				viewDiagnostics.animatedBoundsSource = "MissingCompletedDeformationPlan";
				viewDiagnostics.animatedFallbackReason =
					"Animated demands were present without a completed deformation plan.";
				return;
			}
			const GpuDeformationPlan& deformation = *completedDeformationPlan;
			if(deformation.IsComplete())
			{
				viewDiagnostics.animatedUniqueDispatchCount =
					static_cast<std::uint32_t>(deformation.work.size());
				viewDiagnostics.animatedReuseCount =
					deformation.reusedDemandCount;
				viewDiagnostics.deformationCurrentBytes =
					static_cast<std::uint64_t>(deformation.totalVertexCount)
						* sizeof(GpuDeformedVertexRecord);
				viewDiagnostics.deformationHistoryBytes =
					viewDiagnostics.deformationCurrentBytes;
				viewDiagnostics.deformationBoundsBytes =
					static_cast<std::uint64_t>(
						deformation.totalMeshletBoundsCount
							+ deformation.totalClusterBoundsCount)
						* sizeof(GpuDynamicBoundsRecord);
				viewDiagnostics.deformationHighWaterBytes =
					viewDiagnostics.deformationCurrentBytes
						+ viewDiagnostics.deformationHistoryBytes
						+ viewDiagnostics.deformationBoundsBytes;
				bool everyWorkRecordCanReuseHistory = true;
				for(const DeformationWorkRecord& work : deformation.work)
				{
					if(std::find(viewDiagnostics.animatedAnimationLods.begin(),
						viewDiagnostics.animatedAnimationLods.end(),
						work.key.animationLod)
						== viewDiagnostics.animatedAnimationLods.end())
					{
						viewDiagnostics.animatedAnimationLods.push_back(
							work.key.animationLod);
					}
					everyWorkRecordCanReuseHistory =
						everyWorkRecordCanReuseHistory && work.historyValid;
				}
				std::sort(viewDiagnostics.animatedAnimationLods.begin(),
					viewDiagnostics.animatedAnimationLods.end());
				// Compatible demands are merged into one dispatch and reported by
				// animatedReuseCount. Do not double-count that sharing as skipped work.
				viewDiagnostics.animatedSkippedWorkCount = 0u;
				viewDiagnostics.animatedBoundsSource = everyWorkRecordCanReuseHistory
					? "PlannedCurrentAndHistoryDynamicBounds"
					: "PlannedCurrentGenerationDynamicBounds";
				return;
			}
			viewDiagnostics.animatedFallbackCount =
				viewDiagnostics.animatedPageDemandCount;
			viewDiagnostics.animatedSkippedWorkCount =
				viewDiagnostics.animatedPageDemandCount;
			viewDiagnostics.animatedBoundsSource =
				"ConservativeOwnerEnvelopeFallback";
			viewDiagnostics.animatedFallbackReason =
				deformation.rejectionReason;
		};
		if (!snapshot.virtualGeometryVisibilitySnapshots.empty())
		{
			std::vector<VirtualGeometryViewDiagnosticsSnapshot> diagnostics;
			diagnostics.reserve(snapshot.virtualGeometryVisibilitySnapshots.size());
			for (const VirtualGeometryViewVisibilitySnapshot& visibility : snapshot.virtualGeometryVisibilitySnapshots)
			{
				VirtualGeometryViewDiagnosticsSnapshot viewDiagnostics =
					BuildVirtualGeometryDiagnosticsFromVisibilitySnapshot(visibility);
				ApplyAnimatedGeometryDiagnostics(viewDiagnostics);
				if (inactiveReason != VirtualGeometryInactiveReason::None)
				{
					viewDiagnostics.active = false;
					viewDiagnostics.inactiveReason = inactiveReason;
					viewDiagnostics.warnings.push_back("Virtual geometry counters are unavailable because the view is inactive: " + std::string(ToString(inactiveReason)));
				}
				diagnostics.push_back(std::move(viewDiagnostics));
			}
			return diagnostics;
		}

		// Live RenderSystem snapshots already own the prepared view states. Calling
		// BuildVirtualGeometryViewSelectionStates here used to return those states by
		// value, which deep-copied every draw, cluster, page, batch, and fallback
		// vector solely so the stats publisher could read them. Keep a local fallback
		// only for standalone callers that supplied renderables without view states.
		std::vector<VirtualGeometryViewSelectionState> fallbackStates;
		const std::vector<VirtualGeometryViewSelectionState>* states =
			&snapshot.virtualGeometryViewSelections;
		if(states->empty())
		{
			fallbackStates = BuildVirtualGeometryViewSelectionStates(snapshot);
			states = &fallbackStates;
		}
		std::vector<VirtualGeometryViewDiagnosticsSnapshot> diagnostics;
		diagnostics.reserve(states->size());
		for (const VirtualGeometryViewSelectionState& state : *states)
		{
			VirtualGeometryViewDiagnosticsSnapshot viewDiagnostics;
			viewDiagnostics.viewType = state.viewType;
			viewDiagnostics.viewId = state.outputId;
			viewDiagnostics.frameId = snapshot.frameId;
			viewDiagnostics.active = true;
			viewDiagnostics.inactiveReason = VirtualGeometryInactiveReason::None;
			viewDiagnostics.hiZActive = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZHistoryValid = viewDiagnostics.hiZActive;
			viewDiagnostics.counterSource = VirtualGeometryCounterSource::Prepared;
			VirtualGeometryHiZValidationState hiZState = state.hasHiZValidationState
				? state.hiZValidationState
				: BuildDefaultVirtualGeometryHiZValidationState(
					state,
					BuildVirtualGeometryCameraSignature(snapshot, state));
			if (hiZState.cameraSignature == 0u)
			{
				hiZState.cameraSignature = BuildVirtualGeometryCameraSignature(snapshot, state);
			}
			viewDiagnostics.hiZState = hiZState.state;
			viewDiagnostics.hiZPyramidSize = hiZState.pyramidSize;
			viewDiagnostics.hiZRenderSize = hiZState.renderSize;
			viewDiagnostics.hiZCameraSignature = hiZState.cameraSignature;
			viewDiagnostics.hiZDepthSceneSignature = hiZState.depthSceneSignature;
			viewDiagnostics.hiZWarmupFrames = hiZState.warmupFrames;
			if (hiZState.state != VirtualGeometryHiZState::Active)
			{
				viewDiagnostics.hiZInvalidReason = hiZState.reason;
				viewDiagnostics.hiZActive = false;
				viewDiagnostics.hiZHistoryValid = false;
			}
			viewDiagnostics.effectiveLodErrorScale = quality.lodErrorScale;
			viewDiagnostics.configuredMaxLodDrop = quality.maxLodDrop;
			viewDiagnostics.lodTransitionHysteresisPixels = quality.lodTransitionHysteresisPixels;
			viewDiagnostics.lodTransitionStableFrames = quality.lodTransitionStableFrames;
			viewDiagnostics.geometryBudgetBytes = quality.geometryBudgetBytes;
			ApplyAnimatedGeometryDiagnostics(viewDiagnostics);
			viewDiagnostics.clusterSelection.selectedClusters = static_cast<std::uint32_t>(state.selectedClusters.size());
			viewDiagnostics.clusterPacketCount = static_cast<std::uint32_t>(state.clusterPackets.size());
			// Lightweight frame stats must expose the same packet and batch
			// counters as full diagnostics, but without scanning every cooked
			// asset or generating performance-gate strings on normal frames.
			for (const ClusterWorkBatch& batch : state.clusterBatches)
			{
				++viewDiagnostics.submittedBatchCount;
				viewDiagnostics.submittedMeshlets += batch.submittedMeshlets;
				if (batch.fallbackMode == VirtualGeometryBatchFallbackMode::SinglePacketDraw)
				{
					++viewDiagnostics.singlePacketFallbackBatchCount;
				}
			}
			for (const CompactedVisibilitySet& compaction : state.compactionResults)
			{
				viewDiagnostics.candidateClusterCount += compaction.candidatePackets;
				viewDiagnostics.compactedClusterCount += compaction.visiblePackets;
				viewDiagnostics.rejectedClusterCount += compaction.rejectedByFrustum
					+ compaction.rejectedByHiZ
					+ compaction.rejectedByResidency
					+ compaction.rejectedByCone;
				viewDiagnostics.failOpenClusterCount += compaction.failOpenPackets;
				viewDiagnostics.culledByFrustum += compaction.rejectedByFrustum;
				viewDiagnostics.culledByHiZ += compaction.rejectedByHiZ;
				viewDiagnostics.culledByCone += compaction.rejectedByCone;
				viewDiagnostics.culledByResidency += compaction.rejectedByResidency;
				viewDiagnostics.submittedBatchCount = std::max(viewDiagnostics.submittedBatchCount, compaction.submittedBatches);
			}
			viewDiagnostics.gpuPacketRecordCount = static_cast<std::uint32_t>(state.gpuPacketRecords.size());
			viewDiagnostics.gpuBatchDescriptorCount = static_cast<std::uint32_t>(state.gpuBatchDescriptors.size());
			viewDiagnostics.gpuPacketFrameSetCount = static_cast<std::uint32_t>(state.gpuPacketFrameSets.size());
			viewDiagnostics.gpuCompactionDispatchCount = static_cast<std::uint32_t>(state.gpuCompactionDispatches.size());
			viewDiagnostics.generatedSubmissionCount = static_cast<std::uint32_t>(state.generatedSubmissions.size());
			viewDiagnostics.gpuCounterReadbackCount = static_cast<std::uint32_t>(state.gpuCounterReadbacks.size());
			viewDiagnostics.gpuFallbackRecordCount = static_cast<std::uint32_t>(state.gpuFallbackRecords.size());
			for (const GpuVirtualGeometryPacketFrameSet& frameSet : state.gpuPacketFrameSets)
			{
				switch (frameSet.validity)
				{
				case VirtualGeometryGpuFrameSetValidity::Valid:
					++viewDiagnostics.gpuValidFrameSetCount;
					break;
				case VirtualGeometryGpuFrameSetValidity::Delayed:
					++viewDiagnostics.gpuDelayedFrameSetCount;
					break;
				case VirtualGeometryGpuFrameSetValidity::Invalid:
				case VirtualGeometryGpuFrameSetValidity::Unsupported:
					++viewDiagnostics.gpuInvalidFrameSetCount;
					break;
				case VirtualGeometryGpuFrameSetValidity::FailOpen:
					++viewDiagnostics.gpuFailOpenFrameSetCount;
					break;
				}
				if (!frameSet.fallbackReason.empty() && viewDiagnostics.gpuFallbackReason.empty())
				{
					viewDiagnostics.gpuFallbackReason = frameSet.fallbackReason;
				}
			}
			for (const GeneratedVirtualGeometrySubmissionRecord& submission : state.generatedSubmissions)
			{
				switch (submission.validationState)
				{
				case VirtualGeometryGeneratedSubmissionValidationState::Valid:
					++viewDiagnostics.validGeneratedSubmissionCount;
					break;
				case VirtualGeometryGeneratedSubmissionValidationState::Delayed:
				case VirtualGeometryGeneratedSubmissionValidationState::Mismatch:
				case VirtualGeometryGeneratedSubmissionValidationState::Overflow:
				case VirtualGeometryGeneratedSubmissionValidationState::Unsupported:
					++viewDiagnostics.invalidGeneratedSubmissionCount;
					break;
				case VirtualGeometryGeneratedSubmissionValidationState::FailOpen:
					++viewDiagnostics.fallbackGeneratedSubmissionCount;
					break;
				}
				if (!submission.fallbackReason.empty() && viewDiagnostics.gpuFallbackReason.empty())
				{
					viewDiagnostics.gpuFallbackReason = submission.fallbackReason;
				}
			}
			for (const VirtualGeometryCounterReadbackRecord& readback : state.gpuCounterReadbacks)
			{
				viewDiagnostics.gpuCounterReadbackLatencyFrames = std::max(viewDiagnostics.gpuCounterReadbackLatencyFrames, readback.latencyFrames);
				if (!readback.unavailableReason.empty() && viewDiagnostics.counterUnavailableReason.empty())
				{
					viewDiagnostics.counterUnavailableReason = readback.unavailableReason;
				}
			}
			for (const VirtualGeometryGpuFallbackRecord& fallback : state.gpuFallbackRecords)
			{
				viewDiagnostics.gpuFallbackAffectedPackets += fallback.affectedPackets;
				viewDiagnostics.gpuFallbackAffectedBatches += fallback.affectedBatches;
				if (!fallback.reason.empty() && viewDiagnostics.gpuFallbackReason.empty())
				{
					viewDiagnostics.gpuFallbackReason = fallback.reason;
				}
				if (!fallback.diagnostic.empty())
				{
					viewDiagnostics.warnings.push_back(fallback.diagnostic);
				}
			}
			if (!viewDiagnostics.gpuFallbackReason.empty())
			{
				viewDiagnostics.warnings.push_back("Virtual geometry GPU packet path used fallback: " + viewDiagnostics.gpuFallbackReason);
			}
			if (!state.pageResidency.empty())
			{
				for (const PageResidencyRecord& page : state.pageResidency)
				{
					switch (page.state)
					{
					case GeometryPageState::Resident:
						++viewDiagnostics.residentPages;
						break;
					case GeometryPageState::Pinned:
						++viewDiagnostics.residentPages;
						++viewDiagnostics.pinnedFallbackPages;
						break;
					case GeometryPageState::PendingRetirement:
						++viewDiagnostics.residentPages;
						break;
					case GeometryPageState::Requested:
					case GeometryPageState::Loading:
					case GeometryPageState::PendingIO:
					case GeometryPageState::PendingUpload:
						++viewDiagnostics.requestedPages;
						break;
					case GeometryPageState::Failed:
						++viewDiagnostics.failedPages;
						++viewDiagnostics.missingPages;
						break;
					case GeometryPageState::Evicting:
						++viewDiagnostics.evictingPages;
						++viewDiagnostics.missingPages;
						break;
					case GeometryPageState::Evicted:
					case GeometryPageState::Retired:
						++viewDiagnostics.evictedPages;
						++viewDiagnostics.missingPages;
						break;
					case GeometryPageState::NotResident:
						++viewDiagnostics.missingPages;
						break;
					}
				}
			}
			else
			{
				viewDiagnostics.requestedPages = static_cast<std::uint32_t>(state.pageNeeds.size());
				viewDiagnostics.missingPages = static_cast<std::uint32_t>(state.pageNeeds.size());
			}
			viewDiagnostics.lodTransitionStates = static_cast<std::uint32_t>(state.lodTransitionStates.size());
			for (const VirtualGeometryLODTransitionState& transitionState : state.lodTransitionStates)
			{
				if (transitionState.heldPreviousSelection)
				{
					++viewDiagnostics.lodTransitionHeldSelections;
				}
				if (!transitionState.heldPreviousSelection && transitionState.valid)
				{
					++viewDiagnostics.lodTransitionAcceptedSelections;
				}
				if (transitionState.resetThisFrame)
				{
					++viewDiagnostics.lodTransitionResets;
				}
				viewDiagnostics.lodTransitionHysteresisPixels = transitionState.hysteresisBandPixels;
			}
			for (const VirtualGeometrySelectedClusterRecord& cluster : state.selectedClusters)
			{
				if (cluster.usingFallback)
				{
					++viewDiagnostics.clusterSelection.fallbackClusters;
				}
			}
			if (!state.pageNeeds.empty())
			{
				viewDiagnostics.warnings.push_back("Virtual geometry selected resident fallback clusters while requesting missing high-detail pages.");
			}
			viewDiagnostics.hiZValidation.ownerViewType = state.viewType;
			viewDiagnostics.hiZValidation.sameViewHistory = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZValidation.sameTargetHistory = state.hiZOcclusionAllowed;
			viewDiagnostics.hiZValidation.projectionMatches = viewDiagnostics.hiZActive;
			viewDiagnostics.hiZValidation.failOpen = hiZState.failOpen || hiZState.state != VirtualGeometryHiZState::Active;
			viewDiagnostics.hiZValidation.inactiveReason = viewDiagnostics.hiZInvalidReason;
			viewDiagnostics.occlusionDebugSamples = BuildVirtualGeometryOcclusionDebugSamples(state);
			for (const VirtualGeometryViewDrawRecord& draw : state.draws)
			{
				viewDiagnostics.selectedMeshlets += draw.range.meshletCount;
				viewDiagnostics.dispatchedMeshlets += draw.range.meshletCount;
				if (viewDiagnostics.selectedLodHistogram.size() <= draw.range.selectedLodIndex)
				{
					viewDiagnostics.selectedLodHistogram.resize(static_cast<std::size_t>(draw.range.selectedLodIndex) + 1u, 0u);
				}
				viewDiagnostics.selectedLodHistogram[draw.range.selectedLodIndex] += 1u;
				if (draw.range.failOpen)
				{
					viewDiagnostics.warnings.push_back(draw.range.fallbackReason.empty()
						? "Virtual geometry failed open to preserve visibility."
						: draw.range.fallbackReason);
				}
			}
			viewDiagnostics.sourceMeshlets = std::max(viewDiagnostics.sourceMeshlets, viewDiagnostics.selectedMeshlets);
			if (state.shadowPass)
			{
				viewDiagnostics.warnings.push_back("Shadow virtual geometry uses conservative LOD; camera Hi-Z ownership is diagnostic-only.");
			}
			if (state.exportRuntime)
			{
				viewDiagnostics.warnings.push_back("Export runtime virtual geometry state is isolated from editor Scene View diagnostics.");
			}
			diagnostics.push_back(std::move(viewDiagnostics));
		}
		return diagnostics;
	}

}
