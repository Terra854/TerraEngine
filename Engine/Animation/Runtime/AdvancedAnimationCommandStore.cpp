// AdvancedAnimationCommandStore.cpp - Deferred copied advanced animation state.

#include "Animation/Runtime/AdvancedAnimationCommandStore.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <tuple>
#include <type_traits>

namespace TerraEngine::Animation {
	namespace {
		constexpr bool SameAnimator(
			NativeAnimationAnimatorHandle left,
			NativeAnimationAnimatorHandle right) noexcept
		{
			return left.slotIndex == right.slotIndex && left.generation == right.generation;
		}

		constexpr AnimatorInstanceHandle ToRuntimeHandle(
			NativeAnimationAnimatorHandle value) noexcept
		{
			return {value.slotIndex, value.generation};
		}

		constexpr NativeAnimationAbiHeader Header(
			NativeAnimationRecordKind kind,
			std::size_t size) noexcept
		{
			return {static_cast<std::uint32_t>(size), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
		}

		template <typename Request>
		bool HasValidHeader(const Request& request, NativeAnimationRecordKind kind) noexcept
		{
			return request.header.size == sizeof(Request)
				&& request.header.version == kNativeAnimationAbiVersion
				&& request.header.kind == static_cast<std::uint16_t>(kind);
		}

		bool HasUuid(NativeAnimationUuid value) noexcept
		{
			return value.first != 0U || value.second != 0U;
		}

		bool Valid(const NativeAnimationRetargetRequest& request) noexcept
		{
			if(!HasValidHeader(request, NativeAnimationRecordKind::RetargetRequest)
				|| request.animator.generation == 0U || request.correlationId == 0U
				|| request.targetTick < 0) return false;
			switch(request.operation) {
			case NativeAnimationRetargetOperation::ActivateProfile:
			case NativeAnimationRetargetOperation::RequestRebuild:
				return HasUuid(request.profileAssetId) && request.profileGeneration != 0U;
			case NativeAnimationRetargetOperation::DeactivateProfile:
				return true;
			default:
				return false;
			}
		}

		bool Valid(const NativeAnimationMotionMatchRequest& request) noexcept
		{
			if(!HasValidHeader(request, NativeAnimationRecordKind::MotionMatchRequest)
				|| request.animator.generation == 0U || request.correlationId == 0U
				|| request.targetTick < 0) return false;
			switch(request.operation) {
			case NativeAnimationMotionMatchOperation::SetDatabase:
				return HasUuid(request.databaseAssetId) && request.databaseGeneration != 0U
					&& request.featureCount != 0U;
			case NativeAnimationMotionMatchOperation::Query:
				return request.queryId != 0U && request.featureCount != 0U;
			case NativeAnimationMotionMatchOperation::Clear:
				return true;
			default:
				return false;
			}
		}

		bool Valid(const NativeAnimationMotionMatchQueryRequestV2& request) noexcept
		{
			constexpr std::uint32_t requiredFields =
				static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Identity)
				| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::Features)
				| static_cast<std::uint32_t>(NativeAnimationMotionMatchQueryField::FeatureWeights);
			if(!HasValidHeader(request, NativeAnimationRecordKind::MotionMatchQueryRequestV2)
				|| request.animator.generation == 0U || !HasUuid(request.databaseAssetId)
				|| request.databaseGeneration == 0U || request.schemaFingerprint == 0U
				|| request.featureCount != kNativeAnimationMotionFeatureCount
				|| request.queryId == 0U || request.correlationId == 0U
				|| request.targetTick < 0 || request.reserved16 != 0U
				|| request.reserved32 != 0U
				|| (request.queryFields & requiredFields) != requiredFields
				|| (request.queryFields & ~kNativeAnimationMotionMatchAllQueryFields) != 0U
				|| request.maximumCandidates == 0U
				|| request.maximumCandidates > kNativeAnimationMotionMaximumTopK
				|| request.maximumSamplesToVisit == 0U
				|| request.maximumSamplesToVisit > kNativeAnimationMotionMaximumSamples
				|| request.maximumDiagnosticExclusions > kNativeAnimationMotionMaximumExcludedSamples
				|| request.excludedSampleCount > kNativeAnimationMotionMaximumExcludedSamples
				|| (request.requiredTags & request.excludedTags) != 0U
				|| (request.requiredTransitionFlags & request.excludedTransitionFlags) != 0U) {
				return false;
			}
			const bool hasExclusions = (request.queryFields & static_cast<std::uint32_t>(
				NativeAnimationMotionMatchQueryField::Exclusions)) != 0U;
			if(!hasExclusions && request.excludedSampleCount != 0U) return false;
			std::uint64_t previous = 0U;
			for(std::uint32_t index = 0U;
				index < kNativeAnimationMotionMaximumExcludedSamples; ++index) {
				const std::uint64_t sampleId = request.excludedSampleIds[index];
				if(index < request.excludedSampleCount) {
					if(sampleId == 0U || (index != 0U && sampleId <= previous)) return false;
					previous = sampleId;
				} else if(sampleId != 0U) {
					return false;
				}
			}
			return true;
		}

		bool Valid(const NativeAnimationRagdollRequest& request) noexcept
		{
			if(!HasValidHeader(request, NativeAnimationRecordKind::RagdollRequest)
				|| request.animator.generation == 0U || request.correlationId == 0U
				|| request.targetTick < 0 || !std::isfinite(request.blendWeight)
				|| request.blendWeight < 0.0F || request.blendWeight > 1.0F) return false;
			switch(request.operation) {
			case NativeAnimationRagdollOperation::Activate:
				return HasUuid(request.profileAssetId) && request.profileGeneration != 0U
					&& request.physicsWorldGeneration != 0U;
			case NativeAnimationRagdollOperation::Deactivate:
			case NativeAnimationRagdollOperation::RequestRecovery:
				return request.physicsWorldGeneration != 0U;
			default:
				return false;
			}
		}

		bool Valid(const NativeAnimationDeterminismRequest& request) noexcept
		{
			if(!HasValidHeader(request, NativeAnimationRecordKind::DeterminismRequest)
				|| request.animator.generation == 0U || request.correlationId == 0U
				|| request.sourceTick < 0 || request.targetTick < request.sourceTick) return false;
			switch(request.operation) {
			case NativeAnimationDeterminismOperation::Capture:
			case NativeAnimationDeterminismOperation::Restore:
			case NativeAnimationDeterminismOperation::ReplaySeek:
			case NativeAnimationDeterminismOperation::RequestReplication:
				return true;
			default:
				return false;
			}
		}

		NativeAnimationRetargetReceipt Receipt(
			const NativeAnimationRetargetRequest& request,
			NativeAnimationStatus status) noexcept
		{
			NativeAnimationRetargetReceipt receipt{};
			receipt.header = Header(NativeAnimationRecordKind::RetargetReceipt, sizeof(receipt));
			receipt.status = status;
			receipt.animator = request.animator;
			receipt.operation = request.operation;
			receipt.profileGeneration = request.profileGeneration;
			receipt.correlationId = request.correlationId;
			receipt.observableTick = request.targetTick;
			return receipt;
		}

		NativeAnimationMotionMatchReceipt Receipt(
			const NativeAnimationMotionMatchRequest& request,
			NativeAnimationStatus status) noexcept
		{
			NativeAnimationMotionMatchReceipt receipt{};
			receipt.header = Header(NativeAnimationRecordKind::MotionMatchReceipt, sizeof(receipt));
			receipt.status = status;
			receipt.animator = request.animator;
			receipt.operation = request.operation;
			receipt.databaseGeneration = request.databaseGeneration;
			receipt.queryId = request.queryId;
			receipt.correlationId = request.correlationId;
			receipt.observableTick = request.targetTick;
			return receipt;
		}

		NativeAnimationMotionMatchQueryReceiptV2 Receipt(
			const NativeAnimationMotionMatchQueryRequestV2& request,
			NativeAnimationStatus status) noexcept
		{
			NativeAnimationMotionMatchQueryReceiptV2 receipt{};
			receipt.header = Header(
				NativeAnimationRecordKind::MotionMatchQueryReceiptV2, sizeof(receipt));
			receipt.status = status;
			receipt.animator = request.animator;
			receipt.databaseGeneration = request.databaseGeneration;
			receipt.schemaFingerprint = request.schemaFingerprint;
			receipt.queryId = request.queryId;
			receipt.correlationId = request.correlationId;
			receipt.observableTick = request.targetTick;
			receipt.validatedFields = request.queryFields;
			return receipt;
		}

		NativeAnimationRagdollReceipt Receipt(
			const NativeAnimationRagdollRequest& request,
			NativeAnimationStatus status) noexcept
		{
			NativeAnimationRagdollReceipt receipt{};
			receipt.header = Header(NativeAnimationRecordKind::RagdollReceipt, sizeof(receipt));
			receipt.status = status;
			receipt.animator = request.animator;
			receipt.operation = request.operation;
			receipt.profileGeneration = request.profileGeneration;
			receipt.correlationId = request.correlationId;
			receipt.observableTick = request.targetTick;
			return receipt;
		}

		NativeAnimationDeterminismReceipt Receipt(
			const NativeAnimationDeterminismRequest& request,
			NativeAnimationStatus status) noexcept
		{
			NativeAnimationDeterminismReceipt receipt{};
			receipt.header = Header(NativeAnimationRecordKind::DeterminismReceipt, sizeof(receipt));
			receipt.status = status;
			receipt.animator = request.animator;
			receipt.operation = request.operation;
			receipt.correlationId = request.correlationId;
			receipt.observableTick = request.targetTick;
			return receipt;
		}

		template <typename Request>
		bool RequestLess(const Request& left, const Request& right) noexcept
		{
			return std::tie(left.targetTick, left.animator.slotIndex,
				left.animator.generation, left.correlationId)
				< std::tie(right.targetTick, right.animator.slotIndex,
					right.animator.generation, right.correlationId);
		}

		template <typename Request>
		void DrainVector(AnimationFixedTick tick, std::vector<Request>& pending,
			std::vector<Request>& output)
		{
			output.clear();
			std::vector<Request> future;
			future.reserve(pending.size());
			for(const Request& request : pending) {
				if(request.targetTick <= tick) output.push_back(request);
				else future.push_back(request);
			}
			pending = std::move(future);
			std::sort(output.begin(), output.end(), RequestLess<Request>);
		}

		template <typename Snapshot>
		AnimationStatus AppendSnapshot(
			std::vector<Snapshot>& snapshots,
			Snapshot snapshot,
			std::size_t capacity)
		{
			try {
				std::vector<Snapshot> candidate = snapshots;
				if(candidate.size() >= capacity) candidate.erase(candidate.begin());
				candidate.push_back(std::move(snapshot));
				snapshots = std::move(candidate);
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		template <typename Snapshot>
		AnimationStatus CopySnapshots(
			const std::vector<Snapshot>& snapshots,
			std::span<Snapshot> destination,
			std::uint32_t& requiredCount)
		{
			if(snapshots.size() > UINT32_MAX) return AnimationStatus::RangeOverflow;
			requiredCount = static_cast<std::uint32_t>(snapshots.size());
			if(destination.size() < snapshots.size()) return AnimationStatus::BufferTooSmall;
			std::copy(snapshots.begin(), snapshots.end(), destination.begin());
			return AnimationStatus::Ok;
		}
	}

	AdvancedAnimationCommandStore::AdvancedAnimationCommandStore(
		std::size_t pendingCapacity,
		std::size_t retainedPerAnimatorCapacity,
		std::size_t animatorStateCapacity)
		: pendingCapacity_(std::max<std::size_t>(1U, pendingCapacity))
		, retainedPerAnimatorCapacity_(std::max<std::size_t>(1U, retainedPerAnimatorCapacity))
		, animatorStateCapacity_(std::max<std::size_t>(1U, animatorStateCapacity))
	{
	}

	NativeAnimationRetargetReceipt AdvancedAnimationCommandStore::Submit(
		const NativeAnimationRetargetRequest& request,
		const AnimatorInstanceStore& instances)
	{
		NativeAnimationRetargetReceipt receipt = Receipt(request, NativeAnimationStatus::Queued);
		if(!Valid(request)) receipt.status = NativeAnimationStatus::InvalidArgument;
		else if(!instances.IsAlive(ToRuntimeHandle(request.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
		} else try {
			std::scoped_lock lock(mutex_);
			if(pendingRetarget_.size() >= pendingCapacity_) receipt.status = NativeAnimationStatus::HardLimitExceeded;
			else pendingRetarget_.push_back(request);
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::HardLimitExceeded;
		}
		return receipt;
	}

	NativeAnimationMotionMatchReceipt AdvancedAnimationCommandStore::Submit(
		const NativeAnimationMotionMatchRequest& request,
		const AnimatorInstanceStore& instances)
	{
		NativeAnimationMotionMatchReceipt receipt = Receipt(request, NativeAnimationStatus::Queued);
		if(!Valid(request)) receipt.status = NativeAnimationStatus::InvalidArgument;
		else if(!instances.IsAlive(ToRuntimeHandle(request.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
		} else try {
			std::scoped_lock lock(mutex_);
			if(pendingMotionMatch_.size() >= pendingCapacity_) receipt.status = NativeAnimationStatus::HardLimitExceeded;
			else pendingMotionMatch_.push_back(request);
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::HardLimitExceeded;
		}
		return receipt;
	}

	NativeAnimationMotionMatchQueryReceiptV2 AdvancedAnimationCommandStore::Submit(
		const NativeAnimationMotionMatchQueryRequestV2& request,
		const AnimatorInstanceStore& instances)
	{
		NativeAnimationMotionMatchQueryReceiptV2 receipt = Receipt(
			request, NativeAnimationStatus::Queued);
		if(!Valid(request)) receipt.status = NativeAnimationStatus::InvalidArgument;
		else if(!instances.IsAlive(ToRuntimeHandle(request.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
		} else try {
			std::scoped_lock lock(mutex_);
			if(pendingMotionMatchV2_.size() >= pendingCapacity_) {
				receipt.status = NativeAnimationStatus::HardLimitExceeded;
			} else {
				// The store owns the complete inline query until protected capture;
				// no scripting-side pointer or span survives this call.
				pendingMotionMatchV2_.push_back(request);
				receipt.scheduledFields = request.queryFields;
				receipt.flags = static_cast<std::uint32_t>(
					NativeAnimationMotionMatchQueryReceiptFlag::PayloadRetained);
			}
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::HardLimitExceeded;
		}
		return receipt;
	}

	NativeAnimationRagdollReceipt AdvancedAnimationCommandStore::Submit(
		const NativeAnimationRagdollRequest& request,
		const AnimatorInstanceStore& instances)
	{
		NativeAnimationRagdollReceipt receipt = Receipt(request, NativeAnimationStatus::Queued);
		if(!Valid(request)) receipt.status = NativeAnimationStatus::InvalidArgument;
		else if(!instances.IsAlive(ToRuntimeHandle(request.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
		} else try {
			std::scoped_lock lock(mutex_);
			if(pendingRagdoll_.size() >= pendingCapacity_) receipt.status = NativeAnimationStatus::HardLimitExceeded;
			else pendingRagdoll_.push_back(request);
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::HardLimitExceeded;
		}
		return receipt;
	}

	NativeAnimationDeterminismReceipt AdvancedAnimationCommandStore::Submit(
		const NativeAnimationDeterminismRequest& request,
		const AnimatorInstanceStore& instances)
	{
		NativeAnimationDeterminismReceipt receipt = Receipt(request, NativeAnimationStatus::Queued);
		if(!Valid(request)) receipt.status = NativeAnimationStatus::InvalidArgument;
		else if(!instances.IsAlive(ToRuntimeHandle(request.animator))) {
			receipt.status = NativeAnimationStatus::StaleHandle;
		} else try {
			std::scoped_lock lock(mutex_);
			if(pendingDeterminism_.size() >= pendingCapacity_) receipt.status = NativeAnimationStatus::HardLimitExceeded;
			else pendingDeterminism_.push_back(request);
		} catch(const std::bad_alloc&) {
			receipt.status = NativeAnimationStatus::HardLimitExceeded;
		}
		return receipt;
	}

	void AdvancedAnimationCommandStore::DrainThrough(
		AnimationFixedTick tick,
		std::vector<NativeAnimationRetargetRequest>& retarget,
		std::vector<NativeAnimationMotionMatchRequest>& motionMatch,
		std::vector<NativeAnimationMotionMatchQueryRequestV2>& motionMatchV2,
		std::vector<NativeAnimationRagdollRequest>& ragdoll,
		std::vector<NativeAnimationDeterminismRequest>& determinism)
	{
		std::scoped_lock lock(mutex_);
		DrainVector(tick, pendingRetarget_, retarget);
		DrainVector(tick, pendingMotionMatch_, motionMatch);
		DrainVector(tick, pendingMotionMatchV2_, motionMatchV2);
		DrainVector(tick, pendingRagdoll_, ragdoll);
		DrainVector(tick, pendingDeterminism_, determinism);
	}

	AdvancedAnimationCommandStore::AnimatorState*
	AdvancedAnimationCommandStore::FindOrCreateStateLocked(
		NativeAnimationAnimatorHandle animator)
	{
		const auto found = std::find_if(states_.begin(), states_.end(), [animator](const AnimatorState& state) {
			return SameAnimator(state.animator, animator);
		});
		if(found != states_.end()) return &*found;
		if(states_.size() >= animatorStateCapacity_) return nullptr;
		states_.push_back({animator});
		return &states_.back();
	}

	const AdvancedAnimationCommandStore::AnimatorState*
	AdvancedAnimationCommandStore::FindStateLocked(
		NativeAnimationAnimatorHandle animator) const noexcept
	{
		const auto found = std::find_if(states_.begin(), states_.end(), [animator](const AnimatorState& state) {
			return SameAnimator(state.animator, animator);
		});
		return found == states_.end() ? nullptr : &*found;
	}

	AnimationStatus AdvancedAnimationCommandStore::Publish(NativeAnimationRetargetSnapshot snapshot)
	{
		std::scoped_lock lock(mutex_);
		try {
			AnimatorState* state = FindOrCreateStateLocked(snapshot.animator);
			return state == nullptr ? AnimationStatus::HardLimitExceeded
				: AppendSnapshot(state->retarget, std::move(snapshot), retainedPerAnimatorCapacity_);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AdvancedAnimationCommandStore::Publish(NativeAnimationMotionMatchSnapshot snapshot)
	{
		std::scoped_lock lock(mutex_);
		try {
			AnimatorState* state = FindOrCreateStateLocked(snapshot.animator);
			return state == nullptr ? AnimationStatus::HardLimitExceeded
				: AppendSnapshot(state->motionMatch, std::move(snapshot), retainedPerAnimatorCapacity_);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AdvancedAnimationCommandStore::Publish(
		NativeAnimationMotionMatchResultV2 snapshot)
	{
		std::scoped_lock lock(mutex_);
		try {
			AnimatorState* state = FindOrCreateStateLocked(snapshot.animator);
			return state == nullptr ? AnimationStatus::HardLimitExceeded
				: AppendSnapshot(state->motionMatchV2, std::move(snapshot),
					retainedPerAnimatorCapacity_);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AdvancedAnimationCommandStore::Publish(NativeAnimationRagdollSnapshot snapshot)
	{
		std::scoped_lock lock(mutex_);
		try {
			AnimatorState* state = FindOrCreateStateLocked(snapshot.animator);
			return state == nullptr ? AnimationStatus::HardLimitExceeded
				: AppendSnapshot(state->ragdoll, std::move(snapshot), retainedPerAnimatorCapacity_);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AdvancedAnimationCommandStore::Publish(NativeAnimationDeterminismSnapshot snapshot)
	{
		std::scoped_lock lock(mutex_);
		try {
			AnimatorState* state = FindOrCreateStateLocked(snapshot.animator);
			return state == nullptr ? AnimationStatus::HardLimitExceeded
				: AppendSnapshot(state->determinism, std::move(snapshot), retainedPerAnimatorCapacity_);
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}
	}

	AnimationStatus AdvancedAnimationCommandStore::Copy(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationRetargetSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		return state == nullptr ? AnimationStatus::NotReady
			: CopySnapshots(state->retarget, destination, requiredCount);
	}

	AnimationStatus AdvancedAnimationCommandStore::Copy(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationMotionMatchSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		return state == nullptr ? AnimationStatus::NotReady
			: CopySnapshots(state->motionMatch, destination, requiredCount);
	}

	AnimationStatus AdvancedAnimationCommandStore::Copy(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationMotionMatchResultV2> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		return state == nullptr ? AnimationStatus::NotReady
			: CopySnapshots(state->motionMatchV2, destination, requiredCount);
	}

	AnimationStatus AdvancedAnimationCommandStore::Copy(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationRagdollSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		return state == nullptr ? AnimationStatus::NotReady
			: CopySnapshots(state->ragdoll, destination, requiredCount);
	}

	AnimationStatus AdvancedAnimationCommandStore::Copy(
		NativeAnimationAnimatorHandle animator,
		std::span<NativeAnimationDeterminismSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(mutex_);
		const AnimatorState* state = FindStateLocked(animator);
		return state == nullptr ? AnimationStatus::NotReady
			: CopySnapshots(state->determinism, destination, requiredCount);
	}

	void AdvancedAnimationCommandStore::RetireAnimator(
		NativeAnimationAnimatorHandle animator) noexcept
	{
		std::scoped_lock lock(mutex_);
		auto removeRequests = [animator](auto& requests) {
			std::erase_if(requests, [animator](const auto& request) {
				return SameAnimator(request.animator, animator);
			});
		};
		removeRequests(pendingRetarget_);
		removeRequests(pendingMotionMatch_);
		removeRequests(pendingMotionMatchV2_);
		removeRequests(pendingRagdoll_);
		removeRequests(pendingDeterminism_);
		std::erase_if(states_, [animator](const AnimatorState& state) {
			return SameAnimator(state.animator, animator);
		});
	}

	void AdvancedAnimationCommandStore::Clear() noexcept
	{
		std::scoped_lock lock(mutex_);
		pendingRetarget_.clear();
		pendingMotionMatch_.clear();
		pendingMotionMatchV2_.clear();
		pendingRagdoll_.clear();
		pendingDeterminism_.clear();
		states_.clear();
	}

	std::size_t AdvancedAnimationCommandStore::PendingCount() const noexcept
	{
		std::scoped_lock lock(mutex_);
		return pendingRetarget_.size() + pendingMotionMatch_.size()
			+ pendingMotionMatchV2_.size() + pendingRagdoll_.size()
			+ pendingDeterminism_.size();
	}
}
