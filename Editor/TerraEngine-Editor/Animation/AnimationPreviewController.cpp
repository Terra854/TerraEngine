// AnimationPreviewController.cpp - Private preview worlds and protected output publication.

#include "AnimationPreviewController.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>

namespace TerraEngine::Editor::Animation {
	using namespace TerraEngine::Animation;

	namespace {
		constexpr std::size_t kMaximumLiveEventPayloadBytes = 16U * 1024U;

		bool IsValidDescriptor(const AnimationPreviewSessionDescriptor& descriptor) noexcept {
			if(descriptor.selectionId == EMPTY_UUID
				|| !descriptor.clipAssetId.IsValid()
				|| descriptor.clipGeneration == 0U
				|| descriptor.compatibilityFingerprint == 0U
				|| descriptor.durationTicks < 0
				|| descriptor.sampleRate == 0U
				|| !std::isfinite(descriptor.speed)
				|| descriptor.speed < 0.0F) {
				return false;
			}
			const std::uint64_t maximumDurationTicks =
				GetAnimationRuntimeLimit(AnimationLimit::ClipDurationSeconds).hardMaximum
					* static_cast<std::uint64_t>(kAnimationFixedTicksPerSecond);
			return static_cast<std::uint64_t>(descriptor.durationTicks) <= maximumDurationTicks;
		}

		AnimationStatus ConfigurePlayback(
			const AnimationPreviewSessionDescriptor& descriptor,
			AnimatorPlayback& output) noexcept {
			AnimatorPlaybackConfiguration configuration;
			configuration.durationTicks = descriptor.durationTicks;
			configuration.loop = descriptor.looping;
			configuration.reverse = descriptor.reverse;
			configuration.speed = descriptor.speed;
			configuration.timePolicy = AnimatorTimePolicy::Unscaled;
			configuration.fixedStepTicks = 1;
			return output.Configure(configuration);
		}

		std::string EndReasonText(AnimationPreviewEndReason reason) {
			switch(reason) {
			case AnimationPreviewEndReason::Explicit:
				return "Preview ended; the authored/reference pose remains unchanged.";
			case AnimationPreviewEndReason::SelectionChanged:
				return "Preview cleared because the Inspector selection changed.";
			case AnimationPreviewEndReason::PlayModeTransition:
				return "Preview cleared at the Play Mode ownership boundary.";
			case AnimationPreviewEndReason::ClipGenerationChanged:
				return "Preview cleared because a new immutable clip generation was published.";
			case AnimationPreviewEndReason::InvalidClip:
				return "Preview is unavailable because the clip metadata is invalid.";
			case AnimationPreviewEndReason::ControllerDestroyed:
				return "Preview world was retired with its owning editor controller.";
			case AnimationPreviewEndReason::None:
			default:
				return "No transient animation preview session is active.";
			}
		}

		AnimationPreviewEvaluationMode EvaluationModeFor(
			AnimationPreviewSourceMode sourceMode,
			bool rendererAvailable) noexcept {
			if(sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly) {
				return AnimationPreviewEvaluationMode::LiveInstanceCopy;
			}
			return rendererAvailable
				? AnimationPreviewEvaluationMode::CpuReference
				: AnimationPreviewEvaluationMode::CpuRendererFallback;
		}

		AnimationStatus ValidateStableIds(
			const std::vector<std::uint64_t>& ids,
			AnimationLimit limit) {
			if(ValidateAnimationHardLimit(limit, ids.size()) != AnimationStatus::Ok) {
				return AnimationStatus::HardLimitExceeded;
			}
			std::set<std::uint64_t> unique;
			for(const std::uint64_t id : ids) {
				if(id == 0U || !unique.insert(id).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ValidateWorldOutput(const AnimationPreviewWorldOutput& world) {
			if(!world.complete || world.tick < 0
				|| !IsFinite(world.rootTranslation) || !std::isfinite(world.rootYawRadians)
				|| ValidateAnimationHardLimit(AnimationLimit::SkeletonJoints, world.joints.size())
					!= AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::MeshMorphTargets, world.morphs.size())
					!= AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::ClipChannels, world.curves.size())
					!= AnimationStatus::Ok
				|| ValidateAnimationHardLimit(AnimationLimit::AnimatorRetainedEvents, world.events.size())
					!= AnimationStatus::Ok) {
				return AnimationStatus::InvalidArgument;
			}

			std::set<std::uint64_t> jointIds;
			for(const AnimationPreviewJointOutput& joint : world.joints) {
				if(joint.stableJointId == 0U || !jointIds.insert(joint.stableJointId).second
					|| !IsFinite(joint.localTransform)
					|| !IsNormalized(joint.localTransform.rotation)) {
					return AnimationStatus::InvalidArgument;
				}
			}
			auto validateNamed = [](const auto& values) {
				std::set<std::uint64_t> ids;
				for(const AnimationPreviewNamedFloatOutput& value : values) {
					if(value.stableId == 0U || !std::isfinite(value.value)
						|| !ids.insert(value.stableId).second) {
						return false;
					}
				}
				return true;
			};
			if(!validateNamed(world.morphs) || !validateNamed(world.curves)) {
				return AnimationStatus::InvalidArgument;
			}
			std::set<std::uint64_t> eventIds;
			for(const AnimationClipEvent& event : world.events) {
				if(event.stableEventId == 0U || event.tick < 0
					|| event.payload.size() > kMaximumLiveEventPayloadBytes
					|| !eventIds.insert(event.stableEventId).second) {
					return AnimationStatus::InvalidArgument;
				}
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus NormalizeAssetInput(
			const AnimationPreviewSessionDescriptor& descriptor,
			const AnimationPreviewAssetInput& input,
			AnimationPreviewAssetInput& output) {
			if(!IsValidDescriptor(descriptor)
				|| descriptor.clipGeneration > std::numeric_limits<AnimationGeneration>::max()
				|| ValidateAnimationClipAsset(input.clip) != AnimationStatus::Ok
				|| input.clip.assetId != descriptor.clipAssetId
				|| input.clip.runtimeGeneration != descriptor.clipGeneration
				|| input.clip.compatibilityFingerprint != descriptor.compatibilityFingerprint
				|| input.clip.durationTicks != descriptor.durationTicks
				|| input.clip.sampleRate != descriptor.sampleRate) {
				return AnimationStatus::InvalidArgument;
			}

			AnimationPreviewAssetInput staged = input;
			if(staged.jointStableIds.empty() && staged.referenceSkeleton.has_value()) {
				staged.jointStableIds.reserve(staged.referenceSkeleton->joints.size());
				for(const SkeletonJoint& joint : staged.referenceSkeleton->joints) {
					staged.jointStableIds.push_back(joint.stableId);
				}
			}
			auto deriveTrackIds = [&](AnimationClipTrackKind kind, std::vector<std::uint64_t>& ids) {
				if(!ids.empty()) {
					return;
				}
				for(const AnimationClipTrack& track : staged.clip.tracks) {
					if(track.kind == kind) {
						ids.push_back(track.stableTargetId);
					}
				}
				std::sort(ids.begin(), ids.end());
				ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			};
			deriveTrackIds(AnimationClipTrackKind::MorphWeight, staged.morphStableIds);
			deriveTrackIds(AnimationClipTrackKind::Curve, staged.curveStableIds);
			if(AnimationStatus status = ValidateStableIds(
				staged.jointStableIds, AnimationLimit::SkeletonJoints); status != AnimationStatus::Ok) {
				return status;
			}
			if(AnimationStatus status = ValidateStableIds(
				staged.morphStableIds, AnimationLimit::MeshMorphTargets); status != AnimationStatus::Ok) {
				return status;
			}
			if(AnimationStatus status = ValidateStableIds(
				staged.curveStableIds, AnimationLimit::ClipChannels); status != AnimationStatus::Ok) {
				return status;
			}

			// Opening the view validates copied skeleton compatibility, residency, and
			// every codec range before this candidate can receive a public handle.
			AnimationClipView view;
			const SkeletonAsset* skeleton = staged.referenceSkeleton.has_value()
				? &*staged.referenceSkeleton : nullptr;
			if(AnimationStatus status = view.Open(
				staged.clip, skeleton, staged.residentSegments); status != AnimationStatus::Ok) {
				return status;
			}
			output = std::move(staged);
			return AnimationStatus::Ok;
		}

		AnimationStatus EvaluateAsset(
			const AnimationPreviewAssetInput& input,
			AnimationFixedTick tick,
			bool looping,
			AnimationPreviewWorldOutput& output) {
			AnimationClipView view;
			const SkeletonAsset* skeleton = input.referenceSkeleton.has_value()
				? &*input.referenceSkeleton : nullptr;
			if(AnimationStatus status = view.Open(
				input.clip, skeleton, input.residentSegments); status != AnimationStatus::Ok) {
				return status;
			}
			CpuClipEvaluationRequest request;
			request.tick = tick;
			request.loop = looping;
			request.jointStableIds = input.jointStableIds;
			request.morphStableIds = input.morphStableIds;
			request.curveStableIds = input.curveStableIds;
			CpuClipEvaluationResult evaluated;
			const AnimationStatus status = CpuClipEvaluator{}.Evaluate(view, request, evaluated);
			if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady) {
				return status;
			}

			AnimationPreviewWorldOutput staged;
			staged.tick = evaluated.time.resolvedTick;
			staged.loopIndex = evaluated.time.loopIndex;
			staged.joints.reserve(evaluated.joints.size());
			for(const CpuClipJointSample& joint : evaluated.joints) {
				staged.joints.push_back({joint.stableJointId, joint.localTransform, joint.sources});
			}
			staged.morphs.reserve(evaluated.morphs.size());
			for(const CpuClipNamedFloatSample& morph : evaluated.morphs) {
				staged.morphs.push_back({morph.stableId, morph.value, morph.source});
			}
			staged.curves.reserve(evaluated.curves.size());
			for(const CpuClipNamedFloatSample& curve : evaluated.curves) {
				staged.curves.push_back({curve.stableId, curve.value, curve.source});
			}
			staged.events.reserve(evaluated.pointEvents.size());
			for(const CpuClipPointEvent& event : evaluated.pointEvents) {
				if(event.event == nullptr) {
					return AnimationStatus::InternalError;
				}
				staged.events.push_back(*event.event);
			}
			staged.rootMotionEnabled = evaluated.rootMotion.enabled;
			staged.rootTranslation = evaluated.rootMotion.translation;
			staged.rootYawRadians = evaluated.rootMotion.yawRadians;
			staged.fallbackChannelCount = evaluated.fallbackChannelCount;
			staged.unavailableChannelCount = evaluated.unavailableChannelCount;
			staged.complete = status == AnimationStatus::Ok;
			output = std::move(staged);
			return status;
		}

		bool CheckedTickAdd(
			AnimationFixedTick left,
			AnimationFixedTick right,
			AnimationFixedTick& output) noexcept {
			if((right > 0 && left > std::numeric_limits<AnimationFixedTick>::max() - right)
				|| (right < 0 && left < std::numeric_limits<AnimationFixedTick>::min() - right)) {
				return false;
			}
			output = left + right;
			return true;
		}
	}

	class AnimationPreviewController::Impl final {
	public:
		AnimationStatus CreateIsolatedSession(
			const AnimationPreviewSessionDescriptor& descriptor,
			const AnimationPreviewAssetInput& input,
			bool rendererAvailable,
			AnimationPreviewSessionHandle& output) noexcept {
			try {
				{
					std::scoped_lock lock(mutex_);
					// Modern isolated sessions own copied clip/skeleton data and do
					// not read or mutate the Play world. They may therefore coexist
					// with Play Mode while the legacy Inspector cursor below keeps
					// its historical enter-Play teardown behavior.
					if(!ContextAllowsLocked(descriptor)) {
						return AnimationStatus::Rejected;
					}
				}

				AnimationPreviewAssetInput copiedInput;
				if(AnimationStatus status = NormalizeAssetInput(
					descriptor, input, copiedInput); status != AnimationStatus::Ok) {
					return status;
				}
				AnimatorPlayback playback;
				if(AnimationStatus status = ConfigurePlayback(descriptor, playback);
					status != AnimationStatus::Ok) {
					return status;
				}
				AnimationPreviewWorldOutput initialOutput;
				const AnimationStatus evaluationStatus = EvaluateAsset(
					copiedInput, 0, descriptor.looping, initialOutput);
				if(evaluationStatus != AnimationStatus::Ok
					&& evaluationStatus != AnimationStatus::NotReady) {
					return evaluationStatus;
				}

				Session staged;
				staged.descriptor = descriptor;
				staged.sourceMode = AnimationPreviewSourceMode::IsolatedAsset;
				staged.assetInput = std::make_shared<const AnimationPreviewAssetInput>(
					std::move(copiedInput));
				staged.playback = playback;
				staged.initialOutput = initialOutput;
				staged.output = std::move(initialOutput);
				staged.rendererAvailable = rendererAvailable;
				staged.latestStatus = evaluationStatus;
				staged.revision = 1U;

				std::scoped_lock lock(mutex_);
				if(!ContextAllowsLocked(descriptor)) {
					return AnimationStatus::Rejected;
				}
				return AllocateSessionLocked(std::move(staged), output);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus SelectLiveInstance(
			const AnimationPreviewSessionDescriptor& descriptor,
			const AnimationPreviewLiveInstanceSnapshot& input,
			bool rendererAvailable,
			AnimationPreviewSessionHandle& output) noexcept {
			try {
				if(!IsValidDescriptor(descriptor) || !input.instance.IsValid()
					|| input.poseGeneration == 0U || input.publicationSequence == 0U) {
					return AnimationStatus::InvalidArgument;
				}
				if(AnimationStatus status = ValidateWorldOutput(input.world);
					status != AnimationStatus::Ok) {
					return status;
				}
				AnimatorPlayback playback;
				if(AnimationStatus status = ConfigurePlayback(descriptor, playback);
					status != AnimationStatus::Ok) {
					return status;
				}
				if(AnimationStatus status = playback.Seek(input.world.tick);
					status != AnimationStatus::Ok) {
					return status;
				}

				Session staged;
				staged.descriptor = descriptor;
				staged.sourceMode = AnimationPreviewSourceMode::LiveInstanceReadOnly;
				staged.liveInput = input;
				staged.playback = playback;
				staged.initialOutput = input.world;
				staged.output = input.world;
				staged.rendererAvailable = rendererAvailable;
				staged.latestStatus = AnimationStatus::Ok;
				staged.revision = 1U;

				std::scoped_lock lock(mutex_);
				if(!playModeActive_ || !ContextAllowsLocked(descriptor)) {
					return AnimationStatus::Rejected;
				}
				return AllocateSessionLocked(std::move(staged), output);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus DestroySession(
			AnimationPreviewSessionHandle handle,
			AnimationPreviewEndReason reason,
			AnimationPreviewTeardownResult* teardown) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return DestroySessionLocked(handle, reason, teardown);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		void DestroyAllSessions(AnimationPreviewEndReason reason) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				for(std::size_t index = 0U; index < slots_.size(); ++index) {
					Slot& slot = slots_[index];
					if(slot.alive) {
						RetireSlotLocked(index, reason);
					}
				}
			} catch(...) {
				// Destruction is best-effort only for private editor memory. No authored
				// scene state exists here that would require a compensating mutation.
			}
		}

		template<typename Command>
		AnimationStatus ApplyTransport(
			AnimationPreviewSessionHandle handle,
			Command&& command) noexcept {
			try {
				AnimatorPlayback candidate;
				std::shared_ptr<const AnimationPreviewAssetInput> assetInput;
				std::uint64_t expectedRevision = 0U;
				bool rendererAvailable = false;
				{
					std::scoped_lock lock(mutex_);
					Session* session = nullptr;
					if(AnimationStatus status = ResolveLocked(handle, session);
						status != AnimationStatus::Ok) {
						return status;
					}
					if(session->sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly) {
						return AnimationStatus::Rejected;
					}
					candidate = session->playback;
					assetInput = session->assetInput;
					expectedRevision = session->revision;
					rendererAvailable = session->rendererAvailable;
				}

				const AnimationStatus commandStatus = command(candidate);
				if(commandStatus != AnimationStatus::Ok) {
					return commandStatus;
				}
				AnimationPreviewWorldOutput evaluated;
				const AnimatorPlaybackStateSnapshot playback = candidate.Snapshot();
				const AnimationStatus evaluationStatus = EvaluateAsset(
					*assetInput, playback.absoluteTick, playback.looping, evaluated);
				if(evaluationStatus != AnimationStatus::Ok
					&& evaluationStatus != AnimationStatus::NotReady) {
					return evaluationStatus;
				}

				std::scoped_lock lock(mutex_);
				Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				if(session->revision != expectedRevision
					|| session->rendererAvailable != rendererAvailable) {
					return AnimationStatus::StaleHandle;
				}
				session->playback = candidate;
				session->output = std::move(evaluated);
				session->latestStatus = evaluationStatus;
				++session->revision;
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus SeekNormalized(
			AnimationPreviewSessionHandle handle,
			float normalizedTime) noexcept {
			if(!std::isfinite(normalizedTime) || normalizedTime < 0.0F || normalizedTime > 1.0F) {
				return AnimationStatus::InvalidArgument;
			}
			AnimationFixedTick durationTicks = 0;
			{
				std::scoped_lock lock(mutex_);
				Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				if(session->sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly) {
					return AnimationStatus::Rejected;
				}
				durationTicks = session->descriptor.durationTicks;
			}
			const long double requested = static_cast<long double>(durationTicks)
				* static_cast<long double>(normalizedTime);
			if(requested > static_cast<long double>(
				std::numeric_limits<AnimationFixedTick>::max())) {
				return AnimationStatus::RangeOverflow;
			}
			return ApplyTransport(handle, [requested](AnimatorPlayback& playback) {
				return playback.Seek(static_cast<AnimationFixedTick>(std::llround(requested)));
			});
		}

		AnimationStatus Step(
			AnimationPreviewSessionHandle handle,
			AnimationFixedTick deltaTicks) noexcept {
			return ApplyTransport(handle, [deltaTicks](AnimatorPlayback& playback) {
				AnimationFixedTick requested = 0;
				if(!CheckedTickAdd(playback.AbsoluteTick(), deltaTicks, requested)) {
					return AnimationStatus::RangeOverflow;
				}
				return playback.Seek(requested);
			});
		}

		AnimationStatus SetRendererAvailable(
			AnimationPreviewSessionHandle handle,
			bool available) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				session->rendererAvailable = available;
				++session->revision;
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus PrepareTick(
			AnimationPreviewSessionHandle handle,
			double unscaledDeltaSeconds,
			AnimationPreviewPreparedFrame& output) const noexcept {
			if(!std::isfinite(unscaledDeltaSeconds) || unscaledDeltaSeconds < 0.0) {
				return AnimationStatus::InvalidArgument;
			}
			try {
				AnimationPreviewPreparedFrame staged;
				AnimatorPlayback candidate;
				std::shared_ptr<const AnimationPreviewAssetInput> assetInput;
				{
					std::scoped_lock lock(mutex_);
					const Session* session = nullptr;
					if(AnimationStatus status = ResolveLocked(handle, session);
						status != AnimationStatus::Ok) {
						return status;
					}
					staged.handle = handle;
					staged.expectedSessionRevision = session->revision;
					staged.unscaledDeltaSeconds = unscaledDeltaSeconds;
					staged.sourceMode = session->sourceMode;
					staged.rendererAvailable = session->rendererAvailable;
					staged.evaluationMode = EvaluationModeFor(
						session->sourceMode, session->rendererAvailable);
					candidate = session->playback;
					if(session->sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly) {
						staged.playback = candidate.Snapshot();
						staged.output = session->output;
						staged.evaluationStatus = session->latestStatus;
						output = std::move(staged);
						return AnimationStatus::Ok;
					}
					assetInput = session->assetInput;
				}

				AnimatorPlaybackTimeInput timeInput;
				timeInput.scaledDeltaSeconds = unscaledDeltaSeconds;
				timeInput.unscaledDeltaSeconds = unscaledDeltaSeconds;
				AnimatorPlaybackAdvanceResult advance;
				if(AnimationStatus status = candidate.Advance(timeInput, advance);
					status != AnimationStatus::Ok) {
					return status;
				}
				staged.playback = candidate.Snapshot();
				staged.evaluationStatus = EvaluateAsset(
					*assetInput, staged.playback.absoluteTick,
					staged.playback.looping, staged.output);
				if(staged.evaluationStatus != AnimationStatus::Ok
					&& staged.evaluationStatus != AnimationStatus::NotReady) {
					return staged.evaluationStatus;
				}
				output = std::move(staged);
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus PublishPreparedFrame(
			const AnimationPreviewPreparedFrame& prepared) noexcept {
			if(!prepared.handle.IsValid()
				|| !std::isfinite(prepared.unscaledDeltaSeconds)
				|| prepared.unscaledDeltaSeconds < 0.0) {
				return AnimationStatus::InvalidArgument;
			}
			try {
				AnimatorPlayback candidate;
				std::shared_ptr<const AnimationPreviewAssetInput> assetInput;
				{
					std::scoped_lock lock(mutex_);
					Session* session = nullptr;
					if(AnimationStatus status = ResolveLocked(prepared.handle, session);
						status != AnimationStatus::Ok) {
						return status;
					}
					if(session->revision != prepared.expectedSessionRevision
						|| session->sourceMode != prepared.sourceMode
						|| session->rendererAvailable != prepared.rendererAvailable) {
						return AnimationStatus::StaleHandle;
					}
					if(session->sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly) {
						if(prepared.playback != session->playback.Snapshot()
							|| prepared.output != session->output
							|| prepared.evaluationMode != AnimationPreviewEvaluationMode::LiveInstanceCopy
							|| prepared.evaluationStatus != session->latestStatus) {
							return AnimationStatus::DeterminismViolation;
						}
						return AnimationStatus::Ok;
					}
					candidate = session->playback;
					assetInput = session->assetInput;
				}

				AnimatorPlaybackTimeInput timeInput;
				timeInput.scaledDeltaSeconds = prepared.unscaledDeltaSeconds;
				timeInput.unscaledDeltaSeconds = prepared.unscaledDeltaSeconds;
				AnimatorPlaybackAdvanceResult advance;
				if(AnimationStatus status = candidate.Advance(timeInput, advance);
					status != AnimationStatus::Ok) {
					return status;
				}
				AnimationPreviewWorldOutput verifiedOutput;
				const AnimatorPlaybackStateSnapshot verifiedPlayback = candidate.Snapshot();
				const AnimationStatus evaluationStatus = EvaluateAsset(
					*assetInput, verifiedPlayback.absoluteTick,
					verifiedPlayback.looping, verifiedOutput);
				if(evaluationStatus != AnimationStatus::Ok
					&& evaluationStatus != AnimationStatus::NotReady) {
					return evaluationStatus;
				}
				if(verifiedPlayback != prepared.playback
					|| verifiedOutput != prepared.output
					|| evaluationStatus != prepared.evaluationStatus
					|| prepared.evaluationMode != EvaluationModeFor(
						AnimationPreviewSourceMode::IsolatedAsset, prepared.rendererAvailable)) {
					return AnimationStatus::DeterminismViolation;
				}

				// Protected publication rechecks identity after worker-safe evaluation.
				// A destroyed/reused session or intervening control edit leaves its prior
				// complete output untouched and reports StaleHandle.
				std::scoped_lock lock(mutex_);
				Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(prepared.handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				if(session->revision != prepared.expectedSessionRevision
					|| session->rendererAvailable != prepared.rendererAvailable) {
					return AnimationStatus::StaleHandle;
				}
				session->playback = candidate;
				session->output = std::move(verifiedOutput);
				session->latestStatus = evaluationStatus;
				++session->revision;
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus TickSession(
			AnimationPreviewSessionHandle handle,
			double unscaledDeltaSeconds) noexcept {
			AnimationPreviewPreparedFrame prepared;
			if(AnimationStatus status = PrepareTick(handle, unscaledDeltaSeconds, prepared);
				status != AnimationStatus::Ok) {
				return status;
			}
			return PublishPreparedFrame(prepared);
		}

		AnimationStatus UpdateLiveInstance(
			AnimationPreviewSessionHandle handle,
			const AnimationPreviewLiveInstanceSnapshot& input) noexcept {
			try {
				if(!input.instance.IsValid() || input.poseGeneration == 0U
					|| input.publicationSequence == 0U) {
					return AnimationStatus::InvalidArgument;
				}
				if(AnimationStatus status = ValidateWorldOutput(input.world);
					status != AnimationStatus::Ok) {
					return status;
				}
				AnimationPreviewLiveInstanceSnapshot copied = input;
				std::scoped_lock lock(mutex_);
				Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				if(session->sourceMode != AnimationPreviewSourceMode::LiveInstanceReadOnly) {
					return AnimationStatus::InvalidType;
				}
				if(copied.instance != session->liveInput.instance) {
					return AnimationStatus::StaleHandle;
				}
				if(copied.poseGeneration < session->liveInput.poseGeneration
					|| copied.publicationSequence <= session->liveInput.publicationSequence) {
					return AnimationStatus::StaleHandle;
				}
				AnimatorPlayback playback = session->playback;
				if(AnimationStatus status = playback.Seek(copied.world.tick);
					status != AnimationStatus::Ok) {
					return status;
				}
				session->liveInput = std::move(copied);
				session->playback = playback;
				session->output = session->liveInput.world;
				session->latestStatus = AnimationStatus::Ok;
				++session->revision;
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus GetSessionState(
			AnimationPreviewSessionHandle handle,
			AnimationPreviewSessionState& output) const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				const Session* session = nullptr;
				if(AnimationStatus status = ResolveLocked(handle, session);
					status != AnimationStatus::Ok) {
					return status;
				}
				AnimationPreviewSessionState staged;
				staged.active = true;
				staged.handle = handle;
				staged.descriptor = session->descriptor;
				staged.sourceMode = session->sourceMode;
				staged.evaluationMode = EvaluationModeFor(
					session->sourceMode, session->rendererAvailable);
				staged.playback = session->playback.Snapshot();
				staged.output = session->output;
				staged.liveInstance = session->liveInput.instance;
				staged.livePoseGeneration = session->liveInput.poseGeneration;
				staged.livePublicationSequence = session->liveInput.publicationSequence;
				staged.sessionRevision = session->revision;
				staged.latestStatus = session->latestStatus;
				staged.endReason = session->endReason;
				staged.rendererAvailable = session->rendererAvailable;
				staged.readOnly = session->sourceMode
					== AnimationPreviewSourceMode::LiveInstanceReadOnly;
				output = std::move(staged);
				return AnimationStatus::Ok;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		bool IsSessionAlive(AnimationPreviewSessionHandle handle) const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				const Session* session = nullptr;
				return ResolveLocked(handle, session) == AnimationStatus::Ok;
			} catch(...) {
				return false;
			}
		}

		std::size_t SessionCount() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				return activeSessionCount_;
			} catch(...) {
				return 0U;
			}
		}

		// Legacy Inspector cursor methods.
		AnimationStatus BeginLegacySession(
			const AnimationPreviewSessionDescriptor& descriptor) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(playModeActive_ || !ContextAllowsLocked(descriptor)) {
					return RecordLegacyLocked(AnimationStatus::Rejected);
				}
				if(!IsValidDescriptor(descriptor)) {
					return RecordLegacyLocked(AnimationStatus::InvalidArgument);
				}
				AnimatorPlayback candidate;
				if(AnimationStatus status = ConfigurePlayback(descriptor, candidate);
					status != AnimationStatus::Ok) {
					return RecordLegacyLocked(status);
				}
				legacyPlayback_ = candidate;
				legacyDescriptor_ = descriptor;
				legacyActive_ = true;
				legacyEndReason_ = AnimationPreviewEndReason::None;
				return RecordLegacyLocked(AnimationStatus::Ok);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		void EndLegacySession(AnimationPreviewEndReason reason) noexcept {
			std::scoped_lock lock(mutex_);
			EndLegacyLocked(reason);
		}

		template<typename Command>
		AnimationStatus ApplyLegacy(Command&& command) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(!legacyActive_) {
					return RecordLegacyLocked(AnimationStatus::NotReady);
				}
				return RecordLegacyLocked(command(legacyPlayback_, legacyDescriptor_));
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus SeekLegacyNormalized(float normalizedTime) noexcept {
			if(!std::isfinite(normalizedTime) || normalizedTime < 0.0F || normalizedTime > 1.0F) {
				std::scoped_lock lock(mutex_);
				return RecordLegacyLocked(AnimationStatus::InvalidArgument);
			}
			return ApplyLegacy([normalizedTime](AnimatorPlayback& playback, const auto& descriptor) {
				const long double requested = static_cast<long double>(descriptor.durationTicks)
					* static_cast<long double>(normalizedTime);
				return playback.Seek(static_cast<AnimationFixedTick>(std::llround(requested)));
			});
		}

		AnimationStatus TickLegacy(double unscaledDeltaSeconds) noexcept {
			return ApplyLegacy([unscaledDeltaSeconds](AnimatorPlayback& playback, const auto&) {
				AnimatorPlaybackTimeInput input;
				input.scaledDeltaSeconds = unscaledDeltaSeconds;
				input.unscaledDeltaSeconds = unscaledDeltaSeconds;
				AnimatorPlaybackAdvanceResult output;
				return playback.Advance(input, output);
			});
		}

		AnimationPreviewSnapshot GetLegacySnapshot() const noexcept {
			try {
				std::scoped_lock lock(mutex_);
				AnimationPreviewSnapshot snapshot;
				snapshot.active = legacyActive_;
				snapshot.session = legacyDescriptor_;
				snapshot.playback = legacyPlayback_.Snapshot();
				snapshot.latestStatus = legacyLatestStatus_;
				snapshot.endReason = legacyEndReason_;
				if(!legacyActive_) {
					snapshot.safePoseReason = EndReasonText(legacyEndReason_);
				} else if(legacyLatestStatus_ != AnimationStatus::Ok) {
					snapshot.safePoseReason =
						"Preview retained its last complete cursor because the latest command was rejected.";
				} else if(snapshot.playback.runState == AnimatorPlaybackRunState::Stopped) {
					snapshot.safePoseReason =
						"Preview is stopped; the authored/reference pose remains unchanged.";
				} else {
					snapshot.safePoseReason =
						"Preview time is editor-owned and never serializes into AnimatorComponent.";
				}
				return snapshot;
			} catch(...) {
				return {};
			}
		}

		AnimationStatus BeginLegacyIsolated(
			std::uint64_t sessionId,
			const AnimationPreviewSessionDescriptor& descriptor) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				if(sessionId == 0U || playModeActive_ || !ContextAllowsLocked(descriptor)) {
					return RecordLegacyLocked(AnimationStatus::Rejected);
				}
				if(legacyIsolated_.size() >= kMaximumAnimationPreviewSessions) {
					return RecordLegacyLocked(AnimationStatus::HardLimitExceeded);
				}
				if(legacyIsolated_.find(sessionId) != legacyIsolated_.end()) {
					return RecordLegacyLocked(AnimationStatus::Rejected);
				}
				if(!IsValidDescriptor(descriptor)) {
					return RecordLegacyLocked(AnimationStatus::InvalidArgument);
				}
				LegacyCursor cursor;
				cursor.descriptor = descriptor;
				if(AnimationStatus status = ConfigurePlayback(descriptor, cursor.playback);
					status != AnimationStatus::Ok) {
					return RecordLegacyLocked(status);
				}
				cursor.latestStatus = AnimationStatus::Ok;
				legacyIsolated_.emplace(sessionId, std::move(cursor));
				return RecordLegacyLocked(AnimationStatus::Ok);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus TickLegacyIsolated(double unscaledDeltaSeconds) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				AnimatorPlaybackTimeInput input;
				input.scaledDeltaSeconds = unscaledDeltaSeconds;
				input.unscaledDeltaSeconds = unscaledDeltaSeconds;
				for(auto& [sessionId, cursor] : legacyIsolated_) {
					(void)sessionId;
					AnimatorPlaybackAdvanceResult output;
					cursor.latestStatus = cursor.playback.Advance(input, output);
					if(cursor.latestStatus != AnimationStatus::Ok
						&& cursor.latestStatus != AnimationStatus::NotReady) {
						return RecordLegacyLocked(cursor.latestStatus);
					}
				}
				return RecordLegacyLocked(AnimationStatus::Ok);
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		std::size_t LegacyIsolatedCount() const noexcept {
			std::scoped_lock lock(mutex_);
			return legacyIsolated_.size();
		}

		bool LegacyActive() const noexcept {
			std::scoped_lock lock(mutex_);
			return legacyActive_;
		}

		void SynchronizeContext(const UUID& selectionId, bool playModeActive) noexcept {
			try {
				std::scoped_lock lock(mutex_);
				const bool firstObservation = !contextObserved_;
				const bool playModeChanged = contextObserved_ && playModeActive != playModeActive_;
				contextObserved_ = true;
				observedSelectionId_ = selectionId;
				playModeActive_ = playModeActive;

				if(legacyActive_
					&& ((firstObservation && playModeActive)
						|| playModeChanged
						|| legacyDescriptor_.selectionId != selectionId)) {
					EndLegacyLocked(playModeActive || playModeChanged
						? AnimationPreviewEndReason::PlayModeTransition
						: AnimationPreviewEndReason::SelectionChanged);
				}
				for(auto it = legacyIsolated_.begin(); it != legacyIsolated_.end();) {
					if(playModeActive || it->second.descriptor.selectionId != selectionId) {
						it = legacyIsolated_.erase(it);
					} else {
						++it;
					}
				}

				for(std::size_t index = 0U; index < slots_.size(); ++index) {
					Slot& slot = slots_[index];
					if(!slot.alive || !slot.session.has_value()) {
						continue;
					}
					const Session& session = *slot.session;
					const bool ownershipEnded =
						session.sourceMode == AnimationPreviewSourceMode::LiveInstanceReadOnly
						&& !playModeActive;
					const bool selectionChanged = session.descriptor.followSelection
						&& session.descriptor.selectionId != selectionId;
					if(ownershipEnded || selectionChanged) {
						RetireSlotLocked(index, ownershipEnded
							? AnimationPreviewEndReason::PlayModeTransition
							: AnimationPreviewEndReason::SelectionChanged);
					}
				}
			} catch(...) {
				// Context synchronization never owns authored state. An allocation failure
				// can delay cleanup until the next frame without exposing scene mutation.
			}
		}

	private:
		struct Session {
			AnimationPreviewSessionDescriptor descriptor;
			AnimationPreviewSourceMode sourceMode = AnimationPreviewSourceMode::IsolatedAsset;
			std::shared_ptr<const AnimationPreviewAssetInput> assetInput;
			AnimationPreviewLiveInstanceSnapshot liveInput;
			AnimatorPlayback playback;
			AnimationPreviewWorldOutput initialOutput;
			AnimationPreviewWorldOutput output;
			AnimationStatus latestStatus = AnimationStatus::NotReady;
			AnimationPreviewEndReason endReason = AnimationPreviewEndReason::None;
			std::uint64_t revision = 0U;
			bool rendererAvailable = false;
		};

		struct Slot {
			AnimationGeneration generation = 1U;
			bool alive = false;
			bool retired = false;
			std::optional<Session> session;
		};

		struct LegacyCursor {
			AnimationPreviewSessionDescriptor descriptor;
			AnimatorPlayback playback;
			AnimationStatus latestStatus = AnimationStatus::NotReady;
		};

		bool ContextAllowsLocked(const AnimationPreviewSessionDescriptor& descriptor) const noexcept {
			return !contextObserved_ || !descriptor.followSelection
				|| descriptor.selectionId == observedSelectionId_;
		}

		AnimationStatus AllocateSessionLocked(
			Session staged,
			AnimationPreviewSessionHandle& output) {
			if(activeSessionCount_ >= kMaximumAnimationPreviewSessions) {
				return AnimationStatus::HardLimitExceeded;
			}
			for(std::size_t index = 0U; index < slots_.size(); ++index) {
				Slot& slot = slots_[index];
				if(slot.alive || slot.retired) {
					continue;
				}
				slot.session.emplace(std::move(staged));
				slot.alive = true;
				++activeSessionCount_;
				output = {static_cast<std::uint32_t>(index), slot.generation};
				return AnimationStatus::Ok;
			}
			return AnimationStatus::HardLimitExceeded;
		}

		AnimationStatus ResolveLocked(
			AnimationPreviewSessionHandle handle,
			Session*& output) noexcept {
			if(!handle.IsValid()) {
				return AnimationStatus::InvalidArgument;
			}
			if(handle.slotIndex >= slots_.size()) {
				return AnimationStatus::StaleHandle;
			}
			Slot& slot = slots_[handle.slotIndex];
			if(!slot.alive || slot.generation != handle.generation || !slot.session.has_value()) {
				return AnimationStatus::StaleHandle;
			}
			output = &*slot.session;
			return AnimationStatus::Ok;
		}

		AnimationStatus ResolveLocked(
			AnimationPreviewSessionHandle handle,
			const Session*& output) const noexcept {
			if(!handle.IsValid()) {
				return AnimationStatus::InvalidArgument;
			}
			if(handle.slotIndex >= slots_.size()) {
				return AnimationStatus::StaleHandle;
			}
			const Slot& slot = slots_[handle.slotIndex];
			if(!slot.alive || slot.generation != handle.generation || !slot.session.has_value()) {
				return AnimationStatus::StaleHandle;
			}
			output = &*slot.session;
			return AnimationStatus::Ok;
		}

		AnimationStatus DestroySessionLocked(
			AnimationPreviewSessionHandle handle,
			AnimationPreviewEndReason reason,
			AnimationPreviewTeardownResult* teardown) {
			Session* session = nullptr;
			if(AnimationStatus status = ResolveLocked(handle, session);
				status != AnimationStatus::Ok) {
				return status;
			}
			if(teardown != nullptr) {
				AnimationPreviewTeardownResult staged;
				staged.retiredHandle = handle;
				staged.reason = reason;
				staged.sourceMode = session->sourceMode;
				staged.initialOutput = session->initialOutput;
				staged.lastOutput = session->output;
				staged.restoredOutput = session->initialOutput;
				staged.authoredMutationCount = 0U;
				staged.restored = true;
				*teardown = std::move(staged);
			}
			RetireSlotLocked(handle.slotIndex, reason);
			return AnimationStatus::Ok;
		}

		void RetireSlotLocked(std::size_t index, AnimationPreviewEndReason reason) noexcept {
			Slot& slot = slots_[index];
			if(!slot.alive) {
				return;
			}
			if(slot.session.has_value()) {
				// Restore the private world's initial complete output before releasing it.
				// This is intentionally not an ObjectManager/AnimatorComponent write.
				slot.session->output = std::move(slot.session->initialOutput);
				slot.session->endReason = reason;
			}
			slot.session.reset();
			slot.alive = false;
			if(activeSessionCount_ != 0U) {
				--activeSessionCount_;
			}
			if(slot.generation == std::numeric_limits<AnimationGeneration>::max()) {
				slot.retired = true;
			} else {
				++slot.generation;
			}
		}

		AnimationStatus RecordLegacyLocked(AnimationStatus status) noexcept {
			legacyLatestStatus_ = status;
			return status;
		}

		void EndLegacyLocked(AnimationPreviewEndReason reason) noexcept {
			legacyPlayback_ = {};
			legacyDescriptor_ = {};
			legacyActive_ = false;
			legacyLatestStatus_ = AnimationStatus::NotReady;
			legacyEndReason_ = reason;
		}

		mutable std::mutex mutex_;
		std::array<Slot, kMaximumAnimationPreviewSessions> slots_{};
		std::size_t activeSessionCount_ = 0U;
		bool contextObserved_ = false;
		UUID observedSelectionId_ = EMPTY_UUID;
		bool playModeActive_ = false;

		AnimatorPlayback legacyPlayback_;
		AnimationPreviewSessionDescriptor legacyDescriptor_;
		AnimationStatus legacyLatestStatus_ = AnimationStatus::NotReady;
		AnimationPreviewEndReason legacyEndReason_ = AnimationPreviewEndReason::None;
		bool legacyActive_ = false;
		std::map<std::uint64_t, LegacyCursor> legacyIsolated_;
	};

	AnimationPreviewController::AnimationPreviewController()
		: impl_(std::make_unique<Impl>()) {}

	AnimationPreviewController::~AnimationPreviewController() {
		impl_->DestroyAllSessions(AnimationPreviewEndReason::ControllerDestroyed);
	}

	AnimationStatus AnimationPreviewController::CreateIsolatedSession(
		const AnimationPreviewSessionDescriptor& descriptor,
		const AnimationPreviewAssetInput& input,
		bool rendererAvailable,
		AnimationPreviewSessionHandle& output) noexcept {
		return impl_->CreateIsolatedSession(descriptor, input, rendererAvailable, output);
	}

	AnimationStatus AnimationPreviewController::SelectLiveInstance(
		const AnimationPreviewSessionDescriptor& descriptor,
		const AnimationPreviewLiveInstanceSnapshot& input,
		bool rendererAvailable,
		AnimationPreviewSessionHandle& output) noexcept {
		return impl_->SelectLiveInstance(descriptor, input, rendererAvailable, output);
	}

	AnimationStatus AnimationPreviewController::DestroySession(
		AnimationPreviewSessionHandle handle,
		AnimationPreviewEndReason reason,
		AnimationPreviewTeardownResult* teardown) noexcept {
		return impl_->DestroySession(handle, reason, teardown);
	}

	void AnimationPreviewController::DestroyAllSessions(AnimationPreviewEndReason reason) noexcept {
		impl_->DestroyAllSessions(reason);
	}

	AnimationStatus AnimationPreviewController::Play(AnimationPreviewSessionHandle handle) noexcept {
		return impl_->ApplyTransport(handle, [](AnimatorPlayback& playback) {
			return playback.Play();
		});
	}

	AnimationStatus AnimationPreviewController::Stop(AnimationPreviewSessionHandle handle) noexcept {
		return impl_->ApplyTransport(handle, [](AnimatorPlayback& playback) {
			return playback.Stop();
		});
	}

	AnimationStatus AnimationPreviewController::Pause(AnimationPreviewSessionHandle handle) noexcept {
		return impl_->ApplyTransport(handle, [](AnimatorPlayback& playback) {
			return playback.Pause();
		});
	}

	AnimationStatus AnimationPreviewController::Resume(AnimationPreviewSessionHandle handle) noexcept {
		return impl_->ApplyTransport(handle, [](AnimatorPlayback& playback) {
			return playback.Resume();
		});
	}

	AnimationStatus AnimationPreviewController::Seek(
		AnimationPreviewSessionHandle handle,
		AnimationFixedTick tick) noexcept {
		return impl_->ApplyTransport(handle, [tick](AnimatorPlayback& playback) {
			return playback.Seek(tick);
		});
	}

	AnimationStatus AnimationPreviewController::SeekNormalized(
		AnimationPreviewSessionHandle handle,
		float normalizedTime) noexcept {
		return impl_->SeekNormalized(handle, normalizedTime);
	}

	AnimationStatus AnimationPreviewController::Step(
		AnimationPreviewSessionHandle handle,
		AnimationFixedTick deltaTicks) noexcept {
		return impl_->Step(handle, deltaTicks);
	}

	AnimationStatus AnimationPreviewController::SetLooping(
		AnimationPreviewSessionHandle handle,
		bool looping) noexcept {
		return impl_->ApplyTransport(handle, [looping](AnimatorPlayback& playback) {
			return playback.SetLooping(looping);
		});
	}

	AnimationStatus AnimationPreviewController::SetReverse(
		AnimationPreviewSessionHandle handle,
		bool reverse) noexcept {
		return impl_->ApplyTransport(handle, [reverse](AnimatorPlayback& playback) {
			return playback.SetReverse(reverse);
		});
	}

	AnimationStatus AnimationPreviewController::SetSpeed(
		AnimationPreviewSessionHandle handle,
		float speed) noexcept {
		return impl_->ApplyTransport(handle, [speed](AnimatorPlayback& playback) {
			return playback.SetSpeed(speed);
		});
	}

	AnimationStatus AnimationPreviewController::SetRendererAvailable(
		AnimationPreviewSessionHandle handle,
		bool available) noexcept {
		return impl_->SetRendererAvailable(handle, available);
	}

	AnimationStatus AnimationPreviewController::PrepareTick(
		AnimationPreviewSessionHandle handle,
		double unscaledDeltaSeconds,
		AnimationPreviewPreparedFrame& output) const noexcept {
		return impl_->PrepareTick(handle, unscaledDeltaSeconds, output);
	}

	AnimationStatus AnimationPreviewController::PublishPreparedFrame(
		const AnimationPreviewPreparedFrame& prepared) noexcept {
		return impl_->PublishPreparedFrame(prepared);
	}

	AnimationStatus AnimationPreviewController::TickSession(
		AnimationPreviewSessionHandle handle,
		double unscaledDeltaSeconds) noexcept {
		return impl_->TickSession(handle, unscaledDeltaSeconds);
	}

	AnimationStatus AnimationPreviewController::UpdateLiveInstance(
		AnimationPreviewSessionHandle handle,
		const AnimationPreviewLiveInstanceSnapshot& input) noexcept {
		return impl_->UpdateLiveInstance(handle, input);
	}

	AnimationStatus AnimationPreviewController::GetSessionState(
		AnimationPreviewSessionHandle handle,
		AnimationPreviewSessionState& output) const noexcept {
		return impl_->GetSessionState(handle, output);
	}

	bool AnimationPreviewController::IsSessionAlive(AnimationPreviewSessionHandle handle) const noexcept {
		return impl_->IsSessionAlive(handle);
	}

	std::size_t AnimationPreviewController::SessionCount() const noexcept {
		return impl_->SessionCount();
	}

	AnimationStatus AnimationPreviewController::BeginSession(
		const AnimationPreviewSessionDescriptor& descriptor) noexcept {
		return impl_->BeginLegacySession(descriptor);
	}

	void AnimationPreviewController::EndSession(AnimationPreviewEndReason reason) noexcept {
		impl_->EndLegacySession(reason);
	}

	AnimationStatus AnimationPreviewController::Play() noexcept {
		return impl_->ApplyLegacy([](AnimatorPlayback& playback, const auto&) {
			return playback.Play();
		});
	}

	AnimationStatus AnimationPreviewController::Stop() noexcept {
		return impl_->ApplyLegacy([](AnimatorPlayback& playback, const auto&) {
			return playback.Stop();
		});
	}

	AnimationStatus AnimationPreviewController::Pause() noexcept {
		return impl_->ApplyLegacy([](AnimatorPlayback& playback, const auto&) {
			return playback.Pause();
		});
	}

	AnimationStatus AnimationPreviewController::Resume() noexcept {
		return impl_->ApplyLegacy([](AnimatorPlayback& playback, const auto&) {
			return playback.Resume();
		});
	}

	AnimationStatus AnimationPreviewController::Seek(AnimationFixedTick tick) noexcept {
		return impl_->ApplyLegacy([tick](AnimatorPlayback& playback, const auto&) {
			return playback.Seek(tick);
		});
	}

	AnimationStatus AnimationPreviewController::SeekNormalized(float normalizedTime) noexcept {
		return impl_->SeekLegacyNormalized(normalizedTime);
	}

	AnimationStatus AnimationPreviewController::SetLooping(bool looping) noexcept {
		return impl_->ApplyLegacy([looping](AnimatorPlayback& playback, auto& descriptor) {
			const AnimationStatus status = playback.SetLooping(looping);
			if(status == AnimationStatus::Ok) {
				descriptor.looping = looping;
			}
			return status;
		});
	}

	AnimationStatus AnimationPreviewController::SetReverse(bool reverse) noexcept {
		return impl_->ApplyLegacy([reverse](AnimatorPlayback& playback, auto& descriptor) {
			const AnimationStatus status = playback.SetReverse(reverse);
			if(status == AnimationStatus::Ok) {
				descriptor.reverse = reverse;
			}
			return status;
		});
	}

	AnimationStatus AnimationPreviewController::SetSpeed(float speed) noexcept {
		return impl_->ApplyLegacy([speed](AnimatorPlayback& playback, auto& descriptor) {
			const AnimationStatus status = playback.SetSpeed(speed);
			if(status == AnimationStatus::Ok) {
				descriptor.speed = speed;
			}
			return status;
		});
	}

	AnimationStatus AnimationPreviewController::Tick(double unscaledDeltaSeconds) noexcept {
		return impl_->TickLegacy(unscaledDeltaSeconds);
	}

	void AnimationPreviewController::SynchronizeContext(
		const UUID& selectionId,
		bool playModeActive) noexcept {
		impl_->SynchronizeContext(selectionId, playModeActive);
	}

	AnimationPreviewSnapshot AnimationPreviewController::GetSnapshot() const noexcept {
		return impl_->GetLegacySnapshot();
	}

	AnimationStatus AnimationPreviewController::BeginIsolatedSession(
		std::uint64_t sessionId,
		const AnimationPreviewSessionDescriptor& descriptor) noexcept {
		return impl_->BeginLegacyIsolated(sessionId, descriptor);
	}

	AnimationStatus AnimationPreviewController::TickIsolatedSessions(
		double unscaledDeltaSeconds) noexcept {
		return impl_->TickLegacyIsolated(unscaledDeltaSeconds);
	}

	std::size_t AnimationPreviewController::IsolatedSessionCount() const noexcept {
		return impl_->LegacyIsolatedCount();
	}

	bool AnimationPreviewController::IsActive() const noexcept {
		return impl_->LegacyActive();
	}
}
