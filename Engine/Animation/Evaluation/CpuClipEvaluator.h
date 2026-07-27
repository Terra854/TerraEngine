// CpuClipEvaluator.h - Stateless authoritative clip point sampler.
//
// AnimationSystem schedules different animator ranges as independent jobs. This
// evaluator owns no shared cursor or mutable cache, so random seeks and repeated
// samples produce the same result regardless of worker count or call order.

#pragma once

#include "Animation/Runtime/AnimationClipView.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace TerraEngine::Animation {
	struct CpuClipJointSample {
		std::uint64_t stableJointId = 0U;
		AnimationTransform localTransform{};
		std::array<AnimationChannelSampleSource, 3U> sources{
			AnimationChannelSampleSource::DefaultValue,
			AnimationChannelSampleSource::DefaultValue,
			AnimationChannelSampleSource::DefaultValue};
	};

	struct CpuClipNamedFloatSample {
		std::uint64_t stableId = 0U;
		float value = 0.0F;
		AnimationChannelSampleSource source = AnimationChannelSampleSource::DefaultValue;
	};

	// This is a point on the root-motion channels, not an accumulated interval.
	// RootMotion.cpp owns cycle accumulation and once-only consumption semantics.
	struct CpuClipRootMotionPoint {
		bool enabled = false;
		AnimationFloat3 translation{};
		float yawRadians = 0.0F;
		AnimationChannelSampleSource translationSource = AnimationChannelSampleSource::DefaultValue;
		AnimationChannelSampleSource yawSource = AnimationChannelSampleSource::DefaultValue;
	};

	struct CpuClipPointEvent {
		const AnimationClipEvent* event = nullptr;
		std::int64_t loopIndex = 0;
	};

	struct CpuClipEvaluationRequest {
		AnimationFixedTick tick = 0;
		bool loop = false;
		std::span<const std::uint64_t> jointStableIds;
		std::span<const std::uint64_t> morphStableIds;
		std::span<const std::uint64_t> curveStableIds;
		bool includeRootMotionPoint = true;
		bool includeEventsAtPoint = true;
	};

	struct CpuClipEvaluationResult {
		AnimationResolvedClipTime time{};
		std::vector<CpuClipJointSample> joints;
		std::vector<CpuClipNamedFloatSample> morphs;
		std::vector<CpuClipNamedFloatSample> curves;
		std::vector<CpuClipPointEvent> pointEvents;
		CpuClipRootMotionPoint rootMotion{};
		std::uint32_t fallbackChannelCount = 0U;
		std::uint32_t unavailableChannelCount = 0U;
	};

	class TERRAENGINE_API CpuClipEvaluator {
	public:
		AnimationStatus Evaluate(
			const AnimationClipView& view,
			const CpuClipEvaluationRequest& request,
			CpuClipEvaluationResult& output) const noexcept;

		AnimationStatus SampleJoint(
			const AnimationClipView& view,
			std::uint64_t stableJointId,
			const AnimationResolvedClipTime& time,
			CpuClipJointSample& output) const noexcept;
		AnimationStatus SampleNamedFloat(
			const AnimationClipView& view,
			std::uint64_t stableId,
			AnimationClipTrackKind kind,
			const AnimationResolvedClipTime& time,
			CpuClipNamedFloatSample& output) const noexcept;
		AnimationStatus SampleRootMotionPoint(
			const AnimationClipView& view,
			const AnimationResolvedClipTime& time,
			CpuClipRootMotionPoint& output) const noexcept;
		AnimationStatus CollectEventsAtPoint(
			const AnimationClipView& view,
			const AnimationResolvedClipTime& time,
			std::vector<CpuClipPointEvent>& output) const noexcept;
	};
}
