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
#include "Profiling/ProfilerScope.h"
#include "Physics/PhysicsSystem.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneMutationQueue.h"
#include "Scripting/ScriptingSystem.h"
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"
#include "Core/Logger/Logger.h"

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <nlohmann/json.hpp>

namespace TerraEngine::Animation {
	namespace {
		constexpr std::size_t kInlineAnimatorThreshold = 16U;
		// A 128-animator partition keeps a large crowd at roughly one
		// planning/evaluation job per worker on the production 32-worker pool. The
		// former 64-item grain created two dispatch waves and paid queue, profiler,
		// and dependency bookkeeping twice without exposing more useful parallelism.
		constexpr std::size_t kAnimatorsPerWorkerPartition = 128U;
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
				// immutable controller evaluator is completed in future controller work.
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
			// The propagated canonical pose is the sole world-space authority. The
			// animation graph receives a bounded offset for local joint math only.
			const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
			if(!pose) return false;
			output.translation = {
				static_cast<float>(pose->position.offsetMetres.x),
				static_cast<float>(pose->position.offsetMetres.y),
				static_cast<float>(pose->position.offsetMetres.z)};
			output.rotation = {pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w};
			output.scale = {pose->scale.x, pose->scale.y, pose->scale.z};
			return IsFinite(output) && IsNormalized(output.rotation);
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
			// A newly allocated runtime slot has no published pose, attachment, rollback,
			// or advanced-command state to retire. Remember the first successful bind so
			// crowd creation can avoid thousands of empty cross-subsystem retire calls.
			bool bindingInitialized = false;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			// Direct clips without authored events never mutate delivery history.
			// Keeping this explicit lets protected capture and publication skip the
			// event stream's deque/set copy while preserving the live record unchanged.
			bool eventStateCaptured = false;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 1U;
			std::uint64_t parameterGeneration = 1U;
			std::uint64_t executionModeGeneration = 1U;
			AnimationGeneration contentGeneration = 1U;
			// A history generation names one compatible rollback ring. Rebind and
			// lifecycle invalidation advance it before any replacement snapshot is kept.
			AnimationGeneration historyGeneration = 1U;
			// Ordinary visual animation does not need a deep deterministic snapshot on
			// every frame. The first valid rollback/replication request opts this
			// animator into continuous bounded retention until its runtime record dies.
			bool rollbackHistoryActive = false;
			AnimationExecutionModeState executionModeState{};
			std::vector<AnimationParameterSnapshot> parameters;
			// Runtime layer weights start from immutable authored defaults, then change
			// only through copied commands at the protected animation publication.
			std::vector<float> layerWeights;
			// Controller state advances on worker-owned copies and is committed only
			// with the same all-or-none authority publication as the sampled pose.
			StateMachineSnapshot graphState;
			ControllerTransitionRuntime transition;
			// Published pose state lives exclusively in AnimationAuthorityFrame.  Runtime
			// records retain mutable evaluator state only; workers capture prior rows from
			// the immutable root held for the whole frame.
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
			// Physics retains the completed recovery ticket after the ragdoll body
			// is deactivated. An unsuccessful character handoff can be retried at
			// later protected boundaries without reviving stale root motion.
			std::optional<Physics::RagdollExternalRecoveryToken> pendingExternalRecoveryToken;
			bool pendingRagdollExternalRecovery = false;
			bool pendingExternalRecoveryNeedsRefresh = false;
			// A rejected Activate does not implicitly return an already-granted
			// physical ExternalExclusive lease to locomotion. A later explicit
			// abort/recovery request must name this terminal failure.
			std::uint64_t failedRagdollActivateCorrelationId = 0U;
			RagdollGenerationStamp failedRagdollActivateGeneration{};
			std::uint64_t failedRagdollActivatePublicationGeneration = 0U;
			AnimationFixedTick failedRagdollActivateFrameTick = 0;
		};
		// Runtime configuration remains protected while it is authored, but frame
		// finalization owns a retained immutable root.  Replacing this root is one
		// atomic operation, so a worker can prepare a complete next crowd state
		// without exposing partial per-record updates.
		// Runtime records change much less frequently than compact GPU-visual state.
		// Keep them in copy-on-write pages so a mixed crowd with a handful of CPU
		// exceptions clones only the pages containing those exceptions. The previous
		// flat vector copied every shared_ptr in a 4,000-owner crowd before changing
		// seven rows, even though protected publication ultimately swaps one root.
		class RuntimeRecordRoot {
		public:
			using Record = std::shared_ptr<RuntimeRecord>;
			static constexpr std::size_t kPageCapacity = 128U;

			[[nodiscard]] std::size_t size() const noexcept { return size_; }

			[[nodiscard]] const Record& at(std::size_t index) const
			{
				if(index >= size_) throw std::out_of_range("runtime record slot");
				return pages_.at(index / kPageCapacity)->records[index % kPageCapacity];
			}

			void resize(std::size_t size)
			{
				const std::size_t requiredPages =
					(size + kPageCapacity - 1U) / kPageCapacity;
				pages_.reserve(requiredPages);
				while(pages_.size() < requiredPages) {
					pages_.push_back(std::make_shared<Page>());
				}
				size_ = size;
			}

			void Set(std::size_t index, Record record)
			{
				if(index >= size_) throw std::out_of_range("runtime record slot");
				const std::size_t pageIndex = index / kPageCapacity;
				// A copied root initially shares every immutable page. Clone the one page
				// before replacing a slot so rejected candidates cannot alter the live root.
				if(pages_[pageIndex].use_count() != 1U) {
					pages_[pageIndex] = std::make_shared<Page>(*pages_[pageIndex]);
				}
				pages_[pageIndex]->records[index % kPageCapacity] = std::move(record);
			}

			template<typename Visitor>
			void ForEach(Visitor&& visitor) const
			{
				for(std::size_t index = 0U; index < size_; ++index) {
					visitor(at(index));
				}
			}

		private:
			struct Page {
				std::array<Record, kPageCapacity> records{};
			};

			std::vector<std::shared_ptr<Page>> pages_;
			std::size_t size_ = 0U;
		};

		// Visual-only animators advance a small scalar state while their binding and
		// specialist configuration stay immutable. Publishing this dense root avoids
		// writing through thousands of scattered RuntimeRecord allocations after the
		// authority root is visible. A complete-path transition materializes only its
		// own row before modifying the broad runtime record.
		struct CompactRuntimeState {
			AnimatorInstanceHandle handle{};
			AnimatorPlayback playback{};
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 0U;
			std::uint64_t executionModeGeneration = 0U;
			// This generation identifies the immutable binding whose clip, program,
			// root-motion policy, and resource layout passed compact admission. A warm
			// frame may reuse that proof only while the live runtime record still names
			// the exact same binding generation; dynamic GPU health and mode are always
			// checked again before the owner bypasses complete evaluation.
			AnimationGeneration bindingContentGeneration = 0U;
			AnimationExecutionModeState executionModeState{};

			[[nodiscard]] bool IsValidFor(AnimatorInstanceHandle expected) const noexcept
			{
				return handle == expected && poseGeneration != 0U
					&& stateGeneration != 0U && executionModeGeneration != 0U;
			}
		};
		using CompactRuntimeStateRoot = std::vector<CompactRuntimeState>;

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
			// Original capture position remains stable after complete records are packed
			// around the separate compact SoA column for deterministic publication.
			std::size_t capturedInputIndex = 0U;
			AnimationFrameInput input;
			// Captured from Locomotion's immutable sorted binding table. Worker
			// evaluation never calls back into the live authority coordinator.
			bool rootMotionEnabled = false;
			// Compact captures keep this authority slot only for deterministic commit
			// order. Their broad evaluator state below is never populated; the tagged
			// payload owns the only data a visual-only GPU packet may observe.
			struct CompactCapture {
				std::shared_ptr<const EvaluationProgram> program;
				std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding> fastBinding;
				AnimationGpuExecutionInputs gpuExecution{};
				AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
				AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
				AnimationGeneration programGeneration = 0U;
				AnimationGeneration clipGeneration = 0U;
				AnimationGeneration skeletonGeneration = 0U;
				std::uint32_t jointCount = 0U;
			} compactCapture{};
			bool compactCaptureOnly = false;
			AnimatorPlaybackBinding binding;
			// The two frame-work arrays alternate ownership. Remember which immutable
			// binding generation each slot already contains so an unchanged animator
			// does not deep-copy controller clips, joint ids, morph ids, and resident
			// segment tables again when this slot returns two frames later.
			AnimatorInstanceHandle bindingCaptureHandle{};
			AnimationGeneration bindingCaptureContentGeneration = 0U;
			// These hashes cover only immutable binding data. Alternating frame slots
			// retain them with the deep-copied binding, while per-frame consumer and
			// retarget bits are appended during finalization.
			std::uint64_t bindingOutputRequirementsFingerprint = 0U;
			std::uint64_t bindingResourceLayoutFingerprint = 0U;
			// Generation equality is reusable only after protected publication commits
			// this item's worker mutations back to the live runtime record.
			bool runtimeStateCaptureCommitted = false;
			AnimatorPlayback playback;
			AnimationClipView clipView;
			AnimationEventStream eventStream;
			// Direct-clip crowds usually have no authored events. Track whether this
			// frame actually captured the retained event cursor so capture and commit
			// can avoid copying an otherwise dormant deque and identifier set.
			bool eventStateCaptured = false;
			AnimationStatus dependencyStatus = AnimationStatus::MissingAsset;
			AnimatorPlaybackAdvanceResult advance;
			// Physical interval time is selected before clip playback speed changes
			// source ticks. It is never reconstructed from advancedTicks.
			double physicalEvaluationDurationSeconds = 0.0;
			std::shared_ptr<PoseBuffer> pose;
			// CPU visual-only culling reuses this immutable prior pose while still
			// publishing current playback state and generations for the animator.
			std::shared_ptr<const PoseBuffer> retainedVisualPose;
			std::shared_ptr<const AnimationAuthorityRow> publication;
			AnimationStatus evaluationStatus = AnimationStatus::NotReady;
			AnimationGeneration candidatePoseGeneration = 0U;
			AnimationGeneration capturedPoseGeneration = 0U;
			std::uint64_t candidateAuthorityPublicationGeneration = 0U;
			std::uint64_t visualPoseContentIdLow = 0U;
			std::uint64_t visualPoseContentIdHigh = 0U;
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
			// Controller snapshots are copied only when an immutable evaluation
			// program can consume them. Direct-clip crowds otherwise carried three
			// dormant history vectors through both capture and commit every frame.
			bool controllerStateCaptured = false;
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
			std::shared_ptr<const AnimationAuthorityRow> priorPublication;
			AnimationGeneration sourceObjectGeneration = 0U;
			AnimationTransform sourceObjectWorldTransform{};
			bool sourceObjectWorldTransformCaptured = false;
			// These revisions name the protected scene snapshot that produced the
			// copied world transform. An unchanged scene can reuse the alternating
			// frame slot instead of resolving the same crowd object again.
			std::uint64_t sourceObjectSceneGeneration = 0U;
			std::uint64_t sourceObjectSpatialRevision = 0U;
			AnimationExecutionModeDecision executionDecision{};
			AnimationResolvedConsumerRequirements resolvedConsumers{};
			AnimationEvaluationCacheKey cacheKey{};
			AnimationEvaluationCacheRequirements cacheRequirements{};
			AnimationFallbackGenerationKey fallbackGenerations{};
			AnimationFallbackGenerationKey liveFallbackGenerations{};
			AnimationFallbackDecision fallbackDecision{};
			std::shared_ptr<const AnimationAuthorityRow> selectedPublication;
			std::optional<GpuAnimationLoweringInput> gpuLowering;
			// This record is the compact-path admission proof copied from protected
			// capture. Workers classify it without reopening scene/component storage.
			CompactGpuVisualInput compactGpuVisualInput{};
			AnimationProcessingClass processingClass = AnimationProcessingClass::Complete;
			// Preserve the checked request mask produced during preflight. Detailed
			// diagnostics must report the stages that will actually be dispatched,
			// rather than a hard-coded legacy bit pattern.
			std::uint32_t gpuStageMask = 0U;
			bool cacheEligible = false;
			bool cacheReused = false;
			bool visibleForVisualEvaluation = true;
			bool visualEvaluationSuspended = false;
			bool safePose = true;
			bool planned = false;
			bool evaluated = false;

			void ResetForCaptureReuse() noexcept
			{
				// Candidate storage alternates with the published frame-work vector. Clear
				// produced values without replacing input vectors so their allocations can
				// be reused when the same animator is captured again two frames later.
				// AnimationClipView::Open validates its retained immutable identity and
				// residency bitmap. Keeping it alive enables the exact-match fast path.
				// Binding fingerprints follow that same retained immutable storage and
				// are invalidated only when capture replaces part of the binding below.
				advance = {};
				rootMotionEnabled = false;
				physicalEvaluationDurationSeconds = 0.0;
				pose.reset();
				retainedVisualPose.reset();
				publication.reset();
				evaluationStatus = AnimationStatus::NotReady;
				eventStateCaptured = false;
				visualPoseContentIdLow = 0U;
				visualPoseContentIdHigh = 0U;
				retargetActive = false;
				sampledSourceSkeleton = false;
				retargetMetrics = {};
				retargetDeterministicHash = 0U;
				retargetEvaluationStatus = AnimationStatus::NotReady;
				controllerStateCaptured = false;
				pendingMotionQueries.clear();
				evaluatedMotionQueries.clear();
				pendingMotionQueriesV2.clear();
				evaluatedMotionQueriesV2.clear();
				restoredFromHistory = false;
				activeProceduralGoals.clear();
				proceduralResults.clear();
				proceduralStateObserved = false;
				ragdollPhysicsPose.reset();
				ragdollMotorDrives.clear();
				ragdollRecoveryBlendComplete = false;
				priorPublication.reset();
				// Source-object state is retained until CaptureFrameLocked compares its
				// scene and spatial revisions. That protected comparison either reuses
				// this exact value or clears it before any worker can observe the slot.
				executionDecision = {};
				resolvedConsumers = {};
				cacheKey = {};
				cacheRequirements = {};
				fallbackGenerations = {};
				liveFallbackGenerations = {};
				fallbackDecision = {};
				selectedPublication.reset();
				gpuLowering.reset();
				compactGpuVisualInput = {};
				compactCapture = {};
				compactCaptureOnly = false;
				processingClass = AnimationProcessingClass::Complete;
				gpuStageMask = 0U;
				cacheEligible = false;
				cacheReused = false;
				visibleForVisualEvaluation = true;
				visualEvaluationSuspended = false;
				safePose = true;
				planned = false;
				evaluated = false;
				runtimeStateCaptureCommitted = false;
			}
		};

		// Specialist consumers receive self-contained copied operations from the
		// worker seal. The protected finalizer may submit these sparse operations,
		// but it must never reopen FrameWorkItem or rediscover participants by
		// scanning the animator population after the authority root is visible.
		struct PreparedRagdollPublication {
			std::shared_ptr<RuntimeRecord> runtimeRecord;
			std::uint32_t motorDriveCount = 0U;
			std::optional<RagdollPhysicsRequest> motorRequest;
			std::optional<RagdollPhysicsRequest> deactivateRequest;
		};

		struct PreparedProceduralPublication {
			NativeAnimationAnimatorHandle animator{};
			std::vector<NativeAnimationProceduralResultSnapshot> results;
		};

		struct PreparedRetargetPublication {
			NativeAnimationRetargetSnapshot snapshot{};
		};

		struct PreparedAttachmentPublication {
			AnimatorInstanceHandle animator{};
			std::optional<AnimationBoneSocketSnapshot> snapshot;
		};

		struct PreparedAutomaticRootMotionPublication {
			AnimationRootMotionApplicationRequest request{};
		};

		// Compact GPU animators change only scalar playback/mode progress. Carry
		// those values as a typed post-publication commit instead of cloning the
		// animator's large stable binding, parameter, graph, and specialist state.
		struct PreparedCompactRuntimeCommit {
			// PublishedAuthorityCommit retains the complete expected runtime root until
			// this scalar batch is validated and applied. A raw pointer is therefore
			// stable here and avoids 4,000 shared-owner increments/decrements per frame.
			RuntimeRecord* runtimeRecord = nullptr;
			AnimatorInstanceHandle handle{};
			AnimatorPlayback playback{};
			AnimationGeneration poseGeneration = 0U;
			std::uint64_t stateGeneration = 0U;
			std::uint64_t executionModeGeneration = 0U;
			AnimationExecutionModeState executionModeState{};
		};

		// Worker sealing gathers every uncommon post-publication effect into this
		// self-contained packet. The protected boundary receives the packet only
		// after both immutable roots have been accepted, so it never revisits the
		// crowd-sized FrameWorkItem or captured-index collections to rediscover work.
		struct SealedSpecialistSidecarBatch {
			std::vector<PreparedRagdollPublication> ragdoll;
			std::vector<PreparedProceduralPublication> procedural;
			std::vector<PreparedRetargetPublication> retarget;
			std::vector<PreparedAttachmentPublication> attachments;
			std::vector<PreparedAutomaticRootMotionPublication> automaticRootMotion;
			std::vector<AnimationRootMotionApplicationRequest> explicitRootMotion;
			std::vector<PreparedCompactRuntimeCommit> compactRuntime;
			std::vector<std::size_t> evaluationCacheWorkIndices;
			std::vector<AnimatorInstanceHandle> rollbackHistoryAnimators;
		};

		// Compact workers intentionally receive indexed SoA storage instead of a
		// FrameWorkItem. The complete record remains protected-publication authority,
		// while tightly packed identity and packet columns are the only data a
		// visual-only GPU evaluator may read. This keeps crowd packets cache-friendly
		// and prevents dormant complete-evaluator members from entering worker cache.
		struct CompactGpuVisualWorkItems {
			std::vector<std::size_t> frameWorkIndices;
			std::vector<AnimatorInstanceHandle> animators;
			std::vector<std::uint64_t> sourceGenerations;
			std::vector<CompactGpuVisualInput> inputs;

			[[nodiscard]] std::size_t size() const noexcept { return inputs.size(); }
			[[nodiscard]] bool empty() const noexcept { return inputs.empty(); }
			void clear() noexcept {
				frameWorkIndices.clear();
				animators.clear();
				sourceGenerations.clear();
				inputs.clear();
			}
			void reserve(std::size_t capacity) {
				frameWorkIndices.reserve(capacity);
				animators.reserve(capacity);
				sourceGenerations.reserve(capacity);
				inputs.reserve(capacity);
			}
			void append(
				std::size_t frameWorkIndex,
				AnimatorInstanceHandle animator,
				std::uint64_t sourceGeneration,
				CompactGpuVisualInput input) {
				frameWorkIndices.push_back(frameWorkIndex);
				animators.push_back(animator);
				sourceGenerations.push_back(sourceGeneration);
				inputs.push_back(std::move(input));
			}
			[[nodiscard]] bool IsConsistent() const noexcept {
				return frameWorkIndices.size() == inputs.size()
					&& animators.size() == inputs.size()
					&& sourceGenerations.size() == inputs.size();
			}
		};

		// Completion columns mirror compact work by index. Every worker owns a
		// disjoint index stride, and protected publication validates all columns as
		// one atomic batch before it mutates a live runtime record.
		struct CompactGpuVisualCompletions {
			std::vector<std::size_t> compactItemIndices;
			std::vector<AnimatorInstanceHandle> animators;
			std::vector<std::uint64_t> sourceGenerations;
			std::vector<CompactGpuVisualResult> results;
			std::vector<AnimationStatus> statuses;

			[[nodiscard]] std::size_t size() const noexcept { return statuses.size(); }
			void clear() noexcept {
				compactItemIndices.clear();
				animators.clear();
				sourceGenerations.clear();
				results.clear();
				statuses.clear();
			}
			void resize(std::size_t count) {
				compactItemIndices.resize(count);
				animators.resize(count);
				sourceGenerations.resize(count);
				results.resize(count);
				statuses.assign(count, AnimationStatus::NotReady);
			}
			[[nodiscard]] bool IsConsistent() const noexcept {
				return compactItemIndices.size() == statuses.size()
					&& animators.size() == statuses.size()
					&& sourceGenerations.size() == statuses.size()
					&& results.size() == statuses.size();
			}
		};

		// Thousands of eventless direct-clip animators commonly share the exact same
		// immutable program, resident payload, execution policy, and asset generations.
		// Store those facts once per cohort instead of incrementing two shared pointers
		// and appending nine identical scalar columns for every crowd member. The frame
		// lookup below also reunites interleaved instances, which is important when a
		// crowd intentionally mixes clips or playback phases instead of arriving sorted
		// by animation asset.
		// Dynamic playback and authority generations remain per-instance below.
		struct CompactGpuVisualCohort {
			std::shared_ptr<const EvaluationProgram> program;
			std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding> fastBinding;
			AnimationGpuExecutionInputs gpuExecution{};
			AnimationExecutionPolicy requestedPolicy = AnimationExecutionPolicy::Auto;
			AnimatorUpdatePolicy updatePolicy = AnimatorUpdatePolicy::EveryFrame;
			AnimationGeneration programGeneration = 0U;
			AnimationGeneration clipGeneration = 0U;
			AnimationGeneration skeletonGeneration = 0U;
			std::uint32_t jointCount = 0U;

			[[nodiscard]] bool Matches(const RuntimeRecord& record) const noexcept {
				const AnimationGpuExecutionInputs& candidate = record.binding.gpuExecution;
				return program == record.binding.evaluationProgram
					&& fastBinding == record.binding.gpuFastBinding
					&& gpuExecution.rendererAvailable == candidate.rendererAvailable
					&& gpuExecution.animationCapabilityAvailable
						== candidate.animationCapabilityAvailable
					&& gpuExecution.resourcesResident == candidate.resourcesResident
					&& gpuExecution.evaluationHealthy == candidate.evaluationHealthy
					&& gpuExecution.requiredBudget == candidate.requiredBudget
					&& gpuExecution.availableBudget == candidate.availableBudget
					&& requestedPolicy == record.binding.requestedExecutionPolicy
					&& updatePolicy == record.binding.updatePolicy
					&& programGeneration == record.binding.programGeneration
					&& clipGeneration == record.binding.clipGeneration
					&& skeletonGeneration == record.binding.skeletonGeneration
					&& jointCount == record.binding.jointStableIds.size();
			}
		};

		// Compact planning publishes only the mode, reason, and next retained state.
		// The generic decision also owns consumer closure and transition records that
		// protected capture has already proven empty for this visual-only route.
		struct CompactExecutionDecision {
			AnimationEffectiveExecutionMode effectiveMode =
				AnimationEffectiveExecutionMode::CpuDeterministicFallback;
			AnimationExecutionModeReason reason =
				AnimationExecutionModeReason::Uninitialized;
			AnimationExecutionModeState nextState{};
		};

                // Protected capture keeps simple GPU-visual animators out of
                // FrameWorkItem entirely. Parallel columns retain only the
                // scalar authority advances and immutable bindings that compact
                // planning, evaluation, and publication need. inputIndices
                // preserve the deterministic full-frame order when complete and
                // compact records are later validated as one atomic publication
                // batch.
                struct CompactGpuVisualCaptures {
				  // The generic frame input also carries a controller asset handle. The
				  // protected compact gate has already proven that no controller graph is
				  // observed on this route, so retaining only publication identity avoids
				  // copying unused asset metadata through every warmed crowd frame.
				  struct InputIdentity {
					AnimatorInstanceHandle handle{};
					UUID ownerObjectId = EMPTY_UUID;
					AnimationGeneration componentGeneration = 0U;
					AnimationFixedTick tick = 0;
				  };
                  // The 64-bit value is only a lookup accelerator. Equal-hash
                  // candidates still pass CompactGpuVisualCohort::Matches, so a
                  // collision cannot merge animation programs, clips, skeleton
                  // generations, or execution policies incorrectly.
                  std::unordered_multimap<std::uint64_t, std::size_t>
                      cohortLookup;
                  std::vector<std::size_t> inputIndices;
                  std::vector<InputIdentity> inputs;
                  // The captured runtime root below owns these pointers for the
                  // entire frame. Keeping the exact records beside compact
                  // columns avoids a second 4,000-slot shared_ptr lookup during
                  // authority sealing.
                  std::vector<RuntimeRecord *> runtimeRecords;
                  std::vector<AnimatorPlayback> playbacks;
                  std::vector<AnimationExecutionModeState>
                      previousExecutionModes;
                  std::vector<std::size_t> cohortIndices;
                  std::vector<CompactGpuVisualCohort> cohorts;
                  std::vector<AnimationGeneration> candidatePoseGenerations;
				  // Every row in one sealed candidate belongs to the same authority
				  // generation. Retain that batch identity once instead of streaming an
				  // identical 64-bit value through every compact owner.
				  std::uint64_t authorityGeneration = 0U;
                  std::vector<std::uint64_t> stateGenerations;
                  std::vector<std::uint64_t> parameterGenerations;
                  std::vector<std::uint64_t> executionModeGenerations;
                  // Planning output remains in compact columns until it becomes
                  // a compact work packet. No generic evaluator-owned state is
                  // constructed for it.
                  std::vector<AnimatorPlaybackAdvanceResult> advances;
				  std::vector<CompactExecutionDecision>
					  executionDecisions;
                  std::vector<AnimationStatus> evaluationStatuses;
                  std::vector<AnimationPlanRejectionReason> planningFailures;
                  std::vector<AnimationProcessingClass> processingClasses;
                  std::vector<std::uint32_t> stageMasks;
                  // Warm frames need only the per-owner shader-visible lane. Program
                  // and resident-payload ownership already lives once in the cohort;
                  // a complete lowering object is reconstructed only for a cold plan.
                  std::vector<GpuAnimationVisualInput> gpuPresentationVisuals;
                  std::vector<CompactGpuVisualInput> compactInputs;

                  [[nodiscard]] std::size_t size() const noexcept {
                    return inputs.size();
                  }
                  [[nodiscard]] bool empty() const noexcept {
                    return inputs.empty();
                  }
                  [[nodiscard]] static std::uint64_t
                  CohortFingerprint(const RuntimeRecord &record) noexcept {
                    std::uint64_t hash = 1469598103934665603ULL;
                    const auto mix = [&hash](std::uint64_t value) noexcept {
                      hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) +
                              (hash >> 2U);
                    };
                    const AnimationGpuExecutionInputs &gpu =
                        record.binding.gpuExecution;
                    mix(static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(
                            record.binding.evaluationProgram.get())));
                    mix(static_cast<std::uint64_t>(
                        reinterpret_cast<std::uintptr_t>(
                            record.binding.gpuFastBinding.get())));
                    mix(gpu.rendererAvailable ? 1U : 0U);
                    mix(gpu.animationCapabilityAvailable ? 1U : 0U);
                    mix(gpu.resourcesResident ? 1U : 0U);
                    mix(gpu.evaluationHealthy ? 1U : 0U);
                    mix(std::bit_cast<std::uint64_t>(gpu.requiredBudget));
                    mix(std::bit_cast<std::uint64_t>(gpu.availableBudget));
                    mix(static_cast<std::uint64_t>(
                        record.binding.requestedExecutionPolicy));
                    mix(static_cast<std::uint64_t>(
                        record.binding.updatePolicy));
                    mix(record.binding.programGeneration);
                    mix(record.binding.clipGeneration);
                    mix(record.binding.skeletonGeneration);
                    mix(record.binding.jointStableIds.size());
                    return hash;
                  }
                  void clear() noexcept {
                    cohortLookup.clear();
                    inputIndices.clear();
                    inputs.clear();
                    runtimeRecords.clear();
                    playbacks.clear();
                    previousExecutionModes.clear();
                    cohortIndices.clear();
                    cohorts.clear();
                    candidatePoseGenerations.clear();
					authorityGeneration = 0U;
                    stateGenerations.clear();
                    parameterGenerations.clear();
                    executionModeGenerations.clear();
                    advances.clear();
                    executionDecisions.clear();
                    evaluationStatuses.clear();
                    planningFailures.clear();
                    processingClasses.clear();
                    stageMasks.clear();
                    gpuPresentationVisuals.clear();
                    compactInputs.clear();
                  }
                  void reserve(std::size_t capacity) {
                    cohortLookup.reserve(capacity);
                    inputIndices.reserve(capacity);
                    inputs.reserve(capacity);
                    runtimeRecords.reserve(capacity);
                    playbacks.reserve(capacity);
                    previousExecutionModes.reserve(capacity);
                    cohortIndices.reserve(capacity);
                    candidatePoseGenerations.reserve(capacity);
                    stateGenerations.reserve(capacity);
                    parameterGenerations.reserve(capacity);
                    executionModeGenerations.reserve(capacity);
                    advances.reserve(capacity);
                    executionDecisions.reserve(capacity);
                    evaluationStatuses.reserve(capacity);
                    planningFailures.reserve(capacity);
                    processingClasses.reserve(capacity);
                    stageMasks.reserve(capacity);
                    gpuPresentationVisuals.reserve(capacity);
                    compactInputs.reserve(capacity);
                  }
                  void swap(CompactGpuVisualCaptures &other) noexcept {
                    // Keep both frame buffers allocated. The candidate for
                    // frame N+1 receives frame N-1's storage, so a stable crowd
                    // performs no column reallocations.
                    using std::swap;
                    swap(cohortLookup, other.cohortLookup);
                    swap(inputIndices, other.inputIndices);
                    swap(inputs, other.inputs);
                    swap(runtimeRecords, other.runtimeRecords);
                    swap(playbacks, other.playbacks);
                    swap(previousExecutionModes, other.previousExecutionModes);
                    swap(cohortIndices, other.cohortIndices);
                    swap(cohorts, other.cohorts);
                    swap(candidatePoseGenerations,
                         other.candidatePoseGenerations);
					swap(authorityGeneration, other.authorityGeneration);
                    swap(stateGenerations, other.stateGenerations);
                    swap(parameterGenerations, other.parameterGenerations);
                    swap(executionModeGenerations,
                         other.executionModeGenerations);
                    swap(advances, other.advances);
                    swap(executionDecisions, other.executionDecisions);
                    swap(evaluationStatuses, other.evaluationStatuses);
                    swap(planningFailures, other.planningFailures);
                    swap(processingClasses, other.processingClasses);
                    swap(stageMasks, other.stageMasks);
                    swap(gpuPresentationVisuals, other.gpuPresentationVisuals);
                    swap(compactInputs, other.compactInputs);
                  }
                  void append(std::size_t inputIndex,
                              const AnimationFrameInput &input,
							   RuntimeRecord &record,
							   const CompactRuntimeState* currentState = nullptr) {
                    inputIndices.push_back(inputIndex);
					inputs.push_back({
						input.handle,
						input.descriptor.ownerObjectId,
						input.descriptor.componentGeneration,
						input.tick});
					playbacks.push_back(currentState ? currentState->playback : record.playback);
                    runtimeRecords.push_back(&record);
					previousExecutionModes.push_back(currentState
						? currentState->executionModeState : record.executionModeState);
                    std::size_t cohortIndex = cohorts.size();
                    if (!cohorts.empty() && cohorts.back().Matches(record)) {
                      // Sorted homogeneous crowds retain the former single
                      // comparison fast path.
                      cohortIndex = cohorts.size() - 1U;
                    } else {
                      const std::uint64_t fingerprint =
                          CohortFingerprint(record);
                      const auto [first, last] =
                          cohortLookup.equal_range(fingerprint);
                      for (auto candidate = first; candidate != last;
                           ++candidate) {
                        if (candidate->second < cohorts.size() &&
                            cohorts[candidate->second].Matches(record)) {
                          cohortIndex = candidate->second;
                          break;
                        }
                      }
                      if (cohortIndex == cohorts.size()) {
                        CompactGpuVisualCohort cohort;
                        cohort.program = record.binding.evaluationProgram;
                        cohort.fastBinding = record.binding.gpuFastBinding;
                        cohort.gpuExecution = record.binding.gpuExecution;
                        cohort.requestedPolicy =
                            record.binding.requestedExecutionPolicy;
                        cohort.updatePolicy = record.binding.updatePolicy;
                        cohort.programGeneration =
                            record.binding.programGeneration;
                        cohort.clipGeneration = record.binding.clipGeneration;
                        cohort.skeletonGeneration =
                            record.binding.skeletonGeneration;
                        cohort.jointCount = static_cast<std::uint32_t>(
                            record.binding.jointStableIds.size());
                        cohorts.push_back(std::move(cohort));
                        cohortIndex = cohorts.size() - 1U;
                        cohortLookup.emplace(fingerprint, cohortIndex);
                      }
                    }
                    cohortIndices.push_back(cohortIndex);
					const AnimationGeneration currentPoseGeneration = currentState
						? currentState->poseGeneration : record.poseGeneration;
					candidatePoseGenerations.push_back(currentPoseGeneration +
												   1U);
					stateGenerations.push_back(currentState
						? currentState->stateGeneration : record.stateGeneration);
                    parameterGenerations.push_back(record.parameterGeneration);
					executionModeGenerations.push_back(currentState
						? currentState->executionModeGeneration
						: record.executionModeGeneration);
                    advances.emplace_back();
                    executionDecisions.emplace_back();
                    evaluationStatuses.push_back(AnimationStatus::NotReady);
                    planningFailures.push_back(
                        AnimationPlanRejectionReason::None);
                    processingClasses.push_back(
                        AnimationProcessingClass::Complete);
                    stageMasks.push_back(0U);
                    gpuPresentationVisuals.emplace_back();
                    compactInputs.emplace_back();
                  }

                  [[nodiscard]] const CompactGpuVisualCohort &
                  cohort(std::size_t index) const noexcept {
                    assert(index < cohortIndices.size() &&
                           cohortIndices[index] < cohorts.size());
                    return cohorts[cohortIndices[index]];
                  }
                };

                std::unique_ptr<AnimatorInstanceStore> instances = std::make_unique<AnimatorInstanceStore>();
		std::unique_ptr<AnimationCommandQueue> commands = std::make_unique<AnimationCommandQueue>();
		std::unique_ptr<ProceduralCommandStore> proceduralCommands =
			std::make_unique<ProceduralCommandStore>();
		std::unique_ptr<AdvancedAnimationCommandStore> advancedCommands =
			std::make_unique<AdvancedAnimationCommandStore>();
		AnimatorComponentBindingCoordinator componentBindings;
		// The coordinator publishes only the latest completed non-empty episode.
		// Atomics keep diagnostic readers independent from the coordinator thread
		// and avoid extending the runtime-ownership critical section.
		std::atomic<std::uint64_t> bindingEpisodeCount{ 0U };
		std::atomic<std::uint64_t> bindingReconciliationNanoseconds{ 0U };
		std::atomic<std::size_t> bindingReconciledCount{ 0U };
		std::atomic<std::size_t> bindingFailureCount{ 0U };
		// This is coordinator-thread domain state, not per-frame worker input.
		// Runtime applications keep the default enabled; the editor explicitly
		// switches it at protected Play Mode entry and exit boundaries.
		bool authoredComponentPlaybackEnabled = true;
		bool authoredComponentPlaybackAdvancing = true;
		bool headlessExecutionRequired = false;
		std::atomic<std::shared_ptr<RuntimeRecordRoot>> runtimeRecords{
			std::make_shared<RuntimeRecordRoot>()};
		std::atomic<std::shared_ptr<const CompactRuntimeStateRoot>> compactRuntimeStates{
			std::make_shared<CompactRuntimeStateRoot>()};
		// A cooked clip generation is immutable. Cache its serialized deterministic
		// fingerprint once so a prefab crowd does not serialize and hash the same
		// complete clip independently for every runtime record during initial bind.
		std::map<AnimationAssetHandle, std::uint64_t> clipContentFingerprints;
		std::vector<AnimationFrameInput> capturedFrameInputs;
			std::vector<AnimationCommand> capturedCommands;
			// A single resource-publication epoch protects all controller-qualified
			// commands at the scalar authority exchange. Capture reads it before the
			// per-command validation so even a reload during Capture changes the seal.
			std::uint64_t qualifiedControllerResourceRevision = 0U;
                // Each worker writes only entries targeting its disjoint
                // animator range. The protected publisher later turns these
                // staged values into observable receipts together with the pose
                // generation they affected.
                std::vector<AnimationStatus> capturedCommandStatuses;
		// Ordered workload phases retain the first failed command-frame boundary
		// until protected settlement logs it. Stable command-free frames do no IO.
		const char* commandFrameFailurePhase = nullptr;
                // Procedural requests use their own fixed ABI record because
                // the original gameplay command stride is pinned. They are
                // still drained and completed at the same protected frame
                // boundary as ordinary animation commands.
                std::vector<NativeAnimationProceduralGoalCommand>
                    capturedProceduralCommands;
                std::vector<AnimationStatus> capturedProceduralCommandStatuses;
                // Capture retained procedural state under one store lock, then
                // merge these ordered flat ranges with the ordered animator
                // slots below.
                std::vector<ActiveProceduralGoalRange>
                    capturedProceduralGoalRanges;
                std::vector<NativeAnimationProceduralGoalCommand>
                    capturedProceduralGoals;
                std::vector<NativeAnimationRetargetRequest>
                    capturedRetargetRequests;
                std::vector<NativeAnimationMotionMatchRequest>
                    capturedMotionMatchRequests;
                std::vector<NativeAnimationMotionMatchQueryRequestV2>
                    capturedMotionMatchQueriesV2;
                std::vector<NativeAnimationRagdollRequest>
                    capturedRagdollRequests;
                std::vector<NativeAnimationDeterminismRequest>
                    capturedDeterminismRequests;
                std::vector<DeterminismRequestCompletion>
                    determinismRequestCompletions;
                // Managed/native producers append copied requests from any
                // scripting worker. Only PublishAnimationAuthority drains this
                // bounded queue and touches live scene/physics state,
                // preserving the worker-safe scripting declaration.
                std::vector<AnimationRootMotionApplicationRequest>
                    pendingRootMotionApplications;
                std::vector<FrameWorkItem> frameWork;
                // Capture builds transactionally into this second owned vector.
                // Swapping on success preserves the previous allocation for the
                // next frame, avoiding a fresh FrameWorkItem allocation while a
                // failed capture stays invisible.
                std::vector<FrameWorkItem> candidateFrameWork;
                // Every captured input resolves to exactly one compact or
                // complete slot. This explicit mapping preserves full-frame
                // deterministic order while compact crowds avoid constructing
                // heavyweight FrameWorkItem records.
                struct CapturedInputWorkIndex {
                  std::size_t storageIndex = 0U;
                  bool compact = false;
                };
				std::vector<CapturedInputWorkIndex> capturedInputWorkIndices;
				std::vector<CapturedInputWorkIndex> candidateInputWorkIndices;
				// Protected capture uses workers only to write disjoint admission bytes.
				// The coordinator then assembles deterministic SoA columns in source order.
				std::vector<std::uint8_t> compactCaptureAdmissionProof;
                CompactGpuVisualCaptures compactGpuVisualCaptures;
                CompactGpuVisualCaptures candidateCompactGpuVisualCaptures;
				std::shared_ptr<RuntimeRecordRoot> compactRuntimeCaptureRoot;
				std::shared_ptr<RuntimeRecordRoot>
					candidateCompactRuntimeCaptureRoot;
				std::shared_ptr<const CompactRuntimeStateRoot>
					compactRuntimeStateCaptureRoot;
				std::shared_ptr<const CompactRuntimeStateRoot>
					candidateCompactRuntimeStateCaptureRoot;
                // Presentation consumes immutable shared frames asynchronously.
                // Three retained slots let frame N reuse N-2 storage once all
                // readers release it, while a slow reader safely forces a
                // temporary fallback allocation instead of mutation.
                std::array<std::shared_ptr<GpuAnimationPresentationFrame>, 3U>
                    gpuPresentationFramePool{};
                std::size_t gpuPresentationFramePoolCursor = 0U;
		// Published authority roots are immutable, but retired roots eventually
		// return to sole ownership here. Four slots cover the normal renderer and
		// presentation pipeline depth while preserving vector capacity for stable
		// crowds; a delayed reader simply makes the frame allocate a fallback root.
		std::array<std::vector<std::shared_ptr<AnimationAuthorityPage>>, 4U>
			authorityPagePool{};
		std::size_t authorityPagePoolCursor = 0U;
		std::array<std::shared_ptr<AnimationFrameSidecars>, 4U>
			authoritySidecarPool{};
		std::size_t authoritySidecarPoolCursor = 0U;
                AnimationWorkScheduler workScheduler{
                    MakeAnimationWorkSchedulerConfiguration()};
                AnimationWorkCancellationSource workCancellationSource;
                AnimationWorkPlan workPlan;
                AnimationPlanRejectionReason workPlanRejectionReason =
                    AnimationPlanRejectionReason::None;
                std::size_t workPlanRejectionItemIndex = std::numeric_limits<std::size_t>::max();
		std::unique_ptr<AnimationWorkRangeLedger> workRangeLedger;
		std::vector<AnimationWorkItem> capturedWorkItems;
		std::vector<AnimationWorkCompletion> workCompletions;
		CompactGpuVisualWorkItems compactGpuVisualWorkItems;
		CompactGpuVisualCompletions compactGpuVisualCompletions;
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
		// Consumer declarations are immutable for one captured frame. Remembering the
		// empty common case avoids thousands of contended atomic shared_ptr loads while still
		// making a declaration published mid-frame visible at the next boundary.
		bool frameHasConsumerDeclarations = false;
		AnimationConsumerRegistry::ReadView frameConsumerReadView;
		AnimationFixedTick frameTick = 0;
		std::atomic_size_t scheduledAnimatorCount = 0U;
		std::atomic_size_t plannedAnimatorCount = 0U;
		std::atomic_size_t evaluatedAnimatorCount = 0U;
		std::atomic_size_t publishedAnimatorCount = 0U;
		std::atomic_size_t cacheReusedAnimatorCount = 0U;
		// This counter tracks same-frame visual pose clones separately from the
		// cross-frame evaluation cache. Both avoid clip evaluation, but only the
		// latter retains the exact prior publication pointer.
		std::atomic_size_t sharedPoseAnimatorCount = 0U;
		std::atomic_size_t fallbackAnimatorCount = 0U;
		std::atomic_size_t compactGpuVisualCompletedCount = 0U;
		// Timings accumulate owned worker elapsed time in nanoseconds. They measure
		// real work even when sibling compact and complete branches overlap, instead
		// of attributing wall-clock queue delay to one processing class.
		std::atomic_uint64_t compactPlanningNanoseconds = 0U;
		std::atomic_uint64_t compactPreCopyCaptureCount = 0U;
		std::atomic_uint64_t completePlanningNanoseconds = 0U;
		std::atomic_uint64_t compactEvaluationNanoseconds = 0U;
		std::atomic_uint64_t completeEvaluationNanoseconds = 0U;
		std::atomic_bool frameCancellationRequested = false;
		bool deterministicPublicationValidated = false;
		// BuildPlan validates the immutable work plan once. Worker partitions and
		// protected publication consume that proof instead of rescanning all items.
		bool workPlanValidated = false;
		std::size_t availableWorkerCount = 0U;
		mutable std::mutex workPlanFinalizeMutex;
		std::condition_variable workPlanFinalizeCondition;
		WorkPlanFinalizeState workPlanFinalizeState = WorkPlanFinalizeState::Pending;
		std::atomic_uint64_t authorityPublicationGeneration = 0U;
		// This is the only render/gameplay authority surface.  Runtime records keep
		// evaluator state, but consumers receive this sealed immutable root.
		AnimationAuthorityPublisher authorityPublisher;
		SealedAnimationFrameResult sealedAuthorityCandidate{};
		bool sealedAuthorityCandidateReady = false;
		// Sealing owns all fallible authority construction.  The protected publisher
		// transfers this tiny receipt only after the immutable root has been accepted;
		// the follow-up finalizer can therefore never mutate runtime records for a
		// rejected, cancelled, or stale candidate.
		struct PublishedAuthorityCommitToken {
			std::size_t animatorCount = 0U;
			std::uint64_t frameEpoch = 0U;
			std::uint64_t authorityGeneration = 0U;
			std::uint64_t sessionGeneration = 0U;
			std::uint64_t sceneLifecycleGeneration = 0U;
			// Worker sealing prepares a complete copy-on-write evaluator root.  The
			// protected boundary compares the exact predecessor pointer before it swaps
			// this root, so a concurrent lifecycle change rejects the whole candidate
			// without exposing a partly updated crowd.
			std::shared_ptr<RuntimeRecordRoot> expectedRuntimeRoot;
			std::shared_ptr<RuntimeRecordRoot> nextRuntimeRoot;
			// Compact scalar state is exchanged beside the broad runtime root. The
			// predecessor identity prevents a late worker from replacing state authored
			// by a lifecycle or command mutation.
			std::shared_ptr<const CompactRuntimeStateRoot> expectedCompactRuntimeRoot;
			std::shared_ptr<const CompactRuntimeStateRoot> nextCompactRuntimeRoot;
			// This is sealed by the worker alongside nextRuntimeRoot. It is opaque to
			// ordinary consumers and contains only sparse copied operations.
			std::shared_ptr<SealedSpecialistSidecarBatch> specialistSidecars;
			bool ready = false;
		};
		PublishedAuthorityCommitToken publishedAuthorityCommit{};
		std::optional<AnimationDiagnosticsSnapshot> preparedDiagnostics;
		std::optional<AnimationDetailedDiagnosticsSnapshot> preparedDetailedDiagnostics;
		std::shared_ptr<const GpuAnimationPresentationFrame> preparedPresentationFrame;
		// A frame made only of exact cache hits or compatible hold-last selections has
		// no new authority data. Keeping this explicit lets the protected boundary
		// retain the previous root rather than relabeling old sidecars as a new frame.
		bool retainsPublishedAuthorityRoot = false;
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
		// Locomotion publishes complete copied mappings without entering this mutex;
		// Animation workers pin one immutable root for the entire evaluation phase.
		std::atomic<std::shared_ptr<const AnimationRootControllerBindingSnapshot>>
			rootControllerBindings;
		// Accepted ragdoll Activate is visible at submission, before any worker
		// evaluates root motion or the next locomotion movement publication.
		std::atomic<std::shared_ptr<const AnimationRagdollExclusionSnapshot>>
			ragdollExclusions;
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

		[[nodiscard]] FrameWorkItem* CompleteWorkForInputIndex(std::size_t inputIndex) noexcept
		{
                  if (inputIndex >= capturedInputWorkIndices.size())
                    return nullptr;
                  const CapturedInputWorkIndex &captured =
                      capturedInputWorkIndices[inputIndex];
                  return !captured.compact &&
                                 captured.storageIndex < frameWork.size()
                             ? &frameWork[captured.storageIndex]
                             : nullptr;
                }

                [[nodiscard]] const FrameWorkItem *CompleteWorkForInputIndex(
                    std::size_t inputIndex) const noexcept {
                  return const_cast<Impl *>(this)->CompleteWorkForInputIndex(
                      inputIndex);
                }

                void ResetFrameIntegrationStateLocked() noexcept {
                  workCancellationSource.RequestCancellation();
                  (void)evaluationCancellationSource.Cancel(
                      AnimationCancellationReason::OwnerReleased);
                  workCancellationSource = AnimationWorkCancellationSource{};
                  evaluationCancellationSource = AnimationCancellationSource{};
                  evaluationCancellation = {};
                  workPlan = {};
                  workPlanRejectionReason = AnimationPlanRejectionReason::None;
                  workPlanRejectionItemIndex =
                      std::numeric_limits<std::size_t>::max();
                  workRangeLedger.reset();
                  // Move the last complete frame into candidate ownership
                  // before clearing the active view. CaptureFrameLocked can now
                  // reuse each animator's validated clip view and vector
                  // storage, while a failed capture still leaves frameWork
                  // empty so later phases cannot evaluate stale work.
                  candidateFrameWork.swap(frameWork);
                  frameWork.clear();
                  candidateCompactGpuVisualCaptures =
                      std::move(compactGpuVisualCaptures);
                  compactGpuVisualCaptures.clear();
				  candidateCompactRuntimeCaptureRoot =
					  std::move(compactRuntimeCaptureRoot);
				  candidateCompactRuntimeStateCaptureRoot =
					  std::move(compactRuntimeStateCaptureRoot);
				  candidateInputWorkIndices.swap(capturedInputWorkIndices);
				  capturedInputWorkIndices.clear();
				  compactCaptureAdmissionProof.clear();
                  capturedRetargetRequests.clear();
                  capturedMotionMatchRequests.clear();
                  capturedMotionMatchQueriesV2.clear();
                  capturedRagdollRequests.clear();
                  capturedDeterminismRequests.clear();
                  determinismRequestCompletions.clear();
                  capturedWorkItems.clear();
                  workCompletions.clear();
                  compactGpuVisualWorkItems.clear();
                  compactGpuVisualCompletions.clear();
                  deterministicPublicationOrder.clear();
                  frameScratch.clear();
                  frameScratchLease.Release();
                  framePoseReservation.reset();
                  frameLifecycleGenerations = {};
                  frameSceneGeneration = 0U;
                  frameHasConsumerDeclarations = false;
                  frameConsumerReadView = {};
                  deterministicPublicationValidated = false;
                  // A lifecycle reset may run between root exchange and its
                  // dependent finalizer.  Dropping the scalar receipt prevents
                  // that stale frame from applying internal runtime state after
                  // its authority root was retired.
                  publishedAuthorityCommit = {};
                  preparedDiagnostics.reset();
                  preparedDetailedDiagnostics.reset();
                  preparedPresentationFrame.reset();
                  workPlanValidated = false;
                  retainsPublishedAuthorityRoot = false;
                  availableWorkerCount = 0U;
                  frameCancellationRequested.store(false,
                                                   std::memory_order_release);
                  cacheReusedAnimatorCount.store(0U, std::memory_order_release);
                  sharedPoseAnimatorCount.store(0U, std::memory_order_release);
                  fallbackAnimatorCount.store(0U, std::memory_order_release);
                  compactGpuVisualCompletedCount.store(
                      0U, std::memory_order_release);
                  compactPlanningNanoseconds.store(0U,
                                                   std::memory_order_release);
                  compactPreCopyCaptureCount.store(0U,
                                                   std::memory_order_release);
                  completePlanningNanoseconds.store(0U,
                                                    std::memory_order_release);
                  compactEvaluationNanoseconds.store(0U,
                                                     std::memory_order_release);
                  completeEvaluationNanoseconds.store(
                      0U, std::memory_order_release);
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

                void RetireLifecyclePublicationStateLocked() noexcept {
			// Resolve copied commands before retirement discards their commit token.
			// Accepted historical commits survive; uncommitted work is cancelled.
			PublishCommittedOrdinaryCommandsLocked();
			SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "LifecycleRetirement");
                  // A lifecycle boundary is different from an ordinary failed
                  // frame: the old immutable result names a world that no
                  // longer exists.  Clear the public root before releasing
                  // worker-owned candidate memory so a fresh reader can never
                  // acquire an otherwise well-formed stale authority frame.
                  const AnimationLifecycleGenerationSnapshot lifecycle =
                      lifecycleLatch ? lifecycleLatch->Capture()
                                     : AnimationLifecycleGenerationSnapshot{};
                  authorityPublisher.Clear(
                      runtimeSessionEpoch.load(std::memory_order_acquire),
                      lifecycle.Generation(AnimationLifecycleBoundary::Scene));

                  // ResetFrameIntegrationStateLocked drops the sealed commit
                  // token and its sidecars.  It swaps reusable frame buffers,
                  // so clear both sides here: lifecycle transitions release
                  // old-world references instead of retaining them as the next
                  // frame's allocation cache.
                  ResetFrameIntegrationStateLocked();
                  sealedAuthorityCandidate = {};
                  sealedAuthorityCandidateReady = false;
                  capturedFrameInputs.clear();
                  capturedCommands.clear();
                  qualifiedControllerResourceRevision = 0U;
                  capturedCommandStatuses.clear();
                  capturedProceduralCommands.clear();
                  capturedProceduralCommandStatuses.clear();
                  capturedProceduralGoalRanges.clear();
                  capturedProceduralGoals.clear();
                  capturedRetargetRequests.clear();
                  capturedMotionMatchRequests.clear();
                  capturedMotionMatchQueriesV2.clear();
                  capturedRagdollRequests.clear();
                  capturedDeterminismRequests.clear();
                  pendingRootMotionApplications.clear();
                  frameWork.clear();
                  candidateFrameWork.clear();
                  compactGpuVisualCaptures.clear();
                  candidateCompactGpuVisualCaptures.clear();
				  compactRuntimeCaptureRoot.reset();
				  candidateCompactRuntimeCaptureRoot.reset();
				  compactRuntimeStateCaptureRoot.reset();
				  candidateCompactRuntimeStateCaptureRoot.reset();
				  capturedInputWorkIndices.clear();
				  candidateInputWorkIndices.clear();
				  compactCaptureAdmissionProof.clear();
                  gpuPresentationFramePool = {};
                  gpuPresentationFramePoolCursor = 0U;
					authorityPagePool = {};
					authorityPagePoolCursor = 0U;
					authoritySidecarPool = {};
					authoritySidecarPoolCursor = 0U;
                  publishedAnimatorCount.store(0U, std::memory_order_release);
                  // The scalar generation is observable by deferred producers.
                  // Advancing it makes every receipt captured before this
                  // lifecycle boundary unambiguously older even though the
                  // replacement authority root is intentionally empty.
                  authorityPublicationGeneration.fetch_add(
                      1U, std::memory_order_acq_rel);
                  frameDeclarationEpoch.fetch_add(1U,
                                                  std::memory_order_acq_rel);
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

		[[nodiscard]] static const CompactRuntimeState* FindCompactRuntimeState(
			const std::shared_ptr<const CompactRuntimeStateRoot>& root,
			AnimatorInstanceHandle handle) noexcept
		{
			if(!root || !handle.IsValid() || handle.slotIndex >= root->size()) return nullptr;
			const CompactRuntimeState& state = root->at(handle.slotIndex);
			return state.IsValidFor(handle) ? &state : nullptr;
		}

		[[nodiscard]] bool MaterializeCompactRuntimeStateLocked(
			AnimatorInstanceHandle handle,
			RuntimeRecord& record) noexcept
		{
			std::shared_ptr<const CompactRuntimeStateRoot> expected =
				compactRuntimeStates.load(std::memory_order_acquire);
			const CompactRuntimeState* state = FindCompactRuntimeState(expected, handle);
			if(state == nullptr) return true;
			try {
				auto next = std::make_shared<CompactRuntimeStateRoot>(*expected);
				next->at(handle.slotIndex) = {};
				std::shared_ptr<const CompactRuntimeStateRoot> published = std::move(next);
				if(!compactRuntimeStates.compare_exchange_strong(
					expected, published, std::memory_order_acq_rel,
					std::memory_order_acquire)) return false;
				record.playback = state->playback;
				record.poseGeneration = state->poseGeneration;
				record.stateGeneration = state->stateGeneration;
				record.executionModeGeneration = state->executionModeGeneration;
				record.executionModeState = state->executionModeState;
				return true;
			} catch(const std::bad_alloc&) {
				// Authored mutation remains fail-closed. Leaving the compact root intact
				// is safer than exposing only part of its scalar state.
				return false;
			}
		}

		RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) noexcept
		{
			const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
			if(!handle.IsValid() || root == nullptr || handle.slotIndex >= root->size()) return nullptr;
			RuntimeRecord* record = root->at(handle.slotIndex).get();
			if(record == nullptr || record->handle != handle) return nullptr;
			if(!MaterializeCompactRuntimeStateLocked(handle, *record)) return nullptr;
			return record;
		}

		const RuntimeRecord* FindRuntimeRecordLocked(AnimatorInstanceHandle handle) const noexcept
		{
			const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
			if(!handle.IsValid() || root == nullptr || handle.slotIndex >= root->size()) return nullptr;
			const RuntimeRecord* record = root->at(handle.slotIndex).get();
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
			const AnimationAuthorityRow& published) const noexcept
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
			const AnimationAuthorityRow* published,
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
					// From this protected boundary onward, retain every published tick for
					// the requesting animator. Restore and replay can therefore address a
					// continuous ring without charging unrelated crowd members.
					record->rollbackHistoryActive = true;
					const std::shared_ptr<const AnimationAuthorityFrame> authority =
						authorityPublisher.Load();
					const AnimationAuthorityRow* published = authority
						? authority->Find(record->handle) : nullptr;
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

		void CaptureRollbackHistoryLocked(
			std::span<const AnimatorInstanceHandle> rollbackHistoryAnimators) noexcept
		{
			const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
			if(root == nullptr) return;
			for(const AnimatorInstanceHandle handle : rollbackHistoryAnimators) {
				if(!handle.IsValid() || handle.slotIndex >= root->size()) continue;
				const std::shared_ptr<RuntimeRecord>& owned = root->at(handle.slotIndex);
				if(!owned || owned->handle != handle || !owned->rollbackHistoryActive
					|| !instances->IsAlive(handle)) continue;
				const std::shared_ptr<const AnimationAuthorityFrame> authority =
					authorityPublisher.Load();
				const AnimationAuthorityRow* published = authority
					? authority->Find(owned->handle) : nullptr;
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

		// Child creation/submission/execution can throw. Scoped settlement joins
		// admitted readers first; the workload boundary reports the original error.
		bool CaptureFrameLocked(AnimationFixedTick captureTick,
								std::uint64_t candidateAuthorityGeneration) {
			if(instances == nullptr || commands == nullptr || proceduralCommands == nullptr ||
			   advancedCommands == nullptr || lifecycleLatch == nullptr ||
			   candidateAuthorityGeneration == 0U)
				return false;
			// A thrown/cancelled graph may skip its finalizer. Complete its drained
			// commands before the next capture replaces the copied command vector.
			PublishCommittedOrdinaryCommandsLocked();
			SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "NextCapture");
			commandFrameFailurePhase = nullptr;
			ResetFrameIntegrationStateLocked();
			if(evaluationCancellationSource.Initialize(
				   AnimationCancellationOperation::CpuEvaluation, *lifecycleLatch) !=
			   AnimationStatus::Ok) {
				frameCancellationRequested.store(true, std::memory_order_release);
				return false;
			}
			evaluationCancellation = evaluationCancellationSource.GetToken();
			frameLifecycleGenerations = lifecycleLatch->Capture();
			frameSceneGeneration = ObjectManager::instance().GetSceneGeneration();
			frameConsumerReadView =
				AnimationConsumerRegistry::Instance().CaptureReadView();
			const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);
			const auto rootEnabledFor = [&rootBindings](AnimatorInstanceHandle handle) noexcept {
				if(!rootBindings) return false;
				const auto found = std::lower_bound(rootBindings->active.begin(),
					rootBindings->active.end(), handle,
					[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle key) {
						return row.animator < key;
					});
				return found != rootBindings->active.end() && found->animator == handle
					&& found->rootMotionEnabled;
			};
			frameHasConsumerDeclarations =
				frameConsumerReadView.HasDeclarations();
			const std::uint64_t frameSpatialRevision =
				ObjectManager::instance().GetSpatialRevision();
			try {
				{
					// This scope separates external queue/store snapshots
					// from per-animator state capture, making profiler
					// sessions actionable without per-item noise.
					TERRAENGINE_PROFILE_SCOPE("Animation: Capture Queues And Inputs",
											  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					instances->CaptureFrameInputs(captureTick, capturedFrameInputs);
					commands->DrainThrough(captureTick, capturedCommands, *instances);
					capturedCommandStatuses.assign(capturedCommands.size(),
											   AnimationStatus::NotReady);
					const std::uint64_t controllerRevisionBefore = ResourceManager::Instance()
						.GetAnimationBindingResourceRevision();
					qualifiedControllerResourceRevision = 0U;
					// Qualified commands are tied to the exact resource publication
					// captured by their producer. Reject a hot-reloaded or tombstoned
					// controller before any worker can mutate copied parameters.
					for(std::size_t commandIndex = 0U; commandIndex < capturedCommands.size(); ++commandIndex) {
						const AnimationAssetHandle expected = capturedCommands[commandIndex].expectedController;
						if(!expected.IsValid()) continue;
						const auto current = ResourceManager::Instance().TryGetAnimationAssetRuntimeGeneration(
							expected.assetId.value, AssetType::AnimationController);
						if(!current || *current != expected.generation
							|| !ResourceManager::Instance().IsAnimationAssetPublished(
								expected.assetId.value, AssetType::AnimationController))
							capturedCommandStatuses[commandIndex] = AnimationStatus::StaleHandle;
						else
							qualifiedControllerResourceRevision = controllerRevisionBefore;
					}
					proceduralCommands->DrainThrough(captureTick, capturedProceduralCommands,
													 *instances);
					if(proceduralCommands->CaptureActiveGoals(capturedProceduralGoalRanges,
															  capturedProceduralGoals) !=
					   AnimationStatus::Ok) {
						return false;
					}
					capturedProceduralCommandStatuses.assign(capturedProceduralCommands.size(),
															 AnimationStatus::NotReady);
					advancedCommands->DrainThrough(
						captureTick, capturedRetargetRequests, capturedMotionMatchRequests,
						capturedMotionMatchQueriesV2, capturedRagdollRequests,
						capturedDeterminismRequests);
					// Exact history restores happen only while runtime
					// ownership is protected and before worker-owned frame
					// values are copied. No worker mutates live playback,
					// controller, parameter, or event state.
					PrepareDeterminismRequestsLocked();
					for(std::size_t index = 0U; index < capturedProceduralCommands.size();
						++index) {
						const NativeAnimationProceduralGoalCommand &command =
							capturedProceduralCommands[index];
						const AnimatorInstanceHandle handle{command.animator.slotIndex,
															command.animator.generation};
						const RuntimeRecord *record = FindRuntimeRecordLocked(handle);
						if(record == nullptr || !instances->IsAlive(handle)) {
							capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
							continue;
						}
						if(command.command == NativeAnimationProceduralCommandKind::ClearGoal) {
							capturedProceduralCommandStatuses[index] = AnimationStatus::Ok;
							continue;
						}
						const std::shared_ptr<const AnimationAuthorityFrame> authority =
							authorityPublisher.Load();
						const AnimationAuthorityRow* published = authority
							? authority->Find(record->handle) : nullptr;
						if(!published || !published->snapshot ||
						   published->snapshot->publicationGeneration !=
							   command.expectedPoseGeneration ||
						   published->authorityPublicationGeneration !=
							   command.expectedPublicationGeneration) {
							capturedProceduralCommandStatuses[index] = AnimationStatus::StaleHandle;
                                                        continue;
                                                }
                                                const bool jointExists =
                                                    std::find(
                                                        record->binding
                                                            .jointStableIds
                                                            .begin(),
                                                        record->binding
                                                            .jointStableIds
                                                            .end(),
                                                        command
                                                            .targetJointStableId) !=
                                                    record->binding
                                                        .jointStableIds.end();
                                                capturedProceduralCommandStatuses
                                                    [index] =
                                                        jointExists
                                                            ? AnimationStatus::
                                                                  Ok
                                                            : AnimationStatus::
                                                                  InvalidArgument;
                                        }
                                }
                                {
                                  // Keep two alternating item arrays alive.
                                  // Copy assignment can then reuse binding,
                                  // parameter, controller, and event-vector
                                  // capacity instead of allocating a fresh
                                  // nested object graph for every animator
                                  // every frame.
                                  TERRAENGINE_PROFILE_SCOPE(
                                      "Animation: Copy Runtime State",
                                      TerraEngine::Profiling::ProfilerModuleId::
                                          CpuEngine);
                                  const std::shared_ptr<
                                      const TerraEngine::Renderer::
                                          AnimatedObjectVisibilitySnapshot>
                                      visibility = TerraEngine::Renderer::
                                          LoadAnimatedObjectVisibilitySnapshot();
                                  const bool visibilityUsable =
                                      visibility && visibility->valid &&
                                      visibility->sceneGeneration ==
                                          frameSceneGeneration;
                                  // candidateFrameWork is the retired frame
                                  // buffer after the previous swap. Reuse each
                                  // existing record in place so its nested
                                  // binding, parameter, controller, and event
                                  // allocations survive stable complete-mode
                                  // frames.
                                  std::size_t candidateCompleteCount = 0U;
                                  candidateFrameWork.reserve(
                                      capturedFrameInputs.size());
                                  candidateCompactGpuVisualCaptures.clear();
                                  candidateCompactGpuVisualCaptures.reserve(
                                      capturedFrameInputs.size());
								  candidateCompactGpuVisualCaptures.authorityGeneration =
									  candidateAuthorityGeneration;
                                  candidateInputWorkIndices.clear();
                                  candidateInputWorkIndices.reserve(
                                      capturedFrameInputs.size());
                                  const bool compactFrameHasAdvancedRequests =
                                      !capturedMotionMatchRequests.empty() ||
                                      !capturedMotionMatchQueriesV2.empty() ||
                                      !capturedProceduralGoals.empty();
								  const bool visibleForPresentation =
									  !visibilityUsable ||
									  visibility->hasActiveView;
								  const bool compactAdmissionHasExternalWork =
									  !capturedCommands.empty()
									  || compactFrameHasAdvancedRequests
									  || frameHasConsumerDeclarations;
                                  // The protected capture owns one immutable
                                  // runtime-root identity for the whole frame.
                                  // Loading an atomic shared_ptr for every
                                  // animator needlessly serializes
                                  // reference-count traffic across a large
                                  // crowd; retain the root once and perform
                                  // exact slot/generation checks against that
                                  // same owner.
								  const std::shared_ptr<RuntimeRecordRoot>
									  capturedRuntimeRoot = runtimeRecords.load(
										  std::memory_order_acquire);
								  if (!capturedRuntimeRoot)
									return false;
								  const std::shared_ptr<const CompactRuntimeStateRoot>
									  capturedCompactRuntimeRoot = compactRuntimeStates.load(
										  std::memory_order_acquire);
								  candidateCompactRuntimeCaptureRoot =
									  capturedRuntimeRoot;
								  candidateCompactRuntimeStateCaptureRoot =
									  capturedCompactRuntimeRoot;
								  compactCaptureAdmissionProof.assign(
									  capturedFrameInputs.size(), std::uint8_t{0U});
								  const std::size_t captureWorkerCapacity =
									  ThreadPool::instance().getThreadCount();
								  // A worker proof is a second read-only pass followed by ordered SoA
								  // assembly. Measurements on the 4,015-owner authored crowd show that
								  // queue/join overhead exceeds the saved serial checks at that size;
								  // enable it only once each worker receives enough rows to amortize
								  // scheduling and immutable consumer-resolution costs.
								  constexpr std::size_t kMinimumParallelCompactCaptureRows = 8192U;
								  const bool canProveCompactInParallel =
									  capturedFrameInputs.size() >= kMinimumParallelCompactCaptureRows
									  && captureWorkerCapacity > 1U
									  && capturedCommands.empty()
									  && !compactFrameHasAdvancedRequests;
								  const auto proveCompactRange = [this, &capturedRuntimeRoot,
									  &capturedCompactRuntimeRoot](std::size_t begin,
									  std::size_t end) noexcept {
									for(std::size_t inputIndex = begin; inputIndex < end; ++inputIndex) {
										const AnimationFrameInput& input = capturedFrameInputs[inputIndex];
										RuntimeRecord* record = input.handle.IsValid()
											&& input.handle.slotIndex < capturedRuntimeRoot->size()
											? capturedRuntimeRoot->at(input.handle.slotIndex).get() : nullptr;
										if(!record || record->handle != input.handle) continue;
										const CompactRuntimeState* state = FindCompactRuntimeState(
											capturedCompactRuntimeRoot, input.handle);
										const AnimationGeneration poseGeneration = state
											? state->poseGeneration : record->poseGeneration;
										const AnimationExecutionModeState& mode = state
											? state->executionModeState : record->executionModeState;
										const AnimatorPlaybackBinding& binding = record->binding;
										const AnimationGpuExecutionInputs& gpu = binding.gpuExecution;
										// The read view pins one immutable, sorted declaration root for
										// every worker. A few gameplay consumers can therefore reject only
										// their matching animators instead of forcing the entire visual crowd
										// through the serial complete-admission path.
										const AnimationResolvedConsumerRequirements consumers =
											frameHasConsumerDeclarations
											? frameConsumerReadView.Resolve(
												input.handle, binding.jointParents, input.tick)
											: AnimationResolvedConsumerRequirements{};
										const bool consumerClosureProven =
											consumers.status == AnimationStatus::Ok
											&& consumers.attribution.empty();
										const bool policyEligible =
											binding.requestedExecutionPolicy == AnimationExecutionPolicy::Auto
											|| binding.requestedExecutionPolicy
												== AnimationExecutionPolicy::ForceGpuVisual;
										const bool modeProven = input.tick >= 0 && !headlessExecutionRequired
											&& policyEligible && gpu.rendererAvailable
											&& gpu.animationCapabilityAvailable && gpu.resourcesResident
											&& gpu.evaluationHealthy && std::isfinite(gpu.requiredBudget)
											&& std::isfinite(gpu.availableBudget) && gpu.requiredBudget >= 0.0
											&& gpu.requiredBudget <= gpu.availableBudget && mode.initialized
											&& mode.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
											&& (mode.effectiveReason
													== AnimationExecutionModeReason::GpuVisualEligible
												|| mode.effectiveReason
													== AnimationExecutionModeReason::ForceGpuRequested)
											&& mode.modeEnteredTick >= 0 && mode.modeEnteredTick <= input.tick
											&& !mode.promotionPending;
										const bool rootMotionRequiresCpu = RequiresCpuRootMotionAuthority(
											binding.rootMotionPolicy,
											binding.clip && binding.clip->rootMotion.enabled,
											false, false, consumers.closure.rootMotion);
										const bool proven = poseGeneration
											!= std::numeric_limits<AnimationGeneration>::max()
											&& consumerClosureProven
											&& binding.clip && binding.clip->events.empty()
											&& !rootMotionRequiresCpu && !binding.controllerProgramRequired
											&& !binding.retargetProfile && !binding.motionDatabase
											&& !binding.ragdollProfile && binding.evaluationProgram
											&& binding.gpuFastBinding && binding.gpuFastBinding->IsComplete()
											&& binding.clipGeneration != 0U && binding.skeletonGeneration != 0U
											&& binding.programGeneration != 0U
											&& binding.evaluationGenerations.residencyGeneration != 0U
											&& modeProven && frameSceneGeneration != 0U
											&& binding.gpuFastBinding->visual.renderIdentity.rendererGeneration != 0U
											&& record->dependencyStatus == AnimationStatus::Ok
											&& record->ragdollLifecycle == RagdollLifecycleState::Inactive;
										compactCaptureAdmissionProof[inputIndex] = proven ? 1U : 0U;
									}
				  };
				  if(canProveCompactInParallel) {
									  constexpr std::size_t kCompactCaptureRowsPerTask = 1024U;
									  const std::size_t workerCount = std::min<std::size_t>(
										  captureWorkerCapacity,
										  (capturedFrameInputs.size() + kCompactCaptureRowsPerTask - 1U)
											  / kCompactCaptureRowsPerTask);
									  const std::size_t chunkSize =
										  (capturedFrameInputs.size() + workerCount - 1U) / workerCount;
									  std::vector<std::future<void>> captureFutures;
									  captureFutures.reserve(workerCount - 1U);
										// Join children before the captured proof inputs can unwind.
										TerraEngine::Tasks::ScopedTaskSettlement<void> captureFuturesSettlement(captureFutures, TerraEngine::Tasks::NestedTaskGroup::AnimationCompactCapture);
									  for(std::size_t workerIndex = 1U; workerIndex < workerCount; ++workerIndex) {
										  const std::size_t begin = workerIndex * chunkSize;
										  const std::size_t end = std::min(
											  capturedFrameInputs.size(), begin + chunkSize);
										  captureFuturesSettlement.CreateAndEnqueue(ThreadPool::instance(),
											  [&proveCompactRange, begin, end]() {
												  proveCompactRange(begin, end);
											  });
									  }
									  captureFuturesSettlement.RunInline([&] { proveCompactRange(0U, std::min(capturedFrameInputs.size(), chunkSize)); });
					captureFuturesSettlement.GetAll();
				  }
								  std::size_t proceduralRangeIndex = 0U;
                                  for (std::size_t inputIndex = 0U;
                                       inputIndex < capturedFrameInputs.size();
                                       ++inputIndex) {
                                    const AnimationFrameInput &input =
                                        capturedFrameInputs[inputIndex];
                                    RuntimeRecord *record =
                                        input.handle.IsValid() &&
                                                input.handle.slotIndex <
                                                    capturedRuntimeRoot->size()
                                            ? capturedRuntimeRoot
                                                  ->at(input.handle.slotIndex)
                                                  .get()
                                            : nullptr;
                                    if (record != nullptr &&
                                        record->handle != input.handle)
                                      record = nullptr;
									const CompactRuntimeState* compactRuntimeState =
										FindCompactRuntimeState(capturedCompactRuntimeRoot, input.handle);
									const AnimationGeneration currentPoseGeneration = compactRuntimeState
										? compactRuntimeState->poseGeneration
										: (record ? record->poseGeneration : 0U);
									if (record == nullptr ||
										currentPoseGeneration ==
											std::numeric_limits<
												AnimationGeneration>::max()) {
                                      return false;
                                    }
									const AnimatorPlaybackBinding &liveBinding =
										record->binding;
									if(compactCaptureAdmissionProof[inputIndex] != 0U) {
									candidateCompactGpuVisualCaptures.append(
										inputIndex, input, *record, compactRuntimeState);
										candidateInputWorkIndices.push_back(
											{candidateCompactGpuVisualCaptures.size() - 1U, true});
										compactPreCopyCaptureCount.fetch_add(1U, std::memory_order_acq_rel);
										continue;
									}
									const AnimationGpuExecutionInputs &gpu =
										liveBinding.gpuExecution;
									const AnimationExecutionModeState& modeState = compactRuntimeState
										? compactRuntimeState->executionModeState
										: record->executionModeState;
									const bool compactPolicyEligible =
										liveBinding.requestedExecutionPolicy ==
											AnimationExecutionPolicy::Auto ||
										liveBinding.requestedExecutionPolicy ==
											AnimationExecutionPolicy::ForceGpuVisual;
									const bool compactModeProven =
										input.tick >= 0 &&
										!headlessExecutionRequired &&
										compactPolicyEligible &&
										gpu.rendererAvailable &&
										gpu.animationCapabilityAvailable &&
										gpu.resourcesResident &&
										gpu.evaluationHealthy &&
										std::isfinite(gpu.requiredBudget) &&
										std::isfinite(gpu.availableBudget) &&
										gpu.requiredBudget >= 0.0 &&
										gpu.requiredBudget <= gpu.availableBudget &&
										modeState.initialized &&
										modeState.effectiveMode ==
											AnimationEffectiveExecutionMode::GpuVisual &&
										(modeState.effectiveReason ==
											 AnimationExecutionModeReason::GpuVisualEligible ||
										 modeState.effectiveReason ==
											 AnimationExecutionModeReason::ForceGpuRequested) &&
										modeState.modeEnteredTick >= 0 &&
										modeState.modeEnteredTick <= input.tick &&
										!modeState.promotionPending;
									const bool retainedCompactAdmissionProof =
										compactRuntimeState != nullptr
										&& compactRuntimeState->bindingContentGeneration != 0U
										&& compactRuntimeState->bindingContentGeneration
											== record->contentGeneration
										&& !compactAdmissionHasExternalWork
										&& compactModeProven
										&& liveBinding.gpuFastBinding
										&& liveBinding.gpuFastBinding->IsComplete()
										&& liveBinding.gpuFastBinding->visual.renderIdentity
											.rendererGeneration != 0U
										&& frameSceneGeneration != 0U
										&& record->dependencyStatus == AnimationStatus::Ok
										&& record->ragdollLifecycle ==
											RagdollLifecycleState::Inactive;
									if(retainedCompactAdmissionProof) {
										// The preceding committed compact root proves every immutable
										// admission fact for this exact binding generation. Append the
										// current scalar lanes directly after rechecking all live inputs.
										candidateCompactGpuVisualCaptures.append(
											inputIndex, input, *record, compactRuntimeState);
										candidateInputWorkIndices.push_back(
											{candidateCompactGpuVisualCaptures.size() - 1U, true});
										compactPreCopyCaptureCount.fetch_add(
											1U, std::memory_order_acq_rel);
										continue;
									}
									const bool hasTargetedCommand = std::any_of(
                                        capturedCommands.begin(),
                                        capturedCommands.end(),
                                        [&input](
                                            const AnimationCommand &command) {
                                          return command.target == input.handle;
                                        });
                                    // A registry publication can contain a
                                    // gameplay declaration for one animator
                                    // while a large render-only crowd has no
                                    // consumers at all. Resolve only this
                                    // immutable animator range before deciding
                                    // whether its direct clip may bypass the
                                    // complete capture record. Any matching
                                    // declaration (even one with no current
                                    // joint fields) remains a fail-closed
                                    // authoritative observation and therefore
                                    // uses Complete.
                                    const AnimationResolvedConsumerRequirements
                                        compactConsumers =
											frameHasConsumerDeclarations
												? frameConsumerReadView.Resolve(
                                                      input.handle,
                                                      liveBinding.jointParents,
                                                      input.tick)
                                                : AnimationResolvedConsumerRequirements{};
                                    const bool compactConsumerClosureProven =
                                        compactConsumers.status ==
                                            AnimationStatus::Ok &&
                                        compactConsumers.attribution.empty();
                                    // The prior renderer snapshot is complete
                                    // for views that already existed, but an
                                    // automation/custom capture may introduce
                                    // another camera later in this same frame.
                                    // Do not use per-owner absence as a hard
                                    // publication gate: that would leave a
                                    // newly visible compact crowd in bind pose.
                                    // A valid no-view snapshot may still
                                    // suspend all visual work; any active
                                    // output conservatively retains
                                    // presentation and lets GPU culling discard
                                    // owners. A root-motion track is not itself
                                    // CPU authority. Ignore explicitly
                                    // suppresses extraction, while
                                    // Extract/Apply policies and declared
                                    // readers still require the complete
                                    // capture path. Reuse the execution-mode
                                    // rule so the early SoA gate cannot
                                    // disagree with the later policy decision.
                                    const bool
                                        compactRootMotionRequiresCpuAuthority =
                                            RequiresCpuRootMotionAuthority(
                                                liveBinding.rootMotionPolicy,
                                                liveBinding.clip != nullptr &&
                                                    liveBinding.clip->rootMotion
                                                        .enabled,
                                                false, false,
                                                compactConsumers.closure
                                                    .rootMotion);
                                    // Capture needs only a fail-closed
                                    // admission proof; the worker still runs
                                    // the full execution-mode resolver before
                                    // publication. Checking the exact stable
                                    // GPU state here avoids building and
                                    // hashing a duplicate empty consumer
                                    // closure for every crowd member on the
                                    // coordinator thread.
                                    const bool compactCaptureProven =
                                        compactConsumerClosureProven &&
                                        !hasTargetedCommand &&
                                        !compactFrameHasAdvancedRequests &&
                                        liveBinding.clip &&
                                        liveBinding.clip->events.empty() &&
                                        !compactRootMotionRequiresCpuAuthority &&
                                        !liveBinding
                                             .controllerProgramRequired &&
                                        !liveBinding.retargetProfile &&
                                        !liveBinding.motionDatabase &&
                                        !liveBinding.ragdollProfile &&
                                        liveBinding.evaluationProgram &&
                                        liveBinding.gpuFastBinding &&
                                        liveBinding.gpuFastBinding
                                            ->IsComplete() &&
                                        liveBinding.clipGeneration != 0U &&
                                        liveBinding.skeletonGeneration != 0U &&
                                        liveBinding.programGeneration != 0U &&
                                        liveBinding.evaluationGenerations
                                                .residencyGeneration != 0U
                                        // A direct compact capture omits the
                                        // broad CPU fallback state. Admit it
                                        // only after a protected complete frame
                                        // has committed GpuVisual mode;
                                        // first-frame promotion and any
                                        // hysteresis demotion must retain that
                                        // state so planning can safely choose
                                        // the complete evaluator instead.
										&& modeState.initialized &&
									modeState.effectiveMode ==
                                            AnimationEffectiveExecutionMode::
                                                GpuVisual &&
                                        compactModeProven
                                        // Renderer and deterministic external
                                        // generations are cache-reuse domains.
                                        // Direct authored clips intentionally
                                        // leave them unset until a producer
                                        // opts into reuse, and the compact
                                        // packet never reads them. Its copied
                                        // render identity below is the
                                        // authoritative renderer-lifetime proof
                                        // for GPU presentation.
                                        && frameSceneGeneration != 0U &&
                                        liveBinding.gpuFastBinding->visual
                                                .renderIdentity
                                                .rendererGeneration != 0U &&
                                        record->dependencyStatus ==
                                            AnimationStatus::Ok &&
                                        record->ragdollLifecycle ==
                                            RagdollLifecycleState::Inactive;
                                    if (compactCaptureProven) {
									  candidateCompactGpuVisualCaptures.append(
										  inputIndex, input, *record, compactRuntimeState);
                                      candidateInputWorkIndices.push_back(
                                          {candidateCompactGpuVisualCaptures
                                                   .size() -
                                               1U,
                                           true});
                                      compactPreCopyCaptureCount.fetch_add(
                                          1U, std::memory_order_acq_rel);
									  continue;
									}
									if(compactRuntimeState) {
										// A compact owner entering a complete path needs its latest
										// scalar state exactly once. The protected capture precedes all
										// frame workers, and the mixed commit replaces this broad root.
										record->playback = compactRuntimeState->playback;
										record->poseGeneration = compactRuntimeState->poseGeneration;
										record->stateGeneration = compactRuntimeState->stateGeneration;
										record->executionModeGeneration =
											compactRuntimeState->executionModeGeneration;
										record->executionModeState =
											compactRuntimeState->executionModeState;
									}
                                    if (candidateCompleteCount ==
                                        candidateFrameWork.size()) {
                                      candidateFrameWork.emplace_back();
                                    }
                                    FrameWorkItem &item = candidateFrameWork
                                        [candidateCompleteCount];
                                    item.capturedInputIndex = inputIndex;
                                    candidateInputWorkIndices.push_back(
                                        {candidateCompleteCount, false});
                                    ++candidateCompleteCount;
                                    const bool sameAnimator =
                                        item.bindingCaptureHandle ==
                                        input.handle;
                                    const bool bindingStorageReusable =
                                        sameAnimator &&
                                        item.runtimeStateCaptureCommitted &&
                                        item.bindingCaptureContentGeneration ==
                                            record->contentGeneration;
                                    const bool parameterStorageReusable =
                                        sameAnimator &&
                                        item.runtimeStateCaptureCommitted &&
                                        item.capturedParameterGeneration ==
                                            record->parameterGeneration;
                                    const bool sourceObjectTransformReusable =
                                        sameAnimator &&
                                        item.runtimeStateCaptureCommitted &&
                                        item.sourceObjectWorldTransformCaptured &&
                                        item.sourceObjectSceneGeneration ==
                                            frameSceneGeneration &&
                                        item.sourceObjectSpatialRevision ==
                                            frameSpatialRevision;
                                    const bool priorEventStateCaptured =
                                        item.eventStateCaptured;
                                    const bool priorControllerStateCaptured =
                                        item.controllerStateCaptured;
									item.ResetForCaptureReuse();
									item.input = input;
									item.rootMotionEnabled = rootEnabledFor(input.handle);
                                    if (!sourceObjectTransformReusable) {
                                      item.sourceObjectGeneration = 0U;
                                      item.sourceObjectWorldTransform = {};
                                      item.sourceObjectWorldTransformCaptured =
                                          false;
                                      item.sourceObjectSceneGeneration = 0U;
                                      item.sourceObjectSpatialRevision = 0U;
                                    }
                                    // Missing, first-frame, or stale renderer
                                    // data fails open. Only an exact
                                    // scene-generation publication may suppress
                                    // visual evaluation.
                                    item.visibleForVisualEvaluation =
                                        visibleForPresentation;
                                    if (!bindingStorageReusable) {
                                      // Rebind and hot-reload advance
                                      // contentGeneration, so this is the only
                                      // steady-state path that copies the
                                      // binding's large immutable vectors.
                                      item.binding = record->binding;
                                      item.bindingOutputRequirementsFingerprint =
                                          0U;
                                      item.bindingResourceLayoutFingerprint =
                                          0U;
                                    } else {
                                      // Controller transitions change only this
                                      // small dynamic tail. Preserve the cached
                                      // immutable binding body and refresh the
                                      // fields that can be committed without a
                                      // full asset rebind.
                                      item.binding.stateId =
                                          record->binding.stateId;
                                      const bool clipChanged =
                                          item.binding.clip !=
                                              record->binding.clip ||
                                          item.binding.clipGeneration !=
                                              record->binding.clipGeneration ||
                                          item.binding
                                                  .clipDeterministicContentFingerprint !=
                                              record->binding
                                                  .clipDeterministicContentFingerprint ||
                                          item.binding
                                                  .streamedSegmentPayloads !=
                                              record->binding
                                                  .streamedSegmentPayloads;
                                      item.binding.clip = record->binding.clip;
                                      item.binding.clipGeneration =
                                          record->binding.clipGeneration;
                                      item.binding
                                          .clipDeterministicContentFingerprint =
                                          record->binding
                                              .clipDeterministicContentFingerprint;
                                      item.binding.streamedSegmentPayloads =
                                          record->binding
                                              .streamedSegmentPayloads;
                                      if (clipChanged) {
                                        item.binding.residentSegments =
                                            record->binding.residentSegments;
                                        item.bindingResourceLayoutFingerprint =
                                            0U;
                                      }
                                      item.binding.compiledRetargetProfile =
                                          record->binding
                                              .compiledRetargetProfile;
                                      item.binding.evaluationGenerations =
                                          record->binding.evaluationGenerations;
                                    }
                                    item.bindingCaptureHandle = input.handle;
                                    item.bindingCaptureContentGeneration =
                                        record->contentGeneration;
                                    // Mutable playback/controller state is
                                    // always recopied from the live protected
                                    // record. A generation can remain equal
                                    // after a cancelled or test-driven partial
                                    // frame, so retaining a worker-mutated
                                    // candidate here could otherwise consume a
                                    // command without ever publishing it.
                                    item.playback = record->playback;
                                    item.eventStateCaptured =
                                        item.binding.clip &&
                                        item.binding.eventPolicy !=
                                            AnimatorEventPolicy::Suppress &&
                                        !item.binding.clip->events.empty();
                                    if (item.eventStateCaptured) {
                                      item.eventStream = record->eventStream;
                                    } else if (priorEventStateCaptured ||
                                               !sameAnimator) {
                                      // A slot that previously belonged to an
                                      // event-bearing clip must not leak its
                                      // delivery watermark into a later
                                      // direct-clip fast path.
                                      item.eventStream = {};
                                    }
                                    item.controllerStateCaptured =
                                        item.binding.evaluationProgram !=
                                        nullptr;
                                    if (item.controllerStateCaptured) {
                                      item.capturedGraphState =
                                          record->graphState;
                                      item.candidateGraphState =
                                          record->graphState;
                                      // Both private copies are now complete,
                                      // so exchange the candidate and live
                                      // buffers without changing either value.
                                      // Workers mutate the original allocation
                                      // while the runtime keeps an identical
                                      // rollback copy; successful publication
                                      // can later swap the candidate back in
                                      // without deep-copying three history
                                      // vectors for every animator.
                                      std::swap(item.candidateGraphState,
                                                record->graphState);
                                      item.transition = record->transition;
                                    } else if (priorControllerStateCaptured ||
                                               !sameAnimator) {
                                      // Clear a reused slot once when it leaves
                                      // controller evaluation. Later
                                      // direct-clip frames retain these
                                      // already-empty allocations.
                                      item.capturedGraphState = {};
                                      item.candidateGraphState = {};
                                      item.transition = {};
                                    }
                                    item.dependencyStatus =
                                        record->dependencyStatus;
                                    item.previousExecutionModeState =
                                        record->executionModeState;
                                    if (!parameterStorageReusable) {
                                      // Generation equality proves these
                                      // vectors still match the live protected
                                      // record. Preserve their retained
                                      // allocations on the common command-free
                                      // crowd frame.
                                      item.parameters = record->parameters;
                                      item.layerWeights = record->layerWeights;
                                    }
                                    item.activeRetargetProfileId =
                                        record->activeRetargetProfileId;
                                    item.activeRetargetProfileGeneration =
                                        record->activeRetargetProfileGeneration;
                                    item.retargetActive =
                                        item.binding.retargetProfile &&
                                        item.binding.compiledRetargetProfile &&
                                        item.binding.retargetSourceSkeleton &&
                                        item.binding.skeleton &&
                                        MatchesNativeUuid(
                                            item.activeRetargetProfileId,
                                            item.binding.retargetProfile
                                                ->assetId.value) &&
                                        item.activeRetargetProfileGeneration ==
                                            item.binding
                                                .retargetProfileGeneration;
                                    if (item.bindingOutputRequirementsFingerprint ==
                                        0U) {
                                      item.bindingOutputRequirementsFingerprint =
                                          BuildBindingOutputRequirementsFingerprint(
                                              item.binding);
                                    }
                                    if (item.bindingResourceLayoutFingerprint ==
                                        0U) {
                                      item.bindingResourceLayoutFingerprint =
                                          BuildBindingResourceLayoutFingerprint(
                                              item.binding);
                                    }
                                    item.activeMotionDatabaseId =
                                        record->activeMotionDatabaseId;
                                    item.activeMotionDatabaseGeneration =
                                        record->activeMotionDatabaseGeneration;
                                    item.latestMotionQueryId =
                                        record->latestMotionQueryId;
                                    item.appliedMotionQueryId =
                                        record->appliedMotionQueryId;
                                    item.latestMotionCandidate =
                                        record->latestMotionCandidate;
                                    item.hasMotionCandidate =
                                        record->hasMotionCandidate;
                                    item.restoredFromHistory = std::any_of(
                                        determinismRequestCompletions.begin(),
                                        determinismRequestCompletions.end(),
                                        [&input](
                                            const DeterminismRequestCompletion
                                                &completion) {
                                          return completion.stateRestored &&
                                                 completion.request.animator
                                                         .slotIndex ==
                                                     input.handle.slotIndex &&
                                                 completion.request.animator
                                                         .generation ==
                                                     input.handle.generation;
                                        });
                                    for (const NativeAnimationMotionMatchRequest
                                             &request :
                                         capturedMotionMatchRequests) {
                                      if (request.operation ==
                                              NativeAnimationMotionMatchOperation::
                                                  Query &&
                                          request.animator.slotIndex ==
                                              input.handle.slotIndex &&
                                          request.animator.generation ==
                                              input.handle.generation) {
                                        item.pendingMotionQueries.push_back(
                                            request);
                                      }
                                    }
                                    for (
                                        const NativeAnimationMotionMatchQueryRequestV2
                                            &request :
                                        capturedMotionMatchQueriesV2) {
                                      if (request.animator.slotIndex ==
                                              input.handle.slotIndex &&
                                          request.animator.generation ==
                                              input.handle.generation) {
                                        item.pendingMotionQueriesV2.push_back(
                                            request);
                                      }
                                    }
                                    const NativeAnimationAnimatorHandle
                                        nativeHandle{input.handle.slotIndex,
                                                     input.handle.generation};
                                    while (
                                        proceduralRangeIndex <
                                            capturedProceduralGoalRanges
                                                .size() &&
                                        std::tie(capturedProceduralGoalRanges
                                                     [proceduralRangeIndex]
                                                         .animator.slotIndex,
                                                 capturedProceduralGoalRanges
                                                     [proceduralRangeIndex]
                                                         .animator.generation) <
                                            std::tie(nativeHandle.slotIndex,
                                                     nativeHandle.generation)) {
                                      ++proceduralRangeIndex;
                                    }
                                    if (proceduralRangeIndex <
                                            capturedProceduralGoalRanges
                                                .size() &&
                                        capturedProceduralGoalRanges
                                                [proceduralRangeIndex]
                                                    .animator.slotIndex ==
                                            nativeHandle.slotIndex &&
                                        capturedProceduralGoalRanges
                                                [proceduralRangeIndex]
                                                    .animator.generation ==
                                            nativeHandle.generation) {
                                      const ActiveProceduralGoalRange &range =
                                          capturedProceduralGoalRanges
                                              [proceduralRangeIndex];
                                      item.proceduralStateObserved = true;
                                      item.activeProceduralGoals.assign(
                                          capturedProceduralGoals.begin() +
                                              range.firstGoal,
                                          capturedProceduralGoals.begin() +
                                              range.firstGoal +
                                              range.goalCount);
                                    }
                                    item.ragdollGeneration =
                                        record->ragdollGeneration;
                                    item.ragdollLifecycle =
                                        record->ragdollLifecycle;
                                    item.ragdollBlendWeight =
                                        record->ragdollBlendWeight;
                                    item.ragdollLifecycleStartTick =
                                        record->ragdollLifecycleStartTick;
                                    item.ragdollRecoveryAlignedRootPosition =
                                        record
                                            ->ragdollRecoveryAlignedRootPosition;
                                    item.ragdollRecoveryDurationSeconds =
                                        record->ragdollRecoveryDurationSeconds;
                                    if (record->ragdollGeneration.IsValid() &&
                                        (record->ragdollLifecycle ==
                                             RagdollLifecycleState::Active ||
                                         record->ragdollLifecycle ==
                                             RagdollLifecycleState::
                                                 Recovering)) {
                                      item.ragdollPhysicsPose =
                                          Physics::PhysicsSystem::Instance()
                                              .LatestRagdollPhysicsPoseAtOrAfter(
                                                  record->ragdollId,
                                                  record->ragdollGeneration);
                                      if (item.ragdollPhysicsPose) {
                                        item.ragdollGeneration =
                                            item.ragdollPhysicsPose->generation;
                                      }
                                    }
                                    // A complete candidate owns a copied row
                                    // while workers run.  The source is the
                                    // immutable authority root, never a runtime
                                    // record.
                                    const std::shared_ptr<
                                        const AnimationAuthorityFrame>
                                        authority = authorityPublisher.Load();
                                    const AnimationAuthorityRow *prior =
                                        authority
                                            ? authority->Find(record->handle)
                                            : nullptr;
                                    item.priorPublication =
                                        prior
                                            ? std::make_shared<
                                                  const AnimationAuthorityRow>(
                                                  *prior)
                                            : std::shared_ptr<
                                                  const AnimationAuthorityRow>{};
                                    item.capturedPoseGeneration =
                                        record->poseGeneration;
                                    item.capturedStateGeneration =
                                        record->stateGeneration;
                                    item.capturedParameterGeneration =
                                        record->parameterGeneration;
                                    item.capturedExecutionModeGeneration =
                                        record->executionModeGeneration;
                                    item.contentGeneration =
                                        record->contentGeneration;
                                    item.candidateStateGeneration =
                                        record->stateGeneration;
                                    item.candidateParameterGeneration =
                                        record->parameterGeneration;
                                    item.candidateExecutionModeGeneration =
                                        record->executionModeGeneration;
                                    item.capturedPlaybackState =
                                        record->playback.Snapshot();
                                    // Triggers are edge commands. Their copied
                                    // snapshot may be observed for one
                                    // publication, but they never remain armed
                                    // in a later frame.
                                    std::erase_if(
                                        item.parameters,
                                        [](const AnimationParameterSnapshot
                                               &value) {
                                          return value.type ==
                                                 AnimationParameterType::
                                                     Trigger;
                                        });
                                    item.candidatePoseGeneration =
                                        record->poseGeneration + 1U;
                                    item.candidateAuthorityPublicationGeneration =
                                        candidateAuthorityGeneration;
                                  }
                                  candidateFrameWork.resize(
                                      candidateCompleteCount);
                                }
                                {
					// Scene access stays in one protected pass. This sample
					// exposes hierarchy lookup cost separately from
					// animation-owned state copying.
					TERRAENGINE_PROFILE_SCOPE("Animation: Capture Object Transforms",
											  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					ObjectManager &objects = ObjectManager::instance();
					for(FrameWorkItem& item : candidateFrameWork) {
						const AnimationFrameInput& input = item.input;
						if(item.sourceObjectWorldTransformCaptured) continue;
						if(const std::optional<Object> object =
							   objects.getObject(input.descriptor.ownerObjectId)) {
							item.sourceObjectGeneration = TerraEngine::CompressObjectGeneration(
								objects.getObjectLifetimeGeneration(*object));
							item.sourceObjectWorldTransformCaptured = CaptureObjectWorldTransform(
								objects, *object, item.sourceObjectWorldTransform);
							if(item.sourceObjectGeneration != 0U
								&& item.sourceObjectWorldTransformCaptured) {
								item.sourceObjectSceneGeneration = frameSceneGeneration;
								item.sourceObjectSpatialRevision = frameSpatialRevision;
							}
						}
					}
				}
				{
					// Clip views retain validated immutable payloads across
					// alternating frame slots. Keep their validation cost
					// visible without emitting one sample per animator, which
					// would make detailed captures unnecessarily expensive.
					TERRAENGINE_PROFILE_SCOPE("Animation: Open Clip Views",
										  TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					struct ReusableClipView {
						const AnimationClipAsset* clip = nullptr;
						const SkeletonAsset* skeleton = nullptr;
						const AnimationClipSegmentPayloadSet* streamedPayloads = nullptr;
						AnimationGeneration generation = 0U;
						AnimationClipView view;
					};
					// Identical prefab instances consume the same immutable clip, skeleton,
					// and streamed payload publication. Validate that tuple once per capture
					// and copy the checked view instead of reopening the codec thousands of
					// times after a shared residency promotion.
					std::vector<ReusableClipView> reusableClipViews;
					reusableClipViews.reserve(32U);
					for(FrameWorkItem &item : candidateFrameWork) {
						if(item.dependencyStatus == AnimationStatus::Ok && item.binding.clip &&
						   item.binding.skeleton) {
							const bool sourceCompatible =
								item.retargetActive && item.binding.retargetSourceSkeleton &&
								item.binding.clip->compatibilityFingerprint ==
									item.binding.retargetSourceSkeleton->compatibilityFingerprint &&
								item.binding.clip->skeletonAssetId ==
									item.binding.retargetSourceSkeleton->assetId;
							const SkeletonAsset *samplingSkeleton =
								sourceCompatible ? item.binding.retargetSourceSkeleton.get()
											 : item.binding.skeleton.get();
							item.sampledSourceSkeleton = sourceCompatible;
							const std::span<const std::uint8_t> residentSegments(
								item.binding.residentSegments);
                                                        const auto reusable = std::find_if(
                                                            reusableClipViews
                                                                .begin(),
                                                            reusableClipViews
                                                                .end(),
                                                            [&item,
                                                             samplingSkeleton,
                                                             residentSegments](
                                                                const ReusableClipView
                                                                    &candidate) {
                                                              return candidate
                                                                             .clip ==
                                                                         item.binding
                                                                             .clip
                                                                             .get() &&
                                                                     candidate
                                                                             .skeleton ==
                                                                         samplingSkeleton &&
                                                                     candidate
                                                                             .streamedPayloads ==
                                                                         item.binding
                                                                             .streamedSegmentPayloads
                                                                             .get() &&
                                                                     candidate
                                                                             .generation ==
                                                                         item.binding
                                                                             .clipGeneration &&
                                                                     candidate
                                                                         .view
                                                                         .Matches(
                                                                             *item.binding
                                                                                  .clip,
                                                                             samplingSkeleton,
                                                                             residentSegments,
                                                                             item.binding
                                                                                 .clipGeneration,
                                                                             item.binding
                                                                                 .streamedSegmentPayloads);
                                                            });
                                                        if (reusable !=
                                                            reusableClipViews
                                                                .end()) {
                                                          item.clipView =
                                                              reusable->view;
                                                          item.dependencyStatus =
                                                              AnimationStatus::
                                                                  Ok;
                                                        } else {
                                                          item.dependencyStatus =
                                                              item.clipView.Open(
                                                                  *item.binding
                                                                       .clip,
                                                                  samplingSkeleton,
                                                                  residentSegments,
                                                                  item.binding
                                                                      .clipGeneration,
                                                                  item.binding
                                                                      .streamedSegmentPayloads);
                                                          if (item.dependencyStatus ==
                                                              AnimationStatus::
                                                                  Ok) {
                                                            reusableClipViews.push_back(
                                                                {item.binding
                                                                     .clip
                                                                     .get(),
                                                                 samplingSkeleton,
                                                                 item.binding
                                                                     .streamedSegmentPayloads
                                                                     .get(),
                                                                 item.binding
                                                                     .clipGeneration,
                                                                 item.clipView});
                                                          }
                                                        }
                                                }
					}
				}
				frameWork.swap(candidateFrameWork);
				compactGpuVisualCaptures.swap(candidateCompactGpuVisualCaptures);
				compactRuntimeCaptureRoot.swap(candidateCompactRuntimeCaptureRoot);
				compactRuntimeStateCaptureRoot.swap(
					candidateCompactRuntimeStateCaptureRoot);
				capturedInputWorkIndices.swap(candidateInputWorkIndices);
				frameTick = captureTick;
				return true;
			} catch(const std::bad_alloc &) {
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

		static std::uint64_t BuildBindingOutputRequirementsFingerprint(
			const AnimatorPlaybackBinding& binding) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixWorkFingerprint(hash, binding.jointStableIds.size());
			hash = MixWorkFingerprint(hash, binding.morphStableIds.size());
			hash = MixWorkFingerprint(hash, binding.curveStableIds.size());
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

		static std::uint64_t BuildBindingResourceLayoutFingerprint(
			const AnimatorPlaybackBinding& binding) noexcept
		{
			std::uint64_t hash = 1469598103934665603ULL;
			hash = MixWorkFingerprint(hash, binding.gpuProgramResourceIndex);
			hash = MixWorkFingerprint(hash, binding.residentSegments.size());
			for(const std::uint8_t resident : binding.residentSegments) {
				hash = MixWorkFingerprint(hash, resident);
			}
			if(binding.streamedSegmentPayloads) {
				const AnimationClipSegmentPayloadSet& payloads =
					*binding.streamedSegmentPayloads;
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
			hash = MixWorkFingerprint(hash, binding.authoritativeJointIndices.size());
			hash = MixWorkFingerprint(hash, binding.gpuResidentPayloads ? 1U : 0U);
			hash = MixWorkFingerprint(hash, binding.clipGeneration);
			hash = MixWorkFingerprint(hash, binding.skeletonGeneration);
			hash = MixWorkFingerprint(hash, binding.programGeneration);
			hash = MixWorkFingerprint(hash, binding.retargetProfileGeneration);
			hash = MixWorkFingerprint(hash, binding.retargetSourceSkeletonGeneration);
			hash = MixWorkFingerprint(hash, binding.motionDatabaseGeneration);
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
					sizeof(PoseBuffer) + sizeof(AnimationSnapshot) + sizeof(AnimationAuthorityRow),
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
			item.cacheRequirements.visible = item.visibleForVisualEvaluation;
			item.cacheRequirements.timeAdvances =
				item.advance.previousAbsoluteTick != item.advance.currentAbsoluteTick;
			item.cacheRequirements.events = item.cacheRequirements.timeAdvances
				&& item.binding.eventPolicy != AnimatorEventPolicy::Suppress;
			item.cacheRequirements.rootMotion = item.cacheRequirements.timeAdvances
				&& (item.rootMotionEnabled
					|| item.binding.rootMotionPolicy != AnimatorRootMotionPolicy::Ignore);
			item.cacheRequirements.physics = item.executionDecision.consumerClosure.physics;
			item.cacheRequirements.recording = item.executionDecision.consumerClosure.recording;
			item.cacheRequirements.replication = item.executionDecision.consumerClosure.networking;
			item.cacheRequirements.rollback = item.executionDecision.consumerClosure.rollback;
			item.cacheRequirements.deterministic = item.resolvedConsumers.deterministic;
			item.cacheRequirements.automation = item.executionDecision.consumerClosure.automation;
			item.cacheRequirements.detailedDiagnostics = item.executionDecision.consumerClosure.diagnostics;
			// An advancing clock, event/root-motion publication, physics, recording,
			// replication, rollback, automation, or detailed diagnostics requires new
			// authority by contract. Such work can never reuse this cache entry, so do
			// not take the cache mutex or replace its per-animator tree node every frame.
			// A newly paused/stable animator evaluates once to seed its exact key and
			// then resumes ordinary unchanged-result reuse on subsequent frames.
			item.cacheEligible = !item.cacheRequirements.HasAuthoritativeWork();
			if(!item.cacheEligible) return;
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
			item.safePose = item.priorPublication->safeFallback;
			item.evaluated = true;
			cacheReusedAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
		}

		[[nodiscard]] static bool CanSuspendVisualEvaluation(
			const FrameWorkItem& item) noexcept
		{
			// Visibility may remove only presentation work. Every gameplay, tooling,
			// deterministic, event, root-motion, procedural, and physics obligation
			// keeps full evaluation regardless of the current camera set.
			return !item.visibleForVisualEvaluation
				&& item.binding.cullingPolicy != AnimatorCullingPolicy::AlwaysUpdate
				&& item.evaluationStatus == AnimationStatus::Ok
				&& !item.cacheReused
				&& item.resolvedConsumers.status == AnimationStatus::Ok
				&& !item.resolvedConsumers.deterministic
				&& !item.executionDecision.consumerClosure.HasAuthoritativeSubset()
				&& item.binding.eventPolicy == AnimatorEventPolicy::Suppress
				&& !item.rootMotionEnabled
				&& item.binding.rootMotionPolicy == AnimatorRootMotionPolicy::Ignore
				&& item.activeProceduralGoals.empty()
				&& item.pendingMotionQueries.empty()
				&& item.pendingMotionQueriesV2.empty()
				&& !item.retargetActive
				&& item.ragdollLifecycle == RagdollLifecycleState::Inactive
				&& (item.executionDecision.effectiveMode
						== AnimationEffectiveExecutionMode::GpuVisual
					|| (item.priorPublication && item.priorPublication->snapshot
						&& item.priorPublication->snapshot->pose));
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

		// GPU visual bindings are long-lived asset records, while renderer, scene, and
		// playback identities belong to exactly one captured frame. Keep this stamp in
		// one helper so complete and compact lowering cannot disagree about a packet's
		// shader-visible 32-bit lanes or its exact 64-bit render identity.
		[[nodiscard]] bool StampGpuVisualFrameIdentity(
			GpuAnimationVisualInput& visual,
			AnimationFixedTick resolvedTick,
			AnimationFixedTick frameTick) const noexcept
		{
			const GpuAnimationRenderIdentity& renderIdentity = visual.renderIdentity;
			if(!IsGpuAnimationRenderIdentityValid(renderIdentity)
				|| renderIdentity.meshHeaderVersion > std::numeric_limits<std::uint32_t>::max()
				|| renderIdentity.rendererGeneration > std::numeric_limits<std::uint32_t>::max()
				|| frameSceneGeneration == 0U
				|| frameSceneGeneration > std::numeric_limits<std::uint32_t>::max()
				|| resolvedTick < 0
				|| static_cast<std::uint64_t>(resolvedTick)
					> std::numeric_limits<std::uint32_t>::max()) {
				return false;
			}
			visual.generations.meshGeneration = renderIdentity.meshHeaderVersion;
			visual.generations.rendererGeneration = static_cast<std::uint32_t>(
				renderIdentity.rendererGeneration);
			visual.generations.sceneGeneration = static_cast<std::uint32_t>(
				frameSceneGeneration);
			visual.sampleTick = static_cast<std::uint32_t>(resolvedTick);
			// The renderer validates this full-width value beside the narrowed shader ABI.
			visual.renderIdentity.sceneGeneration = frameSceneGeneration;
			visual.generations.frameIdLow = static_cast<std::uint32_t>(frameTick);
			visual.generations.frameIdHigh = static_cast<std::uint32_t>(
				static_cast<std::uint64_t>(frameTick) >> 32U);
			return true;
		}

		void PlanCompactCapture(FrameWorkItem& item, const AnimatorPlaybackTimeInput& timeInput) noexcept
		{
			// This branch intentionally touches only CompactCapture and scalar authority
			// fields. It is the worker half of the protected early classification above.
			AnimatorPlaybackTimeInput effectiveTime = timeInput;
			if(item.compactCapture.updatePolicy == AnimatorUpdatePolicy::Manual) effectiveTime = {};
			const AnimationStatus advanceStatus = item.playback.Advance(effectiveTime, item.advance);
			item.evaluationStatus = advanceStatus;
			const AnimatorPlaybackStateSnapshot playback = item.playback.Snapshot();
			if(!TryAdvanceGeneration(item.capturedStateGeneration,
				playback.resolvedTick != item.previousExecutionModeState.continuity.stateTimeTick,
				item.candidateStateGeneration)) {
				item.evaluationStatus = AnimationStatus::RangeOverflow;
			}
			AnimationExecutionModeRequest request;
			request.fixedTick = item.input.tick;
			request.requestedPolicy = item.compactCapture.requestedPolicy;
			request.headless = headlessExecutionRequired;
			request.gpu = item.compactCapture.gpuExecution;
			request.continuity.transitionBoundaryTick = item.input.tick;
			request.continuity.stateTimeTick = item.playback.AbsoluteTick();
			request.continuity.normalizedControllerTime = static_cast<float>(playback.resolvedTick);
			request.continuity.controllerState = 1U;
			request.continuity.parameterSnapshot = item.candidateParameterGeneration;
			request.continuity.synchronizationState = item.candidateStateGeneration;
			request.continuity.synchronizationPhaseTick = playback.resolvedTick;
			request.continuity.pose = item.candidatePoseGeneration;
			item.executionDecision = ResolveAnimationExecutionMode(request, item.previousExecutionModeState);
			if(!TryAdvanceGeneration(item.capturedExecutionModeGeneration,
				item.executionDecision.nextState.effectiveMode
					!= item.previousExecutionModeState.effectiveMode
					|| item.executionDecision.nextState.effectiveReason
						!= item.previousExecutionModeState.effectiveReason,
				item.candidateExecutionModeGeneration)) {
				item.evaluationStatus = AnimationStatus::RangeOverflow;
			}
			if(item.evaluationStatus != AnimationStatus::Ok
				|| item.executionDecision.effectiveMode != AnimationEffectiveExecutionMode::GpuVisual) {
				item.processingClass = AnimationProcessingClass::Complete;
				item.planned = true;
				return;
			}
			GpuAnimationLoweringInput lowering;
			lowering.program.program = item.compactCapture.program;
			lowering.program.programContentId = item.compactCapture.program->stableHash;
			lowering.program.programResourceIndex = item.compactCapture.fastBinding->programResourceIndex;
			lowering.visual = item.compactCapture.fastBinding->visual;
			lowering.visual.stableInstanceId = (static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U)
				| item.input.handle.generation;
			lowering.visual.stableSubmissionOrdinal = item.input.handle.slotIndex;
			lowering.visual.jointCount = item.compactCapture.jointCount;
			lowering.visual.evaluationParameterFingerprint = MixWorkFingerprint(
				item.candidateParameterGeneration, item.candidateStateGeneration);
			lowering.visual.sampleTick = static_cast<std::uint32_t>(std::max<AnimationFixedTick>(0, playback.resolvedTick));
			lowering.visual.generations.instanceSlot = item.input.handle.slotIndex;
			lowering.visual.generations.instanceGeneration = item.input.handle.generation;
			lowering.visual.generations.componentGeneration = item.input.descriptor.componentGeneration;
			lowering.visual.generations.workGeneration = item.candidatePoseGeneration;
			lowering.visual.generations.programGeneration = item.compactCapture.programGeneration;
			lowering.visual.generations.controllerGeneration = item.compactCapture.programGeneration;
			lowering.visual.generations.clipGeneration = item.compactCapture.clipGeneration;
			lowering.visual.generations.skeletonGeneration = item.compactCapture.skeletonGeneration;
			if(!StampGpuVisualFrameIdentity(
				lowering.visual, playback.resolvedTick, item.input.tick)) {
				item.processingClass = AnimationProcessingClass::Complete;
				item.planned = true;
				return;
			}
			lowering.effectiveMode = item.executionDecision.effectiveMode;
			lowering.allowSharedVisualEvaluation = true;
			lowering.residentPayloads = item.compactCapture.fastBinding->residentPayloads;
			GpuAnimationWorkRequest probe{};
			if(LowerGpuAnimationWorkRequest(lowering, probe) != GpuAnimationLoweringStatus::Ok
				|| !ValidateGpuAnimationResidentPayloads(probe, *lowering.residentPayloads)) {
				item.processingClass = AnimationProcessingClass::Complete;
				item.planned = true;
				return;
			}
			lowering.residentPayloadsValidated = true;
			lowering.prevalidatedStageMask = probe.batchKey.stageMask;
			item.gpuStageMask = probe.batchKey.stageMask;
			item.gpuLowering = std::move(lowering);
			CompactGpuVisualInput input{};
			input.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
			input.stableSubmissionOrdinal = item.gpuLowering->visual.stableSubmissionOrdinal;
			input.playbackTick = item.playback.AbsoluteTick();
			input.sourcePoseGeneration = item.candidatePoseGeneration;
			input.generations = {input.stableInstanceId, item.input.descriptor.componentGeneration,
				item.compactCapture.programGeneration, item.compactCapture.clipGeneration,
				frameSceneGeneration, item.gpuLowering->visual.renderIdentity.rendererGeneration,
				item.candidateAuthorityPublicationGeneration};
			input.eligibilityReason = item.executionDecision.reason;
			input.gpuLoweringComplete = true;
			// CompactCapture admission positively excludes authority consumers,
			// controller transitions, and advanced animation features. Record that
			// proof explicitly because CompactGpuVisualInput defaults these bits to
			// fail-closed true for every caller that has not performed the capture gate.
			input.hasAuthoritativeConsumer = false;
			input.hasControllerOrTransition = false;
			input.hasAdvancedFeature = false;
			input.hasContinuitySource = item.previousExecutionModeState.initialized;
			item.compactGpuVisualInput = input;
			item.processingClass = ClassifyAnimationProcessing(input);
			item.planned = true;
		}

		void PlanCompactCapture(
			std::size_t compactIndex,
			const AnimatorPlaybackTimeInput& timeInput,
			const CompactGpuVisualCohort*& validatedCohort,
			std::uint32_t& validatedStageMask) noexcept
		{
			if(compactIndex >= compactGpuVisualCaptures.size()) return;
			CompactGpuVisualCaptures& compact = compactGpuVisualCaptures;
			const CompactGpuVisualCohort& cohort = compact.cohort(compactIndex);
			// This is intentionally a direct SoA evaluator. It copies no generic
			// FrameWorkItem and reads only the protected compact capture columns.
			AnimatorPlaybackTimeInput effectiveTime = timeInput;
			if(cohort.updatePolicy == AnimatorUpdatePolicy::Manual) effectiveTime = {};
			AnimationStatus& status = compact.evaluationStatuses[compactIndex];
			AnimationPlanRejectionReason& planningFailure =
				compact.planningFailures[compactIndex];
			planningFailure = AnimationPlanRejectionReason::None;
			status = compact.playbacks[compactIndex].Advance(
				effectiveTime, compact.advances[compactIndex]);
			if(status != AnimationStatus::Ok) {
				planningFailure = AnimationPlanRejectionReason::PlaybackAdvance;
			}
			const AnimatorPlaybackStateSnapshot playback = compact.playbacks[compactIndex].Snapshot();
			const AnimationExecutionModeState& previous = compact.previousExecutionModes[compactIndex];
			if(!TryAdvanceGeneration(compact.stateGenerations[compactIndex],
				playback.resolvedTick != previous.continuity.stateTimeTick,
				compact.stateGenerations[compactIndex])) {
				status = AnimationStatus::RangeOverflow;
				planningFailure = AnimationPlanRejectionReason::GenerationOverflow;
			}
			AnimationModeContinuityToken continuity;
			continuity.transitionBoundaryTick = compact.inputs[compactIndex].tick;
			continuity.stateTimeTick = compact.playbacks[compactIndex].AbsoluteTick();
			continuity.normalizedControllerTime = static_cast<float>(playback.resolvedTick);
			continuity.controllerState = 1U;
			continuity.parameterSnapshot = compact.parameterGenerations[compactIndex];
			continuity.synchronizationState = compact.stateGenerations[compactIndex];
			continuity.synchronizationPhaseTick = playback.resolvedTick;
			continuity.pose = compact.candidatePoseGenerations[compactIndex];
			CompactExecutionDecision& executionDecision =
				compact.executionDecisions[compactIndex];
			const AnimationGpuExecutionInputs& gpu = cohort.gpuExecution;
			// Protected capture admitted this row only after proving a stable GPU-visual
			// mode, an empty authority closure, and complete resident resources. Refresh
			// that retained mode directly while only replacing its per-frame continuity
			// token. The generic resolver remains the fail-closed path if any copied proof
			// is no longer internally consistent, so new policies cannot accidentally
			// inherit this optimized route.
			const bool stableGpuVisualProof = compact.inputs[compactIndex].tick >= 0
				&& !headlessExecutionRequired
				&& (cohort.requestedPolicy == AnimationExecutionPolicy::Auto
					|| cohort.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual)
				&& gpu.rendererAvailable && gpu.animationCapabilityAvailable
				&& gpu.resourcesResident && gpu.evaluationHealthy
				&& std::isfinite(gpu.requiredBudget)
				&& std::isfinite(gpu.availableBudget)
				&& gpu.requiredBudget >= 0.0 && gpu.availableBudget >= 0.0
				&& gpu.requiredBudget <= gpu.availableBudget
				&& previous.initialized
				&& previous.effectiveMode == AnimationEffectiveExecutionMode::GpuVisual
				&& (previous.effectiveReason
						== AnimationExecutionModeReason::GpuVisualEligible
					|| previous.effectiveReason
						== AnimationExecutionModeReason::ForceGpuRequested)
				&& previous.modeEnteredTick >= 0
				&& previous.modeEnteredTick <= compact.inputs[compactIndex].tick
				&& !previous.promotionPending;
			if(stableGpuVisualProof) {
				executionDecision = {};
				const AnimationExecutionModeReason reason =
					cohort.requestedPolicy == AnimationExecutionPolicy::ForceGpuVisual
						? AnimationExecutionModeReason::ForceGpuRequested
						: AnimationExecutionModeReason::GpuVisualEligible;
				executionDecision.effectiveMode = AnimationEffectiveExecutionMode::GpuVisual;
				executionDecision.reason = reason;
				executionDecision.nextState = previous;
				executionDecision.nextState.effectiveReason = reason;
				executionDecision.nextState.continuity = continuity;
				executionDecision.nextState.promotionPending = false;
				executionDecision.nextState.promotionMode =
					AnimationEffectiveExecutionMode::CpuDeterministicFallback;
				executionDecision.nextState.promotionReason =
					AnimationExecutionModeReason::Uninitialized;
				executionDecision.nextState.promotionSinceTick = 0;
				executionDecision.nextState.promotionFingerprint = 0U;
			} else {
				AnimationExecutionModeRequest request;
				request.fixedTick = compact.inputs[compactIndex].tick;
				request.requestedPolicy = cohort.requestedPolicy;
				request.headless = headlessExecutionRequired;
				request.gpu = gpu;
				request.continuity = continuity;
				const AnimationExecutionModeDecision resolved =
					ResolveAnimationExecutionMode(request, previous);
				executionDecision.effectiveMode = resolved.effectiveMode;
				executionDecision.reason = resolved.reason;
				executionDecision.nextState = resolved.nextState;
			}
			if(!TryAdvanceGeneration(compact.executionModeGenerations[compactIndex],
				compact.executionDecisions[compactIndex].nextState.effectiveMode
					!= previous.effectiveMode
				|| compact.executionDecisions[compactIndex].nextState.effectiveReason
					!= previous.effectiveReason,
				compact.executionModeGenerations[compactIndex])) {
				status = AnimationStatus::RangeOverflow;
				planningFailure = AnimationPlanRejectionReason::GenerationOverflow;
			}
			if(status != AnimationStatus::Ok) {
				compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
				return;
			}
			if(compact.executionDecisions[compactIndex].effectiveMode
				!= AnimationEffectiveExecutionMode::GpuVisual) {
				planningFailure = AnimationPlanRejectionReason::ExecutionModeNotGpuVisual;
				compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
				return;
			}
			GpuAnimationVisualInput visual = cohort.fastBinding->visual;
			const CompactGpuVisualCaptures::InputIdentity& input =
				compact.inputs[compactIndex];
			visual.stableInstanceId = (static_cast<std::uint64_t>(input.handle.slotIndex) << 32U)
				| input.handle.generation;
			visual.stableSubmissionOrdinal = input.handle.slotIndex;
			visual.jointCount = cohort.jointCount;
			visual.evaluationParameterFingerprint = MixWorkFingerprint(
				compact.parameterGenerations[compactIndex], compact.stateGenerations[compactIndex]);
			visual.sampleTick = static_cast<std::uint32_t>(std::max<AnimationFixedTick>(0, playback.resolvedTick));
			visual.generations.instanceSlot = input.handle.slotIndex;
			visual.generations.instanceGeneration = input.handle.generation;
			visual.generations.componentGeneration = input.componentGeneration;
			visual.generations.workGeneration = compact.candidatePoseGenerations[compactIndex];
			visual.generations.programGeneration = cohort.programGeneration;
			visual.generations.controllerGeneration = cohort.programGeneration;
			visual.generations.clipGeneration = cohort.clipGeneration;
			visual.generations.skeletonGeneration = cohort.skeletonGeneration;
			if(!StampGpuVisualFrameIdentity(
				visual, playback.resolvedTick, input.tick)) {
				planningFailure = AnimationPlanRejectionReason::FrameIdentityInvalid;
				compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
				return;
			}
			// A worker range commonly contains many members of one immutable cohort.
			// Once one member proves the shared program shape and resident tables, later
			// members still lower their own identity and generation fields but avoid
			// rescanning the same immutable program and payload vectors.
			const bool sharedInputsValidated = validatedCohort == &cohort
				&& validatedStageMask != 0U;
			GpuAnimationWorkRequest probe{};
			// Shared validation already proved all cohort-owned program, resource, and
			// layout fields. Keep the inexpensive per-instance identity/generation proof,
			// then let presentation lower the complete immutable input once at its normal
			// boundary. A one-member cohort still takes the full validation path below.
			const bool instanceIdentityValid = visual.stableInstanceId != 0U
				&& IsGpuAnimationRenderIdentityValid(visual.renderIdentity)
				&& IsGpuAnimationGenerationRecordValid(visual.generations);
			GpuAnimationLoweringStatus loweringStatus = instanceIdentityValid
				? GpuAnimationLoweringStatus::Ok
				: GpuAnimationLoweringStatus::StaleGeneration;
			if(!sharedInputsValidated && instanceIdentityValid) {
				// Only the first member of an immutable cohort needs the complete
				// program/payload envelope. Later members validate their changing lane
				// above and reuse the cohort proof without touching shared ownership.
				GpuAnimationLoweringInput lowering;
				lowering.program.program = cohort.program;
				lowering.program.programContentId = cohort.program->stableHash;
				lowering.program.programResourceIndex = cohort.fastBinding->programResourceIndex;
				lowering.visual = visual;
				lowering.effectiveMode = compact.executionDecisions[compactIndex].effectiveMode;
				lowering.residentPayloads = cohort.fastBinding->residentPayloads;
				lowering.allowSharedVisualEvaluation = true;
				loweringStatus = LowerGpuAnimationWorkRequest(lowering, probe);
			}
			if(sharedInputsValidated) {
				probe.batchKey.stageMask = validatedStageMask;
			}
			if(loweringStatus != GpuAnimationLoweringStatus::Ok) {
				planningFailure = AnimationPlanRejectionReason::GpuLoweringRejected;
				compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
				return;
			}
			if(!sharedInputsValidated
				&& !ValidateGpuAnimationResidentPayloads(
					probe, *cohort.fastBinding->residentPayloads)) {
				planningFailure = AnimationPlanRejectionReason::ResidentPayloadInvalid;
				compact.processingClasses[compactIndex] = AnimationProcessingClass::Complete;
				return;
			}
			if(!sharedInputsValidated) {
				validatedCohort = &cohort;
				validatedStageMask = probe.batchKey.stageMask;
			}
			compact.stageMasks[compactIndex] = probe.batchKey.stageMask;
			compact.gpuPresentationVisuals[compactIndex] = std::move(visual);
			CompactGpuVisualInput compactInput{};
			compactInput.stableInstanceId = compact.gpuPresentationVisuals[compactIndex].stableInstanceId;
			compactInput.stableSubmissionOrdinal = compact.gpuPresentationVisuals[compactIndex].stableSubmissionOrdinal;
			compactInput.playbackTick = compact.playbacks[compactIndex].AbsoluteTick();
			compactInput.sourcePoseGeneration = compact.candidatePoseGenerations[compactIndex];
			compactInput.generations = {compactInput.stableInstanceId, input.componentGeneration,
				cohort.programGeneration, cohort.clipGeneration,
				frameSceneGeneration, compact.gpuPresentationVisuals[compactIndex].renderIdentity.rendererGeneration,
				compact.authorityGeneration};
			compactInput.eligibilityReason = compact.executionDecisions[compactIndex].reason;
			compactInput.gpuLoweringComplete = true;
			// The protected SoA admission gate already proved these exclusion facts.
			// Copy them into the fail-closed value record so classification consumes
			// the captured proof instead of mistaking safe direct clips for unknown work.
			compactInput.hasAuthoritativeConsumer = false;
			compactInput.hasControllerOrTransition = false;
			compactInput.hasAdvancedFeature = false;
			// Direct clips have an explicit, immutable bind-pose origin. They do not
			// need to wait one CPU frame merely to manufacture a previous execution
			// mode: the copied playback and proven direct binding fully determine the
			// first GPU packet. This avoids alternating complete/compact updates when
			// Play Mode begins or presentation completes one scheduler frame later.
			compactInput.hasContinuitySource = previous.initialized
				|| (cohort.program != nullptr && cohort.fastBinding != nullptr);
			compact.compactInputs[compactIndex] = compactInput;
			compact.processingClasses[compactIndex] = ClassifyAnimationProcessing(compactInput);
			if(compact.processingClasses[compactIndex]
				!= AnimationProcessingClass::CompactGpuVisual) {
				planningFailure = AnimationPlanRejectionReason::CompactClassificationRejected;
			}
		}

		void PlanRange(
			std::size_t begin,
			std::size_t end,
			const AnimatorPlaybackTimeInput& timeInput) noexcept
		{
			if(PollFrameCancellation()) return;
			const auto rangePlanningStart = std::chrono::steady_clock::now();
			const std::size_t boundedEnd = std::min(end, capturedInputWorkIndices.size());
			std::size_t locallyPlanned = 0U;
			std::size_t locallyCompactPlanned = 0U;
			std::size_t locallyCompletePlanned = 0U;
			const CompactGpuVisualCohort* validatedCompactCohort = nullptr;
			std::uint32_t validatedCompactStageMask = 0U;
			// Identical prefab instances share their immutable program and resident
			// payload objects. Validate each exact shape once per worker range instead of
			// reparsing the same program and morph tables for every crowd member.
			const EvaluationProgram* validatedProgram = nullptr;
			std::uint32_t validatedProgramStageMask = 0U;
			bool validatedStageAllowsProcedural = false;
			bool validatedStageAllowsRagdoll = false;
			std::uint32_t validatedStageMorphCount = 0U;
			struct ResidentValidationKey {
				const GpuAnimationResidentPayloads* payload = nullptr;
				std::uint64_t programContentId = 0U;
				std::uint32_t stageMask = 0U;
				std::uint32_t flags = 0U;
				std::uint32_t programResourceIndex = 0U;
				std::uint32_t maskResourceIndex = 0U;
				std::uint32_t morphResourceIndex = 0U;
				std::uint32_t jointCount = 0U;
				std::uint32_t morphCount = 0U;
				std::uint32_t ragdollTableOffset = 0U;
				AnimationGeneration ragdollResourceGeneration = 0U;
				AnimationGeneration ragdollSkeletonGeneration = 0U;
				AnimationGeneration ragdollComponentGeneration = 0U;

				bool operator==(const ResidentValidationKey&) const noexcept = default;
			};
			std::optional<ResidentValidationKey> residentValidationKey;
			bool residentValidationResult = false;
			for(std::size_t index = begin; index < boundedEnd; ++index) {
				// The lifecycle token is immutable for this worker range. Poll in bounded
				// batches instead of reading the complete multi-boundary latch once per
				// animator; cancellation still stops within at most sixteen items.
				if(((index - begin) & 15U) == 0U && PollFrameCancellation()) return;
				const CapturedInputWorkIndex& captured = capturedInputWorkIndices[index];
				if(captured.compact) {
					PlanCompactCapture(
						captured.storageIndex,
						timeInput,
						validatedCompactCohort,
						validatedCompactStageMask);
					++locallyCompactPlanned;
					++locallyPlanned;
					continue;
				}
				if(captured.storageIndex >= frameWork.size()) return;
				FrameWorkItem& item = frameWork[captured.storageIndex];
				AnimationStatus commandStatus = AnimationStatus::Ok;
				bool stateCommandApplied = false;
				bool parameterCommandApplied = false;
				bool directStateOverrideQueued = false;
				std::vector<StateMachineTriggerEdge> triggerEdges;
				for(std::size_t commandIndex = 0U; commandIndex < capturedCommands.size(); ++commandIndex) {
					const AnimationCommand& command = capturedCommands[commandIndex];
					if(command.target != item.input.handle) continue;
					if(capturedCommandStatuses[commandIndex] != AnimationStatus::NotReady) {
						if(commandStatus == AnimationStatus::Ok)
							commandStatus = capturedCommandStatuses[commandIndex];
						continue;
					}
					if(command.expectedController.IsValid()
						&& command.expectedController != item.input.descriptor.controller) {
						capturedCommandStatuses[commandIndex] = AnimationStatus::StaleHandle;
						if(commandStatus == AnimationStatus::Ok)
							commandStatus = AnimationStatus::StaleHandle;
						continue;
					}
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
					// Capture already copied the last-good graph into this worker-owned
					// candidate. Advance it directly so synchronized crowds reuse retained
					// history capacity instead of allocating and copying a third snapshot.
					const AnimationStatus graphStatus = StepStateMachineTrustedInPlace(
						*item.binding.evaluationProgram,
						item.candidateGraphState,
						graphInput);
					if(graphStatus == AnimationStatus::Ok) {
						stateCommandApplied = stateCommandApplied
							|| item.candidateGraphState != item.capturedGraphState;
						item.binding.stateId = item.candidateGraphState.activeStateId;
						const AnimationStatus clipStatus = SelectControllerClipForState(
							item, item.binding.stateId);
						if(clipStatus != AnimationStatus::Ok) commandStatus = clipStatus;
					} else if(!directStateOverrideQueued) {
						// The in-place evaluator may have advanced part of its private candidate
						// before an allocation failed. Restore the last-good state so fallback
						// publication remains atomic and can never commit a partial graph.
						item.candidateGraphState = item.capturedGraphState;
						commandStatus = graphStatus;
					} else {
						// A direct Play/CrossFade may intentionally recover this graph below,
						// but it must start from the protected last-good snapshot.
						item.candidateGraphState = item.capturedGraphState;
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
				if(item.binding.updatePolicy != AnimatorUpdatePolicy::Manual
					&& !item.restoredFromHistory) {
					switch(item.binding.playback.timePolicy) {
					case AnimatorTimePolicy::Scaled:
						item.physicalEvaluationDurationSeconds = effectiveTime.scaledDeltaSeconds;
						break;
					case AnimatorTimePolicy::Unscaled:
						item.physicalEvaluationDurationSeconds = effectiveTime.unscaledDeltaSeconds;
						break;
					case AnimatorTimePolicy::Fixed:
						item.physicalEvaluationDurationSeconds = effectiveTime.fixedDeltaTicks > 0
							? static_cast<double>(effectiveTime.fixedDeltaTicks)
								/ static_cast<double>(kAnimationFixedTicksPerSecond) : 0.0;
						break;
					}
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
				const AnimatorPlaybackStateSnapshot currentPlaybackState =
					item.playback.Snapshot();
				const bool playbackChanged = stateCommandApplied
					|| currentPlaybackState != item.capturedPlaybackState;
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
				if(frameHasConsumerDeclarations) {
					item.resolvedConsumers = frameConsumerReadView.Resolve(
						item.input.handle, item.binding.jointParents, item.input.tick);
				} else {
					// Reset any retained vector capacity from an earlier diagnostic/gameplay
					// declaration without touching the registry's shared publication pointer.
					item.resolvedConsumers = {};
				}
				const AnimationResolvedConsumerRequirements& declaredConsumers =
					item.resolvedConsumers;
				std::vector<std::uint32_t> mergedAuthoritativeJoints;
				std::span<const std::uint32_t> authoritativeJoints =
					item.binding.authoritativeJointIndices;
				if(declaredConsumers.status == AnimationStatus::Ok
					&& !declaredConsumers.closure.joints.empty()) {
					// The visual-crowd common case has no CPU joint reader, so borrow
					// the immutable authored list without allocating a temporary vector.
					mergedAuthoritativeJoints = item.binding.authoritativeJointIndices;
					mergedAuthoritativeJoints.insert(mergedAuthoritativeJoints.end(),
						declaredConsumers.closure.joints.begin(), declaredConsumers.closure.joints.end());
					std::sort(mergedAuthoritativeJoints.begin(), mergedAuthoritativeJoints.end());
					mergedAuthoritativeJoints.erase(
						std::unique(mergedAuthoritativeJoints.begin(), mergedAuthoritativeJoints.end()),
						mergedAuthoritativeJoints.end());
					authoritativeJoints = mergedAuthoritativeJoints;
				} else if(declaredConsumers.status == AnimationStatus::Ok) {
					// Keep the branch explicit: an empty declaration must preserve the
					// authored span exactly, including its already-validated ordering.
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
				// A policy says how to consume an authored delta; it does not create one.
				// Keeping a root-motion-free clip on GPU Visual avoids evaluating and
				// publishing a duplicate CPU pose for large in-place crowds. Explicit
				// consumers still request CPU authority even when the current clip has no
				// track, while interrupted/cross-fading root motion remains authoritative.
				modeRequest.consumers.rootMotion = RequiresCpuRootMotionAuthority(
					item.rootMotionEnabled ? AnimatorRootMotionPolicy::ApplyToCharacter
						: item.binding.rootMotionPolicy,
					item.binding.clip != nullptr && item.binding.clip->rootMotion.enabled,
					item.transition.capturedSourceRootMotion.has_value(),
					item.transition.ready && item.transition.destination.clip != nullptr
						&& item.transition.destination.clip->rootMotion.enabled,
					declaredConsumers.closure.rootMotion);
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
				// Do not require the published GPU envelope to equal the immediately
				// preceding authority generation. Animation presentation and rendering are
				// pipelined, so a complete immutable envelope may legitimately be one
				// generation ahead or behind this capture. RenderSystem validates the exact
				// scene, mesh, component, skeleton, resource, and renderer identities before
				// accepting that envelope; an unavailable or incompatible envelope therefore
				// remains a safe render fallback without forcing thousands of otherwise
				// eligible visual animators back through complete CPU evaluation.
				modeRequest.continuity.transitionBoundaryTick = item.input.tick;
				modeRequest.continuity.stateTimeTick = item.playback.AbsoluteTick();
				modeRequest.continuity.normalizedControllerTime = static_cast<float>(
					currentPlaybackState.resolvedTick);
				modeRequest.continuity.controllerState = item.binding.stateId;
				modeRequest.continuity.parameterSnapshot = item.candidateParameterGeneration;
				modeRequest.continuity.transition =
					item.candidateGraphState.activeTransitionStableId;
				modeRequest.continuity.synchronizationState = item.candidateStateGeneration;
				modeRequest.continuity.synchronizationPhaseTick =
					currentPlaybackState.resolvedTick;
				const AnimationEventStreamDiagnostics& eventDiagnostics =
					item.eventStream.Diagnostics();
				modeRequest.continuity.eventWatermark = eventDiagnostics.emittedEvents
					^ (eventDiagnostics.duplicateEventsSuppressed << 1U)
					^ (eventDiagnostics.retainedEventEvictions << 2U);
				modeRequest.continuity.rootMotion = item.candidatePoseGeneration
					^ static_cast<std::uint64_t>(item.playback.AbsoluteTick());
				modeRequest.continuity.motionMatch = item.appliedMotionQueryId;
				modeRequest.continuity.ragdollBlend =
					static_cast<std::uint64_t>(item.ragdollGeneration.publicationGeneration)
					^ static_cast<std::uint64_t>(item.ragdollLifecycle);
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
				item.visualEvaluationSuspended = CanSuspendVisualEvaluation(item);
				if(item.planned && (item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::GpuVisual
					|| item.executionDecision.effectiveMode
					== AnimationEffectiveExecutionMode::HybridAuthoritative)) {
					GpuAnimationLoweringInput gpuInput;
					const std::shared_ptr<const AnimatorPlaybackBinding::GpuFastBinding>&
						fastBinding = item.binding.gpuFastBinding;
					gpuInput.program.program = item.binding.evaluationProgram;
					gpuInput.program.programContentId = item.binding.evaluationProgram
						? item.binding.evaluationProgram->stableHash : 0U;
					gpuInput.program.programResourceIndex = fastBinding && fastBinding->IsComplete()
						? fastBinding->programResourceIndex : kGpuAnimationInvalidResourceIndex;
					gpuInput.visual = fastBinding && fastBinding->IsComplete()
						? fastBinding->visual : GpuAnimationVisualInput{};
					if(item.visualEvaluationSuspended) {
						// Suspension is a per-frame generation-lane decision. It must not
						// rewrite immutable bind/hold fallback packet flags, otherwise view
						// changes would rebuild the topology they are intended to preserve.
						// Every GPU stage reads this bit and leaves the last complete pose
						// untouched until the object becomes visible again.
						gpuInput.visual.suspendVisualEvaluation = true;
					}
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
					const bool gpuFrameIdentityRepresentable = StampGpuVisualFrameIdentity(
						gpuInput.visual, currentPlaybackState.resolvedTick, item.input.tick);
					gpuInput.effectiveMode = item.executionDecision.effectiveMode;
					gpuInput.residentPayloads = fastBinding && fastBinding->IsComplete()
						? fastBinding->residentPayloads
						: std::shared_ptr<const GpuAnimationResidentPayloads>{};
					gpuInput.programValidated = validatedProgram != nullptr
						&& validatedProgram == gpuInput.program.program.get();
					const bool stageShapeMatches = gpuInput.programValidated
						&& validatedStageAllowsProcedural
							== gpuInput.visual.allowVisualProcedural
						&& validatedStageAllowsRagdoll
							== gpuInput.visual.allowVisualRagdoll
						&& validatedStageMorphCount == gpuInput.visual.morphCount;
					gpuInput.prevalidatedStageMask = stageShapeMatches
						? validatedProgramStageMask : 0U;

					// Prove the entire immutable lowering input now, before CPU evaluation
					// begins. Missing programs, descriptor indices, or resident bytes must
					// downgrade this copied frame item to CPU instead of letting a later
					// presentation failure discard the only authoritative pose.
					GpuAnimationWorkRequest probe{};
					const GpuAnimationLoweringStatus loweringStatus =
						gpuFrameIdentityRepresentable
							? LowerGpuAnimationWorkRequest(gpuInput, probe)
							: GpuAnimationLoweringStatus::StaleGeneration;
					if(loweringStatus == GpuAnimationLoweringStatus::Ok
						&& !gpuInput.programValidated) {
						validatedProgram = gpuInput.program.program.get();
					}
					if(loweringStatus == GpuAnimationLoweringStatus::Ok
						&& gpuInput.prevalidatedStageMask == 0U) {
						// Optional visual stages are part of the derivation key. A
						// procedurally enabled hero must never inherit a crowd mask.
						validatedProgramStageMask = probe.batchKey.stageMask;
						validatedStageAllowsProcedural =
							gpuInput.visual.allowVisualProcedural;
						validatedStageAllowsRagdoll =
							gpuInput.visual.allowVisualRagdoll;
						validatedStageMorphCount = gpuInput.visual.morphCount;
					}
					const bool ragdollValidation = HasGpuAnimationFlag(
						probe.flags, GpuAnimationWorkFlag::AllowRagdollVisual);
					const ResidentValidationKey candidateValidationKey{
						gpuInput.residentPayloads.get(),
						probe.batchKey.programContentId,
						probe.batchKey.stageMask,
						probe.flags,
						probe.programResourceIndex,
						probe.maskResourceIndex,
						probe.morphResourceIndex,
						probe.jointCount,
						probe.morphCount,
						ragdollValidation ? probe.generations.reserved : 0U,
						ragdollValidation ? probe.generations.resourceGeneration : 0U,
						ragdollValidation ? probe.generations.skeletonGeneration : 0U,
						ragdollValidation ? probe.generations.componentGeneration : 0U};
					if(!residentValidationKey.has_value()
						|| *residentValidationKey != candidateValidationKey) {
						residentValidationKey = candidateValidationKey;
						residentValidationResult = loweringStatus
							== GpuAnimationLoweringStatus::Ok
							&& gpuInput.residentPayloads != nullptr
							&& ValidateGpuAnimationResidentPayloads(
								probe, *gpuInput.residentPayloads);
					}
					const bool residentInputsComplete = residentValidationResult;
					if(loweringStatus == GpuAnimationLoweringStatus::Ok && residentInputsComplete) {
						gpuInput.residentPayloadsValidated = true;
						gpuInput.prevalidatedStageMask = probe.batchKey.stageMask;
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
				CompactGpuVisualInput compactInput;
				compactInput.stableInstanceId =
					(static_cast<std::uint64_t>(item.input.handle.slotIndex) << 32U)
					| static_cast<std::uint64_t>(item.input.handle.generation);
				compactInput.stableSubmissionOrdinal = item.input.handle.slotIndex;
				compactInput.playbackTick = item.playback.AbsoluteTick();
				compactInput.sourcePoseGeneration = item.candidatePoseGeneration;
				compactInput.generations.membershipGeneration = compactInput.stableInstanceId;
				compactInput.generations.capabilityGeneration =
					item.input.descriptor.componentGeneration;
				compactInput.generations.bindingGeneration = item.binding.programGeneration;
				compactInput.generations.resourceGeneration = item.binding.clipGeneration;
				compactInput.generations.sceneGeneration = frameSceneGeneration;
				compactInput.generations.rendererGeneration = item.gpuLowering
					? item.gpuLowering->visual.renderIdentity.rendererGeneration : 0U;
				compactInput.generations.authorityGeneration =
					item.candidateAuthorityPublicationGeneration;
				compactInput.eligibilityReason = item.executionDecision.reason;
				compactInput.gpuLoweringComplete = item.gpuLowering.has_value();
				compactInput.hasAuthoritativeConsumer =
					item.executionDecision.consumerClosure.HasAuthoritativeSubset();
				compactInput.hasControllerOrTransition = item.binding.controllerProgramRequired
					|| item.candidateGraphState.activeTransitionStableId != 0U;
				compactInput.hasAdvancedFeature = item.retargetActive
					|| item.hasMotionCandidate || !item.activeProceduralGoals.empty()
					|| item.ragdollLifecycle != RagdollLifecycleState::Inactive;
				compactInput.hasContinuitySource =
					item.previousExecutionModeState.initialized;
				item.compactGpuVisualInput = compactInput;
				item.processingClass = ClassifyAnimationProcessing(compactInput);
				ResolveUnchangedCacheReuse(item);
				if(item.processingClass == AnimationProcessingClass::CompactGpuVisual) {
					++locallyCompactPlanned;
				} else {
					++locallyCompletePlanned;
				}
				++locallyPlanned;
			}
			if(locallyPlanned != 0U) {
				const std::uint64_t rangePlanningNanoseconds = static_cast<std::uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - rangePlanningStart).count());
				// Planning telemetry describes worker-owned CPU time; it must not change
				// the cost being measured. Attribute one range timer proportionally when
				// a partition contains both classes, replacing two clock reads and one
				// contended atomic operation for every animator with at most two atomics.
				const std::uint64_t compactRangeNanoseconds = static_cast<std::uint64_t>(
					static_cast<long double>(rangePlanningNanoseconds)
						* static_cast<long double>(locallyCompactPlanned)
						/ static_cast<long double>(locallyPlanned));
				const std::uint64_t completeRangeNanoseconds =
					rangePlanningNanoseconds - compactRangeNanoseconds;
				if(compactRangeNanoseconds != 0U) {
					compactPlanningNanoseconds.fetch_add(
						compactRangeNanoseconds, std::memory_order_relaxed);
				}
				if(completeRangeNanoseconds != 0U) {
					completePlanningNanoseconds.fetch_add(
						completeRangeNanoseconds, std::memory_order_relaxed);
				}
			}
			// One partition owns this complete contiguous range, so a single atomic
			// publication replaces cache-line contention from every animator.
			if(locallyPlanned != 0U) {
				plannedAnimatorCount.fetch_add(
					locallyPlanned, std::memory_order_acq_rel);
			}
		}

		bool FinalizeAnimationWorkPlan() noexcept
		{
			workPlanRejectionReason = AnimationPlanRejectionReason::None;
			workPlanRejectionItemIndex = std::numeric_limits<std::size_t>::max();
			if(capturedInputWorkIndices.empty() || PollFrameCancellation()
				|| plannedAnimatorCount.load(std::memory_order_acquire)
					!= capturedInputWorkIndices.size()) {
				workPlanRejectionReason = AnimationPlanRejectionReason::PlanningIncomplete;
				return false;
			}
			std::size_t estimatedPoseBytes = 0U;
			try {
				capturedWorkItems.clear();
				compactGpuVisualWorkItems.clear();
				capturedWorkItems.reserve(frameWork.size());
				compactGpuVisualWorkItems.reserve(frameWork.size());
				for(std::size_t originalIndex = 0U; originalIndex < frameWork.size(); ++originalIndex) {
					const FrameWorkItem& item = frameWork[originalIndex];
					if(item.compactCaptureOnly
						&& item.processingClass != AnimationProcessingClass::CompactGpuVisual) {
						workPlanRejectionReason =
							AnimationPlanRejectionReason::CompactClassificationRejected;
						workPlanRejectionItemIndex = originalIndex;
						workPlan = {};
						workPlan.status = AnimationStatus::Rejected;
						workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
						return false;
					}
					if(item.processingClass == AnimationProcessingClass::CompactGpuVisual) {
						// Classification is a complete immutable proof. Recheck the two facts
						// that bind it to this publication record before omitting the general
						// evaluator; any inconsistency fails closed into the complete path.
						if(!item.gpuLowering || !item.compactGpuVisualInput.IsComplete()) {
							workPlanRejectionReason =
								AnimationPlanRejectionReason::CompactInputIncomplete;
							workPlanRejectionItemIndex = originalIndex;
							workPlan = {};
							workPlan.status = AnimationStatus::Rejected;
							workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
							return false;
						}
						compactGpuVisualWorkItems.append(
							item.capturedInputIndex,
							item.input.handle,
							item.candidateAuthorityPublicationGeneration,
							item.compactGpuVisualInput);
						continue;
					}
					AnimationWorkItem workItem;
					workItem.animator = item.input.handle;
					workItem.compatibility.programGeneration = item.binding.programGeneration;
					workItem.compatibility.skeletonGeneration = item.binding.skeletonGeneration;
					workItem.compatibility.clipBindingGeneration = item.binding.clipGeneration;
					workItem.compatibility.outputRequirementsFingerprint =
						MixWorkFingerprint(
							item.bindingOutputRequirementsFingerprint,
							item.resolvedConsumers.fingerprint);
					workItem.compatibility.resourceLayoutFingerprint =
						MixWorkFingerprint(
							item.bindingResourceLayoutFingerprint,
							item.retargetActive ? 1U : 0U);
					workItem.compatibility.effectiveMode = item.executionDecision.effectiveMode;
					workItem.stablePublicationOrder = item.capturedInputIndex;
					workItem.sourceGeneration = item.candidateAuthorityPublicationGeneration;
					workItem.scratchBytes = sizeof(AnimationFrameScratchRecord);
					capturedWorkItems.push_back(workItem);
					const bool createsCpuPose = !item.cacheReused
						&& !item.visualEvaluationSuspended
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
				for(std::size_t compactIndex = 0U;
					compactIndex < compactGpuVisualCaptures.size(); ++compactIndex) {
					if(compactGpuVisualCaptures.processingClasses[compactIndex]
						!= AnimationProcessingClass::CompactGpuVisual
						|| !compactGpuVisualCaptures.compactInputs[compactIndex].IsComplete()) {
						workPlanRejectionReason =
							compactGpuVisualCaptures.planningFailures[compactIndex];
						if(workPlanRejectionReason == AnimationPlanRejectionReason::None) {
							workPlanRejectionReason =
								AnimationPlanRejectionReason::CompactInputIncomplete;
						}
						workPlanRejectionItemIndex =
							compactGpuVisualCaptures.inputIndices[compactIndex];
						workPlan = {};
						workPlan.status = AnimationStatus::Rejected;
						workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
						return false;
					}
					compactGpuVisualWorkItems.append(
						compactGpuVisualCaptures.inputIndices[compactIndex],
						compactGpuVisualCaptures.inputs[compactIndex].handle,
						compactGpuVisualCaptures.authorityGeneration,
						compactGpuVisualCaptures.compactInputs[compactIndex]);
				}
			} catch(const std::bad_alloc&) {
				workPlanRejectionReason = AnimationPlanRejectionReason::AllocationFailure;
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
			if(workPlan.status != AnimationStatus::Ok || PollFrameCancellation()) {
				workPlanRejectionReason = AnimationPlanRejectionReason::SchedulerRejected;
				return false;
			}
			// AnimationWorkScheduler::BuildPlan already validates the complete plan.
			// Retain that proof instead of repeating the O(animator-count) scan in
			// every worker partition and again at publication.
			workPlanValidated = true;

			AnimationMemoryLease stagedLease;
			AnimationMemoryLease stagedPoseLease;
			const std::uint64_t planAuthorityGeneration = !compactGpuVisualCaptures.empty()
				? compactGpuVisualCaptures.authorityGeneration
				: frameWork.front().candidateAuthorityPublicationGeneration;
			if(workPlan.totalScratchBytes != 0U) {
				// A frame made entirely of CompactGpuVisual records has no generic
				// evaluator, ranges, or scratch storage. The scheduler deliberately
				// represents that as a valid Omitted plan, while the memory controller
				// correctly rejects zero-byte reservations. Reserve only when generic
				// work actually owns scratch bytes so compact packets can publish their
				// current playback tick through the separate compact completion path.
				const AnimationMemoryReservationRequest memoryRequest{
					AnimationMemoryClass::ScratchPool,
					workPlan.totalScratchBytes,
					planAuthorityGeneration,
					AnimationMemoryRetention::Required,
					0U};
				const AnimationMemoryStatus memoryStatus = memoryController.TryReserve(
					memoryRequest, stagedLease);
				if(memoryStatus != AnimationMemoryStatus::Reserved) {
					workPlanRejectionReason =
						AnimationPlanRejectionReason::MemoryReservationRejected;
					workPlan.status = memoryStatus == AnimationMemoryStatus::BudgetExceeded
						|| memoryStatus == AnimationMemoryStatus::ReservationTableFull
						? AnimationStatus::HardLimitExceeded
						: AnimationStatus::InternalError;
					workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
			}
			if(estimatedPoseBytes != 0U) {
				const AnimationMemoryReservationRequest poseMemoryRequest{
					AnimationMemoryClass::PosePool,
					estimatedPoseBytes,
					planAuthorityGeneration,
					AnimationMemoryRetention::Required,
					0U};
				const AnimationMemoryStatus poseMemoryStatus = memoryController.TryReserve(
					poseMemoryRequest, stagedPoseLease);
				if(poseMemoryStatus != AnimationMemoryStatus::Reserved) {
					workPlanRejectionReason =
						AnimationPlanRejectionReason::MemoryReservationRejected;
					workPlan.status = poseMemoryStatus == AnimationMemoryStatus::BudgetExceeded
						|| poseMemoryStatus == AnimationMemoryStatus::ReservationTableFull
						? AnimationStatus::HardLimitExceeded
						: AnimationStatus::InternalError;
					workPlan.executionPolicy = AnimationWorkExecutionPolicy::Rejected;
					return false;
				}
			}

			try {
				// Frame scratch and completion storage have no observable state until
				// protected publication succeeds. Resize them in place so a steady crowd
				// reuses capacity instead of allocating two O(animator-count) arrays per
				// frame; a failure still rejects the whole unpublished plan atomically.
				frameScratch.resize(workPlan.totalScratchBytes);
				workCompletions.clear();
				workCompletions.resize(workPlan.items.size());
				compactGpuVisualCompletions.clear();
				compactGpuVisualCompletions.resize(compactGpuVisualWorkItems.size());
				auto stagedRangeLedger = std::make_unique<AnimationWorkRangeLedger>(workPlan);
				std::shared_ptr<const AnimationMemoryLease> stagedPoseReservation;
				if(stagedPoseLease.IsActive()) {
					stagedPoseReservation =
						std::make_shared<AnimationMemoryLease>(std::move(stagedPoseLease));
				}
				// The scheduler retains the checked scratch reservation for evaluators that
				// need it, while this typed path publishes directly into workCompletions.
				// Avoid initializing a second terminal-status record that no consumer reads.
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
			request.prior = {item.fallbackGenerations, item.priorPublication != nullptr,
				item.priorPublication != nullptr, item.priorPublication != nullptr};
			request.bindPoseAvailable = poseUsable && item.binding.skeleton;
			item.fallbackDecision = SelectAnimationFallback(request);

			switch(item.fallbackDecision.source) {
			case AnimationFallbackSource::Current:
			case AnimationFallbackSource::CpuDeterministic:
			case AnimationFallbackSource::BindPose:
				item.selectedPublication = item.publication;
				break;
			case AnimationFallbackSource::HeldPriorGeneration:
				item.selectedPublication = item.priorPublication;
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

		static std::pair<std::uint64_t, std::uint64_t>
		BuildVisualPoseContentIdentity(const PoseBuffer& pose) noexcept
		{
			// Hash only model-space deformation inputs. Local transforms and gameplay
			// curves do not change the renderer palette once model transforms and morph
			// values are fixed. Field-wise mixing avoids hashing structure padding.
			std::uint64_t low = 1469598103934665603ULL;
			std::uint64_t high = 0xd6e8feb86659fd93ULL;
			const auto mix = [&low, &high](std::uint64_t value) noexcept {
				for(std::size_t byte = 0U; byte < sizeof(value); ++byte) {
					const std::uint8_t lane = static_cast<std::uint8_t>(
						(value >> (byte * 8U)) & 0xffU);
					low = (low ^ lane) * 1099511628211ULL;
					high ^= static_cast<std::uint64_t>(lane)
						+ 0x9e3779b97f4a7c15ULL + (high << 6U) + (high >> 2U);
				}
			};
			const auto mixFloat = [&mix](float value) noexcept {
				std::uint32_t bits = 0U;
				std::memcpy(&bits, &value, sizeof(bits));
				mix(bits);
			};
			mix(pose.ModelTransforms().size());
			for(const AnimationTransform& transform : pose.ModelTransforms()) {
				mixFloat(transform.translation.x);
				mixFloat(transform.translation.y);
				mixFloat(transform.translation.z);
				mixFloat(transform.rotation.x);
				mixFloat(transform.rotation.y);
				mixFloat(transform.rotation.z);
				mixFloat(transform.rotation.w);
				mixFloat(transform.scale.x);
				mixFloat(transform.scale.y);
				mixFloat(transform.scale.z);
			}
			mix(pose.MorphValues().size());
			for(const AnimationNamedFloatValue& morph : pose.MorphValues()) {
				mix(morph.stableId);
				mixFloat(morph.value);
			}
			return {
				low == 0U ? 1U : low,
				high == 0U ? 1U : high};
		}

		void PublishEvaluatedCpuPose(FrameWorkItem& item) noexcept
		{
			// The pose channels are already sealed before this helper runs. Keep the
			// instance-specific snapshot and ownership envelope separate so a reused
			// visual pose can never inherit another animator's handle, object, or
			// component generation.
			try {
				const std::shared_ptr<const PoseBuffer> publishedPose = item.pose
					? std::shared_ptr<const PoseBuffer>(item.pose)
					: item.retainedVisualPose;
				if(!publishedPose) {
					item.evaluationStatus = AnimationStatus::InternalError;
					item.evaluated = true;
					PrepareFallbackSelection(item);
					return;
				}
				if(item.visualPoseContentIdLow == 0U
					|| item.visualPoseContentIdHigh == 0U) {
					std::tie(
						item.visualPoseContentIdLow,
						item.visualPoseContentIdHigh) =
						BuildVisualPoseContentIdentity(*publishedPose);
				}
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
						layerIndex < item.binding.evaluationProgram->layers.size();
						++layerIndex) {
						const EvaluationLayer& layer =
							item.binding.evaluationProgram->layers[layerIndex];
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
				snapshot.pose = publishedPose;
				auto immutableSnapshot =
					std::make_shared<const AnimationSnapshot>(std::move(snapshot));
				AnimationAuthorityRow published;
				published.key = {item.input.handle.slotIndex, item.input.handle.generation,
					runtimeSessionEpoch.load(std::memory_order_acquire)};
				published.ownerObjectId = item.input.descriptor.ownerObjectId;
				published.objectGeneration = item.sourceObjectGeneration;
				published.componentGeneration = item.input.descriptor.componentGeneration;
				published.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
				published.sourceObjectWorldTransformCaptured =
					item.sourceObjectWorldTransformCaptured;
				published.authorityPublicationGeneration =
					item.candidateAuthorityPublicationGeneration;
				published.visualPoseContentIdLow = item.visualPoseContentIdLow;
				published.visualPoseContentIdHigh = item.visualPoseContentIdHigh;
				published.controller = item.input.descriptor.controller;
				published.jointStableIds = item.binding.jointStableIds;
				if(item.binding.clip) {
					published.clip = {
						item.binding.clip->assetId, item.binding.clipGeneration};
				}
				if(item.binding.skeleton) {
					published.skeleton = {
						item.binding.skeleton->assetId, item.binding.skeletonGeneration};
					published.skeletonAsset = item.binding.skeleton;
					published.skeletonCompatibilityFingerprint =
						item.binding.skeleton->compatibilityFingerprint;
				}
				published.status = item.evaluationStatus;
				published.requestedExecutionPolicy =
					item.binding.requestedExecutionPolicy;
				published.effectiveExecutionMode =
					item.executionDecision.effectiveMode;
				published.rootMotionPolicy = item.binding.rootMotionPolicy;
				published.updatePolicy = item.binding.updatePolicy;
				published.eventPolicy = item.binding.eventPolicy;
				published.cullingPolicy = item.binding.cullingPolicy;
				published.safeFallback = item.safePose;
				published.streamedClipPayloadsActive =
					item.binding.streamedSegmentPayloads != nullptr;
				published.poseMemoryReservation = item.retainedVisualPose
					&& item.priorPublication
					? item.priorPublication->poseMemoryReservation
					: framePoseReservation;
				published.snapshot = std::move(immutableSnapshot);
				item.publication = std::make_shared<const AnimationAuthorityRow>(
					std::move(published));
				item.evaluated = true;
				PrepareFallbackSelection(item);
			} catch(const std::bad_alloc&) {
				workPlanRejectionReason = AnimationPlanRejectionReason::AllocationFailure;
				item.evaluationStatus = AnimationStatus::InternalError;
				item.evaluated = true;
				PrepareFallbackSelection(item);
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
			if(item.eventStateCaptured
				&& item.dependencyStatus == AnimationStatus::Ok
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
				const auto rootSourceFingerprint = [&item](
					AnimationGeneration clipGeneration,
					std::uint64_t clipContentFingerprint) noexcept {
					// This is a source/graph identity, not the authority publication
					// generation that advances every frame. Hot reload changes it even
					// when the animator handle remains unchanged.
					std::uint64_t hash = 14695981039346656037ULL;
					for(const std::uint64_t value : {
						static_cast<std::uint64_t>(item.binding.programGeneration),
						static_cast<std::uint64_t>(clipGeneration), clipContentFingerprint}) {
						hash ^= value;
						hash *= 1099511628211ULL;
					}
					return hash == 0U ? 1U : hash;
				};
				auto extractClipRootMotion = [&item, &evaluator](
					const AnimationClipAsset* clip,
					const AnimationClipView& view,
					const AnimatorPlayback& playback,
					const AnimatorPlaybackAdvanceResult& advance,
					std::uint64_t stateId,
					std::uint64_t sourceGeneration,
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
					extraction.policy = item.rootMotionEnabled
						? AnimatorRootMotionPolicy::ApplyToCharacter
						: item.binding.rootMotionPolicy;
					extraction.sourceGeneration = sourceGeneration;
					extraction.evaluationDurationSeconds =
						item.physicalEvaluationDurationSeconds;
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
					rootSourceFingerprint(item.binding.clipGeneration,
						item.binding.clipDeterministicContentFingerprint),
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
					sourceRootMotion->policy = item.rootMotionEnabled
						? AnimatorRootMotionPolicy::ApplyToCharacter
						: item.binding.rootMotionPolicy;
					sourceRootMotion->sourceGeneration = rootSourceFingerprint(
						item.binding.clipGeneration,
						item.binding.clipDeterministicContentFingerprint);
					sourceRootMotion->evaluationDurationSeconds =
						item.physicalEvaluationDurationSeconds;
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
						rootSourceFingerprint(item.transition.destination.clipGeneration,
							item.transition.destination.deterministicContentFingerprint),
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
							.policy = item.rootMotionEnabled
								? AnimatorRootMotionPolicy::ApplyToCharacter
								: item.binding.rootMotionPolicy};
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
						blended.sourceGeneration = source.sourceGeneration
							^ (destination.sourceGeneration + 0x9e3779b97f4a7c15ULL
								+ (source.sourceGeneration << 6U)
								+ (source.sourceGeneration >> 2U));
						if(blended.sourceGeneration == 0U) blended.sourceGeneration = 1U;
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
			if(item.visualEvaluationSuspended && item.priorPublication
				&& item.priorPublication->snapshot
				&& item.priorPublication->snapshot->pose
				&& item.executionDecision.effectiveMode
					!= AnimationEffectiveExecutionMode::GpuVisual) {
				// Playback/controller state advanced during planning, but a purely visual
				// off-screen animator may publish the prior immutable pose without decoding
				// its clip. Reappearance evaluates the current tick, so no time is lost.
				item.retainedVisualPose = item.priorPublication->snapshot->pose;
				item.visualPoseContentIdLow = item.priorPublication->visualPoseContentIdLow;
				item.visualPoseContentIdHigh = item.priorPublication->visualPoseContentIdHigh;
				item.safePose = item.priorPublication->safeFallback;
				item.evaluationStatus = AnimationStatus::Ok;
				PublishEvaluatedCpuPose(item);
				return;
			}
			PublishEvaluatedCpuPose(item);
		}

		static bool CanShareCpuVisualPose(
			const FrameWorkItem& source,
			const FrameWorkItem& destination) noexcept
		{
			// Same-frame sharing is deliberately narrower than ordinary CPU fallback.
			// Anything that can publish per-instance authority or mutate evaluator state
			// keeps its own evaluation. This leaves synchronized crowd visuals as the
			// useful fast path without aliasing events, root motion, gameplay curves,
			// procedural results, physics, rollback, or diagnostics observations.
			const auto hasNonShareableAuthority =
				[](const AnimationConsumerClosure& closure) noexcept {
					// Root-motion policy selects CPU authority even for clips that contain
					// no root-motion track. The clip checks below prove that special case
					// has no per-instance output; every other authority request remains a
					// hard barrier to sharing.
					return !closure.joints.empty()
						|| closure.curves
						|| closure.morphs
						|| closure.events
						|| closure.contacts
						|| closure.proceduralResults
						|| closure.physics
						|| closure.attachments
						|| closure.networking
						|| closure.rollback
						|| closure.replay
						|| closure.recording
						|| closure.automation
						|| closure.diagnostics
						|| closure.completePose;
				};
			if(!source.evaluated || source.evaluationStatus != AnimationStatus::Ok
				|| source.safePose || source.pose == nullptr
				|| !source.pose->IsSealed()
				|| source.pose->ValidateForPublication() != AnimationStatus::Ok
				|| source.pose->Events().size() != 0U
				|| source.pose->RootMotionDelta().has_value()
				|| !destination.planned || destination.cacheReused
				|| destination.evaluationStatus != AnimationStatus::Ok
				|| source.executionDecision.effectiveMode
					!= AnimationEffectiveExecutionMode::CpuDeterministicFallback
				|| destination.executionDecision.effectiveMode
					!= AnimationEffectiveExecutionMode::CpuDeterministicFallback
				|| source.resolvedConsumers.deterministic
				|| destination.resolvedConsumers.deterministic
				|| hasNonShareableAuthority(
					source.executionDecision.consumerClosure)
				|| hasNonShareableAuthority(
					destination.executionDecision.consumerClosure)
				|| source.binding.clip == nullptr || destination.binding.clip == nullptr
				|| source.binding.clip->rootMotion.enabled
				|| destination.binding.clip->rootMotion.enabled
				|| !source.binding.clip->events.empty()
				|| !destination.binding.clip->events.empty()
				|| source.candidateGraphState.activeTransitionStableId != 0U
				|| destination.candidateGraphState.activeTransitionStableId != 0U
				|| source.retargetActive || destination.retargetActive
				|| source.sampledSourceSkeleton || destination.sampledSourceSkeleton
				|| !source.activeProceduralGoals.empty()
				|| !destination.activeProceduralGoals.empty()
				|| source.ragdollLifecycle != RagdollLifecycleState::Inactive
				|| destination.ragdollLifecycle != RagdollLifecycleState::Inactive
				|| !source.pendingMotionQueries.empty()
				|| !destination.pendingMotionQueries.empty()
				|| !source.pendingMotionQueriesV2.empty()
				|| !destination.pendingMotionQueriesV2.empty()
				|| source.activeMotionDatabaseGeneration != 0U
				|| destination.activeMotionDatabaseGeneration != 0U) {
				return false;
			}

			const bool sameClip = source.binding.clip->assetId
					== destination.binding.clip->assetId
				&& source.binding.clipGeneration
					== destination.binding.clipGeneration
				&& source.binding.clip->compatibilityFingerprint
					== destination.binding.clip->compatibilityFingerprint;
			const bool sameSkeleton = source.binding.skeleton
				&& destination.binding.skeleton
				&& source.binding.skeleton->assetId
					== destination.binding.skeleton->assetId
				&& source.binding.skeletonGeneration
					== destination.binding.skeletonGeneration
				&& source.binding.skeleton->compatibilityFingerprint
					== destination.binding.skeleton->compatibilityFingerprint;
			const std::uint64_t sourceProgram = source.binding.evaluationProgram
				? source.binding.evaluationProgram->stableHash : 0U;
			const std::uint64_t destinationProgram =
				destination.binding.evaluationProgram
					? destination.binding.evaluationProgram->stableHash : 0U;
			return sameClip && sameSkeleton
				&& source.binding.programGeneration
					== destination.binding.programGeneration
				&& sourceProgram == destinationProgram
				&& source.binding.stateId == destination.binding.stateId
				&& source.playback.Snapshot() == destination.playback.Snapshot()
				&& source.candidateGraphState == destination.candidateGraphState
				&& source.parameters == destination.parameters
				&& source.layerWeights == destination.layerWeights
				&& source.binding.jointStableIds
					== destination.binding.jointStableIds
				&& source.binding.morphStableIds
					== destination.binding.morphStableIds
				&& source.binding.curveStableIds
					== destination.binding.curveStableIds
				&& source.pose->Layout().jointCount
					== destination.binding.jointStableIds.size()
				&& source.pose->Layout().morphCount
					== destination.binding.morphStableIds.size()
				&& source.pose->Layout().curveCount
					== destination.binding.curveStableIds.size();
		}

		bool TryShareCpuVisualPose(
			const FrameWorkItem& source,
			FrameWorkItem& destination) noexcept
		{
			if(!CanShareCpuVisualPose(source, destination)) return false;
			try {
				auto pose = std::make_shared<PoseBuffer>();
				AnimationPoseGeneration generation;
				generation.animator = destination.input.handle;
				generation.poseGeneration = destination.candidatePoseGeneration;
				generation.skeleton = {
					destination.binding.skeleton->assetId,
					destination.binding.skeletonGeneration};
				generation.programGeneration =
					destination.binding.programGeneration;
				generation.tick = frameTick;
				// The PoseBuffer owns the per-animator generation but aliases only the
				// leader's already-sealed visual payload. This removes both repeated
				// evaluation and the former local/model/palette memcpy for synchronized
				// crowds while retaining a distinct publication identity per animator.
				if(pose->ShareSealedVisualPayloadFrom(*source.pose, generation)
					!= AnimationStatus::Ok) return false;

				destination.pose = std::move(pose);
				destination.visualPoseContentIdLow = source.visualPoseContentIdLow;
				destination.visualPoseContentIdHigh = source.visualPoseContentIdHigh;
				destination.safePose = false;
				destination.evaluationStatus = AnimationStatus::Ok;
				PublishEvaluatedCpuPose(destination);
				if(!destination.evaluated
					|| destination.fallbackDecision.source
						!= AnimationFallbackSource::Current) {
					return false;
				}
				sharedPoseAnimatorCount.fetch_add(1U, std::memory_order_acq_rel);
				return true;
			} catch(const std::bad_alloc&) {
				return false;
			}
		}

		void EvaluatePartition(
			std::size_t partitionIndex,
			std::size_t partitionCount) noexcept
		{
			if(partitionCount == 0U || partitionIndex >= partitionCount
				|| partitionIndex >= std::numeric_limits<std::uint32_t>::max()
				|| workPlan.status != AnimationStatus::Ok || !workPlanValidated
				|| workRangeLedger == nullptr) return;
			const auto evaluationStart = std::chrono::steady_clock::now();
			// FinalizeAnimationWorkPlan is a single worker dependency between planning
			// and evaluation. Every range therefore observes one completed immutable
			// plan and never blocks a worker on the former shared finalization mutex.
			std::size_t locallyEvaluated = 0U;
			auto publishLocalCount = [this, &locallyEvaluated]() noexcept {
				if(locallyEvaluated != 0U) {
					evaluatedAnimatorCount.fetch_add(
						locallyEvaluated, std::memory_order_acq_rel);
					locallyEvaluated = 0U;
				}
			};
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
					// Keep cancellation responsive without rereading the lifecycle latch
					// for every tiny GPU-visual completion in a large crowd.
					if(((scheduledIndex - range.itemBegin) & 15U) == 0U
						&& PollFrameCancellation()) {
						completion.status = AnimationStatus::Cancelled;
						continue;
					}
					if(scheduled.item.stablePublicationOrder >= capturedInputWorkIndices.size()) {
						completion.status = AnimationStatus::Rejected;
						continue;
					}
					const CapturedInputWorkIndex& captured = capturedInputWorkIndices[
						static_cast<std::size_t>(scheduled.item.stablePublicationOrder)];
					if(captured.compact || captured.storageIndex >= frameWork.size()) {
						completion.status = AnimationStatus::Rejected;
						continue;
					}

					FrameWorkItem& item = frameWork[captured.storageIndex];
					if(item.candidateAuthorityPublicationGeneration
						!= scheduled.item.sourceGeneration) {
						completion.status = AnimationStatus::Rejected;
						continue;
					}
					bool sharedPose = false;
					// Ranges contain one scheduler-proven evaluator/layout contract and
					// execute serially inside one worker. Search only already-completed
					// members of this range, avoiding cross-worker waits while still
					// collapsing synchronized crowds to one evaluator per 64-instance
					// partition.
					const bool mayShareCpuVisualPose = !item.cacheReused
						&& !item.visualEvaluationSuspended
						&& item.executionDecision.effectiveMode
							== AnimationEffectiveExecutionMode::CpuDeterministicFallback;
					if(mayShareCpuVisualPose) {
						for(std::size_t sourceIndex = range.itemBegin;
							!sharedPose && sourceIndex < scheduledIndex;
							++sourceIndex) {
							const AnimationScheduledWorkItem& sourceScheduled =
								workPlan.items[sourceIndex];
							if(sourceScheduled.item.stablePublicationOrder
								>= capturedInputWorkIndices.size()) continue;
							const CapturedInputWorkIndex& sourceCaptured = capturedInputWorkIndices[
								static_cast<std::size_t>(sourceScheduled.item.stablePublicationOrder)];
							if(sourceCaptured.compact || sourceCaptured.storageIndex >= frameWork.size()) continue;
							sharedPose = TryShareCpuVisualPose(
								frameWork[sourceCaptured.storageIndex], item);
						}
					}
					if(!sharedPose && !item.cacheReused
						&& item.executionDecision.effectiveMode
							== AnimationEffectiveExecutionMode::GpuVisual
						&& item.gpuLowering.has_value()
						&& capturedMotionMatchQueriesV2.empty()) {
						// Planning already proved this packet complete. With no CPU query,
						// evaluation has no payload to build; publish the terminal flags
						// directly instead of entering the general CPU evaluator per model.
						item.safePose = false;
						item.evaluated = true;
					} else if(!sharedPose) {
						EvaluateItem(item);
					}
					completion.status = item.evaluated
						? AnimationStatus::Ok
						: AnimationStatus::Rejected;
					if(item.evaluated) ++locallyEvaluated;
				}
				if(!workRangeLedger->Complete(rangeIndex, workerOrdinal)
					&& PollFrameCancellation()) {
					workRangeLedger->CancelUnclaimed();
					publishLocalCount();
					return;
				}
			}
			publishLocalCount();
			completeEvaluationNanoseconds.fetch_add(static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - evaluationStart).count()),
				std::memory_order_acq_rel);
		}

		void EvaluateCompactGpuVisualPartition(
			std::size_t itemBegin,
			std::size_t itemEnd) noexcept
		{
			if(itemBegin > itemEnd || !compactGpuVisualWorkItems.IsConsistent()
				|| !compactGpuVisualCompletions.IsConsistent()
				|| compactGpuVisualCompletions.size() != compactGpuVisualWorkItems.size()) {
				return;
			}
			const auto evaluationStart = std::chrono::steady_clock::now();
			std::size_t locallyCompleted = 0U;
			const std::size_t boundedBegin = std::min(
				itemBegin, compactGpuVisualWorkItems.size());
			const std::size_t boundedEnd = std::min(
				itemEnd, compactGpuVisualWorkItems.size());
			// Scheduler partitions are contiguous. Adjacent completion records therefore
			// stay on one worker instead of being interleaved across cache lines by every
			// worker, which removes false sharing in large compact crowds.
			bool cancellationObserved = false;
			for(std::size_t compactIndex = boundedBegin;
				compactIndex < boundedEnd; ++compactIndex) {
				compactGpuVisualCompletions.compactItemIndices[compactIndex] = compactIndex;
				compactGpuVisualCompletions.animators[compactIndex] =
					compactGpuVisualWorkItems.animators[compactIndex];
				compactGpuVisualCompletions.sourceGenerations[compactIndex] =
					compactGpuVisualWorkItems.sourceGenerations[compactIndex];
				// The cancellation state is monotonic for this frame. Poll once per
				// sixteen compact packets and latch the result, matching the complete
				// evaluator's bounded response without repeated shared-state reads.
				if(!cancellationObserved
					&& ((compactIndex - boundedBegin) & 15U) == 0U) {
					cancellationObserved = PollFrameCancellation();
				}
				if(cancellationObserved) {
					compactGpuVisualCompletions.statuses[compactIndex] =
						AnimationStatus::Cancelled;
					continue;
				}
				// The compact packet has no CPU pose, event, graph, or mutation storage.
				// Its identity is self-contained; protected publication performs the one
				// live-record check before committing the corresponding authority state.
				CompactGpuVisualResult result;
				const CompactGpuVisualInput& input = compactGpuVisualWorkItems.inputs[compactIndex];
				result.stableInstanceId = input.stableInstanceId;
				result.stableSubmissionOrdinal = input.stableSubmissionOrdinal;
				result.sourcePoseGeneration = input.sourcePoseGeneration;
				result.presentationPoseGeneration = input.sourcePoseGeneration;
				result.generations = input.generations;
				result.reason = input.eligibilityReason;
				result.complete = true;
				if(!result.IsPublishableFor(input)) {
					compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Rejected;
					continue;
				}
				compactGpuVisualCompletions.results[compactIndex] = result;
				compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Ok;
				// A command needs broad capture to prepare its private playback state,
				// but can still evaluate as a GPU-only packet. Publish that packet's
				// real completion back to its owned broad row, so the later COW runtime
				// candidate can commit the seek without expecting a CPU evaluator run.
				if(auto* item = CompleteWorkForInputIndex(compactGpuVisualWorkItems.frameWorkIndices[compactIndex])) {
					if(item->input.handle != compactGpuVisualWorkItems.animators[compactIndex]
						|| item->candidateAuthorityPublicationGeneration != compactGpuVisualWorkItems.sourceGenerations[compactIndex]
						|| item->processingClass != AnimationProcessingClass::CompactGpuVisual) {
						compactGpuVisualCompletions.statuses[compactIndex] = AnimationStatus::Rejected; continue;
					}
					item->safePose = false; item->evaluationStatus = AnimationStatus::Ok; item->evaluated = true;
				}
				++locallyCompleted;
			}
			if(locallyCompleted != 0U) {
				compactGpuVisualCompletedCount.fetch_add(locallyCompleted,
					std::memory_order_acq_rel);
				evaluatedAnimatorCount.fetch_add(locallyCompleted, std::memory_order_acq_rel);
			}
			compactEvaluationNanoseconds.fetch_add(static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - evaluationStart).count()),
				std::memory_order_acq_rel);
		}

		[[nodiscard]] bool CanRetainPublishedAuthorityRoot(
			const std::shared_ptr<const AnimationAuthorityFrame>& previous,
			std::uint64_t session, std::uint64_t lifecycle) const noexcept
		{
			// Reusing a root is stricter than reusing a pose. Any command, specialist
			// producer, compact GPU packet, membership difference, or lifecycle change
			// needs a newly sealed authority frame so its sidecars remain exact.
			if(!previous || !previous->IsValid() || previous->sessionGeneration != session
				|| previous->lifecycleGeneration != lifecycle
				|| !capturedCommands.empty() || !capturedProceduralCommands.empty()
				|| !capturedRetargetRequests.empty() || !capturedMotionMatchRequests.empty()
				|| !capturedMotionMatchQueriesV2.empty() || !capturedRagdollRequests.empty()
				|| !capturedDeterminismRequests.empty() || !pendingRootMotionApplications.empty()) {
				return false;
			}
			std::size_t priorRowCount = 0U;
			for(const std::shared_ptr<const AnimationAuthorityPage>& page : *previous->pages) {
				if(!page || page->rows.size() > capturedInputWorkIndices.size() - priorRowCount) {
					return false;
				}
				priorRowCount += page->rows.size();
			}
			if(priorRowCount != capturedInputWorkIndices.size()) return false;
			for(const CapturedInputWorkIndex& captured : capturedInputWorkIndices) {
				if(captured.compact || captured.storageIndex >= frameWork.size()) return false;
				const FrameWorkItem& item = frameWork[captured.storageIndex];
				const bool preservesPrior = item.cacheReused
					|| item.fallbackDecision.source == AnimationFallbackSource::HeldPriorGeneration;
				const AnimationAuthorityRow* prior = previous->Find(item.input.handle);
				if(!preservesPrior || prior == nullptr || item.selectedPublication == nullptr
					|| item.selectedPublication->snapshot == nullptr
					|| item.selectedPublication->poseGeneration != prior->poseGeneration
					|| item.selectedPublication->authorityPublicationGeneration
						!= prior->authorityPublicationGeneration) {
					return false;
				}
			}
			return true;
		}

		// A failed nested batch must reach the scheduler's frame failure boundary,
		// rather than terminate while unwinding an otherwise settled child group.
		[[nodiscard]] bool SealAuthorityFrameCandidate()
		{
			sealedAuthorityCandidate = {};
			sealedAuthorityCandidateReady = false;
			publishedAuthorityCommit = {};
			preparedDiagnostics.reset();
			preparedDetailedDiagnostics.reset();
			preparedPresentationFrame.reset();
			retainsPublishedAuthorityRoot = false;
			if(capturedInputWorkIndices.empty() || frameTick <= 0) return false;
			try {
				std::uint64_t candidateGeneration = 0U;
				std::uint32_t largestSlot = 0U;
				for(const CapturedInputWorkIndex& captured : capturedInputWorkIndices) {
					const AnimatorInstanceHandle handle = captured.compact
						? compactGpuVisualCaptures.inputs[captured.storageIndex].handle
						: frameWork[captured.storageIndex].input.handle;
					if(!handle.IsValid()) return false;
					largestSlot = std::max(largestSlot, handle.slotIndex);
					const std::uint64_t rowGeneration = captured.compact
						? compactGpuVisualCaptures.authorityGeneration
						: frameWork[captured.storageIndex].candidateAuthorityPublicationGeneration;
					if(rowGeneration == 0U || (candidateGeneration != 0U
						&& candidateGeneration != rowGeneration)) return false;
					candidateGeneration = rowGeneration;
				}
				const std::uint64_t session = runtimeSessionEpoch.load(std::memory_order_acquire);
				const std::uint64_t lifecycle = frameLifecycleGenerations.Generation(
					AnimationLifecycleBoundary::Scene);
				if(candidateGeneration == 0U || session == 0U || lifecycle == 0U) return false;
				const std::shared_ptr<const AnimationAuthorityFrame> previous = authorityPublisher.Load();
				if(CanRetainPublishedAuthorityRoot(previous, session, lifecycle)) {
					// No pose, state, sidecar, or membership fact changed. Publishing a new
					// generation here would break hold-last identity and re-expose old events.
					retainsPublishedAuthorityRoot = true;
					return true;
				}
                                const std::size_t pageCount =
                                    static_cast<std::size_t>(largestSlot) /
                                        kAnimationAuthorityPageCapacity128 +
                                    1U;
                                std::vector<
                                    std::shared_ptr<AnimationAuthorityPage>>
									mutablePages;
                                std::vector<bool> pageRetainsPrior(
                                    pageCount, previous != nullptr);
								// Reuse only a complete retired page set. Every page must be
								// owned solely by its pool slot before any row is cleared, so
								// a renderer holding an older authority frame can never observe
								// mutation. The local vector takes a second reference only after
								// that proof succeeds.
								for(std::size_t attempt = 0U;
									attempt < authorityPagePool.size(); ++attempt) {
									const std::size_t slot =
										(authorityPagePoolCursor + attempt)
										% authorityPagePool.size();
									auto& candidatePages = authorityPagePool[slot];
									const bool retired = std::all_of(
										candidatePages.begin(), candidatePages.end(),
										[](const auto& page) {
											return !page || page.use_count() == 1U;
										});
									if(!retired) continue;
									candidatePages.resize(pageCount);
									for(std::size_t pageIndex = 0U;
										pageIndex < pageCount; ++pageIndex) {
										auto& page = candidatePages[pageIndex];
										if(!page) page = std::make_shared<AnimationAuthorityPage>();
										page->firstSlot = static_cast<std::uint32_t>(
											pageIndex * kAnimationAuthorityPageCapacity128);
										page->capacity = static_cast<std::uint32_t>(
											kAnimationAuthorityPageCapacity128);
										page->rows.clear();
										page->changedMasks.assign(2U, 0U);
										page->workerValidatedRowCount = 0U;
									}
									mutablePages = candidatePages;
									authorityPagePoolCursor =
										(slot + 1U) % authorityPagePool.size();
									break;
								}
								if(mutablePages.empty()) {
									mutablePages.resize(pageCount);
									for(std::size_t pageIndex = 0U;
										pageIndex < pageCount; ++pageIndex) {
										auto page = std::make_shared<AnimationAuthorityPage>();
										page->firstSlot = static_cast<std::uint32_t>(
											pageIndex * kAnimationAuthorityPageCapacity128);
										page->capacity = static_cast<std::uint32_t>(
											kAnimationAuthorityPageCapacity128);
										page->changedMasks.assign(2U, 0U);
										mutablePages[pageIndex] = std::move(page);
									}
								}

								std::shared_ptr<AnimationFrameSidecars> sidecars;
								for(std::size_t attempt = 0U;
									attempt < authoritySidecarPool.size(); ++attempt) {
									const std::size_t slot =
										(authoritySidecarPoolCursor + attempt)
										% authoritySidecarPool.size();
									auto& candidate = authoritySidecarPool[slot];
									if(!candidate) candidate =
										std::make_shared<AnimationFrameSidecars>();
									if(candidate.use_count() != 1U) continue;
									candidate->events.clear();
									candidate->rootMotion.clear();
									candidate->callbacks.clear();
									candidate->physics.clear();
									candidate->attachments.clear();
									candidate->procedural.clear();
									candidate->rollback.clear();
									candidate->completions.clear();
									candidate->gpuCandidates.clear();
									candidate->diagnostics.clear();
									candidate->summary = {};
									candidate->workerValidatedAuthorityGeneration = 0U;
									sidecars = candidate;
									authoritySidecarPoolCursor =
										(slot + 1U) % authoritySidecarPool.size();
									break;
								}
								if(!sidecars) sidecars =
									std::make_shared<AnimationFrameSidecars>();
                                bool compactPagesWorkerValidated = false;
                                const bool allCapturedInputsCompact =
                                    capturedInputWorkIndices.size() ==
                                        compactGpuVisualCaptures.size() &&
                                    std::all_of(
                                        capturedInputWorkIndices.begin(),
                                        capturedInputWorkIndices.end(),
                                        [](const CapturedInputWorkIndex
                                               &captured) {
                                          return captured.compact;
                                        });
                                if (allCapturedInputsCompact &&
                                    capturedInputWorkIndices.size() >= 512U &&
                                    availableWorkerCount > 1U) {
                                  TERRAENGINE_PROFILE_SCOPE(
                                      "Animation: Build Compact Publication "
                                      "Roots",
                                      TerraEngine::Profiling::ProfilerModuleId::
                                          CpuEngine);
                                  if (PollFrameCancellation() ||
                                      !workPlanValidated ||
                                      workRangeLedger == nullptr ||
                                      !workRangeLedger->AllCompleted() ||
                                      capturedInputWorkIndices.size() !=
                                          plannedAnimatorCount.load(
                                              std::memory_order_acquire) ||
                                      capturedInputWorkIndices.size() !=
                                          evaluatedAnimatorCount.load(
                                              std::memory_order_acquire) ||
                                      workCompletions.size() !=
                                          workPlan.items.size() ||
                                      compactGpuVisualCompletedCount.load(
                                          std::memory_order_acquire) !=
                                          compactGpuVisualWorkItems.size())
                                    return false;
								  const std::shared_ptr<RuntimeRecordRoot>
									  expectedRuntimeRoot = runtimeRecords.load(
										  std::memory_order_acquire);
								  const std::shared_ptr<const CompactRuntimeStateRoot>
									  expectedCompactRuntimeRoot = compactRuntimeStates.load(
										  std::memory_order_acquire);
								  if (!expectedRuntimeRoot ||
									  expectedRuntimeRoot !=
										  compactRuntimeCaptureRoot ||
									  expectedCompactRuntimeRoot !=
										  compactRuntimeStateCaptureRoot ||
                                      compactGpuVisualCaptures.runtimeRecords
                                              .size() !=
                                          compactGpuVisualCaptures.size())
                                    return false;
								  const std::shared_ptr<RuntimeRecordRoot>
									  nextRuntimeRoot = expectedRuntimeRoot;
								  // Large crowds prepare runtime state in its own settled task group.
								  // Smaller crowds retain the measured single-pass publication path.
								  const bool prepareRuntimeSeparately = capturedInputWorkIndices.size() >= 8192U;
								  auto nextCompactRuntimeRoot = prepareRuntimeSeparately
									  ? std::shared_ptr<CompactRuntimeStateRoot>{}
									  : std::make_shared<CompactRuntimeStateRoot>(expectedRuntimeRoot->size());
                                  auto specialistSidecars = prepareRuntimeSeparately
                                      ? std::shared_ptr<SealedSpecialistSidecarBatch>{}
                                      : std::make_shared<SealedSpecialistSidecarBatch>();
                                  if (!prepareRuntimeSeparately) specialistSidecars->explicitRootMotion =
                                      pendingRootMotionApplications;
                                  // Assign each input one deterministic
                                  // destination before workers run.
                                  // Workers then write distinct
                                  // authority, sidecar, and runtime-root
                                  // elements without locks. Preparing
                                  // both immutable roots in one pass for smaller
                                  // crowds avoids dispatch overhead. Large crowds
                                  // prepare runtime state in its own settled group.
                                  std::vector<std::size_t> rowPositions(
                                      capturedInputWorkIndices.size());
                                  std::vector<std::size_t> pageRowCounts(
                                      pageCount, 0U);
                                  {
                                    TERRAENGINE_PROFILE_SCOPE(
                                        "Animation: Prepare Compact "
                                        "Publication Destinations",
                                        TerraEngine::Profiling::
                                            ProfilerModuleId::CpuEngine);
                                    std::uint32_t previousRuntimeSlot =
                                        TerraEngine::Animation::
                                            kInvalidAnimatorInstanceSlot;
                                    for (std::size_t inputIndex = 0U;
                                         inputIndex <
                                         capturedInputWorkIndices.size();
                                         ++inputIndex) {
                                      const CapturedInputWorkIndex &captured =
                                          capturedInputWorkIndices[inputIndex];
                                      if (captured.storageIndex >=
                                          compactGpuVisualCaptures.size())
                                        return false;
                                      const AnimatorInstanceHandle handle =
                                          compactGpuVisualCaptures
                                              .inputs[captured.storageIndex]
                                              .handle;
                                      if (!handle.IsValid() ||
                                          handle.slotIndex >=
                                              expectedRuntimeRoot->size() ||
                                          handle.slotIndex >=
                                              nextRuntimeRoot->size() ||
                                          (previousRuntimeSlot !=
                                               TerraEngine::Animation::
                                                   kInvalidAnimatorInstanceSlot &&
                                           handle.slotIndex <=
                                               previousRuntimeSlot))
                                        return false;
									  RuntimeRecord *const current =
                                          compactGpuVisualCaptures
                                              .runtimeRecords
												  [captured.storageIndex];
									  const CompactRuntimeState* priorCompactState =
										  FindCompactRuntimeState(
											  expectedCompactRuntimeRoot, handle);
									  const AnimationGeneration priorPoseGeneration = priorCompactState
										  ? priorCompactState->poseGeneration
										  : (current ? current->poseGeneration : 0U);
									  if (current == nullptr ||
									  current->handle != handle ||
									  compactGpuVisualCaptures
											  .candidatePoseGenerations
												  [captured.storageIndex]
										  != priorPoseGeneration + 1U ||
										  compactGpuVisualCaptures.authorityGeneration !=
											  candidateGeneration ||
                                          compactGpuVisualCaptures
                                                  .evaluationStatuses
                                                      [captured.storageIndex] !=
                                              AnimationStatus::Ok)
                                        return false;
                                      const std::size_t pageIndex =
                                          handle.slotIndex /
                                          kAnimationAuthorityPageCapacity128;
                                      if (pageIndex >= mutablePages.size())
                                        return false;
                                      previousRuntimeSlot = handle.slotIndex;
                                      if (!prepareRuntimeSeparately && current->rollbackHistoryActive) {
                                        specialistSidecars
                                            ->rollbackHistoryAnimators
                                            .push_back(handle);
                                      }
                                      rowPositions[inputIndex] =
                                          pageRowCounts[pageIndex]++;
                                      pageRetainsPrior[pageIndex] = false;
                                    }
                                    for (std::size_t pageIndex = 0U;
                                         pageIndex < pageCount; ++pageIndex) {
                                      mutablePages[pageIndex]->rows.resize(
                                          pageRowCounts[pageIndex]);
                                    }
                                  }
                                  sidecars->gpuCandidates.resize(
                                      capturedInputWorkIndices.size());
                                  std::atomic_bool compactRowsValid{true};
                                  const auto buildCompactPublicationRows =
                                      [this, session, candidateGeneration,
                                       &rowPositions, &mutablePages, &sidecars,
									  &specialistSidecars, &nextCompactRuntimeRoot,
									  prepareRuntimeSeparately, &compactRowsValid](
                                          std::size_t begin, std::size_t end) {
                                        TERRAENGINE_PROFILE_SCOPE(
                                            "Animation Task: Build "
                                            "Compact Publication Rows",
                                            TerraEngine::Profiling::
                                                ProfilerModuleId::
                                                    TaskExecution);
                                        for (std::size_t inputIndex = begin;
                                             inputIndex < end; ++inputIndex) {
                                          const std::size_t compactIndex =
                                              capturedInputWorkIndices
                                                  [inputIndex]
                                                      .storageIndex;
                                          const CompactGpuVisualCaptures
                                              &compact =
                                                  compactGpuVisualCaptures;
                                          const AnimatorInstanceHandle handle =
                                              compact.inputs[compactIndex]
                                                  .handle;
                                          AnimationAuthorityRow &row =
                                              mutablePages
                                                  [handle.slotIndex /
                                                   kAnimationAuthorityPageCapacity128]
                                                      ->rows[rowPositions
                                                                 [inputIndex]];
                                          row.key = {handle.slotIndex,
                                                     handle.generation,
                                                     session};
										  row.ownerObjectId =
											  compact.inputs[compactIndex]
												  .ownerObjectId;
										  row.componentGeneration =
											  compact.inputs[compactIndex]
												  .componentGeneration;
                                          row.sourceGeneration =
										  compact.authorityGeneration;
                                          row.poseGeneration =
                                              compact.candidatePoseGenerations
                                                  [compactIndex];
                                          row.tick = frameTick;
                                          row.stateId = 0U;
                                          row.renderBindingGeneration =
                                              compact.executionModeGenerations
                                                  [compactIndex];
                                          row.active = true;
                                          row.safeFallback = false;
                                          AnimationSpecialistRecord &gpu =
                                              sidecars
                                                  ->gpuCandidates[inputIndex];
                                          gpu.key = {handle.slotIndex,
                                                     handle.generation,
                                                     session};
                                          gpu.authorityGeneration =
                                              candidateGeneration;
                                          gpu.kind = 1U;
                                          gpu.status = AnimationStatus::Ok;
                                          if (!row.IsValid() ||
                                              !gpu.IsValidFor(
                                                  candidateGeneration)) {
                                            compactRowsValid.store(
                                                false,
                                                std::memory_order_release);
                                          }

									  if (!prepareRuntimeSeparately) {
									  CompactRuntimeState& publishedState =
										  nextCompactRuntimeRoot->at(handle.slotIndex);
									  publishedState.handle = handle;
									  publishedState.playback = compact.playbacks[compactIndex];
									  publishedState.poseGeneration =
										  compact.candidatePoseGenerations[compactIndex];
									  publishedState.stateGeneration =
										  compact.stateGenerations[compactIndex];
									  publishedState.executionModeGeneration =
										  compact.executionModeGenerations[compactIndex];
									  publishedState.bindingContentGeneration =
										  compact.runtimeRecords[compactIndex]->contentGeneration;
									  publishedState.executionModeState =
										  compact.executionDecisions[compactIndex].nextState;
									  }
                                        }
                                      };
                                  // Compact rows perform only bounded
                                  // scalar copies. Profiling shows that a
                                  // 4,000-row publication completes
                                  // faster on the scheduler worker than
                                  // it does after creating and joining
                                  // several tiny nested tasks. Keep each
                                  // task coarse enough to amortize its
                                  // future, queue, and context-switch
                                  // costs; larger crowds can still use
                                  // multiple workers without
                                  // oversubscribing them.
                                  constexpr std::size_t
                                      kMinimumCompactRowsPerTask = 4096U;
                                  const std::size_t usefulWorkerCount =
                                      std::max<std::size_t>(
                                          1U,
                                          (capturedInputWorkIndices.size() +
                                           kMinimumCompactRowsPerTask - 1U) /
                                              kMinimumCompactRowsPerTask);
                                  const std::size_t workerCount =
                                      std::min<std::size_t>(
                                          availableWorkerCount,
                                          usefulWorkerCount);
                                  const std::size_t chunkSize =
                                      (capturedInputWorkIndices.size() +
                                       workerCount - 1U) /
                                      workerCount;
                                  std::vector<std::future<void>> futures;
                                  futures.reserve(workerCount);
										// This guard dies before every input captured by admitted children.
										TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(futures, TerraEngine::Tasks::NestedTaskGroup::AnimationHomogeneousRows);
                                  // The scheduler worker already
                                  // executing this stage is useful
                                  // capacity. Queue the remaining chunks
                                  // first, then build chunk zero locally
                                  // while they run. This preserves
                                  // disjoint writes and avoids one
                                  // future, queue operation, and context
                                  // handoff on every animated frame.
                                  for (std::size_t workerIndex = 1U;
                                       workerIndex < workerCount;
                                       ++workerIndex) {
                                    const std::size_t begin =
                                        workerIndex * chunkSize;
                                    if (begin >=
                                        capturedInputWorkIndices.size())
                                      break;
                                    const std::size_t end = std::min(
                                        capturedInputWorkIndices.size(),
                                        begin + chunkSize);
                                    futuresSettlement.CreateAndEnqueue(ThreadPool::instance(),
                                            [&buildCompactPublicationRows,
                                             begin, end]() {
                                              buildCompactPublicationRows(begin,
                                                                          end);
                                            });
                                  }
                                  futuresSettlement.RunInline([&] { buildCompactPublicationRows(0U, std::min(capturedInputWorkIndices.size(), chunkSize)); });
                                  futuresSettlement.GetAll();
                                  if (PollFrameCancellation() ||
                                      !compactRowsValid.load(
                                          std::memory_order_acquire))
                                    return false;
                                  compactPagesWorkerValidated = true;
                                  sidecars->workerValidatedAuthorityGeneration =
                                      candidateGeneration;
                                  if (!prepareRuntimeSeparately) {
                                  publishedAuthorityCommit.expectedRuntimeRoot =
                                      expectedRuntimeRoot;
								  publishedAuthorityCommit.nextRuntimeRoot =
									  std::move(nextRuntimeRoot);
								  publishedAuthorityCommit.expectedCompactRuntimeRoot =
									  expectedCompactRuntimeRoot;
								  publishedAuthorityCommit.nextCompactRuntimeRoot =
									  std::move(nextCompactRuntimeRoot);
                                  publishedAuthorityCommit.specialistSidecars =
                                      std::move(specialistSidecars);
                                  }
                                } else if (compactGpuVisualCaptures.size() >=
                                               512U &&
                                           availableWorkerCount > 1U) {
                                  TERRAENGINE_PROFILE_SCOPE(
                                      "Animation: Build Mixed Publication "
                                      "Roots",
                                      TerraEngine::Profiling::ProfilerModuleId::
                                          CpuEngine);
                                  // A few CPU/fallback exceptions must not
                                  // serialize a mostly compact crowd.  Assign
                                  // stable destinations first, then let workers
                                  // fill only the compact rows while this
                                  // worker handles the small exception set.
                                  std::vector<std::size_t> rowPositions(
                                      capturedInputWorkIndices.size());
                                  std::vector<std::size_t> pageRowCounts(
                                      pageCount, 0U);
                                  std::vector<std::size_t> gpuPositions(
                                      capturedInputWorkIndices.size(),
                                      std::numeric_limits<std::size_t>::max());
                                  std::vector<std::uint8_t> rowRetainsPrior(
                                      capturedInputWorkIndices.size(), 0U);
                                  std::uint32_t previousSlot = TerraEngine::
                                      Animation::kInvalidAnimatorInstanceSlot;
                                  std::size_t gpuCandidateCount = 0U;
                                  for (std::size_t inputIndex = 0U;
                                       inputIndex <
                                       capturedInputWorkIndices.size();
                                       ++inputIndex) {
                                    const CapturedInputWorkIndex &captured =
                                        capturedInputWorkIndices[inputIndex];
                                    AnimatorInstanceHandle handle;
                                    bool producesGpuCandidate = false;
                                    if (captured.compact) {
                                      if (captured.storageIndex >=
                                          compactGpuVisualCaptures.size())
                                        return false;
                                      handle =
                                          compactGpuVisualCaptures
                                              .inputs[captured.storageIndex]
                                              .handle;
                                      producesGpuCandidate = true;
                                    } else {
                                      if (captured.storageIndex >=
                                          frameWork.size())
                                        return false;
                                      const FrameWorkItem &item =
                                          frameWork[captured.storageIndex];
                                      handle = item.input.handle;
                                      const bool preservesPrior =
                                          item.cacheReused ||
                                          item.fallbackDecision.source ==
                                              AnimationFallbackSource::
                                                  HeldPriorGeneration;
                                      const AnimationAuthorityRow *prior =
                                          preservesPrior && previous
                                              ? previous->Find(handle)
                                              : nullptr;
                                      rowRetainsPrior[inputIndex] =
                                          prior != nullptr ? 1U : 0U;
                                      producesGpuCandidate =
                                          prior == nullptr && item.gpuLowering;
                                    }
                                    if (!handle.IsValid() ||
                                        (previousSlot !=
                                             TerraEngine::Animation::
                                                 kInvalidAnimatorInstanceSlot &&
                                         handle.slotIndex <= previousSlot))
                                      return false;
                                    const std::size_t pageIndex =
                                        handle.slotIndex /
                                        kAnimationAuthorityPageCapacity128;
                                    if (pageIndex >= mutablePages.size())
                                      return false;
                                    previousSlot = handle.slotIndex;
                                    rowPositions[inputIndex] =
                                        pageRowCounts[pageIndex]++;
                                    if (producesGpuCandidate) {
                                      gpuPositions[inputIndex] =
                                          gpuCandidateCount++;
                                    }
                                  }
                                  for (std::size_t pageIndex = 0U;
                                       pageIndex < pageCount; ++pageIndex) {
                                    mutablePages[pageIndex]->rows.resize(
                                        pageRowCounts[pageIndex]);
                                  }
                                  sidecars->gpuCandidates.resize(
                                      gpuCandidateCount);
                                  std::atomic_bool mixedRowsValid{true};
                                  const auto buildMixedCompactRows =
                                      [this, session, candidateGeneration,
                                       &rowPositions, &gpuPositions,
                                       &mutablePages, &sidecars,
                                       &mixedRowsValid](std::size_t begin,
                                                        std::size_t end) {
                                        TERRAENGINE_PROFILE_SCOPE(
                                            "Animation Task: Build Mixed "
                                            "Compact Rows",
                                            TerraEngine::Profiling::
                                                ProfilerModuleId::
                                                    TaskExecution);
                                        for (std::size_t inputIndex = begin;
                                             inputIndex < end; ++inputIndex) {
                                          const CapturedInputWorkIndex
                                              &captured =
                                                  capturedInputWorkIndices
                                                      [inputIndex];
                                          if (!captured.compact)
                                            continue;
                                          const std::size_t compactIndex =
                                              captured.storageIndex;
                                          const AnimatorInstanceHandle handle =
                                              compactGpuVisualCaptures
                                                  .inputs[compactIndex]
                                                  .handle;
                                          AnimationAuthorityRow &row =
                                              mutablePages
                                                  [handle.slotIndex /
                                                   kAnimationAuthorityPageCapacity128]
                                                      ->rows[rowPositions
                                                                 [inputIndex]];
                                          row.key = {handle.slotIndex,
                                                     handle.generation,
                                                     session};
                                          row.ownerObjectId =
                                              compactGpuVisualCaptures
                                                  .inputs[compactIndex]
                                                  .ownerObjectId;
                                          row.componentGeneration =
                                              compactGpuVisualCaptures
                                                  .inputs[compactIndex]
                                                  .componentGeneration;
                                          row.sourceGeneration =
										  compactGpuVisualCaptures.authorityGeneration;
                                          row.poseGeneration =
                                              compactGpuVisualCaptures
                                                  .candidatePoseGenerations
                                                      [compactIndex];
                                          row.tick = frameTick;
                                          row.stateId = 0U;
                                          row.renderBindingGeneration =
                                              compactGpuVisualCaptures
                                                  .executionModeGenerations
                                                      [compactIndex];
                                          row.active = true;
                                          row.safeFallback = false;
                                          AnimationSpecialistRecord &gpu =
                                              sidecars->gpuCandidates
                                                  [gpuPositions[inputIndex]];
                                          gpu.key = {handle.slotIndex,
                                                     handle.generation,
                                                     session};
                                          gpu.authorityGeneration =
                                              candidateGeneration;
                                          gpu.kind = 1U;
                                          gpu.status = AnimationStatus::Ok;
                                          if (!row.IsValid() ||
                                              !gpu.IsValidFor(
                                                  candidateGeneration)) {
                                            mixedRowsValid.store(
                                                false,
                                                std::memory_order_release);
                                          }
                                        }
                                      };
                                  // Mixed crowds still contain the same cheap
                                  // compact-row writes. Use the measured coarse
                                  // granularity from the homogeneous path while
                                  // the current worker handles the small
                                  // exception set, avoiding nested
                                  // oversubscription.
                                  constexpr std::size_t
                                      kMinimumMixedRowsPerTask = 4096U;
                                  const std::size_t usefulWorkerCount =
                                      std::max<std::size_t>(
                                          1U, (capturedInputWorkIndices.size() +
                                               kMinimumMixedRowsPerTask - 1U) /
                                                  kMinimumMixedRowsPerTask);
                                  const std::size_t workerCount =
                                      std::min<std::size_t>(
                                          availableWorkerCount,
                                          usefulWorkerCount);
                                  const std::size_t chunkSize =
                                      (capturedInputWorkIndices.size() +
                                       workerCount - 1U) /
                                      workerCount;
                                  std::vector<std::future<void>> futures;
                                  futures.reserve(workerCount);
										// This guard dies before every input captured by admitted children.
										TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(futures, TerraEngine::Tasks::NestedTaskGroup::AnimationMixedRows);
                                  // As in the homogeneous path, the current
                                  // scheduler worker builds the first disjoint
                                  // range while queued workers build the rest.
                                  for (std::size_t workerIndex = 1U;
                                       workerIndex < workerCount;
                                       ++workerIndex) {
                                    const std::size_t begin =
                                        workerIndex * chunkSize;
                                    if (begin >=
                                        capturedInputWorkIndices.size())
                                      break;
                                    const std::size_t end = std::min(
                                        capturedInputWorkIndices.size(),
                                        begin + chunkSize);
                                    futuresSettlement.CreateAndEnqueue(ThreadPool::instance(),
                                            [&buildMixedCompactRows, begin,
                                             end]() {
                                              buildMixedCompactRows(begin, end);
                                            });
                                  }
                                  futuresSettlement.RunInline([&] { buildMixedCompactRows(0U, std::min(capturedInputWorkIndices.size(), chunkSize)); });
                                  futuresSettlement.GetAll();
                                  if (PollFrameCancellation() ||
                                      !mixedRowsValid.load(
                                          std::memory_order_acquire))
                                    return false;
                                  // Only the exceptional full rows remain on
                                  // this worker. Their snapshots own
                                  // variable-length events, so merge those
                                  // sidecars after all writes.
                                  for (std::size_t inputIndex = 0U;
                                       inputIndex <
                                       capturedInputWorkIndices.size();
                                       ++inputIndex) {
                                    const CapturedInputWorkIndex &captured =
                                        capturedInputWorkIndices[inputIndex];
                                    if (captured.compact) {
                                      const AnimatorInstanceHandle handle =
                                          compactGpuVisualCaptures
                                              .inputs[captured.storageIndex]
                                              .handle;
                                      pageRetainsPrior
                                          [handle.slotIndex /
                                           kAnimationAuthorityPageCapacity128] =
                                              false;
                                      continue;
                                    }
                                    const FrameWorkItem &item =
                                        frameWork[captured.storageIndex];
                                    const AnimatorInstanceHandle handle =
                                        item.input.handle;
                                    AnimationAuthorityRow &row =
                                        mutablePages
                                            [handle.slotIndex /
                                             kAnimationAuthorityPageCapacity128]
                                                ->rows
                                                    [rowPositions[inputIndex]];
                                    if (rowRetainsPrior[inputIndex] != 0U) {
                                      row = *previous->Find(handle);
                                    } else {
                                      row.key = {handle.slotIndex,
                                                 handle.generation, session};
                                      row.ownerObjectId =
                                          item.input.descriptor.ownerObjectId;
                                      row.objectGeneration =
                                          item.sourceObjectGeneration;
                                      row.componentGeneration =
                                          item.input.descriptor
                                              .componentGeneration;
                                      row.sourceGeneration =
                                          item.candidateAuthorityPublicationGeneration;
                                      row.authorityPublicationGeneration =
                                          item.selectedPublication
                                              ? item.selectedPublication
                                                    ->authorityPublicationGeneration
                                              : item.candidateAuthorityPublicationGeneration;
                                      row.poseGeneration =
                                          item.selectedPublication &&
                                                  item.selectedPublication
                                                      ->snapshot
                                              ? item.selectedPublication
                                                    ->snapshot
                                                    ->publicationGeneration
                                              : item.candidatePoseGeneration;
                                      row.tick = frameTick;
                                      row.stateId = item.binding.stateId;
                                      row.renderBindingGeneration =
                                          item.binding.programGeneration;
                                      row.controller =
                                          item.input.descriptor.controller;
                                      if (item.binding.clip) {
                                        row.clip = {
                                            item.binding.clip->assetId,
                                            item.binding.clipGeneration};
                                      }
                                      if (item.binding.skeleton) {
                                        row.skeleton = {
                                            item.binding.skeleton->assetId,
                                            item.binding.skeletonGeneration};
                                        row.skeletonAsset =
                                            item.binding.skeleton;
                                        row.skeletonCompatibilityFingerprint =
                                            item.binding.skeleton
                                                ->compatibilityFingerprint;
                                      }
                                      row.jointStableIds.assign(
                                          item.binding.jointStableIds.begin(),
                                          item.binding.jointStableIds.end());
                                      row.visualPoseContentIdLow =
                                          item.visualPoseContentIdLow;
                                      row.visualPoseContentIdHigh =
                                          item.visualPoseContentIdHigh;
                                      row.poseMemoryReservation =
                                          item.selectedPublication
                                              ? item.selectedPublication
                                                    ->poseMemoryReservation
                                              : nullptr;
                                      row.sourceObjectWorldTransform =
                                          item.sourceObjectWorldTransform;
                                      row.sourceObjectWorldTransformCaptured =
                                          item.sourceObjectWorldTransformCaptured;
                                      row.requestedExecutionPolicy =
                                          item.binding.requestedExecutionPolicy;
                                      row.effectiveExecutionMode =
                                          item.executionDecision.effectiveMode;
                                      row.rootMotionPolicy =
                                          item.binding.rootMotionPolicy;
                                      row.updatePolicy =
                                          item.binding.updatePolicy;
                                      row.eventPolicy =
                                          item.binding.eventPolicy;
                                      row.cullingPolicy =
                                          item.binding.cullingPolicy;
                                      row.status = item.evaluationStatus;
                                      row.snapshot =
                                          item.selectedPublication
                                              ? item.selectedPublication
                                                    ->snapshot
                                              : nullptr;
                                      row.streamedClipPayloadsActive =
                                          item.selectedPublication
                                              ? item.selectedPublication
                                                    ->streamedClipPayloadsActive
                                              : item.binding
                                                        .streamedSegmentPayloads !=
                                                    nullptr;
                                      row.active = true;
                                      row.safeFallback = item.safePose;
                                      pageRetainsPrior
                                          [handle.slotIndex /
                                           kAnimationAuthorityPageCapacity128] =
                                              false;
                                      if (row.snapshot && row.snapshot->pose) {
                                        sidecars->events.insert(
                                            sidecars->events.end(),
                                            row.snapshot->pose->Events()
                                                .begin(),
                                            row.snapshot->pose->Events().end());
                                        if (row.snapshot->pose
                                                ->RootMotionDelta()) {
                                          sidecars->rootMotion.push_back(
                                              *row.snapshot->pose
                                                   ->RootMotionDelta());
                                        }
                                      }
                                    }
                                    if (!row.IsValid())
                                      return false;
                                    const std::size_t gpuPosition =
                                        gpuPositions[inputIndex];
                                    if (gpuPosition !=
                                        std::numeric_limits<
                                            std::size_t>::max()) {
                                      AnimationSpecialistRecord &gpu =
                                          sidecars->gpuCandidates[gpuPosition];
                                      gpu.key = {row.key.slot,
                                                 row.key.lifetimeGeneration,
                                                 session};
                                      gpu.authorityGeneration =
                                          candidateGeneration;
                                      gpu.kind = 1U;
                                      gpu.status = AnimationStatus::Ok;
                                      if (!gpu.IsValidFor(candidateGeneration))
                                        return false;
                                    }
                                  }
                                  compactPagesWorkerValidated = true;
                                  sidecars->workerValidatedAuthorityGeneration =
                                      candidateGeneration;
                                } else {
                                  for (const CapturedInputWorkIndex &captured :
                                       capturedInputWorkIndices) {
                                    AnimationAuthorityRow row;
                                    std::shared_ptr<const AnimationSnapshot>
                                        snapshot;
                                    if (captured.compact) {
                                      const CompactGpuVisualCaptures &compact =
                                          compactGpuVisualCaptures;
                                      const std::size_t index =
                                          captured.storageIndex;
                                      row.key = {compact.inputs[index]
                                                     .handle.slotIndex,
                                                 compact.inputs[index]
                                                     .handle.generation,
                                                 session};
									  row.ownerObjectId =
										  compact.inputs[index]
											  .ownerObjectId;
									  row.componentGeneration =
										  compact.inputs[index]
											  .componentGeneration;
                                      row.sourceGeneration =
										  compact.authorityGeneration;
                                      row.poseGeneration =
                                          compact
                                              .candidatePoseGenerations[index];
                                      row.tick = frameTick;
                                      // Compact visual packets do not capture
                                      // controller graph state; zero explicitly
                                      // records that this row has no
                                      // CPU-authoritative graph id.
                                      row.stateId = 0U;
                                      row.renderBindingGeneration =
                                          compact
                                              .executionModeGenerations[index];
                                      row.active = true;
                                      row.safeFallback = false;
                                      AnimationSpecialistRecord gpu;
                                      gpu.key = {row.key.slot,
                                                 row.key.lifetimeGeneration,
                                                 session};
                                      gpu.authorityGeneration =
                                          candidateGeneration;
                                      gpu.kind = 1U;
                                      gpu.status = AnimationStatus::Ok;
                                      sidecars->gpuCandidates.push_back(gpu);
                                      pageRetainsPrior
                                          [row.key.slot /
                                           kAnimationAuthorityPageCapacity128] =
                                              false;
                                    } else {
                                      const FrameWorkItem &item =
                                          frameWork[captured.storageIndex];
                                      const bool preservesPrior =
                                          item.cacheReused ||
                                          item.fallbackDecision.source ==
                                              AnimationFallbackSource::
                                                  HeldPriorGeneration;
                                      const AnimationAuthorityRow *prior =
                                          preservesPrior && previous
                                              ? previous->Find(
                                                    item.input.handle)
                                              : nullptr;
                                      const bool retainRow = prior != nullptr;
                                      if (retainRow) {
                                        // A changed neighboring animator may
                                        // require a new root, but this row
                                        // still describes the exact prior
                                        // authority rather than the currently
                                        // incomplete binding that requested
                                        // hold-last.
                                        row = *prior;
                                      } else {
                                        row.key = {item.input.handle.slotIndex,
                                                   item.input.handle.generation,
                                                   session};
                                        row.ownerObjectId =
                                            item.input.descriptor.ownerObjectId;
                                        row.objectGeneration =
                                            item.sourceObjectGeneration;
                                        row.componentGeneration =
                                            item.input.descriptor
                                                .componentGeneration;
                                        row.sourceGeneration =
                                            item.candidateAuthorityPublicationGeneration;
                                        // Preserve the exact immutable row
                                        // selected by worker evaluation.  The
                                        // frame seal may reuse a prior
                                        // publication after a recoverable
                                        // failure, so reconstructing these
                                        // diagnostics from the current binding
                                        // would incorrectly describe the
                                        // retained authority state.
                                        row.authorityPublicationGeneration =
                                            item.selectedPublication
                                                ? item.selectedPublication
                                                      ->authorityPublicationGeneration
                                                : item.candidateAuthorityPublicationGeneration;
                                        row.poseGeneration =
                                            item.selectedPublication &&
                                                    item.selectedPublication
                                                        ->snapshot
                                                ? item.selectedPublication
                                                      ->snapshot
                                                      ->publicationGeneration
                                                : item.candidatePoseGeneration;
                                        row.tick = frameTick;
                                        row.stateId = item.binding.stateId;
                                        row.renderBindingGeneration =
                                            item.binding.programGeneration;
                                        row.controller =
                                            item.input.descriptor.controller;
                                        if (item.binding.clip)
                                          row.clip = {
                                              item.binding.clip->assetId,
                                              item.binding.clipGeneration};
                                        if (item.binding.skeleton) {
                                          row.skeleton = {
                                              item.binding.skeleton->assetId,
                                              item.binding.skeletonGeneration};
                                          row.skeletonAsset =
                                              item.binding.skeleton;
                                          row.skeletonCompatibilityFingerprint =
                                              item.binding.skeleton
                                                  ->compatibilityFingerprint;
                                        }
                                        row.jointStableIds.assign(
                                            item.binding.jointStableIds.begin(),
                                            item.binding.jointStableIds.end());
                                        row.visualPoseContentIdLow =
                                            item.visualPoseContentIdLow;
                                        row.visualPoseContentIdHigh =
                                            item.visualPoseContentIdHigh;
                                        row.poseMemoryReservation =
                                            item.selectedPublication
                                                ? item.selectedPublication
                                                      ->poseMemoryReservation
                                                : nullptr;
                                        row.sourceObjectWorldTransform =
                                            item.sourceObjectWorldTransform;
                                        row.sourceObjectWorldTransformCaptured =
                                            item.sourceObjectWorldTransformCaptured;
                                        row.requestedExecutionPolicy =
                                            item.binding
                                                .requestedExecutionPolicy;
                                        row.effectiveExecutionMode =
                                            item.executionDecision
                                                .effectiveMode;
                                        row.rootMotionPolicy =
                                            item.binding.rootMotionPolicy;
                                        row.updatePolicy =
                                            item.binding.updatePolicy;
                                        row.eventPolicy =
                                            item.binding.eventPolicy;
                                        row.cullingPolicy =
                                            item.binding.cullingPolicy;
                                        row.status = item.evaluationStatus;
                                        row.snapshot =
                                            item.selectedPublication
                                                ? item.selectedPublication
                                                      ->snapshot
                                                : nullptr;
                                        row.streamedClipPayloadsActive =
                                            item.selectedPublication
                                                ? item.selectedPublication
                                                      ->streamedClipPayloadsActive
                                                : item.binding
                                                          .streamedSegmentPayloads !=
                                                      nullptr;
                                        row.active = true;
                                        row.safeFallback = item.safePose;
                                        if (row.snapshot &&
                                            row.snapshot->pose) {
                                          sidecars->events.insert(
                                              sidecars->events.end(),
                                              row.snapshot->pose->Events()
                                                  .begin(),
                                              row.snapshot->pose->Events()
                                                  .end());
                                          if (row.snapshot->pose
                                                  ->RootMotionDelta())
                                            sidecars->rootMotion.push_back(
                                                *row.snapshot->pose
                                                     ->RootMotionDelta());
                                        }
                                        if (item.gpuLowering) {
                                          AnimationSpecialistRecord gpu;
                                          gpu.key = {row.key.slot,
                                                     row.key.lifetimeGeneration,
                                                     session};
                                          gpu.authorityGeneration =
                                              candidateGeneration;
                                          gpu.kind = 1U;
                                          gpu.status = AnimationStatus::Ok;
                                          sidecars->gpuCandidates.push_back(
                                              gpu);
                                        }
                                      }
                                      if (!retainRow) {
                                        pageRetainsPrior
                                            [row.key.slot /
                                             kAnimationAuthorityPageCapacity128] =
                                                false;
                                      }
                                    }
                                    if (!row.IsValid())
                                      return false;
                                    mutablePages
                                        [row.key.slot /
                                         kAnimationAuthorityPageCapacity128]
                                            ->rows.push_back(std::move(row));
                                  }
                                }
                                std::vector<std::shared_ptr<
                                    const AnimationAuthorityPage>>
                                    pages;
                                pages.reserve(mutablePages.size());
                                std::uint32_t reusedPageCount = 0U;
                                std::uint32_t changedPageCount = 0U;
                                {
                                  TERRAENGINE_PROFILE_SCOPE(
                                      "Animation: Finalize Authority Pages",
                                      TerraEngine::Profiling::ProfilerModuleId::
                                          CpuEngine);
                                  for (std::size_t pageIndex = 0U;
                                       pageIndex < mutablePages.size();
                                       ++pageIndex) {
                                    auto &page = mutablePages[pageIndex];
                                    if (!compactPagesWorkerValidated) {
                                      std::sort(
                                          page->rows.begin(), page->rows.end(),
                                          [](const AnimationAuthorityRow &left,
                                             const AnimationAuthorityRow
                                                 &right) {
                                            return left.key.slot <
                                                   right.key.slot;
                                          });
                                    } else {
                                      // Strictly increasing input slots and
                                      // deterministic row positions prove
                                      // ordering; every row was validated by
                                      // its owning worker.
                                      page->workerValidatedRowCount =
                                          page->rows.size();
                                    }
                                    const std::shared_ptr<
                                        const AnimationAuthorityPage>
                                        priorPage =
                                            previous && previous->pages &&
                                                    pageIndex <
                                                        previous->pages->size()
                                                ? previous->pages->at(pageIndex)
                                                : nullptr;
                                    const bool pageMembershipMatches =
                                        pageRetainsPrior[pageIndex] &&
                                        priorPage != nullptr &&
                                        priorPage->firstSlot ==
                                            page->firstSlot &&
                                        priorPage->capacity == page->capacity &&
                                        priorPage->rows.size() ==
                                            page->rows.size() &&
                                        std::equal(
                                            page->rows.begin(),
                                            page->rows.end(),
                                            priorPage->rows.begin(),
                                            [](const AnimationAuthorityRow
                                                   &left,
                                               const AnimationAuthorityRow
                                                   &right) {
                                              return left.key == right.key;
                                            });
                                    if (pageMembershipMatches) {
                                      // Page data and its immutable rows are
                                      // byte-for-byte the prior authority
                                      // facts. Retain the page pointer instead
                                      // of rebuilding a crowd-sized copy merely
                                      // because another page changed this
                                      // frame.
                                      pages.push_back(priorPage);
                                      ++reusedPageCount;
                                    } else {
                                      // A changed page is delivered as one
                                      // complete immutable value. The all-ones
                                      // mask is conservative for this initial
                                      // page implementation: consumers never
                                      // miss a changed row, while wholly
                                      // unchanged pages retain their original
                                      // shared identity above.
                                      std::fill(page->changedMasks.begin(),
                                                page->changedMasks.end(),
                                                std::numeric_limits<
                                                    std::uint64_t>::max());
                                      pages.push_back(std::move(page));
                                      ++changedPageCount;
                                    }
                                  }
                                }
                                sidecars->summary.evaluatedRows =
                                    static_cast<std::uint32_t>(
                                        capturedInputWorkIndices.size());
                                AnimationAuthoritySealInput input;
                                input.frameId =
                                    static_cast<std::uint64_t>(frameTick);
                                input.sessionGeneration = session;
                                input.lifecycleGeneration = lifecycle;
                                input.expectedPreviousGeneration =
                                    previous ? previous->authorityGeneration
                                             : 0U;
                                input.candidateGeneration = candidateGeneration;
                                input.pages = pages;
                                // This worker is the sole mutable owner. Seal
                                // validates it in place and publishes the same
                                // allocation as const, avoiding a 4,000-entry
                                // copy.
                                input.ownedSidecars = std::move(sidecars);
                                input.workersComplete = true;
                                input.reusedPages = reusedPageCount;
                                input.changedPages = changedPageCount;
                                // Existing deterministic history has already
                                // reserved its bounded ring; sealing needs no
                                // later allocation to attach this candidate
                                // generation.
                                input.historyReserved = true;
                                input.cancelled = PollFrameCancellation();
                                sealedAuthorityCandidate =
                                    SealAnimationFrameResult(input);
                                sealedAuthorityCandidateReady =
                                    sealedAuthorityCandidate.IsValid();
								if(!sealedAuthorityCandidateReady) commandFrameFailurePhase = "SealAnimationFrameResult";
                                if (sealedAuthorityCandidateReady) {
                                  // The candidate itself contains the
                                  // authoritative predecessor proof. Keep only
                                  // scalar identities here so protected
                                  // publication never needs to revisit the
                                  // crowd-sized frame work or sidecar
                                  // collections.
                                  publishedAuthorityCommit.animatorCount =
                                      capturedInputWorkIndices.size();
                                  publishedAuthorityCommit.frameEpoch =
                                      frameDeclarationEpoch.load(
                                          std::memory_order_acquire);
                                  publishedAuthorityCommit.authorityGeneration =
                                      candidateGeneration;
                                  publishedAuthorityCommit.sessionGeneration =
                                      session;
                                  publishedAuthorityCommit
                                      .sceneLifecycleGeneration = lifecycle;
                                  const bool runtimeRootPrepared =
                                      publishedAuthorityCommit
                                              .expectedRuntimeRoot != nullptr &&
                                      publishedAuthorityCommit
                                              .nextRuntimeRoot != nullptr &&
                                      publishedAuthorityCommit
                                              .specialistSidecars != nullptr;
                                  if (!runtimeRootPrepared &&
                                      !PrepareRuntimeRootCandidate(
                                          candidateGeneration)) {
									if(!commandFrameFailurePhase) commandFrameFailurePhase = "PrepareRuntimeRootCandidate";
                                    sealedAuthorityCandidate = {};
                                    sealedAuthorityCandidateReady = false;
                                    publishedAuthorityCommit = {};
                                  } else {
                                    // Diagnostics and GPU owner/lowering tables
                                    // are crowd-sized copied values. Build them
                                    // on this worker from the same sealed
                                    // generation so protected publication only
                                    // submits already-owned immutable roots.
                                    if (IsAnimationDetailedDiagnosticsRequested()) {
                                      preparedDetailedDiagnostics =
                                          BuildActiveDetailedDiagnosticsLocked(
                                              candidateGeneration);
                                    }
                                    {
                                      TERRAENGINE_PROFILE_SCOPE(
                                          "Animation: Build Summary "
                                          "Diagnostics",
                                          TerraEngine::Profiling::
                                              ProfilerModuleId::CpuEngine);
                                      preparedDiagnostics =
                                          BuildActiveDiagnosticsLocked(
                                              candidateGeneration,
                                              preparedDetailedDiagnostics
                                                  ? preparedDetailedDiagnostics
                                                        ->animators.size()
                                                  : 0U);
                                    }
                                    {
                                      TERRAENGINE_PROFILE_SCOPE(
                                          "Animation: Build GPU Presentation "
                                          "Frame",
                                          TerraEngine::Profiling::
                                              ProfilerModuleId::CpuEngine);
                                      preparedPresentationFrame =
                                          BuildGpuPresentationFrame(
                                              candidateGeneration,
                                              sealedAuthorityCandidate.frame);
                                    }
                                  }
                                }
                                return sealedAuthorityCandidateReady;
			} catch(const std::bad_alloc&) {
				sealedAuthorityCandidate = {};
				sealedAuthorityCandidateReady = false;
				publishedAuthorityCommit = {};
				preparedDiagnostics.reset();
				preparedDetailedDiagnostics.reset();
				preparedPresentationFrame.reset();
				return false;
			}
		}

		// Runtime candidate preparation owns child work and propagates failures
		// only after those children stop accessing its captured candidate storage.
		[[nodiscard]] bool PrepareRuntimeRootCandidate(
			std::uint64_t authorityGeneration)
		{
			commandFrameFailurePhase = "RuntimeCandidateCompletionGate";
			// Runtime evaluator state is internal mutable state, but it must advance with
			// the same all-or-none visibility as the immutable authority frame.  Build a
			// copy-on-write root on this worker: unchanged slots retain their shared
			// record, while every participating animator receives a private next record.
			// The protected publisher below then performs only pointer/generation checks
			// and two root exchanges, independent of crowd size.
			if(authorityGeneration == 0U || PollFrameCancellation()
				|| !workPlanValidated || workRangeLedger == nullptr
				|| !workRangeLedger->AllCompleted()
				|| capturedInputWorkIndices.size()
					!= plannedAnimatorCount.load(std::memory_order_acquire)
				|| capturedInputWorkIndices.size()
					!= evaluatedAnimatorCount.load(std::memory_order_acquire)
				|| workCompletions.size() != workPlan.items.size()
				|| compactGpuVisualCompletedCount.load(std::memory_order_acquire)
					!= compactGpuVisualWorkItems.size()) return false;

			try {
				const std::shared_ptr<RuntimeRecordRoot> expected = runtimeRecords.load(
					std::memory_order_acquire);
				if(!expected) return false;
				const std::shared_ptr<const CompactRuntimeStateRoot> expectedCompact =
					compactRuntimeStates.load(std::memory_order_acquire);
				if(expectedCompact != compactRuntimeStateCaptureRoot) { commandFrameFailurePhase = "RuntimeCandidateCompactRootChanged"; return false; }
				auto nextCompact =
					std::make_shared<CompactRuntimeStateRoot>(expected->size());
				const std::uint64_t liveSceneGeneration =
					ObjectManager::instance().GetSceneGeneration();
				deterministicPublicationOrder.resize(capturedInputWorkIndices.size());
				std::iota(deterministicPublicationOrder.begin(),
					deterministicPublicationOrder.end(), 0U);
				deterministicPublicationValidated = true;
				auto specialistSidecars = std::make_shared<SealedSpecialistSidecarBatch>();
				// Explicit root-motion requests arrive at protected capture. Copy them
				// into this candidate so a rejected root leaves the live queue intact.
				specialistSidecars->explicitRootMotion = pendingRootMotionApplications;
				const bool allInputsCompact = capturedInputWorkIndices.size()
					== compactGpuVisualCaptures.size()
					&& std::all_of(capturedInputWorkIndices.begin(),
						capturedInputWorkIndices.end(), [](const CapturedInputWorkIndex& captured) {
							return captured.compact;
						});
				// Compact candidates must still refer to the exact root captured before
				// evaluation; otherwise none of their private state may become visible.
				if(allInputsCompact && expected != compactRuntimeCaptureRoot) return false;
				// A few thousand scalar rows fit comfortably in cache and complete faster
				// in one pass than through dozens of task submissions and future joins.
				// Preserve the worker partition for truly large crowds where each task has
				// enough independent rows to repay that coordination cost.
				constexpr std::size_t kMinimumParallelCompactRuntimeRows = 8192U;
				if(allInputsCompact
					&& (capturedInputWorkIndices.size() < kMinimumParallelCompactRuntimeRows
						|| availableWorkerCount <= 1U)) {
					TERRAENGINE_PROFILE_SCOPE("Animation: Build Compact Runtime Root Serial",
						TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					std::vector<std::uint8_t> claimedSlots(nextCompact->size(), 0U);
					for(std::size_t inputIndex = 0U;
						inputIndex < capturedInputWorkIndices.size(); ++inputIndex) {
						const CapturedInputWorkIndex& captured =
							capturedInputWorkIndices[inputIndex];
						if(captured.storageIndex >= compactGpuVisualCaptures.size()) return false;
						const std::size_t compactIndex = captured.storageIndex;
						const AnimatorInstanceHandle handle =
							compactGpuVisualCaptures.inputs[compactIndex].handle;
						if(!handle.IsValid() || handle.slotIndex >= expected->size()
							|| handle.slotIndex >= nextCompact->size()
							|| claimedSlots[handle.slotIndex] != 0U) return false;
						const std::shared_ptr<RuntimeRecord>& current =
							expected->at(handle.slotIndex);
						const CompactRuntimeState* priorCompactState =
							FindCompactRuntimeState(compactRuntimeStateCaptureRoot, handle);
						const AnimationGeneration priorPoseGeneration = priorCompactState
							? priorCompactState->poseGeneration
							: (current ? current->poseGeneration : 0U);
						if(!current || current->handle != handle
							|| compactGpuVisualCaptures.candidatePoseGenerations[compactIndex]
								!= priorPoseGeneration + 1U
							|| compactGpuVisualCaptures.authorityGeneration
								!= authorityGeneration
							|| compactGpuVisualCaptures.evaluationStatuses[compactIndex]
								!= AnimationStatus::Ok) return false;

						claimedSlots[handle.slotIndex] = 1U;
						CompactRuntimeState& state = nextCompact->at(handle.slotIndex);
						state.handle = handle;
						state.playback = compactGpuVisualCaptures.playbacks[compactIndex];
						state.poseGeneration =
							compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
						state.stateGeneration =
							compactGpuVisualCaptures.stateGenerations[compactIndex];
						state.executionModeGeneration =
							compactGpuVisualCaptures.executionModeGenerations[compactIndex];
						state.bindingContentGeneration = current->contentGeneration;
						state.executionModeState =
							compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
						if(current->rollbackHistoryActive) {
							specialistSidecars->rollbackHistoryAnimators.push_back(handle);
						}
					}
					publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
					publishedAuthorityCommit.nextRuntimeRoot =
						publishedAuthorityCommit.expectedRuntimeRoot;
					publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
					publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
					publishedAuthorityCommit.specialistSidecars =
						std::move(specialistSidecars);
					return true;
				}
				if(allInputsCompact) {
					TERRAENGINE_PROFILE_SCOPE("Animation: Build Compact Runtime Root",
						TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
					// Validate unique target slots before workers write disjoint vector
					// elements. This preserves the atomic failure behavior while allowing the
					// expensive RuntimeRecord copies to scale across the available pool.
					std::vector<AnimatorInstanceHandle> compactHandles(
						capturedInputWorkIndices.size());
					std::vector<std::uint8_t> claimedSlots(nextCompact->size(), 0U);
					std::vector<std::uint8_t> rollbackActive(
						capturedInputWorkIndices.size(), 0U);
					for(std::size_t inputIndex = 0U;
						inputIndex < capturedInputWorkIndices.size(); ++inputIndex) {
						const CapturedInputWorkIndex& captured =
							capturedInputWorkIndices[inputIndex];
						if(captured.storageIndex >= compactGpuVisualCaptures.size()) return false;
						const AnimatorInstanceHandle handle =
							compactGpuVisualCaptures.inputs[captured.storageIndex].handle;
						if(!handle.IsValid() || handle.slotIndex >= expected->size()
							|| handle.slotIndex >= nextCompact->size()
							|| claimedSlots[handle.slotIndex] != 0U) return false;
						const std::shared_ptr<RuntimeRecord>& current =
							expected->at(handle.slotIndex);
						const CompactRuntimeState* priorCompactState =
							FindCompactRuntimeState(compactRuntimeStateCaptureRoot, handle);
						const AnimationGeneration priorPoseGeneration = priorCompactState
							? priorCompactState->poseGeneration
							: (current ? current->poseGeneration : 0U);
						if(!current || current->handle != handle
							|| compactGpuVisualCaptures.candidatePoseGenerations[
								captured.storageIndex] != priorPoseGeneration + 1U
							|| compactGpuVisualCaptures.authorityGeneration
								!= authorityGeneration
							|| compactGpuVisualCaptures.evaluationStatuses[
								captured.storageIndex] != AnimationStatus::Ok) return false;
						claimedSlots[handle.slotIndex] = 1U;
						compactHandles[inputIndex] = handle;
						rollbackActive[inputIndex] = current->rollbackHistoryActive ? 1U : 0U;
					}

					const std::size_t workerCount = std::min<std::size_t>(
						availableWorkerCount, capturedInputWorkIndices.size());
					const std::size_t chunkSize = (capturedInputWorkIndices.size()
						+ workerCount - 1U) / workerCount;
					std::vector<std::future<void>> futures;
					futures.reserve(workerCount);
										// This guard dies before every input captured by admitted children.
										TerraEngine::Tasks::ScopedTaskSettlement<void> futuresSettlement(futures, TerraEngine::Tasks::NestedTaskGroup::AnimationCompactRuntime);
					for(std::size_t workerIndex = 0U; workerIndex < workerCount;
						++workerIndex) {
						const std::size_t begin = workerIndex * chunkSize;
						if(begin >= capturedInputWorkIndices.size()) break;
						const std::size_t end = std::min(
							capturedInputWorkIndices.size(), begin + chunkSize);
						futuresSettlement.CreateAndEnqueue(ThreadPool::instance(),
							[this, begin, end, &compactHandles,
								&nextCompact]() {
								TERRAENGINE_PROFILE_SCOPE(
									"Animation Task: Build Compact Runtime State",
									TerraEngine::Profiling::ProfilerModuleId::TaskExecution);
								for(std::size_t inputIndex = begin; inputIndex < end;
									++inputIndex) {
									const CapturedInputWorkIndex& captured =
										capturedInputWorkIndices[inputIndex];
									const AnimatorInstanceHandle handle = compactHandles[inputIndex];
									const std::size_t compactIndex = captured.storageIndex;
									CompactRuntimeState& state =
										nextCompact->at(handle.slotIndex);
									state.handle = handle;
									state.playback = compactGpuVisualCaptures.playbacks[compactIndex];
									state.poseGeneration =
										compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
									state.stateGeneration =
										compactGpuVisualCaptures.stateGenerations[compactIndex];
									state.executionModeGeneration =
										compactGpuVisualCaptures.executionModeGenerations[compactIndex];
									state.bindingContentGeneration =
										compactGpuVisualCaptures.runtimeRecords[compactIndex]
											->contentGeneration;
									state.executionModeState =
										compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
								}
							});
					}
					futuresSettlement.GetAll();
					if(PollFrameCancellation()) return false;
					for(std::size_t inputIndex = 0U; inputIndex < rollbackActive.size();
						++inputIndex) {
						if(rollbackActive[inputIndex] != 0U) {
							specialistSidecars->rollbackHistoryAnimators.push_back(
								compactHandles[inputIndex]);
						}
					}
					publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
					publishedAuthorityCommit.nextRuntimeRoot =
						publishedAuthorityCommit.expectedRuntimeRoot;
					publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
					publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
					publishedAuthorityCommit.specialistSidecars =
						std::move(specialistSidecars);
					return true;
				}

				// Only complete/specialist rows clone their broad evaluator record.
				// Compact rows publish into the narrow scalar root below, so thousands
				// of visual animators do not copy bindings, graphs, or event vectors.
				auto next = std::make_shared<RuntimeRecordRoot>(*expected);
				for(std::size_t inputIndex = 0U;
					inputIndex < capturedInputWorkIndices.size(); ++inputIndex) {
					const CapturedInputWorkIndex& captured = capturedInputWorkIndices[inputIndex];
					const AnimatorInstanceHandle handle = captured.compact
						? compactGpuVisualCaptures.inputs[captured.storageIndex].handle
						: frameWork[captured.storageIndex].input.handle;
					if(!handle.IsValid() || handle.slotIndex >= expected->size()
						|| handle.slotIndex >= next->size()) return false;
					const std::shared_ptr<RuntimeRecord>& current = expected->at(handle.slotIndex);
					if(!current || current->handle != handle) return false;
					if(current->rollbackHistoryActive) {
						specialistSidecars->rollbackHistoryAnimators.push_back(handle);
					}

					if(captured.compact) {
						const std::size_t compactIndex = captured.storageIndex;
						const CompactRuntimeState* priorCompactState =
							FindCompactRuntimeState(compactRuntimeStateCaptureRoot, handle);
						const AnimationGeneration priorPoseGeneration = priorCompactState
							? priorCompactState->poseGeneration : current->poseGeneration;
						if(compactIndex >= compactGpuVisualCaptures.size()
							|| compactGpuVisualCaptures.candidatePoseGenerations[compactIndex]
								!= priorPoseGeneration + 1U
							|| compactGpuVisualCaptures.authorityGeneration
								!= authorityGeneration
							|| compactGpuVisualCaptures.evaluationStatuses[compactIndex]
								!= AnimationStatus::Ok) return false;
						CompactRuntimeState& state = nextCompact->at(handle.slotIndex);
						state.handle = handle;
						state.playback = compactGpuVisualCaptures.playbacks[compactIndex];
						state.poseGeneration =
							compactGpuVisualCaptures.candidatePoseGenerations[compactIndex];
						state.stateGeneration =
							compactGpuVisualCaptures.stateGenerations[compactIndex];
						state.executionModeGeneration =
							compactGpuVisualCaptures.executionModeGenerations[compactIndex];
						state.bindingContentGeneration = current->contentGeneration;
						state.executionModeState =
							compactGpuVisualCaptures.executionDecisions[compactIndex].nextState;
						continue;
					}

					if(captured.storageIndex >= frameWork.size()) return false;
					// Complete/specialist animation owns mutable graph, event, and pose
					// state, so only these uncommon rows require a COW record clone.
					auto record = std::make_shared<RuntimeRecord>(*current);
					FrameWorkItem& item = frameWork[captured.storageIndex];
					const bool preservesPrior = item.cacheReused
						|| item.fallbackDecision.source
							== AnimationFallbackSource::HeldPriorGeneration;
					if(preservesPrior) {
						item.liveFallbackGenerations = BuildLiveFallbackGenerations(
							*current, item, liveSceneGeneration);
					}
					if(current->descriptor.componentGeneration
							!= item.input.descriptor.componentGeneration
						|| (preservesPrior
							? item.selectedPublication == nullptr
								|| item.selectedPublication->poseGeneration
									!= current->poseGeneration
							: item.candidatePoseGeneration != current->poseGeneration + 1U)
						|| item.candidateAuthorityPublicationGeneration != authorityGeneration
						|| !item.evaluated) { commandFrameFailurePhase = "RuntimeCandidateCompleteGenerationGate"; return false; }

					record->playback = item.playback;
					if(item.compactCaptureOnly) {
						record->poseGeneration = item.candidatePoseGeneration;
						record->stateGeneration = item.candidateStateGeneration;
						record->executionModeGeneration = item.candidateExecutionModeGeneration;
						record->executionModeState = item.executionDecision.nextState;
						next->Set(handle.slotIndex, std::move(record));
						continue;
					}
					if(item.eventStateCaptured) record->eventStream = item.eventStream;
					if(item.controllerStateCaptured) {
						record->graphState = item.candidateGraphState;
						record->transition = item.transition;
					}
					record->binding.stateId = item.binding.stateId;
					const bool clipBindingChanged = record->binding.clip != item.binding.clip
						|| record->binding.clipGeneration != item.binding.clipGeneration
						|| record->binding.clipDeterministicContentFingerprint
							!= item.binding.clipDeterministicContentFingerprint
						|| record->binding.streamedSegmentPayloads
							!= item.binding.streamedSegmentPayloads;
					if(clipBindingChanged) {
						record->binding.clip = item.binding.clip;
						record->binding.clipGeneration = item.binding.clipGeneration;
						record->binding.clipDeterministicContentFingerprint =
							item.binding.clipDeterministicContentFingerprint;
						record->binding.residentSegments = item.binding.residentSegments;
						record->binding.streamedSegmentPayloads =
							item.binding.streamedSegmentPayloads;
					}
					record->latestRetargetMetrics = item.retargetActive
						? item.retargetMetrics : RetargetEvaluationMetrics{};
					record->latestRetargetDeterministicHash = item.retargetActive
						? item.retargetDeterministicHash : 0U;
					// A retained GPU-visual authority row intentionally has no CPU pose
					// snapshot. Its row generation is still the exact published owner
					// generation, so keep that identity during a one-frame asset fallback.
					// Requiring a snapshot here would permanently reject every later root.
					record->poseGeneration = preservesPrior && item.selectedPublication
						? item.selectedPublication->poseGeneration
						: item.selectedPublication && item.selectedPublication->snapshot
							? item.selectedPublication->snapshot->publicationGeneration
							: item.candidatePoseGeneration;
					record->stateGeneration = item.candidateStateGeneration;
					const bool parametersChanged = record->parameterGeneration
						!= item.candidateParameterGeneration;
					record->parameterGeneration = item.candidateParameterGeneration;
					record->executionModeGeneration = item.candidateExecutionModeGeneration;
					record->executionModeState = item.executionDecision.nextState;
					if(parametersChanged) {
						record->parameters = item.parameters;
						record->layerWeights = item.layerWeights;
					}
					item.capturedStateGeneration = record->stateGeneration;
					item.capturedParameterGeneration = record->parameterGeneration;
					item.capturedExecutionModeGeneration = record->executionModeGeneration;
					item.bindingCaptureContentGeneration = record->contentGeneration;
					item.runtimeStateCaptureCommitted = true;
					record->appliedMotionQueryId = item.appliedMotionQueryId;
					if(item.ragdollPhysicsPose) record->ragdollGeneration = item.ragdollGeneration;
					if(item.ragdollGeneration.IsValid()
						&& (item.ragdollLifecycle == RagdollLifecycleState::Active
							|| item.ragdollLifecycle == RagdollLifecycleState::Recovering)) {
						PreparedRagdollPublication publication;
						publication.runtimeRecord = record;
						publication.motorDriveCount = static_cast<std::uint32_t>(
							std::min<std::size_t>(item.ragdollMotorDrives.size(),
								std::numeric_limits<std::uint32_t>::max()));
						if(!item.ragdollMotorDrives.empty()) {
							RagdollPhysicsRequest request;
							request.kind = RagdollPhysicsRequestKind::UpdateMotors;
							request.ragdollId = record->ragdollId;
							request.generation = item.ragdollGeneration;
							request.expectedCurrentGeneration = item.ragdollGeneration;
							request.motorDrives = item.ragdollMotorDrives;
							request.correlationId = record->ragdollId
								^ authorityGeneration ^ 0x4D4F544F52000000ULL;
							if(request.correlationId == 0U) request.correlationId = 1U;
							request.producerOrder = 1U;
							publication.motorRequest = std::move(request);
						}
						if(item.ragdollRecoveryBlendComplete
							&& record->pendingRagdollCorrelationId == 0U) {
							RagdollGenerationStamp generation = item.ragdollGeneration;
							generation.publicationGeneration = authorityGeneration;
							generation.fixedStep = std::max<AnimationFixedTick>(1, frameTick);
							RagdollPhysicsRequest request;
							request.kind = RagdollPhysicsRequestKind::Deactivate;
							request.ragdollId = record->ragdollId;
							request.generation = generation;
							request.expectedCurrentGeneration = item.ragdollGeneration;
							request.correlationId = record->ragdollId
								^ authorityGeneration ^ 0x5245434F56455259ULL;
							if(request.correlationId == 0U) request.correlationId = 1U;
							request.producerOrder = 2U;
							publication.deactivateRequest = std::move(request);
						}
						specialistSidecars->ragdoll.push_back(std::move(publication));
					}
					if(item.proceduralStateObserved) {
						specialistSidecars->procedural.push_back({
							{item.input.handle.slotIndex, item.input.handle.generation},
							item.proceduralResults});
					}
					if(item.retargetActive && item.sampledSourceSkeleton) {
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
						specialistSidecars->retarget.push_back({std::move(snapshot)});
					}
					if(AnimationAttachmentSnapshotPublisher::Instance()
						.IsPublicationRequested(item.input.handle)) {
						PreparedAttachmentPublication publication;
						publication.animator = item.input.handle;
						if(!item.cacheReused
							&& item.fallbackDecision.source
								!= AnimationFallbackSource::HeldPriorGeneration
							&& item.selectedPublication && item.selectedPublication->snapshot
							&& item.selectedPublication->snapshot->pose && item.binding.skeleton
							&& item.sourceObjectGeneration != 0U
							&& item.sourceObjectWorldTransformCaptured) {
							AnimationBoneSocketSnapshotRequest request;
							request.animator = item.input.handle;
							request.sourceObjectId = item.input.descriptor.ownerObjectId;
							request.sourceObjectGeneration = item.sourceObjectGeneration;
							request.sourceComponentGeneration =
								item.input.descriptor.componentGeneration;
							request.sourceTick = item.selectedPublication->snapshot->tick;
							request.publicationGeneration =
								item.selectedPublication->snapshot->publicationGeneration;
							request.sourceObjectWorldTransform = item.sourceObjectWorldTransform;
							request.pose = item.selectedPublication->snapshot->pose;
							request.skeleton = item.binding.skeleton;
							AnimationBoneSocketSnapshot snapshot;
							if(CaptureAnimationBoneSocketSnapshot(request, snapshot)
								== AnimationStatus::Ok) {
								publication.snapshot = std::move(snapshot);
							}
						}
						specialistSidecars->attachments.push_back(std::move(publication));
					}
					if(!item.cacheReused
						&& item.fallbackDecision.source
							!= AnimationFallbackSource::HeldPriorGeneration
						&& item.selectedPublication && item.selectedPublication->snapshot
						&& item.selectedPublication->snapshot->pose
						&& item.selectedPublication->snapshot->pose
							->RootMotionDelta().has_value()) {
						PreparedAutomaticRootMotionPublication publication;
						publication.request.delta =
							*item.selectedPublication->snapshot->pose->RootMotionDelta();
						publication.request.ownerObjectId = item.input.descriptor.ownerObjectId;
						publication.request.objectGeneration = item.sourceObjectGeneration;
						publication.request.componentGeneration =
							item.input.descriptor.componentGeneration;
						publication.request.sourceObjectWorldTransform =
							item.sourceObjectWorldTransform;
						publication.request.sourceObjectWorldTransformCaptured =
							item.sourceObjectWorldTransformCaptured;
						switch(item.rootMotionEnabled
						? AnimatorRootMotionPolicy::ApplyToCharacter
						: item.binding.rootMotionPolicy) {
						case AnimatorRootMotionPolicy::Ignore:
							publication.request.mode = AnimationRootMotionApplicationMode::Ignore;
							break;
						case AnimatorRootMotionPolicy::ExtractOnly:
							publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
							break;
						case AnimatorRootMotionPolicy::ApplyToObject:
							publication.request.mode = AnimationRootMotionApplicationMode::ApplyToTransform;
							break;
						case AnimatorRootMotionPolicy::ApplyToCharacter:
							publication.request.mode = AnimationRootMotionApplicationMode::ApplyToCharacter;
							break;
						default:
							publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
							break;
						}
						const bool activatesRagdollAtThisBoundary = std::any_of(
							capturedRagdollRequests.begin(), capturedRagdollRequests.end(),
							[&item](const NativeAnimationRagdollRequest& request) {
								return request.animator.slotIndex == item.input.handle.slotIndex
									&& request.animator.generation == item.input.handle.generation
									&& request.operation
										== NativeAnimationRagdollOperation::Activate;
							});
						if(item.ragdollLifecycle != RagdollLifecycleState::Inactive
							|| activatesRagdollAtThisBoundary) {
							publication.request.mode = AnimationRootMotionApplicationMode::ExtractOnly;
						}
						specialistSidecars->automaticRootMotion.push_back(
							std::move(publication));
					}
					if(item.cacheEligible
						&& item.fallbackDecision.source == AnimationFallbackSource::Current
						&& item.evaluationStatus == AnimationStatus::Ok
						&& item.selectedPublication && item.selectedPublication->snapshot) {
						specialistSidecars->evaluationCacheWorkIndices.push_back(inputIndex);
					}
					next->Set(handle.slotIndex, std::move(record));
				}

				publishedAuthorityCommit.expectedRuntimeRoot = std::move(expected);
				publishedAuthorityCommit.nextRuntimeRoot = std::move(next);
				publishedAuthorityCommit.expectedCompactRuntimeRoot = expectedCompact;
				publishedAuthorityCommit.nextCompactRuntimeRoot = std::move(nextCompact);
				publishedAuthorityCommit.specialistSidecars = std::move(specialistSidecars);
				commandFrameFailurePhase = nullptr;
				return true;
			} catch(const std::bad_alloc&) {
				// The token owns every candidate sidecar. Dropping it makes a failed
				// worker seal unobservable without touching any live specialist queue.
				publishedAuthorityCommit = {};
				return false;
			}
		}

		bool CommitPublishedEvaluationCacheEntries(
			std::size_t animatorCount,
			std::uint64_t frameEpoch,
			std::uint64_t authorityGeneration,
			std::uint64_t sessionGeneration) noexcept
		{
			const PublishedAuthorityCommitToken token = publishedAuthorityCommit;
			if(!token.ready || authorityGeneration == 0U
				|| token.authorityGeneration != authorityGeneration
				|| token.animatorCount != animatorCount
				|| token.frameEpoch != frameEpoch
				|| token.sessionGeneration != sessionGeneration
				|| token.sceneLifecycleGeneration
					!= frameLifecycleGenerations.Generation(
						AnimationLifecycleBoundary::Scene)
				|| !runtimeActive.load(std::memory_order_acquire)
				|| runtimeRecords.load(std::memory_order_acquire)
					!= token.nextRuntimeRoot
				|| compactRuntimeStates.load(std::memory_order_acquire)
					!= token.nextCompactRuntimeRoot
				|| authorityPublisher.Load() != sealedAuthorityCandidate.frame
				|| !token.expectedRuntimeRoot || !token.nextCompactRuntimeRoot
				|| !token.specialistSidecars) {
				return false;
			}

			// The protected publisher exchanged the complete compact scalar root beside
			// authority. Validate its rows here without writing through scattered broad
			// runtime records; any mismatch rejects optional follow-up cache work.
			for(const PreparedCompactRuntimeCommit& update
				: token.specialistSidecars->compactRuntime) {
				const CompactRuntimeState* publishedState =
					FindCompactRuntimeState(token.nextCompactRuntimeRoot, update.handle);
				const RuntimeRecord* publishedBroadRecord = update.handle.IsValid()
					&& update.handle.slotIndex < token.nextRuntimeRoot->size()
					? token.nextRuntimeRoot->at(update.handle.slotIndex).get() : nullptr;
				const bool compactStatePublished = publishedState
					&& publishedState->poseGeneration == update.poseGeneration
					&& publishedState->stateGeneration == update.stateGeneration
					&& publishedState->executionModeGeneration
						== update.executionModeGeneration;
				const bool broadStatePublished = publishedBroadRecord
					&& publishedBroadRecord != update.runtimeRecord
					&& publishedBroadRecord->handle == update.handle
					&& publishedBroadRecord->poseGeneration == update.poseGeneration
					&& publishedBroadRecord->stateGeneration == update.stateGeneration
					&& publishedBroadRecord->executionModeGeneration
						== update.executionModeGeneration;
				if(!update.runtimeRecord || !update.handle.IsValid()
					|| update.handle.slotIndex >= token.expectedRuntimeRoot->size()
					|| token.expectedRuntimeRoot->at(update.handle.slotIndex).get()
						!= update.runtimeRecord
					|| update.runtimeRecord->handle != update.handle
					|| update.poseGeneration == 0U
					|| update.stateGeneration == 0U
					|| update.executionModeGeneration == 0U
					|| (!compactStatePublished && !broadStatePublished)) return false;
			}

			// The accepted authority/root identities above are the transaction
			// boundary. Cache insertion can now traverse the prepared rows on a worker
			// without making rejection observable and without extending the protected
			// publication lock. Each Store owns its internal synchronization; a failed
			// optional insertion remains an ordinary miss and cannot change authority.
			for(const std::size_t inputIndex : token.specialistSidecars->evaluationCacheWorkIndices) {
				FrameWorkItem* item = CompleteWorkForInputIndex(inputIndex);
				if(item == nullptr || !item->selectedPublication
					|| !item->selectedPublication->snapshot) continue;
				AnimationEvaluationCacheKey storedKey = item->cacheKey;
				storedKey.poseGeneration =
					item->selectedPublication->snapshot->publicationGeneration;
				AnimationEvaluationCacheValue cacheValue;
				cacheValue.sourceFrameGeneration =
					item->candidateAuthorityPublicationGeneration;
				cacheValue.publicationGeneration =
					item->selectedPublication->authorityPublicationGeneration;
				cacheValue.poseToken =
					item->selectedPublication->snapshot->publicationGeneration;
				(void)evaluationCache.Store(storedKey, cacheValue);
			}
			return true;
		}

		bool ApplySealedSpecialistSidecars(
			PublishedAuthorityCommitToken& token) noexcept
		{
			if(!token.ready || token.authorityGeneration == 0U
				|| !token.specialistSidecars
				|| runtimeRecords.load(std::memory_order_acquire)
					!= token.nextRuntimeRoot) return false;
			SealedSpecialistSidecarBatch& sidecars = *token.specialistSidecars;
			// Visual workers produce only copied motor scales. The next protected
			// PhysicsCommandCommit owns provider mutation, preserving one Jolt authority.
			for(PreparedRagdollPublication& publication : sidecars.ragdoll) {
				RuntimeRecord* record = publication.runtimeRecord.get();
				if(record == nullptr) continue;
				record->ragdollMotorDriveCount = publication.motorDriveCount;
				if(publication.motorRequest) {
					record->ragdollMotorStatus = Physics::PhysicsSystem::Instance()
						.SubmitRagdollPhysicsRequest(std::move(*publication.motorRequest));
				}
				if(publication.deactivateRequest) {
					if(record->ragdollLifecycle == RagdollLifecycleState::Recovering) {
						const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
						const auto binding = bindings ? std::lower_bound(bindings->active.begin(),
							bindings->active.end(), record->handle,
							[](const AnimationRootControllerBinding& row, AnimatorInstanceHandle handle) {
								return row.animator < handle;
							}) : std::vector<AnimationRootControllerBinding>::const_iterator{};
						if(bindings && binding != bindings->active.end()
							&& binding->animator == record->handle) {
							// A bound controller needs a durable Physics recovery ticket;
							// otherwise keep the ragdoll Recovering and retry next frame.
							if(binding->bindingValidityGuard
								&& binding->bindingValidityGuard->Validate()
									!= Physics::CharacterMotionValidityReason::Valid) continue;
							const auto lease = Physics::PhysicsSystem::Instance()
								.ReadCharacterMotionLease(binding->controller);
							const auto sourcePose = Physics::PhysicsSystem::Instance()
								.LatestRagdollPhysicsPoseAtOrAfter(record->ragdollId, record->ragdollGeneration);
							const auto characterPacket = Physics::PhysicsSystem::Instance()
								.LatestCharacterStatePacket();
							if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive
								|| lease->externalOwnerId != RagdollExternalOwnerId(record->handle)
								|| !sourcePose || !sourcePose->completeCurrentStep
								|| !characterPacket || !characterPacket->states) continue;
							const auto character = std::find_if(characterPacket->states->begin(),
								characterPacket->states->end(), [binding](const Physics::PhysicsCharacterState& state) {
									return state.world == binding->controller.world
										&& state.handle == binding->controller.character
										&& state.objectId == binding->controller.objectId
										&& state.objectGeneration == binding->controller.objectGeneration
										&& state.componentGeneration == binding->controller.componentGeneration;
								});
							if(character == characterPacket->states->end()) continue;
							const AnimationQuaternion rotation{character->pose.rotation.x,
								character->pose.rotation.y, character->pose.rotation.z,
								character->pose.rotation.w};
							const AnimationFloat3 offset = RotateVector(
								rotation, binding->capturedRelativeTranslation);
							Physics::CharacterExternalRecoveryIntent intent;
							intent.expectedExternalLease = *lease;
							intent.externalOwnerId = lease->externalOwnerId;
							intent.frame = binding->frame;
							intent.alignedControllerPose.position = {
								record->ragdollRecoveryAlignedRootPosition.x - offset.x,
								record->ragdollRecoveryAlignedRootPosition.y - offset.y,
								record->ragdollRecoveryAlignedRootPosition.z - offset.z};
							intent.alignedControllerPose.rotation = character->pose.rotation;
							intent.bindingGeneration = binding->bindingGeneration;
							intent.bindingGuard = binding->bindingValidityGuard;
							if(!intent.alignedControllerPose.IsFinite()) continue;
							try {
								publication.deactivateRequest->recoveryIntent =
									std::make_shared<const Physics::CharacterExternalRecoveryIntent>(intent);
								publication.deactivateRequest->recoverySourcePose = sourcePose;
							} catch(const std::bad_alloc&) {
								continue;
							}
						}
					}
					const RagdollGenerationStamp generation =
						publication.deactivateRequest->generation;
					const std::uint64_t correlationId =
						publication.deactivateRequest->correlationId;
					const bool externalRecovery =
						publication.deactivateRequest->recoveryIntent != nullptr;
					const AnimationStatus queued = Physics::PhysicsSystem::Instance()
						.SubmitRagdollPhysicsRequest(
							std::move(*publication.deactivateRequest));
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
						record->pendingRagdollCorrelationId = correlationId;
						record->pendingRagdollOperation =
							NativeAnimationRagdollOperation::Deactivate;
						record->pendingRagdollExternalRecovery = externalRecovery;
					}
				}
			}

			// Procedural observations are diagnostic side publications. They become
			// visible only after the pose authority batch commits, so a result can
			// never advertise a generation whose pose was rejected during preflight.
			for(const PreparedProceduralPublication& publication
				: sidecars.procedural) {
				(void)proceduralCommands->PublishResults(
					publication.animator, publication.results, *instances);
			}

			// An active retarget publishes the evaluator's measured result, not merely
			// the earlier activation acknowledgement. This copied diagnostic follows the
			// committed pose and is therefore safe for managed/editor observations.
			if(advancedCommands != nullptr) {
				for(PreparedRetargetPublication& publication
					: sidecars.retarget) {
					(void)advancedCommands->Publish(std::move(publication.snapshot));
				}
			}

			// The protected publisher already exchanged the complete runtime root before
			// this sparse packet is consumed. Reused/held generations therefore preserve
			// the exact prior attachment publication and never relabel it as fresh work.
			AnimationAttachmentSnapshotPublisher& attachmentPublisher =
				AnimationAttachmentSnapshotPublisher::Instance();
			for(PreparedAttachmentPublication& publication
				: sidecars.attachments) {
				const bool attachmentPublished = publication.snapshot
					&& attachmentPublisher.Publish(std::move(*publication.snapshot));
				// GPU-only, stale-source, or failed captures must retire an older socket
				// generation so copied gameplay reads cannot silently mix publications.
				if(!attachmentPublished) {
					attachmentPublisher.RetireSnapshot(publication.animator);
				}
			}

			// Route every copied root interval only after the complete pose set commits.
			// Explicit gameplay requests have deterministic precedence over authored
			// automatic policy. Both pass through the same reservation ledger, so a
			// later request for the same mutating generation is rejected, never applied
			// twice. Worker submissions remain copied until this protected loop.
			const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);
			const auto exclusions = ragdollExclusions.load(std::memory_order_acquire);
			if(rootBindings && exclusions && !exclusions->entries.empty()) {
				// A ragdoll Activate accepted after Locomotion publication still makes
				// the controller physically exclusive before any root interval routes.
				(void)ApplyRagdollControllerExclusions(*rootBindings, *exclusions,
					Physics::PhysicsSystem::Instance());
			}
			for(const AnimationRootMotionApplicationRequest& request : sidecars.explicitRootMotion) {
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

			// The automatic producer uses one monotonically ordered identity, so a mode
			// switch or duplicate publication cannot apply the same generation twice.
			constexpr std::uint32_t kAutomaticRootMotionProducerId = 0xA11A0001U;
			for(PreparedAutomaticRootMotionPublication& publication
				: sidecars.automaticRootMotion) {
				AnimationRootMotionApplicationRequest& request = publication.request;
				const AnimationRootMotionDelta& delta = request.delta;
				if(automaticRootMotionSequence == std::numeric_limits<std::uint64_t>::max()) {
					// Sequence exhaustion is fail-closed and still observable. Reusing an
					// ordered producer identity could double-apply an earlier publication.
					AnimationRootMotionApplicationResult exhausted;
					exhausted.animator = delta.animator;
					exhausted.status = AnimationStatus::RangeOverflow;
					exhausted.disposition = AnimationRootMotionDisposition::Rejected;
					exhausted.generation = delta.generation;
					exhausted.sourceBeginTick = delta.sourceBeginTick;
					exhausted.sourceEndTick = delta.sourceEndTick;
					exhausted.localTranslation = delta.localTranslation;
					exhausted.localYawRadians = delta.localYawRadians;
					exhausted.ownerObjectId = request.ownerObjectId;
					exhausted.objectGeneration = request.objectGeneration;
					exhausted.componentGeneration = request.componentGeneration;
					exhausted.sourceStateId = delta.sourceStateId;
					exhausted.accumulatedCycles = delta.accumulatedCycles;
					exhausted.layerIndex = delta.layerIndex;
					exhausted.authoredPolicy = delta.policy;
					exhausted.applicationMode = request.mode;
					exhausted.producerId = kAutomaticRootMotionProducerId;
					exhausted.producerSequence = automaticRootMotionSequence;
					exhausted.correlationId = automaticRootMotionSequence;
					exhausted.idempotencyKey = automaticRootMotionSequence;
					exhausted.publicationGeneration = token.authorityGeneration;
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
				request.publicationGeneration = token.authorityGeneration;
				(void)RouteRootMotionApplication(
					request,
					ObjectManager::instance(),
					TerraEngine::Scene::SceneMutationQueue::Instance(),
					TerraEngine::Physics::PhysicsSystem::Instance(),
					request.mode == AnimationRootMotionApplicationMode::ApplyToTransform
						? AnimationRootMotionSceneVisibility::ImmediateProtectedCommit
						: AnimationRootMotionSceneVisibility::DeferredQueue);
			}
			return true;
		}

		AnimationDiagnosticsSnapshot BuildActiveDiagnosticsLocked(
			std::uint64_t publicationGeneration,
			std::size_t detailedRecordCount = 0U) const noexcept
		{
			AnimationDiagnosticsSnapshot snapshot;
			snapshot.frameIndex = publicationGeneration;
			snapshot.candidateGeneration = publicationGeneration;
			snapshot.acceptedGeneration = publicationGeneration;
			snapshot.presentationGeneration =
				AnimationPresentationSystem::Instance().PresentationPublicationGeneration();
			snapshot.sceneGeneration = frameSceneGeneration;
			snapshot.fixedTick = frameTick;
			// Summary counters remain cheap on every active frame. Only the detail
			// level and bounded record count reflect a separately requested copied
			// publication; row construction itself happens in the method below.
			snapshot.detailLevel = detailedRecordCount == 0U
				? AnimationDiagnosticsDetailLevel::Summary
				: AnimationDiagnosticsDetailLevel::Detailed;
			snapshot.dormant = false;
			AnimationDiagnosticsCounters& counters = snapshot.counters;
			// The indexed capture mapping is the authoritative full-frame population:
			// early compact records intentionally never construct FrameWorkItem entries.
			counters.activeAnimators = capturedInputWorkIndices.size();
			// This is the protected pre-copy admission count. It proves compact
			// candidates bypassed broad runtime-state capture even if a later immutable
			// lowering proof rejects the complete frame.
			counters.compactEligibleAnimators = compactPreCopyCaptureCount.load(
				std::memory_order_acquire);
			counters.compactProcessedAnimators =
				compactGpuVisualCompletedCount.load(std::memory_order_acquire);
			// FrameWorkItem is constructed only for the complete-capture branch, so its
			// count cannot be derived by subtracting compact packets (which also include
			// direct SoA entries and would underflow for an all-compact crowd).
			counters.completeCapturedAnimators = frameWork.size();
			counters.completeProcessedAnimators = workCompletions.size();
			// Finalization creates exactly one compact plan envelope for all proven
			// packets. This is a real plan-group count, not a per-animator estimate.
			counters.compactPlanGroups = compactGpuVisualWorkItems.empty() ? 0U : 1U;
			constexpr double kNanosecondsPerMillisecond = 1'000'000.0;
			snapshot.timings.compactCaptureMs = static_cast<double>(
				compactPlanningNanoseconds.load(std::memory_order_acquire))
				/ kNanosecondsPerMillisecond;
			snapshot.timings.completeCaptureMs = static_cast<double>(
				completePlanningNanoseconds.load(std::memory_order_acquire))
				/ kNanosecondsPerMillisecond;
			snapshot.timings.compactEvaluationMs = static_cast<double>(
				compactEvaluationNanoseconds.load(std::memory_order_acquire))
				/ kNanosecondsPerMillisecond;
			snapshot.timings.completeEvaluationMs = static_cast<double>(
				completeEvaluationNanoseconds.load(std::memory_order_acquire))
				/ kNanosecondsPerMillisecond;
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
				if(item.visualEvaluationSuspended
					|| runState == AnimatorPlaybackRunState::Paused
					|| item.binding.updatePolicy == AnimatorUpdatePolicy::Manual) {
					++counters.suspendedAnimators;
				}
				const bool successfulGpuVisual =
					item.executionDecision.effectiveMode
						== AnimationEffectiveExecutionMode::GpuVisual
					&& item.gpuLowering.has_value()
					&& item.evaluationStatus == AnimationStatus::Ok
					&& !item.safePose;
				if(item.executionDecision.transition.occurred) {
					if(item.executionDecision.transition.from
						== AnimationEffectiveExecutionMode::GpuVisual) {
						++counters.compactToCompleteTransitions;
					} else if(item.executionDecision.transition.to
						== AnimationEffectiveExecutionMode::GpuVisual) {
						++counters.completeToCompactTransitions;
					}
				}
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
					if(!item.visualEvaluationSuspended) {
						counters.gpuEvaluatedJoints += jointCount;
					}
					break;
				case AnimationEffectiveExecutionMode::HybridAuthoritative:
					++counters.hybridAuthoritativeAnimators;
					counters.cpuSampledJoints += jointCount;
					counters.gpuEvaluatedJoints += jointCount;
					break;
				case AnimationEffectiveExecutionMode::CpuDeterministicFallback:
				default:
					++counters.cpuFallbackAnimators;
					if(!item.visualEvaluationSuspended) {
						counters.cpuSampledJoints += jointCount;
					}
					break;
				}
				// GPU work is counted from the protected presentation envelope below.
				// A lowering attempt is not necessarily accepted, batched, or recorded by
				// the active graphics backend, so it cannot inflate observable plan counters.
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
				const std::size_t fallbackReason = static_cast<std::size_t>(
					item.fallbackDecision.reason);
				if(fallbackReason < counters.fallbackReasonCounts.size()
					&& item.fallbackDecision.source != AnimationFallbackSource::Current) {
					++counters.fallbackReasonCounts[fallbackReason];
				}
			}
			std::uint32_t compactStageMask = 0U;
			for(std::size_t compactIndex = 0U;
				compactIndex < compactGpuVisualWorkItems.size(); ++compactIndex) {
				const std::size_t inputIndex =
					compactGpuVisualWorkItems.frameWorkIndices[compactIndex];
				if(inputIndex >= capturedInputWorkIndices.size()) continue;
				const CapturedInputWorkIndex& captured = capturedInputWorkIndices[inputIndex];
				if(captured.compact) {
					// Early compact records own no FrameWorkItem; their validated stage
					// mask remains in the compact SoA column through publication.
					if(captured.storageIndex < compactGpuVisualCaptures.size()) {
						compactStageMask |= compactGpuVisualCaptures.stageMasks[captured.storageIndex];
					}
				} else if(captured.storageIndex < frameWork.size()) {
					compactStageMask |= frameWork[captured.storageIndex].gpuStageMask;
				}
			}
			while(compactStageMask != 0U) {
				++counters.compactDispatches;
				compactStageMask &= compactStageMask - 1U;
			}
			const std::shared_ptr<const PublishedGpuAnimationPresentationPlan> presentation =
				AnimationPresentationSystem::Instance().LoadPublishedGpuAnimationPresentationPlan();
			if(presentation && presentation->topology.IsComplete()) {
				snapshot.topologyGeneration = presentation->topology.topologyGeneration;
				// Compact-only crowds do not own FrameWorkItem lowering records. The
				// accepted retained topology is therefore the authoritative renderer
				// generation for the same immutable presentation envelope.
				snapshot.rendererGeneration =
					presentation->topology.generations.rendererGeneration;
				snapshot.timings.topologyBuildMs = presentation->topologyBuildMs;
				snapshot.timings.topologyRefreshMs = presentation->topologyRefreshMs;
				if(presentation->topologyReused) ++counters.retainedTopologyHits;
				else ++counters.retainedTopologyMisses;
			}
			if(presentation && !presentation->safeFallback
				&& presentation->loweringStatus == GpuAnimationLoweringStatus::Ok
				&& presentation->planStatus == GpuAnimationPlanStatus::Ok
				&& presentation->sourceGeneration == publicationGeneration) {
				// Command recording copies each dispatchCommands entry into the backend
				// plan and emits one compute dispatch. Shader payload records may outnumber
				// those actual dispatch calls, so report the executable plan instead.
				counters.gpuBatches = presentation->plan.Batches().size();
				counters.gpuDispatches = presentation->plan.DispatchCommands().size();
			}
			for(const FrameWorkItem& item : frameWork) {
				if(item.gpuLowering && item.gpuLowering->visual.generations.rendererGeneration != 0U) {
					const std::uint64_t candidateRendererGeneration =
						item.gpuLowering->visual.generations.rendererGeneration;
					if(snapshot.rendererGeneration == 0U) {
						snapshot.rendererGeneration = candidateRendererGeneration;
					} else if(snapshot.rendererGeneration != candidateRendererGeneration) {
						// Mixed renderer generations cannot name one retained topology.
						snapshot.rendererGeneration = 0U;
						break;
					}
				}
				if(item.evaluationStatus != AnimationStatus::Ok || !item.evaluated) {
					++counters.rejectedCandidateCount;
				}
			}
			if(counters.rejectedCandidateCount != 0U) {
				snapshot.rejectedGeneration = publicationGeneration;
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
			const std::shared_ptr<const AnimationAuthorityFrame> authority =
				authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority
				? authority->Find(record.handle) : nullptr;
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
				// Consecutive-pose velocity is a future history sidecar responsibility.
				// The current hard cut deliberately does not retain a second row in the
				// mutable runtime record.
				const AnimationAuthorityRow* previous = nullptr;
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
			const std::shared_ptr<const AnimationAuthorityFrame> authority =
				authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority
				? authority->Find(record.handle) : nullptr;
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

		bool HasRagdollExternalLeaseLocked(const RuntimeRecord& record) const noexcept
		{
			const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
			if(!bindings) return false;
			const auto found = std::lower_bound(bindings->active.begin(), bindings->active.end(),
				record.handle, [](const AnimationRootControllerBinding& row,
					AnimatorInstanceHandle handle) { return row.animator < handle; });
			if(found == bindings->active.end() || found->animator != record.handle) return false;
			const auto lease = Physics::PhysicsSystem::Instance()
				.ReadCharacterMotionLease(found->controller);
			return lease && lease->state == Physics::CharacterMotionLeaseState::ExternalExclusive
				&& lease->externalOwnerId == RagdollExternalOwnerId(record.handle);
		}

		void SettleFailedFrameCommandsLocked(AnimationStatus status, const char* fallbackPhase) noexcept
		{
			if(commands == nullptr) return;
			std::size_t unsettled = 0;
			for(const auto& command : capturedCommands) {
				const auto receipt = commands->ReadReceipt(command.producerId, command.producerSequence, command.correlationId);
				if(receipt.status != AnimationStatus::Queued && receipt.status != AnimationStatus::NotReady) continue;
				commands->PublishCompletion(command, status, frameTick, 0);
				++unsettled;
			}
			if(!unsettled) return;
			try {
				// One bounded record survives later successful frames; diagnostic
				// summaries only describe the latest accepted authority publication.
				Logger::Instance().Log(LogLevel::Warning, __FILE__, __LINE__, __func__,
					"Animation command frame failed phase=" + std::string(commandFrameFailurePhase ? commandFrameFailurePhase : fallbackPhase)
					+ " status=" + std::string(ToString(status)) + " commandCount=" + std::to_string(unsettled)
					+ " planReason=" + std::string(ToString(workPlanRejectionReason))
					+ " planned=" + std::to_string(plannedAnimatorCount.load()) + " evaluated=" + std::to_string(evaluatedAnimatorCount.load())
					+ " producer=" + std::to_string(capturedCommands.front().producerId)
					+ " sequence=" + std::to_string(capturedCommands.front().producerSequence));
			} catch(...) { /* Logging cannot alter failed-batch settlement. */ }
		}

		void PublishCommittedOrdinaryCommandsLocked() noexcept
		{
			// ready is set only after the actual protected authority/runtime exchange.
			// It proves a historical commit even if later specialist work is skipped
			// or the next frame changes lifecycle. Never infer it from a planned token.
			const auto& token = publishedAuthorityCommit;
			if(token.ready && token.authorityGeneration != 0)
				PublishCommandCompletionsLocked(token.authorityGeneration);
		}

		AnimationStatus AbortFailedRagdollAdmissionLocked(RuntimeRecord& record) noexcept
		{
			// A terminal failed Activate is not itself a release instruction. This
			// method is reached only for a later explicit Deactivate/RequestRecovery.
			if(record.failedRagdollActivateCorrelationId == 0U
				|| record.failedRagdollActivatePublicationGeneration == 0U)
				return AnimationStatus::NotReady;
			const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
			if(!bindings) return AnimationStatus::NotReady;
			const auto found = std::lower_bound(bindings->active.begin(), bindings->active.end(),
				record.handle, [](const AnimationRootControllerBinding& row,
					AnimatorInstanceHandle handle) { return row.animator < handle; });
			if(found == bindings->active.end() || found->animator != record.handle
				|| !found->bindingValidityGuard
				|| found->bindingValidityGuard->Validate()
					!= Physics::CharacterMotionValidityReason::Valid)
				return AnimationStatus::StaleHandle;
			const auto lease = Physics::PhysicsSystem::Instance()
				.ReadCharacterMotionLease(found->controller);
			if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive
				|| lease->externalOwnerId != RagdollExternalOwnerId(record.handle))
				return AnimationStatus::StaleHandle;
			Physics::CharacterFailedRagdollAdmissionProof proof;
			proof.expectedExternalLease = *lease;
			proof.externalOwnerId = lease->externalOwnerId;
			proof.ragdollId = record.ragdollId;
			proof.failedActivationCorrelationId = record.failedRagdollActivateCorrelationId;
			proof.failedActivationGeneration = record.failedRagdollActivateGeneration;
			proof.failurePublicationGeneration = record.failedRagdollActivatePublicationGeneration;
			proof.failureFrameTick = record.failedRagdollActivateFrameTick;
			proof.frame = found->frame;
			proof.bindingGeneration = found->bindingGeneration;
			proof.bindingGuard = found->bindingValidityGuard;
			const auto released = Physics::PhysicsSystem::Instance()
				.AbortFailedExternalRagdollAdmission(proof);
			if(!released.accepted) return AnimationStatus::NotReady;
			record.failedRagdollActivateCorrelationId = 0U;
			record.failedRagdollActivateGeneration = {};
			record.failedRagdollActivatePublicationGeneration = 0U;
			record.failedRagdollActivateFrameTick = 0;
			return AnimationStatus::Ok;
		}

		AnimationStatus RefreshExternalRagdollRecoveryLocked(RuntimeRecord& record) noexcept
		{
			// A stale recovery ticket preserves the old external owner. Only an
			// explicit new RequestRecovery may replace its inert target pose.
			if(!record.pendingExternalRecoveryToken
				|| !record.pendingExternalRecoveryNeedsRefresh) return AnimationStatus::NotReady;
			const auto bindings = rootControllerBindings.load(std::memory_order_acquire);
			if(!bindings) return AnimationStatus::NotReady;
			const auto found = std::lower_bound(bindings->active.begin(), bindings->active.end(),
				record.handle, [](const AnimationRootControllerBinding& row,
					AnimatorInstanceHandle handle) { return row.animator < handle; });
			if(found == bindings->active.end() || found->animator != record.handle
				|| !found->bindingValidityGuard
				|| found->bindingValidityGuard->Validate()
					!= Physics::CharacterMotionValidityReason::Valid)
				return AnimationStatus::StaleHandle;
			const auto lease = Physics::PhysicsSystem::Instance()
				.ReadCharacterMotionLease(found->controller);
			const auto characterPacket = Physics::PhysicsSystem::Instance()
				.LatestCharacterStatePacket();
			if(!lease || lease->state != Physics::CharacterMotionLeaseState::ExternalExclusive
				|| lease->externalOwnerId != RagdollExternalOwnerId(record.handle)
				|| !characterPacket || !characterPacket->states) return AnimationStatus::StaleHandle;
			const auto character = std::find_if(characterPacket->states->begin(),
				characterPacket->states->end(), [found](const Physics::PhysicsCharacterState& state) {
					return state.world == found->controller.world
						&& state.handle == found->controller.character
						&& state.objectId == found->controller.objectId
						&& state.objectGeneration == found->controller.objectGeneration
						&& state.componentGeneration == found->controller.componentGeneration;
				});
			if(character == characterPacket->states->end()) return AnimationStatus::NotReady;
			const AnimationQuaternion rotation{character->pose.rotation.x,
				character->pose.rotation.y, character->pose.rotation.z,
				character->pose.rotation.w};
			const AnimationFloat3 offset = RotateVector(
				rotation, found->capturedRelativeTranslation);
			Physics::CharacterExternalRecoveryIntent fresh;
			fresh.expectedExternalLease = *lease;
			fresh.externalOwnerId = lease->externalOwnerId;
			fresh.frame = found->frame;
			fresh.alignedControllerPose.position = {
				record.ragdollRecoveryAlignedRootPosition.x - offset.x,
				record.ragdollRecoveryAlignedRootPosition.y - offset.y,
				record.ragdollRecoveryAlignedRootPosition.z - offset.z};
			fresh.alignedControllerPose.rotation = character->pose.rotation;
			fresh.bindingGeneration = found->bindingGeneration;
			fresh.bindingGuard = found->bindingValidityGuard;
			if(!fresh.alignedControllerPose.IsFinite()) return AnimationStatus::InvalidArgument;
			try {
				const auto immutable = std::make_shared<const Physics::CharacterExternalRecoveryIntent>(fresh);
				if(Physics::PhysicsSystem::Instance().RefreshExternalCharacterRecovery(
					*record.pendingExternalRecoveryToken, immutable)
					!= Physics::CharacterExternalRecoveryError::None)
					return AnimationStatus::NotReady;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::HardLimitExceeded;
			}
			record.pendingExternalRecoveryNeedsRefresh = false;
			const auto completed = Physics::PhysicsSystem::Instance()
				.CompleteExternalCharacterRecovery(*record.pendingExternalRecoveryToken);
			if(!completed.accepted) {
				record.pendingExternalRecoveryNeedsRefresh =
					completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected
					&& completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded;
				return AnimationStatus::Queued;
			}
			record.pendingExternalRecoveryToken.reset();
			record.ragdollLifecycle = RagdollLifecycleState::Inactive;
			record.ragdollBlendWeight = 0.0F;
			record.ragdollRecoveryCandidateId = 0U;
			record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
			record.ragdollRecoveryDurationSeconds = 0.0F;
			return AnimationStatus::Ok;
		}

		void PublishRagdollExclusionForRecordLocked(
			const RuntimeRecord& record, std::uint64_t completedCorrelation) noexcept
		{
			try {
				const auto current = ragdollExclusions.load(std::memory_order_acquire);
				if(!current && record.ragdollLifecycle == RagdollLifecycleState::Inactive
					&& record.failedRagdollActivateCorrelationId == 0U) return;
				if(current && current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
					return;
				auto next = std::make_shared<AnimationRagdollExclusionSnapshot>();
				if(current) next->entries = current->entries;
				const auto found = std::find_if(next->entries.begin(), next->entries.end(),
					[handle = record.handle](const AnimationRagdollExclusion& entry) {
						return entry.animator == handle;
					});
				// A newer queued Activate must not be erased by an older completion.
				if(found != next->entries.end()
					&& found->state == AnimationRagdollExclusionState::PendingActivate
					&& found->correlationId != completedCorrelation) return;
				if(record.ragdollLifecycle == RagdollLifecycleState::Inactive
					&& record.failedRagdollActivateCorrelationId == 0U) {
					if(found == next->entries.end()) return;
					next->entries.erase(found);
				} else {
					const AnimationRagdollExclusionState state =
						record.ragdollLifecycle == RagdollLifecycleState::Creating
							? AnimationRagdollExclusionState::PendingActivate
							: AnimationRagdollExclusionState::Active;
					if(found != next->entries.end()) {
						found->state = state;
						found->correlationId = record.pendingRagdollCorrelationId != 0U
							? record.pendingRagdollCorrelationId
							: record.failedRagdollActivateCorrelationId != 0U
								? record.failedRagdollActivateCorrelationId : completedCorrelation;
						found->externalOwnerId = RagdollExternalOwnerId(record.handle);
					} else {
						if(next->entries.size() >= 4096U) return;
						next->entries.push_back({record.handle, state,
							record.failedRagdollActivateCorrelationId != 0U
								? record.failedRagdollActivateCorrelationId : completedCorrelation,
							RagdollExternalOwnerId(record.handle)});
					}
				}
				next->publicationGeneration = current ? current->publicationGeneration + 1U : 1U;
				ragdollExclusions.store(std::move(next), std::memory_order_release);
			} catch(const std::bad_alloc&) {
				// Preserve the previous exclusion on memory pressure. Movement fails
				// closed until a later protected reconciliation succeeds.
			}
		}

		void RejectDrainedFrameCommandsLocked() noexcept
		{
			// A controller reload after Capture invalidates the entire sealed frame.
			// Every queue was already drained, so finish each copied request with a
			// terminal stale receipt while retaining the prior authority/runtime root.
			for(const AnimationCommand& command : capturedCommands)
				commands->PublishCompletion(command, AnimationStatus::StaleHandle, frameTick, 0U);
			for(const NativeAnimationProceduralGoalCommand& command : capturedProceduralCommands)
				proceduralCommands->PublishCompletion(command, AnimationStatus::StaleHandle, frameTick, 0U);
			if(advancedCommands == nullptr) return;
			for(const NativeAnimationRetargetRequest& request : capturedRetargetRequests) {
				NativeAnimationRetargetSnapshot result{};
				result.header = MakeNativeHeader(NativeAnimationRecordKind::RetargetSnapshot, sizeof(result));
				result.status = NativeAnimationStatus::StaleHandle;
				result.animator = request.animator;
				result.profileAssetId = request.profileAssetId;
				result.profileGeneration = request.profileGeneration;
				result.observationTick = frameTick;
				(void)advancedCommands->Publish(result);
			}
			for(const NativeAnimationMotionMatchRequest& request : capturedMotionMatchRequests) {
				NativeAnimationMotionMatchSnapshot result{};
				result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchSnapshot, sizeof(result));
				result.status = NativeAnimationStatus::StaleHandle;
				result.animator = request.animator;
				result.databaseAssetId = request.databaseAssetId;
				result.databaseGeneration = request.databaseGeneration;
				result.queryId = request.queryId;
				result.observationTick = frameTick;
				(void)advancedCommands->Publish(result);
			}
			for(const NativeAnimationMotionMatchQueryRequestV2& request : capturedMotionMatchQueriesV2) {
				NativeAnimationMotionMatchResultV2 result{};
				result.header = MakeNativeHeader(NativeAnimationRecordKind::MotionMatchResultV2, sizeof(result));
				result.status = NativeAnimationStatus::StaleHandle;
				result.gpuStatus = NativeAnimationStatus::StaleHandle;
				result.animator = request.animator;
				result.databaseAssetId = request.databaseAssetId;
				result.databaseGeneration = request.databaseGeneration;
				result.schemaFingerprint = request.schemaFingerprint;
				result.queryId = request.queryId;
				result.observationTick = frameTick;
				result.populatedFields = static_cast<std::uint32_t>(
					NativeAnimationMotionMatchResultField::Identity);
				(void)advancedCommands->Publish(result);
			}
			for(const NativeAnimationRagdollRequest& request : capturedRagdollRequests) {
				NativeAnimationRagdollSnapshot result{};
				result.header = MakeNativeHeader(NativeAnimationRecordKind::RagdollSnapshot, sizeof(result));
				result.status = NativeAnimationStatus::StaleHandle;
				result.animator = request.animator;
				result.profileAssetId = request.profileAssetId;
				result.profileGeneration = request.profileGeneration;
				result.physicsWorldGeneration = request.physicsWorldGeneration;
				result.observationTick = frameTick;
				(void)advancedCommands->Publish(result);
				DropStalePendingRagdollExclusionLocked(
					{request.animator.slotIndex, request.animator.generation}, request.correlationId);
			}
			for(const NativeAnimationDeterminismRequest& request : capturedDeterminismRequests) {
				NativeAnimationDeterminismSnapshot result{};
				result.header = MakeNativeHeader(NativeAnimationRecordKind::DeterminismSnapshot, sizeof(result));
				result.status = NativeAnimationStatus::StaleHandle;
				result.animator = request.animator;
				result.tick = frameTick;
				(void)advancedCommands->Publish(result);
			}
		}

		void DropStalePendingRagdollExclusionLocked(
			AnimatorInstanceHandle animator, std::uint64_t correlationId) noexcept
		{
			try {
				const auto current = ragdollExclusions.load(std::memory_order_acquire);
				if(!current || current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
					return;
				const auto found = std::find_if(current->entries.begin(), current->entries.end(),
					[animator, correlationId](const AnimationRagdollExclusion& entry) {
						return entry.animator == animator
							&& entry.correlationId == correlationId
							&& entry.state == AnimationRagdollExclusionState::PendingActivate;
					});
				if(found == current->entries.end()) return;
				auto next = std::make_shared<AnimationRagdollExclusionSnapshot>(*current);
				next->entries.erase(next->entries.begin()
					+ static_cast<std::ptrdiff_t>(std::distance(current->entries.begin(), found)));
				next->publicationGeneration = current->publicationGeneration + 1U;
				ragdollExclusions.store(std::move(next), std::memory_order_release);
			} catch(const std::bad_alloc&) {
				// Keeping a stale exclusion is safer than accidentally restoring motion.
			}
		}

		void SynchronizeRagdollResultsLocked(
			std::uint64_t publicationGeneration) noexcept
		{
			if(advancedCommands == nullptr) return;
			// Provider Deactivate may have succeeded while the final character
			// teleport/unlease was temporarily unavailable. The Physics ticket
			// retains its immutable pose and lease proof across render frames.
			const auto retryRoot = runtimeRecords.load(std::memory_order_acquire);
			if(retryRoot) retryRoot->ForEach([this, publicationGeneration](
				const std::shared_ptr<RuntimeRecord>& owned) {
				if(!owned || !owned->pendingExternalRecoveryToken
					|| owned->pendingExternalRecoveryNeedsRefresh) return;
				const auto completed = Physics::PhysicsSystem::Instance()
					.CompleteExternalCharacterRecovery(*owned->pendingExternalRecoveryToken);
				if(completed.accepted) {
					owned->pendingExternalRecoveryToken.reset();
					owned->ragdollLifecycle = RagdollLifecycleState::Inactive;
					owned->ragdollBlendWeight = 0.0F;
					owned->ragdollRecoveryCandidateId = 0U;
					owned->ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
					owned->ragdollRecoveryDurationSeconds = 0.0F;
					PublishRagdollExclusionForRecordLocked(*owned, 0U);
					(void)advancedCommands->Publish(MakeRagdollSnapshot(
						*owned, NativeAnimationStatus::Ok, frameTick,
						publicationGeneration));
				} else if(completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected
					&& completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded) {
					owned->pendingExternalRecoveryNeedsRefresh = true;
				}
			});
			const std::shared_ptr<const RagdollPhysicsResultSnapshot> results =
				Physics::PhysicsSystem::Instance().LatestRagdollPhysicsResults();
			if(!results || !results->results) return;
			for(const RagdollPhysicsResult& result : *results->results) {
				const std::shared_ptr<RuntimeRecordRoot> root = runtimeRecords.load(std::memory_order_acquire);
				if(root == nullptr) continue;
				root->ForEach([&](const std::shared_ptr<RuntimeRecord>& owned) {
					if(!owned || owned->ragdollId != result.ragdollId
						|| owned->pendingRagdollCorrelationId == 0U
						|| owned->pendingRagdollCorrelationId != result.correlationId) return;
					RuntimeRecord& record = *owned;
					AnimationStatus snapshotStatus = result.status;
					if(result.status == AnimationStatus::Ok) {
						record.ragdollGeneration = result.generation;
						switch(record.pendingRagdollOperation) {
						case NativeAnimationRagdollOperation::Activate:
							record.ragdollCreated = true;
							record.failedRagdollActivateCorrelationId = 0U;
							record.failedRagdollActivateGeneration = {};
							record.failedRagdollActivatePublicationGeneration = 0U;
							record.failedRagdollActivateFrameTick = 0;
							record.ragdollLifecycle = RagdollLifecycleState::Active;
							record.ragdollLifecycleStartTick = result.committedStep;
							record.ragdollRecoveryBlockReason =
								RagdollRecoveryBlockReason::None;
							break;
						case NativeAnimationRagdollOperation::Deactivate:
							if(record.pendingRagdollExternalRecovery
								&& record.pendingRagdollPreviousLifecycle
									== RagdollLifecycleState::Recovering) {
								Physics::RagdollExternalRecoveryToken token;
								token.world = Physics::PhysicsSystem::Instance().WorldHandle();
								token.ragdollId = result.ragdollId;
								token.correlationId = result.correlationId;
								token.deactivateGeneration = result.generation;
								token.committedStep = result.committedStep;
								const auto completed = Physics::PhysicsSystem::Instance()
									.CompleteExternalCharacterRecovery(token);
								if(completed.accepted) {
									record.ragdollLifecycle = RagdollLifecycleState::Inactive;
									record.ragdollBlendWeight = 0.0F;
									record.ragdollRecoveryCandidateId = 0U;
									record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
									record.ragdollRecoveryDurationSeconds = 0.0F;
									record.pendingExternalRecoveryToken.reset();
									record.pendingExternalRecoveryNeedsRefresh = false;
								} else {
									record.pendingExternalRecoveryToken = token;
									record.ragdollLifecycle = RagdollLifecycleState::TearingDown;
									// The provider stopped the ragdoll, but the character is
									// still physically ExternalExclusive. Do not advertise a
									// completed recovery until the retained ticket grants it.
									snapshotStatus = AnimationStatus::NotReady;
									record.pendingExternalRecoveryNeedsRefresh =
										completed.error != Physics::CharacterExternalRecoveryError::ProviderRejected
										&& completed.error != Physics::CharacterExternalRecoveryError::CapacityExceeded;
								}
							} else {
								record.ragdollLifecycle = RagdollLifecycleState::Inactive;
								record.ragdollBlendWeight = 0.0F;
								record.ragdollRecoveryCandidateId = 0U;
								record.ragdollRecoveryBlockReason = RagdollRecoveryBlockReason::None;
								record.ragdollRecoveryDurationSeconds = 0.0F;
							}
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
						if(record.pendingRagdollOperation
							== NativeAnimationRagdollOperation::Activate) {
							record.failedRagdollActivateCorrelationId = result.correlationId;
							record.failedRagdollActivateGeneration = result.generation;
							record.failedRagdollActivatePublicationGeneration = publicationGeneration;
							record.failedRagdollActivateFrameTick = result.committedStep;
						}
					}
					(void)advancedCommands->Publish(MakeRagdollSnapshot(
						record, ToNativeStatus(snapshotStatus), result.committedStep,
						publicationGeneration));
					record.pendingRagdollCorrelationId = 0U;
					record.pendingRagdollPreviousGeneration = {};
					record.pendingRagdollExternalRecovery = false;
					PublishRagdollExclusionForRecordLocked(record, result.correlationId);
				});
			}
		}

		void PublishAdvancedCommandCompletionsLocked(
			std::uint64_t publicationGeneration) noexcept
		{
			if(advancedCommands == nullptr || publicationGeneration == 0U) return;
			SynchronizeRagdollResultsLocked(publicationGeneration);
			const auto findPreEvaluationPublication = [this](
				NativeAnimationAnimatorHandle animator) -> std::shared_ptr<const AnimationAuthorityRow> {
				const auto work = std::find_if(frameWork.begin(), frameWork.end(),
					[animator](const FrameWorkItem& item) {
						return item.input.handle.slotIndex == animator.slotIndex
							&& item.input.handle.generation == animator.generation;
					});
				// FrameWorkItem owns an immutable pointer captured before worker evaluation.
					// Completion runs after the sealed authority state commits, so
				// only this retained pointer can validate the generation the producer saw.
				return work == frameWork.end() ? std::shared_ptr<const AnimationAuthorityRow>{}
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
					const std::shared_ptr<const AnimationAuthorityFrame> authority =
						authorityPublisher.Load();
					const AnimationAuthorityRow* published = authority
						? authority->Find(record->handle) : nullptr;
					const std::shared_ptr<const AnimationAuthorityRow> preEvaluationPublication =
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
					DropStalePendingRagdollExclusionLocked(
						{request.animator.slotIndex, request.animator.generation},
						request.correlationId);
					continue;
				}

				AnimationStatus status = AnimationStatus::NotReady;
				bool recoveryBlocked = false;
				if(record->pendingRagdollCorrelationId != 0U) {
					status = AnimationStatus::NotReady;
				} else if(request.operation == NativeAnimationRagdollOperation::RequestRecovery
					&& record->pendingExternalRecoveryToken) {
					status = record->pendingExternalRecoveryNeedsRefresh
						? RefreshExternalRagdollRecoveryLocked(*record)
						: AnimationStatus::Queued;
				} else if(record->failedRagdollActivateCorrelationId != 0U
					&& (request.operation == NativeAnimationRagdollOperation::RequestRecovery
						|| request.operation == NativeAnimationRagdollOperation::Deactivate)) {
					status = AbortFailedRagdollAdmissionLocked(*record);
				} else if(request.operation == NativeAnimationRagdollOperation::RequestRecovery) {
					status = BeginRagdollRecoveryLocked(
						*record, request, recoveryBlocked);
				} else if(request.operation == NativeAnimationRagdollOperation::Activate) {
					PhysicsPoseSnapshot activationPose;
					RagdollGenerationStamp generation;
					// SubmitRagdollRequest published PendingActivate already. Recheck
					// the copied controller table here so Physics cannot activate a body
					// while its character still accepts velocity or root movement.
					const auto rootBindings = rootControllerBindings.load(std::memory_order_acquire);
					const auto exclusions = ragdollExclusions.load(std::memory_order_acquire);
					status = rootBindings && exclusions
						? ApplyRagdollControllerExclusions(*rootBindings, *exclusions,
							Physics::PhysicsSystem::Instance()) : AnimationStatus::Ok;
					if(status == AnimationStatus::Ok)
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
				if(request.operation == NativeAnimationRagdollOperation::Activate
					&& status != AnimationStatus::Queued && status != AnimationStatus::Ok) {
					record->failedRagdollActivateCorrelationId = request.correlationId;
					record->failedRagdollActivateGeneration = {};
					record->failedRagdollActivatePublicationGeneration = publicationGeneration;
					record->failedRagdollActivateFrameTick = frameTick;
				}
				if(status != AnimationStatus::Queued)
					PublishRagdollExclusionForRecordLocked(*record, request.correlationId);
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
							const std::shared_ptr<const AnimationAuthorityFrame> authority =
								authorityPublisher.Load();
							const AnimationAuthorityRow* published = authority
								? authority->Find(record->handle) : nullptr;
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
							const std::shared_ptr<const AnimationAuthorityFrame> authority =
								authorityPublisher.Load();
							const AnimationAuthorityRow* published = authority
								? authority->Find(record->handle) : nullptr;
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
                                                if (status ==
                                                    AnimationStatus::
                                                        DeterminismViolation) {
                                                  snapshot.divergenceKind = 1U;
                                                }
                                        }
                                }
                                snapshot.status = ToNativeStatus(status);
                                (void)advancedCommands->Publish(
                                    std::move(snapshot));
                        }
                }

                [[nodiscard]] std::shared_ptr<
                    const GpuAnimationPresentationFrame>
                BuildGpuPresentationFrame(
                    std::uint64_t sourceGeneration,
                    std::shared_ptr<const AnimationAuthorityFrame>
                        sealedAuthority = {}) noexcept {
                  if (sourceGeneration == 0U)
                    return {};
                  const std::shared_ptr<const AnimationAuthorityFrame>
                      authority = sealedAuthority ? std::move(sealedAuthority)
                                                  : authorityPublisher.Load();
                  if (!authority || !authority->IsValid() ||
                      authority->authorityGeneration != sourceGeneration)
                    return {};
                  try {
                    std::shared_ptr<GpuAnimationPresentationFrame> frame;
                    for (std::size_t attempt = 0U;
                         attempt < gpuPresentationFramePool.size(); ++attempt) {
                      const std::size_t slot =
                          (gpuPresentationFramePoolCursor + attempt) %
                          gpuPresentationFramePool.size();
                      std::shared_ptr<GpuAnimationPresentationFrame>
                          &candidate = gpuPresentationFramePool[slot];
                      if (!candidate)
                        candidate =
                            std::make_shared<GpuAnimationPresentationFrame>();
                      if (candidate.use_count() == 1U) {
                        frame = candidate;
                        gpuPresentationFramePoolCursor =
                            (slot + 1U) % gpuPresentationFramePool.size();
                        break;
                      }
                    }
					if (!frame)
					  frame = std::make_shared<GpuAnimationPresentationFrame>();
					frame->inputs.clear();
					frame->retainedStaticPlan.reset();
					frame->preparedDynamicLanes.ResetForReuse();
					frame->preparedEvaluationFingerprints.clear();
					frame->preparedDynamicPlanComplete = false;
					frame->ownerObjectMappings.clear();
                    frame->authority = authority;
                    // This system runs before Physics and can therefore provide
                    // only the authority-bound GPU source.
                    // AnimationPresentationSystem attaches the typed
                    // post-physics boundary after the Physics workload
                    // completes.
                    frame->sourceGeneration = sourceGeneration;
                    frame->planGeneration =
                        static_cast<std::uint32_t>(sourceGeneration);
                    if (frame->planGeneration == 0U)
                      frame->planGeneration = 1U;
					frame->inputs.reserve(frameWork.size() +
						compactGpuVisualCaptures.size());
					frame->ownerObjectMappings.reserve(
                        frameWork.size() + compactGpuVisualCaptures.size());
                    // Stable owner identity is independent of the animated
                    // sample. Hash it while the mandatory mapping pass is
                    // already cache-hot so presentation can reuse its sorted
                    // binding and packet-lookup roots on later frames.
                    std::uint64_t ownerMappingSignature =
                        1469598103934665603ULL;
					GpuAnimationLoweringTopologySignatureBuilder
						topologySignatureBuilder;
					// Capture the original immutable topology root once. The producer is
					// already about to visit every current input for owner/signature sealing,
					// so it can populate the changing generation/tick lanes in the same pass.
					// Presentation then performs only scalar proof checks on the warm path.
					const std::shared_ptr<const PublishedGpuAnimationPresentationPlan>
						previousPresentation = AnimationPresentationSystem::Instance()
							.LoadPublishedGpuAnimationPresentationPlan();
					std::shared_ptr<const GpuAnimationPlan> retainedStaticPlan;
					if(previousPresentation && !previousPresentation->safeFallback) {
						retainedStaticPlan = previousPresentation->plan.retainedStaticPlan
							? previousPresentation->plan.retainedStaticPlan
							: std::shared_ptr<const GpuAnimationPlan>(
								previousPresentation, &previousPresentation->plan);
					}
					const GpuAnimationPlan* staticPlan = retainedStaticPlan.get();
					bool preparedDynamicPlanComplete = staticPlan != nullptr
						&& staticPlan->generation != 0U
						&& staticPlan->topologyInputSignature != 0U
						&& staticPlan->StablePacketIndices().size()
							== staticPlan->WorkPackets().size();
					std::size_t preparedPacketCount = 0U;
					const bool deferStableEnvelope = preparedDynamicPlanComplete;
					std::size_t gpuInputCount = 0U;
					if(preparedDynamicPlanComplete) {
						const std::size_t packetCount = staticPlan->WorkPackets().size();
						frame->preparedDynamicLanes.generationRecords.resize(
							staticPlan->GenerationRecordCount());
						frame->preparedDynamicLanes.sampleTicks.resize(packetCount);
						frame->preparedDynamicLanes.renderIdentities.resize(packetCount);
						frame->preparedEvaluationFingerprints.resize(packetCount);
					}
					const auto appendPreparedDynamicLane = [&] (
						const GpuAnimationVisualInput& visual,
						const EvaluationProgram* program,
						std::uint64_t programContentId,
						std::uint32_t stageMask,
						bool residentPayloadsValidated) noexcept {
						if(!preparedDynamicPlanComplete) return;
						const auto& packets = staticPlan->WorkPackets();
						std::uint32_t packetIndex = 0U;
						if(!staticPlan->FindWorkPacketIndex(
								visual.stableInstanceId,
								visual.stableSubmissionOrdinal,
								packetIndex)
							|| packetIndex >= packets.size()
							|| !residentPayloadsValidated
							|| program == nullptr
							|| program->stableHash != programContentId
							|| program->compatibilityFingerprint
								!= visual.compatibilityFingerprint
							|| !IsGpuAnimationGenerationRecordValid(visual.generations)
							|| !IsGpuAnimationRenderIdentityValid(visual.renderIdentity)) {
							preparedDynamicPlanComplete = false;
							return;
						}
						const GpuAnimationWorkPacket& packet = packets[packetIndex];
						if(stageMask != packet.stageMask
							|| packet.generationRecordIndex
								>= frame->preparedDynamicLanes.generationRecords.size()
							|| IsGpuAnimationGenerationRecordValid(
								frame->preparedDynamicLanes.generationRecords[
									packet.generationRecordIndex])) {
							// A repeated owner would overwrite one lane and leave another absent.
							// Reject it while the mandatory producer pass is already at that row.
							preparedDynamicPlanComplete = false;
							return;
						}
						GpuAnimationGenerationRecord generation = visual.generations;
						if(visual.suspendVisualEvaluation) {
							generation.flags |=
								kGpuAnimationGenerationFlagSuspendVisualEvaluation;
						} else {
							generation.flags &=
								~kGpuAnimationGenerationFlagSuspendVisualEvaluation;
						}
						generation.reserved = visual.allowVisualRagdoll
							? visual.ragdollTableOffsetBytes : 0U;
						frame->preparedDynamicLanes.generationRecords[
							packet.generationRecordIndex] = generation;
						frame->preparedDynamicLanes.sampleTicks[packetIndex] =
							visual.sampleTick;
						frame->preparedDynamicLanes.renderIdentities[packetIndex] =
							visual.renderIdentity;
						// Evaluation sharing also depends on generation equality. Collapse its
						// dynamic half into one CPU-only proof value; static resource selectors,
						// packet flags, and batch layout remain covered by the topology signature.
						std::uint64_t evaluationFingerprint =
							visual.evaluationParameterFingerprint;
						const auto mixEvaluation = [&evaluationFingerprint](
							std::uint64_t value) noexcept {
							evaluationFingerprint ^= value;
							evaluationFingerprint *= 1099511628211ULL;
						};
						mixEvaluation(generation.programGeneration);
						mixEvaluation(generation.controllerGeneration);
						mixEvaluation(generation.skeletonGeneration);
						mixEvaluation(generation.clipGeneration);
						mixEvaluation(generation.meshGeneration);
						mixEvaluation(generation.resourceGeneration);
						mixEvaluation(generation.rendererGeneration);
						mixEvaluation(generation.sceneGeneration);
						frame->preparedEvaluationFingerprints[packetIndex] =
							evaluationFingerprint;
						++preparedPacketCount;
					};
					const auto materializeCompactLowering = [] (
						const CompactGpuVisualCaptures& compact,
						std::size_t compactIndex) {
						const CompactGpuVisualCohort& cohort = compact.cohort(compactIndex);
						GpuAnimationLoweringInput lowering;
						lowering.program.program = cohort.program;
						lowering.program.programContentId = cohort.program->stableHash;
						lowering.program.programResourceIndex =
							cohort.fastBinding->programResourceIndex;
						lowering.visual = compact.gpuPresentationVisuals[compactIndex];
						lowering.effectiveMode =
							compact.executionDecisions[compactIndex].effectiveMode;
						lowering.programValidated = true;
						lowering.prevalidatedStageMask = compact.stageMasks[compactIndex];
						lowering.residentPayloads = cohort.fastBinding->residentPayloads;
						lowering.residentPayloadsValidated = true;
						lowering.allowSharedVisualEvaluation = true;
						return lowering;
					};
                    const auto includeOwnerMapping =
                        [&ownerMappingSignature](
                            const GpuAnimationOwnerObjectMapping &mapping) {
                          const auto combine =
                              [&ownerMappingSignature](std::uint64_t value) {
                                ownerMappingSignature ^= value;
                                ownerMappingSignature *= 1099511628211ULL;
                              };
                          combine(static_cast<std::uint64_t>(
                              std::hash<UUID>{}(mapping.ownerObjectId)));
                          combine(mapping.stableInstanceId);
                          combine(mapping.instanceSlot);
                          combine(mapping.instanceGeneration);
                          combine(mapping.componentGeneration);
                        };
                    for (FrameWorkItem &item : frameWork) {
                      if (!item.gpuLowering)
                        continue;
                      // Diagnostics have already consumed the frame-local
                      // decision by the time this handoff runs. Capture the
                      // owner key before moving the lowering input: a failed
                      // mapping must not depend on an unspecified moved-from
                      // value and accidentally discard the entire presentation
                      // frame.
                      GpuAnimationOwnerObjectMapping mapping{};
                      mapping.ownerObjectId =
                          item.input.descriptor.ownerObjectId;
                      mapping.stableInstanceId =
                          item.gpuLowering->visual.stableInstanceId;
                      mapping.instanceSlot = item.input.handle.slotIndex;
                      mapping.instanceGeneration = item.input.handle.generation;
                      mapping.componentGeneration =
                          item.input.descriptor.componentGeneration;
                      if (!mapping.IsComplete())
                        return {};
					  includeOwnerMapping(mapping);
					  topologySignatureBuilder.Include(*item.gpuLowering);
					  appendPreparedDynamicLane(
						  item.gpuLowering->visual,
						  item.gpuLowering->program.program.get(),
						  item.gpuLowering->program.programContentId,
						  item.gpuLowering->prevalidatedStageMask,
						  item.gpuLowering->residentPayloadsValidated);
                      // Transfer immutable owners only after the complete
                      // mapping proof. This lets presentation distinguish a
                      // valid empty GPU source from a rejected owner identity
                      // without exposing partially moved frame contents.
					  ++gpuInputCount;
					  if(!deferStableEnvelope) {
						  frame->inputs.push_back(std::move(*item.gpuLowering));
						  frame->ownerObjectMappings.push_back(mapping);
					  }
                    }
                    for (std::size_t compactIndex = 0U;
                         compactIndex < compactGpuVisualCaptures.size();
                         ++compactIndex) {
                      CompactGpuVisualCaptures &compact =
                          compactGpuVisualCaptures;
                      if (compact.processingClasses[compactIndex] !=
                          AnimationProcessingClass::CompactGpuVisual)
                        continue;
                      GpuAnimationOwnerObjectMapping mapping{};
                      mapping.ownerObjectId =
                          compact.inputs[compactIndex].ownerObjectId;
                      mapping.stableInstanceId =
                          compact.gpuPresentationVisuals[compactIndex]
                              .stableInstanceId;
                      mapping.instanceSlot =
                          compact.inputs[compactIndex].handle.slotIndex;
                      mapping.instanceGeneration =
                          compact.inputs[compactIndex].handle.generation;
                      mapping.componentGeneration =
                          compact.inputs[compactIndex]
                              .componentGeneration;
                      if (!mapping.IsComplete())
                        return {};
                      includeOwnerMapping(mapping);
					  const CompactGpuVisualCohort& cohort =
						  compact.cohort(compactIndex);
					  const GpuAnimationVisualInput& visual =
						  compact.gpuPresentationVisuals[compactIndex];
					  topologySignatureBuilder.IncludePrepared(
						  cohort.program->stableHash,
						  cohort.fastBinding->programResourceIndex,
						  visual,
						  compact.stageMasks[compactIndex],
						  compact.executionDecisions[compactIndex].effectiveMode,
						  true);
					  appendPreparedDynamicLane(
						  visual,
						  cohort.program.get(),
						  cohort.program->stableHash,
						  compact.stageMasks[compactIndex],
						  true);
                      // A prior no-view snapshot cannot suppress this packet:
                      // custom and editor captures can become visible after
                      // animation capture has completed. Keep every proven
                      // compact owner in the immutable presentation frame and
                      // let renderer-side culling decide whether it is
                      // submitted this frame. Presentation is the sole
                      // remaining consumer after diagnostics, so move the
                      // lowering instead of performing thousands of
                      // shared-ownership bumps.
					  ++gpuInputCount;
					  if(!deferStableEnvelope) {
						  frame->inputs.push_back(
							  materializeCompactLowering(compact, compactIndex));
						  frame->ownerObjectMappings.push_back(mapping);
					  }
					}
					frame->ownerMappingSignature =
						gpuInputCount == 0U
							? 0U
							: ownerMappingSignature;
					frame->topologyInputSignature = gpuInputCount == 0U
						? 0U : topologySignatureBuilder.Complete();
					// Retaining an extra evaluation after two private owners converge is
					// conservative. Diverging a formerly shared owner is not: it would read
					// another owner's pose. Verify only the latter relationship in one tight
					// scalar pass, without relowering requests or touching resource payloads.
					if(preparedDynamicPlanComplete) {
						const auto& evaluationSources =
							staticPlan->EvaluationSourcePacketIndices();
						if(evaluationSources.size() != preparedPacketCount) {
							preparedDynamicPlanComplete = false;
						} else {
							for(std::size_t packetIndex = 0U;
								packetIndex < evaluationSources.size(); ++packetIndex) {
								const std::uint32_t sourceIndex = evaluationSources[packetIndex];
								if(sourceIndex >= evaluationSources.size()
									|| (sourceIndex != packetIndex
										&& (frame->preparedEvaluationFingerprints[packetIndex]
												!= frame->preparedEvaluationFingerprints[sourceIndex]
											|| frame->preparedDynamicLanes.sampleTicks[packetIndex]
												!= frame->preparedDynamicLanes.sampleTicks[sourceIndex]))) {
									preparedDynamicPlanComplete = false;
									break;
								}
							}
						}
					}
					preparedDynamicPlanComplete = preparedDynamicPlanComplete
						&& preparedPacketCount == staticPlan->WorkPackets().size()
						&& frame->topologyInputSignature
							== staticPlan->topologyInputSignature
						&& frame->preparedDynamicLanes.HasParallelLengths(
							staticPlan->WorkPackets().size());
					if(preparedDynamicPlanComplete) {
						frame->preparedEvaluationFingerprints.clear();
						frame->retainedStaticPlan = std::move(retainedStaticPlan);
						frame->preparedDynamicPlanComplete = true;
					} else {
						if(deferStableEnvelope) {
							// A topology proof may fail after the producer has already visited
							// part of the population. Build the complete cold envelope only in
							// that uncommon case; warmed frames retain only dynamic lanes.
							frame->inputs.reserve(gpuInputCount);
							frame->ownerObjectMappings.reserve(gpuInputCount);
							for(FrameWorkItem& item : frameWork) {
								if(!item.gpuLowering) continue;
								GpuAnimationOwnerObjectMapping mapping{};
								mapping.ownerObjectId = item.input.descriptor.ownerObjectId;
								mapping.stableInstanceId = item.gpuLowering->visual.stableInstanceId;
								mapping.instanceSlot = item.input.handle.slotIndex;
								mapping.instanceGeneration = item.input.handle.generation;
								mapping.componentGeneration = item.input.descriptor.componentGeneration;
								if(!mapping.IsComplete()) return {};
								frame->inputs.push_back(std::move(*item.gpuLowering));
								frame->ownerObjectMappings.push_back(mapping);
							}
							for(std::size_t compactIndex = 0U;
								compactIndex < compactGpuVisualCaptures.size(); ++compactIndex) {
								CompactGpuVisualCaptures& compact = compactGpuVisualCaptures;
								if(compact.processingClasses[compactIndex]
									!= AnimationProcessingClass::CompactGpuVisual) continue;
								GpuAnimationOwnerObjectMapping mapping{};
								mapping.ownerObjectId = compact.inputs[compactIndex].ownerObjectId;
								mapping.stableInstanceId = compact.gpuPresentationVisuals[compactIndex].stableInstanceId;
								mapping.instanceSlot = compact.inputs[compactIndex].handle.slotIndex;
								mapping.instanceGeneration = compact.inputs[compactIndex].handle.generation;
								mapping.componentGeneration =
									compact.inputs[compactIndex].componentGeneration;
								if(!mapping.IsComplete()) return {};
								frame->inputs.push_back(
									materializeCompactLowering(compact, compactIndex));
								frame->ownerObjectMappings.push_back(mapping);
							}
						}
						frame->preparedDynamicLanes.ResetForReuse();
						frame->preparedEvaluationFingerprints.clear();
					}
					return frame;
                  } catch (const std::bad_alloc &) {
                    // CPU authority has already committed.  A
                    // render-presentation allocation failure must remain an
                    // isolated CPU fallback for this frame.
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
			impl_->qualifiedControllerResourceRevision = 0U;
			impl_->capturedCommandStatuses.clear();
			impl_->capturedProceduralCommands.clear();
			impl_->capturedProceduralCommandStatuses.clear();
			impl_->capturedProceduralGoalRanges.clear();
			impl_->capturedProceduralGoals.clear();
			if(impl_->proceduralCommands != nullptr) impl_->proceduralCommands->Clear();
			impl_->capturedRetargetRequests.clear();
			impl_->capturedMotionMatchRequests.clear();
			impl_->capturedMotionMatchQueriesV2.clear();
			impl_->capturedRagdollRequests.clear();
			impl_->capturedDeterminismRequests.clear();
			if(impl_->advancedCommands != nullptr) impl_->advancedCommands->Clear();
			impl_->pendingRootMotionApplications.clear();
			impl_->frameWork.clear();
			impl_->candidateFrameWork.clear();
			impl_->gpuPresentationFramePool = {};
			impl_->gpuPresentationFramePoolCursor = 0U;
			impl_->runtimeRecords.store(std::make_shared<Impl::RuntimeRecordRoot>(), std::memory_order_release);
			impl_->compactRuntimeStates.store(
				std::make_shared<Impl::CompactRuntimeStateRoot>(),
				std::memory_order_release);
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
			const AnimationLifecycleGenerationSnapshot lifecycle = impl_->lifecycleLatch
				? impl_->lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
			impl_->authorityPublisher.Clear(impl_->runtimeSessionEpoch.load(std::memory_order_acquire),
				lifecycle.Generation(AnimationLifecycleBoundary::Scene));
			impl_->ResetFrameIntegrationStateLocked();
			impl_->evaluationCache.InvalidateAll();
			impl_->memoryController.Shutdown();
			impl_->capturedFrameInputs.clear();
			impl_->capturedCommands.clear();
			impl_->qualifiedControllerResourceRevision = 0U;
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
			impl_->candidateFrameWork.clear();
			impl_->rollbackHistory.ClearAll();
			impl_->hadScheduledAnimatorMembership = false;
			impl_->rootControllerBindings.store({}, std::memory_order_release);
			impl_->ragdollExclusions.store({}, std::memory_order_release);
			if(impl_->runtimeActive.exchange(false, std::memory_order_acq_rel)) {
				// Make dormant membership visible before invalidating the protected
				// store, then retire all handles while producers remain behind the guard.
				impl_->instances->InvalidateAll();
				impl_->commands.reset();
				impl_->runtimeRecords.store(std::make_shared<Impl::RuntimeRecordRoot>(), std::memory_order_release);
				impl_->compactRuntimeStates.store(
					std::make_shared<Impl::CompactRuntimeStateRoot>(),
					std::memory_order_release);
				impl_->clipContentFingerprints.clear();
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
			kResourceAnimationRollbackState,
			kResourceAnimatedObjectVisibilitySnapshot,
			kResourceLocomotionAnimatorBindings,
			kResourceRootMotionResults,
			kResourceAnimationRagdollMappings,
			kResourcePhysicsCharacterMotionLeases,
			"PhysicsWorld",
			"RagdollPhysicsSnapshots",
			"RagdollPhysicsResults"
		};
		declaration.writes = {
			kResourceAnimationFrameInputs,
			kResourceAnimationExecutionModes,
			kResourceAnimationProcessingClasses,
			kResourceAnimationConsumerMasks,
			kResourceAnimationWorkPackets,
			kResourceAnimationVisualPackets,
			kResourceAnimationGpuVisualFastPackets,
			kResourceAnimationGpuVisualFastPrepared,
			kResourceAnimationAuthoritativeOutputs,
			kResourceAnimationAuthorityPages,
			kResourceAnimationFrameSidecars,
			kResourceAnimationAuthorityFrame,
			kResourceAnimationEvaluationCache,
			kResourceAnimationRuntimeState,
			kResourceAnimationPhysicsHandoffQueue,
			kResourceAnimationManagedCallbackBatches,
			kResourceRootMotionIntervals,
			kResourceRootMotionResults,
			kResourcePhysicsCharacterMotionLeases,
			kResourceAnimationRagdollMappings,
			"PhysicsWorld",
			"RagdollPhysicsQueue",
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
				const auto bindingStart = std::chrono::steady_clock::now();
				const AnimatorComponentSynchronizationSummary binding =
					impl_->componentBindings.Synchronize(
						*this, objects, ResourceManager::Instance());
				const auto bindingNanoseconds =
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - bindingStart).count();
				// Stable frames intentionally return a zero-work summary. Retain the
				// latest real reconciliation instead of replacing it with a dormant poll.
				if(binding.reconciledBindingCount != 0U) {
					impl_->bindingReconciliationNanoseconds.store(
						static_cast<std::uint64_t>(std::max<std::int64_t>(0, bindingNanoseconds)),
						std::memory_order_release);
					impl_->bindingReconciledCount.store(
						binding.reconciledBindingCount, std::memory_order_release);
					impl_->bindingFailureCount.store(
						binding.failedCount, std::memory_order_release);
					impl_->bindingEpisodeCount.fetch_add(1U, std::memory_order_acq_rel);
				}
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
				const AnimationLifecycleGenerationSnapshot lifecycle = impl_->lifecycleLatch
					? impl_->lifecycleLatch->Capture() : AnimationLifecycleGenerationSnapshot{};
				// The first dormant frame retires the complete immutable authority root;
				// later static frames retain the allocation-free dormant fast path.
				impl_->authorityPublisher.Clear(
					impl_->runtimeSessionEpoch.load(std::memory_order_acquire),
					lifecycle.Generation(AnimationLifecycleBoundary::Scene));
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
				// Last-membership retirement also releases the copied command batch.
				impl_->PublishCommittedOrdinaryCommandsLocked();
				impl_->SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "DormantRetirement");
				impl_->ResetFrameIntegrationStateLocked();
				impl_->capturedFrameInputs.clear();
				impl_->capturedCommands.clear();
				impl_->qualifiedControllerResourceRevision = 0U;
				impl_->capturedCommandStatuses.clear();
				impl_->pendingRootMotionApplications.clear();
				impl_->frameWork.clear();
				// Dormancy retires the reusable candidate as well as the active view so
				// old asset owners do not remain pinned until another animator appears.
				impl_->candidateFrameWork.clear();
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

		std::array<FrameWorkloadDeclaration, 9> descriptors;
		ConfigureProtectedWorkload(
			descriptors[0],
			"CaptureAnimationInputs",
			WorkerSystemPhaseKind::Snapshot,
			{kResourceAnimatorComponents, kResourceAnimationCommandQueues, "RuntimeTime", "RuntimeLayout",
				"Resources", "SceneGraph", "ObjectManager",
				kResourceAnimatedObjectVisibilitySnapshot,
				kResourceLocomotionAnimatorBindings, kResourceAnimationRagdollMappings},
			{kResourceAnimationFrameInputs, kResourceAnimationCommandQueues},
			"Live animator membership, component configuration, commands, transforms, and resource views are copied before worker access.");
		ConfigureWorkerWorkload(
			descriptors[1],
			"PlanAnimationEvaluation",
			{kResourceAnimationFrameInputs, kResourceAnimationEvaluationPrograms, kResourceAnimationResidencyCapabilities},
			{kResourceAnimationExecutionModes, kResourceAnimationProcessingClasses,
				kResourceAnimationConsumerMasks, kResourceAnimationWorkPackets,
				kResourceAnimationVisualPackets, kResourceAnimationGpuVisualFastPackets},
			"Reads immutable frame/program/capability views and writes only deterministic owned plan ranges.");
		ConfigureWorkerWorkload(
			descriptors[2],
			"FinalizeAnimationWorkPlan",
			{kResourceAnimationExecutionModes, kResourceAnimationProcessingClasses,
				kResourceAnimationConsumerMasks, kResourceAnimationWorkPackets,
				kResourceAnimationVisualPackets, kResourceAnimationGpuVisualFastPackets},
			{kResourceAnimationWorkPackets, kResourceAnimationGpuVisualFastPackets},
			"Builds the complete scheduler plan and frozen compact packets after every planning range has completed.");
		// Finalization is a single atomic plan build. It must never be partitioned,
		// because evaluation ranges consume one shared ordering and scratch layout.
		descriptors[2].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[2].partitionWorkItemCount = 0U;
		descriptors[2].inlineWorkItemThreshold = 0U;
		ConfigureWorkerWorkload(
			descriptors[3],
			"EvaluateCompleteAnimation",
			{kResourceAnimationWorkPackets, kResourceAnimationClipSegments, kResourceAnimationRollbackState},
			{kResourceAnimationAuthoritativeOutputs},
			"Reads immutable complete packets and writes range-disjoint authoritative output without live scene or component mutation.");
		ConfigureWorkerWorkload(
			descriptors[4],
			"EvaluateCompactGpuVisualAnimation",
			{kResourceAnimationGpuVisualFastPackets},
			{kResourceAnimationGpuVisualFastPrepared},
			"Reads minimal visual-only packets and writes disjoint compact completions without entering the complete evaluator.");
		ConfigureWorkerWorkload(
			descriptors[5],
			"SealAnimationAuthority",
			{kResourceAnimationAuthoritativeOutputs, kResourceAnimationGpuVisualFastPrepared},
			{kResourceAnimationAuthorityPages, kResourceAnimationFrameSidecars},
			"Merges final immutable authority pages and ordered sparse sidecars, reserves every fallible candidate value, and freezes one root candidate.");
		descriptors[5].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[5].partitionWorkItemCount = 0U;
		descriptors[5].inlineWorkItemThreshold = 0U;
		ConfigureProtectedWorkload(
			descriptors[6],
			"PublishAnimationAuthority",
			WorkerSystemPhaseKind::ProtectedCommit,
			{kResourceAnimationAuthorityPages, kResourceAnimationFrameSidecars,
				kResourceAnimationAuthorityFrame, "Resources"},
			{kResourceAnimationAuthorityFrame},
			"Performs the constant-time session/predecessor check and exchanges one sealed authority root.");
		ConfigureWorkerWorkload(
			descriptors[7],
			"CommitPublishedAnimationEvaluationCache",
			{kResourceAnimationAuthorityFrame},
			{kResourceAnimationEvaluationCache},
			"Commits generation-keyed optional cache entries on a worker only after the matching authority root is accepted.");
		descriptors[7].partitionPolicy = FrameWorkloadPartitionPolicy::Single;
		descriptors[7].partitionWorkItemCount = 0U;
		descriptors[7].inlineWorkItemThreshold = 0U;
		ConfigureProtectedWorkload(
			descriptors[8],
			"FinalizePublishedAnimationAuthority",
			WorkerSystemPhaseKind::ProtectedCommit,
			{kResourceAnimationAuthorityFrame, kResourceAnimationFrameSidecars,
				"SceneGraph", "ObjectManager", kResourceLocomotionAnimatorBindings,
				kResourceRootMotionResults, kResourceAnimationRagdollMappings,
				kResourcePhysicsCharacterMotionLeases, "PhysicsWorld",
				"RagdollPhysicsSnapshots", "RagdollPhysicsResults"},
			{kResourceAnimationRuntimeState, kResourceAnimationCommandQueues, kResourceAnimationPhysicsHandoffQueue,
				kResourceAnimationManagedCallbackBatches, kResourceRootMotionIntervals,
				kResourceRootMotionResults, kResourcePhysicsCharacterMotionLeases,
				kResourceAnimationRagdollMappings, "PhysicsWorld", "RagdollPhysicsQueue",
				"SceneMutationQueue", "SceneGraph", "ObjectManager"},
			"Applies the already accepted immutable commit token and its sparse specialist batches.");

		descriptors[1].explicitDependencies = {descriptors[0].stableDescriptorId};
		descriptors[2].explicitDependencies = {descriptors[1].stableDescriptorId};
		descriptors[3].explicitDependencies = {descriptors[2].stableDescriptorId};
		descriptors[4].explicitDependencies = {descriptors[2].stableDescriptorId};
		descriptors[5].explicitDependencies = {
			descriptors[3].stableDescriptorId, descriptors[4].stableDescriptorId};
		descriptors[6].explicitDependencies = {descriptors[5].stableDescriptorId};
		descriptors[7].explicitDependencies = {descriptors[6].stableDescriptorId};
		descriptors[8].explicitDependencies = {descriptors[7].stableDescriptorId};
		for(FrameWorkloadDeclaration& descriptor : descriptors) {
			descriptor.active = animatorCount != 0U;
			descriptor.ready = true;
			descriptor.estimatedWorkItemCount = animatorCount;
		}
		descriptors[2].estimatedWorkItemCount = 1U;
		descriptors[5].estimatedWorkItemCount = 1U;
		descriptors[6].sameFrameVisibilityRequired = animatorCount != 0U;
		descriptors[7].sameFrameVisibilityRequired = animatorCount != 0U;
		descriptors[8].sameFrameVisibilityRequired = animatorCount != 0U;

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
			TERRAENGINE_PROFILE_SCOPE("Animation: Plan Evaluation Range",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			impl_->PlanRange(partition.itemBegin, partition.itemEnd, timeInput);
		};
		descriptors[2].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Finalize Work Plan",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			(void)impl_->EnsureAnimationWorkPlanFinalized();
		};
		descriptors[3].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch](const FrameWorkloadPartition& partition, JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Evaluate Complete Range",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			impl_->EvaluatePartition(partition.partitionIndex, partition.partitionCount);
		};
			descriptors[4].rangeWork = [this, runtimeSessionEpoch, frameDeclarationEpoch](const FrameWorkloadPartition& partition, JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Evaluate Compact GPU Visual Range",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
				impl_->EvaluateCompactGpuVisualPartition(
					partition.itemBegin, partition.itemEnd);
		};
			descriptors[5].work = [this, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Seal Authority Candidate",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch) return;
			// Sealing performs all allocations and deterministic merging on a worker.
			// The following protected workload can therefore exchange only one root.
			impl_->commandFrameFailurePhase = "SealAnimationAuthority";
			if(impl_->SealAuthorityFrameCandidate()) impl_->commandFrameFailurePhase = nullptr;
		};
		descriptors[6].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Publish Animation Authority",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			if(context.IsCancellationRequested()) {
				impl_->RequestFrameCancellation();
				return;
			}
			if(!impl_->runtimeActive.load(std::memory_order_acquire)
				|| impl_->runtimeSessionEpoch.load(std::memory_order_acquire) != runtimeSessionEpoch
				|| impl_->frameDeclarationEpoch.load(std::memory_order_acquire) != frameDeclarationEpoch
				|| impl_->lastPublishedFrameDeclarationEpoch == frameDeclarationEpoch
				|| (!impl_->sealedAuthorityCandidateReady
					&& !impl_->retainsPublishedAuthorityRoot)) return;
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			if(impl_->retainsPublishedAuthorityRoot) {
				// Cache-only and compatible hold-last frames have already proved that every
				// observable authority fact is present in the retained root. Mark this
				// scheduler epoch complete without committing state or swapping a root.
				impl_->lastPublishedFrameDeclarationEpoch = frameDeclarationEpoch;
				return;
			}
			const Impl::PublishedAuthorityCommitToken token = impl_->publishedAuthorityCommit;
			impl_->commandFrameFailurePhase = "PublishAnimationAuthorityGenerationGate";
			if(token.authorityGeneration == 0U || token.animatorCount != animatorCount
				|| token.frameEpoch != frameDeclarationEpoch
				|| token.sessionGeneration != runtimeSessionEpoch
				|| !token.expectedRuntimeRoot || !token.nextRuntimeRoot
				|| !token.expectedCompactRuntimeRoot || !token.nextCompactRuntimeRoot
				|| !token.specialistSidecars
				|| impl_->runtimeRecords.load(std::memory_order_acquire)
					!= token.expectedRuntimeRoot
				|| impl_->compactRuntimeStates.load(std::memory_order_acquire)
					!= token.expectedCompactRuntimeRoot
				|| token.sceneLifecycleGeneration
					!= impl_->frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene)) {
				return;
			}
			// Capture qualified individual commands against the controller generation
			// and sealed the binding-resource revision before those checks. A concurrent
			// resource publication changes this one scalar even if it races Capture;
			// the protected authority exchange remains constant-time for large crowds.
			if(impl_->qualifiedControllerResourceRevision != 0U
				&& ResourceManager::Instance().GetAnimationBindingResourceRevision()
					!= impl_->qualifiedControllerResourceRevision) {
				impl_->RejectDrainedFrameCommandsLocked();
				impl_->sealedAuthorityCandidate = {};
				impl_->sealedAuthorityCandidateReady = false;
				impl_->publishedAuthorityCommit = {};
				return;
			}
			const AnimationPublicationReceipt authorityReceipt = impl_->authorityPublisher.Publish(
				impl_->sealedAuthorityCandidate,
				runtimeSessionEpoch,
				impl_->frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene));
			if(!authorityReceipt.Published()) { impl_->commandFrameFailurePhase = "AuthorityPublisherRejected"; return; }
			// The expected-root identity check above proves no lifecycle or authored
			// mutation replaced evaluator state after worker sealing.  The store is the
			// complete runtime-state commit; it performs no record traversal or allocation.
			impl_->runtimeRecords.store(token.nextRuntimeRoot, std::memory_order_release);
			impl_->compactRuntimeStates.store(
				token.nextCompactRuntimeRoot, std::memory_order_release);
			// The publisher is deliberately the whole protected publication boundary:
			// it validates the sealed predecessor/session receipt and atomically swaps
			// one immutable root.  Crowd-sized internal state and specialist batches
			// are deferred to the dependent finalizer below.
			impl_->publishedAuthorityCommit.ready = true;
			impl_->commandFrameFailurePhase = nullptr;
			impl_->authorityPublicationGeneration.store(token.authorityGeneration, std::memory_order_release);
		};
		descriptors[7].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Commit Published Evaluation Cache",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			// Once authority is published, its internal scalar runtime commit is
			// mandatory. A late graph cancellation may suppress optional cache work on
			// the next frame, but must not leave evaluator progress behind authority.
			(void)context;
			const Impl::PublishedAuthorityCommitToken token =
				impl_->publishedAuthorityCommit;
			(void)impl_->CommitPublishedEvaluationCacheEntries(
				animatorCount,
				frameDeclarationEpoch,
				token.authorityGeneration,
				runtimeSessionEpoch);
		};
		descriptors[8].work = [this, animatorCount, runtimeSessionEpoch, frameDeclarationEpoch](JobGraphContext& context) {
			if(context.IsCancellationRequested()) {
				std::scoped_lock lock(impl_->runtimeOwnershipMutex);
				impl_->PublishCommittedOrdinaryCommandsLocked();
				impl_->SettleFailedFrameCommandsLocked(AnimationStatus::Cancelled, "FinalizeGraphCancelled"); return;
			}
			// Drain previous completed physical ticks before opening the next
			// authority publication. Physics never calls back under its mutex.
			AnimationRootMotionResultPublisher::Instance().ObserveRootMotionIntervalResults(
				Physics::PhysicsSystem::Instance());
			std::scoped_lock lock(impl_->runtimeOwnershipMutex);
			// Ordinary command state already belongs to the committed runtime root.
			// Its exact receipt must precede fallible specialist side effects; failed
			// settlement below only changes commands that remain genuinely uncommitted.
			impl_->PublishCommittedOrdinaryCommandsLocked();
			Impl::PublishedAuthorityCommitToken token = impl_->publishedAuthorityCommit;
			if(!token.ready || token.animatorCount != animatorCount || token.frameEpoch != frameDeclarationEpoch
				|| token.sessionGeneration != runtimeSessionEpoch
				|| token.sceneLifecycleGeneration
					!= impl_->frameLifecycleGenerations.Generation(AnimationLifecycleBoundary::Scene)
				|| !impl_->runtimeActive.load(std::memory_order_acquire)
				|| !token.specialistSidecars) {
				impl_->SettleFailedFrameCommandsLocked(AnimationStatus::Rejected, "FinalizeAuthorityGate"); return;
			}
			if(!impl_->ApplySealedSpecialistSidecars(token)) {
				impl_->SettleFailedFrameCommandsLocked(AnimationStatus::Rejected, "ApplySpecialistSidecars"); return;
			}
			// The accepted packet has now consumed every copied explicit root-motion
			// request. Clear the live capture queue only after that success point.
			impl_->pendingRootMotionApplications.clear();
			impl_->publishedAuthorityCommit = {};
			impl_->CaptureRollbackHistoryLocked(token.specialistSidecars->rollbackHistoryAnimators);
			impl_->PublishCommandCompletionsLocked(token.authorityGeneration);
			impl_->PublishProceduralCommandCompletionsLocked(token.authorityGeneration);
			impl_->PublishAdvancedCommandCompletionsLocked(token.authorityGeneration);
			impl_->publishedAnimatorCount.store(animatorCount, std::memory_order_release);
			// Worker sealing already copied diagnostics and lowered the GPU owner tables.
			// Protected work performs only bounded publication/submission operations.
			if(impl_->preparedDetailedDiagnostics) {
				(void)PublishAnimationDetailedDiagnosticsSnapshot(
					*impl_->preparedDetailedDiagnostics);
			}
			if(impl_->preparedDiagnostics
				&& PublishAnimationDiagnosticsSnapshot(*impl_->preparedDiagnostics)) {
				TerraEngine::Profiling::Profiler::Instance()
					.RecordAnimationDiagnostics(*impl_->preparedDiagnostics);
			}
			// Presentation submission is a separate protected consumer boundary. A
			// rejection (for example a newer frame or shutdown) never rolls back the
			// CPU-authoritative state committed immediately above.
			if(impl_->preparedPresentationFrame) {
				TERRAENGINE_PROFILE_SCOPE("Animation: Submit GPU Presentation Frame",
					TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
				(void)AnimationPresentationSystem::Instance()
					.SubmitGpuAnimationPresentationFrame(impl_->preparedPresentationFrame);
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
			const std::shared_ptr<Impl::RuntimeRecordRoot> currentRoot =
				impl_->runtimeRecords.load(std::memory_order_acquire);
			auto nextRoot = std::make_shared<Impl::RuntimeRecordRoot>(
				currentRoot ? *currentRoot : Impl::RuntimeRecordRoot{});
			if(nextRoot->size() <= handle.slotIndex) {
				nextRoot->resize(static_cast<std::size_t>(handle.slotIndex) + 1U);
			}
			auto record = std::make_shared<Impl::RuntimeRecord>();
			record->handle = handle;
			record->descriptor = descriptor;
			// The copied ragdoll identity is generation-safe and never aliases a
			// reused animator slot. Zero remains reserved as an invalid physics key.
			record->ragdollId = (static_cast<std::uint64_t>(handle.generation) << 32U)
				| (static_cast<std::uint64_t>(handle.slotIndex) + 1U);
			(void)record->playback.Configure({});
			nextRoot->Set(handle.slotIndex, std::move(record));
			impl_->runtimeRecords.store(std::move(nextRoot), std::memory_order_release);
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		} catch(const std::bad_alloc&) {
			(void)impl_->instances->Invalidate(handle);
			return {};
		}
		return handle;
	}

	bool AnimationSystem::InvalidateAnimatorInstance(AnimatorInstanceHandle handle)
	{
		const std::array<AnimatorInstanceHandle, 1U> handles{handle};
		return InvalidateAnimatorInstances(handles) == 1U;
	}

	std::size_t AnimationSystem::InvalidateAnimatorInstances(
		std::span<const AnimatorInstanceHandle> handles)
	{
		impl_->AssertCoordinatorThread();
		if(handles.empty()) return 0U;
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)) return 0U;

		// Stage every allocation before mutating the instance store. A failed reserve
		// or copy therefore leaves both handle liveness and the published root intact.
		std::vector<AnimatorInstanceHandle> retired;
		std::shared_ptr<Impl::RuntimeRecordRoot> nextRoot;
		try {
		retired.reserve(handles.size());
		for(const AnimatorInstanceHandle handle : handles) {
			if(handle.IsValid() && impl_->instances->IsAlive(handle)) {
				retired.push_back(handle);
			}
		}
		std::sort(retired.begin(), retired.end());
		retired.erase(std::unique(retired.begin(), retired.end()), retired.end());
		if(retired.empty()) return 0U;

		const std::shared_ptr<Impl::RuntimeRecordRoot> currentRoot =
			impl_->runtimeRecords.load(std::memory_order_acquire);
		nextRoot = currentRoot
			? std::make_shared<Impl::RuntimeRecordRoot>(*currentRoot)
			: std::shared_ptr<Impl::RuntimeRecordRoot>{};
		// Stage the retired root and every physical Teardown request before making
		// any handle stale. A full bridge queue or failed allocation leaves the old
		// runtime, stream ownership, and external character lease untouched so the
		// component coordinator can retry at the next protected boundary.
		std::vector<RagdollPhysicsRequest> externalTeardowns;
		externalTeardowns.reserve(retired.size());
		const auto bindings = impl_->rootControllerBindings.load(std::memory_order_acquire);
		for(const AnimatorInstanceHandle handle : retired) {
			if(nextRoot && handle.slotIndex < nextRoot->size()) {
				nextRoot->Set(handle.slotIndex, {});
			}
			const std::uint64_t externalOwnerId = RagdollExternalOwnerId(handle);
			const auto external = Physics::PhysicsSystem::Instance()
				.ReadExternalCharacterMotionOwner(externalOwnerId);
			if(!external) continue;
			const Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
			if(!record || record->ragdollId == 0U) return 0U;
			Physics::CharacterExternalRetirementIntent intent;
			intent.expectedExternalLease = *external;
			intent.externalOwnerId = externalOwnerId;
			// Locomotion binds the primary frame. A still-valid copied row can
			// supply its exact frame/generation; a removed AnimatorComponent may
			// already stale that guard, so physical owner identity is authoritative.
			intent.frame = {Physics::kPrimaryPhysicsFrameId,
				external->controller.world.worldGeneration};
			if(bindings) {
				const auto found = std::lower_bound(bindings->active.begin(), bindings->active.end(),
					handle, [](const AnimationRootControllerBinding& row,
						AnimatorInstanceHandle key) { return row.animator < key; });
				if(found != bindings->active.end() && found->animator == handle
					&& found->controller == external->controller
					&& found->bindingValidityGuard
					&& found->bindingValidityGuard->Validate()
						== Physics::CharacterMotionValidityReason::Valid) {
					intent.frame = found->frame;
					intent.bindingGeneration = found->bindingGeneration;
				}
			}
			RagdollPhysicsRequest teardown;
			teardown.kind = RagdollPhysicsRequestKind::Teardown;
			teardown.ragdollId = record->ragdollId;
			teardown.generation = record->ragdollGeneration;
			teardown.expectedCurrentGeneration = record->ragdollGeneration;
			teardown.correlationId = record->ragdollId ^ 0xA11AFFFF00000000ULL;
			if(teardown.correlationId == 0U) teardown.correlationId = 1U;
			teardown.producerOrder = std::numeric_limits<std::uint64_t>::max();
			teardown.retirementIntent =
				std::make_shared<const Physics::CharacterExternalRetirementIntent>(intent);
			externalTeardowns.push_back(std::move(teardown));
		}
		if(!externalTeardowns.empty()
			&& Physics::PhysicsSystem::Instance().SubmitRagdollPhysicsRequestBatch(
				externalTeardowns) != AnimationStatus::Queued) return 0U;
		} catch(const std::bad_alloc&) {
			return 0U;
		}
		for(const AnimatorInstanceHandle handle : retired) {
			// Runtime ownership is held for the complete batch, so a handle proven live
			// above cannot be replaced between validation and this generation advance.
			(void)impl_->instances->Invalidate(handle);
		}

		(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Component);
		(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(
			AnimationLifecycleBoundary::Component);
		(void)impl_->evaluationCache.InvalidateAnimators(retired);
		impl_->rollbackHistory.ClearMany(retired);
		if(impl_->proceduralCommands != nullptr) {
			for(const AnimatorInstanceHandle handle : retired) {
				impl_->proceduralCommands->RetireAnimator(
					{handle.slotIndex, handle.generation});
			}
		}
		if(impl_->advancedCommands != nullptr) {
			impl_->advancedCommands->RetireAnimators(retired);
		}
		if(nextRoot) {
			impl_->runtimeRecords.store(std::move(nextRoot), std::memory_order_release);
		}
		AnimationAttachmentSnapshotPublisher::Instance().RetireMany(retired);
		std::erase_if(impl_->pendingRootMotionApplications,
			[&retired](const AnimationRootMotionApplicationRequest& value) {
				return std::binary_search(retired.begin(), retired.end(), value.delta.animator);
			});
		AnimationRootMotionResultPublisher::Instance().RetireMany(retired);
		impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		return retired.size();
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
				// Unlike a recoverable evaluation failure, a lifecycle change retires the
				// whole authority envelope.  This makes an old scene, Play session, or
				// asset world unobservable to fresh scripting and rendering consumers.
				impl_->RetireLifecyclePublicationStateLocked();
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
			}
		}
		if(status == AnimationStatus::Ok || status == AnimationStatus::Cancelled) {
			// These sidecars borrow the authority session.  Clear them after releasing
			// runtime ownership to preserve their independent publisher lock ordering.
			AnimationAttachmentSnapshotPublisher::Instance().Clear();
			AnimationRootMotionResultPublisher::Instance().Clear();
			ClearPublishedAnimationDiagnostics();
			ClearPublishedAnimationDetailedDiagnostics();
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(boundary);
		}
		return status;
	}

	bool AnimationSystem::IsAnimatorInstanceAlive(AnimatorInstanceHandle handle) const
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		return impl_->instances->IsAlive(handle);
	}

	std::uint64_t AnimationSystem::AnimatorInstanceMembershipRevision() const noexcept
	{
		// Store lifetime is protected by AnimationSystem and survives ordinary play
		// sessions, so reading its monotonic atomic revision needs no ownership lock.
		return impl_->instances ? impl_->instances->MembershipRevision() : 0U;
	}

	AnimationStatus AnimationSystem::BindAnimatorPlayback(
		AnimatorInstanceHandle handle,
		AnimatorPlaybackBinding binding)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool mutated = false;
		return BindAnimatorPlaybackLocked(
			handle, std::move(binding), nullptr, nullptr, false, false, true, mutated);
	}

	AnimationStatus AnimationSystem::BindAnimatorPlaybackLocked(
		AnimatorInstanceHandle handle,
		AnimatorPlaybackBinding binding,
		const AnimationClipView* sharedValidatedClipView,
		ImmutableBindPreparation* sharedPreparation,
		bool immutableBindingValidated,
		bool deferEvaluationCacheInvalidation,
		bool publishGlobalInvalidation,
		bool& outMutated)
	{
		outMutated = false;
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| record == nullptr || !impl_->instances->IsAlive(handle)) {
			return AnimationStatus::StaleHandle;
		}
		// Rebinding would retire the old ragdoll proof, recovery pose, and profile.
		// An external character owner may survive a controller-target replacement,
		// so this low-frequency protected check scans Physics by owner identity rather
		// than trusting the current animator-to-controller binding table alone.
		if(record->bindingInitialized
			&& (record->pendingExternalRecoveryToken.has_value()
				|| Physics::PhysicsSystem::Instance().HasExternalCharacterMotionOwner(
					RagdollExternalOwnerId(handle)))) {
			return AnimationStatus::NotReady;
		}
		if(binding.clipGeneration == 0U || binding.skeletonGeneration == 0U
			|| binding.programGeneration == 0U || binding.stateId == 0U) {
			return AnimationStatus::InvalidArgument;
		}
		if(!immutableBindingValidated
			&& binding.controllerProgramRequired && binding.evaluationProgram) {
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
		if(!immutableBindingValidated && ((binding.retargetProfile
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
						!= binding.ragdollProfileGeneration)))) {
			return AnimationStatus::InvalidArgument;
		}
		if(record->contentGeneration == std::numeric_limits<AnimationGeneration>::max()
			|| record->historyGeneration == std::numeric_limits<AnimationGeneration>::max()
			|| record->stateGeneration == std::numeric_limits<std::uint64_t>::max()
			|| record->parameterGeneration == std::numeric_limits<std::uint64_t>::max()
			|| record->executionModeGeneration == std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}
		std::vector<AnimationParameterSnapshot> localInitialParameters;
		std::vector<float> localInitialLayerWeights;
		std::vector<AnimationParameterSnapshot>& initialParameters = sharedPreparation
			? sharedPreparation->initialParameters : localInitialParameters;
		std::vector<float>& initialLayerWeights = sharedPreparation
			? sharedPreparation->initialLayerWeights : localInitialLayerWeights;
		if(!immutableBindingValidated) {
			initialParameters.clear();
			if(const AnimationStatus status = ParseInitialParameters(
				binding.initialParametersJson, initialParameters); status != AnimationStatus::Ok) {
				return status;
			}
			initialLayerWeights.clear();
		}
		try {
			if(!immutableBindingValidated && binding.evaluationProgram) {
				initialLayerWeights.reserve(binding.evaluationProgram->layers.size());
				for(const EvaluationLayer& layer : binding.evaluationProgram->layers) {
					if(!std::isfinite(layer.defaultWeight)
						|| layer.defaultWeight < 0.0F || layer.defaultWeight > 1.0F) {
						return AnimationStatus::InvalidArgument;
					}
					initialLayerWeights.push_back(layer.defaultWeight);
				}
			}
			if(sharedPreparation != nullptr) {
				// The shared template is copied into each actor only after the
				// controller generation changes. Reserve both destinations now so
				// that late allocation cannot fail after the instance descriptor
				// has advanced to the new controller publication.
				record->parameters.reserve(initialParameters.size());
				record->layerWeights.reserve(initialLayerWeights.size());
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
			// A component crowd shares the exact prepared binding pointer. Once one
			// live record has validated these immutable arrays, repeating four copied
			// sorts for every identical prefab instance only adds protected-frame work.
			if(!immutableBindingValidated
				&& (!validStableIds(binding.jointStableIds)
				|| !validStableIds(binding.retargetSourceJointStableIds)
				|| !validStableIds(binding.morphStableIds)
				|| !validStableIds(binding.curveStableIds))) {
				return AnimationStatus::InvalidArgument;
			}
		} catch(const std::bad_alloc&) {
			return AnimationStatus::InternalError;
		}
		const auto resolveClipFingerprint = [this](
			const AnimationClipAsset& clip,
			AnimationGeneration generation,
			std::uint64_t& output) {
			const AnimationAssetHandle key{clip.assetId, generation};
			if(const auto found = impl_->clipContentFingerprints.find(key);
				found != impl_->clipContentFingerprints.end()) {
				output = found->second;
				return AnimationStatus::Ok;
			}
			std::uint64_t fingerprint = 0U;
			const AnimationStatus status =
				ComputeClipDeterministicContentFingerprint(clip, fingerprint);
			if(status != AnimationStatus::Ok) return status;
			try {
				impl_->clipContentFingerprints.emplace(key, fingerprint);
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
			output = fingerprint;
			return AnimationStatus::Ok;
		};
		if(binding.clip) {
			const AnimationStatus fingerprintStatus = resolveClipFingerprint(
				*binding.clip,
				binding.clipGeneration,
				binding.clipDeterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok) return fingerprintStatus;
		}
		for(AnimatorControllerClipBinding& controllerClip : binding.controllerClips) {
			if(!controllerClip.clip) continue;
			const AnimationStatus fingerprintStatus = resolveClipFingerprint(
				*controllerClip.clip,
				controllerClip.clipGeneration,
				controllerClip.deterministicContentFingerprint);
			if(fingerprintStatus != AnimationStatus::Ok) return fingerprintStatus;
		}

		const bool replacingExistingBinding = record->bindingInitialized;
		AnimationClipView clipView;
		AnimationEventStream eventStream;
		if(dependencyStatus == AnimationStatus::Ok) {
			const SkeletonAsset* samplingSkeleton = clipTargetsRetargetSource
				? binding.retargetSourceSkeleton.get() : binding.skeleton.get();
			const bool sharedViewMatches = sharedValidatedClipView != nullptr
				&& sharedValidatedClipView->Matches(
					*binding.clip,
					samplingSkeleton,
					std::span<const std::uint8_t>(binding.residentSegments),
					binding.clipGeneration,
					binding.streamedSegmentPayloads);
			dependencyStatus = sharedViewMatches
				? clipView.AdoptValidatedPublicationFrom(*sharedValidatedClipView)
				: clipView.Open(
					*binding.clip,
					samplingSkeleton,
					std::span<const std::uint8_t>(binding.residentSegments),
					binding.clipGeneration,
					binding.streamedSegmentPayloads);
			if(dependencyStatus == AnimationStatus::Ok
				&& binding.eventPolicy != AnimatorEventPolicy::Suppress) {
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
		AnimationAssetHandle reboundController{};
		if(record->descriptor.controller.IsValid() && binding.evaluationProgram) {
			reboundController = record->descriptor.controller;
			reboundController.generation = binding.programGeneration;
			// This is the last generation check before the successful bind commits.
			// CaptureFrameInputs copies this store descriptor, not the runtime record.
			if(!impl_->instances->UpdateControllerGeneration(handle, reboundController)) {
				return AnimationStatus::StaleHandle;
			}
		}
		if(publishGlobalInvalidation) {
			// A direct-clip crowd batch reaches this point under one protected lock.
			// The first record invalidates shared authority/presentation ownership;
			// later records still retire their individual caches below.
			(void)impl_->InvalidateLifecycleLocked(AnimationLifecycleBoundary::Asset);
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(
				AnimationLifecycleBoundary::Asset);
		}
		if(replacingExistingBinding && !deferEvaluationCacheInvalidation) {
			(void)impl_->evaluationCache.InvalidateAnimator(handle);
		}
		record->binding = std::move(binding);
		record->bindingInitialized = true;
		if(reboundController.IsValid()) {
			record->descriptor.controller = reboundController;
		}
		record->clipView = std::move(clipView);
		record->eventStream = std::move(eventStream);
		record->playback = playback;
		if(sharedPreparation != nullptr) {
			// Retain each record's capacity across rebinds while reusing the one parsed
			// cohort template. This removes per-instance JSON parsing and layer walks.
			record->parameters.assign(initialParameters.begin(), initialParameters.end());
			record->layerWeights.assign(initialLayerWeights.begin(), initialLayerWeights.end());
		} else {
			record->parameters = std::move(localInitialParameters);
			record->layerWeights = std::move(localInitialLayerWeights);
		}
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
		// A hot-rebind invalidates cached evaluator state.  Consumers retain only a
		// previously acquired authority root until the next sealed root is published.
		if(replacingExistingBinding) {
			// Socket/bone attachment copies belong to the retired pose generation too.
			// Remove them in the same protected rebind operation so a fresh ABI read can
			// never pair an old transform set with the new resource identity.
			if(!deferEvaluationCacheInvalidation) {
				AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
				if(impl_->advancedCommands != nullptr) {
					impl_->advancedCommands->RetireAnimator(
						{handle.slotIndex, handle.generation});
				}
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
		if(replacingExistingBinding && !deferEvaluationCacheInvalidation) {
			std::erase_if(impl_->pendingRootMotionApplications,
				[handle](const AnimationRootMotionApplicationRequest& value) {
					return value.delta.animator == handle;
				});
			AnimationRootMotionResultPublisher::Instance().Retire(handle);
			impl_->rollbackHistory.Clear(handle);
		}
		if(publishGlobalInvalidation) {
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		outMutated = true;
		return dependencyStatus;
	}

	void AnimationSystem::BindAnimatorPlaybackBatch(
		std::span<const AnimatorPlaybackBindRequest> requests,
		std::span<AnimationStatus> statuses)
	{
		impl_->AssertCoordinatorThread();
		if(statuses.size() != requests.size()) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InvalidArgument);
			return;
		}
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool globalInvalidationPublished = false;
		struct PreparedBinding {
			const AnimatorPlaybackBinding* source = nullptr;
			ImmutableBindPreparation immutable;
		};
		std::vector<PreparedBinding> preparedBindings;
		std::vector<AnimatorInstanceHandle> mutatedHandles;
		std::vector<AnimatorInstanceHandle> retiredHandles;
		try {
			preparedBindings.reserve(requests.size());
			mutatedHandles.reserve(requests.size());
			retiredHandles.reserve(requests.size());
		} catch(const std::bad_alloc&) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InternalError);
			return;
		}
		for(std::size_t index = 0U; index < requests.size(); ++index) {
			const AnimatorPlaybackBindRequest& request = requests[index];
			if(request.binding == nullptr) {
				statuses[index] = AnimationStatus::InvalidArgument;
				continue;
			}
			AnimatorPlaybackBinding binding = *request.binding;
			binding.playOnBind = request.playOnBind;
			const Impl::RuntimeRecord* priorRecord =
				impl_->FindRuntimeRecordLocked(request.handle);
			const bool replacingExistingBinding = priorRecord != nullptr
				&& priorRecord->bindingInitialized;
			auto prepared = std::find_if(
				preparedBindings.begin(), preparedBindings.end(),
				[request](const PreparedBinding& value) {
					return value.source == request.binding;
				});
			const bool immutableBindingValidated = prepared != preparedBindings.end();
			if(!immutableBindingValidated) {
				preparedBindings.push_back({request.binding, {}});
				prepared = std::prev(preparedBindings.end());
			}
			bool mutated = false;
			statuses[index] = BindAnimatorPlaybackLocked(
				request.handle,
				std::move(binding),
				request.sharedValidatedClipView,
				&prepared->immutable,
				immutableBindingValidated,
				true,
				!globalInvalidationPublished,
				mutated);
			globalInvalidationPublished = globalInvalidationPublished || mutated;
			if(mutated) {
				mutatedHandles.push_back(request.handle);
				if(replacingExistingBinding) {
					retiredHandles.push_back(request.handle);
				}
			} else if(!immutableBindingValidated) {
				// Failed first-use preparation must not make the next request skip
				// validation or consume an incomplete authored-default template.
				preparedBindings.erase(prepared);
			}
		}
		if(!retiredHandles.empty()) {
			std::sort(retiredHandles.begin(), retiredHandles.end());
			retiredHandles.erase(
				std::unique(retiredHandles.begin(), retiredHandles.end()),
				retiredHandles.end());
			// Every per-record replacement above already cleared its local runtime state.
			// Retire cross-subsystem publications once for the sorted crowd so attachment
			// and root-motion vectors are compacted linearly rather than once per owner.
			AnimationAttachmentSnapshotPublisher::Instance().RetireMany(retiredHandles);
			if(impl_->advancedCommands != nullptr) {
				impl_->advancedCommands->RetireAnimators(retiredHandles);
			}
			std::erase_if(impl_->pendingRootMotionApplications,
				[&retiredHandles](const AnimationRootMotionApplicationRequest& value) {
					return std::binary_search(retiredHandles.begin(), retiredHandles.end(),
						value.delta.animator);
				});
			AnimationRootMotionResultPublisher::Instance().RetireMany(retiredHandles);
			impl_->rollbackHistory.ClearMany(retiredHandles);
		}
		if(!mutatedHandles.empty()) {
			// Evaluation cache keys are animator-contiguous. One bulk operation keeps
			// its mutex for the complete crowd instead of reacquiring it thousands of
			// times during one protected resource publication.
			(void)impl_->evaluationCache.InvalidateAnimators(mutatedHandles);
		}
	}

	AnimationStatus AnimationSystem::RefreshAnimatorDirectClipStreaming(
		AnimatorInstanceHandle handle,
		const AnimatorPlaybackBinding& streamedBinding,
		AnimationClipView* sharedValidatedClipView)
	{
		impl_->AssertCoordinatorThread();
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		bool changed = false;
		bool replacedClipPublication = false;
		const AnimationStatus status = RefreshAnimatorDirectClipStreamingLocked(
			handle, streamedBinding, sharedValidatedClipView,
			EMPTY_UUID, false, false, 0U, nullptr, nullptr, nullptr,
			changed, replacedClipPublication);
		if(changed) {
			(void)impl_->evaluationCache.InvalidateAnimator(handle);
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		return status;
	}

	AnimationStatus AnimationSystem::RefreshAnimatorDirectClipStreamingLocked(
		AnimatorInstanceHandle handle,
		const AnimatorPlaybackBinding& streamedBinding,
		AnimationClipView* sharedValidatedClipView,
		UUID expectedClipAssetId,
		bool allowClipPublicationReplacement,
		bool playOnBind,
		std::uint64_t replacementClipFingerprint,
		const AnimationEventStream* preparedReplacementEventStream,
		const AnimatorPlayback* preparedReplacementPlayback,
		const AnimatorPlaybackConfiguration* preparedReplacementConfiguration,
		bool& outChanged,
		bool& outReplacedClipPublication)
	{
		outChanged = false;
		outReplacedClipPublication = false;
		Impl::RuntimeRecord* record = impl_->FindRuntimeRecordLocked(handle);
		if(!impl_->runtimeActive.load(std::memory_order_acquire)
			|| record == nullptr) {
			return AnimationStatus::StaleHandle;
		}
		// The caller holds runtimeOwnershipMutex. Invalidation clears this exact
		// generation's runtime-record slot while holding the same mutex, so the
		// successful lookup already proves liveness. Rechecking the private store
		// mutex here serialized every member of a crowd-wide streaming refresh.
		const AnimatorPlaybackBinding& current = record->binding;
		const bool replacingPublishedClip = allowClipPublicationReplacement
			&& current.clip && streamedBinding.clip
			&& expectedClipAssetId != EMPTY_UUID
			&& streamedBinding.clip->assetId.value == expectedClipAssetId
			&& current.clip->assetId == streamedBinding.clip->assetId
			&& current.clipGeneration != streamedBinding.clipGeneration;
		const bool publishingPreviouslyUnavailableClip =
			allowClipPublicationReplacement
			&& !current.clip && streamedBinding.clip
			&& expectedClipAssetId != EMPTY_UUID
			&& streamedBinding.clip->assetId.value == expectedClipAssetId;
		const bool publishingClip = replacingPublishedClip
			|| publishingPreviouslyUnavailableClip;
		// The narrow refresh path is valid only when every authored/resource identity
		// is unchanged. Controllers and retargeting may change several active clips or
		// sampling skeletons, so they continue through the full atomic bind path.
		const char* rejectionReason = nullptr;
		if(current.controllerProgramRequired || streamedBinding.controllerProgramRequired) {
			rejectionReason = "ControllerProgram";
		} else if(current.retargetProfile || streamedBinding.retargetProfile) {
			rejectionReason = "RetargetProfile";
		} else if(!streamedBinding.clip
			|| (!current.clip && !publishingPreviouslyUnavailableClip)) {
			rejectionReason = "MissingClip";
		} else if(!current.skeleton || !streamedBinding.skeleton) {
			rejectionReason = "MissingSkeleton";
		} else if(current.clip
			&& current.clip->assetId != streamedBinding.clip->assetId) {
			rejectionReason = "ClipAsset";
		} else if(current.clip && !publishingClip
			&& current.clipGeneration != streamedBinding.clipGeneration) {
			rejectionReason = "ClipGeneration";
		} else if(publishingClip && replacementClipFingerprint == 0U) {
			rejectionReason = "ClipFingerprint";
		} else if(current.skeleton->assetId != streamedBinding.skeleton->assetId) {
			rejectionReason = "SkeletonAsset";
		} else if(current.skeletonGeneration != streamedBinding.skeletonGeneration) {
			rejectionReason = "SkeletonGeneration";
		} else if(!publishingClip
			&& current.programGeneration != streamedBinding.programGeneration) {
			rejectionReason = "ProgramGeneration";
		} else if(!publishingClip && current.stateId != streamedBinding.stateId) {
			rejectionReason = "State";
		} else if(current.requestedExecutionPolicy
			!= streamedBinding.requestedExecutionPolicy) {
			rejectionReason = "ExecutionPolicy";
		} else if(current.rootMotionPolicy != streamedBinding.rootMotionPolicy) {
			rejectionReason = "RootMotionPolicy";
		} else if(current.eventPolicy != streamedBinding.eventPolicy) {
			rejectionReason = "EventPolicy";
		}
		if(rejectionReason != nullptr) {
			return AnimationStatus::InvalidArgument;
		}
		const bool streamingInputsUnchanged = !publishingClip
			&& current.residentSegments == streamedBinding.residentSegments
			&& current.streamedSegmentPayloads
				== streamedBinding.streamedSegmentPayloads
			&& current.gpuResidentPayloads == streamedBinding.gpuResidentPayloads
			&& current.gpuFastBinding == streamedBinding.gpuFastBinding
			&& current.gpuProgramResourceIndex
				== streamedBinding.gpuProgramResourceIndex
			&& current.evaluationGenerations.residencyGeneration
				== streamedBinding.evaluationGenerations.residencyGeneration
			&& current.gpuExecution.rendererAvailable
				== streamedBinding.gpuExecution.rendererAvailable
			&& current.gpuExecution.animationCapabilityAvailable
				== streamedBinding.gpuExecution.animationCapabilityAvailable
			&& current.gpuExecution.resourcesResident
				== streamedBinding.gpuExecution.resourcesResident
			&& current.gpuExecution.evaluationHealthy
				== streamedBinding.gpuExecution.evaluationHealthy
			&& current.gpuExecution.requiredBudget
				== streamedBinding.gpuExecution.requiredBudget
			&& current.gpuExecution.availableBudget
				== streamedBinding.gpuExecution.availableBudget;
		if(streamingInputsUnchanged) {
			// A shared clip publication can wake several retained requests at once.
			// Some components may already expose the final immutable state; do not
			// invalidate their clip views, history, or evaluation cache a second time.
			return AnimationStatus::Ok;
		}
		if(record->contentGeneration
				== std::numeric_limits<AnimationGeneration>::max()
			|| record->historyGeneration
				== std::numeric_limits<AnimationGeneration>::max()
			|| (publishingClip
				&& record->stateGeneration
					== std::numeric_limits<std::uint64_t>::max())
			|| record->executionModeGeneration
				== std::numeric_limits<std::uint64_t>::max()) {
			return AnimationStatus::RangeOverflow;
		}

		AnimationClipView localValidatedView;
		AnimationClipView* validatedView = sharedValidatedClipView;
		AnimationStatus viewStatus = AnimationStatus::NotReady;
		const AnimationClipAsset& validatedClip = publishingClip
			? *streamedBinding.clip : *current.clip;
		const SkeletonAsset* validatedSkeleton = publishingClip
			? streamedBinding.skeleton.get() : current.skeleton.get();
		const AnimationGeneration validatedClipGeneration = publishingClip
			? streamedBinding.clipGeneration : current.clipGeneration;
		if(sharedValidatedClipView != nullptr) {
			if(!sharedValidatedClipView->Matches(
				validatedClip,
				validatedSkeleton,
				std::span<const std::uint8_t>(streamedBinding.residentSegments),
				validatedClipGeneration,
				streamedBinding.streamedSegmentPayloads)) {
				// Preparation can publish a fresh shared_ptr wrapper for unchanged asset
				// bytes. Bind the scratch view once to the first live runtime pointer pair;
				// subsequent identical prefab records then share that completed validation.
				viewStatus = sharedValidatedClipView->Open(
					validatedClip,
					validatedSkeleton,
					std::span<const std::uint8_t>(streamedBinding.residentSegments),
					validatedClipGeneration,
					streamedBinding.streamedSegmentPayloads);
			} else {
				viewStatus = AnimationStatus::Ok;
			}
		} else {
			viewStatus = localValidatedView.Open(
				validatedClip,
				validatedSkeleton,
				std::span<const std::uint8_t>(streamedBinding.residentSegments),
				validatedClipGeneration,
				streamedBinding.streamedSegmentPayloads);
			validatedView = &localValidatedView;
		}
		if(viewStatus != AnimationStatus::Ok) {
			return viewStatus;
		}

		AnimationEventStream replacementEventStream;
		AnimatorPlayback replacementPlayback;
		std::vector<AnimatorControllerClipBinding> replacementControllerClips;
		std::vector<std::uint64_t> replacementMorphStableIds;
		std::vector<std::uint64_t> replacementCurveStableIds;
		AnimatorPlaybackConfiguration replacementPlaybackConfiguration =
			preparedReplacementConfiguration != nullptr
				? *preparedReplacementConfiguration
				: streamedBinding.playback;
		if(publishingClip) {
			try {
				// The direct program and named-output directories are derived from the
				// immutable clip publication. Stage their copies before mutating the live
				// record so allocation failure cannot expose a half-promoted binding.
				replacementControllerClips = streamedBinding.controllerClips;
				replacementMorphStableIds = streamedBinding.morphStableIds;
				replacementCurveStableIds = streamedBinding.curveStableIds;
			} catch(const std::bad_alloc&) {
				return AnimationStatus::InternalError;
			}
			if(preparedReplacementEventStream != nullptr) {
				// Batch preparation performs invariant clip-event sorting and playback
				// setup once per direct-clip cohort before runtime ownership is locked.
				// Copies retain independent cursors while sharing immutable event arrays.
				replacementEventStream = *preparedReplacementEventStream;
			}
			else if(streamedBinding.eventPolicy != AnimatorEventPolicy::Suppress) {
				const AnimationAssetHandle clipHandle{
					streamedBinding.clip->assetId,
					streamedBinding.clipGeneration};
				if(const AnimationStatus eventStatus = replacementEventStream.Configure(
					clipHandle,
					streamedBinding.clip->durationTicks,
					streamedBinding.clip->events);
					eventStatus != AnimationStatus::Ok) {
					return eventStatus;
				}
			}
			if(preparedReplacementPlayback != nullptr) {
				replacementPlayback = *preparedReplacementPlayback;
			}
			else {
				replacementPlaybackConfiguration.durationTicks =
					streamedBinding.clip->durationTicks;
				if(streamedBinding.updatePolicy == AnimatorUpdatePolicy::FixedStep) {
					replacementPlaybackConfiguration.timePolicy = AnimatorTimePolicy::Fixed;
				}
				if(const AnimationStatus playbackStatus = replacementPlayback.Configure(
					replacementPlaybackConfiguration);
					playbackStatus != AnimationStatus::Ok) {
					return playbackStatus;
				}
				if(const AnimationStatus seekStatus = replacementPlayback.Seek(
					streamedBinding.initialPlaybackTick);
					seekStatus != AnimationStatus::Ok) {
					return seekStatus;
				}
				if(playOnBind) {
					if(const AnimationStatus playStatus = replacementPlayback.Play();
						playStatus != AnimationStatus::Ok) {
						return playStatus;
					}
				}
			}
		}

		viewStatus = publishingClip
			? record->clipView.AdoptValidatedPublicationFrom(*validatedView)
			: record->clipView.CopyStreamingStateFrom(*validatedView);
		if(viewStatus != AnimationStatus::Ok) {
			return viewStatus;
		}
		if(publishingClip) {
			// Reuse skeleton/mesh policy storage, but atomically publish every value
			// derived from the direct clip. This also promotes records created while the
			// shared clip was unavailable, without 1,000 independent full binds.
			record->binding.clip = streamedBinding.clip;
			record->binding.skeleton = streamedBinding.skeleton;
			record->binding.clipGeneration = streamedBinding.clipGeneration;
			record->binding.clipDeterministicContentFingerprint =
				replacementClipFingerprint;
			record->binding.evaluationProgram = streamedBinding.evaluationProgram;
			record->binding.controllerClips = std::move(replacementControllerClips);
			record->binding.programGeneration = streamedBinding.programGeneration;
			record->binding.stateId = streamedBinding.stateId;
			record->binding.morphStableIds = std::move(replacementMorphStableIds);
			record->binding.curveStableIds = std::move(replacementCurveStableIds);
			record->binding.playback = replacementPlaybackConfiguration;
			record->binding.initialPlaybackTick = streamedBinding.initialPlaybackTick;
			record->eventStream = std::move(replacementEventStream);
			record->eventStateCaptured = false;
			record->playback = replacementPlayback;
			record->graphState = {};
			record->transition = {};
			++record->stateGeneration;
			record->executionModeState = {};
			AnimationAttachmentSnapshotPublisher::Instance().Retire(handle);
			impl_->rollbackHistory.Clear(handle);
		}
		// The cohort-prepared binding owns an immutable copy-on-write bitmap. Share it
		// across runtime records rather than detaching and copying it once per member.
		record->binding.residentSegments = streamedBinding.residentSegments;
		record->binding.streamedSegmentPayloads =
			streamedBinding.streamedSegmentPayloads;
		record->binding.gpuExecution = streamedBinding.gpuExecution;
		record->binding.gpuProgramResourceIndex =
			streamedBinding.gpuProgramResourceIndex;
		record->binding.gpuVisual = streamedBinding.gpuVisual;
		record->binding.gpuResidentPayloads =
			streamedBinding.gpuResidentPayloads;
		record->binding.gpuFastBinding = streamedBinding.gpuFastBinding;
		if(publishingClip) {
			record->binding.evaluationGenerations =
				streamedBinding.evaluationGenerations;
		} else {
			record->binding.evaluationGenerations.residencyGeneration =
				streamedBinding.evaluationGenerations.residencyGeneration;
		}
		record->dependencyStatus = AnimationStatus::Ok;
		++record->contentGeneration;
		++record->historyGeneration;
		++record->executionModeGeneration;
		outChanged = true;
		outReplacedClipPublication = publishingClip;
		return AnimationStatus::Ok;
	}

	void AnimationSystem::RefreshAnimatorDirectClipStreamingBatch(
		std::span<const AnimatorDirectClipStreamingRefresh> refreshes,
		std::span<AnimationStatus> statuses)
	{
		impl_->AssertCoordinatorThread();
		if(statuses.size() != refreshes.size()) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InvalidArgument);
			return;
		}
		struct PreparedDirectClipReplacement
		{
			const AnimatorPlaybackBinding* binding = nullptr;
			UUID expectedClipAssetId = EMPTY_UUID;
			bool playOnBind = false;
			bool hasEventStream = false;
			AnimationEventStream eventStream;
			AnimatorPlayback playback;
			AnimatorPlaybackConfiguration playbackConfiguration{};
			AnimationStatus status = AnimationStatus::NotReady;
		};
		std::vector<PreparedDirectClipReplacement> preparedReplacements;
		std::vector<std::uint32_t> preparedReplacementIndices;
		std::vector<AnimatorInstanceHandle> changedAnimators;
		try {
			// Reserve and prepare every cohort-invariant replacement before publishing
			// record changes. Allocation or validation failure then leaves the complete
			// protected batch untouched and reports a bounded error per item.
			preparedReplacements.reserve(refreshes.size());
			preparedReplacementIndices.assign(refreshes.size(),
				std::numeric_limits<std::uint32_t>::max());
			changedAnimators.reserve(refreshes.size());
			for(std::size_t index = 0U; index < refreshes.size(); ++index) {
				const AnimatorDirectClipStreamingRefresh& refresh = refreshes[index];
				if(!refresh.allowClipPublicationReplacement
					|| refresh.streamedBinding == nullptr) {
					continue;
				}
				if(!preparedReplacements.empty()) {
					const PreparedDirectClipReplacement& prior = preparedReplacements.back();
					if(prior.binding == refresh.streamedBinding
						&& prior.expectedClipAssetId == refresh.expectedClipAssetId
						&& prior.playOnBind == refresh.playOnBind) {
						preparedReplacementIndices[index] = static_cast<std::uint32_t>(
							preparedReplacements.size() - 1U);
						continue;
					}
				}
				PreparedDirectClipReplacement prepared{};
				prepared.binding = refresh.streamedBinding;
				prepared.expectedClipAssetId = refresh.expectedClipAssetId;
				prepared.playOnBind = refresh.playOnBind;
				if(!refresh.streamedBinding->clip) {
					prepared.status = AnimationStatus::MissingAsset;
				}
				else if(refresh.expectedClipAssetId == EMPTY_UUID
					|| refresh.streamedBinding->clip->assetId.value
						!= refresh.expectedClipAssetId) {
					prepared.status = AnimationStatus::InvalidArgument;
				}
				else {
					prepared.playbackConfiguration = refresh.streamedBinding->playback;
					prepared.playbackConfiguration.durationTicks =
						refresh.streamedBinding->clip->durationTicks;
					if(refresh.streamedBinding->updatePolicy
						== AnimatorUpdatePolicy::FixedStep) {
						prepared.playbackConfiguration.timePolicy = AnimatorTimePolicy::Fixed;
					}
					prepared.status = prepared.playback.Configure(
						prepared.playbackConfiguration);
					if(prepared.status == AnimationStatus::Ok) {
						prepared.status = prepared.playback.Seek(
							refresh.streamedBinding->initialPlaybackTick);
					}
					if(prepared.status == AnimationStatus::Ok && refresh.playOnBind) {
						prepared.status = prepared.playback.Play();
					}
					if(prepared.status == AnimationStatus::Ok
						&& refresh.streamedBinding->eventPolicy
							!= AnimatorEventPolicy::Suppress) {
						const AnimationAssetHandle clipHandle{
							refresh.streamedBinding->clip->assetId,
							refresh.streamedBinding->clipGeneration};
						prepared.status = prepared.eventStream.Configure(
							clipHandle,
							refresh.streamedBinding->clip->durationTicks,
							refresh.streamedBinding->clip->events);
						prepared.hasEventStream = prepared.status == AnimationStatus::Ok;
					}
				}
				preparedReplacements.push_back(std::move(prepared));
				preparedReplacementIndices[index] = static_cast<std::uint32_t>(
					preparedReplacements.size() - 1U);
			}
		} catch(const std::bad_alloc&) {
			std::fill(statuses.begin(), statuses.end(), AnimationStatus::InternalError);
			return;
		}

		std::unique_lock lock(impl_->runtimeOwnershipMutex, std::defer_lock);
		{
			// Measure only contention for runtime ownership. The surrounding batch
			// scope also includes record refresh and presentation invalidation, which
			// otherwise makes an occasional cross-frame lock wait look like copy work.
			TERRAENGINE_PROFILE_SCOPE("Animation: Direct Clip Batch Runtime Lock Wait",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			lock.lock();
		}
		bool replacedClipPublication = false;
		const AnimatorPlaybackBinding* priorReplacementBinding = nullptr;
		UUID priorExpectedClipAssetId = EMPTY_UUID;
		AnimationStatus priorReplacementStatus = AnimationStatus::NotReady;
		std::uint64_t priorReplacementClipFingerprint = 0U;
		{
			TERRAENGINE_PROFILE_SCOPE("Animation: Refresh Direct Clip Runtime Records",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			for(std::size_t index = 0U; index < refreshes.size(); ++index) {
				const AnimatorDirectClipStreamingRefresh& refresh = refreshes[index];
				if(refresh.streamedBinding == nullptr) {
					statuses[index] = AnimationStatus::InvalidArgument;
					continue;
				}
				const std::uint32_t preparedIndex = preparedReplacementIndices[index];
				const PreparedDirectClipReplacement* prepared = preparedIndex
					< preparedReplacements.size()
						? &preparedReplacements[preparedIndex]
						: nullptr;
				if(prepared != nullptr && prepared->status != AnimationStatus::Ok) {
					statuses[index] = prepared->status;
					continue;
				}
				std::uint64_t replacementClipFingerprint = 0U;
				if(refresh.allowClipPublicationReplacement) {
					if(priorReplacementBinding == refresh.streamedBinding
						&& priorExpectedClipAssetId == refresh.expectedClipAssetId) {
						// The coordinator groups prefab instances by one immutable prepared
						// binding. Reuse its completed identity/fingerprint result instead of
						// repeating an ordered-map lookup for every member of a large crowd.
						if(priorReplacementStatus != AnimationStatus::Ok) {
							statuses[index] = priorReplacementStatus;
							continue;
						}
						replacementClipFingerprint =
							priorReplacementClipFingerprint;
					} else {
						priorReplacementBinding = refresh.streamedBinding;
						priorExpectedClipAssetId = refresh.expectedClipAssetId;
						priorReplacementStatus = AnimationStatus::Ok;
						priorReplacementClipFingerprint = 0U;
						if(!refresh.streamedBinding->clip) {
							priorReplacementStatus = AnimationStatus::MissingAsset;
						} else if(refresh.expectedClipAssetId == EMPTY_UUID
							|| refresh.streamedBinding->clip->assetId.value
								!= refresh.expectedClipAssetId) {
							priorReplacementStatus = AnimationStatus::InvalidArgument;
						} else {
							const AnimationAssetHandle clipKey{
								refresh.streamedBinding->clip->assetId,
								refresh.streamedBinding->clipGeneration};
							const auto cachedFingerprint =
								impl_->clipContentFingerprints.find(clipKey);
							if(cachedFingerprint != impl_->clipContentFingerprints.end()) {
								priorReplacementClipFingerprint =
									cachedFingerprint->second;
							} else {
								priorReplacementStatus =
									ComputeClipDeterministicContentFingerprint(
										*refresh.streamedBinding->clip,
										priorReplacementClipFingerprint);
								if(priorReplacementStatus == AnimationStatus::Ok) {
									try {
										impl_->clipContentFingerprints.emplace(
											clipKey,
											priorReplacementClipFingerprint);
									} catch(const std::bad_alloc&) {
										priorReplacementStatus =
											AnimationStatus::InternalError;
									}
								}
							}
						}
						if(priorReplacementStatus != AnimationStatus::Ok) {
							statuses[index] = priorReplacementStatus;
							continue;
						}
						replacementClipFingerprint =
							priorReplacementClipFingerprint;
					}
				}
				bool changed = false;
				bool replacedThisClipPublication = false;
				statuses[index] = RefreshAnimatorDirectClipStreamingLocked(
					refresh.handle,
					*refresh.streamedBinding,
					refresh.sharedValidatedClipView,
					refresh.expectedClipAssetId,
					refresh.allowClipPublicationReplacement,
					refresh.playOnBind,
					replacementClipFingerprint,
					prepared != nullptr && prepared->hasEventStream
						? &prepared->eventStream : nullptr,
					prepared != nullptr ? &prepared->playback : nullptr,
					prepared != nullptr ? &prepared->playbackConfiguration : nullptr,
					changed,
					replacedThisClipPublication);
				if(changed) {
					changedAnimators.push_back(refresh.handle);
					replacedClipPublication = replacedClipPublication
						|| replacedThisClipPublication;
				}
			}
		}
		if(replacedClipPublication) {
			// A shared generation publication invalidates the captured asset world
			// once for the complete batch, not once per prefab runtime record.
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Direct Clip Lifecycle",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			(void)impl_->InvalidateLifecycleLocked(
				AnimationLifecycleBoundary::Asset);
		}
		if(!changedAnimators.empty()) {
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Streamed Evaluation Cache",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			const std::size_t activeCount = impl_->instances->ActiveCount();
			const std::size_t halfActiveCount = activeCount / 2U + activeCount % 2U;
			if(changedAnimators.size() >= halfActiveCount) {
				// A shared publication commonly promotes nearly the complete crowd. One
				// linear clear is much cheaper than thousands of ordered-map lower_bounds;
				// broader eviction is correctness-safe because cache entries are optional.
				impl_->evaluationCache.InvalidateAll();
			} else {
				(void)impl_->evaluationCache.InvalidateAnimators(changedAnimators);
			}
			// One immutable stream publication changes one runtime declaration epoch,
			// regardless of how many prefab records consumed the shared bytes.
			impl_->frameDeclarationEpoch.fetch_add(1U, std::memory_order_acq_rel);
		}
		lock.unlock();
		if(replacedClipPublication) {
			// Presentation owns a separate protected publication. Notify it only after
			// releasing runtime ownership so its retirement path cannot form a lock
			// cycle with animation capture or render publication.
			TERRAENGINE_PROFILE_SCOPE("Animation: Invalidate Direct Clip Presentation",
				TerraEngine::Profiling::ProfilerModuleId::CpuEngine);
			(void)AnimationPresentationSystem::Instance().NotifyLifecycleInvalidation(
				AnimationLifecycleBoundary::Asset);
		}
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
		const std::shared_ptr<const AnimationAuthorityFrame> authority =
			LoadPublishedAuthorityFrame();
		const AnimationAuthorityRow* row = authority ? authority->Find(handle) : nullptr;
		return row ? row->snapshot : std::shared_ptr<const AnimationSnapshot>{};
	}

	AnimatorPlaybackStateSnapshot AnimationSystem::LoadAnimatorPlaybackState(
		AnimatorInstanceHandle handle) const noexcept
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const std::shared_ptr<const Impl::CompactRuntimeStateRoot> compactRoot =
			impl_->compactRuntimeStates.load(std::memory_order_acquire);
		if(const Impl::CompactRuntimeState* compact =
			Impl::FindCompactRuntimeState(compactRoot, handle)) {
			return compact->playback.Snapshot();
		}
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
			const std::shared_ptr<const AnimationAuthorityFrame> authority =
				impl_->authorityPublisher.Load();
			const AnimationAuthorityRow* published = authority
				? authority->Find(request.animator) : nullptr;
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
			std::shared_ptr<const AnimationRagdollExclusionSnapshot> stagedExclusion;
			if(request.operation == NativeAnimationRagdollOperation::Activate) {
				try {
					auto next = std::make_shared<AnimationRagdollExclusionSnapshot>();
					const auto current = impl_->ragdollExclusions.load(std::memory_order_acquire);
					if(current) next->entries = current->entries;
					if(current && current->publicationGeneration == std::numeric_limits<std::uint64_t>::max())
						throw std::bad_alloc{};
					next->publicationGeneration = current ? current->publicationGeneration + 1U : 1U;
					const AnimatorInstanceHandle handle{request.animator.slotIndex, request.animator.generation};
					const auto found = std::find_if(next->entries.begin(), next->entries.end(),
						[handle](const AnimationRagdollExclusion& entry) { return entry.animator == handle; });
					if(found == next->entries.end()) {
						if(next->entries.size() >= 4096U) throw std::bad_alloc{};
						next->entries.push_back({handle, AnimationRagdollExclusionState::PendingActivate,
							request.correlationId, RagdollExternalOwnerId(handle)});
					} else {
						found->state = AnimationRagdollExclusionState::PendingActivate;
						found->correlationId = request.correlationId;
						found->externalOwnerId = RagdollExternalOwnerId(handle);
					}
					stagedExclusion = std::move(next);
				} catch(const std::bad_alloc&) {
					// Do not accept an Activate that cannot become visible to the
					// preceding movement authority boundary in the same frame.
					NativeAnimationRagdollReceipt rejected{};
					rejected.header = MakeNativeHeader(
						NativeAnimationRecordKind::RagdollReceipt, sizeof(rejected));
					rejected.status = NativeAnimationStatus::HardLimitExceeded;
					rejected.animator = request.animator;
					rejected.operation = request.operation;
					rejected.profileGeneration = request.profileGeneration;
					rejected.correlationId = request.correlationId;
					rejected.observableTick = request.targetTick;
					return rejected;
				}
			}
			NativeAnimationRagdollReceipt receipt =
				impl_->advancedCommands->Submit(request, *impl_->instances);
			if(receipt.status == NativeAnimationStatus::Queued && stagedExclusion)
				impl_->ragdollExclusions.store(std::move(stagedExclusion), std::memory_order_release);
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

	void AnimationSystem::PublishRootControllerBindings(
		std::shared_ptr<const AnimationRootControllerBindingSnapshot> bindings) noexcept
	{
		impl_->rootControllerBindings.store(std::move(bindings), std::memory_order_release);
	}

	std::shared_ptr<const AnimationRootControllerBindingSnapshot>
	AnimationSystem::ReadRootControllerBindings() const noexcept
	{
		return impl_->rootControllerBindings.load(std::memory_order_acquire);
	}

	std::shared_ptr<const AnimationRagdollExclusionSnapshot>
	AnimationSystem::ReadRagdollExclusions() const noexcept
	{
		return impl_->ragdollExclusions.load(std::memory_order_acquire);
	}

	AnimatorInstanceHandle AnimationSystem::ResolveAnimatorForObject(
		const UUID& ownerObjectId) const noexcept
	{
		if(ownerObjectId == EMPTY_UUID) return {};
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		const std::shared_ptr<Impl::RuntimeRecordRoot> root =
			impl_->runtimeRecords.load(std::memory_order_acquire);
		if(root == nullptr) return {};
		AnimatorInstanceHandle resolved{};
		root->ForEach([&](const std::shared_ptr<Impl::RuntimeRecord>& record) {
			if(!resolved.IsValid() && record
				&& record->descriptor.ownerObjectId == ownerObjectId
				&& impl_->instances->IsAlive(record->handle)) {
				resolved = record->handle;
			}
		});
		return resolved;
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

	std::shared_ptr<const AnimationAuthorityFrame>
	AnimationSystem::LoadPublishedAuthorityFrame() const noexcept
	{
		return impl_->authorityPublisher.Load();
	}

	AnimationProductionIntegrationSnapshot
	AnimationSystem::ReadProductionIntegrationSnapshot() const noexcept
	{
		std::scoped_lock lock(impl_->runtimeOwnershipMutex);
		std::scoped_lock finalizeLock(impl_->workPlanFinalizeMutex);
		AnimationProductionIntegrationSnapshot snapshot;
		snapshot.planStatus = impl_->workPlan.status;
		snapshot.rejectionReason = impl_->workPlanRejectionReason;
		snapshot.rejectionItemIndex = impl_->workPlanRejectionItemIndex;
		snapshot.executionPolicy = impl_->workPlan.executionPolicy;
		snapshot.availableWorkerCount = impl_->availableWorkerCount;
		snapshot.selectedWorkerCount = impl_->workPlan.selectedWorkerCount;
		snapshot.itemCount = impl_->workPlan.items.size();
		snapshot.rangeCount = impl_->workPlan.ranges.size();
		snapshot.scratchBytes = impl_->workPlan.totalScratchBytes;
		snapshot.cacheEntryCount = impl_->evaluationCache.Size();
		snapshot.reusedItemCount =
			impl_->cacheReusedAnimatorCount.load(std::memory_order_acquire)
			+ impl_->sharedPoseAnimatorCount.load(std::memory_order_acquire);
		snapshot.fallbackItemCount =
			impl_->fallbackAnimatorCount.load(std::memory_order_acquire);
		snapshot.bindingEpisodeCount =
			impl_->bindingEpisodeCount.load(std::memory_order_acquire);
		snapshot.bindingReconciliationNanoseconds =
			impl_->bindingReconciliationNanoseconds.load(std::memory_order_acquire);
		snapshot.bindingReconciledCount =
			impl_->bindingReconciledCount.load(std::memory_order_acquire);
		snapshot.bindingFailureCount =
			impl_->bindingFailureCount.load(std::memory_order_acquire);
		snapshot.memory = impl_->memoryController.Snapshot();
		snapshot.deterministicPublicationValidated =
			impl_->deterministicPublicationValidated;
		snapshot.cancellationRequested =
			impl_->frameCancellationRequested.load(std::memory_order_acquire);
		return snapshot;
	}

	std::string_view ToString(AnimationPlanRejectionReason reason) noexcept
	{
		switch(reason) {
		case AnimationPlanRejectionReason::None: return "None";
		case AnimationPlanRejectionReason::PlanningIncomplete: return "PlanningIncomplete";
		case AnimationPlanRejectionReason::PlaybackAdvance: return "PlaybackAdvance";
		case AnimationPlanRejectionReason::GenerationOverflow: return "GenerationOverflow";
		case AnimationPlanRejectionReason::ExecutionModeNotGpuVisual: return "ExecutionModeNotGpuVisual";
		case AnimationPlanRejectionReason::FrameIdentityInvalid: return "FrameIdentityInvalid";
		case AnimationPlanRejectionReason::GpuLoweringRejected: return "GpuLoweringRejected";
		case AnimationPlanRejectionReason::ResidentPayloadInvalid: return "ResidentPayloadInvalid";
		case AnimationPlanRejectionReason::CompactClassificationRejected: return "CompactClassificationRejected";
		case AnimationPlanRejectionReason::CompactInputIncomplete: return "CompactInputIncomplete";
		case AnimationPlanRejectionReason::SchedulerRejected: return "SchedulerRejected";
		case AnimationPlanRejectionReason::MemoryReservationRejected: return "MemoryReservationRejected";
		case AnimationPlanRejectionReason::AllocationFailure: return "AllocationFailure";
		}
		return "Unknown";
	}
}
