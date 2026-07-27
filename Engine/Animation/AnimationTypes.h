// AnimationTypes.h - Stable, provider-neutral animation identities and limits.
//
// Animation data crosses asset, scene, managed, rendering, and physics boundaries.
// The types in this file deliberately use fixed-width values so a saved asset or
// queued command has the same meaning regardless of the subsystem that reads it.

#pragma once

#include "Core/Export.h"
#include "Utilities/UUID.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace TerraEngine::Animation {
	using AnimationGeneration = std::uint32_t;
	using AnimationFixedTick = std::int64_t;
	using AnimationCompatibilityFingerprint = std::uint64_t;
	inline constexpr std::uint32_t kAnimationFixedTicksPerSecond = 60U;

	// A stable GUID names an authored asset, while generation distinguishes an
	// immutable reimport or hot-reload publication of that same authored asset.
	struct AnimationGuid {
		UUID value{};

		constexpr bool IsValid() const noexcept {
			for(const std::uint8_t byte : value) {
				if(byte != 0U) {
					return true;
				}
			}
			return false;
		}

		auto operator<=>(const AnimationGuid&) const = default;
	};

	// A handle is safe to retain across asynchronous work because consumers must
	// compare both its stable asset GUID and the published generation.
	struct AnimationAssetHandle {
		AnimationGuid assetId;
		AnimationGeneration generation = 0U;

		constexpr bool IsValid() const noexcept {
			return assetId.IsValid() && generation != 0U;
		}

		auto operator<=>(const AnimationAssetHandle&) const = default;
	};

	// Scene authoring stores only the requested policy. AnimationSystem resolves
	// one of the effective runtime modes from consumers and capabilities later;
	// append new requests without changing these durable numeric values.
	enum class AnimationExecutionPolicy : std::uint8_t {
		Auto = 0U,
		ForceGpuVisual = 1U,
		ForceHybridAuthoritative = 2U,
		ForceCpuDeterministicFallback = 3U
	};

	// These authored policy values are shared by AnimatorComponent, package
	// import, serializers, and managed bindings. Keeping the dependency-light
	// contract here lets offline asset tools emit prefab defaults without pulling
	// the live component/object hierarchy into their build targets.
	inline constexpr std::uint32_t kAnimatorComponentSchemaVersion = 3U;

	enum class AnimatorRootMotionPolicy : std::uint8_t {
		Ignore = 0U,
		ExtractOnly = 1U,
		ApplyToObject = 2U,
		ApplyToCharacter = 3U
	};

	// Update and time policies are separate so fixed-step scheduling remains
	// independent from the frame-time source selected by authored content.
	enum class AnimatorUpdatePolicy : std::uint8_t {
		EveryFrame = 0U,
		FixedStep = 1U,
		Manual = 2U
	};

	enum class AnimatorTimePolicy : std::uint8_t {
		Scaled = 0U,
		Unscaled = 1U,
		Fixed = 2U
	};

	// Event policy is durable intent only. Runtime cursors and retained event
	// records remain in the generation-safe animator instance.
	enum class AnimatorEventPolicy : std::uint8_t {
		Dispatch = 0U,
		Retain = 1U,
		Suppress = 2U
	};

	// Automatic culling preserves authority whenever gameplay, physics,
	// recording, or deterministic consumers still require animator updates.
	enum class AnimatorCullingPolicy : std::uint8_t {
		Automatic = 0U,
		AlwaysUpdate = 1U,
		SuspendVisualWhenInvisible = 2U
	};

	// The first values mirror the Animation ABI v1 contract shared with managed
	// callers. Append new statuses after them; never renumber an existing value.
	enum class AnimationStatus : std::uint16_t {
		Ok = 0U,
		Queued = 1U,
		NotReady = 2U,
		BufferTooSmall = 3U,
		InvalidArgument = 4U,
		InvalidType = 5U,
		MissingAsset = 6U,
		IncompatibleAsset = 7U,
		StaleHandle = 8U,
		Cancelled = 9U,
		Unsupported = 10U,
		DeterminismViolation = 11U,
		RendererUnavailable = 12U,
		Rejected = 13U,
		InternalError = 14U,
		UnsupportedMajorVersion = 15U,
		InvalidEnvelope = 16U,
		TruncatedInput = 17U,
		RangeOverflow = 18U,
		RangeOutOfBounds = 19U,
		HardLimitExceeded = 20U,
		UnknownRequiredChunk = 21U,
		InvalidChunk = 22U,
		ContentHashMismatch = 23U
	};

	// Version-1 limits are indexed by this append-only enum so diagnostics and
	// automation can query exactly which rejected bound was exceeded.
	enum class AnimationLimit : std::uint8_t {
		SkeletonJoints = 0U,
		PaletteJoints = 1U,
		RetainedInfluencesPerVertex = 2U,
		PackageSkinsOrSkeletons = 3U,
		PackageClips = 4U,
		ClipDurationSeconds = 5U,
		ClipChannels = 6U,
		ClipKeys = 7U,
		MeshMorphTargets = 8U,
		ControllerNodes = 9U,
		StateMachineDepth = 10U,
		SubgraphCallDepth = 11U,
		ControllerLayers = 12U,
		ControllerParameters = 13U,
		TransitionsPerState = 14U,
		ClipAuthoredEvents = 15U,
		AnimatorEventsPerUpdate = 16U,
		AnimatorRetainedEvents = 17U,
		AnimatorProceduralGoals = 18U,
		ChainIkIterations = 19U,
		MotionDatabaseSamples = 20U,
		MotionQueryTopK = 21U,
		RecordingDurationSeconds = 22U,
		RecordingSampleRateHz = 23U,
		RollbackHistoryTicks = 24U,
		DetailedDiagnosticHistoryFrames = 25U,
		ConcurrentAutomationJobs = 26U
	};

	struct AnimationLimitValues {
		AnimationLimit limit;
		std::uint64_t operationalDefault = 0U;
		std::uint64_t hardMaximum = 0U;
		std::string_view name;
	};

	inline constexpr std::array<AnimationLimitValues, 27U> kAnimationRuntimeLimits{{
		{AnimationLimit::SkeletonJoints, 256U, 4096U, "skeleton joints"},
		{AnimationLimit::PaletteJoints, 256U, 256U, "palette joints"},
		{AnimationLimit::RetainedInfluencesPerVertex, 4U, 4U, "retained influences per vertex"},
		{AnimationLimit::PackageSkinsOrSkeletons, 64U, 1024U, "package skins or skeletons"},
		{AnimationLimit::PackageClips, 256U, 4096U, "package clips"},
		{AnimationLimit::ClipDurationSeconds, 30U * 60U, 24U * 60U * 60U, "clip duration seconds"},
		{AnimationLimit::ClipChannels, 16384U, 65536U, "clip channels"},
		{AnimationLimit::ClipKeys, 4194304U, 16777216U, "clip keys"},
		{AnimationLimit::MeshMorphTargets, 256U, 4096U, "mesh morph targets"},
		{AnimationLimit::ControllerNodes, 8192U, 65536U, "controller nodes"},
		{AnimationLimit::StateMachineDepth, 16U, 64U, "state machine depth"},
		{AnimationLimit::SubgraphCallDepth, 8U, 32U, "subgraph call depth"},
		{AnimationLimit::ControllerLayers, 16U, 32U, "controller layers"},
		{AnimationLimit::ControllerParameters, 1024U, 4096U, "controller parameters"},
		{AnimationLimit::TransitionsPerState, 256U, 4096U, "transitions per state"},
		{AnimationLimit::ClipAuthoredEvents, 65536U, 1048576U, "clip authored events"},
		{AnimationLimit::AnimatorEventsPerUpdate, 1024U, 4096U, "animator events per update"},
		{AnimationLimit::AnimatorRetainedEvents, 2048U, 8192U, "animator retained events"},
		{AnimationLimit::AnimatorProceduralGoals, 64U, 256U, "animator procedural goals"},
		{AnimationLimit::ChainIkIterations, 32U, 128U, "chain IK iterations"},
		{AnimationLimit::MotionDatabaseSamples, 1000000U, 16777216U, "motion database samples"},
		{AnimationLimit::MotionQueryTopK, 32U, 256U, "motion query top-K"},
		{AnimationLimit::RecordingDurationSeconds, 30U * 60U, 4U * 60U * 60U, "recording duration seconds"},
		{AnimationLimit::RecordingSampleRateHz, 60U, 240U, "recording sample rate Hz"},
		{AnimationLimit::RollbackHistoryTicks, 240U, 4096U, "rollback history ticks"},
		{AnimationLimit::DetailedDiagnosticHistoryFrames, 600U, 36000U, "detailed diagnostic history frames"},
		{AnimationLimit::ConcurrentAutomationJobs, 16U, 64U, "concurrent durable automation jobs"}
	}};

	// Malformed ABI, automation, or serialized input can carry an arbitrary
	// numeric enum value. Keep lookup checked so such input cannot index beyond
	// the frozen table before a caller has a chance to return InvalidArgument.
	inline constexpr AnimationLimitValues kInvalidAnimationRuntimeLimit{
		static_cast<AnimationLimit>(0xffU), 0U, 0U, "invalid animation limit"
	};

	constexpr const AnimationLimitValues* TryGetAnimationRuntimeLimit(AnimationLimit limit) noexcept {
		const std::size_t index = static_cast<std::size_t>(limit);
		return index < kAnimationRuntimeLimits.size()
			? &kAnimationRuntimeLimits[index]
			: nullptr;
	}

	constexpr const AnimationLimitValues& GetAnimationRuntimeLimit(AnimationLimit limit) noexcept {
		const AnimationLimitValues* value = TryGetAnimationRuntimeLimit(limit);
		return value != nullptr ? *value : kInvalidAnimationRuntimeLimit;
	}

	constexpr bool IsWithinAnimationHardLimit(AnimationLimit limit, std::uint64_t value) noexcept {
		const AnimationLimitValues* configured = TryGetAnimationRuntimeLimit(limit);
		return configured != nullptr && value <= configured->hardMaximum;
	}

	// Callers use this result instead of silently clamping serialized counts. A
	// rejected value therefore preserves the previously published generation.
	constexpr AnimationStatus ValidateAnimationHardLimit(AnimationLimit limit, std::uint64_t value) noexcept {
		if(TryGetAnimationRuntimeLimit(limit) == nullptr) {
			return AnimationStatus::InvalidArgument;
		}
		return IsWithinAnimationHardLimit(limit, value)
			? AnimationStatus::Ok
			: AnimationStatus::HardLimitExceeded;
	}

	TERRAENGINE_API std::string_view ToString(AnimationStatus status) noexcept;
}
