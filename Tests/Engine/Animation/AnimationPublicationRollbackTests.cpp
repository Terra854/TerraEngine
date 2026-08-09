#include "TestHarness.h"

#include "Animation/Runtime/AnimationAuthorityPublication.h"

#include <memory>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Animation;

		SealedAnimationFrameResult Seal(std::uint64_t previousGeneration,
			std::uint64_t candidateGeneration, std::uint64_t lifecycleGeneration = 7U)
		{
			AnimationAuthoritySealInput input;
			input.frameId = candidateGeneration;
			input.sessionGeneration = 5U;
			input.lifecycleGeneration = lifecycleGeneration;
			input.expectedPreviousGeneration = previousGeneration;
			input.candidateGeneration = candidateGeneration;
			input.sidecars = std::make_shared<AnimationFrameSidecars>();
			input.workersComplete = true;
			input.historyReserved = true;
			// An empty page list is a valid no-animator authority root; it gives this
			// test a compact way to exercise the production atomic exchange path.
			return SealAnimationFrameResult(input);
		}
	}

	void RegisterAnimationPublicationRollbackTests(TestRegistry& registry)
	{
		registry.Add("Animation.PublicationRollback.StaleRestorePreservesExactAuthorityHistory", [](TestContext& context)
		{
			AnimationAuthorityPublisher publisher;
			const SealedAnimationFrameResult first = Seal(0U, 41U);
			const AnimationPublicationReceipt firstReceipt = publisher.Publish(first, 5U, 7U);
			const std::shared_ptr<const AnimationAuthorityFrame> retained = publisher.Load();
			const SealedAnimationFrameResult staleRestore = Seal(0U, 42U);
			const AnimationPublicationReceipt rejection = publisher.Publish(staleRestore, 5U, 7U);
			const std::shared_ptr<const AnimationAuthorityFrame> afterRejection = publisher.Load();

			context.Expect(firstReceipt.Published() && retained != nullptr
				&& rejection.outcome == AnimationAuthorityPublicationOutcome::RejectedStalePredecessor
				&& rejection.expectedPreviousGeneration == 0U && rejection.observedPreviousGeneration == 41U
				&& afterRejection == retained && afterRejection->authorityGeneration == 41U
				&& afterRejection->frameId == retained->frameId
				&& publisher.LastReceipt().outcome == rejection.outcome,
				"A rejected restore must retain the exact last authority root and its generation history.");
		});

		registry.Add("Animation.PublicationRollback.LifecycleRejectedRestoreDoesNotAdvanceRoot", [](TestContext& context)
		{
			AnimationAuthorityPublisher publisher;
			(void)publisher.Publish(Seal(0U, 11U), 5U, 7U);
			const std::shared_ptr<const AnimationAuthorityFrame> retained = publisher.Load();
			const AnimationPublicationReceipt rejection = publisher.Publish(Seal(11U, 12U, 8U), 5U, 7U);
			const std::shared_ptr<const AnimationAuthorityFrame> afterRejection = publisher.Load();

			context.Expect(retained != nullptr
				&& rejection.outcome == AnimationAuthorityPublicationOutcome::RejectedLifecycle
				&& rejection.observedPreviousGeneration == 11U
				&& afterRejection == retained && afterRejection->authorityGeneration == 11U,
				"A lifecycle-mismatched restore must not advance or replace the retained authority history.");
		});
	}
}
