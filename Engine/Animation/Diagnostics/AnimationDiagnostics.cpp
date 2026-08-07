// AnimationDiagnostics.cpp - Atomic latest-value diagnostics publication.

#include "Animation/Diagnostics/AnimationDiagnostics.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		std::mutex gAnimationDiagnosticsMutex;
		std::optional<AnimationDiagnosticsSnapshot> gAnimationDiagnosticsSnapshot;
		std::mutex gAnimationDetailedDiagnosticsMutex;
		std::optional<AnimationDetailedDiagnosticsSnapshot> gAnimationDetailedDiagnosticsSnapshot;
		std::size_t gAnimationDetailedDiagnosticsRequestCount = 0U;

		void ReleaseAnimationDetailedDiagnosticsRequest() noexcept {
			std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
			if(gAnimationDetailedDiagnosticsRequestCount == 0U) return;
			--gAnimationDetailedDiagnosticsRequestCount;
			if(gAnimationDetailedDiagnosticsRequestCount == 0U) {
				// Detail data is meaningful only while at least one consumer asked for
				// it. Clearing at the final release prevents stale rows surviving an
				// editor close or a completed MCP request.
				gAnimationDetailedDiagnosticsSnapshot.reset();
			}
		}

		bool HasDormantOnlyWork(const AnimationDiagnosticsCounters& counters) noexcept {
			return counters.activeAnimators == 0U
				&& counters.evaluatedAnimators == 0U
				&& counters.suspendedAnimators == 0U
				&& counters.degradedAnimators == 0U
				&& counters.schedulerMembershipChecks <= 1U
				&& counters.workerJobs == 0U
				&& counters.transientAllocations == 0U
				&& counters.poseAllocations == 0U
				&& counters.residencyRequests == 0U
				&& counters.gpuDispatches == 0U
				&& counters.gpuUploadBytes == 0U
				&& counters.gpuReadbacks == 0U
				&& counters.gpuWaits == 0U
				&& counters.physicsHandoffs == 0U
				&& counters.managedCallbacks == 0U
				&& counters.detailedRecords == 0U
				&& counters.parameterQueueDepth == 0U
				&& counters.commandQueueDepth == 0U
				&& counters.eventQueueDepth == 0U
				&& counters.rootMotionQueueDepth == 0U
				&& counters.rejectedBatches == 0U
				&& counters.staleWorkItems == 0U
				&& counters.gpuVisualAnimators == 0U
				&& counters.hybridAuthoritativeAnimators == 0U
				&& counters.cpuFallbackAnimators == 0U
				&& counters.cpuSampledJoints == 0U
				&& counters.gpuEvaluatedJoints == 0U
				&& counters.gpuBatches == 0U
				&& counters.gpuTransferBytes == 0U
				&& counters.gpuReadbackBytes == 0U
				&& counters.completePoseReadbackBytes == 0U
				&& counters.boundedMetadataReadbackBytes == 0U
				&& counters.residencyHits == 0U
				&& counters.residencyMisses == 0U
				&& counters.fallbackCount == 0U
				&& counters.modeSwitchCount == 0U
				&& counters.compactEligibleAnimators == 0U
				&& counters.compactProcessedAnimators == 0U
				&& counters.completeCapturedAnimators == 0U
				&& counters.completeProcessedAnimators == 0U
				&& counters.compactToCompleteTransitions == 0U
				&& counters.completeToCompactTransitions == 0U
				&& counters.retainedTopologyHits == 0U
				&& counters.retainedTopologyMisses == 0U
				&& counters.compactPlanGroups == 0U
				&& counters.compactDispatches == 0U
				&& counters.rejectedCandidateCount == 0U
				&& std::all_of(counters.fallbackReasonCounts.begin(),
					counters.fallbackReasonCounts.end(), [](std::uint64_t count) {
						return count == 0U;
					});
		}

		bool HasFiniteNonNegativeTimings(const AnimationDiagnosticsTimings& timings) noexcept {
			return std::isfinite(timings.preparationMs) && timings.preparationMs >= 0.0
				&& std::isfinite(timings.evaluationMs) && timings.evaluationMs >= 0.0
				&& std::isfinite(timings.publicationMs) && timings.publicationMs >= 0.0
				&& std::isfinite(timings.presentationMs) && timings.presentationMs >= 0.0
				&& std::isfinite(timings.cpuAuthorityMs) && timings.cpuAuthorityMs >= 0.0
				&& std::isfinite(timings.gpuPlanningMs) && timings.gpuPlanningMs >= 0.0
				&& std::isfinite(timings.gpuExecutionMs) && timings.gpuExecutionMs >= 0.0
				&& std::isfinite(timings.deformationMs) && timings.deformationMs >= 0.0
				&& std::isfinite(timings.compactCaptureMs) && timings.compactCaptureMs >= 0.0
				&& std::isfinite(timings.compactEvaluationMs) && timings.compactEvaluationMs >= 0.0
				&& std::isfinite(timings.compactPublicationMs) && timings.compactPublicationMs >= 0.0
				&& std::isfinite(timings.completeCaptureMs) && timings.completeCaptureMs >= 0.0
				&& std::isfinite(timings.completeEvaluationMs) && timings.completeEvaluationMs >= 0.0
				&& std::isfinite(timings.completePublicationMs) && timings.completePublicationMs >= 0.0
				&& std::isfinite(timings.topologyBuildMs) && timings.topologyBuildMs >= 0.0
				&& std::isfinite(timings.topologyRefreshMs) && timings.topologyRefreshMs >= 0.0;
		}

		bool IsRequestedPolicyValid(AnimationExecutionPolicy policy) noexcept {
			return policy >= AnimationExecutionPolicy::Auto
				&& policy <= AnimationExecutionPolicy::ForceCpuDeterministicFallback;
		}

		bool IsEffectiveModeValid(AnimationEffectiveExecutionMode mode) noexcept {
			return mode >= AnimationEffectiveExecutionMode::GpuVisual
				&& mode <= AnimationEffectiveExecutionMode::CpuDeterministicFallback;
		}

		bool IsReasonValid(AnimationExecutionModeReason reason) noexcept {
			return reason >= AnimationExecutionModeReason::Uninitialized
				&& reason <= AnimationExecutionModeReason::InvalidPreviousState;
		}

		bool IsPoseResidencyValid(AnimationPoseResidencyState residency) noexcept {
			return residency >= AnimationPoseResidencyState::Unavailable
				&& residency <= AnimationPoseResidencyState::Evicted;
		}
	}

	AnimationDiagnosticsSnapshot MakeDormantAnimationDiagnostics(
		std::uint64_t frameIndex,
		AnimationFixedTick fixedTick,
		std::uint64_t schedulerMembershipChecks) noexcept {
		AnimationDiagnosticsSnapshot snapshot;
		snapshot.frameIndex = frameIndex;
		snapshot.fixedTick = fixedTick;
		snapshot.detailLevel = AnimationDiagnosticsDetailLevel::Summary;
		snapshot.dormant = true;
		snapshot.counters.schedulerMembershipChecks = schedulerMembershipChecks;
		return snapshot;
	}

	bool ValidateAnimationDiagnosticsSnapshot(const AnimationDiagnosticsSnapshot& snapshot) noexcept {
		if(snapshot.schemaVersion != 1U
			|| snapshot.baseline.feature != kAnimationFeatureIdentity
			|| snapshot.baseline.sourceCommit != kAnimationBaselineSourceCommit
			|| snapshot.baseline.manifestSha256 != kAnimationBaselineManifestSha256
			|| snapshot.baseline.evidenceSha256 != kAnimationBaselineEvidenceSha256
			|| snapshot.baseline.workloadsSha256 != kAnimationPerformanceWorkloadsSha256) {
			return false;
		}
		const std::uint64_t processed = snapshot.counters.compactProcessedAnimators
			+ snapshot.counters.completeProcessedAnimators;
		if((snapshot.counters.compactProcessedAnimators > 0U
				|| snapshot.counters.completeProcessedAnimators > 0U)
			&& (processed < snapshot.counters.compactProcessedAnimators
				|| processed > snapshot.counters.activeAnimators)) {
			return false;
		}
		if(snapshot.detailLevel > AnimationDiagnosticsDetailLevel::Detailed
			|| !HasFiniteNonNegativeTimings(snapshot.timings)
			|| snapshot.counters.detailedRecords > GetAnimationRuntimeLimit(AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum
			|| snapshot.counters.completePoseReadbackBytes != 0U
			|| snapshot.counters.boundedMetadataReadbackBytes > snapshot.counters.gpuReadbackBytes
			|| snapshot.counters.gpuReadbackBytes > snapshot.counters.gpuTransferBytes
			|| (snapshot.counters.gpuReadbacks == 0U && snapshot.counters.gpuReadbackBytes != 0U)
			|| snapshot.counters.gpuVisualAnimators > snapshot.counters.activeAnimators
			|| snapshot.counters.hybridAuthoritativeAnimators > snapshot.counters.activeAnimators
			|| snapshot.counters.cpuFallbackAnimators > snapshot.counters.activeAnimators) {
			return false;
		}
		if(snapshot.dormant) {
			return snapshot.detailLevel != AnimationDiagnosticsDetailLevel::Detailed
				&& HasDormantOnlyWork(snapshot.counters)
				&& snapshot.timings.preparationMs == 0.0
				&& snapshot.timings.evaluationMs == 0.0
				&& snapshot.timings.publicationMs == 0.0
				&& snapshot.timings.presentationMs == 0.0
				&& snapshot.timings.cpuAuthorityMs == 0.0
				&& snapshot.timings.gpuPlanningMs == 0.0
				&& snapshot.timings.gpuExecutionMs == 0.0
				&& snapshot.timings.deformationMs == 0.0
				&& snapshot.timings.compactCaptureMs == 0.0
				&& snapshot.timings.compactEvaluationMs == 0.0
				&& snapshot.timings.compactPublicationMs == 0.0
				&& snapshot.timings.completeCaptureMs == 0.0
				&& snapshot.timings.completeEvaluationMs == 0.0
				&& snapshot.timings.completePublicationMs == 0.0
				&& snapshot.timings.topologyBuildMs == 0.0
				&& snapshot.timings.topologyRefreshMs == 0.0;
		}
		return snapshot.counters.activeAnimators > 0U;
	}

	bool PublishAnimationDiagnosticsSnapshot(const AnimationDiagnosticsSnapshot& snapshot) {
		if(!ValidateAnimationDiagnosticsSnapshot(snapshot)) {
			return false;
		}
		// Validation accepts value-equivalent identity text so ABI/editor callers do
		// not need to share these exact string literal addresses. Retained snapshots
		// must not keep a caller-owned string_view, though: the producer may release
		// that storage immediately after this protected publication boundary.
		// Rebinding to the canonical defaults preserves the allocation-free value
		// record while guaranteeing every retained view has program lifetime.
		AnimationDiagnosticsSnapshot retainedSnapshot = snapshot;
		retainedSnapshot.baseline = AnimationBaselineIdentity{};
		std::scoped_lock lock(gAnimationDiagnosticsMutex);
		gAnimationDiagnosticsSnapshot = retainedSnapshot;
		return true;
	}

	std::optional<AnimationDiagnosticsSnapshot> AcquireAnimationDiagnosticsSnapshot() {
		std::scoped_lock lock(gAnimationDiagnosticsMutex);
		return gAnimationDiagnosticsSnapshot;
	}

	void ClearPublishedAnimationDiagnostics() {
		std::scoped_lock lock(gAnimationDiagnosticsMutex);
		gAnimationDiagnosticsSnapshot.reset();
	}

	bool ValidateAnimationDetailedDiagnosticsSnapshot(
		const AnimationDetailedDiagnosticsSnapshot& snapshot) noexcept {
		const std::uint64_t maximumHistory =
			GetAnimationRuntimeLimit(AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum;
		if(snapshot.schemaVersion != 1U
			|| snapshot.animators.size() > kMaximumAnimationExecutionModeBatchSize
			|| snapshot.modeSwitchHistory.size() > maximumHistory
			|| (snapshot.collapsed && (!snapshot.animators.empty() || !snapshot.modeSwitchHistory.empty()))) {
			return false;
		}

		for(const AnimationAnimatorDiagnosticRecord& record : snapshot.animators) {
			if(record.stableInstanceId == 0U
				|| record.instanceGeneration == 0U
				|| !IsRequestedPolicyValid(record.requestedPolicy)
				|| !IsEffectiveModeValid(record.effectiveMode)
				|| !IsReasonValid(record.selectionReason)
				|| !IsReasonValid(record.fallbackReason)
				|| !IsPoseResidencyValid(record.poseResidency)
				|| record.completePoseReadbackBytes != 0U
				|| record.readbackBytes > record.transferBytes
				|| (record.readbackCount == 0U && record.readbackBytes != 0U)
				|| (record.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
					&& record.cpuAuthoritativeJointCount != 0U)) {
				return false;
			}
		}

		for(const AnimationModeSwitchDiagnosticRecord& record : snapshot.modeSwitchHistory) {
			if(record.stableInstanceId == 0U
				|| record.fixedTick < 0
				|| !IsEffectiveModeValid(record.from)
				|| !IsEffectiveModeValid(record.to)
				|| !IsReasonValid(record.reason)
				|| record.from == record.to) {
				return false;
			}
		}
		return true;
	}

	bool PublishAnimationDetailedDiagnosticsSnapshot(
		const AnimationDetailedDiagnosticsSnapshot& snapshot) {
		if(!ValidateAnimationDetailedDiagnosticsSnapshot(snapshot)) {
			return false;
		}
		// Copy before taking the publication lock. Producers remain free to reuse
		// their vectors after this protected boundary, and readers never borrow them.
		AnimationDetailedDiagnosticsSnapshot retained = snapshot;
		std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
		if(gAnimationDetailedDiagnosticsRequestCount == 0U) {
			// A lease may end while the producer is preparing its copied vector.
			// Reject instead of republishing rows after the last consumer released.
			return false;
		}
		gAnimationDetailedDiagnosticsSnapshot = std::move(retained);
		return true;
	}

	std::optional<AnimationDetailedDiagnosticsSnapshot>
	AcquireAnimationDetailedDiagnosticsSnapshot() {
		std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
		return gAnimationDetailedDiagnosticsSnapshot;
	}

	AnimationDetailedDiagnosticsRequest::AnimationDetailedDiagnosticsRequest(
		AnimationDetailedDiagnosticsRequest&& other) noexcept
		: active_(std::exchange(other.active_, false)) {
	}

	AnimationDetailedDiagnosticsRequest& AnimationDetailedDiagnosticsRequest::operator=(
		AnimationDetailedDiagnosticsRequest&& other) noexcept {
		if(this != &other) {
			Reset();
			active_ = std::exchange(other.active_, false);
		}
		return *this;
	}

	AnimationDetailedDiagnosticsRequest::~AnimationDetailedDiagnosticsRequest() {
		Reset();
	}

	void AnimationDetailedDiagnosticsRequest::Reset() noexcept {
		if(!active_) return;
		active_ = false;
		ReleaseAnimationDetailedDiagnosticsRequest();
	}

	AnimationDetailedDiagnosticsRequest AcquireAnimationDetailedDiagnosticsRequest() {
		std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
		if(gAnimationDetailedDiagnosticsRequestCount == std::numeric_limits<std::size_t>::max()) {
			// Saturation cannot safely represent one more independent owner. Return an
			// inactive lease so callers fail explicitly rather than silently sharing it.
			return {};
		}
		++gAnimationDetailedDiagnosticsRequestCount;
		return AnimationDetailedDiagnosticsRequest(true);
	}

	bool IsAnimationDetailedDiagnosticsRequested() noexcept {
		std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
		return gAnimationDetailedDiagnosticsRequestCount != 0U;
	}

	void ClearPublishedAnimationDetailedDiagnostics() {
		std::scoped_lock lock(gAnimationDetailedDiagnosticsMutex);
		gAnimationDetailedDiagnosticsSnapshot.reset();
	}
}
