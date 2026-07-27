// MotionMatchCpuTests.cpp - Authoritative scoring, exclusions, and tie-breaking.

#include "TestHarness.h"

#include "Animation/MotionMatching/MotionMatchCpu.h"

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;
		AnimationGuid Guid(std::uint8_t value) { AnimationGuid result; result.value[0] = value; return result; }
		MotionDatabaseAsset Database() {
			MotionDatabaseAsset result;
			result.assetId = Guid(1U); result.runtimeGeneration = 2U;
			result.compatibilityFingerprint = 3U; result.sourceFingerprint = 4U;
			result.quantizationScales.fill(1.0F);
			for(std::uint64_t id = 1U; id <= 3U; ++id) {
				MotionDatabaseSample sample;
				sample.stableSampleId = id; sample.sourceClipId = id == 2U ? 7U : 6U;
				sample.sourceTick = id == 3U ? 5 : 10; sample.tagMask = id == 2U ? 2U : 1U;
				sample.contactMask = id == 3U ? 2U : 1U; sample.phaseQuantized = 100U;
				sample.continuityGroup = id == 1U ? 9U : 8U;
				sample.transitionFlags = static_cast<std::uint16_t>(1U << (id - 1U));
				if(id == 3U) sample.exclusionFlags = 8U;
				result.samples.push_back(sample);
			}
			result.featureSchemaFingerprint = ComputeMotionDatabaseSchemaFingerprint(result);
			return result;
		}
		MotionMatchQuery Query(const MotionDatabaseAsset& database) {
			MotionMatchQuery query; query.database = {database.assetId, database.runtimeGeneration};
			query.expectedSchemaFingerprint = database.featureSchemaFingerprint; query.featureWeights.fill(1U);
			query.requiredTags = 1U; query.requiredContacts = 1U; query.preferredContinuityGroup = 9U;
			return query;
		}
	}

	void RegisterMotionMatchCpuTests(TestRegistry& registry) {
		registry.Add("Animation.MotionMatch.CpuScoresExclusionsCostsContinuityAndStableTieBreak",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database();
				MotionMatchQuery query = Query(database);
				MotionMatchCpuResult result;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::Ok
					&& result.candidates.size() == 1U && result.candidates.front().stableSampleId == 1U
					&& result.excludedSamples == 2U,
					"Hard tags and contacts must exclude ineligible samples before integer feature and continuity costs are ranked.");
				query.requiredContacts = 0U; query.preferredContinuityGroup = 0U;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::Ok
					&& result.candidates.front().stableSampleId == 3U,
					"Equal effective scores must order by clip, tick, and stable sample ID independent of authored insertion order.");
			});

		registry.Add("Animation.MotionMatch.CpuRejectsStaleSchemaAndUsesBoundedFallback",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database();
				MotionMatchQuery query = Query(database);
				MotionMatchCpuResult result;
				++query.expectedSchemaFingerprint;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::StaleHandle,
					"A database hot reload/schema mismatch must reject before a candidate can cross generations.");
				query = Query(database); query.requiredTags = 0x80U;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::NotReady && result.usedFallback,
					"An empty eligible set must report the configured fallback state instead of expanding search bounds.");
			});

		registry.Add("Animation.MotionMatch.CpuExplainsTransitionExclusionsAndComponentCosts",
			[](TestContext& context) {
				MotionDatabaseAsset database = Database();
				database.samples[0].features.values[0] = 10;
				MotionMatchQuery query = Query(database);
				query.requiredContacts = 0U;
				query.requiredTransitionFlags = 1U;
				query.excludedRegionFlags = 8U;
				query.features.values[0] = 5;
				query.preferredPhase = 100U;
				query.featureWeights.fill(0U);
				query.featureWeights[0] = 2U;
				query.preferredTransitionFlags = 3U;
				query.transitionMismatchPenalty = 7U;
				query.lastSampleId = 99U;
				query.lastSourceClipId = 6U;
				query.sameClipPenalty = 5U;
				MotionMatchCpuResult result;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::Ok
					&& result.candidates.size() == 1U
					&& result.candidates.front().stableSampleId == 1U
					&& result.candidates.front().score.perFeatureCosts[0] == 10U
					&& result.candidates.front().score.FeatureBlockCost(
						MotionDatabaseFeatureSemantic::Trajectory) == 10U
					&& result.candidates.front().score.transitionCost == 12U
					&& result.candidates.front().score.totalCost == 22U,
					"The CPU oracle must expose exact per-feature/block and transition component costs.");
				context.Expect(result.excludedSamples == 2U
					&& result.exclusionReasonCounts[6] == 2U
					&& result.exclusionReasonCounts[5] == 1U
					&& result.exclusionDetails.size() == 2U,
					"Every rejected sample must retain bounded, inspectable hard-exclusion reasons.");
			});

		registry.Add("Animation.MotionMatch.CpuCandidateBudgetAndSubsetSelectionAreDeterministic",
			[](TestContext& context) {
				const MotionDatabaseAsset database = Database();
				MotionMatchQuery query = Query(database);
				query.requiredTags = 2U;
				query.maximumSamplesToVisit = 1U;
				MotionMatchCpuResult result;
				context.Expect(ScoreMotionMatchCpu(database, query, result) == AnimationStatus::NotReady
					&& result.candidateBudgetExhausted
					&& result.fallbackReason == MotionMatchFallbackReason::CandidateBudgetExhausted,
					"A configured visit budget must stop deterministically and report its bounded fallback reason.");

				query = Query(database);
				query.requiredContacts = 0U;
				query.preferredContinuityGroup = 0U;
				const std::array<std::uint64_t, 2U> gpuIds{3U, 1U};
				context.Expect(ScoreMotionMatchCpuSubset(database, query, gpuIds, result)
						== AnimationStatus::Ok
					&& result.visitedSamples == gpuIds.size()
					&& result.candidates.front().stableSampleId == 3U,
					"CPU authority must re-score an unordered top-K ID set and apply the same stable tie-break.");
			});
	}
}
