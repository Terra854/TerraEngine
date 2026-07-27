// PoseBlend.cpp - Checked deterministic pose blending and crossfade progression.

#include "Animation/Evaluation/PoseBlend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace TerraEngine::Animation {
	namespace {
		constexpr double kQuaternionLengthEpsilon = 1.0e-20;

		bool IsSupported(PoseCrossfadeDurationMode mode) noexcept {
			switch(mode) {
			case PoseCrossfadeDurationMode::FixedTicks:
			case PoseCrossfadeDurationMode::NormalizedSource:
				return true;
			}
			return false;
		}

		bool IsSupported(PoseCrossfadeCurve curve) noexcept {
			switch(curve) {
			case PoseCrossfadeCurve::Linear:
			case PoseCrossfadeCurve::SmoothStep:
			case PoseCrossfadeCurve::EaseInQuadratic:
			case PoseCrossfadeCurve::EaseOutQuadratic:
				return true;
			}
			return false;
		}

		bool IsSupported(PoseCrossfadeSyncMode mode) noexcept {
			switch(mode) {
			case PoseCrossfadeSyncMode::Independent:
			case PoseCrossfadeSyncMode::PreserveNormalizedPhase:
				return true;
			}
			return false;
		}

		bool IsSupported(PoseCrossfadeInterruptionPolicy policy) noexcept {
			switch(policy) {
			case PoseCrossfadeInterruptionPolicy::RejectWhileActive:
			case PoseCrossfadeInterruptionPolicy::RestartFromCurrentPose:
				return true;
			}
			return false;
		}

		// Wrapping finite values in double precision keeps very large authored
		// offsets bounded before they reach clip sampling arithmetic.
		double WrapNormalized(double value) noexcept {
			double wrapped = std::fmod(value, 1.0);
			if(wrapped < 0.0) {
				wrapped += 1.0;
			}
			return wrapped;
		}

		double ResolveNormalized(double value, bool loop) noexcept {
			return loop ? WrapNormalized(value) : std::clamp(value, 0.0, 1.0);
		}

		AnimationQuaternion Negate(AnimationQuaternion value) noexcept {
			return {-value.x, -value.y, -value.z, -value.w};
		}

		double Dot(AnimationQuaternion left, AnimationQuaternion right) noexcept {
			return static_cast<double>(left.x) * right.x
				+ static_cast<double>(left.y) * right.y
				+ static_cast<double>(left.z) * right.z
				+ static_cast<double>(left.w) * right.w;
		}

		bool TryNormalize(AnimationQuaternion value, AnimationQuaternion& output) noexcept {
			if(!IsFinite(value)) {
				return false;
			}
			const double lengthSquared = Dot(value, value);
			if(!std::isfinite(lengthSquared) || lengthSquared <= kQuaternionLengthEpsilon) {
				return false;
			}
			const double inverseLength = 1.0 / std::sqrt(lengthSquared);
			output = {
				static_cast<float>(value.x * inverseLength),
				static_cast<float>(value.y * inverseLength),
				static_cast<float>(value.z * inverseLength),
				static_cast<float>(value.w * inverseLength)
			};
			return IsFinite(output);
		}

		// q and -q encode the same rotation. Canonicalizing the first positive-weight
		// input gives all later hemisphere tests one deterministic anchor.
		AnimationQuaternion Canonicalize(AnimationQuaternion value) noexcept {
			const bool negate = value.w < 0.0F
				|| (value.w == 0.0F && value.x < 0.0F)
				|| (value.w == 0.0F && value.x == 0.0F && value.y < 0.0F)
				|| (value.w == 0.0F && value.x == 0.0F && value.y == 0.0F && value.z < 0.0F);
			return negate ? Negate(value) : value;
		}

		bool TryNormalizeTransform(AnimationTransform value, AnimationTransform& output) noexcept {
			if(!IsFinite(value.translation) || !IsFinite(value.scale)) {
				return false;
			}
			AnimationQuaternion rotation;
			if(!TryNormalize(value.rotation, rotation)) {
				return false;
			}
			output = value;
			output.rotation = Canonicalize(rotation);
			return true;
		}

		float ApplyCurve(PoseCrossfadeCurve curve, float progress) noexcept {
			const float clamped = std::clamp(progress, 0.0F, 1.0F);
			switch(curve) {
			case PoseCrossfadeCurve::Linear:
				return clamped;
			case PoseCrossfadeCurve::SmoothStep:
				return clamped * clamped * (3.0F - 2.0F * clamped);
			case PoseCrossfadeCurve::EaseInQuadratic:
				return clamped * clamped;
			case PoseCrossfadeCurve::EaseOutQuadratic:
				return clamped * (2.0F - clamped);
			}
			return clamped;
		}

		AnimationFixedTick NormalizedToTick(
			double normalized,
			AnimationFixedTick duration,
			bool loop) noexcept {
			if(duration <= 0) {
				return 0;
			}
			const double resolved = ResolveNormalized(normalized, loop);
			const long double scaled = static_cast<long double>(resolved)
				* static_cast<long double>(duration);
			// Normalized values are presentation-friendly floats, but the playback
			// clock is integer. Round to the nearest fixed tick so 29/100 returns 29
			// even after a float representation round-trip instead of flooring to 28.
			const AnimationFixedTick tick = static_cast<AnimationFixedTick>(std::llround(scaled));
			const AnimationFixedTick finalTick = loop ? duration - 1 : duration;
			return std::clamp<AnimationFixedTick>(tick, 0, finalTick);
		}

		AnimationStatus ResolveDuration(
			const PoseCrossfadeRequest& request,
			AnimationFixedTick& output) noexcept {
			if(!IsSupported(request.durationMode) || !IsSupported(request.curve)
				|| !IsSupported(request.syncMode) || !IsSupported(request.interruptionPolicy)
				|| request.sourceDurationTicks < 0 || request.destinationDurationTicks < 0
				|| request.durationTicks < 0 || !std::isfinite(request.normalizedDuration)
				|| request.normalizedDuration < 0.0F
				|| !std::isfinite(request.initialNormalizedPhase)
				|| !std::isfinite(request.sourceOffsetNormalized)
				|| !std::isfinite(request.destinationOffsetNormalized)) {
				return AnimationStatus::InvalidArgument;
			}
			const AnimationFixedTick maximumClipDuration = static_cast<AnimationFixedTick>(
				GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
					* kAnimationFixedTicksPerSecond);
			if(request.sourceDurationTicks > maximumClipDuration
				|| request.destinationDurationTicks > maximumClipDuration) {
				return AnimationStatus::HardLimitExceeded;
			}

			if(request.durationMode == PoseCrossfadeDurationMode::FixedTicks) {
				output = request.durationTicks;
				return AnimationStatus::Ok;
			}

			const long double scaled = static_cast<long double>(request.normalizedDuration)
				* static_cast<long double>(request.sourceDurationTicks);
			if(!std::isfinite(scaled)
				|| scaled > static_cast<long double>(std::numeric_limits<AnimationFixedTick>::max())) {
				return AnimationStatus::RangeOverflow;
			}
			output = static_cast<AnimationFixedTick>(std::llround(scaled));
			return AnimationStatus::Ok;
		}
	}

	AnimationStatus BlendLocalPoses(
		std::span<const WeightedLocalPose> inputs,
		std::span<const AnimationTransform> fallbackPose,
		std::vector<AnimationTransform>& output) noexcept {
		try {
			std::vector<WeightedLocalPose> ordered(inputs.begin(), inputs.end());
			std::sort(ordered.begin(), ordered.end(), [](const WeightedLocalPose& left, const WeightedLocalPose& right) {
				return left.stableOrder < right.stableOrder;
			});

			for(std::size_t index = 0U; index < ordered.size(); ++index) {
				const WeightedLocalPose& input = ordered[index];
				if(!std::isfinite(input.weight) || input.weight < 0.0F
					|| input.localTransforms.size() != fallbackPose.size()) {
					return AnimationStatus::InvalidArgument;
				}
				if(index > 0U && ordered[index - 1U].stableOrder == input.stableOrder) {
					return AnimationStatus::InvalidArgument;
				}
			}

			// Normalize and validate the fallback up front. It is the safe result for
			// zero weight and must never smuggle a non-finite transform into output.
			std::vector<AnimationTransform> fallbackCandidate(fallbackPose.size());
			for(std::size_t transformIndex = 0U; transformIndex < fallbackPose.size(); ++transformIndex) {
				if(!TryNormalizeTransform(fallbackPose[transformIndex], fallbackCandidate[transformIndex])) {
					return AnimationStatus::InvalidArgument;
				}
			}

			double totalWeight = 0.0;
			for(const WeightedLocalPose& input : ordered) {
				totalWeight += static_cast<double>(input.weight);
				for(const AnimationTransform& transform : input.localTransforms) {
					AnimationTransform validated;
					if(!TryNormalizeTransform(transform, validated)) {
						return AnimationStatus::InvalidArgument;
					}
				}
			}
			if(!std::isfinite(totalWeight)) {
				return AnimationStatus::RangeOverflow;
			}
			if(totalWeight == 0.0) {
				output = std::move(fallbackCandidate);
				return AnimationStatus::Ok;
			}

			std::vector<AnimationTransform> candidate(fallbackPose.size());
			for(std::size_t transformIndex = 0U; transformIndex < fallbackPose.size(); ++transformIndex) {
				double translationX = 0.0;
				double translationY = 0.0;
				double translationZ = 0.0;
				double scaleX = 0.0;
				double scaleY = 0.0;
				double scaleZ = 0.0;
				double rotationX = 0.0;
				double rotationY = 0.0;
				double rotationZ = 0.0;
				double rotationW = 0.0;
				AnimationQuaternion rotationAnchor{};
				bool hasRotationAnchor = false;

				for(const WeightedLocalPose& input : ordered) {
					if(input.weight == 0.0F) {
						continue;
					}
					const double normalizedWeight = static_cast<double>(input.weight) / totalWeight;
					const AnimationTransform& transform = input.localTransforms[transformIndex];
					AnimationQuaternion rotation;
					TryNormalize(transform.rotation, rotation);
					rotation = Canonicalize(rotation);
					if(!hasRotationAnchor) {
						rotationAnchor = rotation;
						hasRotationAnchor = true;
					} else if(Dot(rotationAnchor, rotation) < 0.0) {
						rotation = Negate(rotation);
					}

					translationX += static_cast<double>(transform.translation.x) * normalizedWeight;
					translationY += static_cast<double>(transform.translation.y) * normalizedWeight;
					translationZ += static_cast<double>(transform.translation.z) * normalizedWeight;
					scaleX += static_cast<double>(transform.scale.x) * normalizedWeight;
					scaleY += static_cast<double>(transform.scale.y) * normalizedWeight;
					scaleZ += static_cast<double>(transform.scale.z) * normalizedWeight;
					rotationX += static_cast<double>(rotation.x) * normalizedWeight;
					rotationY += static_cast<double>(rotation.y) * normalizedWeight;
					rotationZ += static_cast<double>(rotation.z) * normalizedWeight;
					rotationW += static_cast<double>(rotation.w) * normalizedWeight;
				}

				AnimationTransform blended;
				blended.translation = {
					static_cast<float>(translationX),
					static_cast<float>(translationY),
					static_cast<float>(translationZ)
				};
				blended.scale = {
					static_cast<float>(scaleX),
					static_cast<float>(scaleY),
					static_cast<float>(scaleZ)
				};
				if(!TryNormalize({
					static_cast<float>(rotationX),
					static_cast<float>(rotationY),
					static_cast<float>(rotationZ),
					static_cast<float>(rotationW)
				}, blended.rotation) || !IsFinite(blended)) {
					return AnimationStatus::InvalidArgument;
				}
				blended.rotation = Canonicalize(blended.rotation);
				candidate[transformIndex] = blended;
			}

			output = std::move(candidate);
			return AnimationStatus::Ok;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		} catch(...) {
			return AnimationStatus::InternalError;
		}
	}

	AnimationStatus PoseCrossfadeState::Begin(const PoseCrossfadeRequest& request) noexcept {
		return BeginAtPhase(request, request.initialNormalizedPhase);
	}

	AnimationStatus PoseCrossfadeState::BeginAtPhase(
		const PoseCrossfadeRequest& request,
		float initialPhase) noexcept {
		if(!std::isfinite(initialPhase)) {
			return AnimationStatus::InvalidArgument;
		}
		AnimationFixedTick resolvedDuration = 0;
		const AnimationStatus status = ResolveDuration(request, resolvedDuration);
		if(status != AnimationStatus::Ok) {
			return status;
		}

		// Assign only after all validation succeeds so an invalid Begin cannot
		// partially replace a last-known-good transition.
		request_ = request;
		resolvedDurationTicks_ = resolvedDuration;
		elapsedTicks_ = 0;
		initialPhase_ = static_cast<float>(ResolveNormalized(initialPhase, request.sourceLoop));
		initialized_ = true;
		active_ = resolvedDuration > 0;
		usesCapturedSourcePose_ = false;
		capturedSourcePose_.clear();
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseCrossfadeState::Advance(AnimationFixedTick deltaTicks) noexcept {
		if(!initialized_) {
			return AnimationStatus::NotReady;
		}
		if(deltaTicks < 0) {
			return AnimationStatus::InvalidArgument;
		}
		if(!active_) {
			return AnimationStatus::Ok;
		}

		const AnimationFixedTick remaining = resolvedDurationTicks_ - elapsedTicks_;
		elapsedTicks_ += std::min(deltaTicks, remaining);
		active_ = elapsedTicks_ < resolvedDurationTicks_;
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseCrossfadeState::Sample(PoseCrossfadeSample& output) const noexcept {
		if(!initialized_) {
			return AnimationStatus::NotReady;
		}

		PoseCrossfadeSample candidate;
		candidate.elapsedTicks = elapsedTicks_;
		candidate.durationTicks = resolvedDurationTicks_;
		candidate.linearProgress = resolvedDurationTicks_ == 0
			? 1.0F
			: static_cast<float>(static_cast<double>(elapsedTicks_)
				/ static_cast<double>(resolvedDurationTicks_));
		candidate.linearProgress = std::clamp(candidate.linearProgress, 0.0F, 1.0F);
		candidate.destinationWeight = ApplyCurve(request_.curve, candidate.linearProgress);
		candidate.sourceWeight = 1.0F - candidate.destinationWeight;
		candidate.complete = !active_;

		const double sourceAdvance = request_.sourceDurationTicks > 0
			? static_cast<double>(elapsedTicks_) / static_cast<double>(request_.sourceDurationTicks)
			: 0.0;
		const double destinationAdvance = request_.destinationDurationTicks > 0
			? static_cast<double>(elapsedTicks_) / static_cast<double>(request_.destinationDurationTicks)
			: 0.0;
		const double unboundedSourcePhase = static_cast<double>(initialPhase_) + sourceAdvance;
		const double synchronizationPhase = ResolveNormalized(
			unboundedSourcePhase, request_.sourceLoop);
		const double sourceNormalizedTime = ResolveNormalized(
			unboundedSourcePhase + request_.sourceOffsetNormalized, request_.sourceLoop);
		candidate.synchronizationPhase = static_cast<float>(synchronizationPhase);
		candidate.sourceNormalizedTime = static_cast<float>(sourceNormalizedTime);
		const double destinationBase = request_.syncMode == PoseCrossfadeSyncMode::PreserveNormalizedPhase
			? synchronizationPhase
			: static_cast<double>(initialPhase_) + destinationAdvance;
		const double destinationNormalizedTime = ResolveNormalized(
			destinationBase + request_.destinationOffsetNormalized, request_.destinationLoop);
		candidate.destinationNormalizedTime = static_cast<float>(destinationNormalizedTime);
		candidate.sourceSampleTick = NormalizedToTick(
			sourceNormalizedTime, request_.sourceDurationTicks, request_.sourceLoop);
		candidate.destinationSampleTick = NormalizedToTick(
			destinationNormalizedTime, request_.destinationDurationTicks, request_.destinationLoop);

		if(!std::isfinite(candidate.synchronizationPhase)
			|| !std::isfinite(candidate.sourceNormalizedTime)
			|| !std::isfinite(candidate.destinationNormalizedTime)
			|| !std::isfinite(candidate.sourceWeight)
			|| !std::isfinite(candidate.destinationWeight)) {
			return AnimationStatus::InvalidArgument;
		}
		output = candidate;
		return AnimationStatus::Ok;
	}

	AnimationStatus PoseCrossfadeState::Blend(
		std::span<const AnimationTransform> sourcePose,
		std::span<const AnimationTransform> destinationPose,
		std::span<const AnimationTransform> fallbackPose,
		std::vector<AnimationTransform>& output) const noexcept {
		PoseCrossfadeSample sample;
		const AnimationStatus sampleStatus = Sample(sample);
		if(sampleStatus != AnimationStatus::Ok) {
			return sampleStatus;
		}
		const std::span<const AnimationTransform> effectiveSource = usesCapturedSourcePose_
			? std::span<const AnimationTransform>(capturedSourcePose_)
			: sourcePose;
		const WeightedLocalPose weightedPoses[] = {
			{effectiveSource, sample.sourceWeight, 0U},
			{destinationPose, sample.destinationWeight, 1U}
		};
		return BlendLocalPoses(weightedPoses, fallbackPose, output);
	}

	AnimationStatus PoseCrossfadeState::Interrupt(
		const PoseCrossfadeRequest& request,
		std::span<const AnimationTransform> currentSourcePose,
		std::span<const AnimationTransform> currentDestinationPose,
		std::span<const AnimationTransform> fallbackPose) noexcept {
		if(!initialized_) {
			return AnimationStatus::NotReady;
		}
		if(active_ && request.interruptionPolicy == PoseCrossfadeInterruptionPolicy::RejectWhileActive) {
			return AnimationStatus::Rejected;
		}

		PoseCrossfadeSample previousSample;
		const AnimationStatus sampleStatus = Sample(previousSample);
		if(sampleStatus != AnimationStatus::Ok) {
			return sampleStatus;
		}
		std::vector<AnimationTransform> capturedPose;
		const AnimationStatus blendStatus = Blend(
			currentSourcePose, currentDestinationPose, fallbackPose, capturedPose);
		if(blendStatus != AnimationStatus::Ok) {
			return blendStatus;
		}

		PoseCrossfadeState candidate;
		const AnimationStatus beginStatus = candidate.BeginAtPhase(
			request, previousSample.synchronizationPhase);
		if(beginStatus != AnimationStatus::Ok) {
			return beginStatus;
		}
		candidate.capturedSourcePose_ = std::move(capturedPose);
		candidate.usesCapturedSourcePose_ = true;
		*this = std::move(candidate);
		return AnimationStatus::Ok;
	}
}
