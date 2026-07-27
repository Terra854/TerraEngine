// AnimationSystem.cpp - protected/worker animation authority workload shell.

#include "AnimationSystem.h"

#include "Animation/Diagnostics/AnimationDiagnostics.h"
#include "Animation/Determinism/AnimationHistory.h"
#include "Animation/Determinism/AnimationReplay.h"
#include "Animation/Evaluation/CpuClipEvaluator.h"
#include "Animation/Evaluation/PoseBlend.h"
#include "Animation/AnimationPresentationSystem.h"
#include "Animation/Assets/MotionDatabaseAsset.h"
#include "Animation/Assets/RagdollProfileAsset.h"
#include "Animation/Assets/RetargetProfileAsset.h"
#include "Animation/Ragdoll/PhysicsPoseSnapshot.h"
#include "Animation/Ragdoll/RagdollActivation.h"
#include "Animation/Ragdoll/RagdollRecovery.h"
#include "Animation/Ragdoll/RagdollState.h"
#include "Animation/Graph/StateMachineEvaluator.h"
#include "Animation/Graph/ProceduralNodeEvaluator.h"
#include "Animation/Runtime/AnimationEventStream.h"
#include "Animation/Runtime/AnimationConsumerRegistry.h"
#include "Animation/Runtime/AnimationAttachments.h"
#include "Animation/Runtime/AnimatorComponentBindingCoordinator.h"
#include "Animation/Runtime/RootMotion.h"
#include "Animation/Ragdoll/RagdollBlend.h"
#include "Animation/Retargeting/RetargetEvaluator.h"
#include "Animation/Retargeting/RetargetValidator.h"
#include "Animation/MotionMatching/MotionMatchCpu.h"
#include "Components/AnimatorComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Profiling/Profiler.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::size_t kInlineAnimatorThreshold = 16U;
		constexpr std::size_t kAnimatorsPerWorkerPartition = 64U;
		constexpr std::size_t kMaximumPendingRootMotionApplications = 4096U;
		constexpr std::size_t kMaximumAnimationScratchBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationPoseBytes = 64U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationMemoryBytes = 128U * 1024U * 1024U;
		constexpr std::size_t kMaximumAnimationMemoryReservations = 4096U;
		constexpr std::uint32_t kMotionMatchFeatureCountV1 = 48U;

		// A scheduled item owns exactly one of these records in the frame scratch
		// allocation. Planning and evaluation phases transfer only copied terminal
		// state through the record; no worker receives another item's byte interval.
		struct AnimationFrameScratchRecord {
			std::uint64_t sourceGeneration = 0U;
			std::size_t scheduledItemIndex = 0U;
			AnimationStatus evaluationStatus = AnimationStatus::NotReady;
			bool evaluated = false;
		};

		AnimationMemoryConfiguration MakeAnimationMemoryConfiguration() noexcept
		{
			AnimationMemoryConfiguration configuration;
			configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::ScratchPool)].maximumBytes =
				kMaximumAnimationScratchBytes;
			configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::FrameArena)].maximumBytes =
				kMaximumAnimationScratchBytes;
			configuration.classes[static_cast<std::size_t>(AnimationMemoryClass::PosePool)].maximumBytes =
				kMaximumAnimationPoseBytes;
			configuration.maximumTotalBytes = kMaximumAnimationMemoryBytes;
			configuration.maximumReservations = kMaximumAnimationMemoryReservations;
			return configuration;
		}

		NativeAnimationAbiHeader MakeNativeHeader(
			NativeAnimationRecordKind kind,
			std::size_t size) noexcept
		{
			return {static_cast<std::uint32_t>(size), kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(kind)};
		}

		NativeAnimationStatus ToNativeStatus(AnimationStatus status) noexcept
		{
			return static_cast<NativeAnimationStatus>(status);
		}

		NativeAnimationUuid ToNativeUuid(const UUID& value) noexcept
		{
			NativeAnimationUuid result{};
			std::memcpy(&result.first, value.data(), sizeof(result.first));
			std::memcpy(&result.second, value.data() + sizeof(result.first), sizeof(result.second));
			return result;
		}

		bool MatchesNativeUuid(NativeAnimationUuid value, const UUID& expected) noexcept
		{
			const NativeAnimationUuid nativeExpected = ToNativeUuid(expected);
			return value.first == nativeExpected.first && value.second == nativeExpected.second;
		}

		AnimationTransform FromNativeTransform(
			const NativeAnimationTransform& value) noexcept
		{
			AnimationTransform result;
			result.translation = {
				value.translation.x, value.translation.y, value.translation.z};
			result.rotation = {
				value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w};
			result.scale = {value.scale.x, value.scale.y, value.scale.z};
			return result;
		}

		NativeAnimationTransform ToNativeTransform(
			const AnimationTransform& value) noexcept
		{
			NativeAnimationTransform result{};
			result.translation = {
				value.translation.x, value.translation.y, value.translation.z};
			result.rotation = {
				value.rotation.x, value.rotation.y, value.rotation.z, value.rotation.w};
			result.scale = {value.scale.x, value.scale.y, value.scale.z};
			return result;
		}

		std::uint64_t MixDeterministicChecksum(
			std::uint64_t checksum,
			std::uint64_t value) noexcept
		{
			for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
				checksum ^= (value >> (byte * 8U)) & 0xffU;
				checksum *= 1099511628211ULL;
			}
			return checksum;
		}

		std::uint64_t QuantizedFloat(float value) noexcept
		{
			if(!std::isfinite(value)) return 0U;
			const double scaled = std::round(static_cast<double>(value) * 65536.0);
			const double bounded = std::clamp(scaled,
				static_cast<double>(std::numeric_limits<std::int32_t>::min()),
				static_cast<double>(std::numeric_limits<std::int32_t>::max()));
			return static_cast<std::uint32_t>(static_cast<std::int32_t>(bounded));
		}

		std::uint64_t HashDeterministicTransform(
			std::uint64_t checksum,
			const AnimationTransform& transform) noexcept
		{
			const std::array<float, 10U> values{
				transform.translation.x, transform.translation.y, transform.translation.z,
				transform.rotation.x, transform.rotation.y, transform.rotation.z,
				transform.rotation.w, transform.scale.x, transform.scale.y,
				transform.scale.z};
			for(const float value : values) {
				checksum = MixDeterministicChecksum(checksum, QuantizedFloat(value));
			}
			return checksum;
		}

		std::uint64_t HashAnimationGuid(
			std::uint64_t checksum,
			const AnimationGuid& value) noexcept
		{
			for(const std::uint8_t byte : value.value) {
				checksum ^= byte;
				checksum *= 1099511628211ULL;
			}
			return checksum;
		}

		AnimationStatus ComputeClipDeterministicContentFingerprint(
			const AnimationClipAsset& clip,
			std::uint64_t& output) noexcept
		{
			try {
				std::vector<std::uint8_t> bytes;
				const AnimationStatus status = SerializeAnimationClipAsset(clip, bytes);
				if(status != AnimationStatus::Ok) return status;
				std::uint64_t hash = 1469598103934665603ULL;
				for(const std::uint8_t byte : bytes) {
					hash ^= byte;
					hash *= 1099511628211ULL;
				}
				// FNV cannot produce zero for the valid non-empty animation format in
				// practice, but keep the public fingerprint contract fail-closed.
				if(hash == 0U) return AnimationStatus::ContentHashMismatch;
				output = hash;
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		AnimationWorkSchedulerConfiguration MakeAnimationWorkSchedulerConfiguration() noexcept
		{
			AnimationWorkSchedulerConfiguration configuration;
			configuration.inlineItemThreshold = kInlineAnimatorThreshold;
			configuration.targetItemsPerRange = kAnimatorsPerWorkerPartition;
			configuration.maximumRangeCount = 65536U;
			configuration.maximumItemCount = 65536U;
			configuration.maximumScratchBytes = kMaximumAnimationScratchBytes;
			configuration.scratchAlignment = alignof(std::uint64_t);
			return configuration;
		}

		AnimationFallbackReason FallbackReasonForStatus(AnimationStatus status) noexcept
		{
			switch(status) {
			case AnimationStatus::MissingAsset: return AnimationFallbackReason::MissingAsset;
			case AnimationStatus::IncompatibleAsset: return AnimationFallbackReason::IncompatibleAsset;
			case AnimationStatus::Cancelled: return AnimationFallbackReason::Cancelled;
			case AnimationStatus::StaleHandle: return AnimationFallbackReason::StaleGeneration;
			case AnimationStatus::HardLimitExceeded:
			case AnimationStatus::RangeOverflow: return AnimationFallbackReason::BudgetPressure;
			case AnimationStatus::InvalidArgument:
			case AnimationStatus::Rejected: return AnimationFallbackReason::ValidationFailure;
			default: return AnimationFallbackReason::InternalFailure;
			}
		}

		void ConfigureProtectedWorkload(
			TerraEngine::Scheduling::FrameWorkloadDeclaration& workload,
			std::string name,
			TerraEngine::Scheduling::WorkerSystemPhaseKind phaseKind,
			std::vector<std::string> reads,
			std::vector<std::string> writes,
			std::string protectedReason)
		{
			using namespace TerraEngine::Scheduling;
			workload.workloadName = std::move(name);
			workload.ownerSystem = "AnimationSystem";
			workload.phase = "Animation";
			workload.stableDescriptorId = "AnimationSystem/Animation/" + workload.workloadName;
			workload.activationPredicateKey = "animation-active-membership";
			workload.reads = std::move(reads);
			workload.writes = std::move(writes);
			workload.workerEligible = false;
			workload.mainThreadRequired = true;
			workload.mutationPolicy = FrameWorkloadMutationPolicy::DeferredOnly;
			workload.phaseKind = phaseKind;
			workload.boundaryClassification = HardBoundaryClassification::Protected;
			workload.hardBoundary = true;
			workload.protectedCommit = protectedReason;
			workload.protectedReason = std::move(protectedReason);
			workload.profilingLabel = "Animation." + workload.workloadName;
		}

		void ConfigureWorkerWorkload(
			TerraEngine::Scheduling::FrameWorkloadDeclaration& workload,
			std::string name,
			std::vector<std::string> reads,
			std::vector<std::string> writes,
			std::string safeSubwork)
		{
			using namespace TerraEngine::Scheduling;
			workload.workloadName = std::move(name);
			workload.ownerSystem = "AnimationSystem";
			workload.phase = "Animation";
			workload.stableDescriptorId = "AnimationSystem/Animation/" + workload.workloadName;
			workload.activationPredicateKey = "animation-active-membership";
			workload.reads = std::move(reads);
			workload.writes = std::move(writes);
			workload.inlineWorkItemThreshold = kInlineAnimatorThreshold;
			workload.partitionPolicy = FrameWorkloadPartitionPolicy::DeterministicRanges;
			workload.partitionWorkItemCount = kAnimatorsPerWorkerPartition;
			workload.workerEligible = true;
			workload.mainThreadRequired = false;
			workload.mutationPolicy = FrameWorkloadMutationPolicy::None;
			workload.phaseKind = WorkerSystemPhaseKind::WorkerPrepare;
			workload.boundaryClassification = HardBoundaryClassification::WorkerSafe;
			workload.safeSubwork = std::move(safeSubwork);
			workload.profilingLabel = "Animation." + workload.workloadName;
		}

		AnimationStatus ApplyPlaybackCommand(
			AnimatorPlayback& playback,
			const AnimationCommand& command) noexcept
		{
			switch(command.kind) {
			case AnimationCommandKind::Play: return playback.Play();
			case AnimationCommandKind::Stop: return playback.Stop();
			case AnimationCommandKind::Pause: return playback.Pause();
			case AnimationCommandKind::Resume: return playback.Resume();
			case AnimationCommandKind::Seek: return playback.Seek(command.playbackTick);
			case AnimationCommandKind::SetLooping: return playback.SetLooping(command.playbackFlag);
			case AnimationCommandKind::SetReverse: return playback.SetReverse(command.playbackFlag);
			case AnimationCommandKind::SetSpeed: return playback.SetSpeed(command.playbackSpeed);
			case AnimationCommandKind::CrossFade:
			case AnimationCommandKind::SetLayerWeight:
				// Controller commands need the immutable program and layer table, so the
				// planning loop applies them after ordinary state-machine advancement.
				return AnimationStatus::InvalidType;
			case AnimationCommandKind::SetParameter:
			case AnimationCommandKind::FireTrigger:
				// Controller parameters are captured now but become meaningful when the
				// immutable controller evaluator lands in the next user-story phase.
				return AnimationStatus::Ok;
			default: return AnimationStatus::InvalidArgument;
			}
		}

		AnimationStatus ApplyParameterCommand(
			std::vector<AnimationParameterSnapshot>& parameters,
			const AnimationCommand& command)
		{
			if(command.kind != AnimationCommandKind::SetParameter
				&& command.kind != AnimationCommandKind::FireTrigger) return AnimationStatus::InvalidType;
			const auto found = std::find_if(parameters.begin(), parameters.end(), [&command](const AnimationParameterSnapshot& value) {
				return value.parameterId == command.parameterId;
			});
			AnimationParameterSnapshot next;
			next.parameterId = command.parameterId;
			next.type = command.kind == AnimationCommandKind::FireTrigger
				? AnimationParameterType::Trigger
				: command.parameterType;
			next.value = command.value;
			if(found == parameters.end()) parameters.push_back(std::move(next));
			else *found = std::move(next);
			return AnimationStatus::Ok;
		}

		std::uint32_t StableParameterId(std::string_view name) noexcept
		{
			std::uint32_t numeric = 0U;
			const auto numericResult = std::from_chars(name.data(), name.data() + name.size(), numeric);
			if(numericResult.ec == std::errc{} && numericResult.ptr == name.data() + name.size()
				&& numeric != 0U) return numeric;
			// FNV-1a matches the engine's other stable copied fingerprints and does
			// not depend on the process-specific std::hash implementation.
			std::uint32_t hash = 2166136261U;
			for(const unsigned char byte : name) {
				hash ^= byte;
				hash *= 16777619U;
			}
			return hash == 0U ? 1U : hash;
		}

		AnimationStatus ParseInitialParameters(
			std::string_view payload,
			std::vector<AnimationParameterSnapshot>& output)
		{
			try {
				const nlohmann::json document = nlohmann::json::parse(payload.empty() ? "{}" : payload);
				if(!document.is_object()) return AnimationStatus::InvalidType;
				if(ValidateAnimationHardLimit(AnimationLimit::ControllerParameters, document.size())
					!= AnimationStatus::Ok) return AnimationStatus::HardLimitExceeded;
				std::vector<AnimationParameterSnapshot> staged;
				staged.reserve(document.size());
				for(const auto& [name, authored] : document.items()) {
					AnimationParameterSnapshot parameter;
					parameter.parameterId = StableParameterId(name);
					const nlohmann::json* value = &authored;
					std::string explicitType;
					if(authored.is_object() && authored.contains("type") && authored.contains("value")
						&& authored["type"].is_string()) {
						explicitType = authored["type"].get<std::string>();
						value = &authored["value"];
					}
					if(value->is_boolean() && (explicitType.empty() || explicitType == "Boolean")) {
						parameter.type = AnimationParameterType::Boolean;
						parameter.value = value->get<bool>();
					} else if(value->is_number_integer() && (explicitType.empty() || explicitType == "Integer")) {
						const std::int64_t integer = value->get<std::int64_t>();
						if(integer < std::numeric_limits<std::int32_t>::min()
							|| integer > std::numeric_limits<std::int32_t>::max()) return AnimationStatus::RangeOverflow;
						parameter.type = AnimationParameterType::Integer;
						parameter.value = static_cast<std::int32_t>(integer);
					} else if(value->is_number() && (explicitType.empty() || explicitType == "Float")) {
						const float scalar = value->get<float>();
						if(!std::isfinite(scalar)) return AnimationStatus::InvalidArgument;
						parameter.type = AnimationParameterType::Float;
						parameter.value = scalar;
					} else if(value->is_array() && value->size() == 3U
						&& (explicitType.empty() || explicitType == "Vector3")) {
						AnimationFloat3 vector{(*value)[0].get<float>(), (*value)[1].get<float>(), (*value)[2].get<float>()};
						if(!IsFinite(vector)) return AnimationStatus::InvalidArgument;
						parameter.type = AnimationParameterType::Vector3;
						parameter.value = vector;
					} else if(value->is_array() && value->size() == 4U && explicitType == "Quaternion") {
						AnimationQuaternion rotation{(*value)[0].get<float>(), (*value)[1].get<float>(),
							(*value)[2].get<float>(), (*value)[3].get<float>()};
						if(!IsFinite(rotation)) return AnimationStatus::InvalidArgument;
						parameter.type = AnimationParameterType::Quaternion;
						parameter.value = rotation;
					} else if(value->is_object() && explicitType == "Transform"
						&& value->contains("translation") && value->contains("rotation") && value->contains("scale")) {
						const nlohmann::json& translation = (*value)["translation"];
						const nlohmann::json& rotation = (*value)["rotation"];
						const nlohmann::json& scale = (*value)["scale"];
						if(!translation.is_array() || translation.size() != 3U
							|| !rotation.is_array() || rotation.size() != 4U
							|| !scale.is_array() || scale.size() != 3U) return AnimationStatus::InvalidType;
						AnimationTransform transform;
						transform.translation = {translation[0].get<float>(), translation[1].get<float>(), translation[2].get<float>()};
						transform.rotation = {rotation[0].get<float>(), rotation[1].get<float>(), rotation[2].get<float>(), rotation[3].get<float>()};
						transform.scale = {scale[0].get<float>(), scale[1].get<float>(), scale[2].get<float>()};
						if(!IsFinite(transform)) return AnimationStatus::InvalidArgument;
						parameter.type = AnimationParameterType::Transform;
						parameter.value = transform;
					} else {
						return AnimationStatus::InvalidType;
					}
					staged.push_back(std::move(parameter));
				}
				std::sort(staged.begin(), staged.end(), [](const auto& left, const auto& right) {
					return left.parameterId < right.parameterId;
				});
				if(std::adjacent_find(staged.begin(), staged.end(), [](const auto& left, const auto& right) {
					return left.parameterId == right.parameterId;
				}) != staged.end()) return AnimationStatus::Rejected;
				output.swap(staged);
				return AnimationStatus::Ok;
			} catch(const nlohmann::json::exception&) {
				return AnimationStatus::InvalidEnvelope;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationQuaternion MultiplyRotation(
			const AnimationQuaternion& left,
			const AnimationQuaternion& right) noexcept
		{
			AnimationQuaternion value{
				left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
				left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
				left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
				left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
			const float lengthSquared = value.x * value.x + value.y * value.y
				+ value.z * value.z + value.w * value.w;
			if(!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12F) return {};
			const float inverseLength = 1.0F / std::sqrt(lengthSquared);
			value.x *= inverseLength;
			value.y *= inverseLength;
			value.z *= inverseLength;
			value.w *= inverseLength;
			return value;
		}

		AnimationQuaternion ConjugateRotation(AnimationQuaternion value) noexcept
		{
			value.x = -value.x;
			value.y = -value.y;
			value.z = -value.z;
			return value;
		}

		AnimationFloat3 RotateVector(
			const AnimationQuaternion& rotation,
			const AnimationFloat3& vector) noexcept
		{
			const AnimationFloat3 twiceCross{
				2.0F * (rotation.y * vector.z - rotation.z * vector.y),
				2.0F * (rotation.z * vector.x - rotation.x * vector.z),
				2.0F * (rotation.x * vector.y - rotation.y * vector.x)};
			return {
				vector.x + rotation.w * twiceCross.x
					+ (rotation.y * twiceCross.z - rotation.z * twiceCross.y),
				vector.y + rotation.w * twiceCross.y
					+ (rotation.z * twiceCross.x - rotation.x * twiceCross.z),
				vector.z + rotation.w * twiceCross.z
					+ (rotation.x * twiceCross.y - rotation.y * twiceCross.x)};
		}

		AnimationTransform CombineTransform(
			const AnimationTransform& parent,
			const AnimationTransform& local) noexcept
		{
			const AnimationFloat3 scaled{
				local.translation.x * parent.scale.x,
				local.translation.y * parent.scale.y,
				local.translation.z * parent.scale.z};
			const AnimationFloat3 rotated = RotateVector(parent.rotation, scaled);
			AnimationTransform model;
			model.translation = {
				parent.translation.x + rotated.x,
				parent.translation.y + rotated.y,
				parent.translation.z + rotated.z};
			model.rotation = MultiplyRotation(parent.rotation, local.rotation);
			model.scale = {
				parent.scale.x * local.scale.x,
				parent.scale.y * local.scale.y,
				parent.scale.z * local.scale.z};
			return model;
		}

		bool TryMapRagdollBodyWorldToJointModel(
			const AnimationTransform& objectWorld,
			const AnimationTransform& animationJointModel,
			const RagdollBodyRecord& body,
			const AnimationTransform& bodyWorld,
			AnimationTransform& output) noexcept
		{
			// Jolt publishes a rigid body in world space, while animation blends a
			// skeleton joint in object/model space. Undo the authored joint-to-body
			// transform before removing the object's world transform; otherwise a
			// non-origin character or collider offset would visibly jump on activation.
			const AnimationTransform animationJointWorld = CombineTransform(
				objectWorld, animationJointModel);
			AnimationTransform jointWorld;
			jointWorld.rotation = MultiplyRotation(
				bodyWorld.rotation, ConjugateRotation(body.jointToBodyOrientation));
			jointWorld.scale = animationJointWorld.scale;
			const AnimationFloat3 scaledOffset{
				body.jointToBodyOffset.x * jointWorld.scale.x,
				body.jointToBodyOffset.y * jointWorld.scale.y,
				body.jointToBodyOffset.z * jointWorld.scale.z};
			const AnimationFloat3 worldOffset = RotateVector(
				jointWorld.rotation, scaledOffset);
			jointWorld.translation = {
				bodyWorld.translation.x - worldOffset.x,
				bodyWorld.translation.y - worldOffset.y,
				bodyWorld.translation.z - worldOffset.z};
			return IsFinite(jointWorld)
				&& ProceduralMath::TryRelativeTransform(objectWorld, jointWorld, output);
		}

		bool CaptureObjectWorldTransform(
			ObjectManager& objects,
			Object object,
			AnimationTransform& output) noexcept
		{
			AnimationTransform world{};
			bool captured = false;
			std::size_t depth = 0U;
			std::optional<Object> current = object;
			while(current && depth++ < kMaximumAnimationExecutionJoints) {
				const Transform* transform = objects.GetComponent<Transform>(*current);
				if(transform == nullptr) return false;
				AnimationTransform local;
				local.translation = {transform->position.x, transform->position.y, transform->position.z};
				local.rotation = {transform->rotation.x, transform->rotation.y,
					transform->rotation.z, transform->rotation.w};
				local.scale = {transform->scale.x, transform->scale.y, transform->scale.z};
				if(!IsFinite(local) || !IsNormalized(local.rotation)) return false;
				world = captured ? CombineTransform(local, world) : local;
				captured = true;
				current = objects.getParent(*current);
			}
			if(current.has_value()) return false;
			output = world;
			return captured;
		}

		AnimationPlaybackSnapshot MakePublishedPlaybackSnapshot(
			const AnimatorPlaybackStateSnapshot& source) noexcept
		{
			AnimationPlaybackSnapshot snapshot;
			snapshot.currentStateId = 1U;
			snapshot.transitionProgress = 0.0F;
			snapshot.normalizedTime = source.normalizedTime;
			snapshot.loopCount = source.loopCount;
			snapshot.effectiveSpeed = source.effectiveSpeed;
			snapshot.latestStatus = source.latestStatus;
			return snapshot;
		}
	}

	struct AnimationSystem::Impl {
		using PoseFallbackPublication = AnimationFallbackPublication<PublishedAnimatorPose>;
		enum class WorkPlanFinalizeState : std::uint8_t {
			Pending,
			Building,
			Ready,
			Failed
		};

		// A controller transition must advance its destination clip independently
		// while the ordinary playback cursor continues sampling the source clip.
		// This copied worker-owned state is committed with the graph snapshot, so a
		// script crossfade cannot expose a new state with an old or reset clip clock.
		struct ControllerTransitionRuntime {
			std::uint64_t transitionStableId = 0U;
			std::uint64_t sourceStateId = 0U;
			std::uint64_t destinationStateId = 0U;
			AnimatorControllerClipBinding destination;
			AnimatorPlayback destinationPlayback;
			AnimationClipView destinationClipView;
			AnimationEventStream destinationEventStream;
			AnimatorPlaybackAdvanceResult destinationAdvance;
			// Replacing an active transition captures the last complete blended pose.
			// A shared immutable copy avoids duplicating a full skeleton on every
			// worker capture while keeping interruption progress visually continuous.
			std::shared_ptr<const std::vector<AnimationTransform>> capturedSourcePose;
			std::optional<AnimationRootMotionDelta> capturedSourceRootMotion;
			bool ready = false;
		};

		struct RuntimeRecord {
			AnimatorInstanceHandle handle;
			AnimatorInstanceDescriptor descriptor;
			AnimatorPlaybackBinding binding;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 1U;
			std::uint64_t parameterGeneration = 1U;
			std::uint64_t executionModeGeneration = 1U;
			AnimationGeneration contentGeneration = 1U;
			// A history generation names one compatible rollback ring. Rebind and
			// lifecycle invalidation advance it before any replacement snapshot is kept.
			AnimationGeneration historyGeneration = 1U;
			AnimationExecutionModeState executionModeState{};
			std::vector<AnimationParameterSnapshot> parameters;
			// Runtime layer weights start from immutable authored defaults, then change
			// only through copied commands at the protected animation publication.
			std::vector<float> layerWeights;
			// Controller state advances on worker-owned copies and is committed only
			// with the same all-or-none authority publication as the sampled pose.
			StateMachineSnapshot graphState;
			ControllerTransitionRuntime transition;
			std::shared_ptr<const PublishedAnimatorPose> publishedPose;
			// The immediately preceding complete CPU pose is retained only for the
			// next animation-to-physics handoff. It lets activation preserve motion
			// without keeping an unbounded pose history outside rollback storage.
			std::shared_ptr<const PublishedAnimatorPose> previousPublishedPose;
			AnimationFallbackPublicationLatch<PublishedAnimatorPose> fallbackPublication;
			// Advanced control state changes only at PublishAnimationAuthority. These
			// copied identities let later frames validate managed requests without
			// allowing scripting workers to mutate evaluator or physics ownership.
			NativeAnimationUuid activeRetargetProfileId{};
			AnimationGeneration activeRetargetProfileGeneration = 0U;
			RetargetEvaluationMetrics latestRetargetMetrics{};
			std::uint64_t latestRetargetDeterministicHash = 0U;
			NativeAnimationUuid activeMotionDatabaseId{};
			AnimationGeneration activeMotionDatabaseGeneration = 0U;
			std::uint64_t latestMotionQueryId = 0U;
			std::uint64_t appliedMotionQueryId = 0U;
			MotionMatchCandidate latestMotionCandidate{};
			bool hasMotionCandidate = false;
			std::uint64_t ragdollId = 0U;
			RagdollGenerationStamp ragdollGeneration{};
			NativeAnimationUuid activeRagdollProfileId{};
			bool ragdollCreated = false;
			RagdollLifecycleState ragdollLifecycle = RagdollLifecycleState::Inactive;
			float ragdollBlendWeight = 0.0F;
			AnimationFixedTick ragdollLifecycleStartTick = 0;
			std::uint64_t ragdollRecoveryCandidateId = 0U;
			RagdollRecoveryBlockReason ragdollRecoveryBlockReason =
				RagdollRecoveryBlockReason::None;
			AnimationFloat3 ragdollRecoveryAlignedRootPosition{};
			float ragdollRecoveryDurationSeconds = 0.0F;
			std::uint32_t ragdollMotorDriveCount = 0U;
			AnimationStatus ragdollMotorStatus = AnimationStatus::NotReady;
			std::uint64_t pendingRagdollCorrelationId = 0U;
			RagdollGenerationStamp pendingRagdollPreviousGeneration{};
			RagdollLifecycleState pendingRagdollPreviousLifecycle =
				RagdollLifecycleState::Inactive;
			NativeAnimationUuid pendingRagdollPreviousProfileId{};
			float pendingRagdollPreviousBlendWeight = 0.0F;
			NativeAnimationRagdollOperation pendingRagdollOperation =
				NativeAnimationRagdollOperation::Deactivate;
		};

		struct DeterminismRequestCompletion {
			NativeAnimationDeterminismRequest request{};
			AnimationStatus status = AnimationStatus::NotReady;
			AnimationRollbackSnapshot sourceSnapshot{};
			bool stateRestored = false;
		};

		struct MotionMatchQueryCompletionV2 {
			// correlationId is retained only inside the runtime. The public result
			// record stays ABI-frozen while publication can still pair duplicate query
			// identifiers with the exact copied request that produced this result.
			std::uint64_t correlationId = 0U;
			NativeAnimationMotionMatchResultV2 result{};
			MotionMatchCandidate selectedCandidate{};
			bool hasSelectedCandidate = false;
		};

		// Each vector element is owned by one deterministic worker index. Planning
		// and evaluation never append to shared storage, so scheduler partitions are
		// disjoint even when many jobs complete out of order.
		struct FrameWorkItem {
			AnimationFrameInput input;
			AnimatorPlaybackBinding binding;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimatorPlaybackAdvanceResult advance;
			std::shared_ptr<PoseBuffer> pose;
			std::shared_ptr<const PublishedAnimatorPose> publication;
			AnimationStatus evaluationStatus = AnimationStatus::NotReady;
			AnimationGeneration candidatePoseGeneration = 0U;
			AnimationGeneration capturedPoseGeneration = 0U;
			std::uint64_t candidateAuthorityPublicationGeneration = 0U;
			std::uint64_t capturedStateGeneration = 0U;
			std::uint64_t capturedParameterGeneration = 0U;
			std::uint64_t capturedExecutionModeGeneration = 0U;
			AnimationGeneration contentGeneration = 0U;
			std::uint64_t candidateStateGeneration = 0U;
			std::uint64_t candidateParameterGeneration = 0U;
			std::uint64_t candidateExecutionModeGeneration = 0U;
			AnimationExecutionModeState previousExecutionModeState{};
			AnimatorPlaybackStateSnapshot capturedPlaybackState{};
			std::vector<AnimationParameterSnapshot> parameters;
			std::vector<float> layerWeights;
			StateMachineSnapshot capturedGraphState;
			StateMachineSnapshot candidateGraphState;
			ControllerTransitionRuntime transition;
			NativeAnimationUuid activeRetargetProfileId{};
			AnimationGeneration activeRetargetProfileGeneration = 0U;
			bool retargetActive = false;
			bool sampledSourceSkeleton = false;
			RetargetEvaluationMetrics retargetMetrics{};
			std::uint64_t retargetDeterministicHash = 0U;
			AnimationStatus retargetEvaluationStatus = AnimationStatus::NotReady;
			NativeAnimationUuid activeMotionDatabaseId{};
			AnimationGeneration activeMotionDatabaseGeneration = 0U;
			std::uint64_t latestMotionQueryId = 0U;
			std::uint64_t appliedMotionQueryId = 0U;
			MotionMatchCandidate latestMotionCandidate{};
			bool hasMotionCandidate = false;
			std::vector<NativeAnimationMotionMatchRequest> pendingMotionQueries;
			std::vector<NativeAnimationMotionMatchSnapshot> evaluatedMotionQueries;
			// V2 stays separate from the frozen V1 records while sharing the retained
			// candidate that drives the next authoritative controller frame.
			std::vector<NativeAnimationMotionMatchQueryRequestV2> pendingMotionQueriesV2;
			std::vector<MotionMatchQueryCompletionV2> evaluatedMotionQueriesV2;
			// A rollback restore is committed before this frame's worker copies are
			// built. The first evaluation must reproduce that exact retained state;
			// ordinary frame-time advancement resumes on the following frame.
			bool restoredFromHistory = false;
			std::vector<NativeAnimationProceduralGoalCommand> activeProceduralGoals;
			std::vector<NativeAnimationProceduralResultSnapshot> proceduralResults;
			bool proceduralStateObserved = false;
			std::shared_ptr<const PhysicsPoseSnapshot> ragdollPhysicsPose;
			RagdollGenerationStamp ragdollGeneration{};
			RagdollLifecycleState ragdollLifecycle = RagdollLifecycleState::Inactive;
			float ragdollBlendWeight = 0.0F;
			AnimationFixedTick ragdollLifecycleStartTick = 0;
			AnimationFloat3 ragdollRecoveryAlignedRootPosition{};
			float ragdollRecoveryDurationSeconds = 0.0F;
			std::vector<RagdollMotorDrive> ragdollMotorDrives;
			bool ragdollRecoveryBlendComplete = false;
			std::shared_ptr<const PublishedAnimatorPose> priorPublication;
			std::shared_ptr<const PoseFallbackPublication> priorFallbackPublication;
			AnimationGeneration sourceObjectGeneration = 0U;
			AnimationTransform sourceObjectWorldTransform{};
			bool sourceObjectWorldTransformCaptured = false;
			AnimationExecutionModeDecision executionDecision{};
			AnimationResolvedConsumerRequirements resolvedConsumers{};
			AnimationEvaluationCacheKey cacheKey{};
			AnimationEvaluationCacheRequirements cacheRequirements{};
			AnimationFallbackGenerationKey fallbackGenerations{};
			AnimationFallbackGenerationKey liveFallbackGenerations{};
			AnimationFallbackDecision fallbackDecision{};
			std::shared_ptr<const PoseFallbackPublication> fallbackCandidate;
			std::shared_ptr<const PublishedAnimatorPose> selectedPublication;
			std::optional<GpuAnimationLoweringInput> gpuLowering;
			// Preserve the checked request mask produced during preflight. Detailed
			// diagnostics must report the stages that will actually be dispatched,
			// rather than a hard-coded legacy bit pattern.
			std::uint32_t gpuStageMask = 0U;
			bool cacheEligible = false;
			bool cacheReused = false;
			bool safePose = true;
			bool planned = false;
			bool evaluated = false;
		};

		std::unique_ptr<AnimatorInstanceStore> instances = std::make_unique<AnimatorInstanceStore>();
		std::unique_ptr<AnimationCommandQueue> commands = std::make_unique<AnimationCommandQueue>();
		std::unique_ptr<ProceduralCommandStore> proceduralCommands =
			std::make_unique<ProceduralCommandStore>();
		std::unique_ptr<AdvancedAnimationCommandStore> advancedCommands =
			std::make_unique<AdvancedAnimationCommandStore>();
		AnimatorComponentBindingCoordinator componentBindings;
		// This is coordinator-thread domain state, not per-frame worker input.
		// Runtime applications keep the default enabled; the editor explicitly
		// switches it at protected Play Mode entry and exit boundaries.
		bool authoredComponentPlaybackEnabled = true;
		bool authoredComponentPlaybackAdvancing = true;
		bool headlessExecutionRequired = false;
		std::vector<std::unique_ptr<RuntimeRecord>> runtimeRecords;
		std::vector<AnimationFrameInput> capturedFrameInputs;
		std::vector<AnimationCommand> capturedCommands;
		// Each worker writes only entries targeting its disjoint animator range.
		// The protected publisher later turns these staged values into observable
		// receipts together with the pose generation they affected.
		std::vector<AnimationStatus> capturedCommandStatuses;
		// Procedural requests use their own fixed ABI record because the original
		// gameplay command stride is pinned. They are still drained and completed at
		// the same protected frame boundary as ordinary animation commands.
		std::vector<NativeAnimationProceduralGoalCommand> capturedProceduralCommands;
		std::vector<AnimationStatus> capturedProceduralCommandStatuses;
		std::vector<NativeAnimationRetargetRequest> capturedRetargetRequests;
		std::vector<NativeAnimationMotionMatchRequest> capturedMotionMatchRequests;
		std::vector<NativeAnimationMotionMatchQueryRequestV2>
			capturedMotionMatchQueriesV2;
		std::vector<NativeAnimationRagdollRequest> capturedRagdollRequests;
		std::vector<NativeAnimationDeterminismRequest> capturedDeterminismRequests;
		std::vector<DeterminismRequestCompletion> determinismRequestCompletions;
		// Managed/native producers append copied requests from any scripting worker.
		// Only PublishAnimationAuthority drains this bounded queue and touches live
		// scene/physics state, preserving the worker-safe scripting declaration.
		std::vector<AnimationRootMotionApplicationRequest> pendingRootMotionApplications;
		std::vector<FrameWorkItem> frameWork;
		AnimationWorkScheduler workScheduler{MakeAnimationWorkSchedulerConfiguration()};
		AnimationWorkCancellationSource workCancellationSource;
		AnimationWorkPlan workPlan;
		std::unique_ptr<AnimationWorkRangeLedger> workRangeLedger;
		std::vector<AnimationWorkItem> capturedWorkItems;
		std::vector<AnimationWorkCompletion> workCompletions;
		std::vector<std::size_t> deterministicPublicationOrder;
		std::vector<std::byte> frameScratch;
		AnimationMemoryController memoryController;
		AnimationHistory rollbackHistory;
		AnimationRollbackSnapshotCodec rollbackSnapshotCodec;
		AnimationMemoryLease frameScratchLease;
		std::shared_ptr<const AnimationMemoryLease> framePoseReservation;
		AnimationEvaluationCache evaluationCache;
		std::unique_ptr<AnimationGenerationLatch> lifecycleLatch =
			std::make_unique<AnimationGenerationLatch>();
		AnimationCancellationSource evaluationCancellationSource;
		AnimationCancellationToken evaluationCancellation;
		AnimationLifecycleGenerationSnapshot frameLifecycleGenerations{};
		std::uint64_t frameSceneGeneration = 0U;
		AnimationFixedTick frameTick = 0;
		std::atomic_size_t scheduledAnimatorCount = 0U;
		std::atomic_size_t plannedAnimatorCount = 0U;
		std::atomic_size_t evaluatedAnimatorCount = 0U;
		std::atomic_size_t publishedAnimatorCount = 0U;
		std::atomic_size_t cacheReusedAnimatorCount = 0U;
		std::atomic_size_t fallbackAnimatorCount = 0U;
		std::atomic_bool frameCancellationRequested = false;
		bool deterministicPublicationValidated = false;
		std::size_t availableWorkerCount = 0U;
		mutable std::mutex workPlanFinalizeMutex;
		std::condition_variable workPlanFinalizeCondition;
		WorkPlanFinalizeState workPlanFinalizeState = WorkPlanFinalizeState::Pending;
		std::atomic_uint64_t authorityPublicationGeneration = 0U;
		std::atomic_uint64_t runtimeSessionEpoch = 1U;
		std::atomic_uint64_t frameDeclarationEpoch = 0U;
		std::uint64_t lastPublishedFrameDeclarationEpoch = 0U;
		// Authored automatic root-motion requests are a dedicated ordered producer.
		// The sequence survives animator reuse and is reset only with process lifetime.
		std::uint64_t automaticRootMotionSequence = 0U;
		// Only the scheduler coordinator reads or writes this transition marker. It
		// lets a steady static project skip locking merely to clear empty buffers.
		bool hadScheduledAnimatorMembership = false;
		// Managed/gameplay producers may submit typed commands concurrently with a
		// protected shutdown request. This guard covers only pointer lifetime; each
		// runtime type retains its own fine-grained data lock for normal queue/store
		// operations, and worker evaluation never takes this lifecycle mutex.
		mutable std::mutex runtimeOwnershipMutex;
		// The singleton starts usable for the existing engine bootstrap path. The
		// flag then prevents calls after Shutdown from recreating queue ownership;
		// only an explicit Initialize begins another runtime session.
		std::atomic_bool runtimeActive = true;
		// Scheduler ownership remains on the thread that first constructs the
		// process-wide singleton. Lifecycle calls may not silently transfer that
		// protected-boundary responsibility to another thread.
		const std::thread::id coordinatorThread = std::this_thread::get_id();

		Impl()
		{
			(void)memoryController.Configure(MakeAnimationMemoryConfiguration());
		}

		void ResetFrameIntegrationStateLocked() noexcept
		{
			workCancellationSource.RequestCancellation();
			(void)evaluationCancellationSource.Cancel(AnimationCancellationReason::OwnerReleased);
			workCancellationSource = AnimationWorkCancellationSource{};
			evaluationCancellationSource = AnimationCancellationSource{};
			evaluationCancellation = {};
			workPlan = {};
			workRangeLedger.reset();
			frameWork.clear();
			capturedRetargetRequests.clear();
			capturedMotionMatchRequests.clear();
			capturedMotionMatchQueriesV2.clear();
			capturedRagdollRequests.clear();
			capturedDeterminismRequests.clear();
			determinismRequestCompletions.clear();
			capturedWorkItems.clear();
			workCompletions.clear();
			deterministicPublicationOrder.clear();
			frameScratch.clear();
			frameScratchLease.Release();
			framePoseReservation.reset();
			frameLifecycleGenerations = {};
			frameSceneGeneration = 0U;
			deterministicPublicationValidated = false;
			availableWorkerCount = 0U;
			frameCancellationRequested.store(false, std::memory_order_release);
			cacheReusedAnimatorCount.store(0U, std::memory_order_release);
			fallbackAnimatorCount.store(0U, std::memory_order_release);
			{
				std::scoped_lock finalizeLock(workPlanFinalizeMutex);
				workPlanFinalizeState = WorkPlanFinalizeState::Pending;
			}
		}

		void RequestFrameCancellation(
			AnimationCancellationReason reason = AnimationCancellationReason::Explicit) noexcept
		{
			(void)evaluationCancellationSource.Cancel(reason);
			workCancellationSource.RequestCancellation();
			frameCancellationRequested.store(true, std::memory_order_release);
		}

		bool PollFrameCancellation() noexcept
		{
			const bool cancelled = lifecycleLatch == nullptr
				|| !evaluationCancellation.IsValid()
				|| evaluationCancellation.IsCancellationRequested(*lifecycleLatch);
			if(cancelled) {
				workCancellationSource.RequestCancellation();
				frameCancellationRequested.store(true, std::memory_order_release);
			}
			return cancelled;
		}

		AnimationStatus InvalidateLifecycleLocked(AnimationLifecycleBoundary boundary) noexcept
		{
			if(lifecycleLatch == nullptr) return AnimationStatus::Cancelled;
			const AnimationStatus status = lifecycleLatch->Invalidate(boundary);
			if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
				const std::uint64_t currentGeneration = lifecycleLatch->Capture().Generation(boundary);
				(void)evaluationCancellationSource.Cancel(
					CancellationReasonForBoundary(boundary, currentGeneration));
				workCancellationSource.RequestCancellation();
				frameCancellationRequested.store(true, std::memory_order_release);
			}
			return status;
		}

		void ActivateRuntimeOwnershipLocked()
		{
			// AnimatorInstanceStore intentionally survives every shutdown so its
			// slot generations cannot restart and alias handles from an old session.
			if(commands == nullptr) commands = std::make_unique<AnimationCommandQueue>();
			if(proceduralCommands == nullptr) {
				proceduralCommands = std::make_unique<ProceduralCommandStore>();
			}
			if(advancedCommands == nullptr) {
				advancedCommands = std::make_unique<AdvancedAnimationCommandStore>();
			}
			if(lifecycleLatch == nullptr
				|| lifecycleLatch->Capture().Generation(AnimationLifecycleBoundary::Shutdown) == 0U) {
				lifecycleLatch = std::make_unique<AnimationGenerationLatch>();
			}
			(void)memoryController.Configure(MakeAnimationMemoryConfiguration());
			ResetFrameIntegrationStateLocked();
			// Publish activity only after every owned runtime object is restored.
			runtimeActive.store(true, std::memory_order_release);
		}

		RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) noexcept
		{
			if(!handle.IsValid() || handle.slotIndex >= runtimeRecords.size()) return nullptr;
			RuntimeRecord* record = runtimeRecords[handle.slotIndex].get();
			return record != nullptr && record->handle == handle ? record : nullptr;
		}

		const RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) const noexcept
		{
			if(!handle.IsValid() || handle.slotIndex >= runtimeRecords.size()) return nullptr;
			const RuntimeRecord* record = runtimeRecords[handle.slotIndex].get();
			return record != nullptr && record->handle == handle ? record : nullptr;
		}

		static AnimationNetworkFloat3Bits ToNetworkBits(
			const AnimationFloat3& value) noexcept
		{
			return {{
				EncodeAnimationNetworkFloat(value.x),
				EncodeAnimationNetworkFloat(value.y),
				EncodeAnimationNetworkFloat(value.z)}};
		}

		static AnimationNetworkQuaternionBits ToNetworkBits(
			const AnimationQuaternion& value) noexcept
		{
			return {{
				EncodeAnimationNetworkFloat(value.x),
				EncodeAnimationNetworkFloat(value.y),
				EncodeAnimationNetworkFloat(value.z),
				EncodeAnimationNetworkFloat(value.w)}};
		}

		static AnimationNetworkTransformBits ToNetworkBits(
			const AnimationTransform& value) noexcept
		{
			return {
				ToNetworkBits(value.translation),
				ToNetworkBits(value.rotation),
				ToNetworkBits(value.scale)};
		}

		static std::uint64_t RuntimePoseChecksum(
			const PoseBuffer& pose) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			for(const AnimationTransform& transform : pose.ModelTransforms()) {
				hash = HashDeterministicTransform(hash, transform);
			}
			for(const AnimationNamedFloatValue& value : pose.MorphValues()) {
				hash = MixDeterministicChecksum(hash, value.stableId);
				hash = MixDeterministicChecksum(hash, QuantizedFloat(value.value));
			}
			for(const AnimationNamedFloatValue& value : pose.CurveValues()) {
				hash = MixDeterministicChecksum(hash, value.stableId);
				hash = MixDeterministicChecksum(hash, QuantizedFloat(value.value));
			}
			return hash;
		}

		static std::uint64_t RuntimeStateChecksum(
			const RuntimeRecord& record) noexcept
		{
			const AnimatorPlaybackStateSnapshot playback = record.playback.Snapshot();
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixDeterministicChecksum(hash, record.binding.stateId);
			hash = MixDeterministicChecksum(hash,
				static_cast<std::uint64_t>(playback.absoluteTick));
			hash = MixDeterministicChecksum(hash,
				static_cast<std::uint64_t>(playback.loopCount));
			hash = MixDeterministicChecksum(hash,
				QuantizedFloat(playback.normalizedTime));
			hash = MixDeterministicChecksum(hash,
				record.activeRetargetProfileGeneration);
			hash = MixDeterministicChecksum(hash,
				record.activeMotionDatabaseGeneration);
			hash = MixDeterministicChecksum(hash,
				record.ragdollGeneration.publicationGeneration);
			return hash;
		}

		static AnimationDeterministicFingerprint BuildDeterministicFingerprint(
			const RuntimeRecord& record) noexcept
		{
			AnimationDeterministicFingerprint result;
			result.compatibility = record.binding.skeleton
				? record.binding.skeleton->compatibilityFingerprint : 0U;

			const AnimationGuid controllerId = record.descriptor.controller.IsValid()
				? record.descriptor.controller.assetId
				: (record.binding.clip ? record.binding.clip->assetId : AnimationGuid{});
			result.controllerAsset = HashAnimationGuid(1469598103934665603ULL, controllerId);
			result.programAsset = record.binding.evaluationProgram
				&& record.binding.evaluationProgram->stableHash != 0U
				? record.binding.evaluationProgram->stableHash
				: record.binding.clipDeterministicContentFingerprint;
			if(record.binding.retargetProfile) {
				result.retargetAsset = HashAnimationGuid(
					1469598103934665603ULL, record.binding.retargetProfile->assetId);
			}
			if(record.binding.motionDatabase) {
				result.motionDatabaseAsset = HashAnimationGuid(
					1469598103934665603ULL, record.binding.motionDatabase->assetId);
			}
			if(record.binding.ragdollProfile) {
				result.ragdollProfileAsset = HashAnimationGuid(
					1469598103934665603ULL, record.binding.ragdollProfile->assetId);
			}

			std::uint64_t aggregate = 1469598103934665603ULL;
			aggregate = MixDeterministicChecksum(aggregate, result.compatibility);
			aggregate = MixDeterministicChecksum(aggregate, result.controllerAsset);
			aggregate = MixDeterministicChecksum(aggregate, result.programAsset);
			aggregate = MixDeterministicChecksum(aggregate, result.retargetAsset);
			aggregate = MixDeterministicChecksum(aggregate, result.motionDatabaseAsset);
			aggregate = MixDeterministicChecksum(aggregate, result.ragdollProfileAsset);
			aggregate = MixDeterministicChecksum(aggregate, record.binding.clipGeneration);
			aggregate = MixDeterministicChecksum(aggregate, record.binding.skeletonGeneration);
			aggregate = MixDeterministicChecksum(aggregate, record.binding.programGeneration);
			aggregate = MixDeterministicChecksum(
				aggregate, record.binding.clipDeterministicContentFingerprint);
			if(record.binding.skeleton) {
				aggregate = HashAnimationGuid(aggregate, record.binding.skeleton->assetId);
				aggregate = MixDeterministicChecksum(
					aggregate, record.binding.skeleton->referencePoseHash);
			}
			result.aggregateAssetHash = aggregate;
			return result;
		}

		AnimationRollbackGenerationStamp BuildRollbackGenerations(
			const RuntimeRecord& record,
			const PublishedAnimatorPose& published) const noexcept
		{
			const AnimationLifecycleGenerationSnapshot lifecycle = lifecycleLatch
				? lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
			auto narrowLifecycleGeneration = [](std::uint64_t value) noexcept {
				return value > std::numeric_limits<AnimationGeneration>::max()
					? AnimationGeneration{0U}
					: static_cast<AnimationGeneration>(value);
			};
			return {
				published.objectGeneration != 0U
					? published.objectGeneration : record.handle.generation,
				record.descriptor.componentGeneration,
				narrowLifecycleGeneration(
					lifecycle.Generation(AnimationLifecycleBoundary::Scene)),
				record.contentGeneration,
				record.binding.programGeneration,
				narrowLifecycleGeneration(
					lifecycle.Generation(AnimationLifecycleBoundary::PhysicsWorld)),
				record.historyGeneration};
		}

		static AnimationStatus EncodeParameter(
			const AnimationParameterSnapshot& source,
			AnimationParameterRollbackState& output) noexcept
		{
			AnimationParameterRollbackState candidate;
			candidate.parameterId = source.parameterId;
			candidate.type = static_cast<AnimationRollbackValueType>(source.type);
			switch(source.type) {
			case AnimationParameterType::Trigger:
				if(!std::holds_alternative<std::monostate>(source.value)) {
					return AnimationStatus::InvalidArgument;
				}
				break;
			case AnimationParameterType::Boolean: {
				const bool* value = std::get_if<bool>(&source.value);
				if(value == nullptr) return AnimationStatus::InvalidArgument;
				candidate.valueWords[0U] = *value ? 1U : 0U;
				break;
			}
			case AnimationParameterType::Integer: {
				const std::int32_t* value = std::get_if<std::int32_t>(&source.value);
				if(value == nullptr) return AnimationStatus::InvalidArgument;
				candidate.valueWords[0U] = static_cast<std::uint32_t>(*value);
				break;
			}
			case AnimationParameterType::Float: {
				const float* value = std::get_if<float>(&source.value);
				if(value == nullptr || !std::isfinite(*value)) return AnimationStatus::InvalidArgument;
				candidate.valueWords[0U] = EncodeAnimationNetworkFloat(*value);
				break;
			}
			case AnimationParameterType::Vector3: {
				const AnimationFloat3* value = std::get_if<AnimationFloat3>(&source.value);
				if(value == nullptr || !IsFinite(*value)) return AnimationStatus::InvalidArgument;
				const auto words = ToNetworkBits(*value).words;
				std::copy(words.begin(), words.end(), candidate.valueWords.begin());
				break;
			}
			case AnimationParameterType::Quaternion: {
				const AnimationQuaternion* value = std::get_if<AnimationQuaternion>(&source.value);
				if(value == nullptr || !IsFinite(*value)) return AnimationStatus::InvalidArgument;
				const auto words = ToNetworkBits(*value).words;
				std::copy(words.begin(), words.end(), candidate.valueWords.begin());
				break;
			}
			case AnimationParameterType::Transform: {
				const AnimationTransform* value = std::get_if<AnimationTransform>(&source.value);
				if(value == nullptr || !IsFinite(*value)) return AnimationStatus::InvalidArgument;
				candidate.valueWords = ToNetworkBits(*value).words();
				break;
			}
			default:
				return AnimationStatus::InvalidArgument;
			}
			output = candidate;
			return AnimationStatus::Ok;
		}

		static AnimationStatus DecodeParameter(
			const AnimationParameterRollbackState& source,
			AnimationParameterSnapshot& output) noexcept
		{
			AnimationParameterSnapshot candidate;
			if(source.parameterId > UINT32_MAX) return AnimationStatus::RangeOverflow;
			candidate.parameterId = static_cast<std::uint32_t>(source.parameterId);
			candidate.type = static_cast<AnimationParameterType>(source.type);
			switch(source.type) {
			case AnimationRollbackValueType::Trigger:
				candidate.value = std::monostate{};
				break;
			case AnimationRollbackValueType::Boolean:
				candidate.value = source.valueWords[0U] != 0U;
				break;
			case AnimationRollbackValueType::Integer:
				candidate.value = static_cast<std::int32_t>(source.valueWords[0U]);
				break;
			case AnimationRollbackValueType::Float:
				candidate.value = DecodeAnimationNetworkFloat(source.valueWords[0U]);
				break;
			case AnimationRollbackValueType::Vector3:
				candidate.value = AnimationFloat3{
					DecodeAnimationNetworkFloat(source.valueWords[0U]),
					DecodeAnimationNetworkFloat(source.valueWords[1U]),
					DecodeAnimationNetworkFloat(source.valueWords[2U])};
				break;
			case AnimationRollbackValueType::Quaternion:
				candidate.value = AnimationQuaternion{
					DecodeAnimationNetworkFloat(source.valueWords[0U]),
					DecodeAnimationNetworkFloat(source.valueWords[1U]),
					DecodeAnimationNetworkFloat(source.valueWords[2U]),
					DecodeAnimationNetworkFloat(source.valueWords[3U])};
				break;
			case AnimationRollbackValueType::Transform:
				candidate.value = AnimationTransform{
					{DecodeAnimationNetworkFloat(source.valueWords[0U]),
						DecodeAnimationNetworkFloat(source.valueWords[1U]),
						DecodeAnimationNetworkFloat(source.valueWords[2U])},
					{DecodeAnimationNetworkFloat(source.valueWords[3U]),
						DecodeAnimationNetworkFloat(source.valueWords[4U]),
						DecodeAnimationNetworkFloat(source.valueWords[5U]),
						DecodeAnimationNetworkFloat(source.valueWords[6U])},
					{DecodeAnimationNetworkFloat(source.valueWords[7U]),
						DecodeAnimationNetworkFloat(source.valueWords[8U]),
						DecodeAnimationNetworkFloat(source.valueWords[9U])}};
				break;
			default:
				return AnimationStatus::InvalidArgument;
			}
			output = std::move(candidate);
			return AnimationStatus::Ok;
		}

		AnimationStatus BuildRollbackSnapshotLocked(
			const RuntimeRecord& record,
			const std::shared_ptr<const PublishedAnimatorPose>& published,
			AnimationRollbackSnapshot& output) const noexcept
		{
			if(!published || !published->snapshot || !published->snapshot->pose
				|| !record.binding.clip || !record.binding.skeleton
				|| record.binding.clipDeterministicContentFingerprint == 0U) {
				return AnimationStatus::NotReady;
			}
			try {
				AnimationRollbackSnapshot source;
				source.animator = record.handle;
				source.tick = published->snapshot->tick;
				// AnimationHistory retains one ring for one compatible binding epoch.
				// Frame authority remains available in the generic summary below.
				source.publicationGeneration = record.contentGeneration;
				source.frozenWorkerCount = static_cast<std::uint32_t>(std::max<std::size_t>(
					1U, std::min<std::size_t>(availableWorkerCount, UINT32_MAX)));
				source.fingerprint = BuildDeterministicFingerprint(record);
				source.generations = BuildRollbackGenerations(record, *published);
				if(!source.fingerprint.IsValid() || !source.generations.IsValid()) {
					return AnimationStatus::InvalidArgument;
				}

				const AnimatorPlaybackStateSnapshot playback = record.playback.Snapshot();
				const AnimatorPlaybackDeterministicState deterministicPlayback =
					record.playback.CaptureDeterministicState();
				AnimationControllerRollbackState controller;
				controller.controllerInstanceId = source.fingerprint.controllerAsset;
				controller.activeStateId = record.binding.stateId;
				controller.previousStateId = record.graphState.transitionSourceStateId;
				controller.stateEnteredTick = record.graphState.stateEnteredTick;
				controller.lastEvaluatedTick = std::max(
					record.graphState.lastTick, controller.stateEnteredTick);
				controller.deterministicRandomState = record.stateGeneration;
				source.controllers.push_back(controller);

				AnimationClipRollbackState clip;
				clip.clipInstanceId = HashAnimationGuid(
					1469598103934665603ULL, record.binding.clip->assetId);
				clip.clipAssetId = record.binding.clip->assetId;
				clip.clipGeneration = record.binding.clipGeneration;
				clip.clipContentFingerprint =
					record.binding.clipDeterministicContentFingerprint;
				clip.absoluteTick = deterministicPlayback.absoluteTick;
				clip.resolvedTick = playback.resolvedTick;
				clip.durationTicks = deterministicPlayback.configuration.durationTicks;
				clip.loopCount = playback.loopCount;
				clip.speedBits = EncodeAnimationNetworkFloat(
					deterministicPlayback.configuration.speed);
				clip.speedQ16 = deterministicPlayback.speedQ16;
				clip.speedProductRemainder = deterministicPlayback.speedProductRemainder;
				clip.subTickRemainderQ16 = deterministicPlayback.subTickRemainderQ16;
				clip.runState = static_cast<AnimationRollbackPlaybackState>(
					deterministicPlayback.runState);
				clip.direction = deterministicPlayback.configuration.speed == 0.0F
					? AnimationRollbackPlaybackDirection::Stationary
					: (deterministicPlayback.configuration.reverse
						? AnimationRollbackPlaybackDirection::Reverse
						: AnimationRollbackPlaybackDirection::Forward);
				clip.looping = deterministicPlayback.configuration.loop;
				source.clips.push_back(clip);

				if(record.graphState.activeTransitionStableId != 0U) {
					AnimationTransitionRollbackState transition;
					transition.transitionStableId = record.graphState.activeTransitionStableId;
					transition.sourceStateId = record.graphState.transitionSourceStateId;
					transition.destinationStateId =
						record.graphState.transitionDestinationStateId;
					transition.startTick = record.graphState.transitionStartTick;
					transition.durationTicks = record.graphState.transitionDurationTicks;
					transition.progressBits = EncodeAnimationNetworkFloat(
						record.graphState.transitionProgressNormalized);
					transition.destinationOffsetBits = EncodeAnimationNetworkFloat(
						record.graphState.retainedDestinationOffsetNormalized);
					transition.destinationEnteredTick =
						record.graphState.transitionDestinationEnteredTick;
					transition.interruptedTransitionStableId =
						record.graphState.interruptedTransitionStableId;
					transition.interruptedProgressBits = EncodeAnimationNetworkFloat(
						record.graphState.interruptedTransitionProgressNormalized);
					source.transitions.push_back(transition);
				}

				if(record.binding.evaluationProgram) {
					for(std::size_t layerIndex = 0U;
						layerIndex < record.binding.evaluationProgram->layers.size(); ++layerIndex) {
						const EvaluationLayer& value =
							record.binding.evaluationProgram->layers[layerIndex];
						AnimationLayerRollbackState layer;
						layer.layerStableId = value.stableId;
						layer.activeStateId = record.binding.stateId;
						layer.stateTick = record.graphState.stateEnteredTick;
						layer.weightBits = EncodeAnimationNetworkFloat(
							layerIndex < record.layerWeights.size()
								? record.layerWeights[layerIndex] : value.defaultWeight);
						layer.additive = value.additive;
						source.layers.push_back(layer);
					}
				}

				source.parameters.reserve(record.parameters.size());
				for(const AnimationParameterSnapshot& value : record.parameters) {
					AnimationParameterRollbackState encoded;
					const AnimationStatus status = EncodeParameter(value, encoded);
					if(status != AnimationStatus::Ok) return status;
					source.parameters.push_back(encoded);
				}
				for(const StateMachineTriggerEdge& value : record.graphState.retainedTriggerEdges) {
					source.triggers.push_back({
						value.parameterId, value.sequenceId, value.raised, false, false});
				}

				std::vector<AnimationEventIdentity> deliveredEvents;
				record.eventStream.CopyRetainedEvents(deliveredEvents);
				for(std::size_t index = 0U; index < deliveredEvents.size(); ++index) {
					const AnimationEventIdentity& value = deliveredEvents[index];
					AnimationEventCursorRollbackState event;
					event.streamId = source.fingerprint.controllerAsset;
					event.stateInstanceId = value.stateInstanceId;
					event.clipAssetId = value.clipAssetId;
					event.eventId = value.eventId;
					event.sourceTick = value.sourceTick;
					event.loopCount = value.loopCount;
					event.deliveredSequence = index + 1U;
					event.deliveredThroughTick = source.tick;
					event.delivered = true;
					source.events.push_back(event);
				}
				source.eventWatermark = {
					source.fingerprint.controllerAsset,
					static_cast<std::uint64_t>(deliveredEvents.size()),
					source.tick};

				const PoseBuffer& pose = *published->snapshot->pose;
				if(const std::optional<AnimationRootMotionDelta>& delta =
					pose.RootMotionDelta()) {
					AnimationRootMotionRollbackState root;
					root.accumulationId = delta->generation;
					root.sourceBeginTick = delta->sourceBeginTick;
					root.sourceEndTick = delta->sourceEndTick;
					root.localTranslation = ToNetworkBits(delta->localTranslation);
					root.worldTranslation = ToNetworkBits(delta->worldTranslation);
					root.localYawBits = EncodeAnimationNetworkFloat(delta->localYawRadians);
					root.worldYawBits = EncodeAnimationNetworkFloat(delta->worldYawRadians);
					root.accumulatedCycles = delta->accumulatedCycles;
					root.sourceStateId = delta->sourceStateId;
					root.layerIndex = delta->layerIndex;
					root.generation = delta->generation;
					root.appliedThroughTick = delta->sourceEndTick;
					source.rootMotionStates.push_back(root);
					source.rootMotion.sourceTick = delta->sourceEndTick;
					source.rootMotion.translationAndYawBits = {
						EncodeAnimationNetworkFloat(delta->localTranslation.x),
						EncodeAnimationNetworkFloat(delta->localTranslation.y),
						EncodeAnimationNetworkFloat(delta->localTranslation.z),
						EncodeAnimationNetworkFloat(delta->localYawRadians)};
					source.rootMotion.requiresSceneTransaction =
						record.binding.rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToObject;
					source.rootMotion.requiresPhysicsCommand =
						record.binding.rootMotionPolicy == AnimatorRootMotionPolicy::ApplyToCharacter;
				}

				std::uint32_t resultCount = 0U;
				const NativeAnimationAnimatorHandle nativeHandle{
					record.handle.slotIndex, record.handle.generation};
				AnimationStatus resultStatus = proceduralCommands->CopyResults(
					nativeHandle, {}, resultCount);
				if(resultStatus == AnimationStatus::Ok
					|| resultStatus == AnimationStatus::BufferTooSmall) {
					std::vector<NativeAnimationProceduralResultSnapshot> results(resultCount);
					if(resultCount != 0U) {
						resultStatus = proceduralCommands->CopyResults(
							nativeHandle, results, resultCount);
					}
					if(resultStatus != AnimationStatus::Ok) return resultStatus;
					for(const NativeAnimationProceduralResultSnapshot& value : results) {
						AnimationIkRollbackState ik;
						ik.stableNodeId = value.goalId;
						ik.goalId = value.goalId;
						ik.targetStableId = value.targetJointStableId;
						ik.evaluatedTick = value.observationTick;
						ik.solverState = static_cast<std::uint32_t>(value.state)
							<= static_cast<std::uint32_t>(
								NativeAnimationProceduralSolverState::NoContribution)
							? static_cast<AnimationRollbackIkState>(value.state)
							: AnimationRollbackIkState::NoContribution;
						ik.weightBits = EncodeAnimationNetworkFloat(value.finalContribution);
						ik.errorBits = EncodeAnimationNetworkFloat(value.targetErrorMeters);
						ik.lockedContactTransform = ToNetworkBits(
							FromNativeTransform(value.resultTransform));
						ik.contactSurfaceNormal = ToNetworkBits(AnimationFloat3{});
						ik.contactContributionBits = EncodeAnimationNetworkFloat(0.0F);
						source.ikStates.push_back(ik);
					}
				}

				if(record.activeRetargetProfileGeneration != 0U
					&& record.binding.compiledRetargetProfile
					&& record.latestRetargetDeterministicHash != 0U) {
					AnimationRetargetRollbackState retarget;
					retarget.stableNodeId = 1U;
					retarget.profileFingerprint =
						record.binding.compiledRetargetProfile->stableHash;
					retarget.profileGeneration = record.activeRetargetProfileGeneration;
					retarget.sourceSkeletonGeneration =
						record.binding.retargetSourceSkeletonGeneration;
					retarget.targetSkeletonGeneration = record.binding.skeletonGeneration;
					retarget.sourcePoseGeneration = published->snapshot->publicationGeneration;
					retarget.targetPoseGeneration = published->snapshot->publicationGeneration;
					retarget.sourceTick = source.tick;
					retarget.normalizedTimeBits = EncodeAnimationNetworkFloat(
						playback.normalizedTime);
					retarget.deterministicHash = record.latestRetargetDeterministicHash;
					retarget.reducedFidelity =
						record.latestRetargetMetrics.fallbackCount != 0U;
					source.retargetStates.push_back(retarget);
				}

				if(record.hasMotionCandidate && record.binding.motionDatabase
					&& record.latestMotionQueryId != 0U) {
					AnimationMotionMatchRollbackState motion;
					motion.stableNodeId = 1U;
					motion.databaseFingerprint =
						record.binding.motionDatabase->featureSchemaFingerprint;
					motion.databaseGeneration = record.activeMotionDatabaseGeneration;
					motion.querySequence = record.latestMotionQueryId;
					motion.candidateId = record.latestMotionCandidate.stableSampleId;
					motion.sourceClipFingerprint = record.latestMotionCandidate.sourceClipId;
					motion.sampleTick = record.latestMotionCandidate.sourceTick;
					const float score = static_cast<float>(std::min<std::uint64_t>(
						record.latestMotionCandidate.score.totalCost,
						static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
					motion.totalScoreBits = EncodeAnimationNetworkFloat(score);
					std::uint64_t selectionHash = 1469598103934665603ULL;
					selectionHash = MixDeterministicChecksum(
						selectionHash, motion.candidateId);
					selectionHash = MixDeterministicChecksum(
						selectionHash, motion.sourceClipFingerprint);
					selectionHash = MixDeterministicChecksum(
						selectionHash, static_cast<std::uint64_t>(motion.sampleTick));
					motion.selectionHash = selectionHash;
					source.motionMatchStates.push_back(motion);
				}

				for(const AnimationNamedFloatValue& value : pose.MorphValues()) {
					source.morphStates.push_back({
						value.stableId,
						value.stableId,
						EncodeAnimationNetworkFloat(value.value),
						published->snapshot->publicationGeneration});
				}

				if(record.ragdollGeneration.IsValid()) {
					const std::shared_ptr<const PhysicsPoseSnapshot> physicsPose =
						Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(
							record.ragdollId, record.ragdollGeneration);
					if(physicsPose && ValidatePhysicsPoseSnapshot(
							*physicsPose, physicsPose->generation) == AnimationStatus::Ok) {
						for(const PhysicsPoseBodySnapshot& value : physicsPose->bodies) {
							AnimationRagdollRollbackState ragdoll;
							ragdoll.ragdollInstanceId = record.ragdollId;
							ragdoll.bodyId = value.bodyId;
							ragdoll.stableJointId = value.stableJointId;
							ragdoll.lifecycle = static_cast<AnimationRollbackRagdollState>(
								record.ragdollLifecycle);
							ragdoll.worldGeneration = physicsPose->generation.worldGeneration;
							ragdoll.profileGeneration = physicsPose->generation.profileGeneration;
							ragdoll.skeletonGeneration = physicsPose->generation.skeletonGeneration;
							ragdoll.physicsPublicationGeneration =
								physicsPose->generation.publicationGeneration;
							ragdoll.bodyGeneration = value.bodyGeneration;
							ragdoll.fixedStep = physicsPose->generation.fixedStep;
							ragdoll.modelTransform = ToNetworkBits(value.modelPose);
							ragdoll.linearVelocity = ToNetworkBits(value.linearVelocity);
							ragdoll.angularVelocity = ToNetworkBits(value.angularVelocity);
							ragdoll.blendWeightBits = EncodeAnimationNetworkFloat(
								record.ragdollBlendWeight);
							ragdoll.eventSequence =
								record.eventStream.Diagnostics().emittedEvents;
							ragdoll.sleeping = value.sleeping;
							source.ragdollStates.push_back(ragdoll);
						}
					}
				}
				if((record.ragdollLifecycle == RagdollLifecycleState::Creating
					|| record.ragdollLifecycle == RagdollLifecycleState::Active
					|| record.ragdollLifecycle == RagdollLifecycleState::Recovering
					|| record.ragdollLifecycle == RagdollLifecycleState::TearingDown)
					&& source.ragdollStates.empty()) {
					// Never capture an animation-only half of a cross-owner ragdoll state.
					// A later restore would otherwise appear successful while Jolt remained
					// at a different lifecycle or body generation.
					return AnimationStatus::NotReady;
				}

				const std::uint64_t poseHash = RuntimePoseChecksum(pose);
				const std::uint64_t stateHash = RuntimeStateChecksum(record);
				source.state.push_back({
					AnimationDeterministicStateKind::Physics,
					0U,
					0U,
					1U,
					{poseHash, stateHash, published->authorityPublicationGeneration, 0U}});
				return rollbackSnapshotCodec.Capture(source, output);
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		static std::uint64_t RuntimePoseChecksum(
			const AnimationRollbackSnapshot& snapshot) noexcept
		{
			const auto summary = std::find_if(
				snapshot.state.begin(), snapshot.state.end(), [](const auto& value) {
					return value.kind == AnimationDeterministicStateKind::Physics
						&& value.stableId == 1U && value.ordinal == 0U;
				});
			return summary == snapshot.state.end() ? 0U : summary->values[0U];
		}

		static std::uint64_t RuntimeStateChecksum(
			const AnimationRollbackSnapshot& snapshot) noexcept
		{
			const auto summary = std::find_if(
				snapshot.state.begin(), snapshot.state.end(), [](const auto& value) {
					return value.kind == AnimationDeterministicStateKind::Physics
						&& value.stableId == 1U && value.ordinal == 0U;
				});
			return summary == snapshot.state.end() ? 0U : summary->values[1U];
		}

		AnimationStatus RestoreRollbackSnapshotLocked(
			RuntimeRecord& record,
			const AnimationRollbackSnapshot& snapshot) noexcept
		{
			if(snapshot.controllers.size() != 1U || snapshot.clips.size() != 1U
				|| !snapshot.ikStates.empty() || !snapshot.ragdollStates.empty()
				|| snapshot.rootMotion.requiresSceneTransaction
				|| snapshot.rootMotion.requiresPhysicsCommand) {
				// Procedural goals and active physics state require their owning command
				// boundaries. Until a complete cross-owner transaction is available, reject
				// atomically instead of partially restoring only animation-owned fields.
				return AnimationStatus::Unsupported;
			}
			if(record.historyGeneration == std::numeric_limits<AnimationGeneration>::max()
				|| record.stateGeneration == std::numeric_limits<std::uint64_t>::max()
				|| record.parameterGeneration == std::numeric_limits<std::uint64_t>::max()
				|| record.executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
				return AnimationStatus::RangeOverflow;
			}

			try {
				AnimatorPlaybackBinding candidateBinding = record.binding;
				const AnimationClipRollbackState& clipState = snapshot.clips.front();
				auto selectClip = [&](const std::shared_ptr<const AnimationClipAsset>& clip,
					AnimationGeneration generation,
					std::uint64_t fingerprint,
					const std::vector<std::uint8_t>& residentSegments,
					const std::shared_ptr<const AnimationClipSegmentPayloadSet>&
						streamedSegmentPayloads) {
					if(!clip || clip->assetId != clipState.clipAssetId
						|| generation != clipState.clipGeneration
						|| fingerprint != clipState.clipContentFingerprint) {
						return false;
					}
					candidateBinding.clip = clip;
					candidateBinding.clipGeneration = generation;
					candidateBinding.clipDeterministicContentFingerprint = fingerprint;
					candidateBinding.residentSegments = residentSegments;
					candidateBinding.streamedSegmentPayloads = streamedSegmentPayloads;
					return true;
				};
				bool clipSelected = selectClip(
					record.binding.clip,
					record.binding.clipGeneration,
					record.binding.clipDeterministicContentFingerprint,
					record.binding.residentSegments,
					record.binding.streamedSegmentPayloads);
				if(!clipSelected) {
					for(const AnimatorControllerClipBinding& candidate
						: record.binding.controllerClips) {
						if(selectClip(candidate.clip, candidate.clipGeneration,
							candidate.deterministicContentFingerprint,
							candidate.residentSegments,
							candidate.streamedSegmentPayloads)) {
							clipSelected = true;
							break;
						}
					}
				}
				if(!clipSelected || !candidateBinding.clip) {
					return AnimationStatus::IncompatibleAsset;
				}

				const AnimationControllerRollbackState& controller =
					snapshot.controllers.front();
				candidateBinding.stateId = controller.activeStateId;
				StateMachineSnapshot candidateGraph;
				candidateGraph.activeStateId = controller.activeStateId;
				candidateGraph.stateEnteredTick = controller.stateEnteredTick;
				candidateGraph.lastTick = controller.lastEvaluatedTick;
				if(snapshot.transitions.size() > 1U) return AnimationStatus::InvalidArgument;
				if(!snapshot.transitions.empty()) {
					const AnimationTransitionRollbackState& transition =
						snapshot.transitions.front();
					candidateGraph.activeTransitionStableId = transition.transitionStableId;
					candidateGraph.transitionSourceStateId = transition.sourceStateId;
					candidateGraph.transitionDestinationStateId =
						transition.destinationStateId;
					candidateGraph.transitionStartTick = transition.startTick;
					candidateGraph.transitionDurationTicks = transition.durationTicks;
					candidateGraph.transitionProgressNormalized =
						DecodeAnimationNetworkFloat(transition.progressBits);
					candidateGraph.retainedDestinationOffsetNormalized =
						DecodeAnimationNetworkFloat(transition.destinationOffsetBits);
					candidateGraph.transitionDestinationEnteredTick =
						transition.destinationEnteredTick;
					candidateGraph.interruptedTransitionStableId =
						transition.interruptedTransitionStableId;
					candidateGraph.interruptedTransitionProgressNormalized =
						DecodeAnimationNetworkFloat(transition.interruptedProgressBits);
				}
				for(const AnimationTriggerRollbackState& trigger : snapshot.triggers) {
					candidateGraph.retainedTriggerEdges.push_back({
						trigger.parameterId, trigger.sequenceId, trigger.raised});
				}
				if(candidateBinding.evaluationProgram
					&& ValidateStateMachineSnapshot(
						*candidateBinding.evaluationProgram, candidateGraph)
						!= AnimationStatus::Ok) {
					return AnimationStatus::IncompatibleAsset;
				}

				AnimatorPlaybackDeterministicState playbackState;
				playbackState.configuration = record.playback.Configuration();
				playbackState.configuration.durationTicks = clipState.durationTicks;
				playbackState.configuration.loop = clipState.looping;
				playbackState.configuration.reverse =
					clipState.direction == AnimationRollbackPlaybackDirection::Reverse;
				playbackState.configuration.speed =
					DecodeAnimationNetworkFloat(clipState.speedBits);
				playbackState.runState = static_cast<AnimatorPlaybackRunState>(clipState.runState);
				playbackState.absoluteTick = clipState.absoluteTick;
				playbackState.speedQ16 = clipState.speedQ16;
				playbackState.speedProductRemainder = clipState.speedProductRemainder;
				playbackState.subTickRemainderQ16 = clipState.subTickRemainderQ16;
				playbackState.latestStatus = AnimationStatus::Ok;
				playbackState.configured = true;
				AnimatorPlayback candidatePlayback;
				const AnimationStatus playbackStatus =
					candidatePlayback.RestoreDeterministicState(playbackState);
				if(playbackStatus != AnimationStatus::Ok) return playbackStatus;

				std::vector<AnimationParameterSnapshot> candidateParameters;
				candidateParameters.reserve(snapshot.parameters.size());
				for(const AnimationParameterRollbackState& value : snapshot.parameters) {
					AnimationParameterSnapshot decoded;
					const AnimationStatus status = DecodeParameter(value, decoded);
					if(status != AnimationStatus::Ok) return status;
					candidateParameters.push_back(std::move(decoded));
				}
				std::vector<float> candidateLayerWeights;
				if(candidateBinding.evaluationProgram) {
					if(snapshot.layers.size()
						!= candidateBinding.evaluationProgram->layers.size()) {
						return AnimationStatus::IncompatibleAsset;
					}
					candidateLayerWeights.reserve(snapshot.layers.size());
					for(std::size_t layerIndex = 0U;
						layerIndex < snapshot.layers.size(); ++layerIndex) {
						const AnimationLayerRollbackState& retained = snapshot.layers[layerIndex];
						const EvaluationLayer& authored =
							candidateBinding.evaluationProgram->layers[layerIndex];
						const float weight = DecodeAnimationNetworkFloat(retained.weightBits);
						if(retained.layerStableId != authored.stableId
							|| !std::isfinite(weight) || weight < 0.0F || weight > 1.0F) {
							return AnimationStatus::IncompatibleAsset;
						}
						candidateLayerWeights.push_back(weight);
					}
				} else if(!snapshot.layers.empty()) {
					return AnimationStatus::IncompatibleAsset;
				}

				AnimationEventStream candidateEventStream;
				const AnimationStatus eventConfigure = candidateEventStream.Configure(
					{candidateBinding.clip->assetId, candidateBinding.clipGeneration},
					candidateBinding.clip->durationTicks,
					candidateBinding.clip->events);
				if(eventConfigure != AnimationStatus::Ok) return eventConfigure;
				std::vector<AnimationEventIdentity> deliveredEvents;
				deliveredEvents.reserve(snapshot.events.size());
				for(const AnimationEventCursorRollbackState& value : snapshot.events) {
					if(!value.delivered) continue;
					deliveredEvents.push_back({
						record.handle,
						value.stateInstanceId,
						value.clipAssetId,
						value.eventId,
						value.sourceTick,
						value.loopCount});
				}
				const AnimationStatus eventRestore =
					candidateEventStream.RestoreDeliveryHistory(deliveredEvents);
				if(eventRestore != AnimationStatus::Ok) return eventRestore;

				NativeAnimationUuid candidateRetargetId{};
				AnimationGeneration candidateRetargetGeneration = 0U;
				std::uint64_t candidateRetargetHash = 0U;
				if(snapshot.retargetStates.size() > 1U) return AnimationStatus::InvalidArgument;
				if(!snapshot.retargetStates.empty()) {
					const AnimationRetargetRollbackState& value =
						snapshot.retargetStates.front();
					if(!candidateBinding.retargetProfile
						|| !candidateBinding.compiledRetargetProfile
						|| value.profileGeneration != candidateBinding.retargetProfileGeneration
						|| value.profileFingerprint
							!= candidateBinding.compiledRetargetProfile->stableHash) {
						return AnimationStatus::IncompatibleAsset;
					}
					candidateRetargetId = ToNativeUuid(
						candidateBinding.retargetProfile->assetId.value);
					candidateRetargetGeneration = value.profileGeneration;
					candidateRetargetHash = value.deterministicHash;
				}

				NativeAnimationUuid candidateMotionId{};
				AnimationGeneration candidateMotionGeneration = 0U;
				std::uint64_t candidateMotionQueryId = 0U;
				MotionMatchCandidate candidateMotion;
				bool candidateHasMotion = false;
				if(snapshot.motionMatchStates.size() > 1U) return AnimationStatus::InvalidArgument;
				if(!snapshot.motionMatchStates.empty()) {
					const AnimationMotionMatchRollbackState& value =
						snapshot.motionMatchStates.front();
					if(!candidateBinding.motionDatabase
						|| value.databaseGeneration != candidateBinding.motionDatabaseGeneration
						|| value.databaseFingerprint
							!= candidateBinding.motionDatabase->featureSchemaFingerprint) {
						return AnimationStatus::IncompatibleAsset;
					}
					candidateMotionId = ToNativeUuid(candidateBinding.motionDatabase->assetId.value);
					candidateMotionGeneration = value.databaseGeneration;
					candidateMotionQueryId = value.querySequence;
					candidateMotion.stableSampleId = value.candidateId;
					candidateMotion.sourceClipId = value.sourceClipFingerprint;
					candidateMotion.sourceTick = value.sampleTick;
					const float score = DecodeAnimationNetworkFloat(value.totalScoreBits);
					candidateMotion.score.totalCost = score <= 0.0F
						? 0U : static_cast<std::uint64_t>(score);
					candidateHasMotion = !value.usedFallback && value.candidateId != 0U;
				}

				// Every allocation and compatibility check above completed before this
				// protected all-or-none replacement of animation-owned live state.
				record.binding = std::move(candidateBinding);
				record.playback = candidatePlayback;
				record.eventStream = std::move(candidateEventStream);
				record.graphState = std::move(candidateGraph);
				record.parameters = std::move(candidateParameters);
				record.layerWeights = std::move(candidateLayerWeights);
				record.activeRetargetProfileId = candidateRetargetId;
				record.activeRetargetProfileGeneration = candidateRetargetGeneration;
				record.latestRetargetDeterministicHash = candidateRetargetHash;
				record.activeMotionDatabaseId = candidateMotionId;
				record.activeMotionDatabaseGeneration = candidateMotionGeneration;
				record.latestMotionQueryId = candidateMotionQueryId;
				record.appliedMotionQueryId = candidateMotionQueryId;
				record.latestMotionCandidate = candidateMotion;
				record.hasMotionCandidate = candidateHasMotion;
				++record.historyGeneration;
				++record.stateGeneration;
				++record.parameterGeneration;
				++record.executionModeGeneration;
				rollbackHistory.Clear(record.handle);
				(void)evaluationCache.InvalidateAnimator(record.handle);
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			}
		}

		void PrepareDeterminismRequestsLocked() noexcept
		{
			determinismRequestCompletions.clear();
			try {
				determinismRequestCompletions.reserve(capturedDeterminismRequests.size());
				for(const NativeAnimationDeterminismRequest& request
					: capturedDeterminismRequests) {
					DeterminismRequestCompletion completion;
					completion.request = request;
					RuntimeRecord* record = FindRuntimeRecordLocked({
						request.animator.slotIndex, request.animator.generation});
					if(record == nullptr || !instances->IsAlive(record->handle)) {
						completion.status = AnimationStatus::StaleHandle;
						determinismRequestCompletions.push_back(std::move(completion));
						continue;
					}
					const std::shared_ptr<const PublishedAnimatorPose> published =
						std::atomic_load_explicit(
							&record->publishedPose, std::memory_order_acquire);
					if(!published || !published->snapshot || !published->snapshot->pose) {
						completion.status = AnimationStatus::NotReady;
						determinismRequestCompletions.push_back(std::move(completion));
						continue;
					}
					if(request.expectedPublicationGeneration != 0U
						&& request.expectedPublicationGeneration
							!= published->authorityPublicationGeneration) {
						completion.status = AnimationStatus::StaleHandle;
						determinismRequestCompletions.push_back(std::move(completion));
						continue;
					}
					if(request.operation == NativeAnimationDeterminismOperation::Capture
						|| request.operation
							== NativeAnimationDeterminismOperation::RequestReplication) {
						// Capture reports the pose committed later in this same frame.
						completion.status = AnimationStatus::NotReady;
						determinismRequestCompletions.push_back(std::move(completion));
						continue;
					}

					const AnimationRollbackRestoreRequirements requirements{
						record->handle,
						BuildDeterministicFingerprint(*record),
						record->contentGeneration,
						BuildRollbackGenerations(*record, *published)};
					AnimationStatus lookupStatus = AnimationStatus::InvalidArgument;
					if(request.operation == NativeAnimationDeterminismOperation::Restore) {
						lookupStatus = rollbackHistory.LookupDetailed(
							record->handle,
							request.sourceTick,
							requirements,
							completion.sourceSnapshot).status;
					} else if(request.operation
						== NativeAnimationDeterminismOperation::ReplaySeek) {
						AnimationReplayRequest replay;
						replay.mode = AnimationReplayMode::ReplaySeek;
						replay.animator = record->handle;
						replay.seekTick = request.targetTick;
						replay.requirements = requirements;
						replay.eventPolicy = (request.flags & 1U) != 0U
							? AnimationReplayEventPolicy::ReplayHistorical
							: AnimationReplayEventPolicy::SuppressHistorical;
						// Production keeps seek publication atomic by requiring the exact
						// retained tick. The standalone replay helper remains available for
						// host-owned bounded catch-up/resimulation.
						replay.requireExactTick = true;
						const AnimationReplayResult replayResult =
							ResolveAnimationReplay(rollbackHistory, replay);
						lookupStatus = replayResult.status;
						if(lookupStatus == AnimationStatus::Ok) {
							completion.sourceSnapshot = replayResult.resumeSnapshot;
						}
					} else {
						lookupStatus = AnimationStatus::InvalidArgument;
					}

					if(lookupStatus == AnimationStatus::Ok
						&& request.expectedChecksum != 0U
						&& request.expectedChecksum
							!= RuntimePoseChecksum(completion.sourceSnapshot)) {
						lookupStatus = AnimationStatus::DeterminismViolation;
					}
					completion.status = lookupStatus == AnimationStatus::Ok
						? RestoreRollbackSnapshotLocked(
							*record, completion.sourceSnapshot)
						: lookupStatus;
					completion.stateRestored = completion.status == AnimationStatus::Ok;
					determinismRequestCompletions.push_back(std::move(completion));
				}
			} catch(const std::bad_alloc&) {
				// A bounded history allocation failure is reported per request and leaves
				// every live runtime record in its prior complete state.
				for(const NativeAnimationDeterminismRequest& request
					: capturedDeterminismRequests) {
					const auto found = std::find_if(
						determinismRequestCompletions.begin(),
						determinismRequestCompletions.end(),
						[&request](const DeterminismRequestCompletion& value) {
							return value.request.correlationId == request.correlationId;
						});
					if(found == determinismRequestCompletions.end()) {
						determinismRequestCompletions.push_back({
							request, AnimationStatus::HardLimitExceeded, {}, false});
					}
				}
			}
		}

		void CaptureRollbackHistoryLocked() noexcept
		{
			for(const std::unique_ptr<RuntimeRecord>& owned : runtimeRecords) {
				if(!owned || !instances->IsAlive(owned->handle)) continue;
				const std::shared_ptr<const PublishedAnimatorPose> published =
					std::atomic_load_explicit(
						&owned->publishedPose, std::memory_order_acquire);
				AnimationRollbackSnapshot snapshot;
				AnimationStatus status = BuildRollbackSnapshotLocked(
					*owned, published, snapshot);
				if(status == AnimationStatus::Ok) {
					status = rollbackHistory.Store(std::move(snapshot));
				}

				// Capture and replication requests become terminal only after the same
				// immutable value has entered the bounded history ring. This prevents a
				// successful receipt from naming state that cannot subsequently restore.
				for(DeterminismRequestCompletion& completion
					: determinismRequestCompletions) {
					if(completion.request.animator.slotIndex != owned->handle.slotIndex
						|| completion.request.animator.generation != owned->handle.generation
						|| completion.status != AnimationStatus::NotReady
						|| (completion.request.operation
							!= NativeAnimationDeterminismOperation::Capture
							&& completion.request.operation
								!= NativeAnimationDeterminismOperation::RequestReplication)) {
						continue;
					}
					completion.status = completion.request.expectedPublicationGeneration != 0U
						&& (!published || completion.request.expectedPublicationGeneration
							!= published->authorityPublicationGeneration)
						? AnimationStatus::StaleHandle : status;
					if(completion.status == AnimationStatus::Ok && published) {
						const AnimationRollbackRestoreRequirements requirements{
							owned->handle,
							BuildDeterministicFingerprint(*owned),
							owned->contentGeneration,
							BuildRollbackGenerations(*owned, *published)};
						completion.status = rollbackHistory.LookupDetailed(
							owned->handle,
							published->snapshot->tick,
							requirements,
							completion.sourceSnapshot).status;
					}
				}
			}
		}

		bool CaptureFrameLocked(
			AnimationFixedTick captureTick,
			std::uint64_t candidateAuthorityGeneration) noexcept
		{
			if(instances == nullptr || commands == nullptr || proceduralCommands == nullptr
				|| advancedCommands == nullptr
				|| lifecycleLatch == nullptr
				|| candidateAuthorityGeneration == 0U) return false;
			ResetFrameIntegrationStateLocked();
			if(evaluationCancellationSource.Initialize(
				AnimationCancellationOperation::CpuEvaluation,
				*lifecycleLatch) != AnimationStatus::Ok) {
				frameCancellationRequested.store(true, std::memory_order_release);
				return false;
			}
			evaluationCancellation = evaluationCancellationSource.GetToken();
			frameLifecycleGenerations = lifecycleLatch->Capture();
			frameSceneGeneration = ObjectManager::instance().GetSceneGeneration();
			instances->CaptureFrameInputs(captureTick, capturedFrameInputs);
			commands->DrainThrough(captureTick, capturedCommands, *instances);
			try {
				capturedCommandStatuses.assign(capturedCommands.size(), AnimationStatus::NotReady);
				proceduralCommands->DrainThrough(
					captureTick, capturedProceduralCommands, *instances);
				capturedProceduralCommandStatuses.assign(
					capturedProceduralCommands.size(), AnimationStatus::NotReady);
				advancedCommands->DrainThrough(
					captureTick,
					capturedRetargetRequests,
					capturedMotionMatchRequests,
					capturedMotionMatchQueriesV2,
					capturedRagdollRequests,
					capturedDeterminismRequests);
				// Exact history restores happen only while runtime ownership is protected
				// and before worker-owned frame values are copied. No worker mutates live
				// playback, controller, parameter, or event state.
				PrepareDeterminismRequestsLocked();
				for(std::size_t index = 0U; index < capturedProceduralCommands.size(); ++index) {
					const NativeAnimationProceduralGoalCommand& command =
						capturedProceduralCommands[index];
					const AnimatorInstanceHandle handle{
						command.animator.slotIndex, command.animator.generation};
					const RuntimeRecord* record = FindRuntimeRecordLocked(handle);
					if(record == nullptr || !instances->IsAlive(handle)) {
						capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
						continue;
					}
					if(command.command == NativeAnimationProceduralCommandKind::ClearGoal) {
						capturedProceduralCommandStatuses[index] = AnimationStatus::Ok;
						continue;
					}
					const std::shared_ptr<const PublishedAnimatorPose> published =
						std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
					if(!published || !published->snapshot
						|| published->snapshot->publicationGeneration != command.expectedPoseGeneration
						|| published->authorityPublicationGeneration
							!= command.expectedPublicationGeneration) {
						capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
						continue;
					}
					const bool jointExists = std::find(
						record->binding.jointStableIds.begin(),
						record->binding.jointStableIds.end(),
						command.targetJointStableId) != record->binding.jointStableIds.end();
					capturedProceduralCommandStatuses[index] = jointExists
						? AnimationStatus::Ok : AnimationStatus::InvalidArgument;
				}
				std::vector<FrameWorkItem> candidate;
				candidate.reserve(capturedFrameInputs.size());
				for(const AnimationFrameInput& input : capturedFrameInputs) {
					const RuntimeRecord* record = FindRuntimeRecordLocked(input.handle);
					if(record == nullptr || record->poseGeneration == std::numeric_limits<AnimationGeneration>::max()) {
						return false;
					}
					FrameWorkItem item;
					item.input = input;
					item.binding = record->binding;
					item.playback = record->playback;
					item.eventStream = record->eventStream;
					item.dependencyStatus = record->dependencyStatus;
					item.previousExecutionModeState = record->executionModeState;
					item.parameters = record->parameters;
					item.layerWeights = record->layerWeights;
					item.capturedGraphState = record->graphState;
					item.candidateGraphState = record->graphState;
					item.transition = record->transition;
					item.activeRetargetProfileId = record->activeRetargetProfileId;
					item.activeRetargetProfileGeneration =
						record->activeRetargetProfileGeneration;
					item.retargetActive = item.binding.retargetProfile
						&& item.binding.compiledRetargetProfile
						&& item.binding.retargetSourceSkeleton
						&& item.binding.skeleton
						&& MatchesNativeUuid(item.activeRetargetProfileId,
							item.binding.retargetProfile->assetId.value)
						&& item.activeRetargetProfileGeneration
							== item.binding.retargetProfileGeneration;
					item.activeMotionDatabaseId = record->activeMotionDatabaseId;
					item.activeMotionDatabaseGeneration =
						record->activeMotionDatabaseGeneration;
					item.latestMotionQueryId = record->latestMotionQueryId;
					item.appliedMotionQueryId = record->appliedMotionQueryId;
					item.latestMotionCandidate = record->latestMotionCandidate;
					item.hasMotionCandidate = record->hasMotionCandidate;
					item.restoredFromHistory = std::any_of(
						determinismRequestCompletions.begin(),
						determinismRequestCompletions.end(),
						[&input](const DeterminismRequestCompletion& completion) {
							return completion.stateRestored
								&& completion.request.animator.slotIndex
									== input.handle.slotIndex
								&& completion.request.animator.generation
									== input.handle.generation;
						});
					for(const NativeAnimationMotionMatchRequest& request
						: capturedMotionMatchRequests) {
						if(request.operation == NativeAnimationMotionMatchOperation::Query
							&& request.animator.slotIndex == input.handle.slotIndex
							&& request.animator.generation == input.handle.generation) {
							item.pendingMotionQueries.push_back(request);
						}
					}
					for(const NativeAnimationMotionMatchQueryRequestV2& request
						: capturedMotionMatchQueriesV2) {
						if(request.animator.slotIndex == input.handle.slotIndex
							&& request.animator.generation == input.handle.generation) {
							item.pendingMotionQueriesV2.push_back(request);
						}
					}
					const NativeAnimationAnimatorHandle nativeHandle{
						input.handle.slotIndex, input.handle.generation};
					std::uint32_t proceduralGoalCount = 0U;
					AnimationStatus proceduralStatus = proceduralCommands->CopyActiveGoals(
						nativeHandle, {}, proceduralGoalCount);
					if(proceduralStatus == AnimationStatus::Ok
						|| proceduralStatus == AnimationStatus::BufferTooSmall) {
						item.proceduralStateObserved = true;
						item.activeProceduralGoals.resize(proceduralGoalCount);
						if(proceduralGoalCount != 0U) {
							proceduralStatus = proceduralCommands->CopyActiveGoals(
								nativeHandle, item.activeProceduralGoals, proceduralGoalCount);
							if(proceduralStatus != AnimationStatus::Ok) return false;
						}
					}
					item.ragdollGeneration = record->ragdollGeneration;
					item.ragdollLifecycle = record->ragdollLifecycle;
					item.ragdollBlendWeight = record->ragdollBlendWeight;
					item.ragdollLifecycleStartTick = record->ragdollLifecycleStartTick;
					item.ragdollRecoveryAlignedRootPosition =
						record->ragdollRecoveryAlignedRootPosition;
					item.ragdollRecoveryDurationSeconds =
						record->ragdollRecoveryDurationSeconds;
					if(record->ragdollGeneration.IsValid()
						&& (record->ragdollLifecycle == RagdollLifecycleState::Active
							|| record->ragdollLifecycle == RagdollLifecycleState::Recovering)) {
						item.ragdollPhysicsPose = Physics::PhysicsSystem::Instance()
							.LatestRagdollPhysicsPoseAtOrAfter(
								record->ragdollId, record->ragdollGeneration);
						if(item.ragdollPhysicsPose) {
							item.ragdollGeneration = item.ragdollPhysicsPose->generation;
						}
					}
					item.priorPublication = std::atomic_load_explicit(
						&record->publishedPose, std::memory_order_acquire);
					item.priorFallbackPublication = record->fallbackPublication.Load();
					item.capturedPoseGeneration = record->poseGeneration;
					item.capturedStateGeneration = record->stateGeneration;
					item.capturedParameterGeneration = record->parameterGeneration;
					item.capturedExecutionModeGeneration = record->executionModeGeneration;
					item.contentGeneration = record->contentGeneration;
					item.candidateStateGeneration = record->stateGeneration;
					item.candidateParameterGeneration = record->parameterGeneration;
					item.candidateExecutionModeGeneration = record->executionModeGeneration;
					item.capturedPlaybackState = record->playback.Snapshot();
					if(const std::optional<Object> object = ObjectManager::instance().getObject(
						input.descriptor.ownerObjectId)) {
						ObjectManager& objects = ObjectManager::instance();
						item.sourceObjectGeneration = TerraEngine::CompressObjectGeneration(
							objects.getObjectLifetimeGeneration(*object));
						item.sourceObjectWorldTransformCaptured = CaptureObjectWorldTransform(
							objects, *object, item.sourceObjectWorldTransform);
					}
					// Triggers are edge commands. Their copied snapshot may be observed for
					// one publication, but they never remain armed in a later frame.
					std::erase_if(item.parameters, [](const AnimationParameterSnapshot& value) {
						return value.type == AnimationParameterType::Trigger;
					});
					item.candidatePoseGeneration = record->poseGeneration + 1U;
					item.candidateAuthorityPublicationGeneration = candidateAuthorityGeneration;
					if(item.dependencyStatus == AnimationStatus::Ok
						&& item.binding.clip && item.binding.skeleton) {
						const bool sourceCompatible = item.retargetActive
							&& item.binding.retargetSourceSkeleton
							&& item.binding.clip->compatibilityFingerprint
								== item.binding.retargetSourceSkeleton->compatibilityFingerprint
							&& item.binding.clip->skeletonAssetId
								== item.binding.retargetSourceSkeleton->assetId;
						const SkeletonAsset* samplingSkeleton = sourceCompatible
							? item.binding.retargetSourceSkeleton.get()
							: item.binding.skeleton.get();
						item.sampledSourceSkeleton = sourceCompatible;
						item.dependencyStatus = item.clipView.Open(
							*item.binding.clip,
							samplingSkeleton,
							std::span<const std::uint8_t>(item.binding.residentSegments),
							item.binding.clipGeneration,
							item.binding.streamedSegmentPayloads);
					}
					candidate.push_back(std::move(item));
				}
				frameWork = std::move(candidate);
				frameTick = captureTick;
				return true;
			} catch(const std::bad_alloc&) {
				return false;
			}
		}

		static std::uint64_t MixWorkFingerprint(
			std::uint64_t hash,
			std::uint64_t value) noexcept
		{
			// FNV-1a over fixed-width values provides a platform-independent grouping
			// fingerprint. This is a compatibility identity, not a fabricated lifecycle
			// generation, and every evaluator/layout input is mixed below.
			for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
				hash ^= (value >> (byte * 8U)) & 0xffU;
				hash *= 1099511628211ULL;
			}
			return hash;
		}

		static std::uint64_t BuildOutputRequirementsFingerprint(
			const FrameWorkItem& item) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixWorkFingerprint(hash, item.resolvedConsumers.fingerprint);
			hash = MixWorkFingerprint(hash, item.binding.jointStableIds.size());
			hash = MixWorkFingerprint(hash, item.binding.morphStableIds.size());
			hash = MixWorkFingerprint(hash, item.binding.curveStableIds.size());
			for(const std::uint64_t stableId : item.binding.jointStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			for(const std::uint64_t stableId : item.binding.morphStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			for(const std::uint64_t stableId : item.binding.curveStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			return hash;
		}

		static std::uint64_t BuildResourceLayoutFingerprint(
			const FrameWorkItem& item) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixWorkFingerprint(hash, item.binding.gpuProgramResourceIndex);
			hash = MixWorkFingerprint(hash, item.binding.residentSegments.size());
			for(const std::uint8_t resident : item.binding.residentSegments) {
				hash = MixWorkFingerprint(hash, resident);
			}
			if(item.binding.streamedSegmentPayloads) {
				const AnimationClipSegmentPayloadSet& payloads =
					*item.binding.streamedSegmentPayloads;
				hash = MixWorkFingerprint(hash, payloads.generation);
				hash = MixWorkFingerprint(hash, payloads.publicationSequence);
				hash = MixWorkFingerprint(hash, payloads.segments.size());
				for(const AnimationClipSegmentPayload& payload : payloads.segments) {
					hash = MixWorkFingerprint(hash, payload.segmentIndex);
					hash = MixWorkFingerprint(hash, payload.streamOffset);
					hash = MixWorkFingerprint(hash, payload.streamSize);
					hash = MixWorkFingerprint(hash, payload.contentFingerprint);
				}
			} else {
				hash = MixWorkFingerprint(hash, 0U);
			}
			hash = MixWorkFingerprint(hash, item.binding.authoritativeJointIndices.size());
			hash = MixWorkFingerprint(hash, item.binding.gpuResidentPayloads ? 1U : 0U);
			hash = MixWorkFingerprint(hash, item.binding.clipGeneration);
			hash = MixWorkFingerprint(hash, item.binding.skeletonGeneration);
			hash = MixWorkFingerprint(hash, item.binding.programGeneration);
			hash = MixWorkFingerprint(hash, item.binding.retargetProfileGeneration);
			hash = MixWorkFingerprint(hash, item.binding.retargetSourceSkeletonGeneration);
			hash = MixWorkFingerprint(hash, item.binding.motionDatabaseGeneration);
			hash = MixWorkFingerprint(hash, item.retargetActive ? 1U : 0U);
			return hash;
		}

		static AnimationCompatibilityFingerprint BuildFallbackCompatibilityFingerprint(
			const AnimatorPlaybackBinding& binding) noexcept
		{
			if(binding.fallbackCompatibilityFingerprint != 0U) {
				return binding.fallbackCompatibilityFingerprint;
			}
			if(binding.skeleton && binding.skeleton->compatibilityFingerprint != 0U) {
				return binding.skeleton->compatibilityFingerprint;
			}

			// An unbound runtime shell has no skeleton fingerprint, but its empty or
			// partially declared pose layout still has a truthful compatibility identity.
			// Hash the copied layout/resource inputs so restart fixtures and authored
			// zero-channel animators can publish a typed safe pose without inventing a
			// lifecycle generation or accepting a differently shaped fallback later.
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixWorkFingerprint(hash, binding.jointStableIds.size());
			hash = MixWorkFingerprint(hash, binding.morphStableIds.size());
			hash = MixWorkFingerprint(hash, binding.curveStableIds.size());
			hash = MixWorkFingerprint(hash, binding.clipGeneration);
			hash = MixWorkFingerprint(hash, binding.skeletonGeneration);
			hash = MixWorkFingerprint(hash, binding.programGeneration);
			hash = MixWorkFingerprint(hash, binding.gpuProgramResourceIndex);
			for(const std::uint64_t stableId : binding.jointStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			for(const std::uint64_t stableId : binding.morphStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			for(const std::uint64_t stableId : binding.curveStableIds) {
				hash = MixWorkFingerprint(hash, stableId);
			}
			return hash;
		}

		static bool TryAddMemoryBytes(
			std::size_t count,
			std::size_t elementBytes,
			std::size_t& total) noexcept
		{
			if(count != 0U && elementBytes > std::numeric_limits<std::size_t>::max() / count) {
				return false;
			}
			const std::size_t bytes = count * elementBytes;
			if(bytes > std::numeric_limits<std::size_t>::max() - total) return false;
			total += bytes;
			return true;
		}

		static bool TryEstimatePoseBytes(
			const FrameWorkItem& item,
			std::size_t& total) noexcept
		{
			if(!TryAddMemoryBytes(
					1U,
					sizeof(PoseBuffer) + sizeof(AnimationSnapshot) + sizeof(PublishedAnimatorPose),
					total)
				|| !TryAddMemoryBytes(
					item.binding.jointStableIds.size(),
					sizeof(AnimationTransform),
					total)
				|| !TryAddMemoryBytes(
					item.binding.jointStableIds.size(),
					sizeof(AnimationTransform),
					total)
				|| !TryAddMemoryBytes(
					item.binding.morphStableIds.size(),
					sizeof(AnimationNamedFloatValue),
					total)
				|| !TryAddMemoryBytes(
					item.binding.curveStableIds.size(),
					sizeof(AnimationNamedFloatValue),
					total)) {
				return false;
			}
			if(item.binding.clip) {
				if(!TryAddMemoryBytes(
					item.binding.clip->events.size(), sizeof(AnimationEventRecord), total)) {
					return false;
				}
				for(const AnimationClipEvent& event : item.binding.clip->events) {
					if(!TryAddMemoryBytes(event.payload.size(), sizeof(char), total)) return false;
				}
			}
			return true;
		}

		static bool TryAdvanceGeneration(
			std::uint64_t current,
			bool changed,
			std::uint64_t& output) noexcept
		{
			output = current;
			if(!changed) return current != 0U;
			if(current == 0U || current == std::numeric_limits<std::uint64_t>::max()) return false;
			output = current + 1U;
			return true;
		}

		void ResolveUnchangedCacheReuse(FrameWorkItem& item) noexcept
		{
			const AnimationEvaluationExternalGenerations& external =
				item.binding.evaluationGenerations;
			if(item.evaluationStatus != AnimationStatus::Ok
				|| !item.activeProceduralGoals.empty()
				|| item.retargetActive
				|| item.activeMotionDatabaseGeneration != 0U
				|| item.ragdollLifecycle == RagdollLifecycleState::Active
				|| item.ragdollLifecycle == RagdollLifecycleState::Recovering
				|| !external.unchangedInputReuseEligible || !external.IsComplete()) {
				return;
			}
			const std::uint64_t consumerGeneration =
				item.resolvedConsumers.declarationGeneration != 0U
					? item.resolvedConsumers.declarationGeneration
					: external.consumerGeneration;

			item.cacheKey.animator = item.input.handle;
			item.cacheKey.evaluationTick = item.playback.AbsoluteTick();
			item.cacheKey.stateGeneration = item.candidateStateGeneration;
			item.cacheKey.parameterGeneration = item.candidateParameterGeneration;
			item.cacheKey.consumerGeneration = consumerGeneration;
			item.cacheKey.executionModeGeneration = item.candidateExecutionModeGeneration;
			item.cacheKey.poseGeneration = item.capturedPoseGeneration != 0U
				? item.capturedPoseGeneration
				: item.candidatePoseGeneration;
			item.cacheKey.morphGeneration = external.morphGeneration;
			item.cacheKey.graphGeneration = external.graphGeneration;
			item.cacheKey.viewGeneration = external.viewGeneration;
			item.cacheKey.assetGeneration = external.assetGeneration;
			item.cacheKey.residencyGeneration = external.residencyGeneration;
			item.cacheKey.materialGeneration = external.materialGeneration;
			item.cacheKey.rendererGeneration = external.rendererGeneration;
			item.cacheKey.physicsGeneration = external.physicsGeneration;
			item.cacheKey.deterministicGeneration = external.deterministicGeneration;
			item.cacheRequirements.visible = true;
			item.cacheRequirements.timeAdvances =
				item.advance.previousAbsoluteTick != item.advance.currentAbsoluteTick;
			item.cacheRequirements.events = item.cacheRequirements.timeAdvances
				&& item.binding.eventPolicy != AnimatorEventPolicy::Suppress;
			item.cacheRequirements.rootMotion = item.cacheRequirements.timeAdvances
				&& item.binding.rootMotionPolicy != AnimatorRootMotionPolicy::Ignore;
			item.cacheRequirements.physics = item.executionDecision.consumerClosure.physics;
			item.cacheRequirements.recording = item.executionDecision.consumerClosure.recording;
			item.cacheRequirements.replication = item.executionDecision.consumerClosure.networking;
			item.cacheRequirements.rollback = item.executionDecision.consumerClosure.rollback;
			item.cacheRequirements.deterministic = item.resolvedConsumers.deterministic;
			item.cacheRequirements.automation = item.executionDecision.consumerClosure.automation;
			item.cacheRequirements.detailedDiagnostics = item.executionDecision.consumerClosure.diagnostics;
			item.cacheEligible = true;
			if(item.capturedPoseGeneration == 0U || item.priorPublication == nullptr
				|| item.priorPublication->snapshot == nullptr
				|| item.priorPublication->snapshot->pose == nullptr) {
				return;
			}

			const AnimationEvaluationCacheLookup lookup = evaluationCache.Lookup(
				item.cacheKey, item.cacheRequirements);
			const std::uint64_t priorPoseGeneration =
				item.priorPublication->snapshot->publicationGeneration;
			if(lookup.disposition != AnimationEvaluationCacheDisposition::Reused
				|| lookup.value == nullptr
				|| lookup.value->poseToken != priorPoseGeneration
				|| lookup.value->publicationGeneration
					!= item.priorPublication->authorityPublicationGeneration) {
				return;
			}

			// Reuse retains the exact immutable prior generation. It advances neither
			// pose publication nor root motion/events, and the protected boundary will
			// therefore not duplicate last frame's authoritative side effects.
			item.cacheReused = true;
			item.candidatePoseGeneration = item.capturedPoseGeneration;
			item.executionDecision.nextState = item.previousExecutionModeState;
			item.publication = item.priorPublication;
			item.selectedPublication = item.priorPublication;
			item.evaluationStatus = item.priorPublication->status;
			item.safePose = item.priorPublication->safePose;
			item.evaluated = true;
			cacheReusedAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
		}

		const EvaluationState* FindControllerState(
			const AnimatorPlaybackBinding& binding,
			std::uint64_t stateId) const noexcept
		{
			if(!binding.evaluationProgram || stateId == 0U) return nullptr;
			const auto state = std::find_if(
				binding.evaluationProgram->states.begin(),
				binding.evaluationProgram->states.end(),
				[stateId](const EvaluationState& candidate) {
					return candidate.stableId == stateId;
				});
			return state == binding.evaluationProgram->states.end()
				? nullptr : std::addressof(*state);
		}

		const AnimatorControllerClipBinding* FindControllerClipForState(
			const AnimatorPlaybackBinding& binding,
			std::uint64_t stateId) const noexcept
		{
			const EvaluationState* state = FindControllerState(binding, stateId);
			if(state == nullptr || state->nodeOpcode != EvaluationOpcode::SampleClip) {
				return nullptr;
			}
			const auto clip = std::lower_bound(
				binding.controllerClips.begin(), binding.controllerClips.end(),
				state->nodeId,
				[](const AnimatorControllerClipBinding& candidate,
					std::uint64_t nodeId) {
					return candidate.nodeStableId < nodeId;
				});
			return clip == binding.controllerClips.end()
				|| clip->nodeStableId != state->nodeId
				? nullptr : std::addressof(*clip);
		}

		AnimationStatus BeginControllerTransition(
			FrameWorkItem& item) noexcept
		{
			const StateMachineSnapshot& graph = item.candidateGraphState;
			if(graph.activeTransitionStableId == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			if(item.transition.ready
				&& item.transition.transitionStableId
					== graph.activeTransitionStableId
				&& item.transition.sourceStateId
					== graph.transitionSourceStateId
				&& item.transition.destinationStateId
					== graph.transitionDestinationStateId
				&& !item.restoredFromHistory) {
				return AnimationStatus::Ok;
			}

			const AnimatorControllerClipBinding* destination =
				FindControllerClipForState(
					item.binding, graph.transitionDestinationStateId);
			if(destination == nullptr || !destination->clip
				|| destination->clipGeneration == 0U
				|| !item.binding.skeleton) {
				// Non-clip graph nodes retain their existing evaluator path. This
				// transition helper never guesses that an arbitrary dependency is a
				// clip merely because a state references it.
				item.transition = {};
				return AnimationStatus::Ok;
			}

			ControllerTransitionRuntime candidate;
			candidate.transitionStableId = graph.activeTransitionStableId;
			candidate.sourceStateId = graph.transitionSourceStateId;
			candidate.destinationStateId = graph.transitionDestinationStateId;
			try {
				candidate.destination = *destination;
				const bool sourceCompatible = item.retargetActive
					&& item.binding.retargetSourceSkeleton
					&& destination->clip->compatibilityFingerprint
						== item.binding.retargetSourceSkeleton
							->compatibilityFingerprint
					&& destination->clip->skeletonAssetId
						== item.binding.retargetSourceSkeleton->assetId;
				const SkeletonAsset* samplingSkeleton = sourceCompatible
					? item.binding.retargetSourceSkeleton.get()
					: item.binding.skeleton.get();
				AnimationStatus status = candidate.destinationClipView.Open(
					*destination->clip,
					samplingSkeleton,
					destination->residentSegments,
					destination->clipGeneration,
					destination->streamedSegmentPayloads);
				if(status != AnimationStatus::Ok) return status;
				status = candidate.destinationEventStream.Configure(
					{destination->clip->assetId, destination->clipGeneration},
					destination->clip->durationTicks,
					destination->clip->events);
				if(status != AnimationStatus::Ok) return status;

				AnimatorPlaybackConfiguration configuration =
					item.playback.Configuration();
				configuration.durationTicks = destination->clip->durationTicks;
				status = candidate.destinationPlayback.Configure(configuration);
				if(status != AnimationStatus::Ok) return status;
				const long double offset =
					static_cast<long double>(configuration.durationTicks)
					* static_cast<long double>(
						graph.retainedDestinationOffsetNormalized);
				AnimationFixedTick destinationTick =
					static_cast<AnimationFixedTick>(std::llround(std::clamp(
						offset,
						0.0L,
						static_cast<long double>(
							configuration.durationTicks))));
				// Rollback can restore an already-active transition without its
				// transient destination cursor. Rebuild the deterministic elapsed
				// position from the graph tick before the restored frame republishes.
				if(item.restoredFromHistory
					|| item.capturedGraphState.activeTransitionStableId
						== graph.activeTransitionStableId) {
					const long double elapsedValue =
						std::max(
							0.0L,
							static_cast<long double>(graph.lastTick)
								- static_cast<long double>(
									graph.transitionStartTick));
					if(elapsedValue > static_cast<long double>(
						std::numeric_limits<AnimationFixedTick>::max())) {
						return AnimationStatus::RangeOverflow;
					}
					const AnimationFixedTick elapsed =
						static_cast<AnimationFixedTick>(elapsedValue);
					const long double reconstructed =
						static_cast<long double>(destinationTick)
						+ static_cast<long double>(
							configuration.reverse ? -elapsed : elapsed);
					if(reconstructed
							< static_cast<long double>(
								std::numeric_limits<AnimationFixedTick>::min())
						|| reconstructed
							> static_cast<long double>(
								std::numeric_limits<AnimationFixedTick>::max())) {
						return AnimationStatus::RangeOverflow;
					}
					destinationTick =
						static_cast<AnimationFixedTick>(reconstructed);
				}
				status = candidate.destinationPlayback.Seek(destinationTick);
				if(status != AnimationStatus::Ok) return status;
				const AnimatorPlaybackRunState sourceRunState =
					item.playback.RunState();
				if(sourceRunState == AnimatorPlaybackRunState::Playing
					|| sourceRunState == AnimatorPlaybackRunState::Paused
					|| sourceRunState == AnimatorPlaybackRunState::Completed) {
					status = candidate.destinationPlayback.Play();
					if(status == AnimationStatus::Ok
						&& sourceRunState == AnimatorPlaybackRunState::Paused) {
						status = candidate.destinationPlayback.Pause();
					}
					if(status != AnimationStatus::Ok) return status;
				}

				const std::uint64_t priorTransition =
					item.capturedGraphState.activeTransitionStableId;
				if(priorTransition != 0U
					&& graph.interruptedTransitionStableId == priorTransition
					&& item.priorPublication
					&& item.priorPublication->snapshot
					&& item.priorPublication->snapshot->pose) {
					const std::span<const AnimationTransform> priorPose =
						item.priorPublication->snapshot->pose->LocalTransforms();
					if(priorPose.size() == item.binding.jointStableIds.size()) {
						candidate.capturedSourcePose =
							std::make_shared<const std::vector<AnimationTransform>>(
								priorPose.begin(), priorPose.end());
					}
					if(item.priorPublication->snapshot->pose
						->RootMotionDelta()) {
						candidate.capturedSourceRootMotion =
							*item.priorPublication->snapshot->pose
								->RootMotionDelta();
					}
				}
				candidate.ready = true;
				item.transition = std::move(candidate);
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		void PromoteCompletedControllerTransition(
			FrameWorkItem& item) noexcept
		{
			const bool completed = item.transition.ready
				&& item.capturedGraphState.activeTransitionStableId
					== item.transition.transitionStableId
				&& item.candidateGraphState.activeTransitionStableId == 0U
				&& item.candidateGraphState.activeStateId
					== item.transition.destinationStateId;
			if(!completed) {
				item.transition = {};
				return;
			}

			// The destination cursor has advanced independently for every blend tick.
			// Promote the complete destination bundle together instead of reopening
			// the clip at its authored offset and visibly rewinding on completion.
			item.binding.clip = item.transition.destination.clip;
			item.binding.clipGeneration =
				item.transition.destination.clipGeneration;
			item.binding.clipDeterministicContentFingerprint =
				item.transition.destination.deterministicContentFingerprint;
			item.binding.residentSegments =
				item.transition.destination.residentSegments;
			item.binding.streamedSegmentPayloads =
				item.transition.destination.streamedSegmentPayloads;
			item.playback = item.transition.destinationPlayback;
			item.clipView = item.transition.destinationClipView;
			item.eventStream = item.transition.destinationEventStream;
			item.sampledSourceSkeleton = item.retargetActive
				&& item.binding.retargetSourceSkeleton
				&& item.binding.clip
				&& item.binding.clip->compatibilityFingerprint
					== item.binding.retargetSourceSkeleton
						->compatibilityFingerprint
				&& item.binding.clip->skeletonAssetId
					== item.binding.retargetSourceSkeleton->assetId;
			item.transition = {};
		}

		AnimationStatus SynchronizeControllerTransition(
			FrameWorkItem& item) noexcept
		{
			if(item.candidateGraphState.activeTransitionStableId != 0U) {
				return BeginControllerTransition(item);
			}
			PromoteCompletedControllerTransition(item);
			return AnimationStatus::Ok;
		}

		AnimationStatus SelectControllerClipForState(
			FrameWorkItem& item,
			std::uint64_t stateId) noexcept
		{
			if(!item.binding.evaluationProgram || stateId == 0U) {
				return AnimationStatus::InvalidArgument;
			}
			const auto state = std::find_if(
				item.binding.evaluationProgram->states.begin(),
				item.binding.evaluationProgram->states.end(),
				[stateId](const EvaluationState& candidate) {
					return candidate.stableId == stateId;
				});
			if(state == item.binding.evaluationProgram->states.end()) {
				return AnimationStatus::MissingAsset;
			}
			// Non-clip nodes own dedicated graph evaluation stages. Keep the current
			// sampled base pose until that stage consumes it; never reinterpret an
			// arbitrary dependency as a clip merely because a state selected it.
			if(state->nodeOpcode != EvaluationOpcode::SampleClip) {
				return AnimationStatus::Ok;
			}
			const auto clip = std::lower_bound(
				item.binding.controllerClips.begin(), item.binding.controllerClips.end(),
				state->nodeId,
				[](const AnimatorControllerClipBinding& candidate, std::uint64_t nodeId) {
					return candidate.nodeStableId < nodeId;
				});
			if(clip == item.binding.controllerClips.end()
				|| clip->nodeStableId != state->nodeId || !clip->clip
				|| clip->clipGeneration == 0U || !item.binding.skeleton) {
				return AnimationStatus::MissingAsset;
			}
			if(item.binding.clip == clip->clip
				&& item.binding.clipGeneration == clip->clipGeneration) {
				return AnimationStatus::Ok;
			}

			const bool sourceCompatible = item.retargetActive
				&& item.binding.retargetSourceSkeleton
				&& clip->clip->compatibilityFingerprint
					== item.binding.retargetSourceSkeleton->compatibilityFingerprint
				&& clip->clip->skeletonAssetId
					== item.binding.retargetSourceSkeleton->assetId;
			const SkeletonAsset* samplingSkeleton = sourceCompatible
				? item.binding.retargetSourceSkeleton.get()
				: item.binding.skeleton.get();
			AnimationClipView nextView;
			AnimationStatus status = nextView.Open(
				*clip->clip,
				samplingSkeleton,
				clip->residentSegments,
				clip->clipGeneration,
				clip->streamedSegmentPayloads);
			if(status != AnimationStatus::Ok) return status;
			AnimationEventStream nextEvents;
			status = nextEvents.Configure(
				{clip->clip->assetId, clip->clipGeneration},
				clip->clip->durationTicks, clip->clip->events);
			if(status != AnimationStatus::Ok) return status;

			const AnimatorPlaybackRunState priorRunState = item.playback.RunState();
			AnimatorPlaybackConfiguration configuration = item.playback.Configuration();
			configuration.durationTicks = clip->clip->durationTicks;
			AnimatorPlayback nextPlayback;
			status = nextPlayback.Configure(configuration);
			if(status != AnimationStatus::Ok) return status;
			const long double offset = static_cast<long double>(configuration.durationTicks)
				* static_cast<long double>(
					item.candidateGraphState.retainedDestinationOffsetNormalized);
			const AnimationFixedTick destinationTick = static_cast<AnimationFixedTick>(
				std::clamp<long double>(offset, 0.0L,
					static_cast<long double>(configuration.durationTicks)));
			status = nextPlayback.Seek(destinationTick);
			if(status != AnimationStatus::Ok) return status;
			if(priorRunState == AnimatorPlaybackRunState::Playing
				|| priorRunState == AnimatorPlaybackRunState::Paused) {
				status = nextPlayback.Play();
				if(status == AnimationStatus::Ok
					&& priorRunState == AnimatorPlaybackRunState::Paused) {
					status = nextPlayback.Pause();
				}
				if(status != AnimationStatus::Ok) return status;
			}

			item.binding.clip = clip->clip;
			item.binding.clipGeneration = clip->clipGeneration;
			item.binding.clipDeterministicContentFingerprint =
				clip->deterministicContentFingerprint;
			item.binding.residentSegments = clip->residentSegments;
			item.binding.streamedSegmentPayloads = clip->streamedSegmentPayloads;
			item.sampledSourceSkeleton = sourceCompatible;
			item.clipView = std::move(nextView);
			item.eventStream = std::move(nextEvents);
			item.playback = nextPlayback;
			return AnimationStatus::Ok;
		}

		AnimationStatus ResolveRequestedStateLeaf(
			const EvaluationProgram& program,
			std::uint64_t requestedStateId,
			std::uint64_t& outputLeaf) const noexcept
		{
			const EvaluationState* current = nullptr;
			for(const EvaluationState& state : program.states) {
				if(state.stableId == requestedStateId) {
					current = &state;
					break;
				}
			}
			if(current == nullptr || current->anyState || current->exit) {
				return AnimationStatus::InvalidArgument;
			}

			// A command may name a state-machine scope. Resolve its single authored
			// entry chain exactly as ordinary state-machine evaluation does.
			for(std::size_t depth = 0U;
				depth <= GetAnimationRuntimeLimit(AnimationLimit::StateMachineDepth).hardMaximum;
				++depth) {
				const EvaluationState* entryChild = nullptr;
				for(const EvaluationState& candidate : program.states) {
					if(candidate.parentStateId != current->stableId || !candidate.entry) continue;
					if(entryChild != nullptr) return AnimationStatus::Rejected;
					entryChild = &candidate;
				}
				if(entryChild == nullptr) {
					outputLeaf = current->stableId;
					return AnimationStatus::Ok;
				}
				if(entryChild->anyState || entryChild->exit) return AnimationStatus::Rejected;
				current = entryChild;
			}
			return AnimationStatus::HardLimitExceeded;
		}

		std::uint64_t MakeCommandTransitionId(
			const AnimationCommand& command,
			std::uint64_t sourceStateId,
			std::uint64_t destinationStateId) const noexcept
		{
			// Keep explicit gameplay transitions in a disjoint high-bit namespace.
			// The remaining bits are deterministic from copied command identities.
			std::uint64_t hash = 1469598103934665603ULL;
			for(const std::uint64_t value : {
				sourceStateId, destinationStateId, command.producerSequence,
				command.correlationId, static_cast<std::uint64_t>(command.targetTick)}) {
				hash ^= value;
				hash *= 1099511628211ULL;
			}
			return (hash | (1ULL << 63U) | 1ULL);
		}

		AnimationStatus ApplyGraphControlCommand(
			FrameWorkItem& item,
			const AnimationCommand& command) noexcept
		{
			if(command.kind == AnimationCommandKind::SetLayerWeight) {
				if(command.layerIndex >= item.layerWeights.size()
					|| !std::isfinite(command.layerWeight)
					|| command.layerWeight < 0.0F || command.layerWeight > 1.0F) {
					return AnimationStatus::InvalidArgument;
				}
				item.layerWeights[command.layerIndex] = command.layerWeight;
				return AnimationStatus::Ok;
			}
			if(command.kind == AnimationCommandKind::Play
				&& command.stateId == 0U) {
				const AnimationStatus seekStatus = item.playback.Seek(command.playbackTick);
				return seekStatus == AnimationStatus::Ok
					? item.playback.Play() : seekStatus;
			}
			if(command.kind == AnimationCommandKind::Play
				&& !item.binding.evaluationProgram) {
				if(command.stateId != item.binding.stateId) {
					return AnimationStatus::InvalidArgument;
				}
				const AnimationStatus seekStatus = item.playback.Seek(command.playbackTick);
				return seekStatus == AnimationStatus::Ok
					? item.playback.Play() : seekStatus;
			}
			if((command.kind != AnimationCommandKind::Play
					&& command.kind != AnimationCommandKind::CrossFade)
				|| !item.binding.evaluationProgram) {
				return AnimationStatus::NotReady;
			}

			std::uint64_t destinationLeaf = 0U;
			const AnimationStatus resolveStatus = ResolveRequestedStateLeaf(
				*item.binding.evaluationProgram, command.stateId, destinationLeaf);
			if(resolveStatus != AnimationStatus::Ok) return resolveStatus;
			try {
				if(command.kind == AnimationCommandKind::Play
					|| command.transitionTicks == 0
					|| item.candidateGraphState.activeStateId == 0U
					|| item.candidateGraphState.activeStateId == destinationLeaf) {
					StateMachineSnapshot next = item.candidateGraphState;
					next.activeStateId = destinationLeaf;
					next.stateEnteredTick = item.input.tick;
					next.activeTransitionStableId = 0U;
					next.transitionSourceStateId = 0U;
					next.transitionDestinationStateId = 0U;
					next.transitionStartTick = 0;
					next.transitionDurationTicks = 0;
					next.transitionProgressNormalized = 0.0F;
					next.retainedDestinationOffsetNormalized = 0.0F;
					next.transitionDestinationEnteredTick = 0;
					next.interruptedTransitionStableId = 0U;
					next.interruptedTransitionProgressNormalized = 0.0F;
					next.retainedTriggerEdges.clear();
					next.syncHistory.clear();
					next.lastTick = item.input.tick;
					item.candidateGraphState = std::move(next);
					item.binding.stateId = destinationLeaf;
					// An instant Play or zero-duration CrossFade replaces any
					// in-flight blend. Clear its private destination cursor before
					// synchronization so it cannot overwrite the newly requested
					// state and playback tick during completion promotion.
					item.transition = {};
					const AnimationStatus clipStatus = SelectControllerClipForState(
						item, destinationLeaf);
					if(clipStatus != AnimationStatus::Ok) return clipStatus;
					const AnimationStatus seekStatus = item.playback.Seek(
						command.kind == AnimationCommandKind::Play
							? command.playbackTick : 0);
					return seekStatus == AnimationStatus::Ok
						? item.playback.Play() : seekStatus;
				}

				StateMachineSnapshot next = item.candidateGraphState;
				next.interruptedTransitionStableId = next.activeTransitionStableId;
				next.interruptedTransitionProgressNormalized =
					next.transitionProgressNormalized;
				next.activeTransitionStableId = MakeCommandTransitionId(
					command, next.activeStateId, destinationLeaf);
				next.transitionSourceStateId = next.activeStateId;
				next.transitionDestinationStateId = destinationLeaf;
				next.transitionStartTick = item.input.tick;
				next.transitionDurationTicks = command.transitionTicks;
				next.transitionProgressNormalized = 0.0F;
				next.retainedDestinationOffsetNormalized = 0.0F;
				next.transitionDestinationEnteredTick = item.input.tick;
				next.lastTick = item.input.tick;
				next.transitionHistory.push_back({item.input.tick,
					next.activeTransitionStableId, next.transitionSourceStateId,
					destinationLeaf});
				const std::size_t historyLimit = static_cast<std::size_t>(
					GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);
				if(next.transitionHistory.size() > historyLimit) {
					next.transitionHistory.erase(next.transitionHistory.begin(),
						next.transitionHistory.begin()
							+ static_cast<std::ptrdiff_t>(next.transitionHistory.size() - historyLimit));
				}
				item.candidateGraphState = std::move(next);
				item.binding.stateId = item.candidateGraphState.activeStateId;
				return item.playback.Play();
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus ApplyRetainedMotionSelection(
			FrameWorkItem& item,
			bool& changed) noexcept
		{
			changed = false;
			if(!item.hasMotionCandidate || item.latestMotionQueryId == 0U
				|| item.latestMotionQueryId == item.appliedMotionQueryId) {
				return AnimationStatus::Ok;
			}
			// The explicit runtime query API is also useful for observation and AI
			// planning on a direct-clip animator. Without an authored controller there
			// is no motion node/state to drive, so retain the copied result for
			// diagnostics without turning the otherwise valid animator into a failure.
			if(!item.binding.evaluationProgram) return AnimationStatus::Ok;
			if(!item.binding.motionDatabase
				|| item.activeMotionDatabaseGeneration == 0U
				|| item.activeMotionDatabaseGeneration
					!= item.binding.motionDatabaseGeneration) {
				return AnimationStatus::NotReady;
			}

			const auto state = std::find_if(
				item.binding.evaluationProgram->states.begin(),
				item.binding.evaluationProgram->states.end(),
				[&item](const EvaluationState& candidate) {
					return candidate.nodeOpcode == EvaluationOpcode::SampleClip
						&& candidate.nodeId == item.latestMotionCandidate.sourceClipId;
				});
			if(state == item.binding.evaluationProgram->states.end()) {
				return AnimationStatus::MissingAsset;
			}
			try {
				StateMachineSnapshot next = item.candidateGraphState;
				const std::uint64_t priorState = next.activeStateId;
				next.activeStateId = state->stableId;
				next.stateEnteredTick = item.input.tick;
				next.activeTransitionStableId = 0U;
				next.transitionSourceStateId = 0U;
				next.transitionDestinationStateId = 0U;
				next.transitionStartTick = 0;
				next.transitionDurationTicks = 0;
				next.transitionProgressNormalized = 0.0F;
				next.retainedDestinationOffsetNormalized = 0.0F;
				next.transitionDestinationEnteredTick = 0;
				next.interruptedTransitionStableId = 0U;
				next.interruptedTransitionProgressNormalized = 0.0F;
				next.lastTick = item.input.tick;
				if(priorState != 0U && priorState != state->stableId) {
					const std::uint64_t decisionId = item.latestMotionCandidate.stableSampleId
						| (1ULL << 62U) | 1ULL;
					next.transitionHistory.push_back({item.input.tick, decisionId,
						priorState, state->stableId});
					// Motion matching may choose a new source every frame. Keep the
					// diagnostic history bounded by the same retained-event limit used
					// by authored transitions so a long-running animator cannot grow
					// this worker-owned vector without limit.
					const std::size_t historyLimit = static_cast<std::size_t>(
						GetAnimationRuntimeLimit(AnimationLimit::AnimatorRetainedEvents).hardMaximum);
					if(next.transitionHistory.size() > historyLimit) {
						next.transitionHistory.erase(next.transitionHistory.begin(),
							next.transitionHistory.begin() + static_cast<std::ptrdiff_t>(
								next.transitionHistory.size() - historyLimit));
					}
				}
				item.candidateGraphState = std::move(next);
				item.binding.stateId = state->stableId;
				const AnimationStatus clipStatus = SelectControllerClipForState(
					item, state->stableId);
				if(clipStatus != AnimationStatus::Ok) return clipStatus;
				const AnimationStatus seekStatus = item.playback.Seek(
					item.latestMotionCandidate.sourceTick);
				if(seekStatus != AnimationStatus::Ok) return seekStatus;
				const AnimationStatus playStatus = item.playback.Play();
				if(playStatus != AnimationStatus::Ok) return playStatus;
				item.appliedMotionQueryId = item.latestMotionQueryId;
				changed = true;
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		void PlanRange(
			std::size_t begin,
			std::size_t end,
			const AnimatorPlaybackTimeInput& timeInput) noexcept
		{
			if(PollFrameCancellation()) return;
			const std::size_t boundedEnd = std::min(end, frameWork.size());
			for(std::size_t index = begin; index < boundedEnd; ++index) {
				if(PollFrameCancellation()) return;
				FrameWorkItem& item = frameWork[index];
				AnimationStatus commandStatus = AnimationStatus::Ok;
				bool stateCommandApplied = false;
				bool parameterCommandApplied = false;
				bool directStateOverrideQueued = false;
				std::vector<StateMachineTriggerEdge> triggerEdges;
				for(std::size_t commandIndex = 0U; commandIndex < capturedCommands.size(); ++commandIndex) {
					const AnimationCommand& command = capturedCommands[commandIndex];
					if(command.target != item.input.handle) continue;
					if(command.kind == AnimationCommandKind::Play
						|| command.kind == AnimationCommandKind::CrossFade
						|| command.kind == AnimationCommandKind::SetLayerWeight) {
						directStateOverrideQueued = directStateOverrideQueued
							|| command.kind == AnimationCommandKind::Play
							|| command.kind == AnimationCommandKind::CrossFade;
						continue;
					}
					AnimationStatus current = command.kind == AnimationCommandKind::SetParameter
						|| command.kind == AnimationCommandKind::FireTrigger
						? ApplyParameterCommand(item.parameters, command)
						: ApplyPlaybackCommand(item.playback, command);
					if(current == AnimationStatus::Ok
						&& command.kind == AnimationCommandKind::FireTrigger) {
						if(command.parameterId == 0U || command.producerSequence == 0U) {
							current = AnimationStatus::InvalidArgument;
						} else {
							try {
								triggerEdges.push_back({
									command.parameterId, command.producerSequence, true});
							} catch(const std::bad_alloc&) {
								current = AnimationStatus::InternalError;
							}
						}
					}
					capturedCommandStatuses[commandIndex] = current;
					if(current == AnimationStatus::Ok) {
						if(command.kind == AnimationCommandKind::SetParameter
							|| command.kind == AnimationCommandKind::FireTrigger) {
							parameterCommandApplied = true;
						} else {
							stateCommandApplied = true;
						}
					}
					if(current != AnimationStatus::Ok && commandStatus == AnimationStatus::Ok) {
						commandStatus = current;
					}
				}
				const AnimationStatus preGraphCommandStatus = commandStatus;
				if(commandStatus == AnimationStatus::Ok && item.binding.evaluationProgram
					&& !item.restoredFromHistory) {
					StateMachineStepInput graphInput;
					graphInput.tick = item.input.tick;
					graphInput.triggerEdges = triggerEdges;
					StateMachineSnapshot nextGraphState;
					const AnimationStatus graphStatus = StepStateMachineTrusted(
						*item.binding.evaluationProgram,
						item.capturedGraphState,
						graphInput,
						nextGraphState);
					if(graphStatus == AnimationStatus::Ok) {
						stateCommandApplied = stateCommandApplied
							|| nextGraphState != item.capturedGraphState;
						item.candidateGraphState = std::move(nextGraphState);
						item.binding.stateId = item.candidateGraphState.activeStateId;
						const AnimationStatus clipStatus = SelectControllerClipForState(
							item, item.binding.stateId);
						if(clipStatus != AnimationStatus::Ok) commandStatus = clipStatus;
					} else if(!directStateOverrideQueued) {
						commandStatus = graphStatus;
					}
				}
				// Direct state and layer commands intentionally run after the ordinary
				// graph step. A Play/CrossFade can recover an otherwise uninitialized
				// graph, while parameter validation failures still reject the command set.
				if(preGraphCommandStatus == AnimationStatus::Ok) {
					for(std::size_t commandIndex = 0U;
						commandIndex < capturedCommands.size(); ++commandIndex) {
						const AnimationCommand& command = capturedCommands[commandIndex];
						if(command.target != item.input.handle
							|| (command.kind != AnimationCommandKind::Play
								&& command.kind != AnimationCommandKind::CrossFade
								&& command.kind != AnimationCommandKind::SetLayerWeight)) continue;
						const bool layerChanged = command.kind == AnimationCommandKind::SetLayerWeight
							&& command.layerIndex < item.layerWeights.size()
							&& item.layerWeights[command.layerIndex] != command.layerWeight;
						const AnimationStatus current = ApplyGraphControlCommand(item, command);
						capturedCommandStatuses[commandIndex] = current;
						if(current == AnimationStatus::Ok) {
							stateCommandApplied = stateCommandApplied
								|| command.kind != AnimationCommandKind::SetLayerWeight
								|| layerChanged;
							if(command.kind == AnimationCommandKind::Play
								|| command.kind == AnimationCommandKind::CrossFade) {
								commandStatus = AnimationStatus::Ok;
							}
						} else if(commandStatus == AnimationStatus::Ok
							|| directStateOverrideQueued) {
							commandStatus = current;
						}
					}
				}
				if(commandStatus == AnimationStatus::Ok) {
					bool motionSelectionChanged = false;
					const AnimationStatus motionSelectionStatus =
						ApplyRetainedMotionSelection(item, motionSelectionChanged);
					if(motionSelectionStatus != AnimationStatus::Ok) {
						commandStatus = motionSelectionStatus;
					} else {
						stateCommandApplied = stateCommandApplied || motionSelectionChanged;
					}
				}
				if(commandStatus == AnimationStatus::Ok) {
					const AnimationStatus transitionStatus =
						SynchronizeControllerTransition(item);
					if(transitionStatus != AnimationStatus::Ok) {
						commandStatus = transitionStatus;
					}
				}
				AnimatorPlaybackTimeInput effectiveTime = timeInput;
				if(item.binding.updatePolicy == AnimatorUpdatePolicy::Manual
					|| item.restoredFromHistory) {
					// Manual animators still consume explicit commands and publish their
					// resulting snapshot, but automatic frame time never advances them. A
					// restored animator likewise republishes the exact retained fixed-point
					// state once before ordinary advancement resumes.
					effectiveTime = {};
				}
				const AnimationStatus advanceStatus = item.playback.Advance(effectiveTime, item.advance);
				AnimationStatus transitionAdvanceStatus = AnimationStatus::Ok;
				if(item.candidateGraphState.activeTransitionStableId != 0U
					&& item.transition.ready) {
					transitionAdvanceStatus =
						item.transition.destinationPlayback.Advance(
							effectiveTime, item.transition.destinationAdvance);
				}
				item.evaluationStatus = commandStatus != AnimationStatus::Ok
					? commandStatus
					: advanceStatus != AnimationStatus::Ok
						? advanceStatus : transitionAdvanceStatus;
				const bool playbackChanged = stateCommandApplied
					|| item.playback.Snapshot() != item.capturedPlaybackState;
				if(!TryAdvanceGeneration(item.capturedStateGeneration, playbackChanged,
					item.candidateStateGeneration)
					|| !TryAdvanceGeneration(item.capturedParameterGeneration, parameterCommandApplied,
						item.candidateParameterGeneration)) {
					item.evaluationStatus = AnimationStatus::RangeOverflow;
				}
				// A planning failure is still a complete worker result. Evaluation selects a
				// prior or bind-pose fallback instead of stalling the whole frame forever.
				item.planned = true;
				// Mode policy is resolved entirely from this copied frame item.  The
				// resulting next state stays private until the all-or-none authority
				// publication below, so a rejected frame cannot advance hysteresis.
				AnimationExecutionModeRequest modeRequest;
				modeRequest.fixedTick = item.input.tick;
				modeRequest.requestedPolicy = item.binding.requestedExecutionPolicy;
				modeRequest.headless = headlessExecutionRequired;
				item.resolvedConsumers = AnimationConsumerRegistry::Instance().Resolve(
					item.input.handle, item.binding.jointParents, item.input.tick);
				const AnimationResolvedConsumerRequirements& declaredConsumers =
					item.resolvedConsumers;
				std::vector<std::uint32_t> authoritativeJoints = item.binding.authoritativeJointIndices;
				if(declaredConsumers.status == AnimationStatus::Ok) {
					authoritativeJoints.insert(authoritativeJoints.end(),
						declaredConsumers.closure.joints.begin(), declaredConsumers.closure.joints.end());
					std::sort(authoritativeJoints.begin(), authoritativeJoints.end());
					authoritativeJoints.erase(
						std::unique(authoritativeJoints.begin(), authoritativeJoints.end()),
						authoritativeJoints.end());
				}
				modeRequest.deterministicRequired = declaredConsumers.deterministic
					|| declaredConsumers.status != AnimationStatus::Ok;
				modeRequest.consumers.requestedJoints = authoritativeJoints;
				modeRequest.consumers.jointParents = item.binding.jointParents;
				modeRequest.consumers.curves = declaredConsumers.closure.curves;
				modeRequest.consumers.morphs = declaredConsumers.closure.morphs;
				modeRequest.consumers.events = declaredConsumers.closure.events;
				// Any authored mode that exposes or applies a delta is gameplay authority.
				// Force Hybrid/CPU closure even when no external consumer declaration was
				// published, otherwise GPU Visual would skip the root track entirely.
				modeRequest.consumers.rootMotion = declaredConsumers.closure.rootMotion
					|| item.binding.rootMotionPolicy != AnimatorRootMotionPolicy::Ignore;
				modeRequest.consumers.contacts = declaredConsumers.closure.contacts;
				modeRequest.consumers.proceduralResults = declaredConsumers.closure.proceduralResults
					|| !item.activeProceduralGoals.empty();
				modeRequest.consumers.physics = declaredConsumers.closure.physics
					|| item.ragdollLifecycle == RagdollLifecycleState::Active
					|| item.ragdollLifecycle == RagdollLifecycleState::Recovering;
				modeRequest.consumers.attachments = declaredConsumers.closure.attachments;
				modeRequest.consumers.networking = declaredConsumers.closure.networking;
				modeRequest.consumers.rollback = declaredConsumers.closure.rollback;
				modeRequest.consumers.replay = declaredConsumers.closure.replay;
				modeRequest.consumers.recording = declaredConsumers.closure.recording;
				modeRequest.consumers.automation = declaredConsumers.closure.automation;
				modeRequest.consumers.diagnostics = declaredConsumers.closure.diagnostics;
				modeRequest.consumers.completePose = declaredConsumers.closure.completePose
					|| item.retargetActive
					|| declaredConsumers.status != AnimationStatus::Ok;
				modeRequest.gpu = item.binding.gpuExecution;
				modeRequest.continuity.transitionBoundaryTick = item.input.tick;
				modeRequest.continuity.stateTimeTick = item.playback.AbsoluteTick();
				modeRequest.continuity.controllerState = item.binding.stateId;
				modeRequest.continuity.pose = item.candidatePoseGeneration;
				item.executionDecision = ResolveAnimationExecutionMode(
					modeRequest, item.previousExecutionModeState);
				auto forceCpuFallback = [&item](AnimationExecutionModeReason reason) noexcept {
					// This is a frame-local downgrade after the policy resolver has already
					// copied its continuity token. Keep the persisted mode/transition record
					// coherent so a rejected command cannot later look like GPU authority.
					const AnimationEffectiveExecutionMode previousMode =
						item.previousExecutionModeState.initialized
							? item.previousExecutionModeState.effectiveMode
							: AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.effectiveMode =
						AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.reason = reason;
					item.executionDecision.eligibleMode =
						AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.eligibilityReason = reason;
					item.executionDecision.modeChanged = previousMode
						!= AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.transition.occurred = item.executionDecision.modeChanged;
					item.executionDecision.transition.from = previousMode;
					item.executionDecision.transition.to =
						AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.transition.continuity =
						item.executionDecision.nextState.continuity;
					item.executionDecision.nextState.initialized = true;
					item.executionDecision.nextState.effectiveMode = item.executionDecision.effectiveMode;
					item.executionDecision.nextState.effectiveReason = item.executionDecision.reason;
					item.executionDecision.nextState.modeEnteredTick = item.input.tick;
					item.executionDecision.nextState.promotionPending = false;
					item.executionDecision.nextState.promotionMode =
						AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					item.executionDecision.nextState.promotionReason = reason;
					item.executionDecision.nextState.promotionSinceTick = 0;
					item.executionDecision.nextState.promotionFingerprint = 0U;
				};
				if(item.evaluationStatus != AnimationStatus::Ok) {
					// Command/clock failures predate evaluation. They must never publish a
					// GPU-only pose, because no authoritative CPU fallback has been built
					// yet and the published status is already non-OK.
					forceCpuFallback(AnimationExecutionModeReason::GpuEvaluationFailure);
				}
				if(item.candidateGraphState.activeTransitionStableId != 0U
					&& item.transition.ready) {
					// The current GPU packet ABI names only one clip cursor. Until its
					// append-only layout carries both transition inputs, publish the
					// complete CPU-blended pose through the existing safe fallback path.
					// This is an explicit capability downgrade, never a silent one-clip
					// approximation that would hold and snap at transition completion.
					forceCpuFallback(
						AnimationExecutionModeReason::GpuCapabilityUnavailable);
				}
				if(item.planned && (item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::GpuVisual
					|| item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::HybridAuthoritative)) {
					GpuAnimationLoweringInput gpuInput;
					gpuInput.program.program = item.binding.evaluationProgram;
					gpuInput.program.programContentId = item.binding.evaluationProgram
						? item.binding.evaluationProgram->stableHash : 0U;
					gpuInput.program.programResourceIndex = item.binding.gpuProgramResourceIndex;
					gpuInput.visual = item.binding.gpuVisual;
					// The handle generation is part of the stable producer identity; a
					// reused slot therefore cannot share a packet with stale work.
					gpuInput.visual.stableInstanceId = (static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U)
						| static_cast<std::uint64_t>(item.input.handle.generation);
					gpuInput.visual.stableSubmissionOrdinal = item.input.handle.slotIndex;
					gpuInput.visual.jointCount = static_cast<std::uint32_t>(item.binding.jointStableIds.size());
					// The binding coordinator orders GPU morph lanes against the
					// deformable mesh table. Preserve that proven count instead of
					// rebuilding it from clip-facing named-output storage here.
					gpuInput.visual.generations.instanceSlot = item.input.handle.slotIndex;
					gpuInput.visual.generations.instanceGeneration = item.input.handle.generation;
					gpuInput.visual.generations.componentGeneration = item.input.descriptor.componentGeneration;
					gpuInput.visual.generations.workGeneration = item.candidatePoseGeneration;
					gpuInput.visual.generations.programGeneration = item.binding.programGeneration;
					gpuInput.visual.generations.controllerGeneration = item.binding.programGeneration;
					gpuInput.visual.generations.skeletonGeneration = item.binding.skeletonGeneration;
					gpuInput.visual.generations.clipGeneration = item.binding.clipGeneration;
					// Shader-visible generation lanes are intentionally only 32-bit.
					// Stamp the representable renderer/scene/mesh values here, while the
					// exact UUID and 64-bit lifetimes remain in renderIdentity beside the
					// packet. A value that cannot be represented forces the CPU path.
					const GpuAnimationRenderIdentity& renderIdentity =
						gpuInput.visual.renderIdentity;
					const AnimatorPlaybackStateSnapshot playbackSnapshot =
						item.playback.Snapshot();
					const bool gpuFrameIdentityRepresentable =
						IsGpuAnimationRenderIdentityValid(renderIdentity)
						&& renderIdentity.meshHeaderVersion
							<= std::numeric_limits<std::uint32_t>::max()
						&& renderIdentity.rendererGeneration
							<= std::numeric_limits<std::uint32_t>::max()
						&& frameSceneGeneration != 0U
						&& frameSceneGeneration
							<= std::numeric_limits<std::uint32_t>::max()
						&& playbackSnapshot.resolvedTick >= 0
						&& static_cast<std::uint64_t>(playbackSnapshot.resolvedTick)
							<= std::numeric_limits<std::uint32_t>::max();
					if(gpuFrameIdentityRepresentable) {
						gpuInput.visual.generations.meshGeneration =
							renderIdentity.meshHeaderVersion;
						gpuInput.visual.generations.rendererGeneration =
							static_cast<std::uint32_t>(
								renderIdentity.rendererGeneration);
						gpuInput.visual.generations.sceneGeneration =
							static_cast<std::uint32_t>(frameSceneGeneration);
						gpuInput.visual.sampleTick = static_cast<std::uint32_t>(
							playbackSnapshot.resolvedTick);
						// The frame capture, not the long-lived asset binding, owns the
						// scene publication identity used by this immutable packet.
						gpuInput.visual.renderIdentity.sceneGeneration =
							frameSceneGeneration;
					}
					gpuInput.visual.generations.frameIdLow = static_cast<std::uint32_t>(item.input.tick);
					gpuInput.visual.generations.frameIdHigh = static_cast<std::uint32_t>(
						static_cast<std::uint64_t>(item.input.tick) >> 32U);
					gpuInput.execution = item.executionDecision;
					gpuInput.residentPayloads = item.binding.gpuResidentPayloads;

					// Prove the entire immutable lowering input now, before CPU evaluation
					// begins. Missing programs, descriptor indices, or resident bytes must
					// downgrade this copied frame item to CPU instead of letting a later
					// presentation failure discard the only authoritative pose.
					GpuAnimationWorkRequest probe{};
					const GpuAnimationLoweringStatus loweringStatus =
						gpuFrameIdentityRepresentable
							? LowerGpuAnimationWorkRequest(gpuInput, probe)
							: GpuAnimationLoweringStatus::StaleGeneration;
					const bool residentInputsComplete =
						gpuInput.residentPayloads != nullptr
						&& ValidateGpuAnimationResidentPayloads(
							probe, *gpuInput.residentPayloads);
					if(loweringStatus == GpuAnimationLoweringStatus::Ok && residentInputsComplete) {
						item.gpuStageMask = probe.batchKey.stageMask;
						item.gpuLowering = std::move(gpuInput);
					} else {
						forceCpuFallback(loweringStatus == GpuAnimationLoweringStatus::Ok
							? AnimationExecutionModeReason::GpuResourcesUnavailable
							: AnimationExecutionModeReason::GpuEvaluationFailure);
					}
				}
				if(!TryAdvanceGeneration(
					item.capturedExecutionModeGeneration,
					item.executionDecision.nextState.initialized
						!= item.previousExecutionModeState.initialized
						|| item.executionDecision.nextState.effectiveMode
							!= item.previousExecutionModeState.effectiveMode
						|| item.executionDecision.nextState.effectiveReason
							!= item.previousExecutionModeState.effectiveReason,
					item.candidateExecutionModeGeneration)) {
					item.evaluationStatus = AnimationStatus::RangeOverflow;
				}
				ResolveUnchangedCacheReuse(item);
				plannedAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
			}
		}

		bool FinalizeAnimationWorkPlan() noexcept
		{
			if(frameWork.empty() || PollFrameCancellation()
				|| plannedAnimatorCount.load(std::memory_order_acquire) != frameWork.size()) {
				return false;
			}
			std::size_t estimatedPoseBytes = 0U;
			try {
				capturedWorkItems.clear();
				capturedWorkItems.reserve(frameWork.size());
				for(std::size_t originalIndex = 0U; originalIndex < frameWork.size(); ++originalIndex) {
					const FrameWorkItem& item = frameWork[originalIndex];
					AnimationWorkItem workItem;
					workItem.animator = item.input.handle;
					workItem.compatibility.programGeneration = item.binding.programGeneration;
					workItem.compatibility.skeletonGeneration = item.binding.skeletonGeneration;
					workItem.compatibility.clipBindingGeneration = item.binding.clipGeneration;
					workItem.compatibility.outputRequirementsFingerprint =
						BuildOutputRequirementsFingerprint(item);
					workItem.compatibility.resourceLayoutFingerprint =
						BuildResourceLayoutFingerprint(item);
					workItem.compatibility.effectiveMode = item.executionDecision.effectiveMode;
					workItem.stablePublicationOrder = originalIndex;
					workItem.sourceGeneration = item.candidateAuthorityPublicationGeneration;
					workItem.scratchBytes = sizeof(AnimationFrameScratchRecord);
					capturedWorkItems.push_back(workItem);
					const bool createsCpuPose = !item.cacheReused
						&& !(item.executionDecision.effectiveMode
							== AnimationEffectiveExecutionMode::GpuVisual
							&& item.gpuLowering.has_value());
					if(createsCpuPose && !TryEstimatePoseBytes(item, estimatedPoseBytes)) {
						workPlan = {};
						workPlan.status = AnimationStatus::RangeOverflow;
						workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
						return false;
					}
				}
			} catch(const std::bad_alloc&) {
				workPlan = {};
				workPlan.status = AnimationStatus::InternalError;
				workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
				return false;
			}

			availableWorkerCount = ThreadPool::instance().getThreadCount();
			workPlan = workScheduler.BuildPlan(
				capturedWorkItems,
				availableWorkerCount,
				workCancellationSource.CaptureToken());
			if(workPlan.status != AnimationStatus::Ok
				|| ValidateAnimationWorkPlan(workPlan) != AnimationStatus::Ok
				|| PollFrameCancellation()) {
				return false;
			}

			AnimationMemoryLease stagedLease;
			AnimationMemoryLease stagedPoseLease;
			const AnimationMemoryReservationRequest memoryRequest{
				AnimationMemoryClass::ScratchPool,
				workPlan.totalScratchBytes,
				frameWork.front().candidateAuthorityPublicationGeneration,
				AnimationMemoryRetention::Required,
				0U};
			const AnimationMemoryStatus memoryStatus = memoryController.TryReserve(
				memoryRequest, stagedLease);
			if(memoryStatus != AnimationMemoryStatus::Reserved) {
				workPlan.status = memoryStatus == AnimationMemoryStatus::BudgetExceeded
					|| memoryStatus == AnimationMemoryStatus::ReservationTableFull
					? AnimationStatus::HardLimitExceeded
					: AnimationStatus::InternalError;
				workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
				return false;
			}
			if(estimatedPoseBytes != 0U) {
				const AnimationMemoryReservationRequest poseMemoryRequest{
					AnimationMemoryClass::PosePool,
					estimatedPoseBytes,
					frameWork.front().candidateAuthorityPublicationGeneration,
					AnimationMemoryRetention::Required,
					0U};
				const AnimationMemoryStatus poseMemoryStatus = memoryController.TryReserve(
					poseMemoryRequest, stagedPoseLease);
				if(poseMemoryStatus != AnimationMemoryStatus::Reserved) {
					workPlan.status = poseMemoryStatus == AnimationMemoryStatus::BudgetExceeded
						|| poseMemoryStatus == AnimationMemoryStatus::ReservationTableFull
						? AnimationStatus::HardLimitExceeded
						: AnimationStatus::InternalError;
					workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
			}

			try {
				std::vector<std::byte> stagedScratch(workPlan.totalScratchBytes);
				std::vector<AnimationWorkCompletion> stagedCompletions(workPlan.items.size());
				auto stagedRangeLedger = std::make_unique<AnimationWorkRangeLedger>(workPlan);
				std::shared_ptr<const AnimationMemoryLease> stagedPoseReservation;
				if(stagedPoseLease.IsActive()) {
					stagedPoseReservation =
						std::make_shared<AnimationMemoryLease>(std::move(stagedPoseLease));
				}
				for(std::size_t scheduledIndex = 0U; scheduledIndex < workPlan.items.size(); ++scheduledIndex) {
					const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
					AnimationFrameScratchRecord record;
					record.sourceGeneration = scheduled.item.sourceGeneration;
					record.scheduledItemIndex = scheduledIndex;
					std::memcpy(
						stagedScratch.data() + scheduled.scratchOffset,
						&record,
						sizeof(record));
				}
				frameScratch.swap(stagedScratch);
				workCompletions.swap(stagedCompletions);
				workRangeLedger = std::move(stagedRangeLedger);
				frameScratchLease = std::move(stagedLease);
				framePoseReservation = std::move(stagedPoseReservation);
			} catch(const std::bad_alloc&) {
				workPlan.status = AnimationStatus::InternalError;
				workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
				return false;
			}
			return !PollFrameCancellation();
		}

		bool EnsureAnimationWorkPlanFinalized() noexcept
		{
			{
				std::unique_lock lock(workPlanFinalizeMutex);
				if(workPlanFinalizeState == WorkPlanFinalizeState::Ready) return true;
				if(workPlanFinalizeState == WorkPlanFinalizeState::Failed) return false;
				if(workPlanFinalizeState == WorkPlanFinalizeState::Building) {
					workPlanFinalizeCondition.wait(lock, [this]() {
						return workPlanFinalizeState != WorkPlanFinalizeState::Building;
					});
					return workPlanFinalizeState == WorkPlanFinalizeState::Ready;
				}
				workPlanFinalizeState = WorkPlanFinalizeState::Building;
			}

			const bool finalized = FinalizeAnimationWorkPlan();
			{
				std::scoped_lock lock(workPlanFinalizeMutex);
				workPlanFinalizeState = finalized
					? WorkPlanFinalizeState::Ready
					: WorkPlanFinalizeState::Failed;
			}
			workPlanFinalizeCondition.notify_all();
			return finalized;
		}

		AnimationTransform ReferenceTransformForStableId(
			const FrameWorkItem& item,
			std::uint64_t stableId) const noexcept
		{
			if(item.binding.skeleton) {
				for(const SkeletonJoint& joint : item.binding.skeleton->joints) {
					if(joint.stableId == stableId) return joint.localBindTransform;
				}
			}
			return {};
		}

		void BuildModelTransforms(
			const FrameWorkItem& item,
			std::span<const AnimationTransform> local,
			std::span<AnimationTransform> model) const noexcept
		{
			for(std::size_t index = 0U; index < local.size(); ++index) {
				model[index] = local[index];
				if(!item.binding.skeleton || index >= item.binding.jointStableIds.size()) continue;
				const std::uint64_t stableId = item.binding.jointStableIds[index];
				const auto joint = std::find_if(
					item.binding.skeleton->joints.begin(),
					item.binding.skeleton->joints.end(),
					[stableId](const SkeletonJoint& value) { return value.stableId == stableId; });
				if(joint == item.binding.skeleton->joints.end() || joint->parentIndex < 0
					|| static_cast<std::size_t>(joint->parentIndex)
						>= item.binding.skeleton->joints.size()) continue;
				const std::uint64_t parentStableId = item.binding.skeleton->joints[
					static_cast<std::size_t>(joint->parentIndex)].stableId;
				const auto parent = std::find(
					item.binding.jointStableIds.begin(),
					item.binding.jointStableIds.end(),
					parentStableId);
				if(parent == item.binding.jointStableIds.end()) continue;
				const std::size_t parentIndex = static_cast<std::size_t>(
					std::distance(item.binding.jointStableIds.begin(), parent));
				if(parentIndex < index) model[index] = CombineTransform(model[parentIndex], local[index]);
			}
		}

		bool BuildSkeletonModelTransforms(
			const SkeletonAsset& skeleton,
			std::span<const AnimationTransform> local,
			std::vector<AnimationTransform>& model) const
		{
			if(local.size() != skeleton.joints.size()) return false;
			model.resize(local.size());
			for(std::size_t index = 0U; index < local.size(); ++index) {
				const std::int32_t parent = skeleton.joints[index].parentIndex;
				if(parent < 0) {
					model[index] = local[index];
				} else if(static_cast<std::size_t>(parent) < index) {
					model[index] = CombineTransform(
						model[static_cast<std::size_t>(parent)], local[index]);
				} else {
					return false;
				}
			}
			return true;
		}

		AnimationStatus ApplyRetargetStage(
			FrameWorkItem& item,
			const CpuClipEvaluationResult& sampled,
			std::span<AnimationTransform> targetLocal,
			std::span<AnimationTransform> targetModel) const
		{
			if(!item.retargetActive || !item.sampledSourceSkeleton) {
				return AnimationStatus::Ok;
			}
			if(!item.binding.compiledRetargetProfile
				|| !item.binding.retargetProfile
				|| !item.binding.retargetSourceSkeleton
				|| !item.binding.skeleton
				|| sampled.joints.size()
					!= item.binding.retargetSourceSkeleton->joints.size()
				|| targetLocal.size() != item.binding.skeleton->joints.size()
				|| targetModel.size() != targetLocal.size()) {
				return AnimationStatus::IncompatibleAsset;
			}

			try {
				auto sourcePose = std::make_shared<RetargetPoseSnapshot>();
				sourcePose->skeleton = {
					item.binding.retargetSourceSkeleton->assetId,
					item.binding.retargetSourceSkeletonGeneration};
				sourcePose->poseGeneration = item.candidatePoseGeneration;
				sourcePose->tick = item.playback.AbsoluteTick();
				sourcePose->localTransforms.reserve(sampled.joints.size());
				for(const CpuClipJointSample& joint : sampled.joints) {
					sourcePose->localTransforms.push_back(joint.localTransform);
				}
				if(!BuildSkeletonModelTransforms(
						*item.binding.retargetSourceSkeleton,
						sourcePose->localTransforms,
						sourcePose->modelTransforms)) {
					return AnimationStatus::IncompatibleAsset;
				}

				auto targetReference = std::make_shared<RetargetPoseSnapshot>();
				targetReference->skeleton = {
					item.binding.skeleton->assetId,
					item.binding.skeletonGeneration};
				targetReference->poseGeneration = item.binding.skeletonGeneration;
				targetReference->tick = item.input.tick;
				targetReference->localTransforms.reserve(
					item.binding.skeleton->joints.size());
				for(const SkeletonJoint& joint : item.binding.skeleton->joints) {
					targetReference->localTransforms.push_back(joint.localBindTransform);
				}
				if(!BuildSkeletonModelTransforms(
						*item.binding.skeleton,
						targetReference->localTransforms,
						targetReference->modelTransforms)) {
					return AnimationStatus::IncompatibleAsset;
				}

				RetargetGenerationStamp generation;
				generation.profile = {
					item.binding.retargetProfile->assetId,
					item.binding.retargetProfileGeneration};
				generation.sourceSkeleton = sourcePose->skeleton;
				generation.targetSkeleton = targetReference->skeleton;
				generation.sourcePoseGeneration = sourcePose->poseGeneration;
				generation.targetReferencePoseGeneration =
					targetReference->poseGeneration;
				generation.publicationGeneration = item.candidatePoseGeneration;
				// A retarget packet is timed by the sampled source pose, not the
				// enclosing scheduler frame. Clip looping and playback-rate scaling
				// can make those ticks differ, and the constraint snapshot deliberately
				// rejects goals copied for any other source pose tick.
				generation.tick = sourcePose->tick;

				RetargetEvaluationRequest request;
				request.profile = item.binding.compiledRetargetProfile;
				request.sourceSkeleton = item.binding.retargetSourceSkeleton;
				request.targetSkeleton = item.binding.skeleton;
				request.sourcePose = std::move(sourcePose);
				request.targetReferencePose = std::move(targetReference);
				request.generation = generation;
				request.expectedGeneration = generation;
				request.normalizedTime = item.playback.Snapshot().normalizedTime;
				RetargetEvaluationResult result;
				const AnimationStatus status = EvaluateRetargetPose(request, result);
				if(status != AnimationStatus::Ok
					|| result.localTransforms.size() != targetLocal.size()
					|| result.modelTransforms.size() != targetModel.size()) {
					return status == AnimationStatus::Ok
						? AnimationStatus::InternalError : status;
				}
				std::copy(result.localTransforms.begin(), result.localTransforms.end(),
					targetLocal.begin());
				std::copy(result.modelTransforms.begin(), result.modelTransforms.end(),
					targetModel.begin());
				item.retargetMetrics = result.metrics;
				item.retargetDeterministicHash = result.deterministicHash;
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		static std::int16_t QuantizeMotionFeature(
			float value,
			float scale,
			float offset) noexcept
		{
			if(!std::isfinite(value) || !std::isfinite(scale)
				|| !std::isfinite(offset) || scale <= 0.0F) return 0;
			const float normalized = (value - offset) / scale - 32767.0F;
			return static_cast<std::int16_t>(std::clamp(
				std::lround(normalized),
				static_cast<long>(std::numeric_limits<std::int16_t>::min()),
				static_cast<long>(std::numeric_limits<std::int16_t>::max())));
		}

		static float MotionPoseFeature(
			std::span<const AnimationTransform> modelPose,
			std::size_t featureIndex) noexcept
		{
			constexpr std::size_t kTransformFeatureCount = 10U;
			const std::size_t jointIndex = featureIndex / kTransformFeatureCount;
			if(jointIndex >= modelPose.size()) return 0.0F;
			const AnimationTransform& transform = modelPose[jointIndex];
			switch(featureIndex % kTransformFeatureCount) {
			case 0U: return transform.translation.x;
			case 1U: return transform.translation.y;
			case 2U: return transform.translation.z;
			case 3U: return transform.rotation.x;
			case 4U: return transform.rotation.y;
			case 5U: return transform.rotation.z;
			case 6U: return transform.rotation.w;
			case 7U: return transform.scale.x;
			case 8U: return transform.scale.y;
			default: return transform.scale.z;
			}
		}

		void EvaluateMotionMatchRequests(
			FrameWorkItem& item,
			std::span<const AnimationTransform> modelPose) const noexcept
		{
			if(item.pendingMotionQueries.empty()) return;
			try {
				item.evaluatedMotionQueries.clear();
				item.evaluatedMotionQueries.reserve(item.pendingMotionQueries.size());
				for(const NativeAnimationMotionMatchRequest& native
					: item.pendingMotionQueries) {
					NativeAnimationMotionMatchSnapshot snapshot{};
					snapshot.header = MakeNativeHeader(
						NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(snapshot));
					snapshot.animator = native.animator;
					snapshot.databaseAssetId = item.activeMotionDatabaseId;
					snapshot.databaseGeneration = item.activeMotionDatabaseGeneration;
					snapshot.queryId = native.queryId;
					snapshot.observationTick = item.input.tick;
					snapshot.publicationGeneration =
						item.candidateAuthorityPublicationGeneration;

					AnimationStatus status = item.evaluationStatus == AnimationStatus::Ok
						? AnimationStatus::NotReady : item.evaluationStatus;
					const MotionDatabaseAsset* database = item.binding.motionDatabase.get();
					if(item.evaluationStatus == AnimationStatus::Ok
						&& database != nullptr
						&& item.activeMotionDatabaseGeneration != 0U
						&& MatchesNativeUuid(item.activeMotionDatabaseId, database->assetId.value)
						&& MatchesNativeUuid(native.databaseAssetId, database->assetId.value)
						&& native.databaseGeneration == item.activeMotionDatabaseGeneration
						&& item.activeMotionDatabaseGeneration
							== item.binding.motionDatabaseGeneration
						&& native.featureCount == kMotionMatchFeatureCountV1) {
						MotionMatchQuery query;
						query.database = {
							database->assetId, item.activeMotionDatabaseGeneration};
						query.expectedSchemaFingerprint =
							database->featureSchemaFingerprint;
						query.featureWeights.fill(1U);
						query.maximumCandidates = kMotionDatabaseTopKDefault;
						const MotionDatabaseSample* priorSample = nullptr;
						if(item.hasMotionCandidate) {
							const auto prior = std::find_if(
								database->samples.begin(), database->samples.end(),
								[&item](const MotionDatabaseSample& sample) {
									return sample.stableSampleId
										== item.latestMotionCandidate.stableSampleId;
								});
							if(prior != database->samples.end()) priorSample = &*prior;
						}
						if(priorSample != nullptr) {
							query.features = priorSample->features;
							query.preferredPhase = priorSample->phaseQuantized;
							query.preferredContinuityGroup = priorSample->continuityGroup;
							query.lastSampleId = priorSample->stableSampleId;
							query.lastSourceClipId = priorSample->sourceClipId;
						} else {
							for(std::size_t feature = 0U;
								feature < kMotionDatabaseFeatureCount; ++feature) {
								query.features.values[feature] = QuantizeMotionFeature(
									MotionPoseFeature(modelPose, feature),
									database->quantizationScales[feature],
									database->quantizationOffsets[feature]);
							}
							query.preferredPhase = static_cast<std::uint16_t>(std::clamp(
								item.playback.Snapshot().normalizedTime * 65535.0F,
								0.0F, 65535.0F));
						}

						MotionMatchCpuResult scored;
						status = ScoreMotionMatchCpu(*database, query, scored);
						if(status == AnimationStatus::Ok && !scored.candidates.empty()) {
							const MotionMatchCandidate& chosen = scored.candidates.front();
							snapshot.selectedSampleId = chosen.stableSampleId;
							const auto clip = std::lower_bound(
								item.binding.controllerClips.begin(),
								item.binding.controllerClips.end(), chosen.sourceClipId,
								[](const AnimatorControllerClipBinding& candidate,
									std::uint64_t sourceClipId) {
									return candidate.nodeStableId < sourceClipId;
								});
							if(clip != item.binding.controllerClips.end()
								&& clip->nodeStableId == chosen.sourceClipId) {
								snapshot.selectedClipGeneration = clip->clipGeneration;
							}
							const std::uint64_t trajectoryCost = chosen.score.phaseCost
								+ chosen.score.continuityCost + chosen.score.transitionCost;
							snapshot.totalScore = static_cast<float>(chosen.score.totalCost);
							snapshot.trajectoryScore = static_cast<float>(trajectoryCost);
							snapshot.poseScore = static_cast<float>(chosen.score.featureCost);
							item.latestMotionCandidate = chosen;
							item.latestMotionQueryId = native.queryId;
							item.hasMotionCandidate = true;
						} else {
							snapshot.usedFallback = 1U;
						}
					} else if(database == nullptr) {
						status = AnimationStatus::MissingAsset;
					} else if(native.featureCount != kMotionMatchFeatureCountV1) {
						status = AnimationStatus::InvalidArgument;
					} else if(item.activeMotionDatabaseGeneration != 0U) {
						status = AnimationStatus::StaleHandle;
					}
					snapshot.status = ToNativeStatus(status);
					item.evaluatedMotionQueries.push_back(snapshot);
				}
			} catch(const std::bad_alloc&) {
				item.evaluatedMotionQueries.clear();
			}
		}

		void EvaluateMotionMatchQueriesV2(FrameWorkItem& item) const noexcept
		{
			if(item.pendingMotionQueriesV2.empty()) return;
			try {
				item.evaluatedMotionQueriesV2.clear();
				item.evaluatedMotionQueriesV2.reserve(item.pendingMotionQueriesV2.size());
				for(const NativeAnimationMotionMatchQueryRequestV2& native
					: item.pendingMotionQueriesV2) {
					MotionMatchQueryCompletionV2 completion;
					completion.correlationId = native.correlationId;
					NativeAnimationMotionMatchResultV2& result = completion.result;
					result.header = MakeNativeHeader(
						NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
					result.animator = native.animator;
					result.databaseAssetId = native.databaseAssetId;
					result.databaseGeneration = native.databaseGeneration;
					result.schemaFingerprint = native.schemaFingerprint;
					result.queryId = native.queryId;
					result.observationTick = item.input.tick;
					result.publicationGeneration =
						item.candidateAuthorityPublicationGeneration;
					result.populatedFields = static_cast<std::uint32_t>(
						NativeAnimationMotionMatchResultField::Identity);

					AnimationStatus status = item.evaluationStatus == AnimationStatus::Ok
						? AnimationStatus::NotReady : item.evaluationStatus;
					const MotionDatabaseAsset* database = item.binding.motionDatabase.get();
					if(item.evaluationStatus == AnimationStatus::Ok && database == nullptr) {
						status = AnimationStatus::MissingAsset;
					} else if(item.evaluationStatus == AnimationStatus::Ok
						&& !MatchesNativeUuid(native.databaseAssetId, database->assetId.value)) {
						status = AnimationStatus::IncompatibleAsset;
					} else if(item.evaluationStatus == AnimationStatus::Ok
						&& (native.databaseGeneration != item.binding.motionDatabaseGeneration
							|| native.databaseGeneration != database->runtimeGeneration)) {
						status = AnimationStatus::StaleHandle;
					} else if(item.evaluationStatus == AnimationStatus::Ok
						&& native.schemaFingerprint != database->featureSchemaFingerprint) {
						status = AnimationStatus::IncompatibleAsset;
					} else if(item.evaluationStatus == AnimationStatus::Ok) {
						result.populatedFields |= static_cast<std::uint32_t>(
							NativeAnimationMotionMatchResultField::DatabaseSchema);
						MotionMatchQuery query;
						query.database = {database->assetId, native.databaseGeneration};
						query.expectedSchemaFingerprint = native.schemaFingerprint;
						for(std::size_t feature = 0U;
							feature < kMotionDatabaseFeatureCount; ++feature) {
							query.features.values[feature] = native.features[feature];
							query.featureWeights[feature] = native.featureWeights[feature];
						}

						auto hasField = [&native](NativeAnimationMotionMatchQueryField field) {
							return (native.queryFields & static_cast<std::uint32_t>(field)) != 0U;
						};
						// Optional groups are applied only when their field bit is present. This
						// keeps the receipt's scheduledFields mask an honest list of consumed data.
						if(hasField(NativeAnimationMotionMatchQueryField::Tags)) {
							query.requiredTags = native.requiredTags;
							query.excludedTags = native.excludedTags;
							query.preferredTags = native.preferredTags;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Contacts)) {
							query.requiredContacts = native.requiredContacts;
							query.preferredContacts = native.preferredContacts;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Phase)) {
							query.preferredPhase = native.preferredPhase;
							query.maximumPhaseDistance = native.maximumPhaseDistance;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Transitions)) {
							query.requiredTransitionFlags = native.requiredTransitionFlags;
							query.excludedTransitionFlags = native.excludedTransitionFlags;
							query.preferredTransitionFlags = native.preferredTransitionFlags;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Continuity)) {
							query.preferredContinuityGroup = native.preferredContinuityGroup;
							query.lastSampleId = native.lastSampleId;
							query.lastSourceClipId = native.lastSourceClipId;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Exclusions)) {
							query.excludedRegionFlags = native.excludedRegionFlags;
							query.excludedSampleIds = {
								native.excludedSampleIds, native.excludedSampleCount};
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Costs)) {
							query.phaseWeight = native.phaseWeight;
							query.continuityPenalty = native.continuityPenalty;
							query.continuityBreakPenalty = native.continuityBreakPenalty;
							query.sameClipPenalty = native.sameClipPenalty;
							query.missingPreferredTagPenalty =
								native.missingPreferredTagPenalty;
							query.contactMismatchPenalty = native.contactMismatchPenalty;
							query.transitionMismatchPenalty = native.transitionMismatchPenalty;
						}
						if(hasField(NativeAnimationMotionMatchQueryField::Budgets)) {
							query.maximumCandidates = native.maximumCandidates;
							query.maximumSamplesToVisit = native.maximumSamplesToVisit;
							query.maximumDiagnosticExclusions =
								native.maximumDiagnosticExclusions;
						}

						MotionMatchCpuResult scored;
						status = ScoreMotionMatchCpu(*database, query, scored);
						result.gpuStatus = NativeAnimationStatus::RendererUnavailable;
						result.selectionSource =
							NativeAnimationMotionMatchSelectionSource::CpuFullSearch;
						result.fallbackReason = scored.fallbackReason
							== MotionMatchFallbackReason::None
							? NativeAnimationMotionMatchFallbackReason::GpuUnavailable
							: static_cast<NativeAnimationMotionMatchFallbackReason>(
								scored.fallbackReason);
						result.flags = static_cast<std::uint32_t>(
							NativeAnimationMotionMatchResultFlag::UsedFallback)
							| static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultFlag::UsedCpuFallback);
						if(scored.candidateBudgetExhausted) {
							result.flags |= static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultFlag::CandidateBudgetExhausted);
						}
						result.visitedSamples = scored.visitedSamples;
						result.evaluatedSamples = scored.evaluatedSamples;
						result.excludedSamples = scored.excludedSamples;
						for(std::size_t reason = 0U;
							reason < scored.exclusionReasonCounts.size(); ++reason) {
							if(scored.exclusionReasonCounts[reason] != 0U) {
								result.exclusionReasonMask |= 1U << reason;
							}
						}
						result.populatedFields |= static_cast<std::uint32_t>(
							NativeAnimationMotionMatchResultField::Exclusions)
							| static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultField::Fallback)
							| static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultField::SelectionSource)
							| static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultField::GpuStatus)
							| static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultField::CandidateCounts);

						if(status == AnimationStatus::Ok && !scored.candidates.empty()) {
							const MotionMatchCandidate& chosen = scored.candidates.front();
							result.selectedSampleId = chosen.stableSampleId;
							result.selectedSourceClipId = chosen.sourceClipId;
							result.selectedSourceTick = chosen.sourceTick;
							const auto clip = std::lower_bound(
								item.binding.controllerClips.begin(),
								item.binding.controllerClips.end(), chosen.sourceClipId,
								[](const AnimatorControllerClipBinding& candidate,
									std::uint64_t sourceClipId) {
									return candidate.nodeStableId < sourceClipId;
								});
							if(clip != item.binding.controllerClips.end()
								&& clip->nodeStableId == chosen.sourceClipId) {
								result.selectedClipGeneration = clip->clipGeneration;
							}
							result.totalCost = chosen.score.totalCost;
							result.featureCost = chosen.score.featureCost;
							for(std::size_t block = 0U;
								block < chosen.score.featureBlockCosts.size(); ++block) {
								result.featureBlockCosts[block] =
									chosen.score.featureBlockCosts[block];
							}
							result.phaseCost = chosen.score.phaseCost;
							result.tagCost = chosen.score.tagCost;
							result.contactCost = chosen.score.contactCost;
							result.continuityCost = chosen.score.continuityCost;
							result.transitionCost = chosen.score.transitionCost;
							result.populatedFields |= static_cast<std::uint32_t>(
								NativeAnimationMotionMatchResultField::SelectedSample)
								| static_cast<std::uint32_t>(
									NativeAnimationMotionMatchResultField::SelectedSource)
								| static_cast<std::uint32_t>(
									NativeAnimationMotionMatchResultField::TotalCost)
								| static_cast<std::uint32_t>(
									NativeAnimationMotionMatchResultField::FeatureBlockCosts)
								| static_cast<std::uint32_t>(
									NativeAnimationMotionMatchResultField::MetadataCosts);
							item.latestMotionCandidate = chosen;
							item.latestMotionQueryId = native.queryId;
							item.hasMotionCandidate = true;
							completion.selectedCandidate = chosen;
							completion.hasSelectedCandidate = true;
						}
					}
					result.status = ToNativeStatus(status);
					item.evaluatedMotionQueriesV2.push_back(std::move(completion));
				}
			} catch(const std::bad_alloc&) {
				item.evaluatedMotionQueriesV2.clear();
			}
		}

		AnimationStatus ApplyRetargetRootMotionPolicy(
			const FrameWorkItem& item,
			AnimationRootMotionDelta& delta) const noexcept
		{
			if(!item.retargetActive || !item.sampledSourceSkeleton
				|| !item.binding.retargetProfile) return AnimationStatus::Ok;
			const RetargetProfileAsset& profile = *item.binding.retargetProfile;
			float proportionScale = 1.0F;
			switch(profile.proportion.translationScaleMode) {
			case RetargetTranslationScaleMode::None:
				break;
			case RetargetTranslationScaleMode::UniformHeight:
			case RetargetTranslationScaleMode::PerChainLength:
				if(profile.proportion.sourceReferenceHeightMeters <= 0.0F) {
					return AnimationStatus::IncompatibleAsset;
				}
				proportionScale = profile.proportion.targetReferenceHeightMeters
					/ profile.proportion.sourceReferenceHeightMeters;
				break;
			case RetargetTranslationScaleMode::Authored:
				proportionScale = profile.proportion.authoredTranslationScale;
				break;
			}
			float displacementScale = profile.root.displacementScale;
			if(profile.root.motionMode == RetargetRootMotionMode::ScaleByProportion) {
				displacementScale *= proportionScale;
			} else if(profile.root.motionMode == RetargetRootMotionMode::InPlace) {
				displacementScale = 0.0F;
			}
			if(!std::isfinite(displacementScale)) return AnimationStatus::RangeOverflow;
			auto transformTranslation = [&](AnimationFloat3& translation) {
				translation.x *= displacementScale * profile.trajectory.horizontalScale;
				translation.z *= displacementScale * profile.trajectory.horizontalScale;
				translation.y = profile.root.preserveVerticalDisplacement
					? translation.y * displacementScale * profile.trajectory.verticalScale
					: 0.0F;
			};
			transformTranslation(delta.localTranslation);
			transformTranslation(delta.worldTranslation);
			if(!profile.root.preserveFacing
				|| profile.root.motionMode == RetargetRootMotionMode::InPlace) {
				delta.localYawRadians = 0.0F;
				delta.worldYawRadians = 0.0F;
			}
			return IsFinite(delta.localTranslation) && IsFinite(delta.worldTranslation)
				&& std::isfinite(delta.localYawRadians)
				&& std::isfinite(delta.worldYawRadians)
				? AnimationStatus::Ok : AnimationStatus::RangeOverflow;
		}

		bool BuildLocalTransformsFromModel(
			const FrameWorkItem& item,
			std::span<const AnimationTransform> model,
			std::span<AnimationTransform> local) const noexcept
		{
			if(model.size() != local.size()
				|| model.size() != item.binding.jointStableIds.size()) return false;
			for(std::size_t index = 0U; index < model.size(); ++index) {
				const std::int32_t parent = index < item.binding.jointParents.size()
					? item.binding.jointParents[index] : -1;
				if(parent < 0) {
					local[index] = model[index];
					continue;
				}
				if(static_cast<std::size_t>(parent) >= index
					|| !ProceduralMath::TryRelativeTransform(
						model[static_cast<std::size_t>(parent)], model[index], local[index])) {
					return false;
				}
			}
			return true;
		}

		ProceduralGenerationStamp MakeProceduralGeneration(
			const FrameWorkItem& item) const noexcept
		{
			ProceduralGenerationStamp generation;
			generation.animator = item.input.handle;
			generation.componentGeneration = item.input.descriptor.componentGeneration;
			generation.skeletonGeneration = item.binding.skeletonGeneration;
			generation.programGeneration = item.binding.programGeneration;
			generation.poseGeneration = item.candidatePoseGeneration;
			generation.publicationGeneration =
				item.candidateAuthorityPublicationGeneration;
			generation.tick = item.input.tick;
			return generation;
		}

		std::uint32_t FindProceduralJointIndex(
			const FrameWorkItem& item,
			std::uint64_t stableJointId) const noexcept
		{
			const auto found = std::find(
				item.binding.jointStableIds.begin(),
				item.binding.jointStableIds.end(), stableJointId);
			return found == item.binding.jointStableIds.end()
				? kInvalidProceduralJointIndex
				: static_cast<std::uint32_t>(std::distance(
					item.binding.jointStableIds.begin(), found));
		}

		AnimationStatus BuildProceduralNodes(
			const FrameWorkItem& item,
			const ProceduralGenerationStamp& generation,
			std::vector<ProceduralGraphNodeInput>& nodes) const
		{
			nodes.clear();
			nodes.reserve(item.activeProceduralGoals.size());
			for(const NativeAnimationProceduralGoalCommand& command
				: item.activeProceduralGoals) {
				const std::uint32_t jointIndex = FindProceduralJointIndex(
					item, command.targetJointStableId);
				if(jointIndex == kInvalidProceduralJointIndex) {
					return AnimationStatus::MissingAsset;
				}
				ProceduralGraphNodeInput node;
				node.stableId = command.goalId;
				node.layerOrder = command.layerIndex;
				node.nodeOrder = static_cast<std::uint32_t>(
					command.producerSequence & 0xffffffffU);
				node.nodeWeight = std::max(command.positionWeight, command.rotationWeight);
				const AnimationTransform target = FromNativeTransform(command.goalTransform);
				const auto space = static_cast<ProceduralCoordinateSpace>(command.coordinateSpace);
				const auto kind = static_cast<ProceduralGoalKind>(command.goalKind);
				const std::uint32_t rawFlags = static_cast<std::uint32_t>(command.flags);

				auto makeGoalSnapshot = [&]() {
					ProceduralGoal goal;
					goal.generation = generation;
					goal.target.stableTargetId = command.targetJointStableId;
					goal.goalId = command.goalId;
					goal.producerOrder = command.producerSequence;
					goal.priority = command.priority;
					goal.targetJointIndex = jointIndex;
					goal.kind = kind;
					goal.space = space;
					goal.position = target.translation;
					goal.rotation = target.rotation;
					goal.pole = {command.poleVector.x, command.poleVector.y, command.poleVector.z};
					goal.weight = node.nodeWeight;
					goal.effectiveTick = command.targetTick;
					ProceduralGoalSnapshot snapshot;
					(void)BuildProceduralGoalSnapshot(
						generation, std::span<const ProceduralGoal>(&goal, 1U), snapshot);
					return snapshot;
				};

				switch(command.goalKind) {
				case NativeAnimationProceduralGoalKind::Aim: {
					ProceduralAimNodeInput aim;
					aim.jointIndex = jointIndex;
					aim.goals = makeGoalSnapshot();
					aim.solverWeight = node.nodeWeight;
					node.payload = std::move(aim);
					break;
				}
				case NativeAnimationProceduralGoalKind::Look: {
					ProceduralLookNodeInput look;
					look.aim.jointIndex = jointIndex;
					look.aim.goals = makeGoalSnapshot();
					look.aim.solverWeight = node.nodeWeight;
					node.payload = std::move(look);
					break;
				}
				case NativeAnimationProceduralGoalKind::Twist: {
					ProceduralTwistNodeInput twist;
					twist.jointIndices.push_back(jointIndex);
					twist.distributionWeights.push_back(1.0F);
					twist.twistRadians = 2.0F * std::acos(std::clamp(
						target.rotation.w, -1.0F, 1.0F));
					twist.solverWeight = command.rotationWeight;
					twist.goalId = command.goalId;
					node.payload = std::move(twist);
					break;
				}
				case NativeAnimationProceduralGoalKind::Position:
				case NativeAnimationProceduralGoalKind::FootPlant: {
					const std::int32_t middle = jointIndex < item.binding.jointParents.size()
						? item.binding.jointParents[jointIndex] : -1;
					const std::int32_t root = middle >= 0
						&& static_cast<std::size_t>(middle) < item.binding.jointParents.size()
						? item.binding.jointParents[static_cast<std::size_t>(middle)] : -1;
					if(root >= 0 && middle >= 0) {
						ProceduralTwoBoneNodeInput twoBone;
						twoBone.rootJointIndex = static_cast<std::uint32_t>(root);
						twoBone.middleJointIndex = static_cast<std::uint32_t>(middle);
						twoBone.endJointIndex = jointIndex;
						twoBone.targetPosition = target.translation;
						twoBone.polePosition = {
							command.poleVector.x, command.poleVector.y, command.poleVector.z};
						twoBone.space = space;
						twoBone.goalId = command.goalId;
						twoBone.producerOrder = command.producerSequence;
						twoBone.priority = command.priority;
						twoBone.solverWeight = command.positionWeight;
						twoBone.maximumStretchScale = command.maximumStretchScale;
						twoBone.allowStretch = (rawFlags & static_cast<std::uint32_t>(
							NativeAnimationProceduralGoalFlag::AllowStretch)) != 0U;
						node.payload = twoBone;
						break;
					}
					[[fallthrough]];
				}
				case NativeAnimationProceduralGoalKind::Rotation:
				case NativeAnimationProceduralGoalKind::PositionAndRotation:
				case NativeAnimationProceduralGoalKind::CopyTransform: {
					ProceduralCopyTransformNodeInput copy;
					copy.jointIndex = jointIndex;
					copy.parentJointIndex = jointIndex < item.binding.jointParents.size()
						&& item.binding.jointParents[jointIndex] >= 0
						? static_cast<std::uint32_t>(item.binding.jointParents[jointIndex])
						: kInvalidProceduralJointIndex;
					copy.targetTransform = target;
					copy.targetSpace = space;
					copy.translationWeight = command.goalKind
						== NativeAnimationProceduralGoalKind::Rotation
						? 0.0F : command.positionWeight;
					copy.rotationWeight = command.goalKind
						== NativeAnimationProceduralGoalKind::Position
						|| command.goalKind == NativeAnimationProceduralGoalKind::FootPlant
						? 0.0F : command.rotationWeight;
					copy.scaleWeight = 0.0F;
					copy.goalId = command.goalId;
					node.payload = copy;
					break;
				}
				default: return AnimationStatus::Unsupported;
				}
				nodes.push_back(std::move(node));
			}
			return AnimationStatus::Ok;
		}

		NativeAnimationProceduralSolverKind NativeSolverForGoal(
			NativeAnimationProceduralGoalKind kind) const noexcept
		{
			switch(kind) {
			case NativeAnimationProceduralGoalKind::Aim:
				return NativeAnimationProceduralSolverKind::Aim;
			case NativeAnimationProceduralGoalKind::Look:
				return NativeAnimationProceduralSolverKind::Look;
			case NativeAnimationProceduralGoalKind::Twist:
				return NativeAnimationProceduralSolverKind::TwistDistribution;
			case NativeAnimationProceduralGoalKind::CopyTransform:
			case NativeAnimationProceduralGoalKind::Rotation:
			case NativeAnimationProceduralGoalKind::PositionAndRotation:
				return NativeAnimationProceduralSolverKind::CopyTransform;
			case NativeAnimationProceduralGoalKind::FootPlant:
				return NativeAnimationProceduralSolverKind::FootPlacement;
			default: return NativeAnimationProceduralSolverKind::TwoBoneIk;
			}
		}

		AnimationStatus ApplyProceduralStage(
			FrameWorkItem& item,
			GraphPoseContribution& pose) const
		{
			if(item.activeProceduralGoals.empty()) return AnimationStatus::Ok;
			const ProceduralGenerationStamp generation = MakeProceduralGeneration(item);
			std::vector<ProceduralGraphNodeInput> nodes;
			const AnimationStatus nodeStatus = BuildProceduralNodes(item, generation, nodes);
			if(nodeStatus != AnimationStatus::Ok) return nodeStatus;
			ProceduralGraphEvaluationRequest request;
			request.generation = generation;
			request.expectedGeneration = generation;
			request.inputPose = &pose;
			request.nodes = nodes;
			request.authority = ProceduralGraphAuthority::CpuAuthoritative;
			request.authoritativeConsumer = true;
			request.rollbackRequested = item.executionDecision.consumerClosure.rollback;
			request.recordingRequested = item.executionDecision.consumerClosure.recording;
			ProceduralGraphEvaluationResult evaluated;
			const AnimationStatus status = EvaluateProceduralGraphNodes(request, evaluated);
			if(status != AnimationStatus::Ok) return status;
			pose = std::move(evaluated.pose);

			item.proceduralResults.clear();
			item.proceduralResults.reserve(item.activeProceduralGoals.size());
			for(const NativeAnimationProceduralGoalCommand& command
				: item.activeProceduralGoals) {
				NativeAnimationProceduralResultSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(
					NativeAnimationRecordKind::ProceduralResultSnapshot, sizeof(snapshot));
				snapshot.status = ToNativeStatus(AnimationStatus::Ok);
				snapshot.animator = {item.input.handle.slotIndex, item.input.handle.generation};
				snapshot.solver = NativeSolverForGoal(command.goalKind);
				snapshot.state = NativeAnimationProceduralSolverState::NoContribution;
				snapshot.goalId = command.goalId;
				snapshot.targetJointStableId = command.targetJointStableId;
				const std::uint32_t jointIndex = FindProceduralJointIndex(
					item, command.targetJointStableId);
				if(jointIndex < pose.joints.size()) {
					snapshot.resultTransform = ToNativeTransform(pose.joints[jointIndex]);
				}
				snapshot.inputWeight = std::max(
					command.positionWeight, command.rotationWeight);
				const auto diagnostic = std::find_if(
					evaluated.diagnostics.begin(), evaluated.diagnostics.end(),
					[&command](const ProceduralDiagnosticRecord& value) {
						return value.goalId == command.goalId;
					});
				if(diagnostic != evaluated.diagnostics.end()) {
					snapshot.state = static_cast<NativeAnimationProceduralSolverState>(
						diagnostic->state);
					snapshot.targetErrorMeters = diagnostic->targetErrorMeters;
					snapshot.targetErrorRadians = diagnostic->targetErrorRadians;
					snapshot.finalContribution = diagnostic->finalContribution;
					snapshot.iterationCount = diagnostic->iterationCount;
					snapshot.iterationLimit = diagnostic->iterationLimit;
					std::uint32_t flags = static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::CpuAuthoritative);
					if(diagnostic->converged) flags |= static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::Converged);
					if(diagnostic->clamped) flags |= static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::Clamped);
					if(diagnostic->singular) flags |= static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::Singular);
					if(diagnostic->unreachable) flags |= static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::Unreachable);
					if(diagnostic->staleInput) flags |= static_cast<std::uint32_t>(
						NativeAnimationProceduralResultFlag::StaleInput);
					snapshot.flags = static_cast<NativeAnimationProceduralResultFlag>(flags);
				} else {
					snapshot.flags = NativeAnimationProceduralResultFlag::CpuAuthoritative;
				}
				snapshot.poseGeneration = item.candidatePoseGeneration;
				snapshot.componentGeneration = item.input.descriptor.componentGeneration;
				snapshot.skeletonGeneration = item.binding.skeletonGeneration;
				snapshot.programGeneration = item.binding.programGeneration;
				snapshot.observationTick = item.input.tick;
				snapshot.publicationGeneration =
					item.candidateAuthorityPublicationGeneration;
				snapshot.producerOrder = command.producerSequence;
				snapshot.correlationId = command.correlationId;
				item.proceduralResults.push_back(snapshot);
			}
			return AnimationStatus::Ok;
		}

		AnimationStatus ApplyRagdollStage(
			FrameWorkItem& item,
			GraphPoseContribution& pose) const
		{
			if(item.ragdollLifecycle != RagdollLifecycleState::Active
				&& item.ragdollLifecycle != RagdollLifecycleState::Recovering) {
				return AnimationStatus::Ok;
			}
			if(!item.ragdollPhysicsPose || !item.binding.ragdollProfile
				|| !item.sourceObjectWorldTransformCaptured) {
				return AnimationStatus::NotReady;
			}
			if(item.binding.jointParents.size() != pose.jointStableIds.size()
				|| pose.jointStableIds.size() != pose.joints.size()) {
				return AnimationStatus::IncompatibleAsset;
			}
			if(item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
				const auto rootBody = std::find_if(
					item.binding.ragdollProfile->bodies.begin(),
					item.binding.ragdollProfile->bodies.end(),
					[&item](const RagdollBodyRecord& value) {
						return value.bodyId == item.binding.ragdollProfile->rootBodyId;
					});
				if(rootBody != item.binding.ragdollProfile->bodies.end()) {
					const auto rootJoint = std::find(
						pose.jointStableIds.begin(), pose.jointStableIds.end(),
						rootBody->stableJointId);
					if(rootJoint != pose.jointStableIds.end()) {
						const std::size_t rootIndex = static_cast<std::size_t>(
							std::distance(pose.jointStableIds.begin(), rootJoint));
						AnimationTransform alignedWorld = CombineTransform(
							item.sourceObjectWorldTransform, pose.joints[rootIndex]);
						alignedWorld.translation = item.ragdollRecoveryAlignedRootPosition;
						AnimationTransform alignedModel;
						if(!ProceduralMath::TryRelativeTransform(
							item.sourceObjectWorldTransform, alignedWorld, alignedModel)) {
							return AnimationStatus::InvalidArgument;
						}
						pose.joints[rootIndex] = alignedModel;
					}
				}
			}
			PhysicsPoseSnapshot mappedPhysicsPose = *item.ragdollPhysicsPose;
			for(const RagdollBodyRecord& body : item.binding.ragdollProfile->bodies) {
				const auto physicsBody = std::find_if(
					mappedPhysicsPose.bodies.begin(), mappedPhysicsPose.bodies.end(),
					[&body](const PhysicsPoseBodySnapshot& value) {
						return value.bodyId == body.bodyId
							&& value.stableJointId == body.stableJointId;
					});
				const auto joint = std::find(
					pose.jointStableIds.begin(), pose.jointStableIds.end(), body.stableJointId);
				if(physicsBody == mappedPhysicsPose.bodies.end()
					|| joint == pose.jointStableIds.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const std::size_t jointIndex = static_cast<std::size_t>(
					std::distance(pose.jointStableIds.begin(), joint));
				AnimationTransform jointModel;
				if(!TryMapRagdollBodyWorldToJointModel(
					item.sourceObjectWorldTransform, pose.joints[jointIndex], body,
					physicsBody->modelPose, jointModel)) {
					return AnimationStatus::InvalidArgument;
				}
				physicsBody->modelPose = jointModel;
			}
			std::vector<RagdollBlendInput> inputs;
			inputs.reserve(item.binding.ragdollProfile->bodies.size());
			for(const RagdollBodyRecord& body : item.binding.ragdollProfile->bodies) {
				RagdollBlendInput input;
				input.stableJointId = body.stableJointId;
				input.animationDrivenKinematic = body.motionType
					== RagdollMotionType::AnimationDriven
					|| body.motionType == RagdollMotionType::Kinematic;
				input.layerWeight = item.ragdollBlendWeight;
				input.timeStartWeight = item.ragdollLifecycle
					== RagdollLifecycleState::Recovering ? 1.0F : 0.0F;
				input.timeTargetWeight = item.ragdollLifecycle
					== RagdollLifecycleState::Recovering ? 0.0F : 1.0F;
				input.durationSeconds = item.ragdollLifecycle
					== RagdollLifecycleState::Recovering
					? item.ragdollRecoveryDurationSeconds : 0.0F;
				input.elapsedSeconds = item.ragdollLifecycleStartTick <= item.input.tick
					? static_cast<float>(item.input.tick - item.ragdollLifecycleStartTick)
						/ static_cast<float>(kAnimationFixedTicksPerSecond)
					: 0.0F;
				const auto blend = std::find_if(
					item.binding.ragdollProfile->blends.begin(),
					item.binding.ragdollProfile->blends.end(),
					[&body](const RagdollBlendRecord& value) {
						return value.stableJointId == body.stableJointId;
					});
				if(blend != item.binding.ragdollProfile->blends.end()) {
					// The authored joint mask is independent from branch inheritance and
					// the request-wide layer contribution. Keeping them in separate inputs
					// prevents an include-descendants record from squaring its own weight.
					input.weight = 1.0F;
					input.branchWeight = blend->includeDescendants
						? blend->defaultWeight : 1.0F;
					input.maskWeight = blend->includeDescendants
						? 1.0F : blend->defaultWeight;
					input.boundaryPolicy = blend->boundaryPolicy;
					input.motorStrength = blend->motorStrengthScale;
					input.includeDescendants = blend->includeDescendants;
					input.durationSeconds = item.ragdollLifecycle
						== RagdollLifecycleState::Recovering
						? (blend->recoveryBlendSeconds > 0.0F
							? blend->recoveryBlendSeconds
							: item.ragdollRecoveryDurationSeconds)
						: blend->activationBlendSeconds;
				}
				const auto joint = std::find(
					pose.jointStableIds.begin(), pose.jointStableIds.end(), body.stableJointId);
				if(joint == pose.jointStableIds.end()) return AnimationStatus::IncompatibleAsset;
				const std::size_t jointIndex = static_cast<std::size_t>(
					std::distance(pose.jointStableIds.begin(), joint));
				const std::int32_t parentIndex = item.binding.jointParents[jointIndex];
				if(parentIndex >= 0) {
					if(static_cast<std::size_t>(parentIndex) >= jointIndex) {
						return AnimationStatus::IncompatibleAsset;
					}
					const std::uint64_t parentJointId =
						pose.jointStableIds[static_cast<std::size_t>(parentIndex)];
					const auto parentBody = std::find_if(
						item.binding.ragdollProfile->bodies.begin(),
						item.binding.ragdollProfile->bodies.end(),
						[parentJointId](const RagdollBodyRecord& value) {
							return value.stableJointId == parentJointId;
						});
					const bool parentAnimationDriven = parentBody
						== item.binding.ragdollProfile->bodies.end()
						|| parentBody->motionType != RagdollMotionType::Dynamic;
					input.isBoundary = parentBody == item.binding.ragdollProfile->bodies.end()
						|| parentAnimationDriven != input.animationDrivenKinematic;
				}
				inputs.push_back(input);
			}
			RagdollBlendResult blended;
			const AnimationStatus status = BlendRagdollPose(
				pose.jointStableIds, item.binding.jointParents, pose.joints,
				mappedPhysicsPose, item.ragdollGeneration, inputs,
				1.0F, blended);
			if(status != AnimationStatus::Ok) return status;
			item.ragdollMotorDrives.clear();
			item.ragdollMotorDrives.reserve(item.binding.ragdollProfile->constraints.size());
			for(const RagdollConstraintRecord& constraint
				: item.binding.ragdollProfile->constraints) {
				const bool hasMotor = std::any_of(
					constraint.motors.begin(), constraint.motors.end(),
					[](const RagdollAxisMotor& motor) {
						return motor.mode != RagdollMotorMode::Disabled;
					});
				if(!hasMotor) continue;
				const auto childBody = std::find_if(
					item.binding.ragdollProfile->bodies.begin(),
					item.binding.ragdollProfile->bodies.end(),
					[&constraint](const RagdollBodyRecord& value) {
						return value.bodyId == constraint.childBodyId;
					});
				if(childBody == item.binding.ragdollProfile->bodies.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const auto childJoint = std::find(
					pose.jointStableIds.begin(), pose.jointStableIds.end(),
					childBody->stableJointId);
				if(childJoint == pose.jointStableIds.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const std::size_t childIndex = static_cast<std::size_t>(
					std::distance(pose.jointStableIds.begin(), childJoint));
				item.ragdollMotorDrives.push_back({constraint.constraintId,
					blended.effectiveMotorStrengths[childIndex]});
			}
			if(item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
				item.ragdollRecoveryBlendComplete = std::all_of(
					item.binding.ragdollProfile->bodies.begin(),
					item.binding.ragdollProfile->bodies.end(),
					[&](const RagdollBodyRecord& body) {
						const auto joint = std::find(
							pose.jointStableIds.begin(), pose.jointStableIds.end(),
							body.stableJointId);
						return joint != pose.jointStableIds.end()
							&& blended.effectiveWeights[static_cast<std::size_t>(
								std::distance(pose.jointStableIds.begin(), joint))] <= 1.0e-4F;
					});
			}
			pose.joints = std::move(blended.modelPose);
			return AnimationStatus::Ok;
		}

		void PrepareFallbackSelection(FrameWorkItem& item) noexcept
		{
			if(item.cacheReused) return;
			const AnimationCompatibilityFingerprint compatibility =
				BuildFallbackCompatibilityFingerprint(item.binding);
			item.fallbackGenerations.animator = item.input.handle;
			item.fallbackGenerations.componentGeneration =
				item.input.descriptor.componentGeneration;
			item.fallbackGenerations.sceneGeneration = frameSceneGeneration;
			item.fallbackGenerations.contentGeneration = item.contentGeneration;
			item.fallbackGenerations.rendererGeneration =
				item.binding.evaluationGenerations.rendererGeneration;
			item.fallbackGenerations.physicsWorldGeneration =
				item.binding.evaluationGenerations.physicsGeneration;
			item.fallbackGenerations.publicationGeneration = item.candidatePoseGeneration;
			item.fallbackGenerations.compatibilityFingerprint = compatibility;

			const bool poseUsable = item.publication && item.publication->snapshot
				&& item.publication->snapshot->pose
				&& item.publication->snapshot->pose->ValidateForPublication() == AnimationStatus::Ok;
			const bool currentUsable = item.evaluationStatus == AnimationStatus::Ok
				&& poseUsable;
			// A runtime shell without a skeleton has no authored bind pose. Its sealed
			// zero-channel pose is nevertheless a complete typed CPU-safe result and is
			// needed to keep lifecycle membership publications monotonic across restart.
			// Skeleton-backed dependency failures remain bind-pose/hold-last candidates,
			// so a compatible prior generation retains precedence after a failed rebind.
			const bool unboundRuntimeShell = !item.binding.skeleton && !item.binding.clip
				&& item.binding.jointStableIds.empty()
				&& item.binding.morphStableIds.empty()
				&& item.binding.curveStableIds.empty();
			const bool cpuFallbackUsable = poseUsable && unboundRuntimeShell;
			AnimationFallbackRequest request;
			request.policy = item.binding.fallbackPolicy;
			request.reason = currentUsable
				? AnimationFallbackReason::None
				: FallbackReasonForStatus(item.evaluationStatus);
			request.expectedGenerations = item.fallbackGenerations;
			request.current = {item.fallbackGenerations, currentUsable, currentUsable, currentUsable};
			request.cpuFallback = {
				item.fallbackGenerations,
				cpuFallbackUsable,
				cpuFallbackUsable,
				cpuFallbackUsable};
			request.prior = DescribeAnimationFallbackPublication(item.priorFallbackPublication);
			request.bindPoseAvailable = poseUsable && item.binding.skeleton;
			item.fallbackDecision = SelectAnimationFallback(request);

			switch(item.fallbackDecision.source) {
			case AnimationFallbackSource::Current:
			case AnimationFallbackSource::CpuDeterministic:
			case AnimationFallbackSource::BindPose:
				item.selectedPublication = item.publication;
				break;
			case AnimationFallbackSource::HeldPriorGeneration:
				item.selectedPublication = item.priorFallbackPublication
					? item.priorFallbackPublication->payload
					: std::shared_ptr<const PublishedAnimatorPose>{};
				break;
			case AnimationFallbackSource::None:
			case AnimationFallbackSource::Disabled:
				item.selectedPublication.reset();
				break;
			}

			if(item.fallbackDecision.source != AnimationFallbackSource::Current) {
				fallbackAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
			}
			if(!item.selectedPublication
				|| (item.fallbackDecision.source != AnimationFallbackSource::Current
					&& item.fallbackDecision.source != AnimationFallbackSource::CpuDeterministic
					&& item.fallbackDecision.source != AnimationFallbackSource::BindPose)) {
				return;
			}
			try {
				auto candidate = std::make_shared<PoseFallbackPublication>();
				candidate->generations = item.fallbackGenerations;
				candidate->source = item.fallbackDecision.source;
				candidate->payload = item.selectedPublication;
				candidate->complete = true;
				candidate->finite = true;
				item.fallbackCandidate = std::move(candidate);
			} catch(const std::bad_alloc&) {
				// A failed latch envelope allocation leaves the current complete runtime
				// generation untouched; protected publication will reject this candidate.
				item.fallbackCandidate.reset();
			}
		}

		static AnimationFallbackGenerationKey BuildLiveFallbackGenerations(
			const RuntimeRecord& record,
			const FrameWorkItem& item,
			std::uint64_t sceneGeneration) noexcept
		{
			AnimationFallbackGenerationKey key;
			key.animator = record.handle;
			key.componentGeneration = record.descriptor.componentGeneration;
			key.sceneGeneration = sceneGeneration;
			key.contentGeneration = record.contentGeneration;
			key.rendererGeneration =
				record.binding.evaluationGenerations.rendererGeneration;
			key.physicsWorldGeneration =
				record.binding.evaluationGenerations.physicsGeneration;
			key.publicationGeneration = item.candidatePoseGeneration;
			key.compatibilityFingerprint =
				BuildFallbackCompatibilityFingerprint(record.binding);
			return key;
		}

		AnimationStatus BlendControllerTransitionSample(
			FrameWorkItem& item,
			CpuClipEvaluator& evaluator,
			CpuClipEvaluationResult& source) const noexcept
		{
			if(item.candidateGraphState.activeTransitionStableId == 0U
				|| !item.transition.ready) {
				return AnimationStatus::Ok;
			}

			try {
				CpuClipEvaluationRequest destinationRequest;
				destinationRequest.tick =
					item.transition.destinationPlayback.AbsoluteTick();
				destinationRequest.loop =
					item.transition.destinationPlayback.Configuration().loop;
				destinationRequest.jointStableIds = item.sampledSourceSkeleton
					? std::span<const std::uint64_t>(
						item.binding.retargetSourceJointStableIds)
					: std::span<const std::uint64_t>(
						item.binding.jointStableIds);
				destinationRequest.morphStableIds = item.binding.morphStableIds;
				destinationRequest.curveStableIds = item.binding.curveStableIds;
				CpuClipEvaluationResult destination;
				const AnimationStatus destinationStatus = evaluator.Evaluate(
					item.transition.destinationClipView,
					destinationRequest,
					destination);
				if(destinationStatus != AnimationStatus::Ok
					|| source.joints.size() != destination.joints.size()
					|| source.morphs.size() != destination.morphs.size()
					|| source.curves.size() != destination.curves.size()) {
					return destinationStatus == AnimationStatus::Ok
						? AnimationStatus::IncompatibleAsset
						: destinationStatus;
				}

				std::vector<AnimationTransform> sourcePose;
				std::vector<AnimationTransform> destinationPose;
				sourcePose.reserve(source.joints.size());
				destinationPose.reserve(destination.joints.size());
				for(std::size_t index = 0U; index < source.joints.size(); ++index) {
					if(source.joints[index].stableJointId
						!= destination.joints[index].stableJointId) {
						return AnimationStatus::IncompatibleAsset;
					}
					sourcePose.push_back(source.joints[index].localTransform);
					destinationPose.push_back(
						destination.joints[index].localTransform);
				}
				const std::span<const AnimationTransform> effectiveSource =
					item.transition.capturedSourcePose
						? std::span<const AnimationTransform>(
							*item.transition.capturedSourcePose)
						: std::span<const AnimationTransform>(sourcePose);
				if(effectiveSource.size() != destinationPose.size()) {
					return AnimationStatus::IncompatibleAsset;
				}

				const float destinationWeight = std::clamp(
					item.candidateGraphState.transitionProgressNormalized,
					0.0F, 1.0F);
				const WeightedLocalPose weighted[] = {
					{effectiveSource, 1.0F - destinationWeight, 0U},
					{destinationPose, destinationWeight, 1U}};
				std::vector<AnimationTransform> blended;
				const AnimationStatus blendStatus = BlendLocalPoses(
					weighted, effectiveSource, blended);
				if(blendStatus != AnimationStatus::Ok
					|| blended.size() != source.joints.size()) {
					return blendStatus == AnimationStatus::Ok
						? AnimationStatus::InternalError : blendStatus;
				}
				for(std::size_t index = 0U; index < blended.size(); ++index) {
					source.joints[index].localTransform = blended[index];
				}

				auto blendNamedValues = [destinationWeight](
					auto& sourceValues,
					const auto& destinationValues) noexcept {
					for(std::size_t index = 0U;
						index < sourceValues.size(); ++index) {
						if(sourceValues[index].stableId
							!= destinationValues[index].stableId
							|| !std::isfinite(sourceValues[index].value)
							|| !std::isfinite(destinationValues[index].value)) {
							return false;
						}
						sourceValues[index].value =
							sourceValues[index].value
								* (1.0F - destinationWeight)
							+ destinationValues[index].value
								* destinationWeight;
					}
					return true;
				};
				if(!blendNamedValues(source.morphs, destination.morphs)
					|| !blendNamedValues(source.curves, destination.curves)) {
					return AnimationStatus::IncompatibleAsset;
				}
				if(destination.fallbackChannelCount
						> std::numeric_limits<std::uint32_t>::max()
							- source.fallbackChannelCount
					|| destination.unavailableChannelCount
						> std::numeric_limits<std::uint32_t>::max()
							- source.unavailableChannelCount) {
					return AnimationStatus::RangeOverflow;
				}
				source.fallbackChannelCount +=
					destination.fallbackChannelCount;
				source.unavailableChannelCount +=
					destination.unavailableChannelCount;
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}

		void EvaluateItem(FrameWorkItem& item) noexcept
		{
			if(!item.planned || item.candidatePoseGeneration == 0U) return;
			if(item.cacheReused) {
				// V2 supplies its own features, so a reused pose can still perform the
				// worker-side search without rebuilding the unchanged pose publication.
				EvaluateMotionMatchQueriesV2(item);
				return;
			}
			if(PollFrameCancellation()) {
				item.evaluationStatus = AnimationStatus::Cancelled;
				EvaluateMotionMatchQueriesV2(item);
				item.evaluated = true;
				PrepareFallbackSelection(item);
				return;
			}
			if(item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
				&& item.gpuLowering.has_value()) {
				// The direct query remains CPU-authoritative even when presentation owns
				// a GPU-only pose. Its inline features require no pose readback.
				EvaluateMotionMatchQueriesV2(item);
				// A complete Visual GPU request owns joint, morph, and curve evaluation.
				// Do not manufacture a CPU pose: doing so both defeats the mode's CPU
				// budget and could let RenderSystem accidentally prefer stale CPU output.
				item.safePose = false;
				item.evaluated = true;
				// GPU-only publication is produced by AnimationPresentationSystem. There is
				// no CPU payload for the fallback latch at this authority boundary.
				return;
			}
			CpuClipEvaluationResult sampled;
			AnimationStatus sampleStatus = item.dependencyStatus;
			CpuClipEvaluator evaluator;
			if(item.dependencyStatus == AnimationStatus::Ok) {
				CpuClipEvaluationRequest request;
				request.tick = item.playback.AbsoluteTick();
				request.loop = item.playback.Configuration().loop;
				request.jointStableIds = item.sampledSourceSkeleton
					? std::span<const std::uint64_t>(
						item.binding.retargetSourceJointStableIds)
					: std::span<const std::uint64_t>(item.binding.jointStableIds);
				request.morphStableIds = item.binding.morphStableIds;
				request.curveStableIds = item.binding.curveStableIds;
				sampleStatus = evaluator.Evaluate(item.clipView, request, sampled);
				if(sampleStatus == AnimationStatus::Ok) {
					sampleStatus = BlendControllerTransitionSample(
						item, evaluator, sampled);
				}
			}
			item.safePose = sampleStatus != AnimationStatus::Ok;
			if(item.sampledSourceSkeleton) {
				item.retargetEvaluationStatus = sampleStatus;
			}
			if(item.evaluationStatus == AnimationStatus::Ok) item.evaluationStatus = sampleStatus;

			AnimationEventTraversalResult traversedEvents;
			if(item.dependencyStatus == AnimationStatus::Ok
				&& !item.transition.capturedSourcePose
				&& item.advance.previousAbsoluteTick != item.advance.currentAbsoluteTick) {
				AnimationEventTraversalRequest traversal;
				traversal.animator = item.input.handle;
				traversal.stateInstanceId = item.binding.stateId;
				traversal.sourceStateId = item.binding.stateId;
				traversal.previousAbsoluteTick = item.advance.previousAbsoluteTick;
				traversal.currentAbsoluteTick = item.advance.currentAbsoluteTick;
				traversal.producerOrderBase = 1U;
				traversal.eventPolicy = item.binding.eventPolicy;
				const AnimationStatus eventStatus = item.eventStream.Traverse(traversal, traversedEvents);
				if(item.evaluationStatus == AnimationStatus::Ok && eventStatus != AnimationStatus::Ok) {
					item.evaluationStatus = eventStatus;
				}
			}
			if(item.dependencyStatus == AnimationStatus::Ok
				&& item.candidateGraphState.activeTransitionStableId != 0U
				&& item.transition.ready
				&& item.transition.destinationAdvance.previousAbsoluteTick
					!= item.transition.destinationAdvance.currentAbsoluteTick) {
				AnimationEventTraversalRequest traversal;
				traversal.animator = item.input.handle;
				traversal.stateInstanceId =
					item.transition.destinationStateId;
				traversal.sourceStateId =
					item.transition.destinationStateId;
				traversal.previousAbsoluteTick =
					item.transition.destinationAdvance.previousAbsoluteTick;
				traversal.currentAbsoluteTick =
					item.transition.destinationAdvance.currentAbsoluteTick;
				// Keep source and destination producer ranges disjoint. The source
				// clip remains first, while the destination's authored order is
				// stable within this high range throughout the transition.
				traversal.producerOrderBase = 1ULL << 32U;
				traversal.eventPolicy = item.binding.eventPolicy;
				AnimationEventTraversalResult destinationEvents;
				const AnimationStatus destinationEventStatus =
					item.transition.destinationEventStream.Traverse(
						traversal, destinationEvents);
				if(destinationEventStatus == AnimationStatus::Ok) {
					try {
						traversedEvents.events.insert(
							traversedEvents.events.end(),
							std::make_move_iterator(
								destinationEvents.events.begin()),
							std::make_move_iterator(
								destinationEvents.events.end()));
					} catch(const std::bad_alloc&) {
						if(item.evaluationStatus == AnimationStatus::Ok) {
							item.evaluationStatus =
								AnimationStatus::InternalError;
						}
					}
				} else if(item.evaluationStatus == AnimationStatus::Ok) {
					item.evaluationStatus = destinationEventStatus;
				}
			}

			try {
				item.pose = std::make_shared<PoseBuffer>();
			} catch(const std::bad_alloc&) {
				item.evaluationStatus = AnimationStatus::InternalError;
				item.evaluated = true;
				PrepareFallbackSelection(item);
				return;
			}
			PoseBufferLayout layout;
			layout.jointCount = item.binding.jointStableIds.size();
			layout.morphCount = item.binding.morphStableIds.size();
			layout.curveCount = item.binding.curveStableIds.size();
			layout.eventCapacity = traversedEvents.events.size();
			AnimationPoseGeneration poseGeneration;
			poseGeneration.animator = item.input.handle;
			poseGeneration.poseGeneration = item.candidatePoseGeneration;
			if(item.binding.skeleton) {
				poseGeneration.skeleton = {
					item.binding.skeleton->assetId,
					item.binding.skeletonGeneration};
			}
			poseGeneration.programGeneration = item.binding.programGeneration;
			poseGeneration.tick = frameTick;
			if(const AnimationStatus status = item.pose->BeginWrite(layout, poseGeneration);
				status != AnimationStatus::Ok) {
				item.evaluationStatus = status;
				item.evaluated = true;
				PrepareFallbackSelection(item);
				return;
			}

			auto local = item.pose->MutableLocalTransforms();
			for(std::size_t index = 0U; index < local.size(); ++index) {
				local[index] = !item.sampledSourceSkeleton && index < sampled.joints.size()
					? sampled.joints[index].localTransform
					: ReferenceTransformForStableId(item, item.binding.jointStableIds[index]);
			}
			auto model = item.pose->MutableModelTransforms();
			BuildModelTransforms(item, local, model);
			if(item.evaluationStatus == AnimationStatus::Ok && item.sampledSourceSkeleton) {
				const AnimationStatus retargetStatus = ApplyRetargetStage(
					item, sampled, local, model);
				item.retargetEvaluationStatus = retargetStatus;
				if(retargetStatus != AnimationStatus::Ok) {
					item.evaluationStatus = retargetStatus;
					item.safePose = true;
				}
			}
			if(item.evaluationStatus == AnimationStatus::Ok
				&& (!item.activeProceduralGoals.empty()
					|| item.ragdollLifecycle == RagdollLifecycleState::Active
					|| item.ragdollLifecycle == RagdollLifecycleState::Recovering)) {
				try {
					GraphPoseContribution advancedPose;
					advancedPose.compatibilityFingerprint = item.binding.skeleton
						? item.binding.skeleton->compatibilityFingerprint : 0U;
					advancedPose.jointCount = static_cast<std::uint32_t>(model.size());
					advancedPose.jointStableIds = item.binding.jointStableIds;
					advancedPose.joints.assign(model.begin(), model.end());
					AnimationStatus advancedStatus = ApplyProceduralStage(item, advancedPose);
					if(advancedStatus == AnimationStatus::Ok) {
						advancedStatus = ApplyRagdollStage(item, advancedPose);
					}
					if(advancedStatus == AnimationStatus::Ok
						&& advancedPose.joints.size() == model.size()
						&& BuildLocalTransformsFromModel(item, advancedPose.joints, local)) {
						std::copy(advancedPose.joints.begin(), advancedPose.joints.end(), model.begin());
					} else {
						item.evaluationStatus = advancedStatus == AnimationStatus::Ok
							? AnimationStatus::InternalError : advancedStatus;
						item.safePose = true;
					}
				} catch(const std::bad_alloc&) {
					item.evaluationStatus = AnimationStatus::InternalError;
					item.safePose = true;
				}
			}
			// Motion search is part of the worker-owned item. Even a production-size
			// database is scored away from the protected publication boundary; only the
			// bounded copied result is committed later.
			EvaluateMotionMatchRequests(item, model);
			EvaluateMotionMatchQueriesV2(item);
			auto morphs = item.pose->MutableMorphValues();
			for(std::size_t index = 0U; index < morphs.size(); ++index) {
				morphs[index] = {
					item.binding.morphStableIds[index],
					index < sampled.morphs.size() ? sampled.morphs[index].value : 0.0F};
			}
			auto curves = item.pose->MutableCurveValues();
			for(std::size_t index = 0U; index < curves.size(); ++index) {
				curves[index] = {
					item.binding.curveStableIds[index],
					index < sampled.curves.size() ? sampled.curves[index].value : 0.0F};
			}
			for(AnimationEventRecord& event : traversedEvents.events) {
				if(const AnimationStatus status = item.pose->AppendEvent(std::move(event));
					status != AnimationStatus::Ok && item.evaluationStatus == AnimationStatus::Ok) {
					item.evaluationStatus = status;
				}
			}

			if(item.dependencyStatus == AnimationStatus::Ok) {
				auto extractClipRootMotion = [&item, &evaluator](
					const AnimationClipAsset* clip,
					const AnimationClipView& view,
					const AnimatorPlayback& playback,
					const AnimatorPlaybackAdvanceResult& advance,
					std::uint64_t stateId,
					std::optional<AnimationRootMotionDelta>& output) {
					output.reset();
					if(clip == nullptr || !clip->rootMotion.enabled
						|| advance.previousAbsoluteTick
							== advance.currentAbsoluteTick) {
						return AnimationStatus::Ok;
					}
					AnimationResolvedClipTime beginTime;
					AnimationResolvedClipTime endTime;
					CpuClipRootMotionPoint beginPoint;
					CpuClipRootMotionPoint endPoint;
					AnimationStatus status = view.ResolveTime(
						advance.previousAbsoluteTick,
						playback.Configuration().loop,
						beginTime);
					if(status != AnimationStatus::Ok) return status;
					status = view.ResolveTime(
						advance.currentAbsoluteTick,
						playback.Configuration().loop,
						endTime);
					if(status != AnimationStatus::Ok) return status;
					status = evaluator.SampleRootMotionPoint(
						view, beginTime, beginPoint);
					if(status != AnimationStatus::Ok) return status;
					status = evaluator.SampleRootMotionPoint(
						view, endTime, endPoint);
					if(status != AnimationStatus::Ok) return status;
					if(!beginPoint.enabled || !endPoint.enabled) {
						return AnimationStatus::Ok;
					}

					AnimationRootMotionExtractionRequest extraction;
					extraction.animator = item.input.handle;
					extraction.sourceBeginTick =
						advance.previousAbsoluteTick;
					extraction.sourceEndTick =
						advance.currentAbsoluteTick;
					extraction.begin = {
						beginPoint.translation,
						beginPoint.yawRadians,
						beginTime.loopIndex};
					extraction.end = {
						endPoint.translation,
						endPoint.yawRadians,
						endTime.loopIndex};
					extraction.accumulatedCycleTranslation =
						clip->rootMotion.accumulatedCycleTranslation;
					extraction.accumulatedCycleYawRadians =
						clip->rootMotion.accumulatedCycleYawRadians;
					extraction.sourceStateId = stateId;
					extraction.generation = item.candidatePoseGeneration;
					extraction.policy = item.binding.rootMotionPolicy;
					AnimationRootMotionDelta candidate;
					status = ExtractRootMotion(extraction, candidate);
					if(status == AnimationStatus::Ok) {
						output = candidate;
					}
					return status;
				};

				std::optional<AnimationRootMotionDelta> sourceRootMotion;
				AnimationStatus rootStatus = extractClipRootMotion(
					item.binding.clip.get(),
					item.clipView,
					item.playback,
					item.advance,
					item.candidateGraphState.activeTransitionStableId != 0U
						? item.candidateGraphState.transitionSourceStateId
						: item.binding.stateId,
					sourceRootMotion);
				if(rootStatus == AnimationStatus::Ok
					&& item.transition.capturedSourceRootMotion) {
					sourceRootMotion =
						item.transition.capturedSourceRootMotion;
					sourceRootMotion->animator = item.input.handle;
					sourceRootMotion->sourceBeginTick =
						item.advance.previousAbsoluteTick;
					sourceRootMotion->sourceEndTick =
						item.advance.currentAbsoluteTick;
					sourceRootMotion->sourceStateId =
						item.transition.sourceStateId;
					sourceRootMotion->generation =
						item.candidatePoseGeneration;
					sourceRootMotion->policy =
						item.binding.rootMotionPolicy;
				}
				std::optional<AnimationRootMotionDelta> destinationRootMotion;
				if(rootStatus == AnimationStatus::Ok
					&& item.candidateGraphState.activeTransitionStableId != 0U
					&& item.transition.ready) {
					rootStatus = extractClipRootMotion(
						item.transition.destination.clip.get(),
						item.transition.destinationClipView,
						item.transition.destinationPlayback,
						item.transition.destinationAdvance,
						item.transition.destinationStateId,
						destinationRootMotion);
				}
				if(rootStatus == AnimationStatus::Ok
					&& (sourceRootMotion || destinationRootMotion)) {
					AnimationRootMotionDelta blended = sourceRootMotion
						? *sourceRootMotion : *destinationRootMotion;
					if(item.candidateGraphState.activeTransitionStableId != 0U
						&& item.transition.ready) {
						const float destinationWeight = std::clamp(
							item.candidateGraphState
								.transitionProgressNormalized,
							0.0F, 1.0F);
						const float sourceWeight = 1.0F - destinationWeight;
						const AnimationRootMotionDelta empty{
							.animator = item.input.handle,
							.generation = item.candidatePoseGeneration,
							.policy = item.binding.rootMotionPolicy};
						const AnimationRootMotionDelta& source =
							sourceRootMotion ? *sourceRootMotion : empty;
						const AnimationRootMotionDelta& destination =
							destinationRootMotion
								? *destinationRootMotion : empty;
						blended.localTranslation = {
							source.localTranslation.x * sourceWeight
								+ destination.localTranslation.x
									* destinationWeight,
							source.localTranslation.y * sourceWeight
								+ destination.localTranslation.y
									* destinationWeight,
							source.localTranslation.z * sourceWeight
								+ destination.localTranslation.z
									* destinationWeight};
						blended.worldTranslation = {
							source.worldTranslation.x * sourceWeight
								+ destination.worldTranslation.x
									* destinationWeight,
							source.worldTranslation.y * sourceWeight
								+ destination.worldTranslation.y
									* destinationWeight,
							source.worldTranslation.z * sourceWeight
								+ destination.worldTranslation.z
									* destinationWeight};
						blended.localYawRadians =
							source.localYawRadians * sourceWeight
							+ destination.localYawRadians
								* destinationWeight;
						blended.worldYawRadians =
							source.worldYawRadians * sourceWeight
							+ destination.worldYawRadians
								* destinationWeight;
						blended.sourceStateId = destinationWeight >= 0.5F
							? item.transition.destinationStateId
							: item.transition.sourceStateId;
						blended.accumulatedCycles =
							destinationWeight >= 0.5F
								? destination.accumulatedCycles
								: source.accumulatedCycles;
					}
					rootStatus = ApplyRetargetRootMotionPolicy(
						item, blended);
					if(rootStatus == AnimationStatus::Ok) {
						// Ignore remains a complete copied result. Keeping its source
						// interval and generations proves that policy intentionally
						// discarded the blended transition delta.
						rootStatus = item.pose->SetRootMotion(blended);
					}
				}
				if(rootStatus != AnimationStatus::Ok
					&& item.evaluationStatus == AnimationStatus::Ok) {
					item.evaluationStatus = rootStatus;
				}
			}

			if(const AnimationStatus sealStatus = item.pose->Seal(); sealStatus != AnimationStatus::Ok) {
				item.evaluationStatus = sealStatus;
				item.evaluated = true;
				PrepareFallbackSelection(item);
				return;
			}
			try {
				AnimationSnapshot snapshot;
				snapshot.animator = item.input.handle;
				snapshot.publicationGeneration = item.candidatePoseGeneration;
				snapshot.tick = frameTick;
				snapshot.playback = MakePublishedPlaybackSnapshot(item.playback.Snapshot());
				snapshot.playback.currentStateId = item.binding.stateId;
				snapshot.playback.previousStateId =
					item.candidateGraphState.activeTransitionStableId != 0U
						? item.candidateGraphState.transitionSourceStateId : 0U;
				snapshot.playback.transitionProgress =
					item.candidateGraphState.transitionProgressNormalized;
				if(item.binding.evaluationProgram) {
					for(std::size_t layerIndex = 0U;
						layerIndex < item.binding.evaluationProgram->layers.size(); ++layerIndex) {
						const EvaluationLayer& layer = item.binding.evaluationProgram->layers[layerIndex];
						snapshot.playback.activeLayers.push_back({
							static_cast<std::uint32_t>(layerIndex),
							item.binding.stateId,
							layerIndex < item.layerWeights.size()
								? item.layerWeights[layerIndex] : layer.defaultWeight,
							item.input.tick - item.candidateGraphState.stateEnteredTick});
					}
				}
				snapshot.playback.latestStatus = item.evaluationStatus;
				snapshot.playback.parameters = item.parameters;
				snapshot.pose = item.pose;
				auto immutableSnapshot = std::make_shared<const AnimationSnapshot>(std::move(snapshot));
				PublishedAnimatorPose published;
				published.animator = item.input.handle;
				published.ownerObjectId = item.input.descriptor.ownerObjectId;
				published.objectGeneration = item.sourceObjectGeneration;
				published.componentGeneration = item.input.descriptor.componentGeneration;
				published.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
				published.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
				published.authorityPublicationGeneration = item.candidateAuthorityPublicationGeneration;
				published.controller = item.input.descriptor.controller;
				published.jointStableIds = item.binding.jointStableIds;
				if(item.binding.clip) {
					published.clip = {item.binding.clip->assetId, item.binding.clipGeneration};
				}
				if(item.binding.skeleton) {
					published.skeleton = {item.binding.skeleton->assetId, item.binding.skeletonGeneration};
					published.skeletonAsset = item.binding.skeleton;
					published.skeletonCompatibilityFingerprint = item.binding.skeleton->compatibilityFingerprint;
				}
				published.status = item.evaluationStatus;
				published.requestedExecutionPolicy = item.binding.requestedExecutionPolicy;
				published.effectiveExecutionMode = item.executionDecision.effectiveMode;
				published.rootMotionPolicy = item.binding.rootMotionPolicy;
				published.updatePolicy = item.binding.updatePolicy;
				published.eventPolicy = item.binding.eventPolicy;
				published.cullingPolicy = item.binding.cullingPolicy;
				published.safePose = item.safePose;
				published.streamedClipPayloadsActive =
					item.binding.streamedSegmentPayloads != nullptr;
				published.poseMemoryReservation = framePoseReservation;
				published.snapshot = std::move(immutableSnapshot);
				item.publication = std::make_shared<const PublishedAnimatorPose>(std::move(published));
				item.evaluated = true;
				PrepareFallbackSelection(item);
			} catch(const std::bad_alloc&) {
				item.evaluationStatus = AnimationStatus::InternalError;
				item.evaluated = true;
				PrepareFallbackSelection(item);
			}
		}

		void EvaluatePartition(
			std::size_t partitionIndex,
			std::size_t partitionCount) noexcept
		{
			if(partitionCount == 0U || partitionIndex >= partitionCount
				|| partitionIndex >= std::numeric_limits<std::uint32_t>::max()
				|| !EnsureAnimationWorkPlanFinalized()
				|| workPlan.status != AnimationStatus::Ok
				|| ValidateAnimationWorkPlan(workPlan) != AnimationStatus::Ok
				|| workRangeLedger == nullptr) return;
			const std::uint32_t workerOrdinal = static_cast<std::uint32_t>(partitionIndex + 1U);
			for(std::size_t rangeIndex = partitionIndex;
				rangeIndex < workPlan.ranges.size();
				rangeIndex += partitionCount) {
				if(!workRangeLedger->TryBegin(rangeIndex, workerOrdinal)) continue;
				const AnimationWorkRange& range = workPlan.ranges[rangeIndex];
				for(std::size_t scheduledIndex = range.itemBegin;
					scheduledIndex < range.itemEnd;
					++scheduledIndex) {
					const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
					AnimationWorkCompletion& completion = workCompletions[scheduledIndex];
					completion.scheduledItemIndex = scheduledIndex;
					completion.animator = scheduled.item.animator;
					completion.sourceGeneration = scheduled.item.sourceGeneration;
					if(PollFrameCancellation()) {
						completion.status = AnimationStatus::Cancelled;
						continue;
					}
					if(scheduled.originalInputIndex >= frameWork.size()
						|| scheduled.scratchOffset > frameScratch.size()
						|| sizeof(AnimationFrameScratchRecord)
							> frameScratch.size() - scheduled.scratchOffset) {
						completion.status = AnimationStatus::Rejected;
						continue;
					}

					AnimationFrameScratchRecord scratchRecord;
					std::memcpy(
						&scratchRecord,
						frameScratch.data() + scheduled.scratchOffset,
						sizeof(scratchRecord));
					if(scratchRecord.scheduledItemIndex != scheduledIndex
						|| scratchRecord.sourceGeneration != scheduled.item.sourceGeneration) {
						completion.status = AnimationStatus::Rejected;
						continue;
					}

					FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
					EvaluateItem(item);
					scratchRecord.evaluationStatus = item.evaluationStatus;
					scratchRecord.evaluated = item.evaluated;
					std::memcpy(
						frameScratch.data() + scheduled.scratchOffset,
						&scratchRecord,
						sizeof(scratchRecord));
					completion.status = item.evaluated
						? AnimationStatus::Ok
						: AnimationStatus::Rejected;
					if(item.evaluated) {
						evaluatedAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
					}
				}
				if(!workRangeLedger->Complete(rangeIndex, workerOrdinal)
					&& PollFrameCancellation()) {
					workRangeLedger->CancelUnclaimed();
					return;
				}
			}
		}

		bool PublishFrameLocked(
			std::size_t animatorCount,
			std::uint64_t frameEpoch,
			std::uint64_t authorityGeneration) noexcept
		{
			if(authorityGeneration == 0U || frameWork.size() != animatorCount
				|| plannedAnimatorCount.load(std::memory_order_acquire) != animatorCount
				|| evaluatedAnimatorCount.load(std::memory_order_acquire) != animatorCount
				|| PollFrameCancellation()
				|| workRangeLedger == nullptr || !workRangeLedger->AllCompleted()
				|| ValidateAnimationWorkPlan(workPlan) != AnimationStatus::Ok
				|| BuildDeterministicAnimationPublicationOrder(
					workPlan, workCompletions, deterministicPublicationOrder) != AnimationStatus::Ok) {
				return false;
			}
			deterministicPublicationValidated = true;
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				if(scheduledIndex >= workPlan.items.size()) return false;
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				if(scheduled.originalInputIndex >= frameWork.size()) return false;
				FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				const RuntimeRecord* record = FindRuntimeRecordLocked(item.input.handle);
				if(record != nullptr) {
					item.liveFallbackGenerations = BuildLiveFallbackGenerations(
						*record, item, ObjectManager::instance().GetSceneGeneration());
				}
				const bool preservesPrior = item.cacheReused
					|| item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
				if(record == nullptr || !instances->IsAlive(item.input.handle)
					|| record->descriptor.componentGeneration != item.input.descriptor.componentGeneration
					|| (preservesPrior
						? item.selectedPublication == nullptr
							|| item.selectedPublication->snapshot == nullptr
							|| item.selectedPublication->snapshot->publicationGeneration
								!= record->poseGeneration
						: item.candidatePoseGeneration != record->poseGeneration + 1U)
					|| item.candidateAuthorityPublicationGeneration != authorityGeneration
					|| !item.evaluated) {
					return false;
				}
				const bool gpuVisualOnly = item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::GpuVisual && item.gpuLowering.has_value();
				if(!gpuVisualOnly && (!item.selectedPublication
					|| !item.selectedPublication->snapshot
					|| !item.selectedPublication->snapshot->pose
					|| item.selectedPublication->snapshot->pose->ValidateForPublication()
						!= AnimationStatus::Ok)) {
					return false;
				}
				if((item.cacheReused
					|| item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration)
					&& (!item.priorFallbackPublication
						|| !item.priorFallbackPublication->generations.IsHoldCompatibleWith(
							item.liveFallbackGenerations))) {
					return false;
				}
				const bool publishesFallbackCandidate =
					item.fallbackDecision.source == AnimationFallbackSource::Current
					|| item.fallbackDecision.source == AnimationFallbackSource::CpuDeterministic
					|| item.fallbackDecision.source == AnimationFallbackSource::BindPose;
				if(!gpuVisualOnly && !item.cacheReused && !preservesPrior
					&& (item.fallbackDecision.status == AnimationFallbackDecisionStatus::InvalidArgument
						|| !publishesFallbackCandidate || !item.fallbackCandidate
						|| !item.fallbackCandidate->IsUsable()
						|| item.fallbackCandidate->generations != item.fallbackGenerations)) {
					return false;
				}
				if(item.fallbackCandidate) {
					const AnimationFallbackPublishStatus fallbackPreflight =
						record->fallbackPublication.ValidatePublication(
							item.fallbackCandidate, item.liveFallbackGenerations);
					if(fallbackPreflight != AnimationFallbackPublishStatus::Published
						&& fallbackPreflight != AnimationFallbackPublishStatus::AlreadyPublished) {
						return false;
					}
				}
			}
			// No runtime record changes until the complete candidate set has passed.
			// This is the protected all-or-none visibility point for worker results.
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				RuntimeRecord* record = FindRuntimeRecordLocked(item.input.handle);
				if(item.fallbackCandidate) {
					const AnimationFallbackPublishStatus fallbackStatus =
						record->fallbackPublication.TryPublish(
							item.fallbackCandidate, item.liveFallbackGenerations);
					// The protected preflight above proves this cannot fail without an
					// ownership violation. Never introduce a fallible return after the first
					// live record mutation, because the authority batch is all-or-none.
					assert(fallbackStatus == AnimationFallbackPublishStatus::Published
						|| fallbackStatus == AnimationFallbackPublishStatus::AlreadyPublished);
					(void)fallbackStatus;
				}
				record->playback = item.playback;
				record->eventStream = item.eventStream;
				record->graphState = item.candidateGraphState;
				record->transition = item.transition;
				record->binding.stateId = item.binding.stateId;
				record->binding.clip = item.binding.clip;
				record->binding.clipGeneration = item.binding.clipGeneration;
				record->binding.clipDeterministicContentFingerprint =
					item.binding.clipDeterministicContentFingerprint;
				record->binding.residentSegments = item.binding.residentSegments;
				record->binding.streamedSegmentPayloads =
					item.binding.streamedSegmentPayloads;
				if(item.retargetActive) {
					record->latestRetargetMetrics = item.retargetMetrics;
					record->latestRetargetDeterministicHash =
						item.retargetDeterministicHash;
				} else {
					record->latestRetargetMetrics = {};
					record->latestRetargetDeterministicHash = 0U;
				}
				record->poseGeneration = item.selectedPublication && item.selectedPublication->snapshot
					? item.selectedPublication->snapshot->publicationGeneration
					: item.candidatePoseGeneration;
				record->stateGeneration = item.candidateStateGeneration;
				record->parameterGeneration = item.candidateParameterGeneration;
				record->executionModeGeneration = item.candidateExecutionModeGeneration;
				record->executionModeState = item.executionDecision.nextState;
				record->parameters = item.parameters;
				record->layerWeights = item.layerWeights;
				record->appliedMotionQueryId = item.appliedMotionQueryId;
				if(item.ragdollPhysicsPose) {
					// Adopt the exact immutable physics step consumed by this worker. A
					// later motor or recovery request therefore cannot target an old tick.
					record->ragdollGeneration = item.ragdollGeneration;
				}
				// GPU Visual has no CPU pose by contract. Clearing an earlier CPU
				// publication prevents render collection from preferring it over the
				// same frame's owner-mapped immutable GPU packet.
				const std::shared_ptr<const PublishedAnimatorPose> priorPublication =
					std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
				if(priorPublication && priorPublication->snapshot
					&& item.selectedPublication && item.selectedPublication->snapshot
					&& priorPublication != item.selectedPublication
					&& priorPublication->snapshot->tick < item.selectedPublication->snapshot->tick) {
					record->previousPublishedPose = priorPublication;
				}
				std::atomic_store_explicit(&record->publishedPose,
					item.executionDecision.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
						&& item.gpuLowering.has_value()
						? std::shared_ptr<const PublishedAnimatorPose>{} : item.selectedPublication,
					std::memory_order_release);

				if(item.cacheEligible && item.fallbackDecision.source == AnimationFallbackSource::Current
					&& item.evaluationStatus == AnimationStatus::Ok
					&& item.selectedPublication && item.selectedPublication->snapshot) {
					AnimationEvaluationCacheKey storedKey = item.cacheKey;
					storedKey.poseGeneration = item.selectedPublication->snapshot->publicationGeneration;
					AnimationEvaluationCacheValue cacheValue;
					cacheValue.sourceFrameGeneration = item.candidateAuthorityPublicationGeneration;
					cacheValue.publicationGeneration =
						item.selectedPublication->authorityPublicationGeneration;
					cacheValue.poseToken = item.selectedPublication->snapshot->publicationGeneration;
					(void)evaluationCache.Store(storedKey, cacheValue);
				}
			}

			// Visual workers produce only copied motor scales. The next protected
			// PhysicsCommandCommit owns provider mutation, preserving one Jolt authority.
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				RuntimeRecord* record = FindRuntimeRecordLocked(item.input.handle);
				if(record == nullptr || !item.ragdollGeneration.IsValid()
					|| (item.ragdollLifecycle != RagdollLifecycleState::Active
						&& item.ragdollLifecycle != RagdollLifecycleState::Recovering)) {
					continue;
				}
				record->ragdollMotorDriveCount = static_cast<std::uint32_t>(
					std::min<std::size_t>(item.ragdollMotorDrives.size(),
						std::numeric_limits<std::uint32_t>::max()));
				if(!item.ragdollMotorDrives.empty()) {
					RagdollPhysicsRequest motors;
					motors.kind = RagdollPhysicsRequestKind::UpdateMotors;
					motors.ragdollId = record->ragdollId;
					motors.generation = item.ragdollGeneration;
					motors.expectedCurrentGeneration = item.ragdollGeneration;
					motors.motorDrives = std::move(item.ragdollMotorDrives);
					motors.correlationId = record->ragdollId
						^ authorityGeneration ^ 0x4D4F544F52000000ULL;
					if(motors.correlationId == 0U) motors.correlationId = 1U;
					motors.producerOrder = 1U;
					record->ragdollMotorStatus = Physics::PhysicsSystem::Instance()
						.SubmitRagdollPhysicsRequest(std::move(motors));
				}
				if(item.ragdollRecoveryBlendComplete
					&& record->pendingRagdollCorrelationId == 0U) {
					RagdollGenerationStamp generation = item.ragdollGeneration;
					generation.publicationGeneration = authorityGeneration;
					generation.fixedStep = std::max<AnimationFixedTick>(1, frameTick);
					RagdollPhysicsRequest deactivate;
					deactivate.kind = RagdollPhysicsRequestKind::Deactivate;
					deactivate.ragdollId = record->ragdollId;
					deactivate.generation = generation;
					deactivate.expectedCurrentGeneration = item.ragdollGeneration;
					deactivate.correlationId = record->ragdollId
						^ authorityGeneration ^ 0x5245434F56455259ULL;
					if(deactivate.correlationId == 0U) deactivate.correlationId = 1U;
					deactivate.producerOrder = 2U;
					const AnimationStatus queued = Physics::PhysicsSystem::Instance()
						.SubmitRagdollPhysicsRequest(std::move(deactivate));
					if(queued == AnimationStatus::Queued) {
						record->pendingRagdollPreviousGeneration =
							record->ragdollGeneration;
						record->pendingRagdollPreviousLifecycle =
							record->ragdollLifecycle;
						record->pendingRagdollPreviousProfileId =
							record->activeRagdollProfileId;
						record->pendingRagdollPreviousBlendWeight =
							record->ragdollBlendWeight;
						record->ragdollGeneration = generation;
						record->ragdollLifecycle = RagdollLifecycleState::TearingDown;
						record->pendingRagdollCorrelationId = record->ragdollId
							^ authorityGeneration ^ 0x5245434F56455259ULL;
						if(record->pendingRagdollCorrelationId == 0U) {
							record->pendingRagdollCorrelationId = 1U;
						}
						record->pendingRagdollOperation =
							NativeAnimationRagdollOperation::Deactivate;
					}
				}
			}

			// Procedural observations are diagnostic side publications. They become
			// visible only after the pose authority batch commits, so a result can
			// never advertise a generation whose pose was rejected during preflight.
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				const FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				if(!item.proceduralStateObserved) continue;
				(void)proceduralCommands->PublishResults(
					{item.input.handle.slotIndex, item.input.handle.generation},
					item.proceduralResults, *instances);
			}

			// An active retarget publishes the evaluator's measured result, not merely
			// the earlier activation acknowledgement. This copied diagnostic follows the
			// committed pose and is therefore safe for managed/editor observations.
			if(advancedCommands != nullptr) {
				for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
					const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
					const FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
					if(!item.retargetActive || !item.sampledSourceSkeleton) continue;
					NativeAnimationRetargetSnapshot snapshot{};
					snapshot.header = MakeNativeHeader(
						NativeAnimationRecordKind::RetargetSnapshot, sizeof(snapshot));
					snapshot.status = ToNativeStatus(item.retargetEvaluationStatus);
					snapshot.animator = {
						item.input.handle.slotIndex, item.input.handle.generation};
					snapshot.profileAssetId = item.activeRetargetProfileId;
					snapshot.profileGeneration = item.activeRetargetProfileGeneration;
					snapshot.sourceSkeletonGeneration =
						item.binding.retargetSourceSkeletonGeneration;
					snapshot.targetSkeletonGeneration = item.binding.skeletonGeneration;
					snapshot.poseGeneration = item.candidatePoseGeneration;
					snapshot.contactErrorMeters =
						item.retargetMetrics.constraints.maximumContactErrorMeters;
					snapshot.reachableIkErrorMeters =
						item.retargetMetrics.constraints.maximumEndEffectorErrorMeters;
					snapshot.observationTick = item.input.tick;
					snapshot.publicationGeneration = authorityGeneration;
					(void)advancedCommands->Publish(std::move(snapshot));
				}
			}

			// Runtime records become visible as one protected batch before any dependent
			// attachment or root-motion side effect. Reused/held generations preserve the
			// exact prior attachment publication and never relabel it as fresh work.
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				const FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				if(item.cacheReused
					|| item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration) {
					continue;
				}
				bool attachmentPublished = false;
				if(item.selectedPublication && item.selectedPublication->snapshot
					&& item.selectedPublication->snapshot->pose && item.binding.skeleton
					&& item.sourceObjectGeneration != 0U
					&& item.sourceObjectWorldTransformCaptured) {
					AnimationBoneSocketSnapshotRequest request;
					request.animator = item.input.handle;
					request.sourceObjectId = item.input.descriptor.ownerObjectId;
					request.sourceObjectGeneration = item.sourceObjectGeneration;
					request.sourceComponentGeneration = item.input.descriptor.componentGeneration;
					request.sourceTick = item.selectedPublication->snapshot->tick;
					request.publicationGeneration = item.selectedPublication->snapshot->publicationGeneration;
					request.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
					request.pose = item.selectedPublication->snapshot->pose;
					request.skeleton = item.binding.skeleton;
					AnimationBoneSocketSnapshot attachmentSnapshot;
					attachmentPublished =
						CaptureAnimationBoneSocketSnapshot(request, attachmentSnapshot) == AnimationStatus::Ok
						&& AnimationAttachmentSnapshotPublisher::Instance().Publish(
							std::move(attachmentSnapshot));
				}
				// GPU-only, stale-source, or failed captures must retire an older socket
				// generation so copied gameplay reads cannot silently mix publications.
				if(!attachmentPublished) {
					AnimationAttachmentSnapshotPublisher::Instance().Retire(item.input.handle);
				}
			}

			// Route every copied root interval only after the complete pose set commits.
			// Explicit gameplay requests have deterministic precedence over authored
			// automatic policy. Both pass through the same reservation ledger, so a
			// later request for the same mutating generation is rejected, never applied
			// twice. Worker submissions remain copied until this protected loop.
			for(const AnimationRootMotionApplicationRequest& request : pendingRootMotionApplications) {
				(void)RouteRootMotionApplication(
					request,
					ObjectManager::instance(),
					TerraEngine::Scene::SceneMutationQueue::Instance(),
					TerraEngine::Physics::PhysicsSystem::Instance(),
					request.mode == AnimationRootMotionApplicationMode::ApplyToTransform
						? AnimationRootMotionSceneVisibility::ImmediateProtectedCommit
						: AnimationRootMotionSceneVisibility::DeferredQueue,
					true);
			}
			pendingRootMotionApplications.clear();

			// The automatic producer uses one monotonically ordered identity, so a mode
			// switch or duplicate publication cannot apply the same generation twice.
			constexpr std::uint32_t kAutomaticRootMotionProducerId = 0xA11A0001U;
			for(const std::size_t scheduledIndex : deterministicPublicationOrder) {
				const AnimationScheduledWorkItem& scheduled = workPlan.items[scheduledIndex];
				const FrameWorkItem& item = frameWork[scheduled.originalInputIndex];
				if(item.cacheReused
					|| item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration
					|| !item.selectedPublication || !item.selectedPublication->snapshot
					|| !item.selectedPublication->snapshot->pose) continue;
				const std::optional<AnimationRootMotionDelta>& delta =
					item.selectedPublication->snapshot->pose->RootMotionDelta();
				if(!delta) continue;
				AnimationRootMotionApplicationRequest request;
				request.delta = *delta;
				request.ownerObjectId = item.input.descriptor.ownerObjectId;
				request.objectGeneration = item.sourceObjectGeneration;
				request.componentGeneration = item.input.descriptor.componentGeneration;
				request.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
				request.sourceObjectWorldTransformCaptured = item.sourceObjectWorldTransformCaptured;
				switch(item.binding.rootMotionPolicy) {
				case AnimatorRootMotionPolicy::Ignore:
					request.mode = AnimationRootMotionApplicationMode::Ignore;
					break;
				case AnimatorRootMotionPolicy::ExtractOnly:
					request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
					break;
				case AnimatorRootMotionPolicy::ApplyToObject:
					request.mode = AnimationRootMotionApplicationMode::ApplyToTransform;
					break;
				case AnimatorRootMotionPolicy::ApplyToCharacter:
					request.mode = AnimationRootMotionApplicationMode::ApplyToCharacter;
					break;
				default:
					request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
					break;
				}
				const bool activatesRagdollAtThisBoundary = std::any_of(
					capturedRagdollRequests.begin(), capturedRagdollRequests.end(),
					[&item](const NativeAnimationRagdollRequest& ragdollRequest) {
						return ragdollRequest.animator.slotIndex == item.input.handle.slotIndex
							&& ragdollRequest.animator.generation == item.input.handle.generation
							&& ragdollRequest.operation
								== NativeAnimationRagdollOperation::Activate;
					});
				if(item.ragdollLifecycle != RagdollLifecycleState::Inactive
					|| activatesRagdollAtThisBoundary) {
					// Keep extracting the authored delta for events, recording, rollback,
					// replay, and diagnostics, but never move the object/character while
					// Jolt owns ragdoll world motion. Suppressing the activation boundary
					// also keeps its sealed animation pose aligned with the live object.
					request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
				}
				if(automaticRootMotionSequence == std::numeric_limits<std::uint64_t>::max()) {
					// Sequence exhaustion is fail-closed and still observable. Reusing an
					// ordered producer identity could double-apply an earlier publication.
					AnimationRootMotionApplicationResult exhausted;
					exhausted.animator = delta->animator;
					exhausted.status = AnimationStatus::RangeOverflow;
					exhausted.disposition = AnimationRootMotionDisposition::Rejected;
					exhausted.generation = delta->generation;
					exhausted.sourceBeginTick = delta->sourceBeginTick;
					exhausted.sourceEndTick = delta->sourceEndTick;
					exhausted.localTranslation = delta->localTranslation;
					exhausted.localYawRadians = delta->localYawRadians;
					exhausted.ownerObjectId = request.ownerObjectId;
					exhausted.objectGeneration = request.objectGeneration;
					exhausted.componentGeneration = request.componentGeneration;
					exhausted.sourceStateId = delta->sourceStateId;
					exhausted.accumulatedCycles = delta->accumulatedCycles;
					exhausted.layerIndex = delta->layerIndex;
					exhausted.authoredPolicy = delta->policy;
					exhausted.applicationMode = request.mode;
					exhausted.producerId = kAutomaticRootMotionProducerId;
					exhausted.producerSequence = automaticRootMotionSequence;
					exhausted.correlationId = automaticRootMotionSequence;
					exhausted.idempotencyKey = automaticRootMotionSequence;
					exhausted.publicationGeneration = authorityGeneration;
					try {
						AnimationRootMotionResultPublisher::Instance().PublishImmediate(std::move(exhausted));
					} catch(...) {
						// Retained diagnostics are optional under memory exhaustion; the
						// protected authority boundary itself must remain non-throwing.
					}
					continue;
				}
				request.producerId = kAutomaticRootMotionProducerId;
				request.producerSequence = ++automaticRootMotionSequence;
				request.correlationId = request.producerSequence;
				request.idempotencyKey = request.producerSequence;
				request.publicationGeneration = authorityGeneration;
				(void)RouteRootMotionApplication(
					request,
					ObjectManager::instance(),
					TerraEngine::Scene::SceneMutationQueue::Instance(),
					TerraEngine::Physics::PhysicsSystem::Instance(),
					request.mode == AnimationRootMotionApplicationMode::ApplyToTransform
						? AnimationRootMotionSceneVisibility::ImmediateProtectedCommit
						: AnimationRootMotionSceneVisibility::DeferredQueue);
			}
			lastPublishedFrameDeclarationEpoch = frameEpoch;
			return true;
		}

		AnimationDiagnosticsSnapshot BuildActiveDiagnosticsLocked(
			std::uint64_t publicationGeneration,
			std::size_t detailedRecordCount = 0U) const noexcept
		{
			AnimationDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = publicationGeneration;
			snapshot.fixedTick = frameTick;
			// Summary counters remain cheap on every active frame. Only the detail
			// level and bounded record count reflect a separately requested copied
			// publication; row construction itself happens in the method below.
			snapshot.detailLevel = detailedRecordCount == 0U
				? AnimationDiagnosticsDetailLevel::Summary
				: AnimationDiagnosticsDetailLevel::Detailed;
			snapshot.dormant = false;
			AnimationDiagnosticsCounters& counters = snapshot.counters;
			counters.activeAnimators = frameWork.size();
			counters.schedulerMembershipChecks = 1U;
			counters.workerJobs = workPlan.executionPolicy
				== AnimationWorkExecutionPolicy::WorkerTasks
					? workPlan.ranges.size() : 0U;
			counters.commandQueueDepth = commands == nullptr ? 0U : commands->PendingCount();
			counters.detailedRecords = detailedRecordCount;
			counters.fallbackCount = fallbackAnimatorCount.load(std::memory_order_acquire);

			for(const FrameWorkItem& item : frameWork) {
				if(item.evaluated) ++counters.evaluatedAnimators;
				const AnimatorPlaybackRunState runState = item.playback.RunState();
				if(runState == AnimatorPlaybackRunState::Paused
					|| item.binding.updatePolicy == AnimatorUpdatePolicy::Manual) {
					++counters.suspendedAnimators;
				}
				const bool successfulGpuVisual =
					item.executionDecision.effectiveMode
						== AnimationEffectiveExecutionMode::GpuVisual
					&& item.gpuLowering.has_value()
					&& item.evaluationStatus == AnimationStatus::Ok
					&& !item.safePose;
				if(item.evaluationStatus != AnimationStatus::Ok || item.safePose
					|| (!successfulGpuVisual
						&& item.fallbackDecision.source
							!= AnimationFallbackSource::Current)) {
					++counters.degradedAnimators;
				}
				if(item.evaluationStatus == AnimationStatus::StaleHandle) {
					++counters.staleWorkItems;
				}
				if(item.pose) ++counters.poseAllocations;
				if(!item.binding.gpuExecution.resourcesResident) ++counters.residencyMisses;
				else ++counters.residencyHits;
				if(item.executionDecision.modeChanged) ++counters.modeSwitchCount;

				const std::uint64_t jointCount = item.binding.jointStableIds.size();
				switch(item.executionDecision.effectiveMode) {
				case AnimationEffectiveExecutionMode::GpuVisual:
					++counters.gpuVisualAnimators;
					counters.gpuEvaluatedJoints += jointCount;
					break;
				case AnimationEffectiveExecutionMode::HybridAuthoritative:
					++counters.hybridAuthoritativeAnimators;
					counters.cpuSampledJoints += jointCount;
					counters.gpuEvaluatedJoints += jointCount;
					break;
				case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
				default:
					++counters.cpuFallbackAnimators;
					counters.cpuSampledJoints += jointCount;
					break;
				}
				if(item.gpuLowering.has_value()) {
					++counters.gpuDispatches;
					++counters.gpuBatches;
				}
				if(item.ragdollLifecycle == RagdollLifecycleState::Active
					|| item.ragdollLifecycle == RagdollLifecycleState::Recovering) {
					++counters.physicsHandoffs;
				}
				if(item.selectedPublication && item.selectedPublication->snapshot
					&& item.selectedPublication->snapshot->pose) {
					const PoseBuffer& pose = *item.selectedPublication->snapshot->pose;
					counters.eventQueueDepth += pose.Events().size();
					counters.managedCallbacks += pose.Events().size();
					if(pose.RootMotionDelta().has_value()) ++counters.rootMotionQueueDepth;
				}
			}
			for(const AnimationCommand& command : capturedCommands) {
				if(command.kind == AnimationCommandKind::SetParameter
					|| command.kind == AnimationCommandKind::FireTrigger) {
					++counters.parameterQueueDepth;
				}
			}
			for(const AnimationStatus status : capturedCommandStatuses) {
				if(status != AnimationStatus::Ok && status != AnimationStatus::NotReady) {
					++counters.rejectedBatches;
				}
			}
			return snapshot;
		}

		AnimationDetailedDiagnosticsSnapshot BuildActiveDetailedDiagnosticsLocked(
			std::uint64_t publicationGeneration) const
		{
			// This method is called only at ProtectedCommit while a scoped consumer
			// lease is active. It copies frame-owned values after worker evaluation;
			// no worker or editor reader touches live RuntimeRecord storage here.
			AnimationDetailedDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = publicationGeneration;
			snapshot.collapsed = false;
			snapshot.animators.reserve(frameWork.size());
			for(std::size_t index = 0U; index < frameWork.size(); ++index) {
				const FrameWorkItem& item = frameWork[index];
				AnimationAnimatorDiagnosticRecord record;
				record.stableInstanceId = (static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U)
					| static_cast<std::uint64_t>(item.input.handle.generation);
				record.instanceSlot = item.input.handle.slotIndex;
				record.instanceGeneration = item.input.handle.generation;
				record.requestedPolicy = item.binding.requestedExecutionPolicy;
				record.effectiveMode = item.executionDecision.effectiveMode;
				record.selectionReason = item.executionDecision.reason;
				record.fallbackReason = item.executionDecision.reason;
				record.poseResidency = item.binding.gpuExecution.resourcesResident
					? (item.gpuLowering.has_value()
						? AnimationPoseResidencyState::GpuResident
						: AnimationPoseResidencyState::UploadPending)
					: (item.pose ? AnimationPoseResidencyState::CpuOnly
						: AnimationPoseResidencyState::Unavailable);
				// GPU Visual owns no CPU-authoritative pose subset. Hybrid and CPU
				// modes report the already-resolved bounded consumer closure instead.
				record.cpuAuthoritativeJointCount = item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::GpuVisual ? 0U
					: static_cast<std::uint32_t>(std::min<std::size_t>(
						item.resolvedConsumers.closure.joints.size(),
						std::numeric_limits<std::uint32_t>::max()));
				record.gpuStageMask = item.gpuStageMask;
				record.batchOrdinal = static_cast<std::uint32_t>(index);
				if(item.gpuLowering) {
					record.gpuEvaluatedElements = item.binding.jointStableIds.size()
						+ item.binding.morphStableIds.size();
				}
				record.modeSwitchCount = item.executionDecision.modeChanged ? 1U : 0U;
				snapshot.animators.push_back(record);

				if(item.executionDecision.transition.occurred
					&& snapshot.modeSwitchHistory.size()
						< GetAnimationRuntimeLimit(AnimationLimit::DetailedDiagnosticHistoryFrames).hardMaximum) {
					const AnimationModeContinuityToken& continuity =
						item.executionDecision.transition.continuity;
					// This compact opaque fingerprint ties a copied transition to its
					// continuity token without exposing mutable controller state.
					const std::uint64_t fingerprint = record.stableInstanceId
						^ static_cast<std::uint64_t>(continuity.transitionBoundaryTick)
						^ static_cast<std::uint64_t>(continuity.stateTimeTick)
						^ continuity.controllerState ^ continuity.parameterSnapshot
						^ continuity.transition ^ continuity.synchronizationState
						^ continuity.eventWatermark ^ continuity.rootMotion
						^ continuity.motionMatch ^ continuity.ragdollBlend ^ continuity.pose;
					snapshot.modeSwitchHistory.push_back({
						record.stableInstanceId,
						item.input.tick,
						item.executionDecision.transition.from,
						item.executionDecision.transition.to,
						item.executionDecision.reason,
						fingerprint});
				}
			}
			return snapshot;
		}

		void PublishCommandCompletionsLocked(std::uint64_t publicationGeneration) noexcept
		{
			if(commands == nullptr || publicationGeneration == 0U
				|| capturedCommands.size() != capturedCommandStatuses.size()) return;
			for(std::size_t index = 0U; index < capturedCommands.size(); ++index) {
				const AnimationStatus status = capturedCommandStatuses[index] == AnimationStatus::NotReady
					? AnimationStatus::StaleHandle
					: capturedCommandStatuses[index];
				commands->PublishCompletion(
					capturedCommands[index], status, frameTick, publicationGeneration);
			}
		}

		void PublishProceduralCommandCompletionsLocked(
			std::uint64_t publicationGeneration) noexcept
		{
			if(proceduralCommands == nullptr || publicationGeneration == 0U
				|| capturedProceduralCommands.size()
					!= capturedProceduralCommandStatuses.size()) return;
			for(std::size_t index = 0U; index < capturedProceduralCommands.size(); ++index) {
				// A drained request must reach a terminal receipt. NotReady is an
				// internal staging value; if it survives validation, fail closed as a
				// stale generation instead of leaving an un-cancellable orphan.
				const AnimationStatus status =
					capturedProceduralCommandStatuses[index] == AnimationStatus::NotReady
						? AnimationStatus::StaleHandle
						: capturedProceduralCommandStatuses[index];
				proceduralCommands->PublishCompletion(
					capturedProceduralCommands[index], status, frameTick, publicationGeneration);
			}
		}

		AnimationStatus BuildRagdollActivationPoseLocked(
			const RuntimeRecord& record,
			const NativeAnimationRagdollRequest& request,
			std::uint64_t publicationGeneration,
			PhysicsPoseSnapshot& output,
			RagdollGenerationStamp& generation) const noexcept
		{
			const auto published = std::atomic_load_explicit(
				&record.publishedPose, std::memory_order_acquire);
			const Physics::PhysicsWorldHandle world =
				Physics::PhysicsSystem::Instance().WorldHandle();
			if(!world.IsValid() || request.physicsWorldGeneration != world.worldGeneration) {
				return AnimationStatus::StaleHandle;
			}
			if(!record.binding.ragdollProfile || !record.binding.skeleton
				|| !MatchesNativeUuid(request.profileAssetId,
					record.binding.ragdollProfile->assetId.value)
				|| request.profileGeneration != record.binding.ragdollProfileGeneration
				|| record.binding.ragdollProfile->runtimeGeneration
					!= record.binding.ragdollProfileGeneration) {
				return AnimationStatus::MissingAsset;
			}
			if(!published || !published->snapshot || !published->snapshot->pose
				|| !published->sourceObjectWorldTransformCaptured
				|| published->snapshot->pose->ValidateForPublication() != AnimationStatus::Ok) {
				return AnimationStatus::NotReady;
			}
			const auto model = published->snapshot->pose->ModelTransforms();
			if(model.size() != record.binding.jointStableIds.size()
				|| record.binding.ragdollProfile->bodies.empty()
				|| record.binding.ragdollProfile->bodies.size()
					> kRagdollSnapshotMaximumBodies) {
				return AnimationStatus::IncompatibleAsset;
			}
			try {
				PhysicsPoseSnapshot candidate;
				generation.worldGeneration = world.worldGeneration;
				generation.profileGeneration = record.binding.ragdollProfileGeneration;
				generation.skeletonGeneration = record.binding.skeletonGeneration;
				generation.publicationGeneration = publicationGeneration;
				// A ragdoll generation names a physics result. The animation frame tick
				// may be far ahead in an editor session, so use Physics' copied clock or
				// the next physics extraction would be rejected as artificially stale.
				generation.fixedStep = Physics::PhysicsSystem::Instance().CurrentFixedTick();
				candidate.generation = generation;
				candidate.completeCurrentStep = true;
				candidate.bodies.reserve(record.binding.ragdollProfile->bodies.size());
				for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
					const auto joint = std::find(record.binding.jointStableIds.begin(),
						record.binding.jointStableIds.end(), body.stableJointId);
					if(joint == record.binding.jointStableIds.end()) {
						return AnimationStatus::IncompatibleAsset;
					}
					const std::size_t jointIndex = static_cast<std::size_t>(
						std::distance(record.binding.jointStableIds.begin(), joint));
					PhysicsPoseBodySnapshot snapshot;
					snapshot.bodyId = body.bodyId;
					snapshot.stableJointId = body.stableJointId;
					// Physics consumes world-space activation. The pose remains immutable;
					// this composition creates a copied provider-neutral handoff record.
					snapshot.modelPose = CombineTransform(
						published->sourceObjectWorldTransform, model[jointIndex]);
					snapshot.bodyGeneration = 1U;
					candidate.bodies.push_back(snapshot);
				}
				if(ValidatePhysicsPoseSnapshot(candidate, generation) != AnimationStatus::Ok) {
					return AnimationStatus::Rejected;
				}
				const std::shared_ptr<const PublishedAnimatorPose> previous =
					record.previousPublishedPose;
				if(previous && previous->snapshot && previous->snapshot->pose
					&& previous->snapshot->tick > 0
					&& previous->snapshot->tick < published->snapshot->tick
					&& previous->skeleton == published->skeleton
					&& previous->jointStableIds == published->jointStableIds
					&& previous->sourceObjectWorldTransformCaptured
					&& previous->snapshot->pose->ValidateForPublication() == AnimationStatus::Ok) {
					PhysicsPoseSnapshot priorCandidate;
					priorCandidate.generation = generation;
					priorCandidate.generation.publicationGeneration =
						previous->authorityPublicationGeneration;
					priorCandidate.generation.fixedStep = std::max<AnimationFixedTick>(
						1, previous->snapshot->tick);
					priorCandidate.completeCurrentStep = true;
					priorCandidate.bodies.reserve(
						record.binding.ragdollProfile->bodies.size());
					const auto previousModel = previous->snapshot->pose->ModelTransforms();
					for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
						const auto joint = std::find(
							previous->jointStableIds.begin(), previous->jointStableIds.end(),
							body.stableJointId);
						if(joint == previous->jointStableIds.end()) {
							return AnimationStatus::IncompatibleAsset;
						}
						const std::size_t jointIndex = static_cast<std::size_t>(
							std::distance(previous->jointStableIds.begin(), joint));
						if(jointIndex >= previousModel.size()) {
							return AnimationStatus::IncompatibleAsset;
						}
						PhysicsPoseBodySnapshot snapshot;
						snapshot.bodyId = body.bodyId;
						snapshot.stableJointId = body.stableJointId;
						snapshot.modelPose = CombineTransform(
							previous->sourceObjectWorldTransform, previousModel[jointIndex]);
						snapshot.bodyGeneration = 1U;
						priorCandidate.bodies.push_back(snapshot);
					}
					const float elapsedSeconds = static_cast<float>(
						published->snapshot->tick - previous->snapshot->tick)
						/ static_cast<float>(kAnimationFixedTicksPerSecond);
					// Pose-history velocity derives from consecutive animation publications,
					// not from physics steps. Keep that local ordering on the animation
					// clock while the outgoing candidate above remains physics-stamped.
					RagdollGenerationStamp motionGeneration = generation;
					motionGeneration.fixedStep = std::max<AnimationFixedTick>(
						1, published->snapshot->tick);
					PhysicsPoseSnapshot motionCandidate = candidate;
					motionCandidate.generation = motionGeneration;
					RagdollActivationPlan movingPlan;
					const AnimationStatus motionStatus = PrepareRagdollActivation(
						motionCandidate, priorCandidate, elapsedSeconds,
						*record.binding.ragdollProfile, motionGeneration, movingPlan);
					if(motionStatus != AnimationStatus::Ok) return motionStatus;
					for(PhysicsPoseBodySnapshot& body : candidate.bodies) {
						const auto motion = std::find_if(
							movingPlan.bodies.begin(), movingPlan.bodies.end(),
							[&body](const RagdollActivationBody& value) {
								return value.bodyId == body.bodyId
									&& value.stableJointId == body.stableJointId;
							});
						if(motion == movingPlan.bodies.end()) {
							return AnimationStatus::IncompatibleAsset;
						}
						body.linearVelocity = motion->linearVelocity;
						body.angularVelocity = motion->angularVelocity;
					}
				}
				output = std::move(candidate);
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		AnimationStatus BeginRagdollRecoveryLocked(
			RuntimeRecord& record,
			const NativeAnimationRagdollRequest& request,
			bool& recoveryBlocked) noexcept
		{
			recoveryBlocked = false;
			if(!record.ragdollCreated
				|| record.ragdollLifecycle != RagdollLifecycleState::Active
				|| !record.ragdollGeneration.IsValid()
				|| !record.binding.ragdollProfile || !record.binding.skeleton) {
				return AnimationStatus::NotReady;
			}
			if(request.physicsWorldGeneration != record.ragdollGeneration.worldGeneration
				|| request.profileGeneration != record.binding.ragdollProfileGeneration
				|| !MatchesNativeUuid(
					request.profileAssetId, record.binding.ragdollProfile->assetId.value)) {
				return AnimationStatus::StaleHandle;
			}
			const std::shared_ptr<const PhysicsPoseSnapshot> physicsPose =
				Physics::PhysicsSystem::Instance().LatestRagdollPhysicsPoseAtOrAfter(
					record.ragdollId, record.ragdollGeneration);
			const std::shared_ptr<const PublishedAnimatorPose> published =
				std::atomic_load_explicit(&record.publishedPose, std::memory_order_acquire);
			if(!physicsPose || !published || !published->snapshot
				|| !published->snapshot->pose
				|| !published->sourceObjectWorldTransformCaptured) {
				return AnimationStatus::NotReady;
			}

			try {
				RagdollRecoveryObservation observation;
				observation.completeAnimationPose.generation = physicsPose->generation;
				observation.completeAnimationPose.completeCurrentStep = true;
				observation.completeAnimationPose.bodies.reserve(
					record.binding.ragdollProfile->bodies.size());
				const auto model = published->snapshot->pose->ModelTransforms();
				for(const RagdollBodyRecord& body : record.binding.ragdollProfile->bodies) {
					const auto joint = std::find(
						record.binding.jointStableIds.begin(),
						record.binding.jointStableIds.end(), body.stableJointId);
					if(joint == record.binding.jointStableIds.end()) {
						return AnimationStatus::IncompatibleAsset;
					}
					const std::size_t index = static_cast<std::size_t>(
						std::distance(record.binding.jointStableIds.begin(), joint));
					if(index >= model.size()) return AnimationStatus::IncompatibleAsset;
					PhysicsPoseBodySnapshot animationBody;
					animationBody.bodyId = body.bodyId;
					animationBody.stableJointId = body.stableJointId;
					animationBody.modelPose = CombineTransform(
						published->sourceObjectWorldTransform, model[index]);
					animationBody.bodyGeneration = 1U;
					observation.completeAnimationPose.bodies.push_back(animationBody);
				}
				// Existing ABI flags remain fixed-width. Bit zero is a caller-confirmed
				// immutable clearance observation; bits 8..31 carry the observed contact
				// mask. A missing clearance bit truthfully produces SpaceBlocked.
				observation.clearanceConfirmed = (request.flags & 0x1U) != 0U;
				observation.observedContactMask = static_cast<std::uint64_t>(request.flags >> 8U);
				if((request.flags & 0x2U) != 0U) {
					observation.observedPosture = RagdollRecoveryPosture::FaceUp;
				} else if((request.flags & 0x4U) != 0U) {
					observation.observedPosture = RagdollRecoveryPosture::FaceDown;
				}
				RagdollRecoveryRequest recoveryRequest;
				AnimationStatus status = BuildRagdollRecoveryRequest(
					*record.binding.ragdollProfile, *physicsPose,
					physicsPose->generation, observation, recoveryRequest);
				if(status != AnimationStatus::Ok) return status;
				const RagdollRecoveryResult selected = SelectRagdollRecovery(
					*record.binding.ragdollProfile, *physicsPose,
					physicsPose->generation, recoveryRequest);
				if(selected.status != AnimationStatus::Ok) {
					record.ragdollRecoveryBlockReason = selected.blockReason;
					recoveryBlocked = selected.blocked;
					return selected.status;
				}

				const auto authored = std::find_if(
					record.binding.ragdollProfile->recovery.begin(),
					record.binding.ragdollProfile->recovery.end(),
					[&selected](const RagdollRecoveryRecord& value) {
						return value.candidateId == selected.candidateId;
					});
				if(authored == record.binding.ragdollProfile->recovery.end()) {
					return AnimationStatus::IncompatibleAsset;
				}
				const auto clip = std::find_if(
					record.binding.controllerClips.begin(), record.binding.controllerClips.end(),
					[&authored](const AnimatorControllerClipBinding& value) {
						return value.clip && value.clip->assetId == authored->clipAssetId
							&& value.clipGeneration == authored->clipGeneration;
					});
				if(clip == record.binding.controllerClips.end() || !clip->clip
					|| clip->clip->compatibilityFingerprint
						!= record.binding.skeleton->compatibilityFingerprint) {
					return AnimationStatus::MissingAsset;
				}

				AnimationClipView nextView;
				status = nextView.Open(
					*clip->clip,
					record.binding.skeleton.get(),
					clip->residentSegments,
					clip->clipGeneration,
					clip->streamedSegmentPayloads);
				if(status != AnimationStatus::Ok) return status;
				AnimationEventStream nextEvents;
				status = nextEvents.Configure(
					{clip->clip->assetId, clip->clipGeneration},
					clip->clip->durationTicks, clip->clip->events);
				if(status != AnimationStatus::Ok) return status;
				AnimatorPlaybackConfiguration configuration = record.playback.Configuration();
				configuration.durationTicks = clip->clip->durationTicks;
				configuration.loop = false;
				configuration.reverse = false;
				AnimatorPlayback nextPlayback;
				status = nextPlayback.Configure(configuration);
				if(status == AnimationStatus::Ok) status = nextPlayback.Play();
				if(status != AnimationStatus::Ok) return status;
				StateMachineSnapshot nextGraph = record.graphState;
				if(record.binding.evaluationProgram) {
					const auto recoveryState = std::find_if(
						record.binding.evaluationProgram->states.begin(),
						record.binding.evaluationProgram->states.end(),
						[&clip](const EvaluationState& value) {
							return value.nodeOpcode == EvaluationOpcode::SampleClip
								&& value.nodeId == clip->nodeStableId;
						});
					if(recoveryState != record.binding.evaluationProgram->states.end()) {
						nextGraph = {};
						nextGraph.activeStateId = recoveryState->stableId;
						nextGraph.stateEnteredTick = frameTick;
						nextGraph.lastTick = frameTick;
					}
				}
				if(record.stateGeneration == std::numeric_limits<std::uint64_t>::max()) {
					return AnimationStatus::RangeOverflow;
				}

				// All allocation and compatibility checks completed. Publish the clip,
				// selector result, and lifecycle edge together at this protected boundary.
				record.binding.clip = clip->clip;
				record.binding.clipGeneration = clip->clipGeneration;
				record.binding.clipDeterministicContentFingerprint =
					clip->deterministicContentFingerprint;
				record.binding.residentSegments = clip->residentSegments;
				record.binding.streamedSegmentPayloads =
					clip->streamedSegmentPayloads;
				record.clipView = std::move(nextView);
				record.eventStream = std::move(nextEvents);
				record.playback = nextPlayback;
				record.graphState = std::move(nextGraph);
				record.binding.stateId = record.graphState.activeStateId != 0U
					? record.graphState.activeStateId : record.binding.stateId;
				++record.stateGeneration;
				record.ragdollGeneration = physicsPose->generation;
				record.ragdollLifecycle = RagdollLifecycleState::Recovering;
				record.ragdollLifecycleStartTick = frameTick;
				record.ragdollBlendWeight = request.blendWeight;
				record.ragdollRecoveryCandidateId = selected.candidateId;
				record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
				record.ragdollRecoveryAlignedRootPosition = selected.alignedRootPosition;
				record.ragdollRecoveryDurationSeconds = selected.transitionDurationSeconds;
				return AnimationStatus::Ok;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			} catch(...) {
				return AnimationStatus::InternalError;
			}
		}

		NativeAnimationRagdollSnapshot MakeRagdollSnapshot(
			const RuntimeRecord& record,
			NativeAnimationStatus status,
			AnimationFixedTick observationTick,
			std::uint64_t publicationGeneration,
			bool recoveryBlocked = false) const noexcept
		{
			NativeAnimationRagdollSnapshot snapshot{};
			snapshot.header = MakeNativeHeader(
				NativeAnimationRecordKind::RagdollSnapshot, sizeof(snapshot));
			snapshot.status = status;
			snapshot.animator = {record.handle.slotIndex, record.handle.generation};
			snapshot.profileAssetId = record.activeRagdollProfileId;
			snapshot.profileGeneration = record.ragdollGeneration.profileGeneration;
			snapshot.physicsWorldGeneration = record.ragdollGeneration.worldGeneration;
			snapshot.lifecycle = static_cast<std::uint32_t>(record.ragdollLifecycle);
			snapshot.bodyCount = record.binding.ragdollProfile
				? static_cast<std::uint32_t>(record.binding.ragdollProfile->bodies.size()) : 0U;
			snapshot.blendWeight = record.ragdollBlendWeight;
			// Values remain boolean-compatible for ABI-v1 managed callers, while native
			// diagnostics can distinguish the exact recovery rejection reason.
			snapshot.recoveryBlocked = recoveryBlocked
				? std::max<std::uint32_t>(1U, static_cast<std::uint32_t>(
					record.ragdollRecoveryBlockReason)) : 0U;
			snapshot.reserved = static_cast<std::uint16_t>(std::min<std::uint32_t>(
				record.ragdollMotorDriveCount,
				std::numeric_limits<std::uint16_t>::max()));
			snapshot.observationTick = observationTick;
			snapshot.publicationGeneration = publicationGeneration;
			return snapshot;
		}

		void SynchronizeRagdollResultsLocked(
			std::uint64_t publicationGeneration) noexcept
		{
			if(advancedCommands == nullptr) return;
			const std::shared_ptr<const RagdollPhysicsResultSnapshot> results =
				Physics::PhysicsSystem::Instance().LatestRagdollPhysicsResults();
			if(!results || !results->results) return;
			for(const RagdollPhysicsResult& result : *results->results) {
				for(const std::unique_ptr<RuntimeRecord>& owned : runtimeRecords) {
					if(!owned || owned->ragdollId != result.ragdollId
						|| owned->pendingRagdollCorrelationId == 0U
						|| owned->pendingRagdollCorrelationId != result.correlationId) continue;
					RuntimeRecord& record = *owned;
					if(result.status == AnimationStatus::Ok) {
						record.ragdollGeneration = result.generation;
						switch(record.pendingRagdollOperation) {
						case NativeAnimationRagdollOperation::Activate:
							record.ragdollCreated = true;
							record.ragdollLifecycle = RagdollLifecycleState::Active;
							record.ragdollLifecycleStartTick = result.committedStep;
							record.ragdollRecoveryBlockReason =
								RagdollRecoveryBlockReason::None;
							break;
						case NativeAnimationRagdollOperation::Deactivate:
							record.ragdollLifecycle = RagdollLifecycleState::Inactive;
							record.ragdollBlendWeight = 0.0F;
							record.ragdollRecoveryCandidateId = 0U;
							record.ragdollRecoveryBlockReason =
								RagdollRecoveryBlockReason::None;
							record.ragdollRecoveryDurationSeconds = 0.0F;
							break;
						case NativeAnimationRagdollOperation::RequestRecovery:
							record.ragdollLifecycle = RagdollLifecycleState::Recovering;
							break;
						}
					} else {
						// Physics owns the complete lifecycle batch. If it rejects, restore
						// the exact animation-side state captured before enqueueing instead of
						// exposing a half-transitioned Creating/TearingDown record.
						record.ragdollGeneration =
							record.pendingRagdollPreviousGeneration;
						record.ragdollLifecycle =
							record.pendingRagdollPreviousLifecycle;
						record.activeRagdollProfileId =
							record.pendingRagdollPreviousProfileId;
						record.ragdollBlendWeight =
							record.pendingRagdollPreviousBlendWeight;
					}
					(void)advancedCommands->Publish(MakeRagdollSnapshot(
						record, ToNativeStatus(result.status), result.committedStep,
						publicationGeneration));
					record.pendingRagdollCorrelationId = 0U;
					record.pendingRagdollPreviousGeneration = {};
				}
			}
		}

		void PublishAdvancedCommandCompletionsLocked(
			std::uint64_t publicationGeneration) noexcept
		{
			if(advancedCommands == nullptr || publicationGeneration == 0U) return;
			SynchronizeRagdollResultsLocked(publicationGeneration);
			const auto findPreEvaluationPublication = [this](
				NativeAnimationAnimatorHandle animator) -> std::shared_ptr<const PublishedAnimatorPose> {
				const auto work = std::find_if(frameWork.begin(), frameWork.end(),
					[animator](const FrameWorkItem& item) {
						return item.input.handle.slotIndex == animator.slotIndex
							&& item.input.handle.generation == animator.generation;
					});
				// FrameWorkItem owns an immutable pointer captured before worker evaluation.
				// Completion runs after PublishFrameLocked has replaced the live record, so
				// only this retained pointer can validate the generation the producer saw.
				return work == frameWork.end() ? std::shared_ptr<const PublishedAnimatorPose>{}
					: work->priorPublication;
			};

			for(const NativeAnimationRetargetRequest& request : capturedRetargetRequests) {
				RuntimeRecord* record = FindRuntimeRecordLocked(
					{request.animator.slotIndex, request.animator.generation});
				NativeAnimationRetargetSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(
					NativeAnimationRecordKind::RetargetSnapshot, sizeof(snapshot));
				snapshot.animator = request.animator;
				snapshot.profileAssetId = request.profileAssetId;
				snapshot.profileGeneration = request.profileGeneration;
				snapshot.observationTick = frameTick;
				snapshot.publicationGeneration = publicationGeneration;
				AnimationStatus status = AnimationStatus::StaleHandle;
				if(record != nullptr && instances->IsAlive(record->handle)) {
					const auto published = std::atomic_load_explicit(
						&record->publishedPose, std::memory_order_acquire);
					const std::shared_ptr<const PublishedAnimatorPose> preEvaluationPublication =
						findPreEvaluationPublication(request.animator);
					snapshot.sourceSkeletonGeneration =
						record->binding.retargetSourceSkeletonGeneration;
					snapshot.targetSkeletonGeneration = record->binding.skeletonGeneration;
					snapshot.poseGeneration = published && published->snapshot
						? published->snapshot->publicationGeneration : 0U;
					if(request.operation == NativeAnimationRetargetOperation::DeactivateProfile) {
						record->activeRetargetProfileId = {};
						record->activeRetargetProfileGeneration = 0U;
						status = AnimationStatus::Ok;
					} else if(!record->binding.retargetProfile
						|| !MatchesNativeUuid(request.profileAssetId,
							record->binding.retargetProfile->assetId.value)
						|| request.profileGeneration
							!= record->binding.retargetProfileGeneration) {
						status = AnimationStatus::MissingAsset;
					} else if((request.expectedPoseGeneration != 0U
							&& (!preEvaluationPublication || !preEvaluationPublication->snapshot
								|| request.expectedPoseGeneration
									!= preEvaluationPublication->snapshot->publicationGeneration))
						|| (request.expectedPublicationGeneration != 0U
							&& (!preEvaluationPublication || request.expectedPublicationGeneration
								!= preEvaluationPublication->authorityPublicationGeneration))) {
						status = AnimationStatus::StaleHandle;
					} else if(!record->binding.retargetSourceSkeleton
						|| !record->binding.skeleton
						|| record->binding.retargetProfile->sourceSkeletonFingerprint
							!= record->binding.retargetSourceSkeleton->compatibilityFingerprint
						|| record->binding.retargetProfile->targetSkeletonFingerprint
							!= record->binding.skeleton->compatibilityFingerprint) {
						status = AnimationStatus::IncompatibleAsset;
					} else {
						RetargetValidationResult validation;
						RetargetCompiledProfile compiled;
						status = CompileRetargetProfile(
							*record->binding.retargetProfile,
							*record->binding.retargetSourceSkeleton,
							*record->binding.skeleton,
							validation,
							compiled);
						if(status == AnimationStatus::Ok) {
							try {
								// Cooked bytes intentionally omit session-local generations. Pair the
								// newly compiled table with the exact publication validated above.
								compiled.profile.runtimeGeneration = request.profileGeneration;
								auto immutable = std::make_shared<const RetargetCompiledProfile>(
									std::move(compiled));
								// The complete mapping is allocated before either live identity changes,
								// preserving the prior active profile on any failed rebuild.
								record->binding.compiledRetargetProfile = std::move(immutable);
								record->activeRetargetProfileId = request.profileAssetId;
								record->activeRetargetProfileGeneration = request.profileGeneration;
							} catch(const std::bad_alloc&) {
								status = AnimationStatus::InternalError;
							}
						}
					}
				}
				snapshot.status = ToNativeStatus(status);
				(void)advancedCommands->Publish(std::move(snapshot));
			}

			for(const NativeAnimationMotionMatchRequest& request : capturedMotionMatchRequests) {
				RuntimeRecord* record = FindRuntimeRecordLocked(
					{request.animator.slotIndex, request.animator.generation});
				NativeAnimationMotionMatchSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(
					NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(snapshot));
				snapshot.animator = request.animator;
				snapshot.databaseAssetId = request.databaseAssetId;
				snapshot.databaseGeneration = request.databaseGeneration;
				snapshot.queryId = request.queryId;
				snapshot.observationTick = frameTick;
				snapshot.publicationGeneration = publicationGeneration;
				AnimationStatus status = AnimationStatus::StaleHandle;
				if(record != nullptr && instances->IsAlive(record->handle)) {
					if(request.operation == NativeAnimationMotionMatchOperation::Clear) {
						record->activeMotionDatabaseId = {};
						record->activeMotionDatabaseGeneration = 0U;
						record->latestMotionQueryId = 0U;
						record->appliedMotionQueryId = 0U;
						record->latestMotionCandidate = {};
						record->hasMotionCandidate = false;
						status = AnimationStatus::Ok;
					} else if(request.operation == NativeAnimationMotionMatchOperation::SetDatabase) {
						if(!record->binding.motionDatabase
							|| !MatchesNativeUuid(request.databaseAssetId,
								record->binding.motionDatabase->assetId.value)
							|| request.databaseGeneration
								!= record->binding.motionDatabaseGeneration
							|| request.featureCount != kMotionMatchFeatureCountV1) {
							status = record->binding.motionDatabase
								? AnimationStatus::IncompatibleAsset : AnimationStatus::MissingAsset;
						} else {
							record->activeMotionDatabaseId = request.databaseAssetId;
							record->activeMotionDatabaseGeneration = request.databaseGeneration;
							record->latestMotionQueryId = 0U;
							record->appliedMotionQueryId = 0U;
							record->latestMotionCandidate = {};
							record->hasMotionCandidate = false;
							status = AnimationStatus::Ok;
						}
					} else if(record->activeMotionDatabaseGeneration == 0U
						|| request.featureCount != kMotionMatchFeatureCountV1) {
						status = AnimationStatus::NotReady;
					} else {
						const auto work = std::find_if(frameWork.begin(), frameWork.end(),
							[&request](const FrameWorkItem& item) {
								return item.input.handle.slotIndex == request.animator.slotIndex
									&& item.input.handle.generation
										== request.animator.generation;
							});
						const auto evaluated = work == frameWork.end()
							? std::vector<NativeAnimationMotionMatchSnapshot>::const_iterator{}
							: std::find_if(
								work->evaluatedMotionQueries.begin(),
								work->evaluatedMotionQueries.end(),
								[&request](const NativeAnimationMotionMatchSnapshot& value) {
									return value.queryId == request.queryId;
								});
						if(work != frameWork.end()
							&& evaluated != work->evaluatedMotionQueries.end()) {
							snapshot = *evaluated;
							snapshot.observationTick = frameTick;
							snapshot.publicationGeneration = publicationGeneration;
							status = static_cast<AnimationStatus>(snapshot.status);
							if(status == AnimationStatus::Ok && work->hasMotionCandidate) {
								record->latestMotionQueryId = request.queryId;
								record->latestMotionCandidate = work->latestMotionCandidate;
								record->hasMotionCandidate = true;
							}
						} else {
							// A newly activated database is visible to worker queries on the next
							// capture boundary. Publication never performs a large synchronous search.
							status = AnimationStatus::NotReady;
						}
					}
				}
				snapshot.status = ToNativeStatus(status);
				if(status != AnimationStatus::Ok) snapshot.usedFallback = 1U;
				(void)advancedCommands->Publish(std::move(snapshot));
			}

			for(const NativeAnimationMotionMatchQueryRequestV2& request
				: capturedMotionMatchQueriesV2) {
				RuntimeRecord* record = FindRuntimeRecordLocked(
					{request.animator.slotIndex, request.animator.generation});
				NativeAnimationMotionMatchResultV2 result{};
				result.header = MakeNativeHeader(
					NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
				result.animator = request.animator;
				result.databaseAssetId = request.databaseAssetId;
				result.databaseGeneration = request.databaseGeneration;
				result.schemaFingerprint = request.schemaFingerprint;
				result.queryId = request.queryId;
				result.observationTick = frameTick;
				result.publicationGeneration = publicationGeneration;
				result.populatedFields = static_cast<std::uint32_t>(
					NativeAnimationMotionMatchResultField::Identity);
				AnimationStatus status = AnimationStatus::StaleHandle;
				if(record != nullptr && instances->IsAlive(record->handle)) {
					const auto work = std::find_if(frameWork.begin(), frameWork.end(),
						[&request](const FrameWorkItem& item) {
							return item.input.handle.slotIndex == request.animator.slotIndex
								&& item.input.handle.generation
									== request.animator.generation;
						});
					if(work != frameWork.end()) {
						const auto evaluated = std::find_if(
							work->evaluatedMotionQueriesV2.begin(),
							work->evaluatedMotionQueriesV2.end(),
							[&request](const MotionMatchQueryCompletionV2& value) {
								return value.correlationId == request.correlationId
									&& value.result.queryId == request.queryId;
							});
						if(evaluated != work->evaluatedMotionQueriesV2.end()) {
							result = evaluated->result;
							result.observationTick = frameTick;
							result.publicationGeneration = publicationGeneration;
							status = static_cast<AnimationStatus>(result.status);
							if(status == AnimationStatus::Ok
								&& evaluated->hasSelectedCandidate) {
								// A successful direct query validates the immutable bound database.
								// Retaining that identity lets the chosen candidate enter the same
								// next-frame controller path used by the frozen V1 API.
								record->activeMotionDatabaseId = request.databaseAssetId;
								record->activeMotionDatabaseGeneration =
									request.databaseGeneration;
								record->latestMotionQueryId = request.queryId;
								record->appliedMotionQueryId = 0U;
								record->latestMotionCandidate =
									evaluated->selectedCandidate;
								record->hasMotionCandidate = true;
							}
						} else {
							// A live scheduled item without a completion indicates bounded
							// staging failed; do not silently report an endless pending query.
							status = AnimationStatus::InternalError;
						}
					} else {
						status = AnimationStatus::NotReady;
					}
				}
				result.status = ToNativeStatus(status);
				(void)advancedCommands->Publish(std::move(result));
			}

			for(const NativeAnimationRagdollRequest& request : capturedRagdollRequests) {
				RuntimeRecord* record = FindRuntimeRecordLocked(
					{request.animator.slotIndex, request.animator.generation});
				if(record == nullptr || !instances->IsAlive(record->handle)) {
					NativeAnimationRagdollSnapshot stale{};
					stale.header = MakeNativeHeader(
						NativeAnimationRecordKind::RagdollSnapshot, sizeof(stale));
					stale.status = NativeAnimationStatus::StaleHandle;
					stale.animator = request.animator;
					stale.profileAssetId = request.profileAssetId;
					stale.profileGeneration = request.profileGeneration;
					stale.physicsWorldGeneration = request.physicsWorldGeneration;
					stale.observationTick = frameTick;
					stale.publicationGeneration = publicationGeneration;
					(void)advancedCommands->Publish(std::move(stale));
					continue;
				}

				AnimationStatus status = AnimationStatus::NotReady;
				bool recoveryBlocked = false;
				if(record->pendingRagdollCorrelationId != 0U) {
					status = AnimationStatus::NotReady;
				} else if(request.operation == NativeAnimationRagdollOperation::RequestRecovery) {
					status = BeginRagdollRecoveryLocked(
						*record, request, recoveryBlocked);
				} else if(request.operation == NativeAnimationRagdollOperation::Activate) {
					PhysicsPoseSnapshot activationPose;
					RagdollGenerationStamp generation;
					status = BuildRagdollActivationPoseLocked(
						*record, request, publicationGeneration, activationPose, generation);
					if(status == AnimationStatus::Ok) {
						RagdollPhysicsRequest activate;
						activate.kind = RagdollPhysicsRequestKind::Activate;
						activate.ragdollId = record->ragdollId;
						activate.generation = generation;
						activate.expectedCurrentGeneration = record->ragdollGeneration;
						activate.profile = record->binding.ragdollProfile;
						activate.activationPose = activationPose;
						activate.correlationId = request.correlationId;
						activate.producerOrder = 2U;
						if(record->ragdollCreated) {
							status = Physics::PhysicsSystem::Instance()
								.SubmitRagdollPhysicsRequest(std::move(activate));
						} else {
							RagdollPhysicsRequest create = activate;
							create.kind = RagdollPhysicsRequestKind::Create;
							create.expectedCurrentGeneration = {};
							create.correlationId = request.correlationId
								^ 0xA11A000000000000ULL;
							if(create.correlationId == 0U) create.correlationId = 1U;
							create.producerOrder = 1U;
							activate.expectedCurrentGeneration = generation;
							const std::array<RagdollPhysicsRequest, 2U> batch{
								std::move(create), std::move(activate)};
							status = Physics::PhysicsSystem::Instance()
								.SubmitRagdollPhysicsRequestBatch(batch);
						}
						if(status == AnimationStatus::Queued) {
							record->pendingRagdollPreviousGeneration =
								record->ragdollGeneration;
							record->pendingRagdollPreviousLifecycle =
								record->ragdollLifecycle;
							record->pendingRagdollPreviousProfileId =
								record->activeRagdollProfileId;
							record->pendingRagdollPreviousBlendWeight =
								record->ragdollBlendWeight;
							record->ragdollGeneration = generation;
							record->activeRagdollProfileId = request.profileAssetId;
							record->ragdollLifecycle = RagdollLifecycleState::Creating;
							record->ragdollBlendWeight = request.blendWeight;
							record->pendingRagdollCorrelationId = request.correlationId;
							record->pendingRagdollOperation = request.operation;
						}
					}
				} else if(!record->ragdollCreated || !record->ragdollGeneration.IsValid()) {
					status = AnimationStatus::NotReady;
				} else if(request.physicsWorldGeneration
					!= record->ragdollGeneration.worldGeneration) {
					status = AnimationStatus::StaleHandle;
				} else {
					RagdollGenerationStamp generation = record->ragdollGeneration;
					generation.publicationGeneration = publicationGeneration;
					generation.fixedStep = std::max<AnimationFixedTick>(1, frameTick);
					RagdollPhysicsRequest deactivate;
					deactivate.kind = RagdollPhysicsRequestKind::Deactivate;
					deactivate.ragdollId = record->ragdollId;
					deactivate.generation = generation;
					deactivate.expectedCurrentGeneration = record->ragdollGeneration;
					deactivate.correlationId = request.correlationId;
					deactivate.producerOrder = 1U;
					status = Physics::PhysicsSystem::Instance()
						.SubmitRagdollPhysicsRequest(std::move(deactivate));
					if(status == AnimationStatus::Queued) {
						record->pendingRagdollPreviousGeneration =
							record->ragdollGeneration;
						record->pendingRagdollPreviousLifecycle =
							record->ragdollLifecycle;
						record->pendingRagdollPreviousProfileId =
							record->activeRagdollProfileId;
						record->pendingRagdollPreviousBlendWeight =
							record->ragdollBlendWeight;
						record->ragdollGeneration = generation;
						record->ragdollLifecycle = RagdollLifecycleState::TearingDown;
						record->pendingRagdollCorrelationId = request.correlationId;
						record->pendingRagdollOperation = request.operation;
					}
				}
				(void)advancedCommands->Publish(MakeRagdollSnapshot(
					*record, ToNativeStatus(status), frameTick,
					publicationGeneration, recoveryBlocked));
			}

			for(const NativeAnimationDeterminismRequest& request : capturedDeterminismRequests) {
				RuntimeRecord* record = FindRuntimeRecordLocked(
					{request.animator.slotIndex, request.animator.generation});
				NativeAnimationDeterminismSnapshot snapshot{};
				snapshot.header = MakeNativeHeader(
					NativeAnimationRecordKind::DeterminismSnapshot, sizeof(snapshot));
				snapshot.animator = request.animator;
				snapshot.tick = frameTick;
				snapshot.publicationGeneration = publicationGeneration;
				AnimationStatus status = AnimationStatus::StaleHandle;
				if(record != nullptr && instances->IsAlive(record->handle)) {
					const auto completion = std::find_if(
						determinismRequestCompletions.begin(),
						determinismRequestCompletions.end(),
						[&request](const DeterminismRequestCompletion& value) {
							return value.request.correlationId == request.correlationId
								&& value.request.animator.slotIndex
									== request.animator.slotIndex
								&& value.request.animator.generation
									== request.animator.generation;
						});
					if(completion == determinismRequestCompletions.end()) {
						status = AnimationStatus::InternalError;
					} else {
						status = completion->status;
						AnimationRollbackSnapshot currentObserved;
						const AnimationRollbackSnapshot* observed = nullptr;
						if((request.operation
								== NativeAnimationDeterminismOperation::Capture
							|| request.operation
								== NativeAnimationDeterminismOperation::RequestReplication)
							&& status == AnimationStatus::Ok) {
							observed = &completion->sourceSnapshot;
						} else if((request.operation
								== NativeAnimationDeterminismOperation::Restore
							|| request.operation
								== NativeAnimationDeterminismOperation::ReplaySeek)
							&& status == AnimationStatus::Ok) {
							const auto published = std::atomic_load_explicit(
								&record->publishedPose, std::memory_order_acquire);
							status = BuildRollbackSnapshotLocked(
								*record, published, currentObserved);
							if(status == AnimationStatus::Ok) {
								observed = &currentObserved;
								const std::uint64_t expectedPose =
									RuntimePoseChecksum(completion->sourceSnapshot);
								if(expectedPose == 0U
									|| RuntimePoseChecksum(*observed) != expectedPose) {
									status = AnimationStatus::DeterminismViolation;
								}
							}
						} else if(status == AnimationStatus::DeterminismViolation
							&& RuntimePoseChecksum(completion->sourceSnapshot) != 0U) {
							// Mismatch diagnostics retain the rejected source identity without
							// implying that its state became live.
							observed = &completion->sourceSnapshot;
						}

						if(observed != nullptr) {
							snapshot.tick = observed->tick;
							snapshot.stateChecksum = RuntimeStateChecksum(*observed);
							snapshot.poseChecksum = RuntimePoseChecksum(*observed);
							if(!observed->controllers.empty()) {
								snapshot.controllerStateId =
									observed->controllers.front().activeStateId;
							}
							snapshot.controllerGeneration = record->binding.programGeneration;
							snapshot.eventWatermark = static_cast<std::uint32_t>(
								std::min<std::uint64_t>(
									observed->eventWatermark.deliveredSequence,
									UINT32_MAX));
							if(request.expectedChecksum != 0U
								&& request.expectedChecksum != snapshot.poseChecksum) {
								status = AnimationStatus::DeterminismViolation;
							}
						} else if(request.operation
								== NativeAnimationDeterminismOperation::Capture
							|| request.operation
								== NativeAnimationDeterminismOperation::RequestReplication) {
							// An unbound or compatibility-fallback animator has no valid asset
							// fingerprint and therefore cannot enter rollback history. Preserve
							// useful copied diagnostics while the non-Ok status makes that lack of
							// restorability explicit to the caller.
							const auto published = std::atomic_load_explicit(
								&record->publishedPose, std::memory_order_acquire);
							if(published && published->snapshot && published->snapshot->pose) {
								snapshot.tick = published->snapshot->tick;
								snapshot.stateChecksum = RuntimeStateChecksum(*record);
								snapshot.poseChecksum = RuntimePoseChecksum(
									*published->snapshot->pose);
								snapshot.controllerStateId = record->binding.stateId;
								snapshot.controllerGeneration = record->binding.programGeneration;
								snapshot.eventWatermark = static_cast<std::uint32_t>(
									std::min<std::size_t>(
										published->snapshot->pose->Events().size(),
										UINT32_MAX));
							}
						}
						if(status == AnimationStatus::DeterminismViolation) {
							snapshot.divergenceKind = 1U;
						}
					}
				}
				snapshot.status = ToNativeStatus(status);
				(void)advancedCommands->Publish(std::move(snapshot));
			}
		}

		[[nodiscard]] std::shared_ptr<const GpuAnimationPresentationFrame>
		BuildGpuPresentationFrame(std::uint64_t sourceGeneration) const noexcept
		{
			if(sourceGeneration == 0U) return {};
			try {
				auto frame = std::make_shared<GpuAnimationPresentationFrame>();
				frame->sourceGeneration = sourceGeneration;
				frame->planGeneration = static_cast<std::uint32_t>(sourceGeneration);
				if(frame->planGeneration == 0U) frame->planGeneration = 1U;
				frame->inputs.reserve(frameWork.size());
				frame->ownerObjectMappings.reserve(frameWork.size());
				for(const FrameWorkItem& item : frameWork) {
					if(!item.gpuLowering) continue;
					frame->inputs.push_back(*item.gpuLowering);
					GpuAnimationOwnerObjectMapping mapping{};
					mapping.ownerObjectId = item.input.descriptor.ownerObjectId;
					mapping.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
					mapping.instanceSlot = item.input.handle.slotIndex;
					mapping.instanceGeneration = item.input.handle.generation;
					mapping.componentGeneration = item.input.descriptor.componentGeneration;
					if(!mapping.IsComplete()) return {};
					frame->ownerObjectMappings.push_back(mapping);
				}
				return frame;
			} catch(const std::bad_alloc&) {
				// CPU authority has already committed.  A render-presentation allocation
				// failure must remain an isolated CPU fallback for this frame.
				return {};
			}
		}

		void AssertCoordinatorThread() const
		{
			// SystemScheduler invokes DeclareFrameWorkloads on its protected
			// coordinator before any worker job is launched. The one membership load
			// and dormant diagnostics publication rely on that invariant.
			assert(coordinatorThread == std::this_thread::get_id()
				&& "Animation workload declarations must be bound on the coordinator thread.");
		}
	};

	AnimationSystem& AnimationSystem::Instance()
	{
		static AnimationSystem system;
		return system;
	}

	AnimationSystem::AnimationSystem()
		: impl_(std::make_unique<Impl>())
	{
	}

	AnimationSystem::~AnimationSystem() = default;

	void AnimationSystem::Initialize()
	{
		impl_->AssertCoordinatorThread();
		{
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			// SystemsBase initialization is idempotent. Reinitializing an already
			// active singleton must not erase published counts or captured state while
			// leaving their public generation identity unchanged.
			if(impl_->runtimeActive.load(std::memory_order_acquire)) return;
			impl_->ActivateRuntimeOwnershipLocked();
			impl_->capturedFrameInputs.clear();
			impl_->capturedCommands.clear();
			impl_->capturedCommandStatuses.clear();
			impl_->capturedProceduralCommands.clear();
			impl_->capturedProceduralCommandStatuses.clear();
			if(impl_->proceduralCommands != nullptr) impl_->proceduralCommands->Clear();
			impl_->capturedRetargetRequests.clear();
			impl_->capturedMotionMatchRequests.clear();
			impl_->capturedMotionMatchQueriesV2.clear();
			impl_->capturedRagdollRequests.clear();
			impl_->capturedDeterminismRequests.clear();
			if(impl_->advancedCommands != nullptr) impl_->advancedCommands->Clear();
			impl_->pendingRootMotionApplications.clear();
			impl_->frameWork.clear();
			impl_->runtimeRecords.clear();
			impl_->rollbackHistory.ClearAll();
			impl_->frameTick = 0;
			impl_->hadScheduledAnimatorMembership = false;
		}
		impl_->scheduledAnimatorCount.store(0U, std::memory_order_release);
		impl_->plannedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluatedAnimatorCount.store(0U, std::memory_order_release);
		impl_->publishedAnimatorCount.store(0U, std::memory_order_release);
		AnimationAttachmentSnapshotPublisher::Instance().Clear();
		AnimationRootMotionResultPublisher::Instance().Clear();
		// Publication generations are public stale-work identities and intentionally
		// remain monotonic across runtime sessions.
	}

	void AnimationSystem::Update(float)
	{
		// SystemScheduler executes the explicit declarations below. Keeping this
		// compatibility entry empty prevents a direct legacy call from evaluating
		// or publishing animation outside the declared protected boundaries.
	}

	void AnimationSystem::Shutdown()
	{
		impl_->AssertCoordinatorThread();
		// Authored/runtime mappings re-enter the public invalidation API, so retire
		// them before taking the nonrecursive lifecycle mutex below. This also makes
		// every component-owned handle stale before the runtime session is retired.
		impl_->componentBindings.InvalidateAll(*this);
		bool retiredRuntimeSession = false;
		{
			// Holding the lifecycle guard while the queue is released and every live
			// slot is invalidated makes shutdown atomic with concurrent producers.
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Shutdown);
			impl_->ResetFrameIntegrationStateLocked();
			impl_->evaluationCache.InvalidateAll();
			impl_->memoryController.Shutdown();
			impl_->capturedFrameInputs.clear();
			impl_->capturedCommands.clear();
			impl_->capturedCommandStatuses.clear();
			impl_->capturedProceduralCommands.clear();
			impl_->capturedProceduralCommandStatuses.clear();
			if(impl_->proceduralCommands != nullptr) impl_->proceduralCommands->Clear();
			impl_->capturedRetargetRequests.clear();
			impl_->capturedMotionMatchRequests.clear();
			impl_->capturedMotionMatchQueriesV2.clear();
			impl_->capturedRagdollRequests.clear();
			impl_->capturedDeterminismRequests.clear();
			if(impl_->advancedCommands != nullptr) impl_->advancedCommands->Clear();
			impl_->pendingRootMotionApplications.clear();
			impl_->frameWork.clear();
			impl_->rollbackHistory.ClearAll();
			impl_->hadScheduledAnimatorMembership = false;
			if(impl_->runtimeActive.exchange(false, std::memory_order_acq_rel)) {
				// Make dormant membership visible before invalidating the protected
				// store, then retire all handles while producers remain behind the guard.
				impl_->instances->InvalidateAll();
				impl_->commands.reset();
				impl_->runtimeRecords.clear();
				impl_->runtimeSessionEpoch.fetch_add(1U, std::memory_order_acq_rel);
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
				retiredRuntimeSession = true;
			}
		}
		// Published attachment snapshots borrow the runtime session's animator
		// identity. Clear them only after releasing lifecycle ownership so readers
		// cannot create a lock-order cycle with shutdown.
		AnimationAttachmentSnapshotPublisher::Instance().Clear();
		AnimationRootMotionResultPublisher::Instance().Clear();
		impl_->scheduledAnimatorCount.store(0U, std::memory_order_release);
		impl_->plannedAnimatorCount.store(0U, std::memory_order_release);
		impl_->evaluatedAnimatorCount.store(0U, std::memory_order_release);
		impl_->publishedAnimatorCount.store(0U, std::memory_order_release);
		if(retiredRuntimeSession) {
			// Retire the complete authority identity even when no animator happened
			// to publish in this session. Initialize preserves this monotonic value.
			impl_->authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
		}
	}

	std::string AnimationSystem::SystemName()
	{
		return "AnimationSystem";
	}

	TerraEngine::Scheduling::SystemDeclaration AnimationSystem::SchedulerDeclaration()
	{
		using namespace TerraEngine::Scheduling;
		SystemDeclaration declaration;
		declaration.name = "AnimationSystem";
		declaration.phase = "Animation";
		declaration.after = {"ScriptingSystem"};
		declaration.before = {"PhysicsSystem"};
		declaration.reads = {
			kResourceAnimatorComponents,
			kResourceAnimationCommandQueues,
			"RuntimeTime",
			"RuntimeLayout",
			"Resources",
			kResourceAnimationEvaluationPrograms,
			kResourceAnimationResidencyCapabilities,
			kResourceAnimationClipSegments,
			kResourceAnimationRollbackState
		};
		declaration.writes = {
			kResourceAnimationFrameInputs,
			kResourceAnimationExecutionModes,
			kResourceAnimationConsumerMasks,
			kResourceAnimationWorkPackets,
			kResourceAnimationVisualPackets,
			kResourceAnimationAuthoritativeOutputs,
			kResourceAnimationRuntimeState,
			kResourceAnimationPhysicsHandoffQueue,
			kResourceAnimationManagedCallbackBatches,
			"SceneMutationQueue",
			"SceneGraph",
			"ObjectManager"
		};
		declaration.mainThread = true;
		// AnimatorComponent membership follows the engine's normal structural rule:
		// mutations submitted by scripts flush after scripting and are synchronized
		// into the next frame's frozen workload count before live input capture.
		declaration.requiresFlushedSceneMutations = true;
		declaration.phaseKind = WorkerSystemPhaseKind::ProtectedCommit;
		declaration.boundaryClassification = HardBoundaryClassification::PartiallyWorkerSafe;
		declaration.hardBoundary = true;
		declaration.safeSubwork = "Immutable input planning and range-disjoint authoritative evaluation can run across deterministic worker partitions.";
		declaration.protectedCommit = "Live animator/component capture and complete generation-validated authority, physics, and copied managed handoff publication.";
		declaration.protectedReason = declaration.protectedCommit;
		declaration.profilingLabel = "Animation";
		declaration.omitWhenNoFrameWorkloads = true;
		declaration.usesDefaultMetadata = false;
		return declaration;
	}

	std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> AnimationSystem::DeclareFrameWorkloads(float deltaTime)
	{
	using namespace TerraEngine::Scheduling;
	impl_->AssertCoordinatorThread();
	const ScriptingSystem::RuntimeTimeSnapshot runtimeTime =
		ScriptingSystem::Instance().GetRuntimeTimeSnapshot();
	const bool pausedPlayModeWithoutStep = runtimeTime.playSessionActive
		&& runtimeTime.playSessionPaused && !runtimeTime.advancesSimulation;
	if(pausedPlayModeWithoutStep) {
		// Play Mode pause owns the simulation clock rather than merely setting a
		// zero delta.  Retaining existing binding membership and publications keeps
		// generation-stamped automation targets valid until an explicit Next step;
		// importantly, no protected capture can consume a queued mutation early.
		// Editor previews remain active because they do not have a running Play
		// session and therefore never take this branch.
		return {};
	}
	if(impl_->runtimeActive.load(std::memory_order_acquire)
		&& impl_->authoredComponentPlaybackEnabled) {
			ObjectManager& objects = ObjectManager::instance();
			const bool hadScheduledMembership =
				impl_->scheduledAnimatorCount.load(std::memory_order_acquire) != 0U;
			const bool hasAuthoredComponentStorage =
				objects.HasAnyObjectWithComponentsIncludingInactive<AnimatorComponent>();
			// Declaration runs on the protected coordinator after the previous flush.
			// Synchronize before freezing membership so zero-to-one authored changes
			// can create workloads and every deterministic partition uses one count.
			// The editor disables this authored-component path outside Play Mode;
			// preview-window instances are independent and remain schedulable.
			//
			// A completely static project first probes EnTT's component storage.
			// That constant-time empty check avoids walking every active object each
			// frame. Existing membership still synchronizes once after removal so
			// handles, streaming retains, and published state are retired atomically.
			if(hadScheduledMembership || hasAuthoredComponentStorage) {
				(void)impl_->componentBindings.Synchronize(
					*this, objects, ResourceManager::Instance());
			}
		}

		// This is the sole dormant membership check. AnimationPresentation reads
		// the cached count below and never scans AnimatorInstanceStore again.
		const std::size_t animatorCount = impl_->runtimeActive.load(std::memory_order_acquire)
			? impl_->instances->ActiveCount()
			: 0U;
		// Presentation only needs a new release publication when membership changes.
		// A steady static project therefore performs a read instead of rewriting the
		// same activation cache line on every frame.
		if(impl_->scheduledAnimatorCount.load(std::memory_order_relaxed) != animatorCount) {
			impl_->scheduledAnimatorCount.store(animatorCount, std::memory_order_release);
		}
		if(animatorCount == 0U) {
			// The membership query already proves the fixed dormant facts. Profiler's
			// specialized fast path avoids building and validating a full diagnostics
			// snapshot, and remains a no-op when capture is inactive.
			TerraEngine::Profiling::Profiler::Instance().RecordDormantAnimationFacts(1U);
			if(impl_->hadScheduledAnimatorMembership) {
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
				// Retire the last active global copies once. Steady dormant frames keep
				// the zero-work fast path and never take diagnostics publication locks.
				ClearPublishedAnimationDiagnostics();
				ClearPublishedAnimationDetailedDiagnostics();
				// Captured vectors can retain active-frame storage until the one active to
				// dormant transition. Steady zero-membership frames do no lifecycle work.
				if(impl_->publishedAnimatorCount.exchange(0U, std::memory_order_acq_rel) != 0U) {
					// Retire the last complete active publication so later consumers cannot
					// mistake its generation for a current static-project result.
					impl_->authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
				}
				std::scoped_lock lock(impl_->runtimeOwnershipMutex);
				impl_->ResetFrameIntegrationStateLocked();
				impl_->capturedFrameInputs.clear();
				impl_->capturedCommands.clear();
				impl_->capturedCommandStatuses.clear();
				impl_->pendingRootMotionApplications.clear();
				impl_->frameWork.clear();
				impl_->hadScheduledAnimatorMembership = false;
			}
			return {};
		}
		impl_->hadScheduledAnimatorMembership = true;
		const std::uint64_t runtimeSessionEpoch =
			impl_->runtimeSessionEpoch.load(std::memory_order_acquire);
		const std::uint64_t frameDeclarationEpoch =
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel) + 1U;
		AnimatorPlaybackTimeInput timeInput;
		timeInput.scaledDeltaSeconds = impl_->authoredComponentPlaybackAdvancing
			&& std::isfinite(deltaTime) && deltaTime >= 0.0F
			? static_cast<double>(deltaTime)
			: 0.0;
		timeInput.unscaledDeltaSeconds = timeInput.scaledDeltaSeconds;
		timeInput.fixedDeltaTicks = 1;
	const bool useLatchedSimulationDelta = runtimeTime.playSessionActive
		&& runtimeTime.advancesSimulation
		&& std::isfinite(runtimeTime.simulationDeltaTime)
		&& runtimeTime.simulationDeltaTime >= 0.0F;
	if(useLatchedSimulationDelta) {
		// A paused explicit step must advance animation by the same fixed duration
		// as scripting and physics, not by the editor's render-frame duration.
		timeInput.scaledDeltaSeconds = runtimeTime.simulationDeltaTime;
		timeInput.unscaledDeltaSeconds = runtimeTime.simulationDeltaTime;
	}
	if(runtimeTime.frameCount != 0U
		&& !useLatchedSimulationDelta
		&& std::isfinite(runtimeTime.unscaledDeltaTime)
			&& runtimeTime.unscaledDeltaTime >= 0.0F) {
			timeInput.unscaledDeltaSeconds = runtimeTime.unscaledDeltaTime;
		}

		std::array<FrameWorkloadDeclaration, 4> descriptors;
		ConfigureProtectedWorkload(
			descriptors[0],
			"CaptureAnimationInputs",
			WorkerSystemPhaseKind::Snapshot,
			{kResourceAnimatorComponents, kResourceAnimationCommandQueues, "RuntimeTime", "RuntimeLayout",
				"Resources", "SceneGraph", "ObjectManager"},
			{kResourceAnimationFrameInputs},
			"Live animator membership, component configuration, commands, transforms, and resource views are copied before worker access.");
		ConfigureWorkerWorkload(
			descriptors[1],
			"PlanAnimationEvaluation",
			{kResourceAnimationFrameInputs, kResourceAnimationEvaluationPrograms, kResourceAnimationResidencyCapabilities},
			{kResourceAnimationExecutionModes, kResourceAnimationConsumerMasks, kResourceAnimationWorkPackets, kResourceAnimationVisualPackets},
			"Reads immutable frame/program/capability views and writes only deterministic owned plan ranges.");
		ConfigureWorkerWorkload(
			descriptors[2],
			"EvaluateAuthoritativeAnimation",
			{kResourceAnimationWorkPackets, kResourceAnimationClipSegments, kResourceAnimationRollbackState},
			{kResourceAnimationAuthoritativeOutputs},
			"Reads immutable typed packets and writes range-disjoint authoritative output without live scene or component mutation.");
		ConfigureProtectedWorkload(
			descriptors[3],
			"PublishAnimationAuthority",
			WorkerSystemPhaseKind::ProtectedCommit,
			{kResourceAnimationAuthoritativeOutputs, "SceneGraph", "ObjectManager"},
			{kResourceAnimationRuntimeState, kResourceAnimationPhysicsHandoffQueue,
				kResourceAnimationManagedCallbackBatches, "SceneMutationQueue", "SceneGraph", "ObjectManager"},
			"Complete generation-validated outputs are swapped and typed physics/copied managed handoffs are published once before Physics.");

		for(std::size_t index = 1U; index < descriptors.size(); ++index) {
			descriptors[index].explicitDependencies = {descriptors[index - 1U].stableDescriptorId};
		}
		for(FrameWorkloadDeclaration& descriptor : descriptors) {
			descriptor.active = animatorCount != 0U;
			descriptor.ready = true;
			descriptor.estimatedWorkItemCount = animatorCount;
		}
		descriptors[3].sameFrameVisibilityRequired = animatorCount != 0U;

		descriptors[0].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) {
				return;
			}
			impl_->plannedAnimatorCount.store(0U, std::memory_order_release);
			impl_->evaluatedAnimatorCount.store(0U, std::memory_order_release);
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			const AnimationFixedTick captureTick = impl_->frameTick == std::numeric_limits<AnimationFixedTick>::max()
				? impl_->frameTick
				: impl_->frameTick + 1;
			const std::uint64_t currentAuthorityGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_relaxed);
			const std::uint64_t candidateAuthorityGeneration =
				currentAuthorityGeneration == std::numeric_limits<std::uint64_t>::max()
					? 0U : currentAuthorityGeneration + 1U;
			(void)impl_->CaptureFrameLocked(captureTick, candidateAuthorityGeneration);
		};
		descriptors[1].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch, timeInput](const FrameWorkloadPartition& partition, JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			impl_->PlanRange(partition.itemBegin, partition.itemEnd, timeInput);
		};
		descriptors[2].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch](const FrameWorkloadPartition& partition, JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			impl_->EvaluatePartition(partition.partitionIndex, partition.partitionCount);
		};
		descriptors[3].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch
				|| impl_->lastPublishedFrameDeclarationEpoch == frameDeclarationEpoch) return;
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			const std::uint64_t nextAuthorityGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_relaxed) + 1U;
			if(nextAuthorityGeneration == 0U
				|| !impl_->PublishFrameLocked(
					animatorCount, frameDeclarationEpoch, nextAuthorityGeneration)) return;
			// Retain the just-committed authority state before determinism receipts are
			// published. A successful capture therefore always names a restorable value.
			impl_->CaptureRollbackHistoryLocked();
			impl_->PublishCommandCompletionsLocked(nextAuthorityGeneration);
			impl_->PublishProceduralCommandCompletionsLocked(nextAuthorityGeneration);
			impl_->PublishAdvancedCommandCompletionsLocked(nextAuthorityGeneration);
			const std::shared_ptr<const GpuAnimationPresentationFrame> presentationFrame =
				impl_->BuildGpuPresentationFrame(nextAuthorityGeneration);
			impl_->publishedAnimatorCount.store(animatorCount, std::memory_order_release);
			impl_->authorityPublicationGeneration.fetch_add(1U, std::memory_order_acq_rel);
			// Detail records are constructed only after the authoritative state has
			// committed and only while at least one scoped editor/MCP consumer owns a
			// request. This protected boundary makes the copied rows agree with the
			// summary generation without putting allocations in worker partitions.
			std::optional<AnimationDetailedDiagnosticsSnapshot> detailedDiagnostics;
			if(IsAnimationDetailedDiagnosticsRequested()) {
				try {
					detailedDiagnostics = impl_->BuildActiveDetailedDiagnosticsLocked(
						nextAuthorityGeneration);
					if(!PublishAnimationDetailedDiagnosticsSnapshot(*detailedDiagnostics)) {
						detailedDiagnostics.reset();
					}
				} catch(const std::bad_alloc&) {
					// Detailed inspection is optional. An allocation failure must never
					// roll back an otherwise complete authoritative animation frame.
				}
			}
			const AnimationDiagnosticsSnapshot diagnostics = impl_->BuildActiveDiagnosticsLocked(
				nextAuthorityGeneration,
				detailedDiagnostics ? detailedDiagnostics->animators.size() : 0U);
			if(PublishAnimationDiagnosticsSnapshot(diagnostics)) {
				TerraEngine::Profiling::Profiler::Instance()
					.RecordAnimationDiagnostics(diagnostics);
			}
			// Presentation submission is a separate protected consumer boundary. A
			// rejection (for example a newer frame or shutdown) never rolls back the
			// CPU-authoritative state committed immediately above.
			if(presentationFrame) {
				(void)AnimationPresentationSystem::Instance().SubmitGpuAnimationPresentationFrame(
					presentationFrame);
			}
		};
		return {descriptors.begin(), descriptors.end()};
	}

	AnimatorInstanceHandle AnimationSystem::CreateAnimatorInstance(AnimatorInstanceDescriptor descriptor)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)) return {};
		const AnimatorInstanceHandle handle = impl_->instances->Create(descriptor);
		if(!handle.IsValid()) return {};
		try {
			if(impl_->runtimeRecords.size() <= handle.slotIndex) {
				impl_->runtimeRecords.resize(static_cast<std::size_t>(handle.slotIndex) + 1U);
			}
			auto record = std::make_unique<Impl::RuntimeRecord>();
			record->handle = handle;
			record->descriptor = descriptor;
			// The copied ragdoll identity is generation-safe and never aliases a
			// reused animator slot. Zero remains reserved as an invalid physics key.
			record->ragdollId = (static_cast<std::uint64_t>(handle.generation) << 32U)
				| (static_cast<std::uint64_t>(handle.slotIndex) + 1U);
			(void)record->playback.Configure({});
			impl_->runtimeRecords[handle.slotIndex] = std::move(record);
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		} catch(const std::bad_alloc&) {
			(void)impl_->instances->Invalidate(handle);
			return {};
		}
		return handle;
	}

	bool AnimationSystem::InvalidateAnimatorInstance(AnimatorInstanceHandle handle)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->instances->Invalidate(handle)) return false;
		(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Component);
		(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(
			AnimationLifecycleBoundary::Component);
		(void)impl_->evaluationCache.InvalidateAnimator(handle);
		impl_->rollbackHistory.Clear(handle);
		if(impl_->proceduralCommands != nullptr) {
			impl_->proceduralCommands->RetireAnimator(
				{handle.slotIndex, handle.generation});
		}
		if(impl_->advancedCommands != nullptr) {
			impl_->advancedCommands->RetireAnimator(
				{handle.slotIndex, handle.generation});
		}
		if(handle.slotIndex < impl_->runtimeRecords.size()) {
			impl_->runtimeRecords[handle.slotIndex].reset();
		}
		AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
		std::erase_if(impl_->pendingRootMotionApplications, [handle](const AnimationRootMotionApplicationRequest& value) {
			return value.delta.animator == handle;
		});
		AnimationRootMotionResultPublisher::Instance().Retire(handle);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return true;
	}

	void AnimationSystem::SetAuthoredComponentPlaybackEnabled(bool enabled)
	{
		impl_->AssertCoordinatorThread();
		if(impl_->authoredComponentPlaybackEnabled == enabled) return;

		// Invalidate every component-owned instance before changing domains. The
		// next protected synchronization either rebuilds runtime bindings or leaves
		// them absent in the edit domain, retiring the last GPU/runtime pose.
		impl_->componentBindings.InvalidateAll(*this);
		impl_->authoredComponentPlaybackEnabled = enabled;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsAuthoredComponentPlaybackEnabled() const noexcept
	{
		return impl_->authoredComponentPlaybackEnabled;
	}

	void AnimationSystem::SetHeadlessExecutionRequired(bool required)
	{
		impl_->AssertCoordinatorThread();
		if(impl_->headlessExecutionRequired == required) return;

		// This configuration is changed only before system initialization or at a
		// protected lifecycle boundary. Existing component-owned instances are
		// invalidated so their persisted hysteresis state cannot keep a GPU-capable
		// mode after the renderer has been removed.
		impl_->componentBindings.InvalidateAll(*this);
		impl_->headlessExecutionRequired = required;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsHeadlessExecutionRequired() const noexcept
	{
		return impl_->headlessExecutionRequired;
	}

	void AnimationSystem::SetAuthoredComponentPlaybackAdvancing(bool advancing)
	{
		impl_->AssertCoordinatorThread();
		if(impl_->authoredComponentPlaybackAdvancing == advancing) return;

		// Restart each retained binding at the next protected capture boundary.
		// Keeping its handle and compiled controller prevents Edit/Play transitions
		// from exposing an asynchronous NotReady/reference-pose flash. The already
		// published complete pose stays visible until that replacement frame lands.
		impl_->componentBindings.RequestPlaybackRestart();
		impl_->authoredComponentPlaybackAdvancing = advancing;
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
	}

	bool AnimationSystem::IsAuthoredComponentPlaybackAdvancing() const noexcept
	{
		return impl_->authoredComponentPlaybackAdvancing;
	}

	AnimationStatus AnimationSystem::NotifyLifecycleInvalidation(
		AnimationLifecycleBoundary boundary)
	{
		impl_->AssertCoordinatorThread();
		AnimationStatus status = AnimationStatus::Cancelled;
		{
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			if(!impl_->runtimeActive.load(std::memory_order_acquire)) {
				return AnimationStatus::Cancelled;
			}
			status = impl_->InvalidateLifecycleLocked(boundary);
			if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
				impl_->evaluationCache.InvalidateAll();
				if(boundary != AnimationLifecycleBoundary::Renderer
					&& boundary != AnimationLifecycleBoundary::Device) {
					// World, component, asset, physics, play-domain, and shutdown changes
					// invalidate at least one generation captured by rollback state. Retain
					// visual-only history across renderer/device recovery because its CPU
					// authority and compatibility identities remain unchanged.
					impl_->rollbackHistory.ClearAll();
				}
				// Scene/domain/play/asset/physics changes invalidate captured world or
				// program inputs owned by procedural goals. Renderer/device recovery is
				// visual-only and therefore preserves CPU-authoritative gameplay goals.
				if(impl_->proceduralCommands != nullptr
					&& boundary != AnimationLifecycleBoundary::Renderer
					&& boundary != AnimationLifecycleBoundary::Device) {
					impl_->proceduralCommands->Clear();
				}
				if(impl_->advancedCommands != nullptr
					&& boundary != AnimationLifecycleBoundary::Renderer
					&& boundary != AnimationLifecycleBoundary::Device) {
					impl_->advancedCommands->Clear();
				}
				impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
			}
		}
		if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(boundary);
		}
		return status;
	}

	bool AnimationSystem::IsAnimatorInstanceAlive(AnimatorInstanceHandle handle) const
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->instances->IsAlive(handle);
	}

	AnimationStatus AnimationSystem::BindAnimatorPlayback(
		AnimatorInstanceHandle handle,
		AnimatorPlaybackBinding binding)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| record == nullptr || !impl_->instances->IsAlive(handle)) {
			return AnimationStatus::StaleHandle;
		}
		if(binding.clipGeneration == 0U || binding.skeletonGeneration == 0U
			|| binding.programGeneration == 0U || binding.stateId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(binding.controllerProgramRequired && binding.evaluationProgram) {
			const AnimationStatus stateProgramStatus = ValidateStateMachineProgram(
				*binding.evaluationProgram);
			if(stateProgramStatus != AnimationStatus::Ok) return stateProgramStatus;
			const bool initialStateExists = std::any_of(
				binding.evaluationProgram->states.begin(),
				binding.evaluationProgram->states.end(),
				[&binding](const EvaluationState& state) {
					return state.stableId == binding.stateId;
				});
			if(!initialStateExists) return AnimationStatus::InvalidArgument;
		}
		if((binding.retargetProfile
				&& (binding.retargetProfileGeneration == 0U
					|| binding.retargetProfile->runtimeGeneration
						!= binding.retargetProfileGeneration))
			|| (binding.motionDatabase
				&& (binding.motionDatabaseGeneration == 0U
					|| binding.motionDatabase->runtimeGeneration
						!= binding.motionDatabaseGeneration))
			|| (binding.ragdollProfile
				&& (binding.ragdollProfileGeneration == 0U
					|| binding.ragdollProfile->runtimeGeneration
						!= binding.ragdollProfileGeneration))) {
			return AnimationStatus::InvalidArgument;
		}
		if(record->contentGeneration == std::numeric_limits<AnimationGeneration>::max()
			|| record->historyGeneration == std::numeric_limits<AnimationGeneration>::max()
			|| record->stateGeneration == std::numeric_limits<std::uint64_t>::max()
			|| record->parameterGeneration == std::numeric_limits<std::uint64_t>::max()
			|| record->executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}
		std::vector<AnimationParameterSnapshot> initialParameters;
		if(const AnimationStatus status = ParseInitialParameters(
			binding.initialParametersJson, initialParameters); status != AnimationStatus::Ok) {
			return status;
		}
		std::vector<float> initialLayerWeights;
		try {
			if(binding.evaluationProgram) {
				initialLayerWeights.reserve(binding.evaluationProgram->layers.size());
				for(const EvaluationLayer& layer : binding.evaluationProgram->layers) {
					if(!std::isfinite(layer.defaultWeight)
						|| layer.defaultWeight < 0.0F || layer.defaultWeight > 1.0F) {
						return AnimationStatus::InvalidArgument;
					}
					initialLayerWeights.push_back(layer.defaultWeight);
				}
			}
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		// Compile the complete source/target mapping once at this protected bind
		// boundary. A malformed profile remains an unavailable optional dependency;
		// it cannot expose a partially compiled table to worker evaluation.
		binding.compiledRetargetProfile.reset();
		if(binding.retargetProfile && binding.retargetSourceSkeleton && binding.skeleton) {
			RetargetValidationResult validation;
			RetargetCompiledProfile compiled;
			if(CompileRetargetProfile(
					*binding.retargetProfile,
					*binding.retargetSourceSkeleton,
					*binding.skeleton,
					validation,
					compiled) == AnimationStatus::Ok) {
				try {
					// The compiler canonicalizes authored content and clears the runtime
					// generation. Restore the exact ResourceManager publication identity
					// before workers validate this immutable compiled table.
					compiled.profile.runtimeGeneration =
						binding.retargetProfileGeneration;
					binding.compiledRetargetProfile =
						std::make_shared<const RetargetCompiledProfile>(std::move(compiled));
				} catch(const std::bad_alloc&) {
					return AnimationStatus::InternalError;
				}
			}
		}
		const bool sourceRetargetAvailable = binding.compiledRetargetProfile
			&& binding.retargetSourceSkeleton;

		AnimationStatus dependencyStatus = AnimationStatus::Ok;
		if(binding.controllerProgramRequired && !binding.evaluationProgram) {
			dependencyStatus = binding.controllerProgramStatus == AnimationStatus::Ok
				|| binding.controllerProgramStatus == AnimationStatus::Queued
				? AnimationStatus::NotReady : binding.controllerProgramStatus;
		} else if(binding.evaluationProgram) {
			EvaluationDispatchState dispatchValidation;
			const AnimationStatus programStatus = DispatchEvaluationProgramCpu(
				*binding.evaluationProgram, dispatchValidation);
			if(programStatus != AnimationStatus::Ok) {
				dependencyStatus = programStatus;
			} else if(binding.skeleton
				&& binding.evaluationProgram->compatibilityFingerprint
					!= binding.skeleton->compatibilityFingerprint
				&& (!sourceRetargetAvailable
					|| binding.evaluationProgram->compatibilityFingerprint
						!= binding.retargetSourceSkeleton->compatibilityFingerprint)) {
				dependencyStatus = AnimationStatus::IncompatibleAsset;
			}
		}
		if(dependencyStatus == AnimationStatus::Ok && (!binding.skeleton || !binding.clip)) {
			dependencyStatus = AnimationStatus::MissingAsset;
		} else if(dependencyStatus == AnimationStatus::Ok
			&& (!binding.skeleton->assetId.IsValid() || !binding.clip->assetId.IsValid())) {
			return AnimationStatus::InvalidArgument;
		}
		const bool clipTargetsOutputSkeleton = dependencyStatus == AnimationStatus::Ok
			&& binding.skeleton && binding.clip
			&& binding.clip->compatibilityFingerprint
				== binding.skeleton->compatibilityFingerprint
			&& binding.clip->skeletonAssetId == binding.skeleton->assetId;
		const bool clipTargetsRetargetSource = dependencyStatus == AnimationStatus::Ok
			&& sourceRetargetAvailable && binding.clip
			&& binding.clip->compatibilityFingerprint
				== binding.retargetSourceSkeleton->compatibilityFingerprint
			&& binding.clip->skeletonAssetId == binding.retargetSourceSkeleton->assetId;
		if(dependencyStatus == AnimationStatus::Ok
			&& (binding.skeleton->compatibilityFingerprint == 0U
				|| (!clipTargetsOutputSkeleton && !clipTargetsRetargetSource))) {
			dependencyStatus = AnimationStatus::IncompatibleAsset;
		}

		if(binding.jointStableIds.empty() && binding.skeleton) {
			try {
				binding.jointStableIds.reserve(binding.skeleton->joints.size());
				for(const SkeletonJoint& joint : binding.skeleton->joints) {
					binding.jointStableIds.push_back(joint.stableId);
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}
		if(binding.retargetSourceJointStableIds.empty()
			&& binding.retargetSourceSkeleton) {
			try {
				binding.retargetSourceJointStableIds.reserve(
					binding.retargetSourceSkeleton->joints.size());
				for(const SkeletonJoint& joint : binding.retargetSourceSkeleton->joints) {
					binding.retargetSourceJointStableIds.push_back(joint.stableId);
				}
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
		}
		auto validStableIds = [](const std::vector<std::uint64_t>& values) {
			std::vector<std::uint64_t> sorted = values;
			if(std::any_of(sorted.begin(), sorted.end(), [](std::uint64_t value) { return value == 0U; })) {
				return false;
			}
			std::sort(sorted.begin(), sorted.end());
			return std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end();
		};
		try {
			if(!validStableIds(binding.jointStableIds)
				|| !validStableIds(binding.retargetSourceJointStableIds)
				|| !validStableIds(binding.morphStableIds)
				|| !validStableIds(binding.curveStableIds)) {
				return AnimationStatus::InvalidArgument;
			}
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		if(binding.clip) {
			const AnimationStatus fingerprintStatus =
				ComputeClipDeterministicContentFingerprint(
					*binding.clip, binding.clipDeterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok) return fingerprintStatus;
		}
		for(AnimatorControllerClipBinding& controllerClip : binding.controllerClips) {
			if(!controllerClip.clip) continue;
			const AnimationStatus fingerprintStatus =
				ComputeClipDeterministicContentFingerprint(
					*controllerClip.clip,
					controllerClip.deterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok) return fingerprintStatus;
		}

		AnimationClipView clipView;
		AnimationEventStream eventStream;
		if(dependencyStatus == AnimationStatus::Ok) {
			const SkeletonAsset* samplingSkeleton = clipTargetsRetargetSource
				? binding.retargetSourceSkeleton.get() : binding.skeleton.get();
			dependencyStatus = clipView.Open(
				*binding.clip,
				samplingSkeleton,
				std::span<const std::uint8_t>(binding.residentSegments),
				binding.clipGeneration,
				binding.streamedSegmentPayloads);
			if(dependencyStatus == AnimationStatus::Ok) {
				const AnimationAssetHandle clipHandle{binding.clip->assetId, binding.clipGeneration};
				dependencyStatus = eventStream.Configure(
					clipHandle, binding.clip->durationTicks, binding.clip->events);
			}
		}
		binding.playback.durationTicks = binding.clip ? binding.clip->durationTicks : 0;
		if(binding.updatePolicy == AnimatorUpdatePolicy::FixedStep) {
			// Fixed-step update intent is enforced at bind time so the playback clock
			// cannot accidentally consume render-frame seconds.
			binding.playback.timePolicy = AnimatorTimePolicy::Fixed;
		}
		AnimatorPlayback playback;
		if(const AnimationStatus status = playback.Configure(binding.playback);
			status != AnimationStatus::Ok) return status;
		// Seek before optional autoplay so a stopped editor binding and a running
		// runtime binding publish the same configured first pose. The coordinator
		// already clamps authored values to the currently published clip duration.
		if(const AnimationStatus status = playback.Seek(binding.initialPlaybackTick);
			status != AnimationStatus::Ok) return status;
		if(binding.playOnBind && dependencyStatus == AnimationStatus::Ok) {
			// Autoplay is applied before the new binding is published, keeping the
			// clip, event stream, and clock transition atomic at this protected
			// boundary. Missing dependencies stay stopped and retry on a later bind.
			if(const AnimationStatus status = playback.Play();
				status != AnimationStatus::Ok) return status;
		}

		if(binding.fallbackCompatibilityFingerprint == 0U && binding.skeleton) {
			binding.fallbackCompatibilityFingerprint =
				binding.skeleton->compatibilityFingerprint;
		}
		(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Asset);
		(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(
			AnimationLifecycleBoundary::Asset);
		(void)impl_->evaluationCache.InvalidateAnimator(handle);
		record->binding = std::move(binding);
		if(record->descriptor.controller.IsValid()
			&& record->binding.evaluationProgram) {
			record->descriptor.controller.generation = record->binding.programGeneration;
		}
		record->clipView = std::move(clipView);
		record->eventStream = std::move(eventStream);
		record->playback = playback;
		record->parameters = std::move(initialParameters);
		record->layerWeights = std::move(initialLayerWeights);
		record->graphState = {};
		record->transition = {};
		record->dependencyStatus = dependencyStatus;
		++record->contentGeneration;
		++record->historyGeneration;
		++record->stateGeneration;
		++record->parameterGeneration;
		++record->executionModeGeneration;
		// Rebinding replaces the immutable program/resource identity, so retaining
		// promotion hysteresis from the previous binding could select a stale mode.
		record->executionModeState = {};
		// A hot-rebind immediately retires the old generation bundle. Consumers may
		// use their previously copied pointer, but a fresh lookup cannot mix it with
		// the new dependency identity before the next complete publication.
		std::atomic_store_explicit(
			&record->publishedPose,
			std::shared_ptr<const PublishedAnimatorPose>{},
			std::memory_order_release);
		record->previousPublishedPose.reset();
		// Socket/bone attachment copies belong to the retired pose generation too.
		// Remove them in the same protected rebind operation so a fresh ABI read can
		// never pair an old transform set with the new resource identity.
		AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
		if(impl_->advancedCommands != nullptr) {
			impl_->advancedCommands->RetireAnimator({handle.slotIndex, handle.generation});
		}
		if(record->ragdollGeneration.IsValid()
			&& record->ragdollLifecycle != RagdollLifecycleState::Inactive) {
			RagdollPhysicsRequest teardown;
			teardown.kind = RagdollPhysicsRequestKind::Teardown;
			teardown.ragdollId = record->ragdollId;
			teardown.generation = record->ragdollGeneration;
			teardown.expectedCurrentGeneration = record->ragdollGeneration;
			teardown.correlationId = record->ragdollId;
			teardown.producerOrder = record->ragdollId;
			(void)TerraEngine::Physics::PhysicsSystem::Instance()
				.SubmitRagdollPhysicsRequest(std::move(teardown));
		}
		record->activeRetargetProfileId = {};
		record->activeRetargetProfileGeneration = 0U;
		record->latestRetargetMetrics = {};
		record->latestRetargetDeterministicHash = 0U;
		record->activeMotionDatabaseId = {};
		record->activeMotionDatabaseGeneration = 0U;
		record->latestMotionQueryId = 0U;
		record->appliedMotionQueryId = 0U;
		record->latestMotionCandidate = {};
		record->hasMotionCandidate = false;
		record->ragdollGeneration = {};
		record->activeRagdollProfileId = {};
		record->ragdollCreated = false;
		record->ragdollLifecycle = RagdollLifecycleState::Inactive;
		record->ragdollBlendWeight = 0.0F;
		record->ragdollLifecycleStartTick = 0;
		record->ragdollRecoveryCandidateId = 0U;
		record->ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
		record->ragdollRecoveryAlignedRootPosition = {};
		record->ragdollRecoveryDurationSeconds = 0.0F;
		record->ragdollMotorDriveCount = 0U;
		record->ragdollMotorStatus = AnimationStatus::NotReady;
		record->pendingRagdollCorrelationId = 0U;
		record->pendingRagdollPreviousGeneration = {};
		std::erase_if(impl_->pendingRootMotionApplications, [handle](const AnimationRootMotionApplicationRequest& value) {
			return value.delta.animator == handle;
		});
		AnimationRootMotionResultPublisher::Instance().Retire(handle);
		impl_->rollbackHistory.Clear(handle);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return dependencyStatus;
	}

	AnimationStatus AnimationSystem::UpdateEvaluationGenerations(
		AnimatorInstanceHandle handle,
		AnimationEvaluationExternalGenerations generations)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| record == nullptr || !impl_->instances->IsAlive(handle)) {
			return AnimationStatus::StaleHandle;
		}
		if(generations.unchangedInputReuseEligible && !generations.IsComplete()) {
			return AnimationStatus::InvalidArgument;
		}
		if(record->binding.evaluationGenerations == generations) {
			return AnimationStatus::Ok;
		}

		record->binding.evaluationGenerations = generations;
		(void)impl_->evaluationCache.InvalidateAnimator(handle);
		(void)impl_->evaluationCancellationSource.Cancel(
			AnimationCancellationReason::Explicit);
		impl_->workCancellationSource.RequestCancellation();
		impl_->frameCancellationRequested.store(true, std::memory_order_release);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return AnimationStatus::Ok;
	}

	std::shared_ptr<const AnimationSnapshot> AnimationSystem::LoadAnimatorSnapshot(
		AnimatorInstanceHandle handle) const noexcept
	{
		const std::shared_ptr<const PublishedAnimatorPose> published = LoadPublishedAnimatorPose(handle);
		return published ? published->snapshot : std::shared_ptr<const AnimationSnapshot>{};
	}

	std::shared_ptr<const PublishedAnimatorPose> AnimationSystem::LoadPublishedAnimatorPose(
		AnimatorInstanceHandle handle) const noexcept
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(record == nullptr || !impl_->instances->IsAlive(handle)) return {};
		return std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
	}

	std::shared_ptr<const PublishedAnimatorPose> AnimationSystem::LoadPublishedAnimatorPoseForObject(
		const UUID& ownerObjectId) const noexcept
	{
		if(ownerObjectId == EMPTY_UUID) return {};
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		for(const std::unique_ptr<Impl::RuntimeRecord>& record : impl_->runtimeRecords) {
			if(record && record->descriptor.ownerObjectId == ownerObjectId
				&& impl_->instances->IsAlive(record->handle)) {
				return std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
			}
		}
		return {};
	}

	AnimatorPlaybackStateSnapshot AnimationSystem::LoadAnimatorPlaybackState(
		AnimatorInstanceHandle handle) const noexcept
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		return record != nullptr && impl_->instances->IsAlive(handle)
			? record->playback.Snapshot()
			: AnimatorPlaybackStateSnapshot{};
	}

	AnimationCommandReceipt AnimationSystem::SubmitCommand(AnimationCommand command)
	{
		// Command submission is the intentional cross-producer entry point. The
		// short lifecycle guard makes queue/store ownership atomic with Shutdown;
		// command ordering and payload storage remain inside AnimationCommandQueue.
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			// Only Initialize may begin another runtime session. A producer arriving
			// during or after shutdown gets a copied terminal receipt and cannot
			// resurrect queue ownership on its thread.
			AnimationCommandReceipt receipt;
			receipt.target = command.target;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.status = AnimationStatus::Cancelled;
			receipt.targetTick = command.targetTick;
			return receipt;
		}
		return impl_->commands->Enqueue(std::move(command), *impl_->instances);
	}

	AnimationCommandBatchReceipt AnimationSystem::SubmitCommandBatch(
		std::span<const AnimationCommand> commands)
	{
		// The lifecycle lock keeps the instance store and command queue from being
		// retired between validation and the queue's all-or-nothing insertion.
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire) && impl_->commands != nullptr) {
			return impl_->commands->EnqueueBatch(commands, *impl_->instances);
		}

		AnimationCommandBatchReceipt batch;
		batch.status = commands.empty() ? AnimationStatus::InvalidArgument : AnimationStatus::Cancelled;
		try {
			batch.commands.reserve(commands.size());
			for(const AnimationCommand& command : commands) {
				AnimationCommandReceipt receipt;
				receipt.target = command.target;
				receipt.producerId = command.producerId;
				receipt.producerSequence = command.producerSequence;
				receipt.correlationId = command.correlationId;
				receipt.idempotencyKey = command.idempotencyKey;
				receipt.cancellationToken = command.cancellationToken;
				receipt.status = batch.status;
				receipt.targetTick = command.targetTick;
				batch.commands.push_back(receipt);
			}
		}
		catch(const std::bad_alloc&) {
			batch.commands.clear();
			batch.status = AnimationStatus::InternalError;
		}
		return batch;
	}

	AnimationCommandReceipt AnimationSystem::CancelCommand(
		std::uint64_t cancellationToken,
		std::uint64_t correlationId)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			AnimationCommandReceipt receipt;
			receipt.cancellationToken = cancellationToken;
			receipt.correlationId = correlationId;
			receipt.status = AnimationStatus::Cancelled;
			return receipt;
		}
		return impl_->commands->Cancel(cancellationToken, correlationId);
	}

	AnimationCommandReceipt AnimationSystem::ReadCommandReceipt(
		std::uint32_t producerId,
		std::uint64_t producerSequence,
		std::uint64_t correlationId) const
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire) || impl_->commands == nullptr) {
			AnimationCommandReceipt receipt;
			receipt.producerId = producerId;
			receipt.producerSequence = producerSequence;
			receipt.correlationId = correlationId;
			receipt.status = AnimationStatus::Cancelled;
			return receipt;
		}
		return impl_->commands->ReadReceipt(producerId, producerSequence, correlationId);
	}

	AnimationRootMotionApplicationReceipt AnimationSystem::SubmitRootMotionApplication(
		AnimationRootMotionDeferredApplicationRequest request)
	{
		AnimationRootMotionApplicationReceipt receipt;
		receipt.animator = request.animator;
		receipt.mode = request.mode;
		receipt.generation = request.expectedPoseGeneration;
		receipt.producerId = request.producerId;
		receipt.producerSequence = request.producerSequence;
		receipt.correlationId = request.correlationId;
		receipt.idempotencyKey = request.idempotencyKey;
		receipt.publicationGeneration = request.expectedPublicationGeneration;
		AnimationRootMotionApplicationRequest resolved;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		{
			if(!impl_->runtimeActive.load(std::memory_order_acquire)) {
				receipt.status = AnimationStatus::Cancelled;
				return receipt;
			}
			const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(request.animator);
			if(record == nullptr || !impl_->instances->IsAlive(request.animator)) {
				receipt.status = AnimationStatus::StaleHandle;
				return receipt;
			}
			const std::shared_ptr<const PublishedAnimatorPose> published =
				std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
			if(!published || !published->snapshot || !published->snapshot->pose
				|| !published->snapshot->pose->RootMotionDelta()) {
				receipt.status = AnimationStatus::NotReady;
				return receipt;
			}
			const AnimationRootMotionDelta& delta = *published->snapshot->pose->RootMotionDelta();
			if(request.expectedPoseGeneration == 0U
				|| request.expectedPublicationGeneration == 0U) {
				receipt.status = AnimationStatus::InvalidArgument;
				return receipt;
			}
			if(delta.generation != request.expectedPoseGeneration
				|| published->authorityPublicationGeneration
					!= request.expectedPublicationGeneration) {
				receipt.status = AnimationStatus::StaleHandle;
				return receipt;
			}
			resolved.delta = delta;
			resolved.ownerObjectId = published->ownerObjectId;
			resolved.objectGeneration = published->objectGeneration;
			resolved.componentGeneration = published->componentGeneration;
			resolved.sourceObjectWorldTransform = published->sourceObjectWorldTransform;
			resolved.sourceObjectWorldTransformCaptured =
				published->sourceObjectWorldTransformCaptured;
			if(resolved.sourceObjectWorldTransformCaptured) {
				const AnimationFloat3 scaled{
					delta.localTranslation.x * resolved.sourceObjectWorldTransform.scale.x,
					delta.localTranslation.y * resolved.sourceObjectWorldTransform.scale.y,
					delta.localTranslation.z * resolved.sourceObjectWorldTransform.scale.z};
				resolved.delta.worldTranslation = RotateVector(
					resolved.sourceObjectWorldTransform.rotation, scaled);
				resolved.delta.worldYawRadians = delta.localYawRadians;
			}
			resolved.mode = request.mode;
			resolved.producerId = request.producerId;
			resolved.producerSequence = request.producerSequence;
			resolved.correlationId = request.correlationId;
			resolved.idempotencyKey = request.idempotencyKey;
			resolved.publicationGeneration = request.expectedPublicationGeneration;
			resolved.fixedDurationSeconds = request.fixedDurationSeconds;
		}
		if(impl_->pendingRootMotionApplications.size()
			>= kMaximumPendingRootMotionApplications) {
			receipt.status = AnimationStatus::HardLimitExceeded;
			return receipt;
		}
		try {
			// Grow the copied queue before reserving the once-only publisher record.
			// The request itself owns no dynamic storage, so push_back cannot allocate
			// after this point and the reservation remains fail-atomic.
			impl_->pendingRootMotionApplications.reserve(
				impl_->pendingRootMotionApplications.size() + 1U);
		} catch(const std::bad_alloc&) {
			receipt.status = AnimationStatus::InternalError;
			return receipt;
		}
		bool shouldQueue = false;
		receipt = AnimationRootMotionResultPublisher::Instance().ReserveApplication(
			resolved, shouldQueue);
		if(!shouldQueue) return receipt;
		impl_->pendingRootMotionApplications.push_back(resolved);
		return receipt;
	}

	NativeAnimationProceduralGoalReceipt AnimationSystem::SubmitProceduralGoal(
		const NativeAnimationProceduralGoalCommand& command)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			NativeAnimationProceduralGoalReceipt receipt{};
			receipt.header = {
				static_cast<std::uint32_t>(sizeof(receipt)),
				kNativeAnimationAbiVersion,
				static_cast<std::uint16_t>(NativeAnimationRecordKind::ProceduralGoalReceipt)};
			receipt.status = NativeAnimationStatus::Cancelled;
			receipt.animator = command.animator;
			receipt.command = command.command;
			receipt.producerId = command.producerId;
			receipt.producerSequence = command.producerSequence;
			receipt.correlationId = command.correlationId;
			receipt.idempotencyKey = command.idempotencyKey;
			receipt.cancellationToken = command.cancellationToken;
			receipt.goalId = command.goalId;
			receipt.observableTick = command.targetTick;
			return receipt;
		}
		return impl_->proceduralCommands->Submit(command, *impl_->instances);
	}

	AnimationStatus AnimationSystem::CopyProceduralResults(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationProceduralResultSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			return AnimationStatus::Cancelled;
		}
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->proceduralCommands->CopyResults(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyProceduralGoals(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationProceduralGoalCommand> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| impl_->proceduralCommands == nullptr || impl_->instances == nullptr) {
			return AnimationStatus::Cancelled;
		}
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->proceduralCommands->CopyActiveGoals(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	NativeAnimationRetargetReceipt AnimationSystem::SubmitRetargetRequest(
		const NativeAnimationRetargetRequest& request)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire)
			&& impl_->advancedCommands && impl_->instances) {
			NativeAnimationRetargetReceipt receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationRetargetReceipt receipt{};
		receipt.header = MakeNativeHeader(
			NativeAnimationRecordKind::RetargetReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.profileGeneration = request.profileGeneration;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationMotionMatchReceipt AnimationSystem::SubmitMotionMatchRequest(
		const NativeAnimationMotionMatchRequest& request)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire)
			&& impl_->advancedCommands && impl_->instances) {
			NativeAnimationMotionMatchReceipt receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationMotionMatchReceipt receipt{};
		receipt.header = MakeNativeHeader(
			NativeAnimationRecordKind::MotionMatchReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.databaseGeneration = request.databaseGeneration;
		receipt.queryId = request.queryId;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationMotionMatchQueryReceiptV2 AnimationSystem::SubmitMotionMatchQueryV2(
		const NativeAnimationMotionMatchQueryRequestV2& request)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire)
			&& impl_->advancedCommands && impl_->instances) {
			NativeAnimationMotionMatchQueryReceiptV2 receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationMotionMatchQueryReceiptV2 receipt{};
		receipt.header = MakeNativeHeader(
			NativeAnimationRecordKind::MotionMatchQueryReceiptV2, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.databaseGeneration = request.databaseGeneration;
		receipt.schemaFingerprint = request.schemaFingerprint;
		receipt.queryId = request.queryId;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationRagdollReceipt AnimationSystem::SubmitRagdollRequest(
		const NativeAnimationRagdollRequest& request)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire)
			&& impl_->advancedCommands && impl_->instances) {
			NativeAnimationRagdollReceipt receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationRagdollReceipt receipt{};
		receipt.header = MakeNativeHeader(
			NativeAnimationRecordKind::RagdollReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.profileGeneration = request.profileGeneration;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	NativeAnimationDeterminismReceipt AnimationSystem::SubmitDeterminismRequest(
		const NativeAnimationDeterminismRequest& request)
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(impl_->runtimeActive.load(std::memory_order_acquire)
			&& impl_->advancedCommands && impl_->instances) {
			NativeAnimationDeterminismReceipt receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			receipt.publicationGeneration =
				impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
			return receipt;
		}
		NativeAnimationDeterminismReceipt receipt{};
		receipt.header = MakeNativeHeader(
			NativeAnimationRecordKind::DeterminismReceipt, sizeof(receipt));
		receipt.status = NativeAnimationStatus::Cancelled;
		receipt.animator = request.animator;
		receipt.operation = request.operation;
		receipt.correlationId = request.correlationId;
		receipt.observableTick = request.targetTick;
		return receipt;
	}

	AnimationStatus AnimationSystem::CopyRetargetSnapshots(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationRetargetSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->advancedCommands || !impl_->instances) return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyMotionMatchSnapshots(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationMotionMatchSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->advancedCommands || !impl_->instances) return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyMotionMatchResultsV2(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationMotionMatchResultV2> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->advancedCommands || !impl_->instances) return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyRagdollSnapshots(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationRagdollSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->advancedCommands || !impl_->instances) return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimationStatus AnimationSystem::CopyDeterminismSnapshots(
		AnimatorInstanceHandle handle,
		std::span<NativeAnimationDeterminismSnapshot> destination,
		std::uint32_t& requiredCount) const
	{
		requiredCount = 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| !impl_->advancedCommands || !impl_->instances) return AnimationStatus::Cancelled;
		if(!impl_->instances->IsAlive(handle)) return AnimationStatus::StaleHandle;
		return impl_->advancedCommands->Copy(
			{handle.slotIndex, handle.generation}, destination, requiredCount);
	}

	AnimatorInstanceHandle AnimationSystem::ResolveAnimatorForObject(
		const UUID& ownerObjectId) const noexcept
	{
		if(ownerObjectId == EMPTY_UUID) return {};
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		for(const std::unique_ptr<Impl::RuntimeRecord>& record : impl_->runtimeRecords) {
			if(record && record->descriptor.ownerObjectId == ownerObjectId
				&& impl_->instances->IsAlive(record->handle)) return record->handle;
		}
		return {};
	}

	void AnimationSystem::CopyPublishedAnimatorPoses(
		std::vector<std::shared_ptr<const PublishedAnimatorPose>>& output) const
	{
		std::vector<std::shared_ptr<const PublishedAnimatorPose>> staged;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		try {
			staged.reserve(impl_->runtimeRecords.size());
			for(const std::unique_ptr<Impl::RuntimeRecord>& record : impl_->runtimeRecords) {
				if(!record || !impl_->instances->IsAlive(record->handle)) continue;
				std::shared_ptr<const PublishedAnimatorPose> pose =
					std::atomic_load_explicit(&record->publishedPose, std::memory_order_acquire);
				if(pose) staged.push_back(std::move(pose));
			}
			output.swap(staged);
		} catch(const std::bad_alloc&) {
			// A copied batch is atomic: allocation failure keeps the caller's prior
			// complete value instead of exposing an arbitrary prefix.
		}
	}

	std::size_t AnimationSystem::PendingCommandCount() const
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->commands == nullptr ? 0U : impl_->commands->PendingCount();
	}

	std::size_t AnimationSystem::CapturedCommandCount() const
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->capturedCommands.size();
	}

	std::size_t AnimationSystem::ScheduledAnimatorCount() const noexcept
	{
		return impl_->scheduledAnimatorCount.load(std::memory_order_acquire);
	}

	std::size_t AnimationSystem::PublishedAnimatorCount() const noexcept
	{
		return impl_->publishedAnimatorCount.load(std::memory_order_acquire);
	}

	std::uint64_t AnimationSystem::AuthorityPublicationGeneration() const noexcept
	{
		return impl_->authorityPublicationGeneration.load(std::memory_order_acquire);
	}

	AnimationProductionIntegrationSnapshot
	AnimationSystem::ReadProductionIntegrationSnapshot() const noexcept
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		std::scoped_lock finalizeLock(impl_->workPlanFinalizeMutex);
		AnimationProductionIntegrationSnapshot snapshot;
		snapshot.planStatus = impl_->workPlan.status;
		snapshot.executionPolicy = impl_->workPlan.executionPolicy;
		snapshot.availableWorkerCount = impl_->availableWorkerCount;
		snapshot.selectedWorkerCount = impl_->workPlan.selectedWorkerCount;
		snapshot.itemCount = impl_->workPlan.items.size();
		snapshot.rangeCount = impl_->workPlan.ranges.size();
		snapshot.scratchBytes = impl_->workPlan.totalScratchBytes;
		snapshot.cacheEntryCount = impl_->evaluationCache.Size();
		snapshot.reusedItemCount =
			impl_->cacheReusedAnimatorCount.load(std::memory_order_acquire);
		snapshot.fallbackItemCount =
			impl_->fallbackAnimatorCount.load(std::memory_order_acquire);
		snapshot.memory = impl_->memoryController.Snapshot();
		snapshot.deterministicPublicationValidated =
			impl_->deterministicPublicationValidated;
		snapshot.cancellationRequested =
			impl_->frameCancellationRequested.load(std::memory_order_acquire);
		return snapshot;
	}
}
