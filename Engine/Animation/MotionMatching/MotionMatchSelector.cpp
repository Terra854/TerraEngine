// MotionMatchSelector.cpp - Deterministic CPU authority and safe fallback.

#include "Animation/MotionMatching/MotionMatchSelector.h"

#include <algorithm>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		MotionMatchFallbackReason ClassifyGpuFallback(
			const MotionDatabaseAsset& database,
			const MotionMatchQuery& query,
			const MotionMatchGpuReadback* readback) noexcept {
			if(readback == nullptr) return MotionMatchFallbackReason::GpuUnavailable;
			if(readback->cancelled || readback->status == AnimationStatus::Cancelled) {
				return MotionMatchFallbackReason::GpuCancelled;
			}
			if(readback->generation.database != query.database
				|| readback->generation.schemaFingerprint != database.featureSchemaFingerprint
				|| readback->state == MotionMatchGpuRequestState::Stale
				|| readback->status == AnimationStatus::StaleHandle) {
				return MotionMatchFallbackReason::StaleGpuResult;
			}
			if(readback->status == AnimationStatus::NotReady
				|| readback->state == MotionMatchGpuRequestState::Submitted
				|| readback->state == MotionMatchGpuRequestState::AwaitingFence) {
				return MotionMatchFallbackReason::GpuNotReady;
			}
			if(readback->status != AnimationStatus::Ok
				|| readback->state == MotionMatchGpuRequestState::Failed) {
				return MotionMatchFallbackReason::GpuBackendFailure;
			}
			if(readback->candidates.empty()
				|| readback->candidates.size() > query.maximumCandidates
				|| readback->candidates.size() > kMotionDatabaseMaximumTopK) {
				return MotionMatchFallbackReason::InvalidGpuCandidates;
			}
			return MotionMatchFallbackReason::None;
		}
	}

	AnimationStatus SelectMotionMatchAuthoritatively(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, const MotionMatchGpuReadback* gpuReadback,
		MotionMatchSelection& output) noexcept {
		MotionMatchSelection staged;
		staged.gpuStatus = gpuReadback == nullptr
			? AnimationStatus::RendererUnavailable : gpuReadback->status;
		staged.gpuCandidateCount = gpuReadback == nullptr ? 0U
			: static_cast<std::uint32_t>(gpuReadback->candidates.size());
		staged.fallbackReason = ClassifyGpuFallback(database, query, gpuReadback);

		try {
			if(staged.fallbackReason == MotionMatchFallbackReason::None) {
				std::vector<std::uint64_t> candidateIds;
				candidateIds.reserve(gpuReadback->candidates.size());
				for(const MotionMatchCandidate& candidate : gpuReadback->candidates) {
					candidateIds.push_back(candidate.stableSampleId);
				}
				MotionMatchCpuResult authoritative;
				const AnimationStatus subsetStatus = ScoreMotionMatchCpuSubset(
					database, query, candidateIds, authoritative);
				if(subsetStatus == AnimationStatus::Ok && !authoritative.candidates.empty()) {
					staged.authoritativeScores = std::move(authoritative);
					staged.chosen = staged.authoritativeScores.candidates.front();
					staged.usedGpuCandidates = true;
					staged.source = MotionMatchSelectionSource::GpuTopKCpuAuthoritative;
					staged.status = AnimationStatus::Ok;
					output = std::move(staged);
					return AnimationStatus::Ok;
				}
				// A matching generation that names invalid, duplicate, or newly excluded
				// IDs is not authoritative. Fall back to a complete CPU search and retain
				// the reason so backend corruption cannot become a silent mode change.
				staged.fallbackReason = MotionMatchFallbackReason::InvalidGpuCandidates;
			}

			MotionMatchCpuResult authoritative;
			const AnimationStatus status = ScoreMotionMatchCpu(database, query, authoritative);
			staged.authoritativeScores = std::move(authoritative);
			staged.usedCpuFallback = true;
			staged.source = MotionMatchSelectionSource::CpuFullSearch;
			staged.status = status;
			if(status == AnimationStatus::Ok && !staged.authoritativeScores.candidates.empty()) {
				staged.chosen = staged.authoritativeScores.candidates.front();
			} else if(staged.authoritativeScores.fallbackReason != MotionMatchFallbackReason::None) {
				staged.fallbackReason = staged.authoritativeScores.fallbackReason;
			}
			output = std::move(staged);
			return status;
		} catch(const std::bad_alloc&) {
			staged.status = AnimationStatus::InternalError;
			staged.usedCpuFallback = true;
			staged.source = MotionMatchSelectionSource::CpuFullSearch;
			output = std::move(staged);
			return output.status;
		} catch(...) {
			staged.status = AnimationStatus::InternalError;
			staged.usedCpuFallback = true;
			staged.source = MotionMatchSelectionSource::CpuFullSearch;
			output = std::move(staged);
			return output.status;
		}
	}
}
