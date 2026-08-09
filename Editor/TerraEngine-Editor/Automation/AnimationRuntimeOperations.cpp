// AnimationRuntimeOperations.cpp - typed deferred runtime commands and safe reads.

#include "AnimationRuntimeOperations.h"
#include "AnimationAutomationSchemas.h"
#include "AnimationAutomationJobs.h"

#include "Editor/AssetCompilers/MotionDatabaseCompiler.h"

#include "Engine/Animation/AnimationSystem.h"
#include "Engine/Animation/Diagnostics/AnimationDiagnostics.h"
#include "Utilities/UUID.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace TerraEngine::Editor::Automation
{
	bool AnimationAutomationDetailedDiagnosticsLease::EnsureRequested() noexcept
	{
		if(request_.has_value() && request_->IsActive())
		{
			return true;
		}
		request_.emplace(TerraEngine::Animation::AcquireAnimationDetailedDiagnosticsRequest());
		return request_->IsActive();
	}

	bool AnimationAutomationDetailedDiagnosticsLease::IsRequested() const noexcept
	{
		return request_.has_value() && request_->IsActive();
	}

	void AnimationAutomationDetailedDiagnosticsLease::Reset() noexcept
	{
		if(request_.has_value())
		{
			request_->Reset();
			request_.reset();
		}
	}

	namespace
	{
		using namespace TerraEngine::Animation;

		std::uint64_t StableIdentity(std::string_view value) noexcept
		{
			// FNV-1a is an opaque process-independent queue identity. A collision is
			// handled safely by the native duplicate checks as a rejected submission.
			std::uint64_t hash = 14695981039346656037ULL;
			for(const unsigned char character : value)
			{
				hash ^= character;
				hash *= 1099511628211ULL;
			}
			return hash == 0U ? 1U : hash;
		}

		AutomationDispatchResult Fail(ErrorCategory category, std::string message)
		{
			AutomationDispatchResult result;
			result.error = category;
			result.message = std::move(message);
			return result;
		}

		std::string MachineStatus(AnimationStatus status)
		{
			// Native status names are display-oriented title case. Automation fields
			// use stable lowercase tokens so clients never need UI-string knowledge.
			std::string value(ToString(status));
			std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
				return static_cast<char>(std::tolower(character));
			});
			return value;
		}

		std::optional<float> ReadFiniteFloat(const nlohmann::json& value)
		{
			if(!value.is_number())
			{
				return std::nullopt;
			}
			try
			{
				const double number = value.get<double>();
				if(!std::isfinite(number)
					|| number < -static_cast<double>(std::numeric_limits<float>::max())
					|| number > static_cast<double>(std::numeric_limits<float>::max()))
				{
					return std::nullopt;
				}
				return static_cast<float>(number);
			}
			catch(const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		std::optional<AnimationFloat3> ReadFloat3(const nlohmann::json& value)
		{
			if(!value.is_array() || value.size() != 3U)
			{
				return std::nullopt;
			}
			const std::optional<float> x = ReadFiniteFloat(value[0U]);
			const std::optional<float> y = ReadFiniteFloat(value[1U]);
			const std::optional<float> z = ReadFiniteFloat(value[2U]);
			if(!x || !y || !z)
			{
				return std::nullopt;
			}
			return AnimationFloat3{*x, *y, *z};
		}

		std::optional<AnimationQuaternion> ReadQuaternion(const nlohmann::json& value)
		{
			if(!value.is_array() || value.size() != 4U)
			{
				return std::nullopt;
			}
			std::array<std::optional<float>, 4U> components{
				ReadFiniteFloat(value[0U]), ReadFiniteFloat(value[1U]),
				ReadFiniteFloat(value[2U]), ReadFiniteFloat(value[3U])};
			if(!components[0U] || !components[1U] || !components[2U] || !components[3U])
			{
				return std::nullopt;
			}
			return AnimationQuaternion{
				*components[0U], *components[1U], *components[2U], *components[3U]};
		}

		std::optional<AnimationTransform> ReadTransform(const nlohmann::json& value)
		{
			if(!value.is_object())
			{
				return std::nullopt;
			}
			const std::optional<AnimationFloat3> translation = ReadFloat3(value.value("translation", nlohmann::json{}));
			const std::optional<AnimationQuaternion> rotation = ReadQuaternion(value.value("rotation", nlohmann::json{}));
			const std::optional<AnimationFloat3> scale = ReadFloat3(value.value("scale", nlohmann::json{}));
			if(!translation || !rotation || !scale)
			{
				return std::nullopt;
			}
			return AnimationTransform{*translation, *rotation, *scale};
		}

		std::optional<std::int64_t> ReadNonNegativeTick(const nlohmann::json& value)
		{
			if(!value.is_number_unsigned() && !value.is_number_integer())
			{
				return std::nullopt;
			}
			try
			{
				const std::uint64_t tick = value.get<std::uint64_t>();
				if(tick > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				{
					return std::nullopt;
				}
				return static_cast<std::int64_t>(tick);
			}
			catch(const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		std::optional<AnimatorInstanceHandle> ResolveTarget(const nlohmann::json& target)
		{
			if(!target.is_object())
			{
				return std::nullopt;
			}
			const bool hasObject = target.contains("objectId");
			const bool hasAnimator = target.contains("animator");
			if(hasObject == hasAnimator)
			{
				return std::nullopt;
			}
			AnimationSystem& system = AnimationSystem::Instance();
			if(hasObject)
			{
				const std::optional<UUID> objectId = TerraEngine::ParseUUID(target.value("objectId", std::string{}));
				if(!objectId)
				{
					return std::nullopt;
				}
				const AnimatorInstanceHandle resolved = system.ResolveAnimatorForObject(*objectId);
				return resolved.IsValid() ? std::optional<AnimatorInstanceHandle>(resolved) : std::nullopt;
			}

			const nlohmann::json& animator = target["animator"];
			try
			{
				const std::uint64_t slot = animator.at("slotIndex").get<std::uint64_t>();
				const std::uint64_t generation = animator.at("generation").get<std::uint64_t>();
				if(slot > std::numeric_limits<std::uint32_t>::max()
					|| generation == 0U || generation > std::numeric_limits<std::uint32_t>::max())
				{
					return std::nullopt;
				}
				return AnimatorInstanceHandle{
					static_cast<std::uint32_t>(slot), static_cast<std::uint32_t>(generation)};
			}
			catch(const nlohmann::json::exception&)
			{
				return std::nullopt;
			}
		}

		std::optional<AnimationCommandKind> ParseCommandKind(std::string_view kind) noexcept
		{
			if(kind == "set_parameter") return AnimationCommandKind::SetParameter;
			if(kind == "fire_trigger") return AnimationCommandKind::FireTrigger;
			if(kind == "play") return AnimationCommandKind::Play;
			if(kind == "stop") return AnimationCommandKind::Stop;
			if(kind == "pause") return AnimationCommandKind::Pause;
			if(kind == "resume") return AnimationCommandKind::Resume;
			if(kind == "seek") return AnimationCommandKind::Seek;
			if(kind == "set_looping") return AnimationCommandKind::SetLooping;
			if(kind == "set_reverse") return AnimationCommandKind::SetReverse;
			if(kind == "set_speed") return AnimationCommandKind::SetSpeed;
			return std::nullopt;
		}

		std::optional<NativeAnimationProceduralGoalKind> ParseProceduralGoalKind(
			std::string_view value) noexcept
		{
			if(value == "position") return NativeAnimationProceduralGoalKind::Position;
			if(value == "rotation") return NativeAnimationProceduralGoalKind::Rotation;
			if(value == "position_and_rotation") return NativeAnimationProceduralGoalKind::PositionAndRotation;
			if(value == "aim") return NativeAnimationProceduralGoalKind::Aim;
			if(value == "look") return NativeAnimationProceduralGoalKind::Look;
			if(value == "foot_plant") return NativeAnimationProceduralGoalKind::FootPlant;
			if(value == "copy_transform") return NativeAnimationProceduralGoalKind::CopyTransform;
			if(value == "twist") return NativeAnimationProceduralGoalKind::Twist;
			return std::nullopt;
		}

		std::optional<NativeAnimationProceduralCoordinateSpace> ParseProceduralCoordinateSpace(
			std::string_view value) noexcept
		{
			if(value == "local") return NativeAnimationProceduralCoordinateSpace::Local;
			if(value == "parent") return NativeAnimationProceduralCoordinateSpace::Parent;
			if(value == "model") return NativeAnimationProceduralCoordinateSpace::Model;
			if(value == "world") return NativeAnimationProceduralCoordinateSpace::World;
			return std::nullopt;
		}

		std::optional<NativeAnimationRetargetOperation> ParseRetargetOperation(
			std::string_view value) noexcept
		{
			if(value == "activate_profile") return NativeAnimationRetargetOperation::ActivateProfile;
			if(value == "deactivate_profile") return NativeAnimationRetargetOperation::DeactivateProfile;
			if(value == "request_rebuild") return NativeAnimationRetargetOperation::RequestRebuild;
			return std::nullopt;
		}

		std::optional<NativeAnimationMotionMatchOperation> ParseMotionMatchOperation(std::string_view value) noexcept
		{
			if(value == "set_database") return NativeAnimationMotionMatchOperation::SetDatabase;
			if(value == "query") return NativeAnimationMotionMatchOperation::Query;
			if(value == "clear") return NativeAnimationMotionMatchOperation::Clear;
			return std::nullopt;
		}

		std::optional<NativeAnimationRagdollOperation> ParseRagdollOperation(std::string_view value) noexcept
		{
			if(value == "activate") return NativeAnimationRagdollOperation::Activate;
			if(value == "deactivate") return NativeAnimationRagdollOperation::Deactivate;
			if(value == "request_recovery") return NativeAnimationRagdollOperation::RequestRecovery;
			return std::nullopt;
		}

		NativeAnimationAbiHeader MakeNativeHeader(NativeAnimationRecordKind kind, std::size_t size) noexcept
		{
			return {static_cast<std::uint32_t>(size), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
		}

		NativeAnimationUuid ToNativeUuid(const UUID& value) noexcept
		{
			NativeAnimationUuid native{};
			std::memcpy(&native.first, value.data(), sizeof(native.first));
			std::memcpy(&native.second, value.data() + sizeof(native.first), sizeof(native.second));
			return native;
		}

		std::string ToHexString(NativeAnimationUuid value)
		{
			UUID uuid{};
			std::memcpy(uuid.data(), &value.first, sizeof(value.first));
			std::memcpy(uuid.data() + sizeof(value.first), &value.second, sizeof(value.second));
			return TerraEngine::ToHexString(uuid);
		}

		NativeAnimationTransform ToNativeTransform(const AnimationTransform& value) noexcept
		{
			return {{value.translation.x, value.translation.y, value.translation.z},
				{value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w},
				{value.scale.x, value.scale.y, value.scale.z}};
		}

		std::string MachineNativeStatus(NativeAnimationStatus status)
		{
			return MachineStatus(static_cast<AnimationStatus>(status));
		}

		ErrorCategory MapNativeFailure(NativeAnimationStatus status) noexcept
		{
			switch(status)
			{
			case NativeAnimationStatus::InvalidArgument:
			case NativeAnimationStatus::InvalidType:
			case NativeAnimationStatus::IncompatibleAsset: return ErrorCategory::Validation;
			case NativeAnimationStatus::MissingAsset: return ErrorCategory::MissingTarget;
			case NativeAnimationStatus::StaleHandle: return ErrorCategory::StaleTarget;
			case NativeAnimationStatus::Cancelled: return ErrorCategory::Canceled;
			case NativeAnimationStatus::Unsupported: return ErrorCategory::UnsupportedCapability;
			case NativeAnimationStatus::HardLimitExceeded:
			case NativeAnimationStatus::BufferTooSmall: return ErrorCategory::OverBudget;
			case NativeAnimationStatus::Rejected: return ErrorCategory::Conflict;
			default: return ErrorCategory::Internal;
			}
		}

		bool IsAcceptedNativeStatus(NativeAnimationStatus status) noexcept
		{
			return status == NativeAnimationStatus::Queued || status == NativeAnimationStatus::Ok;
		}

		std::optional<AnimationClipRecordingClock> ParseRecordingClock(std::string_view value) noexcept
		{
			if(value == "fixed_simulation") return AnimationClipRecordingClock::FixedSimulation;
			if(value == "scaled_game_time") return AnimationClipRecordingClock::ScaledGameTime;
			if(value == "unscaled_game_time") return AnimationClipRecordingClock::UnscaledGameTime;
			if(value == "editor_preview") return AnimationClipRecordingClock::EditorPreview;
			if(value == "manual") return AnimationClipRecordingClock::Manual;
			return std::nullopt;
		}

		std::optional<AnimationClipRecordedChannelKind> ParseRecordingChannelKind(std::string_view value) noexcept
		{
			if(value == "bone_translation") return AnimationClipRecordedChannelKind::BoneTranslation;
			if(value == "bone_rotation") return AnimationClipRecordedChannelKind::BoneRotation;
			if(value == "bone_scale") return AnimationClipRecordedChannelKind::BoneScale;
			if(value == "morph_weight") return AnimationClipRecordedChannelKind::MorphWeight;
			if(value == "curve") return AnimationClipRecordedChannelKind::Curve;
			if(value == "event") return AnimationClipRecordedChannelKind::Event;
			if(value == "root_translation") return AnimationClipRecordedChannelKind::RootTranslation;
			if(value == "root_yaw") return AnimationClipRecordedChannelKind::RootYaw;
			return std::nullopt;
		}

		std::optional<AnimationInterpolation> ParseRecordingInterpolation(std::string_view value) noexcept
		{
			if(value == "constant") return AnimationInterpolation::Constant;
			if(value == "step") return AnimationInterpolation::Step;
			if(value == "linear") return AnimationInterpolation::Linear;
			if(value == "cubic") return AnimationInterpolation::Cubic;
			return std::nullopt;
		}

		bool IsPathWithinRoot(const std::filesystem::path& root, const std::filesystem::path& candidate) noexcept
		{
			auto rootPart = root.begin();
			auto candidatePart = candidate.begin();
			for(; rootPart != root.end(); ++rootPart, ++candidatePart) {
				if(candidatePart == candidate.end() || *rootPart != *candidatePart) return false;
			}
			return true;
		}

		AutomationDispatchResult StartRecording(
			const nlohmann::json& arguments,
			AnimationAutomationJobs& jobs,
			const std::filesystem::path& assetRoot)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target || !AnimationSystem::Instance().IsAnimatorInstanceAlive(*target)) {
				return Fail(ErrorCategory::StaleTarget,
					"Recording requires one currently live animator target.");
			}
			const auto assetId = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
			const auto skeletonId = TerraEngine::ParseUUID(arguments.value("skeletonAssetId", std::string{}));
			const auto sourceAnimatorId = TerraEngine::ParseUUID(arguments.value("sourceAnimatorId", std::string{}));
			const std::optional<AnimationClipRecordingClock> clock =
				ParseRecordingClock(arguments.value("clock", std::string{}));
			const std::filesystem::path requestedDestination = arguments.value("destinationPath", std::string{});
			if(!assetId || !skeletonId || !sourceAnimatorId || !clock || assetRoot.empty()
				|| requestedDestination.empty() || requestedDestination.is_absolute()
				|| requestedDestination.extension() != ".teanim") {
				return Fail(ErrorCategory::Validation,
					"Recording requires checked asset identities, a known clock, and a relative .teanim destination.");
			}
			const std::filesystem::path destination =
				std::filesystem::absolute(assetRoot / requestedDestination).lexically_normal();
			const std::filesystem::path normalizedRoot = std::filesystem::absolute(assetRoot).lexically_normal();
			if(!IsPathWithinRoot(normalizedRoot, destination)) {
				return Fail(ErrorCategory::Validation,
					"Recording destination must remain beneath the open project's Assets root.");
			}

			Animation::AnimationRecorderDescriptor descriptor;
			descriptor.assetId = {*assetId};
			descriptor.skeletonAssetId = {*skeletonId};
			descriptor.compatibilityFingerprint = arguments.value("compatibilityFingerprint", 0ULL);
			descriptor.firstTick = arguments.value("firstTick", std::int64_t{-1});
			descriptor.lastTick = arguments.value("lastTick", std::int64_t{-1});
			descriptor.sampleRate = arguments.value("sampleRate", 0U);
			descriptor.clock = *clock;
			descriptor.samplesPerChunk = arguments.value("samplesPerChunk", 256U);
			descriptor.maximumCapturedSamples = arguments.value("maximumCapturedSamples", 0ULL);
			descriptor.maximumStagedBytes = arguments.value("maximumStagedBytes", 256ULL * 1024ULL * 1024ULL);
			descriptor.segmentDurationTicks = arguments.value("segmentDurationTicks", std::int64_t{0});
			descriptor.destinationPath = destination;
			// A bind-current request carries a relative recording duration because
			// its approval can remain open while the live animator keeps publishing.
			// Validate that duration before rebasing it onto the approval-time pose.
			if(descriptor.firstTick < 0 || descriptor.lastTick < descriptor.firstTick)
			{
				return Fail(ErrorCategory::Validation,
					"Recording requires a non-negative ordered tick range.");
			}
			const std::int64_t requestedTickSpan = descriptor.lastTick - descriptor.firstTick;
			descriptor.provenance.sourceAnimatorId = {*sourceAnimatorId};
			descriptor.provenance.sourceSceneIdentity = arguments.value("sourceSceneIdentity", std::string{});
			// The existing provenance chunk has no incompatible feature-specific flags.
			// Record the truthful capture policy in its durable session identity: all
			// selected pose tracks contain the already-composed retarget/procedural/
			// ragdoll result, never live subsystem references.
			descriptor.provenance.sourceSessionIdentity = arguments.value("sourceSessionIdentity", std::string{})
				+ ";final_pose_tracks_bake=retarget,procedural,ragdoll";
			for(const nlohmann::json& input : arguments["channels"]) {
				const auto kind = ParseRecordingChannelKind(input.value("kind", std::string{}));
				const auto interpolation = ParseRecordingInterpolation(input.value("interpolation", std::string{}));
				if(!kind || !interpolation) return Fail(ErrorCategory::Validation, "Recording channel kind or interpolation is unsupported.");
				descriptor.channels.push_back({input.value("stableChannelId", 0ULL),
					input.value("stableTargetId", 0ULL), input.value("name", std::string{}), *kind, *interpolation});
			}
			Animation::AnimationRecorderSnapshotBinding binding;
			binding.animator = *target;
			binding.expectedPublicationGeneration = arguments.value("publicationGeneration", 0U);
			const bool bindCurrent = arguments.value("bindCurrentPublication", false);
			std::shared_ptr<const TerraEngine::Animation::AnimationAuthorityFrame> boundAuthority;
			const TerraEngine::Animation::AnimationAuthorityRow* boundPose = nullptr;
			if(bindCurrent) {
				boundAuthority = AnimationSystem::Instance().LoadPublishedAuthorityFrame();
				boundPose = boundAuthority ? boundAuthority->Find(*target) : nullptr;
				if(!boundPose || !boundPose->snapshot) return Fail(ErrorCategory::StaleTarget, "No immutable published animator pose is available for current binding.");
				binding.expectedPublicationGeneration = boundPose->snapshot->publicationGeneration;
				binding.jointStableIds = boundPose->jointStableIds;
				if(!boundPose->skeleton.assetId.IsValid() || boundPose->ownerObjectId == TerraEngine::EMPTY_UUID
					|| binding.jointStableIds.empty()) return Fail(ErrorCategory::Validation, "Published animator snapshot lacks stable recording metadata.");
				descriptor.skeletonAssetId = boundPose->skeleton.assetId;
				descriptor.compatibilityFingerprint = boundPose->skeletonCompatibilityFingerprint;
				descriptor.provenance.sourceAnimatorId = {boundPose->ownerObjectId};
				// The current immutable publication is sample zero. Preserve the
				// caller-approved duration, but move the whole range to begin at
				// the pose that actually exists when the user approves the request.
				if(boundPose->snapshot->tick < 0
					|| requestedTickSpan > std::numeric_limits<std::int64_t>::max() - boundPose->snapshot->tick)
				{
					return Fail(ErrorCategory::Validation,
						"The approval-time recording range cannot be represented safely.");
				}
				descriptor.firstTick = boundPose->snapshot->tick;
				descriptor.lastTick = descriptor.firstTick + requestedTickSpan;
			}
			else if(arguments.contains("jointStableIds")) binding.jointStableIds = arguments["jointStableIds"].get<std::vector<std::uint64_t>>();
			if(binding.expectedPublicationGeneration == 0U || binding.jointStableIds.empty()) {
				return Fail(ErrorCategory::Validation,
					"Recording requires an explicit published generation and copied non-empty stable-joint ID binding.");
			}
			// Even explicit caller bindings must resolve to one current immutable approval
			// pose. The recorder retains that value as sample zero and never reads live
			// animation storage while the durable job is active.
			if(!boundPose) {
				boundAuthority = AnimationSystem::Instance().LoadPublishedAuthorityFrame();
				boundPose = boundAuthority ? boundAuthority->Find(*target) : nullptr;
			}
			if(!boundPose || !boundPose->snapshot
				|| boundPose->snapshot->publicationGeneration != binding.expectedPublicationGeneration
				|| boundPose->snapshot->tick != descriptor.firstTick
				|| boundPose->skeleton.assetId != descriptor.skeletonAssetId
				|| boundPose->skeletonCompatibilityFingerprint != descriptor.compatibilityFingerprint
				|| boundPose->jointStableIds != binding.jointStableIds)
			{
				return Fail(ErrorCategory::StaleTarget,
					"Recording approval no longer matches the declared range, skeleton compatibility, or stable joint binding.");
			}
			AutomationDispatchResult started = jobs.StartRecording({"animation_recording", "animation.recording.finalize",
				arguments.value("correlationId", std::string{}), arguments.value("correlationId", std::string{}),
				arguments.value("idempotencyKey", std::string{}), true}, descriptor, binding, boundPose->snapshot);
			if(started.ok) {
				started.data["boundPublicationGeneration"] = binding.expectedPublicationGeneration;
				started.data["boundTick"] = boundPose->snapshot->tick;
				started.data["boundJointStableIds"] = binding.jointStableIds;
				started.data["boundSkeletonAssetId"] = TerraEngine::ToHexString(boundPose->skeleton.assetId.value);
				started.data["boundSourceAnimatorId"] =
					TerraEngine::ToHexString(descriptor.provenance.sourceAnimatorId.value);
			}
			return started;
		}

		AutomationDispatchResult StartMotionDatabaseBuild(
			const nlohmann::json& arguments,
			AnimationAutomationTransactions& transactions,
			std::uint64_t generation,
			AnimationAutomationJobs& jobs,
			const std::filesystem::path& assetRoot)
		{
			if(assetRoot.empty()) return Fail(ErrorCategory::Unavailable,
				"Motion database builds require the open project's Assets root.");
			const std::filesystem::path authoringRelative = arguments.value("authoringPath", std::string{});
			const std::filesystem::path outputRelative = arguments.value("outputPath", std::string{});
			const std::filesystem::path normalizedRoot = std::filesystem::absolute(assetRoot).lexically_normal();
			const std::filesystem::path authoringPath = std::filesystem::absolute(
				normalizedRoot / authoringRelative).lexically_normal();
			const std::filesystem::path outputPath = std::filesystem::absolute(
				normalizedRoot / outputRelative).lexically_normal();
			if(authoringRelative.empty() || outputRelative.empty() || authoringRelative.is_absolute()
				|| outputRelative.is_absolute() || authoringPath.extension() != ".json"
				|| outputPath.extension() != ".temotiondb"
				|| !IsPathWithinRoot(normalizedRoot, authoringPath)
				|| !IsPathWithinRoot(normalizedRoot, outputPath))
			{
				return Fail(ErrorCategory::Validation,
					"Motion database authoring and output paths must be relative paths beneath Assets with .json and .temotiondb extensions.");
			}
			const bool dryRun = arguments.value("dryRun", false);
			if(dryRun)
			{
				AssetPipeline::MotionDatabaseAuthoringDocument document;
				const AnimationStatus status = AssetPipeline::LoadMotionDatabaseAuthoringDocument(authoringPath, document);
				if(status != AnimationStatus::Ok) return Fail(status == AnimationStatus::MissingAsset
					? ErrorCategory::MissingTarget : ErrorCategory::Validation,
					"The motion database authoring sidecar failed dry-run validation.");
				AutomationDispatchResult result;
				result.ok = true;
				result.message = "Validated the motion database build inputs without queueing worker work.";
				result.data = {{"status", "validated"}, {"visibilityBoundary", "none_dry_run"},
					{"authoringGeneration", document.authoringGeneration}};
				return result;
			}
			// The transaction commits only the durable-job admission. The existing
			// authoring session owns all worker cooking and atomic publication later.
			AnimationAutomationMutation mutation{"animation.motion_database.build",
				arguments.value("idempotencyKey", std::string{}), arguments.value("correlationId", std::string{}),
				arguments.value("expectedGeneration", 0ULL), false, arguments};
			return transactions.ExecuteWithCommit(mutation, generation, [&jobs, &arguments, authoringPath, outputPath]() {
				return jobs.StartMotionDatabaseBuild({"motion_database_build", "animation.motion_database.build",
					arguments.value("correlationId", std::string{}), arguments.value("correlationId", std::string{}),
					arguments.value("idempotencyKey", std::string{}), true}, authoringPath, outputPath);
			});
		}

		std::optional<std::string> PopulateTypedPayload(
			const nlohmann::json& input,
			AnimationCommand& output)
		{
			switch(output.kind)
			{
			case AnimationCommandKind::SetParameter:
			{
				if(!input.contains("parameterId") || !input.contains("parameterType"))
				{
					return "set_parameter requires parameterId and parameterType.";
				}
				output.parameterId = input["parameterId"].get<std::uint32_t>();
				const std::string type = input["parameterType"].get<std::string>();
				if(type == "boolean" && input.contains("booleanValue"))
				{
					output.parameterType = AnimationParameterType::Boolean;
					output.value = input["booleanValue"].get<bool>();
					return std::nullopt;
				}
				if(type == "integer" && input.contains("integerValue")
					&& (input["integerValue"].is_number_integer() || input["integerValue"].is_number_unsigned()))
				{
					const std::int64_t value = input["integerValue"].get<std::int64_t>();
					if(value < std::numeric_limits<std::int32_t>::min()
						|| value > std::numeric_limits<std::int32_t>::max())
					{
						return "integerValue is outside the signed 32-bit parameter range.";
					}
					output.parameterType = AnimationParameterType::Integer;
					output.value = static_cast<std::int32_t>(value);
					return std::nullopt;
				}
				if(type == "float" && input.contains("floatValue"))
				{
					const std::optional<float> value = ReadFiniteFloat(input["floatValue"]);
					if(!value) return "floatValue must fit a finite 32-bit float.";
					output.parameterType = AnimationParameterType::Float;
					output.value = *value;
					return std::nullopt;
				}
				if(type == "vector3" && input.contains("vectorValue"))
				{
					const std::optional<AnimationFloat3> value = ReadFloat3(input["vectorValue"]);
					if(!value) return "vectorValue must contain three finite 32-bit floats.";
					output.parameterType = AnimationParameterType::Vector3;
					output.value = *value;
					return std::nullopt;
				}
				if(type == "quaternion" && input.contains("quaternionValue"))
				{
					const std::optional<AnimationQuaternion> value = ReadQuaternion(input["quaternionValue"]);
					if(!value) return "quaternionValue must contain four finite 32-bit floats.";
					output.parameterType = AnimationParameterType::Quaternion;
					output.value = *value;
					return std::nullopt;
				}
				if(type == "transform" && input.contains("transformValue"))
				{
					const std::optional<AnimationTransform> value = ReadTransform(input["transformValue"]);
					if(!value) return "transformValue must contain finite translation, rotation, and scale values.";
					output.parameterType = AnimationParameterType::Transform;
					output.value = *value;
					return std::nullopt;
				}
				return "set_parameter is missing the value field selected by parameterType.";
			}
			case AnimationCommandKind::FireTrigger:
				if(!input.contains("parameterId")) return "fire_trigger requires parameterId.";
				output.parameterId = input["parameterId"].get<std::uint32_t>();
				output.parameterType = AnimationParameterType::Trigger;
				output.value = std::monostate{};
				return std::nullopt;
			case AnimationCommandKind::Seek:
				if(!input.contains("seekTick")) return "seek requires seekTick.";
				if(const std::optional<std::int64_t> tick = ReadNonNegativeTick(input["seekTick"]))
				{
					output.playbackTick = *tick;
					return std::nullopt;
				}
				return "seekTick is outside the supported fixed-tick range.";
			case AnimationCommandKind::SetLooping:
			case AnimationCommandKind::SetReverse:
				if(!input.contains("flag")) return "The playback flag command requires flag.";
				output.playbackFlag = input["flag"].get<bool>();
				return std::nullopt;
			case AnimationCommandKind::SetSpeed:
				if(!input.contains("speed")) return "set_speed requires speed.";
				if(const std::optional<float> speed = ReadFiniteFloat(input["speed"]))
				{
					output.playbackSpeed = *speed;
					return std::nullopt;
				}
				return "speed must fit a finite 32-bit float.";
			case AnimationCommandKind::Play:
			case AnimationCommandKind::Stop:
			case AnimationCommandKind::Pause:
			case AnimationCommandKind::Resume:
				return std::nullopt;
			default:
				return "The animation command kind is unsupported.";
			}
		}

		AutomationDispatchResult BuildCommands(
			const nlohmann::json& arguments,
			AnimatorInstanceHandle target,
			std::vector<AnimationCommand>& output)
		{
			const std::string idempotencyKey = arguments.value("idempotencyKey", std::string{});
			const std::string correlationId = arguments.value("correlationId", std::string{});
			const std::optional<std::int64_t> defaultTick = arguments.contains("targetTick")
				? ReadNonNegativeTick(arguments["targetTick"])
				: std::optional<std::int64_t>(0);
			if(!defaultTick)
			{
				return Fail(ErrorCategory::Validation, "targetTick is outside the supported fixed-tick range.");
			}

			const nlohmann::json& commands = arguments["commands"];
			output.clear();
			output.reserve(commands.size());
			for(std::size_t index = 0U; index < commands.size(); ++index)
			{
				const nlohmann::json& input = commands[index];
				AnimationCommand command;
				command.target = target;
				command.kind = *ParseCommandKind(input["kind"].get<std::string>());
				command.targetTick = *defaultTick;
				if(input.contains("targetTick"))
				{
					const std::optional<std::int64_t> commandTick = ReadNonNegativeTick(input["targetTick"]);
					if(!commandTick)
					{
						return Fail(ErrorCategory::Validation, "commands[" + std::to_string(index) + "].targetTick is outside the supported fixed-tick range.");
					}
					command.targetTick = *commandTick;
				}
				command.producerId = 0x4155544FU;
				const std::string suffix = "#" + std::to_string(index);
				command.producerSequence = StableIdentity(idempotencyKey + "#sequence" + suffix);
				command.correlationId = StableIdentity(correlationId + "#correlation" + suffix);
				command.idempotencyKey = StableIdentity(idempotencyKey + "#idempotency" + suffix);
				command.cancellationToken = StableIdentity(idempotencyKey + "#cancellation" + suffix);
				if(const std::optional<std::string> error = PopulateTypedPayload(input, command))
				{
					return Fail(ErrorCategory::Validation, "commands[" + std::to_string(index) + "]: " + *error);
				}
				output.push_back(std::move(command));
			}

			AutomationDispatchResult result;
			result.ok = true;
			return result;
		}

		ErrorCategory MapCommandFailure(const AnimationCommandBatchReceipt& batch) noexcept
		{
			switch(batch.status)
			{
			case AnimationStatus::InvalidArgument:
			case AnimationStatus::InvalidType: return ErrorCategory::Validation;
			case AnimationStatus::StaleHandle: return ErrorCategory::StaleTarget;
			case AnimationStatus::Cancelled: return ErrorCategory::Canceled;
			case AnimationStatus::Unsupported: return ErrorCategory::UnsupportedCapability;
			case AnimationStatus::HardLimitExceeded:
			case AnimationStatus::BufferTooSmall: return ErrorCategory::OverBudget;
			case AnimationStatus::Rejected:
				return std::any_of(batch.commands.begin(), batch.commands.end(), [](const AnimationCommandReceipt& receipt) {
					return receipt.duplicate;
				}) ? ErrorCategory::Conflict : ErrorCategory::QueueFull;
			default: return ErrorCategory::Internal;
			}
		}

		nlohmann::json SerializeReceipt(const AnimationCommandReceipt& receipt)
		{
			return {
				{"target", {{"slotIndex", receipt.target.slotIndex}, {"generation", receipt.target.generation}}},
				{"producerId", receipt.producerId},
				{"producerSequence", receipt.producerSequence},
				{"correlationId", receipt.correlationId},
				{"idempotencyKey", receipt.idempotencyKey},
				{"cancellationToken", receipt.cancellationToken},
				{"status", MachineStatus(receipt.status)},
				{"targetTick", receipt.targetTick},
				{"observableTick", receipt.observableTick},
				{"publicationGeneration", receipt.publicationGeneration},
				{"duplicate", receipt.duplicate}
			};
		}

		AutomationDispatchResult ExecuteParameterBatch(
			const nlohmann::json& arguments,
			AnimationAutomationTransactions& transactions,
			std::uint64_t generation)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target)
			{
				return Fail(ErrorCategory::MissingTarget,
					"target must select exactly one live animator handle or object UUID.");
			}
			AnimationSystem& system = AnimationSystem::Instance();
			if(!system.IsAnimatorInstanceAlive(*target))
			{
				return Fail(ErrorCategory::StaleTarget,
					"The selected animator generation is no longer alive.");
			}

			std::vector<AnimationCommand> commands;
			if(AutomationDispatchResult built = BuildCommands(arguments, *target, commands); !built.ok)
			{
				return built;
			}
			const bool dryRun = arguments.value("dryRun", false);
			AnimationAutomationMutation mutation{
				"animation.runtime.parameters",
				arguments.value("idempotencyKey", std::string{}),
				arguments.value("correlationId", std::string{}),
				arguments.value("expectedGeneration", 0ULL),
				dryRun,
				arguments};
			return transactions.ExecuteWithCommit(mutation, generation, [&system, &commands, dryRun]() {
				AutomationDispatchResult result;
				nlohmann::json receipts = nlohmann::json::array();
				if(dryRun)
				{
					for(const AnimationCommand& command : commands)
					{
						AnimationCommandReceipt receipt;
						receipt.target = command.target;
						receipt.producerId = command.producerId;
						receipt.producerSequence = command.producerSequence;
						receipt.correlationId = command.correlationId;
						receipt.idempotencyKey = command.idempotencyKey;
						receipt.cancellationToken = command.cancellationToken;
						receipt.status = AnimationStatus::Ok;
						receipt.targetTick = command.targetTick;
						receipts.push_back(SerializeReceipt(receipt));
					}
					result.ok = true;
					result.message = "Validated the typed animation command batch without queueing it.";
					result.data = {
						{"status", "validated"},
						{"commandCount", commands.size()},
						{"commands", std::move(receipts)},
						{"visibilityBoundary", "none_dry_run"}
					};
					return result;
				}

				const AnimationCommandBatchReceipt batch = system.SubmitCommandBatch(commands);
				if(batch.status != AnimationStatus::Queued && batch.status != AnimationStatus::Ok)
				{
					result.error = MapCommandFailure(batch);
					result.message = "The typed animation command batch was rejected atomically with status "
						+ std::string(ToString(batch.status)) + ".";
					result.data = {
						{"status", MachineStatus(batch.status)},
						{"commandCount", batch.commands.size()},
						{"queuedCount", 0U}
					};
					return result;
				}
				for(const AnimationCommandReceipt& receipt : batch.commands)
				{
					receipts.push_back(SerializeReceipt(receipt));
				}
				result.ok = true;
				result.message = batch.status == AnimationStatus::Ok
					? "Reconciled the exact typed animation command batch."
					: "Queued the typed animation command batch for the protected animation boundary.";
				result.data = {
					{"status", MachineStatus(batch.status)},
					{"commandCount", batch.commands.size()},
					{"queuedCount", batch.status == AnimationStatus::Queued ? batch.commands.size() : 0U},
					{"commands", std::move(receipts)},
					{"visibilityBoundary", "animation_protected_capture_then_publication"}
				};
				return result;
			});
		}

		nlohmann::json SerializeRuntimeTarget(AnimatorInstanceHandle target)
		{
			return {{"slotIndex", target.slotIndex}, {"generation", target.generation}};
		}

		nlohmann::json SerializePublishedGenerations(AnimatorInstanceHandle target)
		{
			const std::shared_ptr<const AnimationAuthorityFrame> authority =
				AnimationSystem::Instance().LoadPublishedAuthorityFrame();
			const AnimationAuthorityRow* published = authority ? authority->Find(target) : nullptr;
			if(!published || !published->snapshot)
			{
				return {{"available", false}};
			}
			return {
				{"available", true},
				{"poseGeneration", published->snapshot->publicationGeneration},
				{"authorityPublicationGeneration", published->authorityPublicationGeneration},
				{"tick", published->snapshot->tick}
			};
		}

		nlohmann::json SerializeProceduralGoal(const NativeAnimationProceduralGoalCommand& command)
		{
			return {
				{"command", command.command == NativeAnimationProceduralCommandKind::SetGoal ? "set" : "clear"},
				{"goalKind", static_cast<std::uint32_t>(command.goalKind)},
				{"coordinateSpace", static_cast<std::uint32_t>(command.coordinateSpace)},
				{"layerIndex", command.layerIndex}, {"goalId", command.goalId},
				{"targetJointStableId", command.targetJointStableId},
				{"goalTransform", {{"translation", {command.goalTransform.translation.x, command.goalTransform.translation.y, command.goalTransform.translation.z}},
					{"rotation", {command.goalTransform.rotation.x, command.goalTransform.rotation.y, command.goalTransform.rotation.z, command.goalTransform.rotation.w}},
					{"scale", {command.goalTransform.scale.x, command.goalTransform.scale.y, command.goalTransform.scale.z}}}},
				{"poleVector", {command.poleVector.x, command.poleVector.y, command.poleVector.z}},
				{"priority", command.priority}, {"positionWeight", command.positionWeight},
				{"rotationWeight", command.rotationWeight}, {"poleWeight", command.poleWeight},
				{"maximumStretchScale", command.maximumStretchScale}, {"flags", static_cast<std::uint32_t>(command.flags)},
				{"targetTick", command.targetTick}, {"expectedPoseGeneration", command.expectedPoseGeneration},
				{"expectedPublicationGeneration", command.expectedPublicationGeneration},
				{"correlationId", command.correlationId}
			};
		}

		nlohmann::json SerializeProceduralResult(const NativeAnimationProceduralResultSnapshot& snapshot)
		{
			return {
				{"status", MachineNativeStatus(snapshot.status)}, {"solver", static_cast<std::uint32_t>(snapshot.solver)},
				{"state", static_cast<std::uint32_t>(snapshot.state)}, {"goalId", snapshot.goalId},
				{"targetJointStableId", snapshot.targetJointStableId},
				{"resultTransform", {{"translation", {snapshot.resultTransform.translation.x, snapshot.resultTransform.translation.y, snapshot.resultTransform.translation.z}},
					{"rotation", {snapshot.resultTransform.rotation.x, snapshot.resultTransform.rotation.y, snapshot.resultTransform.rotation.z, snapshot.resultTransform.rotation.w}},
					{"scale", {snapshot.resultTransform.scale.x, snapshot.resultTransform.scale.y, snapshot.resultTransform.scale.z}}}},
				{"targetErrorMeters", snapshot.targetErrorMeters}, {"targetErrorRadians", snapshot.targetErrorRadians},
				{"inputWeight", snapshot.inputWeight}, {"finalContribution", snapshot.finalContribution},
				{"iterationCount", snapshot.iterationCount}, {"iterationLimit", snapshot.iterationLimit},
				{"flags", static_cast<std::uint32_t>(snapshot.flags)}, {"poseGeneration", snapshot.poseGeneration},
				{"componentGeneration", snapshot.componentGeneration}, {"skeletonGeneration", snapshot.skeletonGeneration},
				{"programGeneration", snapshot.programGeneration}, {"observationTick", snapshot.observationTick},
				{"publicationGeneration", snapshot.publicationGeneration}, {"producerOrder", snapshot.producerOrder},
				{"correlationId", snapshot.correlationId}
			};
		}

		nlohmann::json SerializeRetargetSnapshot(const NativeAnimationRetargetSnapshot& snapshot)
		{
			return {
				{"status", MachineNativeStatus(snapshot.status)}, {"profileAssetId", ToHexString(snapshot.profileAssetId)},
				{"profileGeneration", snapshot.profileGeneration}, {"sourceSkeletonGeneration", snapshot.sourceSkeletonGeneration},
				{"targetSkeletonGeneration", snapshot.targetSkeletonGeneration}, {"poseGeneration", snapshot.poseGeneration},
				{"contactErrorMeters", snapshot.contactErrorMeters}, {"reachableIkErrorMeters", snapshot.reachableIkErrorMeters},
				{"observationTick", snapshot.observationTick}, {"publicationGeneration", snapshot.publicationGeneration}
			};
		}

		std::optional<NativeAnimationProceduralGoalCommand> BuildProceduralGoalCommand(
			const nlohmann::json& arguments, AnimatorInstanceHandle target, std::string& error)
		{
			NativeAnimationProceduralGoalCommand command{};
			command.header = MakeNativeHeader(NativeAnimationRecordKind::ProceduralGoalCommand, sizeof(command));
			command.animator = {target.slotIndex, target.generation};
			command.goalId = arguments.value("goalId", 0ULL);
			const std::optional<std::int64_t> targetTick = ReadNonNegativeTick(
				arguments.value("targetTick", nlohmann::json(0U)));
			if(!targetTick)
			{
				error = "targetTick is outside the supported fixed-tick range.";
				return std::nullopt;
			}
			command.targetTick = *targetTick;
			const std::string idempotencyKey = arguments.value("idempotencyKey", std::string{});
			const std::string correlationId = arguments.value("correlationId", std::string{});
			command.producerId = 0x4155544FU;
			command.producerSequence = StableIdentity(idempotencyKey + "#procedural-sequence");
			command.correlationId = StableIdentity(correlationId + "#procedural-correlation");
			command.idempotencyKey = StableIdentity(idempotencyKey + "#procedural-idempotency");
			command.cancellationToken = StableIdentity(idempotencyKey + "#procedural-cancellation");
			if(arguments.value("action", std::string{}) == "clear")
			{
				command.command = NativeAnimationProceduralCommandKind::ClearGoal;
				return command;
			}
			const std::optional<NativeAnimationProceduralGoalKind> kind = ParseProceduralGoalKind(arguments.value("goalKind", std::string{}));
			const std::optional<NativeAnimationProceduralCoordinateSpace> space = ParseProceduralCoordinateSpace(arguments.value("coordinateSpace", std::string{}));
			const std::optional<AnimationTransform> transform = ReadTransform(arguments.value("goalTransform", nlohmann::json{}));
			const std::optional<AnimationFloat3> pole = ReadFloat3(arguments.value("poleVector", nlohmann::json{}));
			const std::optional<float> positionWeight = ReadFiniteFloat(arguments.value("positionWeight", nlohmann::json{}));
			const std::optional<float> rotationWeight = ReadFiniteFloat(arguments.value("rotationWeight", nlohmann::json{}));
			const std::optional<float> poleWeight = ReadFiniteFloat(arguments.value("poleWeight", nlohmann::json{}));
			const std::optional<float> maximumStretch = ReadFiniteFloat(arguments.value("maximumStretchScale", nlohmann::json{}));
			if(!kind || !space || !transform || !pole || !positionWeight || !rotationWeight || !poleWeight || !maximumStretch
				|| !arguments["priority"].is_number_integer() && !arguments["priority"].is_number_unsigned())
			{
				error = "The procedural goal payload did not contain a finite typed direct-transform target.";
				return std::nullopt;
			}
			const std::int64_t priority = arguments["priority"].get<std::int64_t>();
			if(priority < std::numeric_limits<std::int32_t>::min() || priority > std::numeric_limits<std::int32_t>::max())
			{
				error = "procedural priority is outside the signed 32-bit range.";
				return std::nullopt;
			}
			command.command = NativeAnimationProceduralCommandKind::SetGoal;
			command.goalKind = *kind;
			command.coordinateSpace = *space;
			command.layerIndex = arguments.value("layerIndex", 0U);
			command.targetJointStableId = arguments.value("targetJointStableId", 0ULL);
			command.goalTransform = ToNativeTransform(*transform);
			command.poleVector = {pole->x, pole->y, pole->z};
			command.priority = static_cast<std::int32_t>(priority);
			command.positionWeight = *positionWeight;
			command.rotationWeight = *rotationWeight;
			command.poleWeight = *poleWeight;
			command.maximumStretchScale = *maximumStretch;
			command.flags = static_cast<NativeAnimationProceduralGoalFlag>(arguments.value("flags", 0U));
			command.expectedPoseGeneration = arguments.value("expectedPoseGeneration", 0U);
			command.expectedPublicationGeneration = arguments.value("expectedPublicationGeneration", 0ULL);
			return command;
		}

		AutomationDispatchResult ExecuteProceduralGoal(
			const nlohmann::json& arguments, AnimationAutomationTransactions& transactions, std::uint64_t generation)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			AnimationSystem& system = AnimationSystem::Instance();
			if(!target) return Fail(ErrorCategory::MissingTarget, "target must select exactly one live animator handle or object UUID.");
			if(!system.IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			std::string error;
			const std::optional<NativeAnimationProceduralGoalCommand> command = BuildProceduralGoalCommand(arguments, *target, error);
			if(!command) return Fail(ErrorCategory::Validation, std::move(error));
			const bool dryRun = arguments.value("dryRun", false);
			AnimationAutomationMutation mutation{"animation.runtime.procedural.goal", arguments.value("idempotencyKey", std::string{}),
				arguments.value("correlationId", std::string{}), arguments.value("expectedGeneration", 0ULL), dryRun, arguments};
			return transactions.ExecuteWithCommit(mutation, generation, [&system, command = *command, dryRun]() {
				AutomationDispatchResult result;
				if(dryRun)
				{
					result.ok = true;
					result.message = "Validated the copied procedural goal without queueing it.";
					result.data = {{"status", "validated"}, {"acknowledgement", "none_dry_run"},
						{"goal", SerializeProceduralGoal(command)}, {"visibilityBoundary", "none_dry_run"}};
					return result;
				}
				const NativeAnimationProceduralGoalReceipt receipt = system.SubmitProceduralGoal(command);
				if(!IsAcceptedNativeStatus(receipt.status)) return Fail(MapNativeFailure(receipt.status),
					"The typed procedural goal was rejected before protected capture.");
				result.ok = true;
				result.message = "Queued the copied procedural goal for protected animation capture.";
				result.data = {{"status", MachineNativeStatus(receipt.status)}, {"acknowledgement", "queued_not_yet_published"},
					{"receipt", {{"target", SerializeRuntimeTarget({receipt.animator.slotIndex, receipt.animator.generation})},
						{"command", receipt.command == NativeAnimationProceduralCommandKind::SetGoal ? "set" : "clear"},
						{"goalId", receipt.goalId}, {"producerId", receipt.producerId}, {"producerSequence", receipt.producerSequence},
						{"correlationId", receipt.correlationId}, {"idempotencyKey", receipt.idempotencyKey},
						{"cancellationToken", receipt.cancellationToken}, {"observableTick", receipt.observableTick},
						{"publicationGeneration", receipt.publicationGeneration}, {"duplicate", receipt.duplicate != 0U}}},
					{"visibilityBoundary", "animation_protected_capture_then_publication"}};
				return result;
			});
		}

		AutomationDispatchResult ExecuteRetargetRequest(
			const nlohmann::json& arguments, AnimationAutomationTransactions& transactions, std::uint64_t generation)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			AnimationSystem& system = AnimationSystem::Instance();
			if(!target) return Fail(ErrorCategory::MissingTarget, "target must select exactly one live animator handle or object UUID.");
			if(!system.IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			const std::optional<NativeAnimationRetargetOperation> operation = ParseRetargetOperation(arguments.value("action", std::string{}));
			if(!operation) return Fail(ErrorCategory::Validation, "The retarget action is unsupported.");
			NativeAnimationRetargetRequest request{};
			request.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetRequest, sizeof(request));
			request.animator = {target->slotIndex, target->generation};
			request.operation = *operation;
			request.layerIndex = arguments.value("layerIndex", 0U);
			request.expectedPoseGeneration = arguments.value("expectedPoseGeneration", 0U);
			request.expectedPublicationGeneration = arguments.value("expectedPublicationGeneration", 0ULL);
			const std::optional<std::int64_t> targetTick = ReadNonNegativeTick(
				arguments.value("targetTick", nlohmann::json(0U)));
			if(!targetTick)
			{
				return Fail(ErrorCategory::Validation,
					"targetTick is outside the supported fixed-tick range.");
			}
			request.targetTick = *targetTick;
			request.correlationId = StableIdentity(arguments.value("correlationId", std::string{}) + "#retarget-correlation");
			if(*operation != NativeAnimationRetargetOperation::DeactivateProfile)
			{
				const std::optional<UUID> profileId = TerraEngine::ParseUUID(arguments.value("profileAssetId", std::string{}));
				if(!profileId) return Fail(ErrorCategory::Validation, "profileAssetId is not a UUID.");
				request.profileAssetId = ToNativeUuid(*profileId);
				request.profileGeneration = arguments.value("profileGeneration", 0U);
			}
			const bool dryRun = arguments.value("dryRun", false);
			AnimationAutomationMutation mutation{"animation.runtime.retarget.request", arguments.value("idempotencyKey", std::string{}),
				arguments.value("correlationId", std::string{}), arguments.value("expectedGeneration", 0ULL), dryRun, arguments};
			return transactions.ExecuteWithCommit(mutation, generation, [&system, request, dryRun]() {
				AutomationDispatchResult result;
				if(dryRun)
				{
					result.ok = true;
					result.message = "Validated the copied retarget request without queueing it.";
					result.data = {{"status", "validated"}, {"acknowledgement", "none_dry_run"},
						{"visibilityBoundary", "none_dry_run"}};
					return result;
				}
				const NativeAnimationRetargetReceipt receipt = system.SubmitRetargetRequest(request);
				if(!IsAcceptedNativeStatus(receipt.status)) return Fail(MapNativeFailure(receipt.status),
					"The typed retarget request was rejected before protected capture.");
				result.ok = true;
				result.message = "Queued the copied retarget request for protected animation capture.";
				result.data = {{"status", MachineNativeStatus(receipt.status)}, {"acknowledgement", "queued_not_yet_published"},
					{"receipt", {{"target", SerializeRuntimeTarget({receipt.animator.slotIndex, receipt.animator.generation})},
						{"operation", static_cast<std::uint32_t>(receipt.operation)}, {"profileGeneration", receipt.profileGeneration},
						{"correlationId", receipt.correlationId}, {"observableTick", receipt.observableTick},
						{"publicationGeneration", receipt.publicationGeneration}}},
					{"visibilityBoundary", "animation_protected_capture_then_publication"}};
				return result;
			});
		}

		AutomationDispatchResult ExecuteMotionMatchRequest(const nlohmann::json& arguments,
			AnimationAutomationTransactions& transactions, std::uint64_t generation)
		{
			// Convert the bounded JSON envelope into a value-only native record before
			// it crosses the animation system's protected queue boundary.
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target || !AnimationSystem::Instance().IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			const std::optional<NativeAnimationMotionMatchOperation> operation = ParseMotionMatchOperation(arguments.value("action", std::string{}));
			if(!operation) return Fail(ErrorCategory::Validation, "The motion-match action is unsupported.");
			NativeAnimationMotionMatchRequest request{};
			request.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchRequest, sizeof(request));
			request.animator = {target->slotIndex, target->generation}; request.operation = *operation;
			request.flags = arguments.value("flags", 0U); request.featureCount = arguments.value("featureCount", 48U);
			request.queryId = arguments.value("queryId", 0ULL); request.correlationId = StableIdentity(arguments.value("correlationId", std::string{}));
			const std::optional<std::int64_t> tick = ReadNonNegativeTick(arguments.value("targetTick", nlohmann::json(0U)));
			if(!tick) return Fail(ErrorCategory::Validation, "targetTick is outside the supported fixed-tick range.");
			request.targetTick = *tick;
			if(*operation != NativeAnimationMotionMatchOperation::Clear)
			{
				const std::optional<UUID> asset = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
				if(!asset || !arguments.contains("assetGeneration")) return Fail(ErrorCategory::Validation, "Motion database set/query requires assetId and assetGeneration.");
				request.databaseAssetId = ToNativeUuid(*asset); request.databaseGeneration = arguments.value("assetGeneration", 0U);
			}
			if(*operation == NativeAnimationMotionMatchOperation::Query && request.queryId == 0U) return Fail(ErrorCategory::Validation, "Motion-match query requires queryId.");
			const bool dryRun = arguments.value("dryRun", false);
			return transactions.ExecuteWithCommit({"animation.runtime.motion_match.request", arguments.value("idempotencyKey", std::string{}), arguments.value("correlationId", std::string{}), arguments.value("expectedGeneration", 0ULL), dryRun, arguments}, generation, [&request, dryRun]() {
				if(dryRun) { AutomationDispatchResult result; result.ok = true; result.data = {{"status", "validated"}, {"visibilityBoundary", "none_dry_run"}}; return result; }
				const NativeAnimationMotionMatchReceipt receipt = AnimationSystem::Instance().SubmitMotionMatchRequest(request);
				if(!IsAcceptedNativeStatus(receipt.status)) return Fail(MapNativeFailure(receipt.status), "The copied motion-match request was rejected before protected capture.");
				AutomationDispatchResult result; result.ok = true; result.data = {{"status", MachineNativeStatus(receipt.status)}, {"acknowledgement", "queued_not_yet_published"}, {"queryId", receipt.queryId}, {"visibilityBoundary", "animation_protected_capture_then_publication"}}; return result;
			});
		}

		AutomationDispatchResult ExecuteRagdollRequest(const nlohmann::json& arguments,
			AnimationAutomationTransactions& transactions, std::uint64_t generation)
		{
			// This bridge is deliberately limited to lifecycle/recovery requests. It
			// neither creates physics bodies nor exposes direct impact or motor calls.
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target || !AnimationSystem::Instance().IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			const std::optional<NativeAnimationRagdollOperation> operation = ParseRagdollOperation(arguments.value("action", std::string{}));
			if(!operation) return Fail(ErrorCategory::Validation, "The ragdoll action is unsupported.");
			const std::optional<UUID> asset = TerraEngine::ParseUUID(arguments.value("assetId", std::string{}));
			if(!asset || !arguments.contains("assetGeneration") || !arguments.contains("physicsWorldGeneration")) return Fail(ErrorCategory::Validation, "Ragdoll requests require assetId, assetGeneration, and physicsWorldGeneration.");
			const std::optional<float> blend = ReadFiniteFloat(arguments.value("blendWeight", nlohmann::json(1.0)));
			const std::optional<std::int64_t> tick = ReadNonNegativeTick(arguments.value("targetTick", nlohmann::json(0U)));
			if(!blend || !tick) return Fail(ErrorCategory::Validation, "Ragdoll blendWeight or targetTick is invalid.");
			NativeAnimationRagdollRequest request{}; request.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollRequest, sizeof(request));
			request.animator = {target->slotIndex, target->generation}; request.operation = *operation; request.flags = arguments.value("flags", 0U);
			request.profileAssetId = ToNativeUuid(*asset); request.profileGeneration = arguments.value("assetGeneration", 0U);
			request.physicsWorldGeneration = arguments.value("physicsWorldGeneration", 0U); request.blendWeight = *blend;
			request.correlationId = StableIdentity(arguments.value("correlationId", std::string{})); request.targetTick = *tick;
			const bool dryRun = arguments.value("dryRun", false);
			return transactions.ExecuteWithCommit({"animation.runtime.ragdoll.request", arguments.value("idempotencyKey", std::string{}), arguments.value("correlationId", std::string{}), arguments.value("expectedGeneration", 0ULL), dryRun, arguments}, generation, [&request, dryRun]() {
				if(dryRun) { AutomationDispatchResult result; result.ok = true; result.data = {{"status", "validated"}, {"visibilityBoundary", "none_dry_run"}}; return result; }
				const NativeAnimationRagdollReceipt receipt = AnimationSystem::Instance().SubmitRagdollRequest(request);
				if(!IsAcceptedNativeStatus(receipt.status)) return Fail(MapNativeFailure(receipt.status), "The copied ragdoll request was rejected before protected capture.");
				AutomationDispatchResult result; result.ok = true; result.data = {{"status", MachineNativeStatus(receipt.status)}, {"acknowledgement", "queued_not_yet_published"}, {"visibilityBoundary", "animation_protected_capture_then_publication"}}; return result;
			});
		}

		template<typename Value, typename Copy>
		AnimationStatus CopyBoundedSnapshots(
			Copy&& copy, AnimatorInstanceHandle target, std::vector<Value>& output)
		{
			std::uint32_t count = 0U;
			AnimationStatus status = copy(target, std::span<Value>{}, count);
			if(status == AnimationStatus::NotReady)
			{
				output.clear();
				return status;
			}
			if(status != AnimationStatus::Ok && status != AnimationStatus::BufferTooSmall) return status;
			try { output.resize(count); }
			catch(const std::bad_alloc&) { return AnimationStatus::HardLimitExceeded; }
			if(count == 0U) return AnimationStatus::Ok;
			return copy(target, std::span<Value>(output), count);
		}

		template<typename Value, typename Serialize>
		nlohmann::json SerializeBoundedTail(
			const std::vector<Value>& values, std::size_t maximumItems, Serialize&& serialize)
		{
			const std::size_t first = values.size() > maximumItems ? values.size() - maximumItems : 0U;
			nlohmann::json result = nlohmann::json::array();
			for(std::size_t index = first; index < values.size(); ++index) result.push_back(serialize(values[index]));
			return result;
		}

		AutomationDispatchResult ExecuteProceduralRead(const nlohmann::json& arguments)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			AnimationSystem& system = AnimationSystem::Instance();
			if(!target) return Fail(ErrorCategory::MissingTarget, "target must select exactly one live animator handle or object UUID.");
			if(!system.IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			const std::size_t maximumItems = arguments.value("maximumItems", static_cast<std::uint64_t>(64U));
			std::vector<NativeAnimationProceduralGoalCommand> goals;
			const AnimationStatus goalStatus = CopyBoundedSnapshots<NativeAnimationProceduralGoalCommand>(
				[&system](AnimatorInstanceHandle handle, std::span<NativeAnimationProceduralGoalCommand> destination, std::uint32_t& count) {
					return system.CopyProceduralGoals(handle, destination, count); }, *target, goals);
			if(goalStatus != AnimationStatus::Ok && goalStatus != AnimationStatus::NotReady)
				return Fail(goalStatus == AnimationStatus::HardLimitExceeded ? ErrorCategory::OverBudget : ErrorCategory::Internal,
					"Could not copy the protected procedural goal publication.");
			std::vector<NativeAnimationProceduralResultSnapshot> results;
			AnimationStatus resultStatus = AnimationStatus::NotReady;
			if(arguments.value("includeResults", true))
			{
				resultStatus = CopyBoundedSnapshots<NativeAnimationProceduralResultSnapshot>(
					[&system](AnimatorInstanceHandle handle, std::span<NativeAnimationProceduralResultSnapshot> destination, std::uint32_t& count) {
						return system.CopyProceduralResults(handle, destination, count); }, *target, results);
				if(resultStatus != AnimationStatus::Ok && resultStatus != AnimationStatus::NotReady)
					return Fail(resultStatus == AnimationStatus::HardLimitExceeded ? ErrorCategory::OverBudget : ErrorCategory::Internal,
						"Could not copy the protected procedural result publication.");
			}
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Read copied procedural goal and result publications.";
			result.data = {{"target", SerializeRuntimeTarget(*target)}, {"currentPublication", SerializePublishedGenerations(*target)},
				{"activeGoals", SerializeBoundedTail(goals, maximumItems, SerializeProceduralGoal)},
				{"activeGoalCount", goals.size()}, {"activeGoalsTruncated", goals.size() > maximumItems},
				{"results", SerializeBoundedTail(results, maximumItems, SerializeProceduralResult)},
				{"resultCount", results.size()}, {"resultsTruncated", results.size() > maximumItems},
				{"visibilityBoundary", "immutable_procedural_store_copies"}};
			// A direct latest record lets bounded automation assert the actual solver
			// outcome without inventing array-index syntax in its object-only bindings.
			if(!results.empty()) result.data["latestResult"] = SerializeProceduralResult(results.back());
			return result;
		}

		AutomationDispatchResult ExecuteRetargetRead(const nlohmann::json& arguments)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			AnimationSystem& system = AnimationSystem::Instance();
			if(!target) return Fail(ErrorCategory::MissingTarget, "target must select exactly one live animator handle or object UUID.");
			if(!system.IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			const std::size_t maximumItems = arguments.value("maximumItems", static_cast<std::uint64_t>(64U));
			std::vector<NativeAnimationRetargetSnapshot> snapshots;
			const AnimationStatus status = CopyBoundedSnapshots<NativeAnimationRetargetSnapshot>(
				[&system](AnimatorInstanceHandle handle, std::span<NativeAnimationRetargetSnapshot> destination, std::uint32_t& count) {
					return system.CopyRetargetSnapshots(handle, destination, count); }, *target, snapshots);
			if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady)
				return Fail(status == AnimationStatus::HardLimitExceeded ? ErrorCategory::OverBudget : ErrorCategory::Internal,
					"Could not copy the protected retarget publication.");
			AutomationDispatchResult result;
			result.ok = true;
			result.message = "Read copied retarget publications.";
			result.data = {{"target", SerializeRuntimeTarget(*target)}, {"currentPublication", SerializePublishedGenerations(*target)},
				{"snapshots", SerializeBoundedTail(snapshots, maximumItems, SerializeRetargetSnapshot)},
				{"snapshotCount", snapshots.size()}, {"snapshotsTruncated", snapshots.size() > maximumItems},
				{"visibilityBoundary", "immutable_retarget_store_copies"}};
			// Keep the complete bounded history while also exposing its newest copied
			// status for fail-closed live validation and reconnecting MCP clients.
			if(!snapshots.empty()) result.data["latestSnapshot"] = SerializeRetargetSnapshot(snapshots.back());
			return result;
		}

		AutomationDispatchResult ExecuteMotionMatchRead(const nlohmann::json& arguments)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target || !AnimationSystem::Instance().IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			std::vector<NativeAnimationMotionMatchSnapshot> snapshots;
			const AnimationStatus status = CopyBoundedSnapshots<NativeAnimationMotionMatchSnapshot>([](AnimatorInstanceHandle handle, std::span<NativeAnimationMotionMatchSnapshot> output, std::uint32_t& count) { return AnimationSystem::Instance().CopyMotionMatchSnapshots(handle, output, count); }, *target, snapshots);
			if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady) return Fail(
				status == AnimationStatus::HardLimitExceeded ? ErrorCategory::OverBudget : ErrorCategory::Internal,
				"Could not copy motion-match publications.");
			const std::size_t maximum = arguments.value("maximumItems", static_cast<std::uint64_t>(64U)); nlohmann::json values = nlohmann::json::array();
			for(std::size_t index = snapshots.size() > maximum ? snapshots.size() - maximum : 0U; index < snapshots.size(); ++index) { const auto& value = snapshots[index]; values.push_back({{"status", MachineNativeStatus(value.status)}, {"queryId", value.queryId}, {"selectedSampleId", value.selectedSampleId}, {"totalScore", value.totalScore}, {"trajectoryScore", value.trajectoryScore}, {"poseScore", value.poseScore}, {"usedFallback", value.usedFallback != 0U}, {"publicationGeneration", value.publicationGeneration}}); }
			const nlohmann::json latest = values.empty() ? nlohmann::json(nullptr) : values.back();
			AutomationDispatchResult result; result.ok = true; result.data = {{"snapshots", std::move(values)}, {"snapshotCount", snapshots.size()}, {"truncated", snapshots.size() > maximum}, {"latestSnapshot", latest}, {"visibilityBoundary", "immutable_motion_match_store_copies"}}; return result;
		}

		AutomationDispatchResult ExecuteRagdollRead(const nlohmann::json& arguments)
		{
			const std::optional<AnimatorInstanceHandle> target = ResolveTarget(arguments["target"]);
			if(!target || !AnimationSystem::Instance().IsAnimatorInstanceAlive(*target)) return Fail(ErrorCategory::StaleTarget, "The selected animator generation is no longer alive.");
			std::vector<NativeAnimationRagdollSnapshot> snapshots;
			const AnimationStatus status = CopyBoundedSnapshots<NativeAnimationRagdollSnapshot>([](AnimatorInstanceHandle handle, std::span<NativeAnimationRagdollSnapshot> output, std::uint32_t& count) { return AnimationSystem::Instance().CopyRagdollSnapshots(handle, output, count); }, *target, snapshots);
			if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady) return Fail(
				status == AnimationStatus::HardLimitExceeded ? ErrorCategory::OverBudget : ErrorCategory::Internal,
				"Could not copy ragdoll publications.");
			const std::size_t maximum = arguments.value("maximumItems", static_cast<std::uint64_t>(64U)); nlohmann::json values = nlohmann::json::array();
			for(std::size_t index = snapshots.size() > maximum ? snapshots.size() - maximum : 0U; index < snapshots.size(); ++index) { const auto& value = snapshots[index]; values.push_back({{"status", MachineNativeStatus(value.status)}, {"profileGeneration", value.profileGeneration}, {"physicsWorldGeneration", value.physicsWorldGeneration}, {"lifecycle", value.lifecycle}, {"bodyCount", value.bodyCount}, {"blendWeight", value.blendWeight}, {"recoveryBlocked", value.recoveryBlocked != 0U}, {"publicationGeneration", value.publicationGeneration}}); }
			const nlohmann::json latest = values.empty() ? nlohmann::json(nullptr) : values.back();
			AutomationDispatchResult result; result.ok = true; result.data = {{"snapshots", std::move(values)}, {"snapshotCount", snapshots.size()}, {"truncated", snapshots.size() > maximum}, {"latestSnapshot", latest}, {"visibilityBoundary", "immutable_ragdoll_store_copies"}}; return result;
		}
	}

	AutomationDispatchResult ExecuteAnimationRuntimeOperation(
		std::string_view operation,
		const nlohmann::json& arguments,
		AnimationAutomationTransactions& transactions,
		std::uint64_t generation,
		const std::function<AutomationDispatchResult()>& protectedCommit,
		AnimationAutomationJobs* recordingJobs,
		const std::filesystem::path& recordingAssetRoot,
		AnimationAutomationDetailedDiagnosticsLease* detailedDiagnosticsLease)
	{
		AutomationDispatchResult valid = ValidateAnimationAutomationRequest(operation, arguments);
		if(!valid.ok)
		{
			return valid;
		}
		if(operation == "animation.camera.capture")
		{
			if(!arguments.value("rendererAvailable", false))
			{
				return Fail(ErrorCategory::Unavailable,
					"Animation camera capture is unavailable because the renderer is unavailable.");
			}
			valid.data["captureQueued"] = true;
			return valid;
		}
		if(operation == "animation.runtime.diagnostics")
		{
			if(arguments.value("includeDetails", false)) {
				// The service-owned lease survives this RPC and therefore gives the
				// next protected animation frame a chance to publish copied rows.
				if(detailedDiagnosticsLease == nullptr
					|| !detailedDiagnosticsLease->EnsureRequested()) {
					return Fail(ErrorCategory::Unavailable,
						"Detailed animation diagnostics could not acquire the bounded automation collection request.");
				}
				const std::optional<AnimationDetailedDiagnosticsSnapshot> detailed =
					AcquireAnimationDetailedDiagnosticsSnapshot();
				valid.data["detailsPendingPublication"] = !detailed.has_value();
				if(detailed) {
					valid.data["detailedFrameIndex"] = detailed->frameIndex;
					valid.data["detailedAnimatorCount"] = detailed->animators.size();
				}
			}
			valid.data["rendererAvailable"] = arguments.value("rendererAvailable", false);
			return valid;
		}
		if(operation == "animation.runtime.residency" || operation == "animation.runtime.profiler")
		{
			// Compact reads use only the immutable summary. An explicit detail request
			// promotes the service-owned lease until a later protected frame can copy
			// rows; this synchronous RPC never manufactures those rows itself.
			const std::optional<AnimationDiagnosticsSnapshot> snapshot =
				AcquireAnimationDiagnosticsSnapshot();
			std::optional<AnimationDetailedDiagnosticsSnapshot> detailed;
			if(arguments.value("includeDetails", false)) {
				if(detailedDiagnosticsLease == nullptr
					|| !detailedDiagnosticsLease->EnsureRequested()) {
					return Fail(ErrorCategory::Unavailable,
						"Detailed animation diagnostics could not acquire the bounded automation collection request.");
				}
				detailed = AcquireAnimationDetailedDiagnosticsSnapshot();
			}
			valid.data["summaryAvailable"] = snapshot.has_value();
			valid.data["collectionEnabled"] = snapshot.has_value()
				&& snapshot->detailLevel != AnimationDiagnosticsDetailLevel::Disabled;
			valid.data["detailedCollectionEnabled"] = snapshot.has_value()
				&& snapshot->detailLevel == AnimationDiagnosticsDetailLevel::Detailed;
			valid.data["collectionPromotedByRequest"] = arguments.value("includeDetails", false);
			valid.data["detailsRequested"] = arguments.value("includeDetails", false);
			valid.data["detailsAvailable"] = detailed.has_value();
			valid.data["detailsPendingPublication"] = arguments.value("includeDetails", false)
				&& !detailed.has_value();
			if(snapshot)
			{
				valid.data["frameIndex"] = snapshot->frameIndex;
				valid.data["dormant"] = snapshot->dormant;
				if(operation == "animation.runtime.residency")
				{
					valid.data["residency"] = {
						{"requests", snapshot->counters.residencyRequests},
						{"hits", snapshot->counters.residencyHits},
						{"misses", snapshot->counters.residencyMisses},
						{"gpuTransferBytes", snapshot->counters.gpuTransferBytes},
						{"gpuReadbackBytes", snapshot->counters.gpuReadbackBytes}
					};
				}
				else
				{
					valid.data["profiler"] = {
						{"detailLevel", static_cast<std::uint32_t>(snapshot->detailLevel)},
						{"workerJobs", snapshot->counters.workerJobs},
						{"gpuDispatches", snapshot->counters.gpuDispatches},
						{"detailedRecords", snapshot->counters.detailedRecords}
					};
				}
			}
			if(detailed) {
				valid.data["details"] = {
					{"frameIndex", detailed->frameIndex},
					{"animatorCount", detailed->animators.size()},
					{"modeSwitchHistoryCount", detailed->modeSwitchHistory.size()}
				};
			}
			return valid;
		}
		if(operation == "animation.runtime.parameters")
		{
			return ExecuteParameterBatch(arguments, transactions, generation);
		}
		if(operation == "animation.runtime.procedural.goal")
		{
			return ExecuteProceduralGoal(arguments, transactions, generation);
		}
		if(operation == "animation.runtime.procedural.read")
		{
			return ExecuteProceduralRead(arguments);
		}
		if(operation == "animation.runtime.retarget.request")
		{
			return ExecuteRetargetRequest(arguments, transactions, generation);
		}
		if(operation == "animation.runtime.retarget.read")
		{
			return ExecuteRetargetRead(arguments);
		}
		if(operation == "animation.runtime.motion_match.request") return ExecuteMotionMatchRequest(arguments, transactions, generation);
		if(operation == "animation.runtime.motion_match.read") return ExecuteMotionMatchRead(arguments);
		if(operation == "animation.runtime.ragdoll.request") return ExecuteRagdollRequest(arguments, transactions, generation);
		if(operation == "animation.runtime.ragdoll.read") return ExecuteRagdollRead(arguments);
		if(operation == "animation.recording.start")
		{
			if(recordingJobs == nullptr) {
				return Fail(ErrorCategory::Unavailable,
					"Recording start requires the editor-owned durable animation job store.");
			}
			return StartRecording(arguments, *recordingJobs, recordingAssetRoot);
		}
		if(operation == "animation.recording.stop")
		{
			if(recordingJobs == nullptr) return Fail(ErrorCategory::Unavailable,
				"Recording stop requires the editor-owned durable animation job store.");
			return transactions.ExecuteWithCommit({"animation.recording.stop",
				arguments.value("idempotencyKey", std::string{}), arguments.value("correlationId", std::string{}),
				arguments.value("expectedGeneration", 0ULL), false, arguments}, generation,
				[recordingJobs, jobId = arguments.value("jobId", std::string{})]() {
					return recordingJobs->StopRecording(jobId);
				});
		}
		if(operation == "animation.motion_database.build")
		{
			if(recordingJobs == nullptr) return Fail(ErrorCategory::Unavailable,
				"Motion database builds require the editor-owned durable animation job store.");
			return StartMotionDatabaseBuild(arguments, transactions, generation,
				*recordingJobs, recordingAssetRoot);
		}
		if(operation == "animation.preview.play")
		{
			if(!protectedCommit)
			{
				return Fail(ErrorCategory::Unavailable,
					"Preview playback requires the editor-owned preview protected boundary.");
			}
			return transactions.ExecuteWithCommit({
				std::string(operation),
				arguments.value("idempotencyKey", std::string{}),
				arguments.value("correlationId", std::string{}),
				arguments.value("expectedGeneration", 0ULL),
				arguments.value("dryRun", false),
				arguments}, generation, protectedCommit);
		}
		return Fail(ErrorCategory::UnsupportedCapability,
			"The runtime animation operation has no typed execution adapter.");
	}

	AutomationDispatchResult CaptureAnimationRecordingSnapshot(
		Animation::AnimationRecorder& recorder,
		const Animation::AnimationRecorderSnapshotBinding& binding)
	{
		const std::shared_ptr<const AnimationSnapshot> snapshot =
			AnimationSystem::Instance().LoadAnimatorSnapshot(binding.animator);
		if(!snapshot)
		{
			return Fail(ErrorCategory::Unavailable,
				"The selected animator has no immutable published snapshot to record yet.");
		}
		const Animation::AnimationAuthoringStatus status =
			recorder.CapturePublishedSnapshot(*snapshot, binding);
		AutomationDispatchResult result;
		result.ok = status == Animation::AnimationAuthoringStatus::Ok;
		if(!result.ok)
		{
			result.error = status == Animation::AnimationAuthoringStatus::Stale
				? ErrorCategory::StaleTarget : ErrorCategory::Validation;
			result.message = "The copied published animation snapshot was rejected by the recording session.";
			return result;
		}
		const Animation::AnimationRecorderProgress progress = recorder.Progress();
		result.message = "Copied one immutable AnimationSystem publication into the recording session.";
		result.data = {
			{"capturedSamples", progress.capturedSamples},
			{"capturedEvents", progress.capturedEvents},
			{"progress", progress.overallNormalized},
			{"visibilityBoundary", "animation_snapshot_publication_then_recording_capture"}
		};
		return result;
	}
}
