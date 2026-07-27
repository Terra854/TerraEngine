// MotionMatchCpu.cpp - Integer-only authoritative scoring over cooked features.

#include "Animation/MotionMatching/MotionMatchCpu.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		bool IsExcluded(std::span<const std::uint64_t> excluded,
			std::uint64_t sampleId) noexcept {
			return std::binary_search(excluded.begin(), excluded.end(), sampleId);
		}

		std::uint64_t AbsDistance(std::int16_t left, std::int16_t right) noexcept {
			const int difference = static_cast<int>(left) - static_cast<int>(right);
			return static_cast<std::uint64_t>(difference < 0 ? -difference : difference);
		}

		std::uint32_t PhaseDistance(std::uint16_t left, std::uint16_t right) noexcept {
			const std::uint32_t direct = left >= right ? left - right : right - left;
			return std::min(direct, 65536U - direct);
		}

		AnimationStatus ValidateQuery(const MotionDatabaseAsset& database,
			const MotionMatchQuery& query) noexcept {
			if(!query.database.IsValid() || query.database.assetId != database.assetId
				|| query.database.generation != database.runtimeGeneration
				|| query.expectedSchemaFingerprint != database.featureSchemaFingerprint) {
				return AnimationStatus::StaleHandle;
			}
			if(query.maximumCandidates == 0U || query.maximumSamplesToVisit == 0U
				|| query.reserved != 0U
				|| (query.requiredTags & query.excludedTags) != 0U
				|| (query.requiredTransitionFlags & query.excludedTransitionFlags) != 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(query.maximumCandidates > kMotionDatabaseMaximumTopK
				|| query.maximumSamplesToVisit > kMotionDatabaseMaximumSamples
				|| query.maximumDiagnosticExclusions > kMotionMatchMaximumDiagnosticExclusions
				|| query.excludedSampleIds.size() > kMotionDatabaseMaximumTopK
				|| !std::is_sorted(query.excludedSampleIds.begin(), query.excludedSampleIds.end())) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		void CountExclusionReasons(MotionMatchExclusionReason reasons,
			MotionMatchCpuResult& result) noexcept {
			const std::uint32_t bits = static_cast<std::uint32_t>(reasons);
			for(std::size_t bit = 0U; bit < result.exclusionReasonCounts.size(); ++bit) {
				if((bits & (1U << bit)) != 0U) ++result.exclusionReasonCounts[bit];
			}
		}

		MotionMatchCandidate Score(const MotionDatabaseSample& sample,
			const MotionMatchQuery& query) noexcept {
			MotionMatchCandidate candidate;
			candidate.stableSampleId = sample.stableSampleId;
			candidate.sourceClipId = sample.sourceClipId;
			candidate.sourceTick = sample.sourceTick;
			for(std::size_t blockIndex = 0U;
				blockIndex < kMotionDatabaseDefaultFeatureBlocks.size(); ++blockIndex) {
				const MotionDatabaseFeatureBlock& block =
					kMotionDatabaseDefaultFeatureBlocks[blockIndex];
				for(std::size_t offset = 0U; offset < block.featureCount; ++offset) {
					const std::size_t feature = block.firstFeature + offset;
					const std::uint64_t cost = AbsDistance(sample.features.values[feature],
						query.features.values[feature]) * query.featureWeights[feature];
					candidate.score.perFeatureCosts[feature] = cost;
					candidate.score.featureBlockCosts[blockIndex] += cost;
					candidate.score.featureCost += cost;
				}
			}
			candidate.score.phaseCost = static_cast<std::uint64_t>(
				PhaseDistance(sample.phaseQuantized, query.preferredPhase)) * query.phaseWeight;
			candidate.score.tagCost = static_cast<std::uint64_t>(
				std::popcount(query.preferredTags & ~sample.tagMask))
				* query.missingPreferredTagPenalty;
			candidate.score.contactCost = static_cast<std::uint64_t>(
				std::popcount(query.preferredContacts ^ sample.contactMask))
				* query.contactMismatchPenalty;

			if(query.preferredContinuityGroup != 0U
				&& sample.continuityGroup != query.preferredContinuityGroup) {
				candidate.score.continuityCost += query.continuityPenalty;
			}
			if(query.lastSampleId != 0U && sample.stableSampleId != query.lastSampleId
				&& sample.continuityPredecessorId != query.lastSampleId
				&& sample.continuitySuccessorId != query.lastSampleId) {
				candidate.score.continuityCost += query.continuityBreakPenalty;
			}

			if(sample.stableSampleId != query.lastSampleId
				&& sample.sourceClipId == query.lastSourceClipId) {
				candidate.score.transitionCost += query.sameClipPenalty;
			}
			candidate.score.transitionCost += static_cast<std::uint64_t>(
				std::popcount(static_cast<std::uint16_t>(
					sample.transitionFlags ^ query.preferredTransitionFlags)))
				* query.transitionMismatchPenalty;
			candidate.score.totalCost = candidate.score.featureCost + candidate.score.phaseCost
				+ candidate.score.tagCost + candidate.score.contactCost
				+ candidate.score.continuityCost + candidate.score.transitionCost;
			return candidate;
		}

		void InsertCandidate(MotionMatchCpuResult& result, MotionMatchCandidate candidate,
			std::size_t maximumCandidates) {
			auto position = std::lower_bound(result.candidates.begin(), result.candidates.end(),
				candidate, IsMotionMatchCandidateBetter);
			if(position != result.candidates.end()
				|| result.candidates.size() < maximumCandidates) {
				result.candidates.insert(position, std::move(candidate));
				if(result.candidates.size() > maximumCandidates) result.candidates.pop_back();
			}
		}

		void RecordExclusion(const MotionDatabaseSample& sample,
			MotionMatchExclusionReason reasons, const MotionMatchQuery& query,
			MotionMatchCpuResult& result) {
			++result.excludedSamples;
			CountExclusionReasons(reasons, result);
			if(result.exclusionDetails.size() < query.maximumDiagnosticExclusions) {
				result.exclusionDetails.push_back({sample.stableSampleId, reasons});
			}
		}

		void FinalizeResult(bool emptyDatabase, MotionMatchCpuResult& result) noexcept {
			if(!result.candidates.empty()) {
				result.status = AnimationStatus::Ok;
				result.usedFallback = false;
				result.fallbackReason = MotionMatchFallbackReason::None;
				return;
			}
			result.status = AnimationStatus::NotReady;
			result.usedFallback = true;
			if(emptyDatabase) result.fallbackReason = MotionMatchFallbackReason::EmptyDatabase;
			else if(result.candidateBudgetExhausted) {
				result.fallbackReason = MotionMatchFallbackReason::CandidateBudgetExhausted;
			} else {
				result.fallbackReason = MotionMatchFallbackReason::NoEligibleCandidates;
			}
		}
	}

	MotionMatchExclusionReason ComputeMotionMatchExclusionReasons(
		const MotionDatabaseSample& sample, const MotionMatchQuery& query) noexcept {
		MotionMatchExclusionReason reasons = MotionMatchExclusionReason::None;
		if((sample.tagMask & query.requiredTags) != query.requiredTags) {
			reasons |= MotionMatchExclusionReason::MissingRequiredTags;
		}
		if((sample.tagMask & query.excludedTags) != 0U) {
			reasons |= MotionMatchExclusionReason::ContainsExcludedTags;
		}
		if((sample.contactMask & query.requiredContacts) != query.requiredContacts) {
			reasons |= MotionMatchExclusionReason::MissingRequiredContacts;
		}
		if(PhaseDistance(sample.phaseQuantized, query.preferredPhase)
			> query.maximumPhaseDistance) {
			reasons |= MotionMatchExclusionReason::PhaseOutsideWindow;
		}
		if(IsExcluded(query.excludedSampleIds, sample.stableSampleId)) {
			reasons |= MotionMatchExclusionReason::ExplicitSample;
		}
		if((sample.exclusionFlags & query.excludedRegionFlags) != 0U) {
			reasons |= MotionMatchExclusionReason::DatabaseExclusionRegion;
		}
		if((sample.transitionFlags & query.requiredTransitionFlags)
			!= query.requiredTransitionFlags) {
			reasons |= MotionMatchExclusionReason::MissingRequiredTransitionFlags;
		}
		if((sample.transitionFlags & query.excludedTransitionFlags) != 0U) {
			reasons |= MotionMatchExclusionReason::ContainsExcludedTransitionFlags;
		}
		return reasons;
	}

	bool IsMotionMatchCandidateBetter(const MotionMatchCandidate& left,
		const MotionMatchCandidate& right) noexcept {
		if(left.score.totalCost != right.score.totalCost) {
			return left.score.totalCost < right.score.totalCost;
		}
		if(left.sourceClipId != right.sourceClipId) {
			return left.sourceClipId < right.sourceClipId;
		}
		if(left.sourceTick != right.sourceTick) {
			return left.sourceTick < right.sourceTick;
		}
		return left.stableSampleId < right.stableSampleId;
	}

	AnimationStatus ScoreMotionMatchCpu(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, MotionMatchCpuResult& output) noexcept {
		MotionMatchCpuResult staged;
		if(const AnimationStatus status = ValidateMotionDatabaseAsset(database);
			status != AnimationStatus::Ok) {
			staged.status = status;
			output = std::move(staged);
			return status;
		}
		if(const AnimationStatus status = ValidateQuery(database, query);
			status != AnimationStatus::Ok) {
			staged.status = status;
			output = std::move(staged);
			return status;
		}
		try {
			staged.candidates.reserve(std::min(query.maximumCandidates, database.samples.size()));
			staged.exclusionDetails.reserve(std::min(
				query.maximumDiagnosticExclusions, database.samples.size()));
			const std::size_t visitCount = std::min(
				query.maximumSamplesToVisit, database.samples.size());
			staged.candidateBudgetExhausted = visitCount < database.samples.size();
			for(std::size_t index = 0U; index < visitCount; ++index) {
				const MotionDatabaseSample& sample = database.samples[index];
				++staged.visitedSamples;
				const MotionMatchExclusionReason reasons =
					ComputeMotionMatchExclusionReasons(sample, query);
				if(reasons != MotionMatchExclusionReason::None) {
					RecordExclusion(sample, reasons, query, staged);
					continue;
				}
				++staged.evaluatedSamples;
				InsertCandidate(staged, Score(sample, query), query.maximumCandidates);
			}
			FinalizeResult(database.samples.empty(), staged);
			output = std::move(staged);
			return output.status;
		} catch(const std::bad_alloc&) {
			staged.status = AnimationStatus::InternalError;
			output = std::move(staged);
			return output.status;
		} catch(...) {
			staged.status = AnimationStatus::InternalError;
			output = std::move(staged);
			return output.status;
		}
	}

	AnimationStatus ScoreMotionMatchCpuSubset(const MotionDatabaseAsset& database,
		const MotionMatchQuery& query, std::span<const std::uint64_t> candidateIds,
		MotionMatchCpuResult& output) noexcept {
		MotionMatchCpuResult staged;
		if(const AnimationStatus status = ValidateMotionDatabaseAsset(database);
			status != AnimationStatus::Ok) {
			staged.status = status;
			output = std::move(staged);
			return status;
		}
		if(const AnimationStatus status = ValidateQuery(database, query);
			status != AnimationStatus::Ok) {
			staged.status = status;
			output = std::move(staged);
			return status;
		}
		if(candidateIds.empty() || candidateIds.size() > query.maximumCandidates
			|| candidateIds.size() > kMotionDatabaseMaximumTopK) {
			staged.status = AnimationStatus::InvalidArgument;
			output = std::move(staged);
			return output.status;
		}
		try {
			std::vector<std::uint64_t> ordered(candidateIds.begin(), candidateIds.end());
			std::sort(ordered.begin(), ordered.end());
			if(std::adjacent_find(ordered.begin(), ordered.end()) != ordered.end()) {
				staged.status = AnimationStatus::InvalidArgument;
				output = std::move(staged);
				return output.status;
			}
			staged.candidates.reserve(ordered.size());
			staged.exclusionDetails.reserve(std::min(
				query.maximumDiagnosticExclusions, ordered.size()));
			for(const std::uint64_t sampleId : ordered) {
				const auto found = std::lower_bound(database.samples.begin(), database.samples.end(), sampleId,
					[](const MotionDatabaseSample& sample, std::uint64_t id) {
						return sample.stableSampleId < id;
					});
				if(found == database.samples.end() || found->stableSampleId != sampleId) {
					staged.status = AnimationStatus::StaleHandle;
					output = std::move(staged);
					return output.status;
				}
				++staged.visitedSamples;
				const MotionMatchExclusionReason reasons =
					ComputeMotionMatchExclusionReasons(*found, query);
				if(reasons != MotionMatchExclusionReason::None) {
					RecordExclusion(*found, reasons, query, staged);
					continue;
				}
				++staged.evaluatedSamples;
				InsertCandidate(staged, Score(*found, query), query.maximumCandidates);
			}
			FinalizeResult(false, staged);
			output = std::move(staged);
			return output.status;
		} catch(const std::bad_alloc&) {
			staged.status = AnimationStatus::InternalError;
			output = std::move(staged);
			return output.status;
		} catch(...) {
			staged.status = AnimationStatus::InternalError;
			output = std::move(staged);
			return output.status;
		}
	}

	std::string_view ToString(MotionMatchFallbackReason reason) noexcept {
		switch(reason) {
		case MotionMatchFallbackReason::None: return "None";
		case MotionMatchFallbackReason::EmptyDatabase: return "EmptyDatabase";
		case MotionMatchFallbackReason::NoEligibleCandidates: return "NoEligibleCandidates";
		case MotionMatchFallbackReason::CandidateBudgetExhausted: return "CandidateBudgetExhausted";
		case MotionMatchFallbackReason::GpuUnavailable: return "GpuUnavailable";
		case MotionMatchFallbackReason::GpuNotReady: return "GpuNotReady";
		case MotionMatchFallbackReason::GpuCancelled: return "GpuCancelled";
		case MotionMatchFallbackReason::StaleGpuResult: return "StaleGpuResult";
		case MotionMatchFallbackReason::GpuBackendFailure: return "GpuBackendFailure";
		case MotionMatchFallbackReason::InvalidGpuCandidates: return "InvalidGpuCandidates";
		}
		return "Unknown";
	}
}
