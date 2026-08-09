// AnimationAuthorityPublicationContractTests.cpp - Immutable authority root contracts.

#include "TestHarness.h"

#include "Animation/Runtime/AnimationAuthorityPublication.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		using namespace TerraEngine::Animation;

		std::shared_ptr<const AnimationAuthorityPage> MakePage(
			std::uint64_t authorityGeneration, std::uint64_t sessionGeneration)
		{
			auto pose = std::make_shared<PoseBuffer>();
			AnimationPoseGeneration generation;
			generation.animator = {3U, 7U};
			generation.poseGeneration = 11U;
			generation.tick = 42;
			if(pose->BeginWrite({}, generation) != AnimationStatus::Ok
				|| pose->Seal() != AnimationStatus::Ok) return {};
			auto snapshot = std::make_shared<AnimationSnapshot>();
			snapshot->animator = generation.animator;
			snapshot->publicationGeneration = generation.poseGeneration;
			snapshot->tick = generation.tick;
			snapshot->pose = std::move(pose);
			auto page = std::make_shared<AnimationAuthorityPage>();
			page->firstSlot = 0U;
			page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
			page->changedMasks.assign(2U, 1U << 3U);
			AnimationAuthorityRow row;
			row.key = {3U, 7U, sessionGeneration};
			row.ownerObjectId = TerraEngine::UUID{1U};
			row.componentGeneration = 1U;
			row.sourceGeneration = authorityGeneration;
			row.poseGeneration = generation.poseGeneration;
			row.tick = generation.tick;
			row.stateId = 1U;
			row.snapshot = std::move(snapshot);
			row.active = true;
			page->rows.push_back(std::move(row));
			return page;
		}

		SealedAnimationFrameResult MakeSealed(std::uint64_t previous,
			std::uint64_t candidate, std::uint64_t session = 9U)
		{
			const auto page = MakePage(candidate, session);
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			sidecars->summary.evaluatedRows = 1U;
			std::vector<std::shared_ptr<const AnimationAuthorityPage>> pages{page};
			AnimationAuthoritySealInput input;
			input.frameId = candidate;
			input.sessionGeneration = session;
			input.lifecycleGeneration = 4U;
			input.expectedPreviousGeneration = previous;
			input.candidateGeneration = candidate;
			input.pages = pages;
			input.sidecars = std::move(sidecars);
			input.workersComplete = true;
			input.historyReserved = true;
			return SealAnimationFrameResult(input);
		}

		SealedAnimationFrameResult SealPages(std::uint64_t previous,
			std::uint64_t candidate,
			const std::vector<std::shared_ptr<const AnimationAuthorityPage>>& pages,
			std::uint32_t reusedPages, std::uint32_t changedPages)
		{
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			sidecars->summary.evaluatedRows = 2U;
			AnimationAuthoritySealInput input;
			input.frameId = candidate;
			input.sessionGeneration = 9U;
			input.lifecycleGeneration = 4U;
			input.expectedPreviousGeneration = previous;
			input.candidateGeneration = candidate;
			input.pages = pages;
			input.sidecars = std::move(sidecars);
			input.reusedPages = reusedPages;
			input.changedPages = changedPages;
			input.workersComplete = true;
			input.historyReserved = true;
			return SealAnimationFrameResult(input);
		}

		SealedAnimationFrameResult MakeFourThousandRowCandidate()
		{
			constexpr std::size_t rowCount = 4000U;
			constexpr std::size_t pageCount = (rowCount + kAnimationAuthorityPageCapacity128 - 1U)
				/ kAnimationAuthorityPageCapacity128;
			std::vector<std::shared_ptr<const AnimationAuthorityPage>> pages;
			pages.reserve(pageCount);
			for(std::size_t pageIndex = 0U; pageIndex < pageCount; ++pageIndex) {
				auto page = std::make_shared<AnimationAuthorityPage>();
				page->firstSlot = static_cast<std::uint32_t>(pageIndex * kAnimationAuthorityPageCapacity128);
				page->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
				page->changedMasks.assign(2U, 0U);
				const std::size_t end = std::min(rowCount,
					(pageIndex + 1U) * kAnimationAuthorityPageCapacity128);
				for(std::size_t slot = page->firstSlot; slot < end; ++slot) {
					AnimationAuthorityRow row;
					row.key = {static_cast<std::uint32_t>(slot), 1U, 9U};
					row.ownerObjectId[0U] = static_cast<std::uint8_t>((slot % 255U) + 1U);
					row.ownerObjectId[1U] = static_cast<std::uint8_t>((slot / 255U) + 1U);
					row.componentGeneration = 1U;
					row.sourceGeneration = 77U;
					row.poseGeneration = static_cast<std::uint64_t>(slot) + 1U;
					row.authorityPublicationGeneration = 77U;
					row.effectiveExecutionMode = slot % 2U == 0U
						? AnimationEffectiveExecutionMode::GpuVisual
						: AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					row.safeFallback = row.effectiveExecutionMode
						== AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					row.active = true;
					page->rows.push_back(std::move(row));
				}
				pages.push_back(std::move(page));
			}
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			sidecars->summary.evaluatedRows = static_cast<std::uint32_t>(rowCount);
			AnimationAuthoritySealInput input;
			input.frameId = 77U;
			input.sessionGeneration = 9U;
			input.lifecycleGeneration = 4U;
			input.candidateGeneration = 77U;
			input.pages = pages;
			input.sidecars = std::move(sidecars);
			input.workersComplete = true;
			input.historyReserved = true;
			return SealAnimationFrameResult(input);
		}
	}

	void RegisterAnimationAuthorityPublicationContractTests(TestRegistry& registry)
	{
		registry.Add("Animation.AuthorityPublication.SealSharesImmutablePages", [](TestContext& context) {
			const SealedAnimationFrameResult sealed = MakeSealed(0U, 1U);
			context.Expect(sealed.IsValid() && sealed.frame && sealed.frame->pages
				&& sealed.frame->Find({3U, 7U, 9U}) != nullptr,
				"Sealing must retain immutable pages and expose rows only through the authority root.");
		});

		registry.Add("Animation.AuthorityPublication.RejectsMissingSealedValidationProof",
			[](TestContext& context) {
				SealedAnimationFrameResult candidate = MakeSealed(0U, 1U);
				candidate.sealedValidationComplete = false;
				AnimationAuthorityPublisher publisher;
				const AnimationPublicationReceipt receipt = publisher.Publish(
					candidate, 9U, 4U);
				context.Expect(!candidate.IsValid() && !receipt.Published()
					&& publisher.Load() == nullptr,
					"Protected publication must reject a candidate that did not complete the immutable page-and-sidecar validation pass.");
			});

		registry.Add("Animation.AuthorityPublication.SealTransfersOwnedSidecars", [](TestContext& context) {
			const auto page = MakePage(1U, 9U);
			auto sidecars = std::make_shared<AnimationFrameSidecars>();
			sidecars->summary.evaluatedRows = 1U;
			const AnimationFrameSidecars* expectedIdentity = sidecars.get();
			std::vector<std::shared_ptr<const AnimationAuthorityPage>> pages{page};
			AnimationAuthoritySealInput input;
			input.frameId = 1U;
			input.sessionGeneration = 9U;
			input.lifecycleGeneration = 4U;
			input.candidateGeneration = 1U;
			input.pages = pages;
			input.ownedSidecars = std::move(sidecars);
			input.workersComplete = true;
			input.historyReserved = true;

			const SealedAnimationFrameResult sealed = SealAnimationFrameResult(input);
			context.Expect(sealed.IsValid() && sealed.frame
				&& sealed.frame->sidecars.get() == expectedIdentity,
				"A worker-owned sidecar root must become immutable without deep-copying its crowd-sized records.");
		});

		registry.Add("Animation.AuthorityPublication.ReusesUnchangedPageIdentity", [](TestContext& context) {
			const std::shared_ptr<const AnimationAuthorityPage> page = MakePage(1U, 9U);
			const SealedAnimationFrameResult first = MakeSealed(0U, 1U, 9U);
			AnimationAuthoritySealInput input;
			input.frameId = 2U;
			input.sessionGeneration = 9U;
			input.lifecycleGeneration = 4U;
			input.expectedPreviousGeneration = 1U;
			input.candidateGeneration = 2U;
			input.pages = std::span<const std::shared_ptr<const AnimationAuthorityPage>>(&page, 1U);
			input.sidecars = std::make_shared<AnimationFrameSidecars>();
			input.workersComplete = true;
			input.historyReserved = true;
			const SealedAnimationFrameResult second = SealAnimationFrameResult(input);
			context.Expect(first.IsValid() && second.IsValid() && second.frame->pages
				&& second.frame->pages->front() == page,
				"A sealed frame must retain an unchanged immutable page by shared identity rather than cloning its rows.");
		});

		registry.Add("Animation.AuthorityPublication.OneChangedRowReplacesOnlyItsPage",
			[](TestContext& context) {
				const auto firstChangedPage = MakePage(1U, 9U);
				auto unchangedPage = std::make_shared<AnimationAuthorityPage>();
				unchangedPage->firstSlot = 128U;
				unchangedPage->capacity = static_cast<std::uint32_t>(kAnimationAuthorityPageCapacity128);
				unchangedPage->changedMasks.assign(2U, 0U);
				AnimationAuthorityRow unchangedRow;
				unchangedRow.key = {129U, 8U, 9U};
				unchangedRow.ownerObjectId[0U] = 2U;
				unchangedRow.componentGeneration = 1U;
				unchangedRow.sourceGeneration = 1U;
				unchangedRow.poseGeneration = 1U;
				unchangedRow.active = true;
				unchangedPage->rows.push_back(std::move(unchangedRow));
				const std::shared_ptr<const AnimationAuthorityPage> retainedPage = unchangedPage;

				const SealedAnimationFrameResult first = SealPages(0U, 1U,
					{firstChangedPage, retainedPage}, 0U, 2U);
				auto replacement = std::make_shared<AnimationAuthorityPage>(*firstChangedPage);
				replacement->rows.front().sourceGeneration = 2U;
				replacement->rows.front().stateId = 2U;
				replacement->changedMasks.assign(2U, 1U << 3U);
				const SealedAnimationFrameResult second = SealPages(1U, 2U,
					{replacement, retainedPage}, 1U, 1U);

				AnimationAuthorityPublisher publisher;
				const AnimationPublicationReceipt firstReceipt = publisher.Publish(first, 9U, 4U);
				const AnimationPublicationReceipt secondReceipt = publisher.Publish(second, 9U, 4U);
				const std::shared_ptr<const AnimationAuthorityFrame> root = publisher.Load();
				const AnimationAuthorityRow* changedRow = root
					? root->Find({3U, 7U, 9U}) : nullptr;
				context.Expect(firstReceipt.Published() && secondReceipt.Published() && root
					&& root->pages->size() == 2U
					&& root->pages->at(0U) != firstChangedPage
					&& root->pages->at(1U) == retainedPage
					&& changedRow != nullptr && changedRow->stateId == 2U
					&& secondReceipt.reusedPages == 1U && secondReceipt.changedPages == 1U,
					"A one-row COW update must replace its page while retaining every unchanged page identity.");
			});

		registry.Add("Animation.AuthorityPublication.StalePredecessorLeavesRootUntouched", [](TestContext& context) {
			AnimationAuthorityPublisher publisher;
			const AnimationPublicationReceipt first = publisher.Publish(MakeSealed(0U, 1U), 9U, 4U);
			const auto root = publisher.Load();
			const AnimationPublicationReceipt stale = publisher.Publish(MakeSealed(0U, 2U), 9U, 4U);
			context.Expect(first.Published() && stale.outcome
				== AnimationAuthorityPublicationOutcome::RejectedStalePredecessor
				&& publisher.Load() == root,
				"A stale sealed candidate must not replace pages, generations, or sidecars.");
		});

		registry.Add("Animation.AuthorityPublication.SealFailureRejectsAtomically", [](TestContext& context) {
			SetAnimationAuthorityFailureInjectionForTesting(
				AnimationAuthorityPublicationOutcome::RejectedAllocation);
			const SealedAnimationFrameResult rejected = MakeSealed(0U, 1U);
			ClearAnimationAuthorityFailureInjectionForTesting();
			context.Expect(!rejected.IsValid() && rejected.receipt.outcome
				== AnimationAuthorityPublicationOutcome::RejectedAllocation,
				"Injected seal failure must produce no root candidate for protected publication.");
		});

		registry.Add("Animation.AuthorityPublication.LifecycleResetRetiresVisibleRoot",
			[](TestContext& context) {
				AnimationAuthorityPublisher publisher;
				const AnimationPublicationReceipt published = publisher.Publish(
					MakeSealed(0U, 1U, 9U), 9U, 4U);
				const std::shared_ptr<const AnimationAuthorityFrame> retained = publisher.Load();
				publisher.Clear(10U, 5U);
				context.Expect(published.Published() && retained && retained->IsValid()
					&& publisher.Load() == nullptr
					&& publisher.LastReceipt().outcome
						== AnimationAuthorityPublicationOutcome::RejectedLifecycle,
					"A lifecycle/session reset must retire fresh lookups atomically while existing readers retain their immutable root.");
		});

		registry.Add("Animation.AuthorityPublication.FourThousandRowsUseOneRootAcquisition", [](TestContext& context) {
			const SealedAnimationFrameResult sealed = MakeFourThousandRowCandidate();
			AnimationAuthorityPublisher publisher;
			const AnimationPublicationReceipt receipt = publisher.Publish(sealed, 9U, 4U);
			// Consumers acquire one shared root, then resolve every required row from
			// that retained value. Reusing the sealed page identities proves the
			// protected exchange did not rebuild a per-row publication representation.
			const std::shared_ptr<const AnimationAuthorityFrame> acquiredRoot = publisher.Load();
			const AnimationAuthorityRow* first = acquiredRoot
				? acquiredRoot->Find({0U, 1U, 9U}) : nullptr;
			const AnimationAuthorityRow* middle = acquiredRoot
				? acquiredRoot->Find({2048U, 1U, 9U}) : nullptr;
			const AnimationAuthorityRow* last = acquiredRoot
				? acquiredRoot->Find({3999U, 1U, 9U}) : nullptr;
			context.Expect(sealed.IsValid() && receipt.Published() && acquiredRoot == sealed.frame
				&& acquiredRoot->pages == sealed.frame->pages
				&& acquiredRoot->pages->size() == 32U
				&& first != nullptr && middle != nullptr && last != nullptr
				&& first->authorityPublicationGeneration == 77U
				&& middle->authorityPublicationGeneration == 77U
				&& last->authorityPublicationGeneration == 77U,
				"A 4,000-row heterogeneous crowd must become visible through one immutable root acquisition.");
		});
	}
}
