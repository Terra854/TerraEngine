// AnimationEvaluationCache.cpp - Bounded, synchronized animation result reuse.

#include "Animation/Runtime/AnimationEvaluationCache.h"

#include <algorithm>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		inline constexpr std::size_t kAbsoluteMaximumAnimationCacheEntries = 65536U;
		inline constexpr std::size_t kAbsoluteMaximumAnimationCacheBatchRequests = 1048576U;
		inline constexpr std::size_t kAbsoluteMaximumAnimationCacheWatermarks = 1048576U;

		AnimationStatus ValidateConfiguration(
			const AnimationEvaluationCacheConfiguration& configuration) noexcept
		{
			if(configuration.maximumEntries == 0U || configuration.maximumBatchRequests == 0U
				|| configuration.maximumAnimatorWatermarks == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(configuration.maximumEntries > kAbsoluteMaximumAnimationCacheEntries
				|| configuration.maximumBatchRequests > kAbsoluteMaximumAnimationCacheBatchRequests
				|| configuration.maximumAnimatorWatermarks > kAbsoluteMaximumAnimationCacheWatermarks) {
				return AnimationStatus::HardLimitExceeded;
			}
			return AnimationStatus::Ok;
		}

		bool IsCompleteKey(const AnimationEvaluationCacheKey& key) noexcept
		{
			return key.animator.IsValid()
				&& key.stateGeneration != 0U
				&& key.parameterGeneration != 0U
				&& key.consumerGeneration != 0U
				&& key.executionModeGeneration != 0U
				&& key.poseGeneration != 0U
				&& key.morphGeneration != 0U
				&& key.graphGeneration != 0U
				&& key.viewGeneration != 0U
				&& key.assetGeneration != 0U
				&& key.residencyGeneration != 0U
				&& key.materialGeneration != 0U
				&& key.rendererGeneration != 0U
				&& key.physicsGeneration != 0U
				&& key.deterministicGeneration != 0U;
		}

		bool TryReadGeneration(
			const AnimationEvaluationCacheKey& key,
			AnimationEvaluationCacheGenerationDomain domain,
			std::uint64_t& output) noexcept
		{
			switch(domain) {
			case AnimationEvaluationCacheGenerationDomain::Pose: output = key.poseGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Morph: output = key.morphGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Graph: output = key.graphGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::View: output = key.viewGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Asset: output = key.assetGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Residency: output = key.residencyGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Material: output = key.materialGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Renderer: output = key.rendererGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Physics: output = key.physicsGeneration; return true;
			case AnimationEvaluationCacheGenerationDomain::Deterministic: output = key.deterministicGeneration; return true;
			}
			return false;
		}
	}

	bool AnimationEvaluationCacheRequirements::HasAuthoritativeWork() const noexcept
	{
		return timeAdvances || events || rootMotion || physics || recording
			|| replication || rollback || deterministic || automation || detailedDiagnostics;
	}

	struct AnimationEvaluationCache::Impl {
		struct Entry {
			std::shared_ptr<const AnimationEvaluationCacheValue> value;
			std::uint64_t lastTouch = 0U;
		};

		struct AnimatorState {
			std::uint64_t latestSourceFrameGeneration = 0U;
			std::size_t entryCount = 0U;
		};

		explicit Impl(AnimationEvaluationCacheConfiguration requestedConfiguration)
			: configuration(requestedConfiguration)
		{
		}

		std::uint64_t NextTouchLocked()
		{
			if(touchCounter != std::numeric_limits<std::uint64_t>::max()) return ++touchCounter;

			// A 64-bit wrap is practically unreachable, but deterministic eviction must
			// still remain defined. Rebase existing entries in prior LRU/key order.
			std::vector<decltype(entries)::iterator> ordered;
			ordered.reserve(entries.size());
			for(auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
				ordered.push_back(iterator);
			}
			std::sort(ordered.begin(), ordered.end(), [](const auto left, const auto right) {
				if(left->second.lastTouch != right->second.lastTouch) {
					return left->second.lastTouch < right->second.lastTouch;
				}
				return left->first < right->first;
			});
			std::uint64_t rebased = 0U;
			for(const auto iterator : ordered) iterator->second.lastTouch = ++rebased;
			touchCounter = rebased;
			return ++touchCounter;
		}

		AnimationEvaluationCacheLookup LookupLocked(
			const AnimationEvaluationCacheKey& key,
			const AnimationEvaluationCacheRequirements& requirements)
		{
			AnimationEvaluationCacheLookup result;
			if(!IsCompleteKey(key)) {
				result.status = AnimationStatus::InvalidArgument;
				result.disposition = AnimationEvaluationCacheDisposition::InvalidKey;
				return result;
			}

			const bool authoritative = requirements.HasAuthoritativeWork();
			auto found = entries.find(key);
			if(found != entries.end()) {
				found->second.lastTouch = NextTouchLocked();
				result.value = found->second.value;
				if(!authoritative) {
					result.status = AnimationStatus::Ok;
					result.disposition = AnimationEvaluationCacheDisposition::Reused;
					return result;
				}

				// The prior immutable result may be retained as a hold-last fallback, but
				// authority explicitly requires a new evaluation and protected publish.
				result.status = AnimationStatus::NotReady;
				result.disposition = AnimationEvaluationCacheDisposition::RequiredWork;
				result.requiresEvaluation = true;
				result.requiresPublication = true;
				result.requiresUpload = requirements.visible;
				result.requiresDetailedDiagnostics = requirements.detailedDiagnostics;
				return result;
			}

			result.status = AnimationStatus::NotReady;
			result.disposition = AnimationEvaluationCacheDisposition::Miss;
			result.requiresEvaluation = requirements.visible || authoritative;
			result.requiresPublication = requirements.visible || authoritative;
			result.requiresUpload = requirements.visible;
			result.requiresVisibilityInvalidation = !requirements.visible && !authoritative;
			result.requiresDetailedDiagnostics = requirements.detailedDiagnostics;
			return result;
		}

		void EraseAnimatorEntriesLocked(AnimatorInstanceHandle animator)
		{
			for(auto iterator = entries.begin(); iterator != entries.end();) {
				if(iterator->first.animator == animator) iterator = entries.erase(iterator);
				else ++iterator;
			}
			auto state = animatorStates.find(animator);
			if(state != animatorStates.end()) state->second.entryCount = 0U;
		}

		void EvictOldestLocked()
		{
			auto victim = entries.end();
			for(auto iterator = entries.begin(); iterator != entries.end(); ++iterator) {
				if(victim == entries.end()
					|| iterator->second.lastTouch < victim->second.lastTouch
					|| (iterator->second.lastTouch == victim->second.lastTouch
						&& iterator->first < victim->first)) victim = iterator;
			}
			if(victim == entries.end()) return;
			const AnimatorInstanceHandle evictedAnimator = victim->first.animator;
			entries.erase(victim);
			auto state = animatorStates.find(evictedAnimator);
			if(state == animatorStates.end()) return;
			if(state->second.entryCount != 0U) --state->second.entryCount;
			// Keep the source-frame watermark after payload eviction. A still-running
			// older worker must not reinsert a stale result merely because memory
			// pressure selected this animator's payload as the deterministic victim.
		}

		AnimationEvaluationCacheConfiguration configuration;
		mutable std::mutex mutex;
		std::map<AnimationEvaluationCacheKey, Entry> entries;
		std::map<AnimatorInstanceHandle, AnimatorState> animatorStates;
		std::uint64_t touchCounter = 0U;
	};

	AnimationEvaluationCache::AnimationEvaluationCache()
		: AnimationEvaluationCache(AnimationEvaluationCacheConfiguration{})
	{
	}

	AnimationEvaluationCache::AnimationEvaluationCache(
		AnimationEvaluationCacheConfiguration configuration)
		: impl_(std::make_unique<Impl>(configuration))
	{
	}

	AnimationEvaluationCache::~AnimationEvaluationCache() = default;
	AnimationEvaluationCache::AnimationEvaluationCache(AnimationEvaluationCache&&) noexcept = default;
	AnimationEvaluationCache& AnimationEvaluationCache::operator=(AnimationEvaluationCache&&) noexcept = default;

	AnimationStatus AnimationEvaluationCache::Store(
		const AnimationEvaluationCacheKey& key,
		const AnimationEvaluationCacheValue& value)
	{
		if(impl_ == nullptr) return AnimationStatus::InvalidArgument;
		const AnimationStatus configurationStatus = ValidateConfiguration(impl_->configuration);
		if(configurationStatus != AnimationStatus::Ok) return configurationStatus;
		if(!IsCompleteKey(key) || value.sourceFrameGeneration == 0U
			|| value.publicationGeneration == 0U) return AnimationStatus::InvalidArgument;

		// Allocate both immutable payload and map nodes before taking the cache lock.
		// Node insertion is allocation-free, which lets eviction and generation
		// replacement remain atomic even when the process is under memory pressure.
		std::shared_ptr<const AnimationEvaluationCacheValue> immutableValue;
		std::map<AnimationEvaluationCacheKey, Impl::Entry> stagedEntries;
		std::map<AnimatorInstanceHandle, Impl::AnimatorState> stagedStates;
		try {
			immutableValue = std::make_shared<const AnimationEvaluationCacheValue>(value);
			stagedEntries.emplace(key, Impl::Entry{immutableValue, 0U});
			stagedStates.emplace(key.animator,
				Impl::AnimatorState{value.sourceFrameGeneration, 0U});
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		auto stagedEntry = stagedEntries.extract(stagedEntries.begin());
		auto stagedState = stagedStates.extract(stagedStates.begin());

		std::scoped_lock lock(impl_->mutex);
		std::uint64_t nextTouch = 0U;
		try {
			// Rebase, if ever required, before erasing or replacing any payload so an
			// allocation failure still leaves the prior cache generation untouched.
			nextTouch = impl_->NextTouchLocked();
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		auto state = impl_->animatorStates.find(key.animator);
		if(state != impl_->animatorStates.end()) {
			if(value.sourceFrameGeneration < state->second.latestSourceFrameGeneration) {
				return AnimationStatus::StaleHandle;
			}
			if(value.sourceFrameGeneration > state->second.latestSourceFrameGeneration) {
				impl_->EraseAnimatorEntriesLocked(key.animator);
				state->second.latestSourceFrameGeneration = value.sourceFrameGeneration;
			}
		} else {
			if(impl_->animatorStates.size() >= impl_->configuration.maximumAnimatorWatermarks) {
				return AnimationStatus::HardLimitExceeded;
			}
			state = impl_->animatorStates.insert(std::move(stagedState)).position;
		}

		auto found = impl_->entries.find(key);
		if(found != impl_->entries.end()) {
			found->second.value = immutableValue;
			found->second.lastTouch = nextTouch;
			return AnimationStatus::Ok;
		}

		stagedEntry.mapped().lastTouch = nextTouch;

		if(impl_->entries.size() >= impl_->configuration.maximumEntries) {
			impl_->EvictOldestLocked();
		}
		// Eviction can remove the last other view for the incoming animator. Re-find
		// its compact watermark before recording the newly inserted immutable view.
		state = impl_->animatorStates.find(key.animator);
		if(state == impl_->animatorStates.end()) {
			// Erasing payloads never erases their compact watermark, so reaching this
			// branch would indicate an internal invariant violation.
			return AnimationStatus::InternalError;
		}
		impl_->entries.insert(std::move(stagedEntry));
		++state->second.entryCount;
		return AnimationStatus::Ok;
	}

	AnimationEvaluationCacheLookup AnimationEvaluationCache::Lookup(
		const AnimationEvaluationCacheKey& key,
		const AnimationEvaluationCacheRequirements& requirements)
	{
		if(impl_ == nullptr) {
			AnimationEvaluationCacheLookup result;
			result.status = AnimationStatus::InvalidArgument;
			result.disposition = AnimationEvaluationCacheDisposition::InvalidKey;
			return result;
		}
		const AnimationStatus configurationStatus = ValidateConfiguration(impl_->configuration);
		if(configurationStatus != AnimationStatus::Ok) {
			AnimationEvaluationCacheLookup result;
			result.status = configurationStatus;
			result.disposition = AnimationEvaluationCacheDisposition::InvalidKey;
			return result;
		}
		std::scoped_lock lock(impl_->mutex);
		try {
			return impl_->LookupLocked(key, requirements);
		} catch(const std::bad_alloc&) {
			AnimationEvaluationCacheLookup result;
			result.status = AnimationStatus::InternalError;
			result.disposition = AnimationEvaluationCacheDisposition::Miss;
			result.requiresEvaluation = true;
			result.requiresPublication = true;
			return result;
		}
	}

	AnimationEvaluationCacheBatchSummary AnimationEvaluationCache::LookupBatch(
		std::span<const AnimationEvaluationCacheLookupRequest> requests,
		std::vector<AnimationEvaluationCacheLookup>& output)
	{
		AnimationEvaluationCacheBatchSummary summary;
		if(impl_ == nullptr) {
			summary.status = AnimationStatus::InvalidArgument;
			return summary;
		}
		const AnimationStatus configurationStatus = ValidateConfiguration(impl_->configuration);
		if(configurationStatus != AnimationStatus::Ok) {
			summary.status = configurationStatus;
			return summary;
		}
		if(requests.size() > impl_->configuration.maximumBatchRequests) {
			summary.status = AnimationStatus::HardLimitExceeded;
			return summary;
		}
		for(const AnimationEvaluationCacheLookupRequest& request : requests) {
			if(!IsCompleteKey(request.key)) {
				summary.status = AnimationStatus::InvalidArgument;
				return summary;
			}
		}

		try {
			std::vector<AnimationEvaluationCacheLookup> staged;
			staged.reserve(requests.size());
			std::scoped_lock lock(impl_->mutex);
			for(std::size_t index = 0U; index < requests.size(); ++index) {
				AnimationEvaluationCacheLookup lookup = impl_->LookupLocked(
					requests[index].key, requests[index].requirements);
				lookup.requestIndex = index;
				switch(lookup.disposition) {
				case AnimationEvaluationCacheDisposition::Reused: ++summary.reusedCount; break;
				case AnimationEvaluationCacheDisposition::RequiredWork: ++summary.requiredWorkCount; break;
				case AnimationEvaluationCacheDisposition::Miss: ++summary.missCount; break;
				case AnimationEvaluationCacheDisposition::InvalidKey:
					summary.status = AnimationStatus::InvalidArgument;
					return summary;
				}
				staged.push_back(std::move(lookup));
			}
			summary.requestCount = requests.size();
			output.swap(staged);
			return summary;
		} catch(const std::bad_alloc&) {
			// The caller keeps its prior complete batch; a partial lookup vector is
			// never published when optional cache bookkeeping cannot allocate.
			summary = {};
			summary.status = AnimationStatus::InternalError;
			return summary;
		}
	}

	std::size_t AnimationEvaluationCache::InvalidateGeneration(
		AnimationEvaluationCacheGenerationDomain domain,
		std::uint64_t generation)
	{
		if(impl_ == nullptr || generation == 0U) return 0U;
		std::uint64_t probe = 0U;
		if(!TryReadGeneration(AnimationEvaluationCacheKey{}, domain, probe)) return 0U;

		std::scoped_lock lock(impl_->mutex);
		std::size_t removed = 0U;
		for(auto iterator = impl_->entries.begin(); iterator != impl_->entries.end();) {
			std::uint64_t entryGeneration = 0U;
			(void)TryReadGeneration(iterator->first, domain, entryGeneration);
			if(entryGeneration != generation) {
				++iterator;
				continue;
			}
			auto state = impl_->animatorStates.find(iterator->first.animator);
			if(state != impl_->animatorStates.end() && state->second.entryCount != 0U) {
				--state->second.entryCount;
			}
			iterator = impl_->entries.erase(iterator);
			++removed;
		}
		// Generation invalidation removes payloads but deliberately retains source
		// watermarks. Animator destruction is the explicit lifetime boundary that
		// permits forgetting late-store history for that generation-safe handle.
		return removed;
	}

	std::size_t AnimationEvaluationCache::InvalidateAnimator(AnimatorInstanceHandle animator)
	{
		if(impl_ == nullptr || !animator.IsValid()) return 0U;
		std::scoped_lock lock(impl_->mutex);
		std::size_t removed = 0U;
		for(auto iterator = impl_->entries.begin(); iterator != impl_->entries.end();) {
			if(iterator->first.animator == animator) {
				iterator = impl_->entries.erase(iterator);
				++removed;
			} else {
				++iterator;
			}
		}
		impl_->animatorStates.erase(animator);
		return removed;
	}

	void AnimationEvaluationCache::InvalidateAll()
	{
		if(impl_ == nullptr) return;
		std::scoped_lock lock(impl_->mutex);
		impl_->entries.clear();
		impl_->animatorStates.clear();
		impl_->touchCounter = 0U;
	}

	std::size_t AnimationEvaluationCache::Size() const noexcept
	{
		if(impl_ == nullptr) return 0U;
		std::scoped_lock lock(impl_->mutex);
		return impl_->entries.size();
	}
}
