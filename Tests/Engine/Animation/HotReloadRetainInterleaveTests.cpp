// Qualify independent retain bookkeeping without weakening staged-table safety.
#include "TestHarness.h"
#include "Animation/Streaming/AnimationHotReload.h"

#include <array>

namespace TerraEngine::Tests {
	namespace {
		Animation::AnimationReloadCandidate ReloadCandidate(std::uint8_t seed, std::uint64_t generation) {
			Animation::AnimationReloadCandidate candidate;
			candidate.assetId[0] = seed;
			candidate.generation = generation;
			candidate.compatibilityFingerprint = 0x102U;
			candidate.contentFingerprint = 101U * generation;
			candidate.parsed = true;
			candidate.dependenciesReady = true;
			return candidate;
		}
	}
	void RegisterHotReloadRetainInterleaveTests(TestRegistry& registry) {
		registry.Add("Animation.Review.HotReload.RetainDoesNotInvalidateIndependentPreparation", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const auto first = ReloadCandidate(31U, 1U);
			context.Expect(publisher.Publish(first).published, "The first complete generation must publish.");
			auto prepared = publisher.PrepareBatch(std::array{ReloadCandidate(31U, 2U)}, 2U);
			context.Expect(prepared.Accepted() && publisher.RetainGeneration(first.assetId, 1U), "A retained old owner must interleave after accepted preparation.");
			context.Expect(publisher.CommitPreparedBatch(prepared) == Animation::AnimationReloadCommitStatus::Committed,
				"A retain map which preparation does not stage must not invalidate its independent publication.");
			context.Expect(publisher.Query(first.assetId).activeGeneration == 2U && publisher.RetireBefore(2U) == 0U,
				"Successful prepared publication must preserve the live old-generation retain.");
			context.Expect(publisher.ReleaseGeneration(first.assetId, 1U) && publisher.RetireBefore(2U) == 1U,
				"The old generation may retire only after its actual owner releases it.");
		});
		registry.Add("Animation.Review.HotReload.ReleaseDoesNotInvalidateIndependentPreparation", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const auto first = ReloadCandidate(32U, 1U);
			context.Expect(publisher.Publish(first).published && publisher.RetainGeneration(first.assetId, 1U), "The initial generation needs one live retain.");
			auto prepared = publisher.PrepareBatch(std::array{ReloadCandidate(32U, 2U)}, 2U);
			context.Expect(prepared.Accepted() && publisher.ReleaseGeneration(first.assetId, 1U), "Release must occur between actual prepare and commit.");
			context.Expect(publisher.CommitPreparedBatch(prepared) == Animation::AnimationReloadCommitStatus::Committed
				&& publisher.RetireBefore(2U) == 1U, "Independent release must preserve publication while allowing safe later retirement.");
		});
		registry.Add("Animation.Review.HotReload.UnrelatedRetainRoundTripPreservesPreparedBatch", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const auto first = ReloadCandidate(33U, 1U), other = ReloadCandidate(34U, 1U);
			context.Expect(publisher.PublishBatch(std::array{first, other}, 1U)[0].published, "Both independent assets must publish initially.");
			auto prepared = publisher.PrepareBatch(std::array{ReloadCandidate(33U, 2U)}, 2U);
			context.Expect(prepared.Accepted() && publisher.RetainGeneration(other.assetId, 1U)
				&& publisher.ReleaseGeneration(other.assetId, 1U), "An unrelated owner must perform a complete retain round trip.");
			context.Expect(publisher.CommitPreparedBatch(prepared) == Animation::AnimationReloadCommitStatus::Committed
				&& publisher.Query(other.assetId).activeGeneration == 1U, "Independent bookkeeping must neither stale nor overwrite unrelated live state.");
		});
		registry.Add("Animation.Review.HotReload.StagedInstanceChangeStillRejectsPreparation", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const auto first = ReloadCandidate(35U, 1U);
			context.Expect(publisher.Publish(first).published, "The staged-table control needs a published asset.");
			auto prepared = publisher.PrepareBatch(std::array{ReloadCandidate(35U, 2U)}, 2U);
			UUID instance{}; instance[0] = 36U;
			context.Expect(publisher.BindInstance(instance, first.assetId, 1U), "A live binding must change after preparation.");
			context.Expect(publisher.CommitPreparedBatch(prepared) == Animation::AnimationReloadCommitStatus::RejectedStalePreparation
				&& publisher.Query(first.assetId).activeGeneration == 1U && publisher.QueryInstance(instance)->generation == 1U,
				"Staged-table mutation must still reject without erasing its intervening live binding.");
		});
		registry.Add("Animation.Review.HotReload.RetirementStillRejectsPreparation", [](TestContext& context) {
			Animation::AnimationHotReloadPublisher publisher;
			const auto first = ReloadCandidate(37U, 1U);
			context.Expect(publisher.Publish(first).published && publisher.Publish(ReloadCandidate(37U, 2U)).published,
				"The retirement control needs one replaced generation.");
			auto prepared = publisher.PrepareBatch(std::array{ReloadCandidate(37U, 3U)}, 3U);
			context.Expect(prepared.Accepted() && publisher.RetireBefore(2U) == 1U, "Retirement must change the staged pending-retirement table.");
			context.Expect(publisher.CommitPreparedBatch(prepared) == Animation::AnimationReloadCommitStatus::RejectedStalePreparation
				&& publisher.Query(first.assetId).activeGeneration == 2U && publisher.RetireBefore(2U) == 0U,
				"Independent retain qualification must not resurrect already retired state.");
		});
	}
}
