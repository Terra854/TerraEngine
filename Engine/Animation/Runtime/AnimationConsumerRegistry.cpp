// AnimationConsumerRegistry.cpp - Atomic declaration batches and closure attribution.

#include "Animation/Runtime/AnimationConsumerRegistry.h"

#include <algorithm>
#include <limits>
#include <new>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::size_t kMaximumConsumerDeclarations = 4096U;
		constexpr std::uint64_t kLocomotionRootConsumerId = 0x4c4f434f524f4f54ULL;
		constexpr std::uint64_t kFingerprintOffset = 14695981039346656037ULL;
		constexpr std::uint64_t kFingerprintPrime = 1099511628211ULL;

		void HashU64(std::uint64_t& hash, std::uint64_t value) noexcept
		{
			for(std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
				hash ^= static_cast<std::uint8_t>((value >> shift) & 0xffULL);
				hash *= kFingerprintPrime;
			}
		}

		bool IsKnownKind(AnimationConsumerKind kind) noexcept
		{
			return kind >= AnimationConsumerKind::Gameplay
				&& kind <= AnimationConsumerKind::Procedural;
		}

		AnimationConsumerReason ReasonsFor(const AnimationConsumerDeclaration& declaration) noexcept
		{
			AnimationConsumerReason reasons = AnimationConsumerReason::None;
			if(!declaration.joints.empty()) reasons = reasons | AnimationConsumerReason::Joints;
			if(!declaration.curves.empty()) reasons = reasons | AnimationConsumerReason::Curves;
			if(declaration.morphs) reasons = reasons | AnimationConsumerReason::Morphs;
			if(!declaration.events.empty()) reasons = reasons | AnimationConsumerReason::Events;
			if(declaration.rootMotion) reasons = reasons | AnimationConsumerReason::RootMotion;
			if(declaration.contacts) reasons = reasons | AnimationConsumerReason::Contacts;
			if(declaration.proceduralResults) reasons = reasons | AnimationConsumerReason::ProceduralResults;
			if(declaration.completePose) reasons = reasons | AnimationConsumerReason::CompletePose;
			if(declaration.deterministic) reasons = reasons | AnimationConsumerReason::Deterministic;
			return reasons;
		}

		template<typename Value>
		void SortUnique(std::vector<Value>& values)
		{
			std::sort(values.begin(), values.end());
			values.erase(std::unique(values.begin(), values.end()), values.end());
		}
	}

	struct AnimationConsumerRegistry::PublishedDeclarations {
		AnimationGeneration generation = 0U;
		AnimationFixedTick declarationTick = 0;
		std::vector<AnimationConsumerDeclaration> values;
	};

	AnimationConsumerRegistry& AnimationConsumerRegistry::Instance()
	{
		static AnimationConsumerRegistry registry;
		return registry;
	}

	AnimationStatus AnimationConsumerRegistry::PublishDeclarations(
		std::span<const AnimationConsumerDeclaration> declarations,
		AnimationGeneration generation,
		AnimationFixedTick declarationTick)
	{
		if(generation == 0U || declarationTick < 0
			|| declarations.size() > kMaximumConsumerDeclarations) {
			return declarations.size() > kMaximumConsumerDeclarations
				? AnimationStatus::HardLimitExceeded
				: AnimationStatus::InvalidArgument;
		}
		try {
			auto staged = std::make_shared<PublishedDeclarations>();
			staged->generation = generation;
			staged->declarationTick = declarationTick;
			staged->values.assign(declarations.begin(), declarations.end());
			for(AnimationConsumerDeclaration& declaration : staged->values) {
				if(declaration.consumerId == 0U || !IsKnownKind(declaration.kind)
					|| !declaration.animator.IsValid()
					|| declaration.declarationGeneration != generation
					|| declaration.declarationTick != declarationTick
					|| declaration.maximumSnapshotAgeTicks < 0
					|| declaration.joints.size() > kMaximumAnimationExecutionJoints
					|| declaration.curves.size() > kMaximumAnimationExecutionModeBatchSize
					|| declaration.events.size() > kMaximumAnimationExecutionModeBatchSize) {
					return AnimationStatus::InvalidArgument;
				}
				SortUnique(declaration.joints);
				SortUnique(declaration.curves);
				SortUnique(declaration.events);
				if(std::any_of(declaration.curves.begin(), declaration.curves.end(), [](std::uint64_t id) { return id == 0U; })
					|| std::any_of(declaration.events.begin(), declaration.events.end(), [](std::uint64_t id) { return id == 0U; })) {
					return AnimationStatus::InvalidArgument;
				}
			}
			std::sort(staged->values.begin(), staged->values.end(), [](const auto& left, const auto& right) {
				if(left.animator.slotIndex != right.animator.slotIndex) return left.animator.slotIndex < right.animator.slotIndex;
				if(left.animator.generation != right.animator.generation) return left.animator.generation < right.animator.generation;
				return left.consumerId < right.consumerId;
			});
			if(std::adjacent_find(staged->values.begin(), staged->values.end(), [](const auto& left, const auto& right) {
				return left.animator == right.animator && left.consumerId == right.consumerId;
			}) != staged->values.end()) return AnimationStatus::Rejected;

			std::scoped_lock lock(publishMutex_);
			const std::shared_ptr<const PublishedDeclarations> current =
				std::atomic_load_explicit(&published_, std::memory_order_acquire);
			const std::shared_ptr<const PublishedDeclarations> roots =
				std::atomic_load_explicit(&rootMotionPublished_, std::memory_order_acquire);
			if(current && generation <= current->generation) return AnimationStatus::Rejected;
			if(roots && staged->values.size() + roots->values.size() > kMaximumConsumerDeclarations)
				return AnimationStatus::HardLimitExceeded;
			std::atomic_store_explicit(
				&published_, std::shared_ptr<const PublishedDeclarations>(std::move(staged)),
				std::memory_order_release);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationResolvedConsumerRequirements AnimationConsumerRegistry::Resolve(
		AnimatorInstanceHandle animator,
		std::span<const std::int32_t> jointParents,
		AnimationFixedTick fixedTick) const
	{
		return CaptureReadView().Resolve(animator, jointParents, fixedTick);
	}

	AnimationConsumerRegistry::PreparedRootMotionConsumers
	AnimationConsumerRegistry::PrepareRootMotionConsumers(
		std::span<const AnimatorInstanceHandle> activeAnimators,
		AnimationGeneration generation,
		AnimationFixedTick declarationTick) const
	{
		PreparedRootMotionConsumers prepared;
		if(generation == 0U || declarationTick < 0) {
			prepared.status_ = AnimationStatus::InvalidArgument;
			return prepared;
		}
		if(activeAnimators.size() > kMaximumConsumerDeclarations) {
			prepared.status_ = AnimationStatus::HardLimitExceeded;
			return prepared;
		}
		try {
			auto staged = std::make_shared<PublishedDeclarations>();
			staged->generation = generation;
			staged->declarationTick = declarationTick;
			staged->values.reserve(activeAnimators.size());
			for(const AnimatorInstanceHandle animator : activeAnimators) {
				if(!animator.IsValid()) {
					prepared.status_ = AnimationStatus::InvalidArgument;
					return prepared;
				}
				AnimationConsumerDeclaration declaration;
				declaration.consumerId = kLocomotionRootConsumerId;
				declaration.kind = AnimationConsumerKind::Physics;
				declaration.animator = animator;
				declaration.declarationGeneration = generation;
				declaration.declarationTick = declarationTick;
				declaration.rootMotion = true;
				staged->values.push_back(std::move(declaration));
			}
			std::sort(staged->values.begin(), staged->values.end(), [](const auto& left, const auto& right) {
				return left.animator < right.animator;
			});
			if(std::adjacent_find(staged->values.begin(), staged->values.end(), [](const auto& left, const auto& right) {
				return left.animator == right.animator;
			}) != staged->values.end()) {
				prepared.status_ = AnimationStatus::Rejected;
				return prepared;
			}
			// Keep this lock through the owner's physical lease preflight and root
			// swap. Commit only exchanges an already allocated immutable candidate.
			prepared.reservation_ = std::unique_lock(publishMutex_);
			const auto ordinary = std::atomic_load_explicit(&published_, std::memory_order_acquire);
			const auto current = std::atomic_load_explicit(&rootMotionPublished_, std::memory_order_acquire);
			if((ordinary && ordinary->values.size() + staged->values.size() > kMaximumConsumerDeclarations)
				|| (current && generation <= current->generation)) {
				prepared.status_ = ordinary && ordinary->values.size() + staged->values.size() > kMaximumConsumerDeclarations
					? AnimationStatus::HardLimitExceeded : AnimationStatus::Rejected;
				prepared.reservation_.unlock();
				return prepared;
			}
			prepared.candidate_ = std::move(staged);
			prepared.status_ = AnimationStatus::Ok;
			return prepared;
		} catch(const std::bad_alloc&) {
			prepared.status_ = AnimationStatus::InternalError;
			return prepared;
		}
	}

	void AnimationConsumerRegistry::CommitRootMotionConsumers(
		PreparedRootMotionConsumers&& prepared) noexcept
	{
		if(prepared.status_ != AnimationStatus::Ok || !prepared.reservation_.owns_lock()
			|| !prepared.candidate_) return;
		std::atomic_store_explicit(&rootMotionPublished_, std::move(prepared.candidate_),
			std::memory_order_release);
		prepared.reservation_.unlock();
	}

	void AnimationConsumerRegistry::ClearRootMotionConsumers() noexcept
	{
		std::scoped_lock lock(publishMutex_);
		std::atomic_store_explicit(&rootMotionPublished_,
			std::shared_ptr<const PublishedDeclarations>{}, std::memory_order_release);
	}

	AnimationConsumerRegistry::ReadView
	AnimationConsumerRegistry::CaptureReadView() const noexcept
	{
		ReadView view;
		view.snapshot_ = std::atomic_load_explicit(
			&published_, std::memory_order_acquire);
		view.rootMotionSnapshot_ = std::atomic_load_explicit(
			&rootMotionPublished_, std::memory_order_acquire);
		return view;
	}

	bool AnimationConsumerRegistry::ReadView::HasDeclarations() const noexcept
	{
		return (snapshot_ != nullptr && !snapshot_->values.empty())
			|| (rootMotionSnapshot_ != nullptr && !rootMotionSnapshot_->values.empty());
	}

	AnimationResolvedConsumerRequirements
	AnimationConsumerRegistry::ReadView::Resolve(
		AnimatorInstanceHandle animator,
		std::span<const std::int32_t> jointParents,
		AnimationFixedTick fixedTick) const
	{
		AnimationResolvedConsumerRequirements resolved;
		if(!animator.IsValid() || fixedTick < 0) {
			resolved.status = AnimationStatus::InvalidArgument;
			return resolved;
		}
		if(!snapshot_ && !rootMotionSnapshot_) return resolved;
		if((snapshot_ && snapshot_->declarationTick > fixedTick)
			|| (rootMotionSnapshot_ && rootMotionSnapshot_->declarationTick > fixedTick)) {
			resolved.status = AnimationStatus::InvalidArgument;
			return resolved;
		}
		resolved.declarationGeneration = std::max(snapshot_ ? snapshot_->generation : 0U,
			rootMotionSnapshot_ ? rootMotionSnapshot_->generation : 0U);
		resolved.declarationTick = std::max(snapshot_ ? snapshot_->declarationTick : 0,
			rootMotionSnapshot_ ? rootMotionSnapshot_->declarationTick : 0);
		resolved.maximumSnapshotAgeTicks = std::numeric_limits<AnimationFixedTick>::max();
		try {
			std::vector<std::uint32_t> requestedJoints;
			AnimationConsumerRequirements requirements;
			requirements.jointParents = jointParents;
			// Publications are sorted by generation-safe animator handle. Skip directly
			// to this animator's contiguous declaration range instead of walking every
			// unrelated gameplay/tooling consumer for every visual crowd member.
			auto accumulate = [&](const std::shared_ptr<const PublishedDeclarations>& publication) {
				if(!publication) return;
				const auto first = std::lower_bound(
					publication->values.begin(), publication->values.end(), animator,
					[](const AnimationConsumerDeclaration& declaration,
						AnimatorInstanceHandle target) {
						return declaration.animator.slotIndex != target.slotIndex
							? declaration.animator.slotIndex < target.slotIndex
							: declaration.animator.generation < target.generation;
					});
				for(auto iterator = first;
					iterator != publication->values.end() && iterator->animator == animator;
					++iterator) {
				const AnimationConsumerDeclaration& declaration = *iterator;
				const std::size_t before = requestedJoints.size();
				requestedJoints.insert(requestedJoints.end(), declaration.joints.begin(), declaration.joints.end());
				resolved.curves.insert(resolved.curves.end(), declaration.curves.begin(), declaration.curves.end());
				resolved.events.insert(resolved.events.end(), declaration.events.begin(), declaration.events.end());
				requirements.curves = requirements.curves || !declaration.curves.empty();
				requirements.morphs = requirements.morphs || declaration.morphs;
				requirements.events = requirements.events || !declaration.events.empty();
				requirements.rootMotion = requirements.rootMotion || declaration.rootMotion;
				requirements.contacts = requirements.contacts || declaration.contacts;
				requirements.proceduralResults = requirements.proceduralResults || declaration.proceduralResults;
				requirements.physics = requirements.physics || declaration.kind == AnimationConsumerKind::Physics;
				requirements.attachments = requirements.attachments || declaration.kind == AnimationConsumerKind::Attachment;
				requirements.networking = requirements.networking || declaration.kind == AnimationConsumerKind::Networking;
				requirements.rollback = requirements.rollback || declaration.kind == AnimationConsumerKind::Rollback;
				requirements.replay = requirements.replay || declaration.kind == AnimationConsumerKind::Replay;
				requirements.recording = requirements.recording || declaration.kind == AnimationConsumerKind::Recording;
				requirements.automation = requirements.automation || declaration.kind == AnimationConsumerKind::Automation;
				requirements.diagnostics = requirements.diagnostics || declaration.kind == AnimationConsumerKind::Diagnostics;
				requirements.completePose = requirements.completePose || declaration.completePose;
				resolved.deterministic = resolved.deterministic || declaration.deterministic;
				resolved.maximumSnapshotAgeTicks = std::min(
					resolved.maximumSnapshotAgeTicks, declaration.maximumSnapshotAgeTicks);
				resolved.attribution.push_back({
					declaration.consumerId,
					declaration.kind,
					ReasonsFor(declaration),
					declaration.maximumSnapshotAgeTicks,
					static_cast<std::uint32_t>(requestedJoints.size() - before),
					0U});
				}
			};
			accumulate(snapshot_);
			accumulate(rootMotionSnapshot_);
			if(resolved.attribution.empty()) {
				resolved.maximumSnapshotAgeTicks = 0;
				return resolved;
			}
			SortUnique(requestedJoints);
			SortUnique(resolved.curves);
			SortUnique(resolved.events);
			requirements.requestedJoints = requestedJoints;
			AnimationExecutionModeRequest request;
			request.fixedTick = fixedTick;
			request.requestedPolicy = AnimationExecutionPolicy::Auto;
			request.consumers = requirements;
			request.gpu = {};
			const AnimationExecutionModeDecision decision = ResolveAnimationExecutionMode(request, {});
			resolved.closure = decision.consumerClosure;
			resolved.status = resolved.closure.status;
			for(AnimationConsumerAttribution& attribution : resolved.attribution) {
				attribution.closedJointCount = static_cast<std::uint32_t>(resolved.closure.joints.size());
			}
			std::uint64_t hash = kFingerprintOffset;
			HashU64(hash, resolved.closure.fingerprint);
			for(const std::uint64_t curve : resolved.curves) HashU64(hash, curve);
			for(const std::uint64_t event : resolved.events) HashU64(hash, event);
			for(const AnimationConsumerAttribution& attribution : resolved.attribution) {
				HashU64(hash, attribution.consumerId);
				HashU64(hash, static_cast<std::uint32_t>(attribution.reasons));
			}
			resolved.fingerprint = hash;
		} catch(const std::bad_alloc&) {
			resolved.status = AnimationStatus::InternalError;
			resolved.closure = {};
			resolved.curves.clear();
			resolved.events.clear();
			resolved.attribution.clear();
		}
		return resolved;
	}

	void AnimationConsumerRegistry::Clear() noexcept
	{
		std::scoped_lock lock(publishMutex_);
		std::atomic_store_explicit(
			&published_, std::shared_ptr<const PublishedDeclarations>{}, std::memory_order_release);
		std::atomic_store_explicit(
			&rootMotionPublished_, std::shared_ptr<const PublishedDeclarations>{}, std::memory_order_release);
	}

	AnimationGeneration AnimationConsumerRegistry::PublishedGeneration() const noexcept
	{
		const std::shared_ptr<const PublishedDeclarations> snapshot =
			std::atomic_load_explicit(&published_, std::memory_order_acquire);
		return snapshot ? snapshot->generation : 0U;
	}

	std::size_t AnimationConsumerRegistry::DeclarationCount() const noexcept
	{
		const std::shared_ptr<const PublishedDeclarations> snapshot =
			std::atomic_load_explicit(&published_, std::memory_order_acquire);
		return snapshot ? snapshot->values.size() : 0U;
	}

	std::size_t AnimationConsumerRegistry::RootMotionDeclarationCount() const noexcept
	{
		const auto snapshot = std::atomic_load_explicit(&rootMotionPublished_, std::memory_order_acquire);
		return snapshot ? snapshot->values.size() : 0U;
	}
}
