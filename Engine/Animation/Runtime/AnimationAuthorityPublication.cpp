// AnimationAuthorityPublication.cpp - Validation, sealing, and atomic root exchange.

#include "Animation/Runtime/AnimationAuthorityPublication.h"

#include <algorithm>
#include <limits>
#include <new>
#include <tuple>

namespace TerraEngine::Animation {
	namespace {
		std::atomic<AnimationAuthorityPublicationOutcome> g_failureInjection{
			AnimationAuthorityPublicationOutcome::Published};

		[[nodiscard]] bool IsPageCapacityValid(std::uint32_t capacity) noexcept
		{
			return capacity == kAnimationAuthorityPageCapacity128
				|| capacity == kAnimationAuthorityPageCapacity256;
		}

		[[nodiscard]] AnimationPublicationReceipt Rejection(
			AnimationAuthorityPublicationOutcome outcome,
			std::uint64_t expected,
			std::uint64_t observed) noexcept
		{
			AnimationPublicationReceipt receipt;
			receipt.outcome = outcome;
			receipt.expectedPreviousGeneration = expected;
			receipt.observedPreviousGeneration = observed;
			receipt.authorityGeneration = observed;
			return receipt;
		}

		void StoreReceipt(
			std::atomic<std::shared_ptr<const AnimationPublicationReceipt>>& target,
			AnimationPublicationReceipt receipt) noexcept
		{
			try {
				std::atomic_store_explicit(&target,
					std::make_shared<const AnimationPublicationReceipt>(std::move(receipt)),
					std::memory_order_release);
			} catch(const std::bad_alloc&) {
				// A diagnostic receipt is never permitted to turn a successful root swap
				// into a partial failure.  The root itself remains the authority.
			}
		}
	}

	bool AnimationFrameSidecars::ValidateAndOrder(std::uint64_t authorityGeneration) noexcept
	{
		if(authorityGeneration != 0U
			&& workerValidatedAuthorityGeneration == authorityGeneration)
			return true;
		auto specialistValid = [authorityGeneration](const AnimationSpecialistRecord& value) {
			return value.IsValidFor(authorityGeneration);
		};
		if(authorityGeneration == 0U
			|| !std::all_of(callbacks.begin(), callbacks.end(), specialistValid)
			|| !std::all_of(physics.begin(), physics.end(), specialistValid)
			|| !std::all_of(attachments.begin(), attachments.end(), specialistValid)
			|| !std::all_of(procedural.begin(), procedural.end(), specialistValid)
			|| !std::all_of(rollback.begin(), rollback.end(), specialistValid)
			|| !std::all_of(completions.begin(), completions.end(), specialistValid)
			|| !std::all_of(gpuCandidates.begin(), gpuCandidates.end(), specialistValid)
			|| !std::all_of(diagnostics.begin(), diagnostics.end(), specialistValid)) return false;

		auto order = [](auto& values) {
			const auto less = [](const auto& left, const auto& right) {
				return std::tie(left.key.slot, left.key.lifetimeGeneration, left.key.sessionGeneration,
					left.correlationId, left.kind) < std::tie(right.key.slot,
					right.key.lifetimeGeneration, right.key.sessionGeneration,
					right.correlationId, right.kind);
			};
			// Capture already emits ordinary animator sidecars in slot order. Preserve
			// that proof and pay for sorting only when a specialist producer appended
			// genuinely out-of-order records.
			if(!std::is_sorted(values.begin(), values.end(), less)) {
				std::sort(values.begin(), values.end(), less);
			}
		};
		order(callbacks); order(physics); order(attachments); order(procedural);
		order(rollback); order(completions); order(gpuCandidates); order(diagnostics);
		const auto eventLess = [](const AnimationEventRecord& left,
			const AnimationEventRecord& right) { return left.producerOrder < right.producerOrder; };
		if(!std::is_sorted(events.begin(), events.end(), eventLess)) {
			std::sort(events.begin(), events.end(), eventLess);
		}
		const auto rootMotionLess = [](const AnimationRootMotionDelta& left,
			const AnimationRootMotionDelta& right) { return left.animator < right.animator; };
		if(!std::is_sorted(rootMotion.begin(), rootMotion.end(), rootMotionLess)) {
			std::sort(rootMotion.begin(), rootMotion.end(), rootMotionLess);
		}
		return true;
	}

	bool AnimationAuthorityRow::IsValid() const noexcept
	{
		if(!key.IsValid() || ownerObjectId == EMPTY_UUID || componentGeneration == 0U
			|| sourceGeneration == 0U || poseGeneration == 0U) return false;
		// GPU-only presentation rows deliberately retain no CPU pose.  Their exact
		// backend-neutral candidate lives in the same immutable authority frame.
		return !snapshot || (snapshot->animator == key.Handle() && snapshot->pose != nullptr
			&& snapshot->pose->IsSealed());
	}

	bool AnimationAuthorityPage::IsValid() const noexcept
	{
		if(!IsPageCapacityValid(capacity) || rows.size() > capacity
			|| changedMasks.size() != (static_cast<std::size_t>(capacity) + 63U) / 64U) return false;
		if(!rows.empty() && workerValidatedRowCount == rows.size())
			return true;
		for(std::size_t rowIndex = 0U; rowIndex < rows.size(); ++rowIndex) {
			const AnimationAuthorityRow& row = rows[rowIndex];
			if(!row.IsValid() || row.key.slot < firstSlot
				|| row.key.slot >= firstSlot + capacity) return false;
			// Find() uses lower_bound, so strict slot order is both the lookup contract
			// and the within-page duplicate proof.
			if(rowIndex != 0U && rows[rowIndex - 1U].key.slot >= row.key.slot)
				return false;
		}
		return true;
	}

	const AnimationAuthorityRow* AnimationAuthorityPage::Find(AnimationSlotGenerationKey key) const noexcept
	{
		if(!key.IsValid() || key.slot < firstSlot) return nullptr;
		const auto row = std::lower_bound(rows.begin(), rows.end(), key.slot,
			[](const AnimationAuthorityRow& value, std::uint32_t slot) {
				return value.key.slot < slot;
			});
		return row != rows.end() && row->key == key ? &*row : nullptr;
	}

	bool AnimationAuthorityFrame::IsValid() const noexcept
	{
		if(frameId == 0U || sessionGeneration == 0U || lifecycleGeneration == 0U
			|| authorityGeneration == 0U || !pages || !sidecars || !receipt.Published()
			|| receipt.authorityGeneration != authorityGeneration) return false;
		return std::all_of(pages->begin(), pages->end(), [](const auto& page) {
			return page != nullptr && page->IsValid();
		});
	}

	const AnimationAuthorityRow* AnimationAuthorityFrame::Find(AnimationSlotGenerationKey key) const noexcept
	{
		if(!pages || !key.IsValid() || key.sessionGeneration != sessionGeneration) return nullptr;
		for(const auto& page : *pages) {
			if(page && key.slot >= page->firstSlot
				&& key.slot < page->firstSlot + page->capacity) return page->Find(key);
		}
		return nullptr;
	}

	const AnimationAuthorityRow* AnimationAuthorityFrame::Find(AnimatorInstanceHandle handle) const noexcept
	{
		return Find({handle.slotIndex, handle.generation, sessionGeneration});
	}

	bool SealedAnimationFrameResult::IsValid() const noexcept
	{
		return !cancelled && workersComplete && historyReserved
			&& sealedValidationComplete && candidateGeneration != 0U
			&& frame != nullptr && frame->frameId != 0U
			&& frame->sessionGeneration != 0U && frame->lifecycleGeneration != 0U
			&& frame->pages != nullptr && frame->sidecars != nullptr
			&& frame->authorityGeneration == candidateGeneration
			&& frame->receipt.Published()
			&& frame->receipt.authorityGeneration == candidateGeneration
			&& receipt.Published() && receipt.authorityGeneration == candidateGeneration;
	}

	SealedAnimationFrameResult SealAnimationFrameResult(const AnimationAuthoritySealInput& input) noexcept
	{
		SealedAnimationFrameResult output;
		output.expectedPreviousGeneration = input.expectedPreviousGeneration;
		output.candidateGeneration = input.candidateGeneration;
		output.cancelled = input.cancelled;
		output.workersComplete = input.workersComplete;
		output.historyReserved = input.historyReserved;
		const AnimationAuthorityPublicationOutcome injected =
			g_failureInjection.exchange(AnimationAuthorityPublicationOutcome::Published,
				std::memory_order_acq_rel);
		if(injected != AnimationAuthorityPublicationOutcome::Published) {
			output.receipt = Rejection(injected, input.expectedPreviousGeneration,
				input.expectedPreviousGeneration);
			return output;
		}
		if(input.cancelled) { output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedCancelled,
			input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output; }
		if(!input.workersComplete) { output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedMissingWorker,
			input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output; }
		if(!input.historyReserved) { output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedHistoryReservation,
			input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output; }
		if(input.frameId == 0U || input.sessionGeneration == 0U || input.lifecycleGeneration == 0U
			|| input.candidateGeneration == 0U || input.candidateGeneration <= input.expectedPreviousGeneration
			|| (!input.sidecars && !input.ownedSidecars)) { output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedInvalidCandidate,
			input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output; }
		try {
			auto pages = std::make_shared<std::vector<std::shared_ptr<const AnimationAuthorityPage>>>(
				input.pages.begin(), input.pages.end());
			std::uint64_t priorPageEnd = 0U;
			bool hasPriorPage = false;
			for(const auto& page : *pages) {
				if(!page || !page->IsValid()) {
					output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedInvalidCandidate,
						input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output;
				}
				const std::uint64_t pageBegin = page->firstSlot;
				const std::uint64_t pageEnd = pageBegin + page->capacity;
				// Page ranges are immutable and ordered. A repeated or overlapping range
				// is therefore the only possible cross-page duplicate-slot source; checking
				// it avoids collecting and sorting every animator slot during every seal.
				if(hasPriorPage && pageBegin < priorPageEnd) {
					output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedDuplicateSlot,
						input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output;
				}
				priorPageEnd = pageEnd;
				hasPriorPage = true;
			}
			std::shared_ptr<AnimationFrameSidecars> sidecars = input.ownedSidecars;
			if(sidecars == nullptr) {
				sidecars = std::make_shared<AnimationFrameSidecars>(*input.sidecars);
			}
			if(!sidecars->ValidateAndOrder(input.candidateGeneration)) {
				output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedSidecars,
					input.expectedPreviousGeneration, input.expectedPreviousGeneration); return output;
			}
			auto frame = std::make_shared<AnimationAuthorityFrame>();
			frame->frameId = input.frameId; frame->sessionGeneration = input.sessionGeneration;
			frame->lifecycleGeneration = input.lifecycleGeneration;
			frame->authorityGeneration = input.candidateGeneration;
			frame->pages = std::move(pages); frame->sidecars = std::move(sidecars);
			frame->receipt.outcome = AnimationAuthorityPublicationOutcome::Published;
			frame->receipt.authorityGeneration = input.candidateGeneration;
			frame->receipt.expectedPreviousGeneration = input.expectedPreviousGeneration;
			frame->receipt.reusedPages = input.reusedPages;
			frame->receipt.changedPages = input.changedPages;
			frame->receipt.evaluatedRows = frame->sidecars->summary.evaluatedRows;
			frame->receipt.fallbackRows = frame->sidecars->summary.fallbackRows;
			frame->receipt.eventCount = static_cast<std::uint32_t>(frame->sidecars->events.size());
			frame->receipt.gpuCandidateCount = static_cast<std::uint32_t>(frame->sidecars->gpuCandidates.size());
			output.frame = std::move(frame); output.receipt = output.frame->receipt;
			// Every owned page and sidecar was validated above and the published frame
			// is immutable. Record that proof once so IsValid()/Publish() stay O(1).
			output.sealedValidationComplete = true;
		} catch(const std::bad_alloc&) {
			output.receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedAllocation,
				input.expectedPreviousGeneration, input.expectedPreviousGeneration);
		}
		return output;
	}

	void SetAnimationAuthorityFailureInjectionForTesting(AnimationAuthorityPublicationOutcome outcome) noexcept
	{
		g_failureInjection.store(outcome, std::memory_order_release);
	}

	void ClearAnimationAuthorityFailureInjectionForTesting() noexcept
	{
		g_failureInjection.store(AnimationAuthorityPublicationOutcome::Published, std::memory_order_release);
	}

	AnimationPublicationReceipt AnimationAuthorityPublisher::Publish(const SealedAnimationFrameResult& candidate,
		std::uint64_t expectedSessionGeneration, std::uint64_t expectedLifecycleGeneration) noexcept
	{
		const std::shared_ptr<const AnimationAuthorityFrame> previous = Load();
		const std::uint64_t observed = previous ? previous->authorityGeneration : 0U;
		if(!candidate.IsValid()) {
			const AnimationPublicationReceipt receipt = Rejection(candidate.cancelled
				? AnimationAuthorityPublicationOutcome::RejectedCancelled
				: AnimationAuthorityPublicationOutcome::RejectedInvalidCandidate,
				candidate.expectedPreviousGeneration, observed); StoreReceipt(lastReceipt_, receipt); return receipt;
		}
		if(candidate.expectedPreviousGeneration != observed) {
			const auto receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedStalePredecessor,
				candidate.expectedPreviousGeneration, observed); StoreReceipt(lastReceipt_, receipt); return receipt;
		}
		if(candidate.frame->sessionGeneration != expectedSessionGeneration
			|| candidate.frame->lifecycleGeneration != expectedLifecycleGeneration) {
			const auto receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedLifecycle,
				candidate.expectedPreviousGeneration, observed); StoreReceipt(lastReceipt_, receipt); return receipt;
		}
		// No allocation or crowd traversal is allowed after this point.  A single
		// shared-pointer exchange makes the fully sealed root visible atomically.
		std::shared_ptr<const AnimationAuthorityFrame> expected = previous;
		if(!root_.compare_exchange_strong(expected, candidate.frame, std::memory_order_release,
			std::memory_order_acquire)) {
			const auto receipt = Rejection(AnimationAuthorityPublicationOutcome::RejectedStalePredecessor,
				candidate.expectedPreviousGeneration, expected ? expected->authorityGeneration : 0U);
			StoreReceipt(lastReceipt_, receipt); return receipt;
		}
		AnimationPublicationReceipt receipt = candidate.receipt;
		receipt.outcome = AnimationAuthorityPublicationOutcome::Published;
		receipt.observedPreviousGeneration = observed;
		StoreReceipt(lastReceipt_, receipt);
		return receipt;
	}

	std::shared_ptr<const AnimationAuthorityFrame> AnimationAuthorityPublisher::Load() const noexcept
	{
		return std::atomic_load_explicit(&root_, std::memory_order_acquire);
	}

	AnimationPublicationReceipt AnimationAuthorityPublisher::LastReceipt() const noexcept
	{
		const auto receipt = std::atomic_load_explicit(&lastReceipt_, std::memory_order_acquire);
		return receipt ? *receipt : AnimationPublicationReceipt{};
	}

	void AnimationAuthorityPublisher::Clear(std::uint64_t sessionGeneration,
		std::uint64_t lifecycleGeneration) noexcept
	{
		std::atomic_store_explicit(&root_, std::shared_ptr<const AnimationAuthorityFrame>{},
			std::memory_order_release);
		AnimationPublicationReceipt receipt;
		receipt.outcome = AnimationAuthorityPublicationOutcome::RejectedLifecycle;
		receipt.expectedPreviousGeneration = sessionGeneration;
		receipt.observedPreviousGeneration = lifecycleGeneration;
		StoreReceipt(lastReceipt_, receipt);
	}
}
