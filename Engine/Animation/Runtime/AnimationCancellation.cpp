// AnimationCancellation.cpp - Lifecycle-safe cancellation token implementation.

#include "Animation/Runtime/AnimationCancellation.h"

#include <atomic>
#include <initializer_list>
#include <limits>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		[[nodiscard]] bool IsValidBoundary(AnimationLifecycleBoundary boundary) noexcept
		{
			return static_cast<std::size_t>(boundary) < kAnimationLifecycleBoundaryCount;
		}

		[[nodiscard]] bool IsValidOperation(AnimationCancellationOperation operation) noexcept
		{
			return static_cast<std::size_t>(operation)
				< static_cast<std::size_t>(AnimationCancellationOperation::Count);
		}

		[[nodiscard]] bool IsValidReason(AnimationCancellationReason reason) noexcept
		{
			return reason > AnimationCancellationReason::None
				&& reason <= AnimationCancellationReason::GenerationExhausted;
		}

		[[nodiscard]] AnimationLifecycleMask CombineMasks(
			std::initializer_list<AnimationLifecycleBoundary> boundaries) noexcept
		{
			AnimationLifecycleMask mask = 0U;
			for(const AnimationLifecycleBoundary boundary : boundaries) {
				mask |= AnimationLifecycleBoundaryMask(boundary);
			}
			return mask;
		}
	}

	struct AnimationCancellationState {
		AnimationCancellationOperation operation = AnimationCancellationOperation::Count;
		AnimationLifecycleMask mask = 0U;
		AnimationLifecycleGenerationSnapshot captured{};
		std::atomic<AnimationCancellationReason> reason{AnimationCancellationReason::None};
	};

	std::uint64_t AnimationLifecycleGenerationSnapshot::Generation(
		AnimationLifecycleBoundary boundary) const noexcept
	{
		return IsValidBoundary(boundary)
			? generations[static_cast<std::size_t>(boundary)]
			: 0U;
	}

	bool AnimationLifecycleGenerationSnapshot::IsValid(AnimationLifecycleMask mask) const noexcept
	{
		if(mask == 0U || (mask & ~kAllAnimationLifecycleBoundaries) != 0U) {
			return false;
		}
		for(std::size_t index = 0U; index < kAnimationLifecycleBoundaryCount; ++index) {
			const AnimationLifecycleMask bit = AnimationLifecycleMask{1U} << index;
			if((mask & bit) != 0U && generations[index] == 0U) {
				return false;
			}
		}
		return true;
	}

	AnimationGenerationLatch::AnimationGenerationLatch() noexcept
	{
		// Generation zero is permanently invalid. Fresh work always starts from the
		// first valid generation and can therefore distinguish default snapshots.
		current_.generations.fill(1U);
	}

	AnimationLifecycleGenerationSnapshot AnimationGenerationLatch::Capture() const noexcept
	{
		std::lock_guard lock(mutex_);
		return current_;
	}

	AnimationLifecycleValidation AnimationGenerationLatch::Validate(
		const AnimationLifecycleGenerationSnapshot& snapshot,
		AnimationLifecycleMask mask) const noexcept
	{
		AnimationLifecycleValidation result;
		if(mask == 0U || (mask & ~kAllAnimationLifecycleBoundaries) != 0U) {
			return result;
		}

		std::lock_guard lock(mutex_);
		for(std::size_t index = 0U; index < kAnimationLifecycleBoundaryCount; ++index) {
			const AnimationLifecycleMask bit = AnimationLifecycleMask{1U} << index;
			if((mask & bit) == 0U) {
				continue;
			}
			const std::uint64_t captured = snapshot.generations[index];
			const std::uint64_t current = current_.generations[index];
			if(captured == 0U || current == 0U || captured != current) {
				result.boundary = static_cast<AnimationLifecycleBoundary>(index);
				result.capturedGeneration = captured;
				result.currentGeneration = current;
				return result;
			}
		}
		result.current = true;
		return result;
	}

	AnimationStatus AnimationGenerationLatch::Invalidate(
		AnimationLifecycleBoundary boundary) noexcept
	{
		if(!IsValidBoundary(boundary)) {
			return AnimationStatus::InvalidArgument;
		}

		std::lock_guard lock(mutex_);
		std::uint64_t& generation = current_.generations[static_cast<std::size_t>(boundary)];
		if(generation == 0U) {
			return AnimationStatus::Cancelled;
		}
		if(boundary == AnimationLifecycleBoundary::Shutdown
			|| generation == std::numeric_limits<std::uint64_t>::max()) {
			// Zero is a terminal latch. It can never wrap back to a generation held
			// by ancient work and it prevents admitting new jobs for this boundary.
			generation = 0U;
			return AnimationStatus::Ok;
		}
		++generation;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationGenerationLatch::SetGenerationForTesting(
		AnimationLifecycleBoundary boundary,
		std::uint64_t generation) noexcept
	{
		if(!IsValidBoundary(boundary) || generation == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		std::lock_guard lock(mutex_);
		std::uint64_t& current = current_.generations[static_cast<std::size_t>(boundary)];
		if(current == 0U || generation < current) {
			return AnimationStatus::StaleHandle;
		}
		current = generation;
		return AnimationStatus::Ok;
	}

	AnimationCancellationPollResult AnimationCancellationToken::Poll(
		const AnimationGenerationLatch& latch) const noexcept
	{
		AnimationCancellationPollResult result;
		if(!state_) {
			return result;
		}

		result.operation = state_->operation;
		AnimationCancellationReason explicitReason =
			state_->reason.load(std::memory_order_acquire);
		if(explicitReason != AnimationCancellationReason::None) {
			result.reason = explicitReason;
			return result;
		}

		const AnimationLifecycleValidation lifecycle = latch.Validate(
			state_->captured,
			state_->mask);
		if(!lifecycle.current) {
			result.reason = CancellationReasonForBoundary(
				lifecycle.boundary,
				lifecycle.currentGeneration);
			result.boundary = lifecycle.boundary;
			result.capturedGeneration = lifecycle.capturedGeneration;
			result.currentGeneration = lifecycle.currentGeneration;
			return result;
		}

		// Recheck after the generation comparison so an explicit cancel racing the
		// protected-boundary poll is not accidentally reported as publishable.
		explicitReason = state_->reason.load(std::memory_order_acquire);
		if(explicitReason != AnimationCancellationReason::None) {
			result.reason = explicitReason;
			return result;
		}

		result.status = AnimationStatus::Ok;
		result.reason = AnimationCancellationReason::None;
		return result;
	}

	AnimationCancellationSource::~AnimationCancellationSource()
	{
		static_cast<void>(Cancel(AnimationCancellationReason::OwnerReleased));
	}

	AnimationCancellationSource::AnimationCancellationSource(
		AnimationCancellationSource&& other) noexcept
		: state_(std::move(other.state_))
	{
	}

	AnimationCancellationSource& AnimationCancellationSource::operator=(
		AnimationCancellationSource&& other) noexcept
	{
		if(this == &other) {
			return *this;
		}
		static_cast<void>(Cancel(AnimationCancellationReason::OwnerReleased));
		state_ = std::move(other.state_);
		return *this;
	}

	AnimationStatus AnimationCancellationSource::Initialize(
		AnimationCancellationOperation operation,
		const AnimationGenerationLatch& latch,
		AnimationLifecycleMask mask) noexcept
	{
		if(!IsValidOperation(operation)) {
			return AnimationStatus::InvalidArgument;
		}
		if(mask == 0U) {
			mask = DefaultAnimationCancellationMask(operation);
		}
		if(mask == 0U || (mask & ~kAllAnimationLifecycleBoundaries) != 0U) {
			return AnimationStatus::InvalidArgument;
		}

		const AnimationLifecycleGenerationSnapshot captured = latch.Capture();
		if(!captured.IsValid(mask)) {
			return AnimationStatus::Cancelled;
		}

		std::shared_ptr<AnimationCancellationState> staged;
		try {
			staged = std::make_shared<AnimationCancellationState>();
			staged->operation = operation;
			staged->mask = mask;
			staged->captured = captured;
		} catch(...) {
			return AnimationStatus::InternalError;
		}

		static_cast<void>(Cancel(AnimationCancellationReason::OwnerReleased));
		state_ = std::move(staged);
		return AnimationStatus::Ok;
	}

	AnimationCancellationToken AnimationCancellationSource::GetToken() const noexcept
	{
		return AnimationCancellationToken(state_);
	}

	bool AnimationCancellationSource::Cancel(AnimationCancellationReason reason) noexcept
	{
		if(!state_ || !IsValidReason(reason)) {
			return false;
		}
		AnimationCancellationReason expected = AnimationCancellationReason::None;
		return state_->reason.compare_exchange_strong(
			expected,
			reason,
			std::memory_order_acq_rel,
			std::memory_order_acquire);
	}

	AnimationLifecycleMask DefaultAnimationCancellationMask(
		AnimationCancellationOperation operation) noexcept
	{
		switch(operation) {
		case AnimationCancellationOperation::Import:
			return CombineMasks({
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::CpuEvaluation:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::ScriptDomain,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::PhysicsWorld,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::GpuBatch:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Asset,
				// Script hot reload can replace program/binding identities consumed by
				// GPU work just as it can replace CPU evaluation state. Retaining a
				// batch across that boundary would allow stale compact work to publish.
				AnimationLifecycleBoundary::ScriptDomain,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::Renderer,
				AnimationLifecycleBoundary::Device,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::Recording:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::ScriptDomain,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::Reload:
			return CombineMasks({
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::ScriptDomain,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::ScenePublication:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::PhysicsHandoff:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::PhysicsWorld,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::ManagedDelivery:
			return CombineMasks({
				AnimationLifecycleBoundary::Object,
				AnimationLifecycleBoundary::Component,
				AnimationLifecycleBoundary::Scene,
				AnimationLifecycleBoundary::Asset,
				AnimationLifecycleBoundary::ScriptDomain,
				AnimationLifecycleBoundary::PlayMode,
				AnimationLifecycleBoundary::Shutdown});
		case AnimationCancellationOperation::Shutdown:
			return AnimationLifecycleBoundaryMask(AnimationLifecycleBoundary::Shutdown);
		case AnimationCancellationOperation::Count:
			break;
		}
		return 0U;
	}

	AnimationCancellationReason CancellationReasonForBoundary(
		AnimationLifecycleBoundary boundary,
		std::uint64_t currentGeneration) noexcept
	{
		if(boundary != AnimationLifecycleBoundary::Shutdown && currentGeneration == 0U) {
			return AnimationCancellationReason::GenerationExhausted;
		}
		switch(boundary) {
		case AnimationLifecycleBoundary::Object: return AnimationCancellationReason::ObjectReused;
		case AnimationLifecycleBoundary::Component: return AnimationCancellationReason::ComponentRemoved;
		case AnimationLifecycleBoundary::Scene: return AnimationCancellationReason::SceneUnloaded;
		case AnimationLifecycleBoundary::Asset: return AnimationCancellationReason::AssetReloaded;
		case AnimationLifecycleBoundary::ScriptDomain: return AnimationCancellationReason::ScriptReloaded;
		case AnimationLifecycleBoundary::PlayMode: return AnimationCancellationReason::PlayModeStopped;
		case AnimationLifecycleBoundary::PhysicsWorld: return AnimationCancellationReason::PhysicsWorldReplaced;
		case AnimationLifecycleBoundary::Renderer: return AnimationCancellationReason::RendererRecovered;
		case AnimationLifecycleBoundary::Device: return AnimationCancellationReason::DeviceLost;
		case AnimationLifecycleBoundary::Shutdown: return AnimationCancellationReason::Shutdown;
		case AnimationLifecycleBoundary::Count: break;
		}
		return AnimationCancellationReason::OwnerReleased;
	}

	std::string_view ToString(AnimationLifecycleBoundary boundary) noexcept
	{
		switch(boundary) {
		case AnimationLifecycleBoundary::Object: return "Object";
		case AnimationLifecycleBoundary::Component: return "Component";
		case AnimationLifecycleBoundary::Scene: return "Scene";
		case AnimationLifecycleBoundary::Asset: return "Asset";
		case AnimationLifecycleBoundary::ScriptDomain: return "ScriptDomain";
		case AnimationLifecycleBoundary::PlayMode: return "PlayMode";
		case AnimationLifecycleBoundary::PhysicsWorld: return "PhysicsWorld";
		case AnimationLifecycleBoundary::Renderer: return "Renderer";
		case AnimationLifecycleBoundary::Device: return "Device";
		case AnimationLifecycleBoundary::Shutdown: return "Shutdown";
		case AnimationLifecycleBoundary::Count: break;
		}
		return "InvalidAnimationLifecycleBoundary";
	}

	std::string_view ToString(AnimationCancellationOperation operation) noexcept
	{
		switch(operation) {
		case AnimationCancellationOperation::Import: return "Import";
		case AnimationCancellationOperation::CpuEvaluation: return "CpuEvaluation";
		case AnimationCancellationOperation::GpuBatch: return "GpuBatch";
		case AnimationCancellationOperation::Recording: return "Recording";
		case AnimationCancellationOperation::Reload: return "Reload";
		case AnimationCancellationOperation::ScenePublication: return "ScenePublication";
		case AnimationCancellationOperation::PhysicsHandoff: return "PhysicsHandoff";
		case AnimationCancellationOperation::ManagedDelivery: return "ManagedDelivery";
		case AnimationCancellationOperation::Shutdown: return "Shutdown";
		case AnimationCancellationOperation::Count: break;
		}
		return "InvalidAnimationCancellationOperation";
	}

	std::string_view ToString(AnimationCancellationReason reason) noexcept
	{
		switch(reason) {
		case AnimationCancellationReason::None: return "None";
		case AnimationCancellationReason::Explicit: return "Explicit";
		case AnimationCancellationReason::OwnerReleased: return "OwnerReleased";
		case AnimationCancellationReason::ObjectReused: return "ObjectReused";
		case AnimationCancellationReason::ComponentRemoved: return "ComponentRemoved";
		case AnimationCancellationReason::SceneUnloaded: return "SceneUnloaded";
		case AnimationCancellationReason::AssetReloaded: return "AssetReloaded";
		case AnimationCancellationReason::ScriptReloaded: return "ScriptReloaded";
		case AnimationCancellationReason::PlayModeStopped: return "PlayModeStopped";
		case AnimationCancellationReason::PhysicsWorldReplaced: return "PhysicsWorldReplaced";
		case AnimationCancellationReason::RendererRecovered: return "RendererRecovered";
		case AnimationCancellationReason::DeviceLost: return "DeviceLost";
		case AnimationCancellationReason::Shutdown: return "Shutdown";
		case AnimationCancellationReason::GenerationExhausted: return "GenerationExhausted";
		}
		return "InvalidAnimationCancellationReason";
	}
}
