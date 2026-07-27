// AnimationPreviewController.h - Isolated, generation-safe editor preview worlds.
//
// The controller never receives an ObjectManager or component pointer. Asset
// preview copies immutable clip/skeleton inputs into a private world, while live
// preview accepts complete copied runtime snapshots through a distinct read-only
// path. Worker preparation and protected publication are split explicitly so a
// stale task can never replace a newer session generation.

#pragma once

#include "Animation/Assets/SkeletonAsset.h"
#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Runtime/AnimatorInstanceStore.h"
#include "Animation/Runtime/AnimatorPlayback.h"
#include "Utilities/UUID.h"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Animation {
	inline constexpr std::uint32_t kInvalidAnimationPreviewSessionSlot = 0xffffffffU;
	inline constexpr std::size_t kMaximumAnimationPreviewSessions = 16U;

	enum class AnimationPreviewEndReason : std::uint8_t {
		None = 0U,
		Explicit = 1U,
		SelectionChanged = 2U,
		PlayModeTransition = 3U,
		ClipGenerationChanged = 4U,
		InvalidClip = 5U,
		ControllerDestroyed = 6U
	};

	enum class AnimationPreviewSourceMode : std::uint8_t {
		IsolatedAsset = 0U,
		LiveInstanceReadOnly = 1U
	};

	// The preview controller always produces a complete CPU reference snapshot.
	// Renderer availability changes only how that output is presented; it never
	// turns a missing renderer into a fake successful GPU result.
	enum class AnimationPreviewEvaluationMode : std::uint8_t {
		CpuReference = 0U,
		CpuRendererFallback = 1U,
		LiveInstanceCopy = 2U
	};

	struct AnimationPreviewSessionHandle {
		std::uint32_t slotIndex = kInvalidAnimationPreviewSessionSlot;
		TerraEngine::Animation::AnimationGeneration generation = 0U;

		[[nodiscard]] constexpr bool IsValid() const noexcept {
			return slotIndex != kInvalidAnimationPreviewSessionSlot && generation != 0U;
		}

		auto operator<=>(const AnimationPreviewSessionHandle&) const = default;
	};

	// The descriptor copies durable identity and timing metadata from the asset or
	// selected live instance. Paths are informational; GUID plus generation is the
	// authoritative immutable identity used by stale-work checks.
	struct AnimationPreviewSessionDescriptor {
		UUID selectionId = EMPTY_UUID;
		TerraEngine::Animation::AnimationGuid clipAssetId{};
		std::uint64_t clipGeneration = 0U;
		TerraEngine::Animation::AnimationCompatibilityFingerprint compatibilityFingerprint = 0U;
		TerraEngine::Animation::AnimationFixedTick durationTicks = 0;
		std::uint32_t sampleRate = 0U;
		std::string displayName;
		std::filesystem::path compiledPath;
		bool looping = true;
		bool reverse = false;
		float speed = 1.0F;
		// Detached tool tabs may opt out of Inspector selection following. Play Mode
		// boundaries still retire isolated or live sessions as required.
		bool followSelection = true;

		auto operator<=>(const AnimationPreviewSessionDescriptor&) const = default;
	};

	// Asset input is copied in full before a session handle becomes visible. The
	// source vectors identify the bounded channels required by this preview; empty
	// lists are derived from the copied skeleton and clip track tables.
	struct AnimationPreviewAssetInput {
		TerraEngine::Animation::AnimationClipAsset clip;
		std::optional<TerraEngine::Animation::SkeletonAsset> referenceSkeleton;
		std::vector<std::uint8_t> residentSegments;
		std::vector<std::uint64_t> jointStableIds;
		std::vector<std::uint64_t> morphStableIds;
		std::vector<std::uint64_t> curveStableIds;
	};

	struct AnimationPreviewJointOutput {
		std::uint64_t stableJointId = 0U;
		TerraEngine::Animation::AnimationTransform localTransform{};
		std::array<TerraEngine::Animation::AnimationChannelSampleSource, 3U> sources{
			TerraEngine::Animation::AnimationChannelSampleSource::DefaultValue,
			TerraEngine::Animation::AnimationChannelSampleSource::DefaultValue,
			TerraEngine::Animation::AnimationChannelSampleSource::DefaultValue};

		auto operator<=>(const AnimationPreviewJointOutput&) const = default;
	};

	struct AnimationPreviewNamedFloatOutput {
		std::uint64_t stableId = 0U;
		float value = 0.0F;
		TerraEngine::Animation::AnimationChannelSampleSource source =
			TerraEngine::Animation::AnimationChannelSampleSource::DefaultValue;

		auto operator<=>(const AnimationPreviewNamedFloatOutput&) const = default;
	};

	// Every output is caller-owned by value. Point-event pointers produced by the
	// CPU evaluator are converted to complete event copies before publication.
	struct AnimationPreviewWorldOutput {
		TerraEngine::Animation::AnimationFixedTick tick = 0;
		std::int64_t loopIndex = 0;
		std::vector<AnimationPreviewJointOutput> joints;
		std::vector<AnimationPreviewNamedFloatOutput> morphs;
		std::vector<AnimationPreviewNamedFloatOutput> curves;
		std::vector<TerraEngine::Animation::AnimationClipEvent> events;
		bool rootMotionEnabled = false;
		TerraEngine::Animation::AnimationFloat3 rootTranslation{};
		float rootYawRadians = 0.0F;
		std::uint32_t fallbackChannelCount = 0U;
		std::uint32_t unavailableChannelCount = 0U;
		bool complete = false;

		auto operator<=>(const AnimationPreviewWorldOutput&) const = default;
	};

	struct AnimationPreviewLiveInstanceSnapshot {
		TerraEngine::Animation::AnimatorInstanceHandle instance;
		TerraEngine::Animation::AnimationGeneration poseGeneration = 0U;
		std::uint64_t publicationSequence = 0U;
		AnimationPreviewWorldOutput world;

		auto operator<=>(const AnimationPreviewLiveInstanceSnapshot&) const = default;
	};

	// PrepareTick is worker-safe: it captures one immutable source generation and
	// returns a detached candidate. PublishPreparedFrame is the protected boundary
	// that revalidates both handle and session revision before changing visibility.
	struct AnimationPreviewPreparedFrame {
		AnimationPreviewSessionHandle handle;
		std::uint64_t expectedSessionRevision = 0U;
		double unscaledDeltaSeconds = 0.0;
		TerraEngine::Animation::AnimatorPlaybackStateSnapshot playback;
		AnimationPreviewWorldOutput output;
		AnimationPreviewSourceMode sourceMode = AnimationPreviewSourceMode::IsolatedAsset;
		AnimationPreviewEvaluationMode evaluationMode = AnimationPreviewEvaluationMode::CpuReference;
		TerraEngine::Animation::AnimationStatus evaluationStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		bool rendererAvailable = false;
	};

	struct AnimationPreviewSessionState {
		bool active = false;
		AnimationPreviewSessionHandle handle;
		AnimationPreviewSessionDescriptor descriptor;
		AnimationPreviewSourceMode sourceMode = AnimationPreviewSourceMode::IsolatedAsset;
		AnimationPreviewEvaluationMode evaluationMode = AnimationPreviewEvaluationMode::CpuReference;
		TerraEngine::Animation::AnimatorPlaybackStateSnapshot playback;
		AnimationPreviewWorldOutput output;
		TerraEngine::Animation::AnimatorInstanceHandle liveInstance;
		TerraEngine::Animation::AnimationGeneration livePoseGeneration = 0U;
		std::uint64_t livePublicationSequence = 0U;
		std::uint64_t sessionRevision = 0U;
		TerraEngine::Animation::AnimationStatus latestStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		AnimationPreviewEndReason endReason = AnimationPreviewEndReason::None;
		bool rendererAvailable = false;
		bool readOnly = false;
	};

	struct AnimationPreviewTeardownResult {
		AnimationPreviewSessionHandle retiredHandle;
		AnimationPreviewEndReason reason = AnimationPreviewEndReason::None;
		AnimationPreviewSourceMode sourceMode = AnimationPreviewSourceMode::IsolatedAsset;
		AnimationPreviewWorldOutput initialOutput;
		AnimationPreviewWorldOutput lastOutput;
		AnimationPreviewWorldOutput restoredOutput;
		std::uint64_t authoredMutationCount = 0U;
		bool restored = false;
	};

	// This compact snapshot preserves the original Inspector-facing API. It is a
	// compatibility view over one transient cursor and never exposes world data.
	struct AnimationPreviewSnapshot {
		bool active = false;
		AnimationPreviewSessionDescriptor session;
		TerraEngine::Animation::AnimatorPlaybackStateSnapshot playback;
		TerraEngine::Animation::AnimationStatus latestStatus =
			TerraEngine::Animation::AnimationStatus::NotReady;
		AnimationPreviewEndReason endReason = AnimationPreviewEndReason::None;
		std::string safePoseReason;
	};

	class AnimationPreviewController final {
	public:
		AnimationPreviewController();
		~AnimationPreviewController();

		AnimationPreviewController(const AnimationPreviewController&) = delete;
		AnimationPreviewController& operator=(const AnimationPreviewController&) = delete;
		AnimationPreviewController(AnimationPreviewController&&) = delete;
		AnimationPreviewController& operator=(AnimationPreviewController&&) = delete;

		// Production isolated-world API.
		TerraEngine::Animation::AnimationStatus CreateIsolatedSession(
			const AnimationPreviewSessionDescriptor& descriptor,
			const AnimationPreviewAssetInput& input,
			bool rendererAvailable,
			AnimationPreviewSessionHandle& output) noexcept;
		TerraEngine::Animation::AnimationStatus SelectLiveInstance(
			const AnimationPreviewSessionDescriptor& descriptor,
			const AnimationPreviewLiveInstanceSnapshot& input,
			bool rendererAvailable,
			AnimationPreviewSessionHandle& output) noexcept;
		TerraEngine::Animation::AnimationStatus DestroySession(
			AnimationPreviewSessionHandle handle,
			AnimationPreviewEndReason reason = AnimationPreviewEndReason::Explicit,
			AnimationPreviewTeardownResult* teardown = nullptr) noexcept;
		void DestroyAllSessions(
			AnimationPreviewEndReason reason = AnimationPreviewEndReason::Explicit) noexcept;

		TerraEngine::Animation::AnimationStatus Play(AnimationPreviewSessionHandle handle) noexcept;
		TerraEngine::Animation::AnimationStatus Stop(AnimationPreviewSessionHandle handle) noexcept;
		TerraEngine::Animation::AnimationStatus Pause(AnimationPreviewSessionHandle handle) noexcept;
		TerraEngine::Animation::AnimationStatus Resume(AnimationPreviewSessionHandle handle) noexcept;
		TerraEngine::Animation::AnimationStatus Seek(
			AnimationPreviewSessionHandle handle,
			TerraEngine::Animation::AnimationFixedTick tick) noexcept;
		TerraEngine::Animation::AnimationStatus SeekNormalized(
			AnimationPreviewSessionHandle handle,
			float normalizedTime) noexcept;
		TerraEngine::Animation::AnimationStatus Step(
			AnimationPreviewSessionHandle handle,
			TerraEngine::Animation::AnimationFixedTick deltaTicks) noexcept;
		TerraEngine::Animation::AnimationStatus SetLooping(
			AnimationPreviewSessionHandle handle,
			bool looping) noexcept;
		TerraEngine::Animation::AnimationStatus SetReverse(
			AnimationPreviewSessionHandle handle,
			bool reverse) noexcept;
		TerraEngine::Animation::AnimationStatus SetSpeed(
			AnimationPreviewSessionHandle handle,
			float speed) noexcept;
		TerraEngine::Animation::AnimationStatus SetRendererAvailable(
			AnimationPreviewSessionHandle handle,
			bool available) noexcept;

		TerraEngine::Animation::AnimationStatus PrepareTick(
			AnimationPreviewSessionHandle handle,
			double unscaledDeltaSeconds,
			AnimationPreviewPreparedFrame& output) const noexcept;
		TerraEngine::Animation::AnimationStatus PublishPreparedFrame(
			const AnimationPreviewPreparedFrame& prepared) noexcept;
		TerraEngine::Animation::AnimationStatus TickSession(
			AnimationPreviewSessionHandle handle,
			double unscaledDeltaSeconds) noexcept;
		TerraEngine::Animation::AnimationStatus UpdateLiveInstance(
			AnimationPreviewSessionHandle handle,
			const AnimationPreviewLiveInstanceSnapshot& input) noexcept;

		[[nodiscard]] TerraEngine::Animation::AnimationStatus GetSessionState(
			AnimationPreviewSessionHandle handle,
			AnimationPreviewSessionState& output) const noexcept;
		[[nodiscard]] bool IsSessionAlive(AnimationPreviewSessionHandle handle) const noexcept;
		[[nodiscard]] std::size_t SessionCount() const noexcept;

		// Inspector compatibility API. These methods keep the current editor surface
		// source-compatible while window work migrates to generation-safe handles.
		TerraEngine::Animation::AnimationStatus BeginSession(
			const AnimationPreviewSessionDescriptor& descriptor) noexcept;
		void EndSession(AnimationPreviewEndReason reason = AnimationPreviewEndReason::Explicit) noexcept;
		TerraEngine::Animation::AnimationStatus Play() noexcept;
		TerraEngine::Animation::AnimationStatus Stop() noexcept;
		TerraEngine::Animation::AnimationStatus Pause() noexcept;
		TerraEngine::Animation::AnimationStatus Resume() noexcept;
		TerraEngine::Animation::AnimationStatus Seek(
			TerraEngine::Animation::AnimationFixedTick tick) noexcept;
		TerraEngine::Animation::AnimationStatus SeekNormalized(float normalizedTime) noexcept;
		TerraEngine::Animation::AnimationStatus SetLooping(bool looping) noexcept;
		TerraEngine::Animation::AnimationStatus SetReverse(bool reverse) noexcept;
		TerraEngine::Animation::AnimationStatus SetSpeed(float speed) noexcept;
		TerraEngine::Animation::AnimationStatus Tick(double unscaledDeltaSeconds) noexcept;
		void SynchronizeContext(const UUID& selectionId, bool playModeActive) noexcept;
		[[nodiscard]] AnimationPreviewSnapshot GetSnapshot() const noexcept;
		TerraEngine::Animation::AnimationStatus BeginIsolatedSession(
			std::uint64_t sessionId,
			const AnimationPreviewSessionDescriptor& descriptor) noexcept;
		TerraEngine::Animation::AnimationStatus TickIsolatedSessions(
			double unscaledDeltaSeconds) noexcept;
		[[nodiscard]] std::size_t IsolatedSessionCount() const noexcept;
		[[nodiscard]] bool IsActive() const noexcept;

	private:
		class Impl;
		std::unique_ptr<Impl> impl_;
	};
}
