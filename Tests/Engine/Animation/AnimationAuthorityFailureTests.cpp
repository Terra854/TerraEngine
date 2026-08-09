// AnimationAuthorityFailureTests.cpp - Fail-closed authority sealing contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationAuthorityPublication.h"

#include <array>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		std::shared_ptr<const AnimationAuthorityPage> MakePage(
			std::uint32_t slot, std::uint64_t generation)
		{
			auto page = std::make_shared<AnimationAuthorityPage>();
			page->firstSlot = (slot / static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128))
				* static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
			page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
			page->changedMasks.assign(2U, 0U);
			page->changedMasks[(slot - page->firstSlot) / 64U] = 1ULL
				<< ((slot - page->firstSlot) % 64U);
			AnimationAuthorityRow row;
			row.key = {slot, 1U, 9U};
			row.ownerObjectId[0U] = static_cast<std::uint8_t>(slot + 1U);
			row.componentGeneration = 1U;
			row.sourceGeneration = generation;
			row.poseGeneration = generation;
			row.authorityPublicationGeneration = generation;
			row.active = true;
			page->rows.push_back(std::move(row));
			return page;
		}

		SealedAnimationFrameResult Seal(
			std::uint64_t previous, std::uint64_t candidate,
			const std::vector<std::shared_ptr<const AnimationAuthorityPage>>& pages,
			std::shared_ptr<const AnimationFrameSidecars> sidecars)
		{
			AnimationAuthoritySealInput input;
			input.frameId = candidate;
			input.sessionGeneration = 9U;
			input.lifecycleGeneration = 4U;
			input.expectedPreviousGeneration = previous;
			input.candidateGeneration = candidate;
			input.pages = pages;
			input.sidecars = std::move(sidecars);
			input.workersComplete = true;
			input.historyReserved = true;
			return SealAnimationFrameResult(input);
		}

		SealedAnimationFrameResult ValidSeal(std::uint64_t previous, std::uint64_t candidate)
		{
			return Seal(previous, candidate, {MakePage(3U, candidate)},
				std::make_shared<AnimationFrameSidecars>());
		}
	}

	void RegisterAnimationAuthorityFailureTests(TestRegistry& registry)
	{
		registry.Add("Animation.AuthorityFailure.InjectableSealRejectionsAreAtomic",
			[](TestContext& context) {
			using namespace TerraEngine::Animation;
			constexpr std::array outcomes{
				AnimationAuthorityPublicationOutcome::RejectedAllocation,
				AnimationAuthorityPublicationOutcome::RejectedHistoryReservation,
				AnimationAuthorityPublicationOutcome::RejectedSidecars,
				AnimationAuthorityPublicationOutcome::RejectedMissingWorker};
			for(const AnimationAuthorityPublicationOutcome outcome : outcomes) {
				SetAnimationAuthorityFailureInjectionForTesting(outcome);
				const SealedAnimationFrameResult rejected = SealAnimationFrameResult({});
				context.Expect(!rejected.IsValid() && rejected.frame == nullptr
					&& rejected.receipt.outcome == outcome,
					"Every injected worker-side sealing failure must reject before it can create a visible authority root.");
			}
			ClearAnimationAuthorityFailureInjectionForTesting();
		});

		registry.Add("Animation.AuthorityFailure.CancelledCandidateHasNoReceiptOfPublication",
			[](TestContext& context) {
				using namespace TerraEngine::Animation;
				AnimationAuthoritySealInput input;
				input.cancelled = true;
				const SealedAnimationFrameResult rejected = SealAnimationFrameResult(input);
				context.Expect(!rejected.IsValid() && rejected.frame == nullptr
					&& rejected.receipt.outcome
						== AnimationAuthorityPublicationOutcome::RejectedCancelled,
					"Cancellation must fail closed without manufacturing a partially sealed authority frame.");
			});

		registry.Add("Animation.AuthorityFailure.OutOfOrderCompletionRetainsAcceptedRoot",
			[](TestContext& context) {
				using namespace TerraEngine::Animation;
				AnimationAuthorityPublisher publisher;
				const AnimationPublicationReceipt first = publisher.Publish(ValidSeal(0U, 1U), 9U, 4U);
				const AnimationPublicationReceipt second = publisher.Publish(ValidSeal(1U, 2U), 9U, 4U);
				const std::shared_ptr<const AnimationAuthorityFrame> accepted = publisher.Load();
				// This result sealed against generation one but completed only after the
				// generation-two root was accepted. It must remain entirely invisible.
				const AnimationPublicationReceipt late = publisher.Publish(ValidSeal(1U, 3U), 9U, 4U);
				context.Expect(first.Published() && second.Published()
					&& late.outcome == AnimationAuthorityPublicationOutcome::RejectedStalePredecessor
					&& publisher.Load() == accepted && accepted->authorityGeneration == 2U,
					"An out-of-order sealed completion must not replace the newer immutable authority root.");
			});

		registry.Add("Animation.AuthorityFailure.DuplicateSlotAndRejectedSidecarAreUnobservable",
			[](TestContext& context) {
				using namespace TerraEngine::Animation;
				AnimationAuthorityPublisher publisher;
				const AnimationPublicationReceipt published = publisher.Publish(ValidSeal(0U, 1U), 9U, 4U);
				const std::shared_ptr<const AnimationAuthorityFrame> accepted = publisher.Load();

				// A slot is a generation-validated identity, not an append-only row key.
				// Repeating it across pages would make Find() depend on page traversal order.
				const SealedAnimationFrameResult duplicate = Seal(1U, 2U,
					{MakePage(3U, 2U), MakePage(3U, 2U)},
					std::make_shared<AnimationFrameSidecars>());

				auto staleSidecars = std::make_shared<AnimationFrameSidecars>();
				staleSidecars->callbacks.push_back({{3U, 1U, 9U}, 1U, 11U,
					AnimationStatus::Ok, 1U, 0U});
				const SealedAnimationFrameResult staleSidecar = Seal(1U, 2U,
					{MakePage(3U, 2U)}, staleSidecars);
				const AnimationPublicationReceipt sidecarReceipt = publisher.Publish(staleSidecar, 9U, 4U);

				context.Expect(published.Published() && accepted
					&& !duplicate.IsValid()
					&& duplicate.receipt.outcome
						== AnimationAuthorityPublicationOutcome::RejectedDuplicateSlot
					&& !staleSidecar.IsValid()
					&& staleSidecar.receipt.outcome
						== AnimationAuthorityPublicationOutcome::RejectedSidecars
					&& sidecarReceipt.outcome
						== AnimationAuthorityPublicationOutcome::RejectedInvalidCandidate
					&& publisher.Load() == accepted,
					"Duplicate rows or stale sidecars must fail while sealing and leave the accepted root untouched.");
			});
	}
}
