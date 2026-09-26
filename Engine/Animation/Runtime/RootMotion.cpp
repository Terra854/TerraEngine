// RootMotion.cpp - Loop-aware root transforms without live scene mutation.

#include "Animation/Runtime/RootMotion.h"

#include "Animation/AnimationSystem.h"

#include "Components/Transform.h"
#include "Core/Object/Object.h"
#include "Core/Object/ObjectManager.h"
#include "Physics/IncrementalBodyStateStore.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>

namespace TerraEngine::Animation {
	namespace {
		struct YawTransform {
			AnimationFloat3 translation{};
			float yaw = 0.0F;
		};

		AnimationFloat3 RotateAroundY(AnimationFloat3 value, float yaw) noexcept {
			const float cosine = std::cos(yaw);
			const float sine = std::sin(yaw);
			return {
				cosine * value.x + sine * value.z,
				value.y,
				-sine * value.x + cosine * value.z
			};
		}

		YawTransform Compose(const YawTransform& left, const YawTransform& right) noexcept {
			const AnimationFloat3 rotated = RotateAroundY(right.translation, left.yaw);
			return {{
				left.translation.x + rotated.x,
				left.translation.y + rotated.y,
				left.translation.z + rotated.z
			}, left.yaw + right.yaw};
		}

		YawTransform Inverse(const YawTransform& value) noexcept {
			const float inverseYaw = -value.yaw;
			return {RotateAroundY({
				-value.translation.x,
				-value.translation.y,
				-value.translation.z
			}, inverseYaw), inverseYaw};
		}

		YawTransform Power(YawTransform value, std::int64_t exponent) noexcept {
			// Exponentiation by squaring keeps multi-loop catch-up logarithmic. The
			// unsigned conversion is safe even for INT64_MIN.
			std::uint64_t magnitude = 0U;
			if(exponent < 0) {
				value = Inverse(value);
				magnitude = static_cast<std::uint64_t>(-(exponent + 1)) + 1U;
			} else {
				magnitude = static_cast<std::uint64_t>(exponent);
			}
			YawTransform result{};
			while(magnitude != 0U) {
				if((magnitude & 1U) != 0U) result = Compose(result, value);
				magnitude >>= 1U;
				if(magnitude != 0U) value = Compose(value, value);
			}
			return result;
		}

		bool TrySubtract(std::int64_t left, std::int64_t right, std::int64_t& output) noexcept {
			if((right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)
				|| (right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)) return false;
			output = left - right;
			return true;
		}

		bool TryAdd(std::int64_t left, std::int64_t right, std::int64_t& output) noexcept {
			if((right > 0 && left > std::numeric_limits<std::int64_t>::max() - right)
				|| (right < 0 && left < std::numeric_limits<std::int64_t>::min() - right)) return false;
			output = left + right;
			return true;
		}

		bool IsFiniteDelta(const AnimationRootMotionDelta& delta) noexcept {
			return delta.animator.IsValid() && delta.generation != 0U
				&& delta.sourceBeginTick != delta.sourceEndTick
				&& IsFinite(delta.localTranslation) && IsFinite(delta.worldTranslation)
				&& std::isfinite(delta.localYawRadians) && std::isfinite(delta.worldYawRadians)
				&& std::isfinite(delta.evaluationDurationSeconds)
				&& delta.evaluationDurationSeconds >= 0.0;
		}

		glm::quat ToGlm(AnimationQuaternion value) noexcept
		{
			return {value.w, value.x, value.y, value.z};
		}

		AnimationQuaternion FromGlm(glm::quat value) noexcept
		{
			value = glm::normalize(value);
			return {value.x, value.y, value.z, value.w};
		}

		AnimationFloat3 FromGlm(glm::vec3 value) noexcept
		{
			return {value.x, value.y, value.z};
		}

		glm::vec3 ToGlm(AnimationFloat3 value) noexcept
		{
			return {value.x, value.y, value.z};
		}

		AnimationQuaternion YawRotation(float yaw) noexcept
		{
			return FromGlm(glm::angleAxis(yaw, glm::vec3{0.0F, 1.0F, 0.0F}));
		}

		bool NearlyEqual(float left, float right, float epsilon = 1.0e-4F) noexcept
		{
			return std::abs(left - right) <= epsilon;
		}

		bool NearlyEqual(AnimationFloat3 left, AnimationFloat3 right) noexcept
		{
			return NearlyEqual(left.x, right.x)
				&& NearlyEqual(left.y, right.y)
				&& NearlyEqual(left.z, right.z);
		}

		bool NearlyEquivalent(AnimationQuaternion left, AnimationQuaternion right) noexcept
		{
			const glm::quat leftValue = glm::normalize(ToGlm(left));
			const glm::quat rightValue = glm::normalize(ToGlm(right));
			return std::abs(glm::dot(leftValue, rightValue)) >= 0.9999F;
		}

		AnimationTransform CombineTransform(
			const AnimationTransform& parent,
			const AnimationTransform& local) noexcept
		{
			const glm::vec3 scaled = ToGlm(local.translation) * ToGlm(parent.scale);
			AnimationTransform world;
			world.translation = FromGlm(ToGlm(parent.translation) + ToGlm(parent.rotation) * scaled);
			world.rotation = FromGlm(ToGlm(parent.rotation) * ToGlm(local.rotation));
			world.scale = FromGlm(ToGlm(parent.scale) * ToGlm(local.scale));
			return world;
		}

		bool CaptureWorldTransform(
			ObjectManager& objects,
			Object object,
			AnimationTransform& output) noexcept
		{
			// Transform already owns one propagated canonical pose. Root motion uses
			// only its bounded local offset for animation-space rotation/scale math;
			// it never reconstructs an absolute float position by walking parents.
			const std::optional<TerraEngine::Scene::WorldPose> pose = objects.GetCanonicalWorldPose(object);
			if(!pose) return false;
			output.translation = {
				static_cast<float>(pose->position.offsetMetres.x),
				static_cast<float>(pose->position.offsetMetres.y),
				static_cast<float>(pose->position.offsetMetres.z)};
			output.rotation = FromGlm(pose->rotation);
			output.scale = FromGlm(pose->scale);
			return IsFinite(output) && IsNormalized(output.rotation);
		}

		bool SameSourceTransform(
			const AnimationTransform& expected,
			const AnimationTransform& actual) noexcept
		{
			return NearlyEqual(expected.translation, actual.translation)
				&& NearlyEquivalent(expected.rotation, actual.rotation)
				&& NearlyEqual(expected.scale, actual.scale);
		}

		bool CompatibleRootBasis(const AnimationRootControllerBinding& binding) noexcept
		{
			const AnimationFloat3 scale = binding.capturedRelativeBasisScale;
			if(!IsFinite(scale) || scale.x <= 0.0F || scale.y <= 0.0F || scale.z <= 0.0F
				|| std::abs(scale.x - scale.y) > 1.0e-3F
				|| std::abs(scale.x - scale.z) > 1.0e-3F) return false;
			const glm::quat q = ToGlm(binding.capturedRelativeBasisRotation);
			if(!std::isfinite(q.x) || !std::isfinite(q.y) || !std::isfinite(q.z)
				|| !std::isfinite(q.w) || glm::dot(q, q) < 1.0e-8F) return false;
			const glm::vec3 up = glm::normalize(q) * glm::vec3{0.0F, 1.0F, 0.0F};
			return std::abs(up.x) <= 1.0e-3F && std::abs(up.y - 1.0F) <= 1.0e-3F
				&& std::abs(up.z) <= 1.0e-3F;
		}

		bool ConvertWorldTargetToLocal(
			ObjectManager& objects,
			Object object,
			const AnimationTransform& desiredWorld,
			glm::vec3& localPosition,
			glm::quat& localRotation) noexcept
		{
			const std::optional<Object> parent = objects.getParent(object);
			if(!parent) {
				localPosition = ToGlm(desiredWorld.translation);
				localRotation = ToGlm(desiredWorld.rotation);
				return true;
			}
			AnimationTransform parentWorld;
			if(!CaptureWorldTransform(objects, *parent, parentWorld)
				|| std::abs(parentWorld.scale.x) <= 1.0e-8F
				|| std::abs(parentWorld.scale.y) <= 1.0e-8F
				|| std::abs(parentWorld.scale.z) <= 1.0e-8F) return false;
			const glm::quat inverseParent = glm::inverse(ToGlm(parentWorld.rotation));
			const glm::vec3 parentRelative = inverseParent
				* (ToGlm(desiredWorld.translation) - ToGlm(parentWorld.translation));
			localPosition = parentRelative / ToGlm(parentWorld.scale);
			localRotation = glm::normalize(inverseParent * ToGlm(desiredWorld.rotation));
			return std::isfinite(localPosition.x) && std::isfinite(localPosition.y)
				&& std::isfinite(localPosition.z) && std::isfinite(localRotation.x)
				&& std::isfinite(localRotation.y) && std::isfinite(localRotation.z)
				&& std::isfinite(localRotation.w);
		}

		AnimationFloat3 LocalMotionToWorld(
			const AnimationFloat3& local,
			const AnimationTransform& source) noexcept
		{
			return FromGlm(ToGlm(source.rotation) * (ToGlm(local) * ToGlm(source.scale)));
		}

		bool IsMutatingMode(AnimationRootMotionApplicationMode mode) noexcept
		{
			return mode == AnimationRootMotionApplicationMode::ApplyToTransform
				|| mode == AnimationRootMotionApplicationMode::ApplyToCharacter
				|| mode == AnimationRootMotionApplicationMode::ApplyToPhysicsBody;
		}
	}

	AnimationStatus ExtractRootMotion(
		const AnimationRootMotionExtractionRequest& request,
		AnimationRootMotionDelta& output) noexcept {
		if(!request.animator.IsValid() || request.generation == 0U
			|| request.sourceBeginTick == request.sourceEndTick
			|| !IsFinite(request.begin.translation) || !IsFinite(request.end.translation)
			|| !IsFinite(request.accumulatedCycleTranslation)
			|| !std::isfinite(request.begin.yawRadians) || !std::isfinite(request.end.yawRadians)
			|| !std::isfinite(request.accumulatedCycleYawRadians)
			|| !std::isfinite(request.evaluationDurationSeconds)
			|| request.evaluationDurationSeconds < 0.0) {
			return AnimationStatus::InvalidArgument;
		}
		std::int64_t cycles = 0;
		if(!TrySubtract(request.end.loopIndex, request.begin.loopIndex, cycles)) {
			return AnimationStatus::RangeOverflow;
		}
		const YawTransform cycle{request.accumulatedCycleTranslation, request.accumulatedCycleYawRadians};
		const YawTransform beginAbsolute = Compose(
			Power(cycle, request.begin.loopIndex),
			{request.begin.translation, request.begin.yawRadians});
		const YawTransform endAbsolute = Compose(
			Power(cycle, request.end.loopIndex),
			{request.end.translation, request.end.yawRadians});
		const YawTransform delta = Compose(Inverse(beginAbsolute), endAbsolute);
		if(!IsFinite(delta.translation) || !std::isfinite(delta.yaw)) return AnimationStatus::RangeOverflow;

		AnimationRootMotionDelta staged;
		staged.animator = request.animator;
		staged.sourceBeginTick = request.sourceBeginTick;
		staged.sourceEndTick = request.sourceEndTick;
		staged.localTranslation = delta.translation;
		// Parent/world conversion belongs to the later protected consumer. Until
		// then world equals the extracted animator-local request exactly.
		staged.worldTranslation = delta.translation;
		staged.localYawRadians = delta.yaw;
		staged.worldYawRadians = delta.yaw;
		staged.accumulatedCycles = cycles;
		staged.sourceStateId = request.sourceStateId;
		staged.layerIndex = request.layerIndex;
		staged.generation = request.generation;
		staged.policy = request.policy;
		staged.sourceGeneration = request.sourceGeneration;
		staged.evaluationDurationSeconds = request.evaluationDurationSeconds;
		output = staged;
		return AnimationStatus::Ok;
	}

	AnimationStatus AnimationRootMotionAccumulator::Accumulate(
		const AnimationRootMotionDelta& delta) noexcept {
		if(!IsFiniteDelta(delta) || delta.policy == AnimatorRootMotionPolicy::Ignore) {
			return AnimationStatus::InvalidArgument;
		}
		if(!accumulated_.has_value()) {
			accumulated_ = delta;
			return AnimationStatus::Ok;
		}

		const AnimationRootMotionDelta& current = *accumulated_;
		if(current.animator != delta.animator || current.generation != delta.generation
			|| current.policy != delta.policy || current.sourceStateId != delta.sourceStateId
			|| current.layerIndex != delta.layerIndex
			|| current.sourceGeneration != delta.sourceGeneration) {
			return AnimationStatus::StaleHandle;
		}
		if(current.sourceBeginTick == delta.sourceBeginTick
			&& current.sourceEndTick == delta.sourceEndTick) return AnimationStatus::Rejected;
		if(current.sourceEndTick != delta.sourceBeginTick) return AnimationStatus::InvalidArgument;

		std::int64_t cycles = 0;
		if(!TryAdd(current.accumulatedCycles, delta.accumulatedCycles, cycles)) {
			return AnimationStatus::RangeOverflow;
		}
		const YawTransform combinedLocal = Compose(
			{current.localTranslation, current.localYawRadians},
			{delta.localTranslation, delta.localYawRadians});
		const YawTransform combinedWorld = Compose(
			{current.worldTranslation, current.worldYawRadians},
			{delta.worldTranslation, delta.worldYawRadians});
		if(!IsFinite(combinedLocal.translation) || !IsFinite(combinedWorld.translation)
			|| !std::isfinite(combinedLocal.yaw) || !std::isfinite(combinedWorld.yaw)) {
			return AnimationStatus::RangeOverflow;
		}

		AnimationRootMotionDelta staged = current;
		staged.sourceEndTick = delta.sourceEndTick;
		staged.localTranslation = combinedLocal.translation;
		staged.localYawRadians = combinedLocal.yaw;
		staged.worldTranslation = combinedWorld.translation;
		staged.worldYawRadians = combinedWorld.yaw;
		staged.accumulatedCycles = cycles;
		// A merged sample spans both physical evaluations; clip tick progression
		// can differ from wall time when an animator changes playback speed.
		staged.evaluationDurationSeconds += delta.evaluationDurationSeconds;
		if(!std::isfinite(staged.evaluationDurationSeconds)) return AnimationStatus::RangeOverflow;
		accumulated_ = staged;
		return AnimationStatus::Ok;
	}

	std::optional<AnimationRootMotionDelta> AnimationRootMotionAccumulator::Consume() noexcept {
		std::optional<AnimationRootMotionDelta> result = accumulated_;
		accumulated_.reset();
		return result;
	}

	struct AnimationRootMotionResultPublisher::PendingScene {
		std::string transactionId;
		AnimationRootMotionApplicationResult result;
	};

	struct AnimationRootMotionResultPublisher::PendingPhysics {
		std::uint64_t commandId = 0U;
		AnimationRootMotionApplicationResult result;
		UUID objectId = EMPTY_UUID;
		std::uint32_t objectGeneration = 0U;
		std::uint32_t componentGeneration = 0U;
		AnimationFloat3 startPosition{};
		AnimationFloat3 requestedEndPosition{};
		AnimationQuaternion startRotation{};
		AnimationQuaternion requestedEndRotation{};
		bool character = false;
	};

	struct AnimationRootMotionResultPublisher::PendingRootInterval {
		Physics::RootMotionIntervalId intervalId;
		AnimationRootMotionApplicationResult result;
		std::uint64_t lastObservedTickSequence = 0U;
	};

	std::size_t AnimationRootMotionResultPublisher::RootIntervalIdHash::operator()(
		const Physics::RootMotionIntervalId& id) const noexcept
	{
		std::uint64_t value = id.world.worldId.value;
		value ^= static_cast<std::uint64_t>(id.world.worldGeneration)
			+ 0x9e3779b97f4a7c15ULL + (value << 6U) + (value >> 2U);
		value ^= id.sequence + 0x9e3779b97f4a7c15ULL
			+ (value << 6U) + (value >> 2U);
		return static_cast<std::size_t>(value);
	}

	struct AnimationRootMotionResultPublisher::ApplicationRecord {
		AnimationRootMotionApplicationRequest request;
		AnimationRootMotionApplicationReceipt receipt;
	};

	struct AnimationRootMotionResultPublisher::AppliedMutationRecord {
		AnimatorInstanceHandle animator;
		AnimationGeneration generation = 0U;
		std::uint64_t publicationGeneration = 0U;
	};

	// These definitions intentionally live after the private pending-record
	// definitions. Keeping destruction out of the public header lets any system
	// include RootMotion.h without instantiating std::vector on incomplete types.
	AnimationRootMotionResultPublisher::AnimationRootMotionResultPublisher()
	{
		// Both histories are fixed-size rings once warm. Reserving their records and
		// lookup buckets up front keeps the per-frame publication path allocation-free
		// for the common steady-state animated crowd.
		applications_.reserve(kRetainedRootMotionApplications);
		results_.reserve(kRetainedRootMotionResults);
		applicationIndex_.reserve(kRetainedRootMotionApplications + 1U);
		resultIndex_.reserve(kRetainedRootMotionResults + 1U);
	}
	AnimationRootMotionResultPublisher::~AnimationRootMotionResultPublisher() = default;

	namespace {
		bool ValidMode(AnimationRootMotionApplicationMode mode) noexcept
		{
			return mode >= AnimationRootMotionApplicationMode::Ignore
				&& mode <= AnimationRootMotionApplicationMode::DeterministicNetwork;
		}

		bool ValidApplicationRequest(const AnimationRootMotionApplicationRequest& request) noexcept
		{
			return request.delta.animator.IsValid() && request.delta.generation != 0U
				&& request.delta.sourceBeginTick != request.delta.sourceEndTick
				&& request.delta.sourceBeginTick >= 0 && request.delta.sourceEndTick >= 0
				&& request.ownerObjectId != EMPTY_UUID && request.objectGeneration != 0U
				&& request.componentGeneration != 0U && request.producerId != 0U
				&& request.producerSequence != 0U && request.correlationId != 0U
				&& request.idempotencyKey != 0U && request.publicationGeneration != 0U
				&& ValidMode(request.mode) && IsFiniteDelta(request.delta)
				&& (!request.sourceObjectWorldTransformCaptured
					|| (IsFinite(request.sourceObjectWorldTransform)
						&& IsNormalized(request.sourceObjectWorldTransform.rotation)))
				&& std::isfinite(request.fixedDurationSeconds)
				&& request.fixedDurationSeconds > 0.0F;
		}

		bool SameApplication(
			const AnimationRootMotionApplicationRequest& left,
			const AnimationRootMotionApplicationRequest& right) noexcept
		{
			return left.delta == right.delta
				&& left.ownerObjectId == right.ownerObjectId
				&& left.objectGeneration == right.objectGeneration
				&& left.componentGeneration == right.componentGeneration
				&& left.sourceObjectWorldTransform == right.sourceObjectWorldTransform
				&& left.sourceObjectWorldTransformCaptured == right.sourceObjectWorldTransformCaptured
				&& left.mode == right.mode && left.producerId == right.producerId
				&& left.producerSequence == right.producerSequence
				&& left.correlationId == right.correlationId
				&& left.idempotencyKey == right.idempotencyKey
				&& left.publicationGeneration == right.publicationGeneration
				&& left.fixedDurationSeconds == right.fixedDurationSeconds;
		}

		AnimationRootMotionApplicationRequest ResolveWorldMotion(
			const AnimationRootMotionApplicationRequest& request) noexcept
		{
			AnimationRootMotionApplicationRequest resolved = request;
			if(request.sourceObjectWorldTransformCaptured) {
				resolved.delta.worldTranslation = LocalMotionToWorld(
					request.delta.localTranslation, request.sourceObjectWorldTransform);
				resolved.delta.worldYawRadians = request.delta.localYawRadians;
			}
			return resolved;
		}

		AnimationRootMotionApplicationResult MakeApplicationResult(
			const AnimationRootMotionApplicationRequest& request) noexcept
		{
			AnimationRootMotionApplicationResult result;
			result.animator = request.delta.animator;
			result.generation = request.delta.generation;
			result.sourceBeginTick = request.delta.sourceBeginTick;
			result.sourceEndTick = request.delta.sourceEndTick;
			result.requestedTranslation = request.delta.worldTranslation;
			result.requestedYawRadians = request.delta.worldYawRadians;
			result.localTranslation = request.delta.localTranslation;
			result.localYawRadians = request.delta.localYawRadians;
			result.requestedRotation = YawRotation(request.delta.worldYawRadians);
			result.ownerObjectId = request.ownerObjectId;
			result.objectGeneration = request.objectGeneration;
			result.componentGeneration = request.componentGeneration;
			result.sourceStateId = request.delta.sourceStateId;
			result.accumulatedCycles = request.delta.accumulatedCycles;
			result.layerIndex = request.delta.layerIndex;
			// The current runtime evaluates one root contribution. Keeping this
			// explicit avoids implying that a missing value means zero influence.
			result.layerContribution = 1.0F;
			result.authoredPolicy = request.delta.policy;
			result.applicationMode = request.mode;
			result.producerId = request.producerId;
			result.producerSequence = request.producerSequence;
			result.idempotencyKey = request.idempotencyKey;
			result.correlationId = request.correlationId;
			result.publicationGeneration = request.publicationGeneration;
			return result;
		}

		TerraEngine::Physics::PhysicsQuaternion ComposeYaw(
			TerraEngine::Physics::PhysicsQuaternion rotation,
			float yaw) noexcept
		{
			const float half = yaw * 0.5F;
			const TerraEngine::Physics::PhysicsQuaternion delta{0.0F, std::sin(half), 0.0F, std::cos(half)};
			return {
				rotation.w * delta.x + rotation.x * delta.w + rotation.y * delta.z - rotation.z * delta.y,
				rotation.w * delta.y - rotation.x * delta.z + rotation.y * delta.w + rotation.z * delta.x,
				rotation.w * delta.z + rotation.x * delta.y - rotation.y * delta.x + rotation.z * delta.w,
				rotation.w * delta.w - rotation.x * delta.x - rotation.y * delta.y - rotation.z * delta.z};
		}

		AnimationQuaternion FromPhysicsRotation(
			TerraEngine::Physics::PhysicsQuaternion value) noexcept
		{
			return FromGlm({value.w, value.x, value.y, value.z});
		}

		TerraEngine::Physics::PhysicsQuaternion ToPhysicsRotation(
			AnimationQuaternion value) noexcept
		{
			return {value.x, value.y, value.z, value.w};
		}

		AnimationStatus FromPhysicsStatus(TerraEngine::Physics::PhysicsResultStatus status) noexcept
		{
			using TerraEngine::Physics::PhysicsResultStatus;
			switch(status) {
			case PhysicsResultStatus::Success: return AnimationStatus::Ok;
			case PhysicsResultStatus::Cancelled: return AnimationStatus::Cancelled;
			case PhysicsResultStatus::StaleTarget: return AnimationStatus::StaleHandle;
			case PhysicsResultStatus::Rejected:
			case PhysicsResultStatus::LimitExceeded:
			case PhysicsResultStatus::Conflict: return AnimationStatus::Rejected;
			case PhysicsResultStatus::Accepted: return AnimationStatus::Queued;
			default: return AnimationStatus::InternalError;
			}
		}

		float RelativeYaw(AnimationQuaternion start, AnimationQuaternion end) noexcept
		{
			return glm::yaw(glm::normalize(glm::inverse(ToGlm(start)) * ToGlm(end)));
		}
	}

	AnimationRootMotionResultPublisher& AnimationRootMotionResultPublisher::Instance()
	{
		static AnimationRootMotionResultPublisher publisher;
		return publisher;
	}

	std::size_t AnimationRootMotionResultPublisher::ApplicationIdentityHash::operator()(
		const ApplicationIdentity& identity) const noexcept
	{
		// This is the same inexpensive avalanche combine used by many hash tables.
		// Equality still compares every identity field, so a hash collision can never
		// merge receipts belonging to different animators or producers.
		std::size_t hash = std::hash<std::uint64_t>{}(
			(static_cast<std::uint64_t>(identity.animatorSlot) << 32U)
				| identity.animatorGeneration);
		auto combine = [&hash](std::uint64_t value) {
			hash ^= std::hash<std::uint64_t>{}(value)
				+ static_cast<std::size_t>(0x9E3779B97F4A7C15ULL)
				+ (hash << 6U) + (hash >> 2U);
		};
		combine(identity.producerId);
		combine(identity.idempotencyKey);
		return hash;
	}

	std::size_t AnimationRootMotionResultPublisher::ResultIdentityHash::operator()(
		const ResultIdentity& identity) const noexcept
	{
		std::size_t hash = ApplicationIdentityHash{}(identity.application);
		hash ^= std::hash<std::uint64_t>{}(identity.correlationId)
			+ static_cast<std::size_t>(0x9E3779B97F4A7C15ULL)
			+ (hash << 6U) + (hash >> 2U);
		hash ^= std::hash<bool>{}(identity.idempotencyConflict)
			+ static_cast<std::size_t>(0x9E3779B97F4A7C15ULL)
			+ (hash << 6U) + (hash >> 2U);
		return hash;
	}

	AnimationRootMotionResultPublisher::ApplicationIdentity
	AnimationRootMotionResultPublisher::MakeApplicationIdentity(
		const AnimationRootMotionApplicationRequest& request) noexcept
	{
		return {
			request.delta.animator.slotIndex,
			request.delta.animator.generation,
			request.producerId,
			request.idempotencyKey};
	}

	AnimationRootMotionResultPublisher::ApplicationIdentity
	AnimationRootMotionResultPublisher::MakeApplicationIdentity(
		const AnimationRootMotionApplicationResult& result) noexcept
	{
		return {
			result.animator.slotIndex,
			result.animator.generation,
			result.producerId,
			result.idempotencyKey};
	}

	AnimationRootMotionResultPublisher::ResultIdentity
	AnimationRootMotionResultPublisher::MakeResultIdentity(
		const AnimationRootMotionApplicationResult& result) noexcept
	{
		return {
			MakeApplicationIdentity(result),
			result.correlationId,
			result.idempotencyConflict};
	}

	AnimationRootMotionResultPublisher::ApplicationRecord*
	AnimationRootMotionResultPublisher::FindApplicationLocked(
		const AnimationRootMotionApplicationRequest& request) noexcept
	{
		const auto found = applicationIndex_.find(MakeApplicationIdentity(request));
		if(found == applicationIndex_.end() || found->second >= applications_.size()) return nullptr;
		return &applications_[found->second];
	}

	AnimationRootMotionResultPublisher::ApplicationRecord*
	AnimationRootMotionResultPublisher::FindApplicationLocked(
		const AnimationRootMotionApplicationResult& result) noexcept
	{
		const auto found = applicationIndex_.find(MakeApplicationIdentity(result));
		if(found == applicationIndex_.end() || found->second >= applications_.size()) return nullptr;
		return &applications_[found->second];
	}

	void AnimationRootMotionResultPublisher::RebuildLookupIndexesLocked()
	{
		applicationIndex_.clear();
		for(std::size_t index = 0U; index < applications_.size(); ++index) {
			applicationIndex_.emplace(
				MakeApplicationIdentity(applications_[index].request), index);
		}
		resultIndex_.clear();
		for(std::size_t index = 0U; index < results_.size(); ++index) {
			resultIndex_.emplace(MakeResultIdentity(results_[index]), index);
		}
		applicationEvictionCursor_ = 0U;
		resultEvictionCursor_ = 0U;
	}

	AnimationRootMotionApplicationReceipt AnimationRootMotionResultPublisher::ReserveApplication(
		const AnimationRootMotionApplicationRequest& request,
		bool& shouldRoute)
	{
		shouldRoute = false;
		AnimationRootMotionApplicationReceipt receipt;
		receipt.animator = request.delta.animator;
		receipt.mode = request.mode;
		receipt.generation = request.delta.generation;
		receipt.producerId = request.producerId;
		receipt.producerSequence = request.producerSequence;
		receipt.correlationId = request.correlationId;
		receipt.idempotencyKey = request.idempotencyKey;
		receipt.publicationGeneration = request.publicationGeneration;
		receipt.observableTick = request.delta.sourceEndTick;
		AnimationRootMotionApplicationResult deferred = MakeApplicationResult(request);
		if(!ValidApplicationRequest(request)) {
			receipt.status = AnimationStatus::InvalidArgument;
			deferred.status = receipt.status;
			deferred.disposition = AnimationRootMotionDisposition::Rejected;
			try {
				PublishImmediate(std::move(deferred));
			} catch(...) {
				receipt.status = AnimationStatus::InternalError;
			}
			return receipt;
		}

		std::scoped_lock lock(mutex_);
		auto retainTerminal = [this, &receipt](AnimationRootMotionApplicationResult value) {
			try {
				RetainResultLocked(std::move(value));
				return true;
			} catch(...) {
				receipt.status = AnimationStatus::InternalError;
				return false;
			}
		};
		ApplicationRecord* exact = FindApplicationLocked(request);
		if(exact != nullptr) {
			if(SameApplication(exact->request, request)) {
				exact->receipt.duplicate = true;
				receipt = exact->receipt;
				for(AnimationRootMotionApplicationResult& result : results_) {
					if(result.animator == request.delta.animator
						&& result.producerId == request.producerId
						&& result.idempotencyKey == request.idempotencyKey) result.duplicate = true;
				}
				return receipt;
			}
			receipt.status = AnimationStatus::Rejected;
			receipt.idempotencyConflict = true;
			deferred.status = receipt.status;
			deferred.disposition = AnimationRootMotionDisposition::Rejected;
			deferred.idempotencyConflict = true;
			(void)retainTerminal(std::move(deferred));
			return receipt;
		}
		auto appliedMutation = appliedMutations_.end();
		if(IsMutatingMode(request.mode)) {
			const bool consumed = std::any_of(applications_.begin(), applications_.end(), [&request](const ApplicationRecord& value) {
				return IsMutatingMode(value.request.mode)
					&& value.request.delta.animator == request.delta.animator
					&& value.request.delta.generation == request.delta.generation
					&& value.request.publicationGeneration == request.publicationGeneration
					&& (value.receipt.status == AnimationStatus::Queued
						|| value.receipt.status == AnimationStatus::Ok);
			});
			if(consumed) {
				receipt.status = AnimationStatus::Rejected;
				receipt.idempotencyConflict = true;
				deferred.status = receipt.status;
				deferred.disposition = AnimationRootMotionDisposition::Rejected;
				deferred.idempotencyConflict = true;
				(void)retainTerminal(std::move(deferred));
				return receipt;
			}
			appliedMutation = std::find_if(appliedMutations_.begin(), appliedMutations_.end(), [&request](const AppliedMutationRecord& value) {
				return value.animator == request.delta.animator;
			});
			if(appliedMutation != appliedMutations_.end()
				&& appliedMutation->publicationGeneration != 0U
				&& request.publicationGeneration <= appliedMutation->publicationGeneration) {
				receipt.status = AnimationStatus::Rejected;
				receipt.idempotencyConflict = true;
				deferred.status = receipt.status;
				deferred.disposition = AnimationRootMotionDisposition::Rejected;
				deferred.idempotencyConflict = true;
				(void)retainTerminal(std::move(deferred));
				return receipt;
			}
		}
		const bool requiresApplicationEviction =
			applications_.size() >= kRetainedRootMotionApplications;
		std::size_t evictableApplicationIndex = applications_.size();
		if(requiresApplicationEviction) {
			// The cursor names the logically oldest retained record. A cyclic scan
			// preserves FIFO eviction while skipping queued work that still owns an
			// acknowledgement from a later scene or physics boundary.
			for(std::size_t offset = 0U; offset < applications_.size(); ++offset) {
				const std::size_t candidate =
					(applicationEvictionCursor_ + offset) % applications_.size();
				if(applications_[candidate].receipt.status != AnimationStatus::Queued) {
					evictableApplicationIndex = candidate;
					break;
				}
			}
		}
		if(requiresApplicationEviction
			&& evictableApplicationIndex == applications_.size()) {
			receipt.status = AnimationStatus::HardLimitExceeded;
			deferred.status = receipt.status;
			deferred.disposition = AnimationRootMotionDisposition::Rejected;
			(void)retainTerminal(std::move(deferred));
			return receipt;
		}
		try {
			// Reserve all potentially growing storage before changing either retained
			// collection. If allocation fails, no application identity or result is
			// partially installed and an exact retry may route normally.
			const bool addAppliedMutationSlot = IsMutatingMode(request.mode)
				&& appliedMutation == appliedMutations_.end();
			if(addAppliedMutationSlot) {
				if(appliedMutations_.size() >= kRetainedRootMotionApplications) {
					receipt.status = AnimationStatus::HardLimitExceeded;
					return receipt;
				}
				appliedMutations_.reserve(appliedMutations_.size() + 1U);
			}
			receipt.status = AnimationStatus::Queued;
			deferred.status = AnimationStatus::Queued;
			deferred.disposition = AnimationRootMotionDisposition::Deferred;
			const ApplicationIdentity newIdentity = MakeApplicationIdentity(request);
			if(!requiresApplicationEviction) {
				// Append until the fixed history fills. If either lookup or optional
				// diagnostic retention fails, remove the new identity atomically.
				applications_.push_back({request, receipt});
				const std::size_t newIndex = applications_.size() - 1U;
				try {
					const auto inserted = applicationIndex_.emplace(newIdentity, newIndex);
					if(!inserted.second) throw std::bad_alloc{};
					RetainResultLocked(std::move(deferred));
				} catch(...) {
					applicationIndex_.erase(newIdentity);
					applications_.pop_back();
					throw;
				}
			} else {
				// A full history is a fixed ring. Insert the new lookup before replacing
				// the old slot, keeping both identities available until result retention
				// succeeds so exception rollback never needs another allocation.
				ApplicationRecord replacement{request, receipt};
				ApplicationRecord& slot = applications_[evictableApplicationIndex];
				const ApplicationIdentity oldIdentity = MakeApplicationIdentity(slot.request);
				const auto inserted = applicationIndex_.emplace(
					newIdentity, evictableApplicationIndex);
				if(!inserted.second) throw std::bad_alloc{};
				using std::swap;
				swap(slot, replacement);
				try {
					RetainResultLocked(std::move(deferred));
				} catch(...) {
					swap(slot, replacement);
					applicationIndex_.erase(newIdentity);
					throw;
				}
				applicationIndex_.erase(oldIdentity);
				applicationEvictionCursor_ =
					(evictableApplicationIndex + 1U) % applications_.size();
			}
			if(addAppliedMutationSlot) {
				// The placeholder does not reject a later retry unless a protected target
				// boundary actually publishes Applied or Adjusted for this animator.
				appliedMutations_.push_back({request.delta.animator, 0U, 0U});
			}
			shouldRoute = true;
			return receipt;
		} catch(...) {
			receipt.status = AnimationStatus::InternalError;
			return receipt;
		}
	}

	bool AnimationRootMotionResultPublisher::ApplicationWasRetried(
		const AnimationRootMotionApplicationRequest& request) const noexcept
	{
		std::scoped_lock lock(mutex_);
		const auto found = applicationIndex_.find(MakeApplicationIdentity(request));
		return found != applicationIndex_.end() && found->second < applications_.size()
			&& applications_[found->second].receipt.duplicate;
	}

	void AnimationRootMotionResultPublisher::UpdateApplicationReceipt(
		const AnimationRootMotionApplicationRequest& request,
		const AnimationRootMotionApplicationReceipt& receipt)
	{
		std::scoped_lock lock(mutex_);
		ApplicationRecord* found = FindApplicationLocked(request);
		if(found == nullptr) return;
		// Scalar receipt progress is always retained. Optional diagnostic strings
		// are copied separately so an allocation failure cannot unwind a protected
		// route after the target request has already been accepted.
		found->receipt.status = receipt.status;
		found->receipt.animator = receipt.animator;
		found->receipt.mode = receipt.mode;
		found->receipt.generation = receipt.generation;
		found->receipt.producerId = receipt.producerId;
		found->receipt.producerSequence = receipt.producerSequence;
		found->receipt.idempotencyKey = receipt.idempotencyKey;
		found->receipt.publicationGeneration = receipt.publicationGeneration;
		found->receipt.observableTick = receipt.observableTick;
		found->receipt.duplicate = found->receipt.duplicate || receipt.duplicate;
		found->receipt.idempotencyConflict = receipt.idempotencyConflict;
		found->receipt.physicsCommandId = receipt.physicsCommandId;
		found->receipt.rootIntervalId = receipt.rootIntervalId;
		found->receipt.correlationId = receipt.correlationId;
		if(receipt.status == AnimationStatus::Ok
			&& IsMutatingMode(found->request.mode)) {
			// Only an immediate protected scene commit returns Ok from routing. Deferred
			// scene and physics submissions remain Queued until their observer runs.
			RememberAppliedMutationLocked(*found);
		}
		try {
			found->receipt.sceneTransactionId = receipt.sceneTransactionId;
		} catch(...) {
			// Transaction result lookup is retained separately in PendingScene.
		}
	}

	std::uint64_t AnimationRootMotionResultPublisher::AbandonPendingApplication(
		const AnimationRootMotionApplicationRequest& request) noexcept
	{
		std::scoped_lock lock(mutex_);
		std::erase_if(pendingScene_, [&request](const PendingScene& value) {
			return value.result.animator == request.delta.animator
				&& value.result.producerId == request.producerId
				&& value.result.idempotencyKey == request.idempotencyKey;
		});
		std::uint64_t commandId = 0U;
		const auto physics = std::find_if(pendingPhysics_.begin(), pendingPhysics_.end(), [&request](const PendingPhysics& value) {
			return value.result.animator == request.delta.animator
				&& value.result.producerId == request.producerId
				&& value.result.idempotencyKey == request.idempotencyKey;
		});
		if(physics != pendingPhysics_.end()) {
			commandId = physics->commandId;
			pendingPhysics_.erase(physics);
		}
		const auto interval = std::find_if(pendingRootIntervals_.begin(), pendingRootIntervals_.end(),
			[&request](const PendingRootInterval& value) {
				return value.result.animator == request.delta.animator
					&& value.result.producerId == request.producerId
					&& value.result.idempotencyKey == request.idempotencyKey;
			});
		if(interval != pendingRootIntervals_.end()) {
			const std::size_t position = static_cast<std::size_t>(interval - pendingRootIntervals_.begin());
			const auto intervalId = interval->intervalId;
			if(position + 1U != pendingRootIntervals_.size()) {
				pendingRootIntervals_[position] = std::move(pendingRootIntervals_.back());
				pendingRootIntervalIndex_[pendingRootIntervals_[position].intervalId] = position;
			}
			pendingRootIntervals_.pop_back();
			pendingRootIntervalIndex_.erase(intervalId);
		}
		return commandId;
	}

	void AnimationRootMotionResultPublisher::RetainResultLocked(
		AnimationRootMotionApplicationResult result)
	{
		const ResultIdentity identity = MakeResultIdentity(result);
		const auto duplicate = resultIndex_.find(identity);
		if(duplicate != resultIndex_.end() && duplicate->second < results_.size()) {
			results_[duplicate->second] = std::move(result);
			return;
		}
		if(results_.size() < kRetainedRootMotionResults) {
			results_.push_back(std::move(result));
			const std::size_t newIndex = results_.size() - 1U;
			try {
				const auto inserted = resultIndex_.emplace(identity, newIndex);
				if(!inserted.second) throw std::bad_alloc{};
			} catch(...) {
				results_.pop_back();
				throw;
			}
			return;
		}

		// Once warm, result history is another fixed ring. Replacing the oldest
		// slot avoids shifting 4,095 large diagnostic records for every animator.
		const std::size_t replacementIndex = resultEvictionCursor_ % results_.size();
		const ResultIdentity oldIdentity = MakeResultIdentity(results_[replacementIndex]);
		const auto inserted = resultIndex_.emplace(identity, replacementIndex);
		if(!inserted.second) throw std::bad_alloc{};
		results_[replacementIndex] = std::move(result);
		resultIndex_.erase(oldIdentity);
		resultEvictionCursor_ = (replacementIndex + 1U) % results_.size();
	}

	void AnimationRootMotionResultPublisher::RememberAppliedMutationLocked(
		const ApplicationRecord& application) noexcept
	{
		if(!IsMutatingMode(application.request.mode)) return;
		const auto found = std::find_if(appliedMutations_.begin(), appliedMutations_.end(), [&application](const AppliedMutationRecord& value) {
			return value.animator == application.request.delta.animator;
		});
		if(found == appliedMutations_.end()) return;
		if(found->publicationGeneration <= application.request.publicationGeneration) {
			found->generation = application.request.delta.generation;
			found->publicationGeneration = application.request.publicationGeneration;
		}
	}

	void AnimationRootMotionResultPublisher::PublishImmediate(
		AnimationRootMotionApplicationResult result)
	{
		std::scoped_lock lock(mutex_);
		ApplicationRecord* application = FindApplicationLocked(result);
		if(application != nullptr) {
			application->receipt.status = result.status;
			result.duplicate = result.duplicate || application->receipt.duplicate;
			if(result.disposition == AnimationRootMotionDisposition::Applied
				|| result.disposition == AnimationRootMotionDisposition::Adjusted) {
				RememberAppliedMutationLocked(*application);
			}
		}
		RetainResultLocked(std::move(result));
	}

	bool AnimationRootMotionResultPublisher::RegisterSceneRequest(
		std::string transactionId,
		AnimationRootMotionApplicationResult result)
	{
		if(transactionId.empty()) return false;
		std::scoped_lock lock(mutex_);
		if(pendingScene_.size() >= kRetainedRootMotionApplications) return false;
		if(std::any_of(pendingScene_.begin(), pendingScene_.end(), [&transactionId](const PendingScene& value) {
			return value.transactionId == transactionId;
		})) return false;
		try {
			pendingScene_.push_back({std::move(transactionId), std::move(result)});
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	bool AnimationRootMotionResultPublisher::RegisterPhysicsRequest(
		std::uint64_t commandId,
		AnimationRootMotionApplicationResult result,
		UUID objectId,
		std::uint32_t objectGeneration,
		std::uint32_t componentGeneration,
		AnimationFloat3 startPosition,
		AnimationFloat3 requestedEndPosition,
		AnimationQuaternion startRotation,
		AnimationQuaternion requestedEndRotation,
		bool character)
	{
		if(commandId == 0U || objectId == EMPTY_UUID || objectGeneration == 0U || componentGeneration == 0U) return false;
		std::scoped_lock lock(mutex_);
		if(pendingPhysics_.size() >= kRetainedRootMotionApplications) return false;
		if(std::any_of(pendingPhysics_.begin(), pendingPhysics_.end(), [commandId](const PendingPhysics& value) {
			return value.commandId == commandId;
		})) return false;
		try {
			pendingPhysics_.push_back({commandId, std::move(result), objectId, objectGeneration, componentGeneration,
				startPosition, requestedEndPosition, startRotation, requestedEndRotation, character});
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	void AnimationRootMotionResultPublisher::ObserveSceneTransactionResult(
		const TerraEngine::Scene::TransactionResult& transaction)
	{
		std::scoped_lock lock(mutex_);
		const auto pending = std::find_if(pendingScene_.begin(), pendingScene_.end(), [&transaction](const PendingScene& value) {
			return value.transactionId == transaction.transactionId;
		});
		if(pending == pendingScene_.end()) return;
		AnimationRootMotionApplicationResult result = pending->result;
		if(transaction.status == TerraEngine::Scene::TransactionStatus::Committed) {
			result.status = AnimationStatus::Ok;
			result.disposition = AnimationRootMotionDisposition::Applied;
			result.appliedTranslation = result.requestedTranslation;
			result.appliedYawRadians = result.requestedYawRadians;
			result.appliedRotation = result.requestedRotation;
		} else {
			result.status = transaction.status == TerraEngine::Scene::TransactionStatus::Canceled
				? AnimationStatus::Cancelled
				: AnimationStatus::Rejected;
			result.disposition = AnimationRootMotionDisposition::Rejected;
		}
		pendingScene_.erase(pending);
		ApplicationRecord* application = FindApplicationLocked(result);
		if(application != nullptr) {
			application->receipt.status = result.status;
			result.duplicate = result.duplicate || application->receipt.duplicate;
			if(result.disposition == AnimationRootMotionDisposition::Applied
				|| result.disposition == AnimationRootMotionDisposition::Adjusted) {
				RememberAppliedMutationLocked(*application);
			}
		}
		try {
			// The authoritative receipt and once-only ledger are already terminal. A
			// diagnostic-history allocation failure may drop only this optional copy.
			RetainResultLocked(std::move(result));
		} catch(...) {
			// Never unwind or restore Queued after the scene transaction committed.
		}
	}

	void AnimationRootMotionResultPublisher::ObservePhysicsResults(
		const TerraEngine::Physics::PhysicsCommandResultSnapshot& results,
		const std::shared_ptr<const TerraEngine::Physics::IncrementalBodyStateSnapshot>& bodies,
		const std::shared_ptr<const TerraEngine::Physics::PhysicsCharacterStatePacket>& characters)
	{
		if(!results.results) return;
		std::scoped_lock lock(mutex_);
		for(const TerraEngine::Physics::PhysicsCommandResult& command : *results.results) {
			const auto pending = std::find_if(pendingPhysics_.begin(), pendingPhysics_.end(), [&command](const PendingPhysics& value) {
				return value.commandId == command.commandId;
			});
			if(pending == pendingPhysics_.end()) continue;
			AnimationRootMotionApplicationResult result = pending->result;
			try {
				result.status = FromPhysicsStatus(command.status);
				if(result.status == AnimationStatus::Ok) {
					std::optional<AnimationFloat3> publishedPosition;
					std::optional<AnimationQuaternion> publishedRotation;
					if(pending->character && characters && characters->states) {
						const auto state = std::find_if(characters->states->begin(), characters->states->end(), [&pending](const auto& value) {
							return value.objectId == pending->objectId
								&& value.objectGeneration == pending->objectGeneration
								&& value.componentGeneration == pending->componentGeneration;
						});
						if(state != characters->states->end()) {
							publishedPosition = {state->pose.position.x, state->pose.position.y, state->pose.position.z};
							publishedRotation = FromPhysicsRotation(state->pose.rotation);
						}
					} else if(!pending->character && bodies) {
						const std::vector<TerraEngine::Physics::PhysicsBodyState> flat = bodies->Flatten();
						const auto state = std::find_if(flat.begin(), flat.end(), [&pending](const auto& value) {
							return value.objectId == pending->objectId
								&& value.objectGeneration == pending->objectGeneration
								&& value.componentGeneration == pending->componentGeneration;
						});
						if(state != flat.end()) {
							publishedPosition = {state->worldPose.position.x, state->worldPose.position.y, state->worldPose.position.z};
							publishedRotation = FromPhysicsRotation(state->worldPose.rotation);
						}
					}
					if(publishedPosition && publishedRotation) {
						result.appliedTranslation = {
							publishedPosition->x - pending->startPosition.x,
							publishedPosition->y - pending->startPosition.y,
							publishedPosition->z - pending->startPosition.z};
						result.appliedRotation = FromGlm(glm::normalize(
							glm::inverse(ToGlm(pending->startRotation)) * ToGlm(*publishedRotation)));
						result.appliedYawRadians = RelativeYaw(pending->startRotation, *publishedRotation);
						result.disposition = NearlyEqual(*publishedPosition, pending->requestedEndPosition)
							&& NearlyEquivalent(*publishedRotation, pending->requestedEndRotation)
							? AnimationRootMotionDisposition::Applied
							: AnimationRootMotionDisposition::Adjusted;
					} else {
						result.status = AnimationStatus::StaleHandle;
						result.disposition = AnimationRootMotionDisposition::Rejected;
					}
				} else {
					result.disposition = AnimationRootMotionDisposition::Rejected;
				}
			} catch(...) {
				// An authoritative result cannot remain Queued because copying an optional
				// flattened body view failed. Report the terminal diagnostic honestly.
				result.status = AnimationStatus::InternalError;
				result.disposition = AnimationRootMotionDisposition::Rejected;
			}
			pendingPhysics_.erase(pending);
			ApplicationRecord* application = FindApplicationLocked(result);
			if(application != nullptr) {
				application->receipt.status = result.status;
				result.duplicate = result.duplicate || application->receipt.duplicate;
				if(result.disposition == AnimationRootMotionDisposition::Applied
					|| result.disposition == AnimationRootMotionDisposition::Adjusted) {
					RememberAppliedMutationLocked(*application);
				}
			}
			try {
				RetainResultLocked(std::move(result));
			} catch(...) {
				// Receipt and once-only state were terminalized before optional history.
			}
		}
	}

	void AnimationRootMotionResultPublisher::Retire(AnimatorInstanceHandle animator)
	{
		std::scoped_lock lock(mutex_);
		std::erase_if(pendingScene_, [animator](const PendingScene& value) {
			return value.result.animator == animator;
		});
		std::erase_if(pendingPhysics_, [animator](const PendingPhysics& value) {
			return value.result.animator == animator;
		});
		std::erase_if(pendingRootIntervals_, [animator](const PendingRootInterval& value) {
			return value.result.animator == animator;
		});
		pendingRootIntervalIndex_.clear();
		for(std::size_t index = 0U; index < pendingRootIntervals_.size(); ++index)
			pendingRootIntervalIndex_.emplace(pendingRootIntervals_[index].intervalId, index);
		std::erase_if(applications_, [animator](const ApplicationRecord& value) {
			return value.request.delta.animator == animator;
		});
		std::erase_if(appliedMutations_, [animator](const AppliedMutationRecord& value) {
			return value.animator == animator;
		});
		std::erase_if(results_, [animator](const AnimationRootMotionApplicationResult& value) {
			return value.animator == animator;
		});
		// Retirement is infrequent and may compact vector slots. Rebuilding both
		// indexes once here keeps every hot publication lookup exact afterward.
		RebuildLookupIndexesLocked();
	}

	void AnimationRootMotionResultPublisher::RetireMany(
		std::span<const AnimatorInstanceHandle> animators)
	{
		if(animators.empty()) return;
		const auto contains = [animators](AnimatorInstanceHandle animator) {
			return std::binary_search(animators.begin(), animators.end(), animator);
		};
		std::scoped_lock lock(mutex_);
		std::erase_if(pendingScene_, [&contains](const PendingScene& value) {
			return contains(value.result.animator);
		});
		std::erase_if(pendingPhysics_, [&contains](const PendingPhysics& value) {
			return contains(value.result.animator);
		});
		std::erase_if(pendingRootIntervals_, [&contains](const PendingRootInterval& value) {
			return contains(value.result.animator);
		});
		pendingRootIntervalIndex_.clear();
		for(std::size_t index = 0U; index < pendingRootIntervals_.size(); ++index)
			pendingRootIntervalIndex_.emplace(pendingRootIntervals_[index].intervalId, index);
		std::erase_if(applications_, [&contains](const ApplicationRecord& value) {
			return contains(value.request.delta.animator);
		});
		std::erase_if(appliedMutations_, [&contains](const AppliedMutationRecord& value) {
			return contains(value.animator);
		});
		std::erase_if(results_, [&contains](const AnimationRootMotionApplicationResult& value) {
			return contains(value.animator);
		});
		RebuildLookupIndexesLocked();
	}

	void AnimationRootMotionResultPublisher::CopyForAnimator(
		AnimatorInstanceHandle animator,
		std::vector<AnimationRootMotionApplicationResult>& output) const
	{
		std::vector<AnimationRootMotionApplicationResult> staged;
		std::scoped_lock lock(mutex_);
		try {
			for(const AnimationRootMotionApplicationResult& result : results_) {
				if(result.animator != animator) continue;
				AnimationRootMotionApplicationResult copied = result;
				if(copied.rootIntervalId.IsValid() && copied.firstTickResultSequence != 0U) {
					// Evidence is a rolling scene ring: a terminal result can outlive
					// its detailed ticks, so expiration is evaluated on every read.
					if(copied.rootIntervalId.world != observedRootWorld_
						|| rootTickEvidenceSize_ == 0U) {
						copied.tickEvidenceExpired = true;
					} else {
						const std::size_t oldest = (rootTickEvidenceCursor_
							+ rootTickEvidenceRing_.size() - rootTickEvidenceSize_)
							% rootTickEvidenceRing_.size();
						copied.tickEvidenceExpired = copied.tickEvidenceExpired
							|| copied.firstTickResultSequence
								< rootTickEvidenceRing_[oldest].resultSequence;
					}
				}
				staged.push_back(std::move(copied));
			}
			std::sort(staged.begin(), staged.end(), [](const auto& left, const auto& right) {
				if(left.sourceEndTick != right.sourceEndTick) return left.sourceEndTick < right.sourceEndTick;
				return left.correlationId < right.correlationId;
			});
			output.swap(staged);
		} catch(const std::bad_alloc&) {
			// Atomic copied reads retain the caller's previous complete batch.
		}
	}

	void AnimationRootMotionResultPublisher::Clear()
	{
		std::scoped_lock lock(mutex_);
		pendingScene_.clear();
		pendingPhysics_.clear();
		pendingRootIntervals_.clear();
		pendingRootIntervalIndex_.clear();
		rootResultCursor_ = 0U;
		observedRootWorld_ = {};
		rootTickEvidenceSize_ = 0U;
		rootTickEvidenceCursor_ = 0U;
		applications_.clear();
		applicationIndex_.clear();
		applicationEvictionCursor_ = 0U;
		appliedMutations_.clear();
		results_.clear();
		resultIndex_.clear();
		resultEvictionCursor_ = 0U;
	}

	AnimationStatus ApplyRagdollControllerExclusions(
		const AnimationRootControllerBindingSnapshot& bindings,
		const AnimationRagdollExclusionSnapshot& exclusions,
		Physics::PhysicsSystem& physics)
	{
		using namespace TerraEngine::Physics;
		try {
			std::vector<CharacterMotionLeaseChange> changes;
			changes.reserve(std::min<std::size_t>(exclusions.entries.size(), 256U));
			for(const AnimationRagdollExclusion& exclusion : exclusions.entries) {
				if(!exclusion.animator.IsValid()
					|| exclusion.externalOwnerId != RagdollExternalOwnerId(exclusion.animator))
					return AnimationStatus::InvalidArgument;
				const auto binding = std::find_if(bindings.active.begin(), bindings.active.end(),
					[&exclusion](const AnimationRootControllerBinding& candidate) {
						return candidate.animator == exclusion.animator;
					});
				// An unbound animator may ragdoll without a locomotion controller.
				if(binding == bindings.active.end()) continue;
				if(binding->bindingValidityGuard
					&& binding->bindingValidityGuard->Validate()
						!= CharacterMotionValidityReason::Valid)
					return AnimationStatus::StaleHandle;
				const auto existing = physics.ReadCharacterMotionLease(binding->controller);
				if(!existing) return AnimationStatus::NotReady;
				if(existing->state == CharacterMotionLeaseState::ExternalExclusive) {
					if(existing->externalOwnerId != exclusion.externalOwnerId)
						return AnimationStatus::Rejected;
					continue;
				}
				const auto duplicate = std::find_if(changes.begin(), changes.end(),
					[&existing](const CharacterMotionLeaseChange& change) {
						return change.expected.controller == existing->controller;
					});
				if(duplicate != changes.end()) {
					if(duplicate->next.externalOwnerId != exclusion.externalOwnerId)
						return AnimationStatus::Rejected;
					continue;
				}
				if(changes.size() >= 256U) return AnimationStatus::HardLimitExceeded;
				if(existing->motionEpoch == std::numeric_limits<std::uint64_t>::max()
					|| existing->publicationEpoch == std::numeric_limits<std::uint64_t>::max())
					return AnimationStatus::RangeOverflow;
				CharacterMotionLease next = *existing;
				next.owner = {};
				next.state = CharacterMotionLeaseState::ExternalExclusive;
				next.externalOwnerId = exclusion.externalOwnerId;
				++next.motionEpoch;
				++next.publicationEpoch;
				changes.push_back({*existing, next});
			}
			if(changes.empty()) return AnimationStatus::Ok;
			if(!physics.PreflightCharacterMotionLeases(changes).accepted)
				return AnimationStatus::Rejected;
			return physics.PublishCharacterMotionLeases(changes).accepted
				? AnimationStatus::Ok : AnimationStatus::Rejected;
		} catch(const std::bad_alloc&) {
			return AnimationStatus::HardLimitExceeded;
		}

	}

	bool AnimationRootMotionResultPublisher::RegisterRootMotionInterval(
		Physics::RootMotionIntervalId intervalId,
		AnimationRootMotionApplicationResult result)
	{
		if(!intervalId.IsValid()) return false;
		std::scoped_lock lock(mutex_);
		if(pendingRootIntervals_.size() >= kRetainedRootMotionApplications
			|| pendingRootIntervalIndex_.contains(intervalId)) return false;
		try {
			result.rootIntervalId = intervalId;
			pendingRootIntervals_.push_back({intervalId, std::move(result)});
			try {
				pendingRootIntervalIndex_.emplace(intervalId, pendingRootIntervals_.size() - 1U);
			} catch(...) {
				pendingRootIntervals_.pop_back();
				throw;
			}
			return true;
		} catch(const std::bad_alloc&) {
			return false;
		}
	}

	void AnimationRootMotionResultPublisher::CancelRootMotionIntervalApplication(
		Physics::RootMotionIntervalId intervalId, AnimationStatus status) noexcept
	{
		std::scoped_lock lock(mutex_);
		const auto indexed = pendingRootIntervalIndex_.find(intervalId);
		if(indexed == pendingRootIntervalIndex_.end()) return;
		const std::size_t position = indexed->second;
		AnimationRootMotionApplicationResult terminal = std::move(pendingRootIntervals_[position].result);
		terminal.status = status;
		terminal.disposition = AnimationRootMotionDisposition::Rejected;
		if(ApplicationRecord* application = FindApplicationLocked(terminal))
			application->receipt.status = status;
		try {
			RetainResultLocked(std::move(terminal));
		} catch(...) {
			// The scalar application receipt is terminal even if diagnostic
			// history cannot retain an optional copy during memory pressure.
		}
		if(position + 1U != pendingRootIntervals_.size()) {
			pendingRootIntervals_[position] = std::move(pendingRootIntervals_.back());
			pendingRootIntervalIndex_[pendingRootIntervals_[position].intervalId] = position;
		}
		pendingRootIntervals_.pop_back();
		pendingRootIntervalIndex_.erase(indexed);
	}


	bool AnimationRootMotionResultPublisher::ObserveRootMotionResultPacket(
		const Physics::RootMotionResultPacket& page)
	{
		std::scoped_lock lock(mutex_);
		if(!page.world.IsValid() || page.expiredCursor) return false;
		if(!observedRootWorld_.IsValid()) observedRootWorld_ = page.world;
		if(observedRootWorld_ != page.world) return false;
		try {
			for(const Physics::RootMotionResultRecord& record : page.records) {
				if(const auto* tick = std::get_if<Physics::RootMotionTickResult>(&record.result)) {
					AnimationRootMotionTickEvidence evidence;
					evidence.resultSequence = record.sequence;
					evidence.world = page.world;
					evidence.tick = tick->tick;
					evidence.completedStepDuration = tick->completedStepDuration;
					evidence.requestedTranslation = tick->requestedTranslation;
					evidence.realizedTranslation = tick->realizedTranslation;
					evidence.requestedYawRadians = tick->requestedYawRadians;
					evidence.realizedYawRadians = tick->realizedYawRadians;
					evidence.contributingCount = std::min<std::uint8_t>(
						tick->contributingCount,
						static_cast<std::uint8_t>(tick->contributingIntervals.size()));
					evidence.adjustedByLimit = tick->adjustedByLimit;
					evidence.adjustedByCollision = tick->adjustedByCollision;
					for(std::uint8_t index = 0U; index < evidence.contributingCount; ++index)
						evidence.contributingIntervalSequences[index] =
							tick->contributingIntervals[index].sequence;
					const bool alreadyRetained = rootTickEvidenceSize_ != 0U
						&& rootTickEvidenceRing_[(rootTickEvidenceCursor_
							+ rootTickEvidenceRing_.size() - 1U)
							% rootTickEvidenceRing_.size()].resultSequence >= record.sequence;
					if(!alreadyRetained) {
						rootTickEvidenceRing_[rootTickEvidenceCursor_] = evidence;
						rootTickEvidenceCursor_ = (rootTickEvidenceCursor_ + 1U)
							% rootTickEvidenceRing_.size();
						rootTickEvidenceSize_ = std::min(rootTickEvidenceSize_ + 1U,
							rootTickEvidenceRing_.size());
					}
					for(std::uint8_t index = 0U; index < evidence.contributingCount; ++index) {
						const auto found = pendingRootIntervalIndex_.find(tick->contributingIntervals[index]);
						if(found == pendingRootIntervalIndex_.end()) continue;
						auto& pending = pendingRootIntervals_[found->second];
						if(record.sequence <= pending.lastObservedTickSequence) continue;
						auto& result = pending.result;
						result.aggregateRequestedTranslation.x += evidence.requestedTranslation.x;
						result.aggregateRequestedTranslation.y += evidence.requestedTranslation.y;
						result.aggregateRequestedTranslation.z += evidence.requestedTranslation.z;
						result.aggregateRealizedTranslation.x += evidence.realizedTranslation.x;
						result.aggregateRealizedTranslation.y += evidence.realizedTranslation.y;
						result.aggregateRealizedTranslation.z += evidence.realizedTranslation.z;
						result.aggregateRequestedYawRadians += evidence.requestedYawRadians;
						result.aggregateRealizedYawRadians += evidence.realizedYawRadians;
						if(result.observedPhysicalTickCount != std::numeric_limits<std::uint64_t>::max())
							++result.observedPhysicalTickCount;
						result.sharedTickAttribution = result.sharedTickAttribution
							|| evidence.contributingCount > 1U;
						pending.lastObservedTickSequence = record.sequence;
					}
				} else if(const auto* terminal =
					std::get_if<Physics::RootMotionTerminalResult>(&record.result)) {
					const auto found = pendingRootIntervalIndex_.find(terminal->intervalId);
					if(found == pendingRootIntervalIndex_.end()) continue;
					const std::size_t position = found->second;
					AnimationRootMotionApplicationResult finalized =
						pendingRootIntervals_[position].result;
					finalized.finalPhysicalTick = terminal->finalTick;
					finalized.firstTickResultSequence = terminal->firstTickResultSequence;
					finalized.lastTickResultSequence = terminal->lastTickResultSequence;
					finalized.sharedTickAttribution = finalized.sharedTickAttribution
						|| terminal->sharedTickAttribution;
					if(finalized.firstTickResultSequence != 0U && rootTickEvidenceSize_ != 0U) {
					const std::size_t oldest = (rootTickEvidenceCursor_
						+ rootTickEvidenceRing_.size() - rootTickEvidenceSize_)
						% rootTickEvidenceRing_.size();
					finalized.tickEvidenceExpired = finalized.firstTickResultSequence
						< rootTickEvidenceRing_[oldest].resultSequence;
					}
					switch(terminal->disposition) {
					case Physics::RootMotionTerminalDisposition::Consumed:
						finalized.status = AnimationStatus::Ok;
						finalized.disposition = AnimationRootMotionDisposition::Applied;
						break;
					case Physics::RootMotionTerminalDisposition::Adjusted:
						finalized.status = AnimationStatus::Ok;
						finalized.disposition = AnimationRootMotionDisposition::Adjusted;
						break;
					case Physics::RootMotionTerminalDisposition::Cancelled:
						finalized.status = AnimationStatus::Cancelled;
						finalized.disposition = AnimationRootMotionDisposition::Rejected;
						break;
					case Physics::RootMotionTerminalDisposition::Rejected:
						finalized.status = AnimationStatus::Rejected;
						finalized.disposition = AnimationRootMotionDisposition::Rejected;
						break;
					}
				if(!finalized.sharedTickAttribution) {
					// Only a sole contributor can claim an independently
					// measured collision-resolved displacement.
					finalized.appliedTranslation = {
						finalized.aggregateRealizedTranslation.x,
						finalized.aggregateRealizedTranslation.y,
						finalized.aggregateRealizedTranslation.z};
					finalized.appliedYawRadians = finalized.aggregateRealizedYawRadians;
				}
				RetainResultLocked(finalized);
				if(ApplicationRecord* application = FindApplicationLocked(finalized))
					application->receipt.status = finalized.status;
				if(position + 1U != pendingRootIntervals_.size()) {
					pendingRootIntervals_[position] = std::move(pendingRootIntervals_.back());
					pendingRootIntervalIndex_[pendingRootIntervals_[position].intervalId] = position;
				}
				pendingRootIntervals_.pop_back();
				pendingRootIntervalIndex_.erase(found);
				}
			}
		} catch(const std::bad_alloc&) {
			return false;
		}
		return true;
	}

	void AnimationRootMotionResultPublisher::ObserveRootMotionIntervalResults(
		Physics::PhysicsSystem& physics)
	{
		const Physics::PhysicsWorldHandle currentWorld = physics.WorldHandle();
		// Physics may destroy its result ring on StopWorld. Terminalize old
		// generation applications here rather than leaving Queued receipts forever.
		for(;;) {
			Physics::RootMotionIntervalId stale;
			{
				std::scoped_lock lock(mutex_);
				if(observedRootWorld_ != currentWorld) {
					observedRootWorld_ = currentWorld;
					rootResultCursor_ = 0U;
					rootTickEvidenceSize_ = 0U;
					rootTickEvidenceCursor_ = 0U;
				}
				const auto found = std::find_if(pendingRootIntervals_.begin(),
					pendingRootIntervals_.end(), [&currentWorld](const PendingRootInterval& pending) {
						return pending.intervalId.world != currentWorld;
					});
				if(found == pendingRootIntervals_.end()) break;
				stale = found->intervalId;
			}
			CancelRootMotionIntervalApplication(stale, AnimationStatus::Cancelled);
		}
		if(!currentWorld.IsValid()) return;
		// Pages are copied from Physics before touching the Animation ledger, so
		// neither subsystem calls into the other while holding its own lock.
		for(std::size_t pageIndex = 0U; pageIndex < 256U; ++pageIndex) {
			std::uint64_t cursor = 0U;
			{
				std::scoped_lock lock(mutex_);
				cursor = rootResultCursor_;
			}
			const auto page = physics.ReadRootMotionResults(cursor, 256U);
			if(!page || page->expiredCursor || page->records.empty()) return;
			if(!ObserveRootMotionResultPacket(*page)) return;
			if(!physics.AcknowledgeRootMotionResults(page->nextCursor)) return;
			{
				std::scoped_lock lock(mutex_);
				rootResultCursor_ = page->nextCursor;
			}
			if(!page->hasMore) return;
		}
	}

	std::size_t AnimationRootMotionResultPublisher::CopyRootMotionTickEvidence(
		Physics::PhysicsWorldHandle world, std::uint64_t afterSequence,
		std::span<AnimationRootMotionTickEvidence> output, bool& omitted) const noexcept
	{
		omitted = false;
		std::scoped_lock lock(mutex_);
		if(world != observedRootWorld_) {
			omitted = true;
			return 0U;
		}
		if(rootTickEvidenceSize_ == 0U) return 0U;
		const std::size_t oldest = (rootTickEvidenceCursor_
			+ rootTickEvidenceRing_.size() - rootTickEvidenceSize_)
			% rootTickEvidenceRing_.size();
		const std::uint64_t oldestSequence = rootTickEvidenceRing_[oldest].resultSequence;
		omitted = afterSequence < oldestSequence - 1U;
		std::size_t copied = 0U;
		for(std::size_t offset = 0U; offset < rootTickEvidenceSize_ && copied < output.size(); ++offset) {
			const auto& value = rootTickEvidenceRing_[(oldest + offset)
				% rootTickEvidenceRing_.size()];
			if(value.resultSequence > afterSequence) output[copied++] = value;
		}
		return copied;
	}

	std::size_t AnimationRootMotionResultPublisher::RetainedRootMotionTickEvidenceCount() const noexcept
	{
		std::scoped_lock lock(mutex_);
		return rootTickEvidenceSize_;
	}

	static AnimationRootMotionApplicationReceipt RouteRootMotionApplicationImpl(
		const AnimationRootMotionApplicationRequest& request,
		ObjectManager& objects,
		TerraEngine::Scene::SceneMutationQueue& sceneMutations,
		TerraEngine::Physics::PhysicsSystem& physics,
		AnimationRootMotionSceneVisibility sceneVisibility,
		bool applicationReserved)
	{
		using namespace TerraEngine::Physics;
		using namespace TerraEngine::Scene;
		const AnimationRootMotionApplicationRequest resolved = ResolveWorldMotion(request);
		AnimationRootMotionApplicationReceipt receipt;
		bool shouldRoute = applicationReserved;
		if(applicationReserved) {
			receipt.status = AnimationStatus::Queued;
			receipt.animator = resolved.delta.animator;
			receipt.mode = resolved.mode;
			receipt.generation = resolved.delta.generation;
			receipt.producerId = resolved.producerId;
			receipt.producerSequence = resolved.producerSequence;
			receipt.correlationId = resolved.correlationId;
			receipt.idempotencyKey = resolved.idempotencyKey;
			receipt.publicationGeneration = resolved.publicationGeneration;
			receipt.observableTick = resolved.delta.sourceEndTick;
			receipt.duplicate = AnimationRootMotionResultPublisher::Instance()
				.ApplicationWasRetried(resolved);
		} else {
			receipt = AnimationRootMotionResultPublisher::Instance().ReserveApplication(
				resolved, shouldRoute);
		}
		if(!shouldRoute) return receipt;
		AnimationRootMotionApplicationResult result = MakeApplicationResult(resolved);
		result.duplicate = receipt.duplicate;
		auto publishTerminal = [&](const AnimationRootMotionApplicationResult& terminal) {
			// Terminal scalar state owns idempotency and bounded-eviction semantics.
			// Record it before retaining optional result history so OOM cannot strand
			// an accepted application as permanently Queued.
			AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(
				resolved, receipt);
			try {
				AnimationRootMotionResultPublisher::Instance().PublishImmediate(terminal);
			} catch(...) {
				// The copied receipt remains truthful when optional history is unavailable.
			}
		};
		auto reject = [&](AnimationStatus status) {
			result.status = status;
			result.disposition = AnimationRootMotionDisposition::Rejected;
			receipt.status = status;
			publishTerminal(result);
			return receipt;
		};

		if(resolved.mode == AnimationRootMotionApplicationMode::Ignore) {
			result.status = AnimationStatus::Ok;
			result.disposition = AnimationRootMotionDisposition::Ignored;
			receipt.status = result.status;
			publishTerminal(result);
			return receipt;
		}
		if(resolved.mode == AnimationRootMotionApplicationMode::ExtractOnly
			|| resolved.mode == AnimationRootMotionApplicationMode::DeterministicNetwork) {
			result.status = AnimationStatus::Ok;
			result.disposition = AnimationRootMotionDisposition::Extracted;
			receipt.status = result.status;
			publishTerminal(result);
			return receipt;
		}

		const auto bindings = AnimationSystem::Instance().ReadRootControllerBindings();
		const auto exclusions = AnimationSystem::Instance().ReadRagdollExclusions();
		const bool ragdollExcluded = exclusions && std::any_of(
			exclusions->entries.begin(), exclusions->entries.end(),
			[&resolved](const AnimationRagdollExclusion& entry) {
				return entry.animator == resolved.delta.animator;
			});
		if(ragdollExcluded) return reject(AnimationStatus::Rejected);
		const AnimationRootControllerBinding* bound = nullptr;
		if(bindings) {
			const auto found = std::find_if(bindings->active.begin(), bindings->active.end(),
				[&resolved](const AnimationRootControllerBinding& value) {
					return value.animator == resolved.delta.animator;
				});
			if(found != bindings->active.end()) bound = &*found;
		}
		if(bound && bound->locomotionOwned && !bound->rootMotionEnabled)
			return reject(AnimationStatus::Rejected);
		if(bound && bound->rootMotionEnabled) {
			if(!bound->locomotionOwned || bound->mode != CharacterMotionMode::RootMotion
				|| bound->animatorObjectId != resolved.ownerObjectId
				|| TerraEngine::CompressObjectGeneration(bound->animatorObjectGeneration)
					!= resolved.objectGeneration
				|| TerraEngine::CompressObjectGeneration(bound->animatorComponentGeneration)
					!= resolved.componentGeneration
				|| !CompatibleRootBasis(*bound)
				|| !resolved.sourceObjectWorldTransformCaptured
				|| resolved.delta.sourceGeneration == 0U
				|| !std::isfinite(resolved.delta.evaluationDurationSeconds)
				|| resolved.delta.evaluationDurationSeconds <= 0.0
				|| resolved.delta.evaluationDurationSeconds
					> static_cast<double>(std::numeric_limits<float>::max()))
				return reject(AnimationStatus::StaleHandle);
			const auto object = objects.getObject(resolved.ownerObjectId);
			AnimationTransform currentWorld;
			if(!object || !CaptureWorldTransform(objects, *object, currentWorld)
				|| !SameSourceTransform(resolved.sourceObjectWorldTransform, currentWorld))
				return reject(AnimationStatus::StaleHandle);
			const auto lease = physics.ReadCharacterMotionLease(bound->controller);
			if(!lease || lease->state != CharacterMotionLeaseState::LocomotionOwned
				|| lease->mode != CharacterMotionMode::RootMotion
				|| lease->owner != bound->owner
				|| lease->authorityEpoch != bound->authorityEpoch
				|| lease->motionEpoch != bound->motionEpoch)
				return reject(AnimationStatus::Rejected);
			RootMotionInterval interval;
			interval.lease = *lease;
			interval.frame = bound->frame;
			interval.animator.slotIndex = resolved.delta.animator.slotIndex;
			interval.animator.animatorGeneration = resolved.delta.animator.generation;
			interval.animator.poseGeneration = resolved.delta.generation;
			interval.animator.sourceGeneration = resolved.delta.sourceGeneration;
			interval.animator.sourceStateId = resolved.delta.sourceStateId;
			interval.animator.layerIndex = resolved.delta.layerIndex;
			interval.animator.evaluationSequence = resolved.publicationGeneration;
			interval.animator.bindingGeneration = bound->bindingGeneration;
			interval.animator.publicationGeneration = resolved.publicationGeneration;
			interval.animator.sourceBeginTick = resolved.delta.sourceBeginTick;
			interval.animator.sourceEndTick = resolved.delta.sourceEndTick;
			interval.animator.accumulatedCycles = resolved.delta.accumulatedCycles;
			interval.ledger = {resolved.producerId, resolved.producerSequence,
				resolved.idempotencyKey, resolved.correlationId};
			interval.localTranslation = {resolved.delta.localTranslation.x,
				resolved.delta.localTranslation.y, resolved.delta.localTranslation.z};
			interval.localYawRadians = resolved.delta.localYawRadians;
			// ResolveWorldMotion used the captured animator world basis exactly
			// once. The validation-only relative/rest basis is never applied again.
			interval.frameTranslation = {resolved.delta.worldTranslation.x,
				resolved.delta.worldTranslation.y, resolved.delta.worldTranslation.z};
			interval.frameYawRadians = resolved.delta.worldYawRadians;
			interval.durationSeconds = static_cast<float>(resolved.delta.evaluationDurationSeconds);
			interval.maximumSpeed = bound->maximumRootSpeed;
			interval.maximumTurnRate = bound->maximumYawRate;
			interval.validityGuard = bound->validityGuard;
			const RootMotionIntervalAdmission reserved = physics.ReserveRootMotionInterval(interval);
			if(!reserved.accepted) return reject(reserved.error == RootMotionIntervalError::CapacityExceeded
				? AnimationStatus::HardLimitExceeded : AnimationStatus::Rejected);
			struct ReservationGuard {
				PhysicsSystem& physics;
				RootMotionIntervalReservation reservation;
				bool active = true;
				~ReservationGuard() noexcept {
					if(active) try {
						(void)physics.CancelRootMotionIntervalReservation(reservation);
					} catch(...) {}
				}
			} reservationGuard{physics, reserved.reservation};
			receipt.rootIntervalId = reserved.reservation.intervalId;
			result.rootIntervalId = receipt.rootIntervalId;
			if(!AnimationRootMotionResultPublisher::Instance().RegisterRootMotionInterval(
				receipt.rootIntervalId, result)) {
				return reject(AnimationStatus::HardLimitExceeded);
			}
			const RootMotionIntervalAdmission committed =
				physics.CommitRootMotionInterval(reserved.reservation);
			if(!committed.accepted) {
				AnimationRootMotionResultPublisher::Instance()
					.CancelRootMotionIntervalApplication(receipt.rootIntervalId,
						AnimationStatus::Rejected);
				return reject(AnimationStatus::Rejected);
			}
			reservationGuard.active = false;
			receipt.status = AnimationStatus::Queued;
			AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(resolved, receipt);
			return receipt;
		}

		if(resolved.mode == AnimationRootMotionApplicationMode::ApplyToTransform) {
			const std::optional<Object> object = objects.getObject(resolved.ownerObjectId);
			if(!object
				|| TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*object)) != resolved.objectGeneration
				|| TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(*object)) != resolved.componentGeneration) {
				return reject(AnimationStatus::StaleHandle);
			}
			const std::optional<TerraEngine::Scene::WorldPose> currentPose = objects.GetCanonicalWorldPose(*object);
			if(!currentPose) return reject(AnimationStatus::InvalidType);
			AnimationTransform currentWorld;
			if(!CaptureWorldTransform(objects, *object, currentWorld)) {
				return reject(AnimationStatus::InvalidType);
			}
			if(resolved.sourceObjectWorldTransformCaptured
				&& !SameSourceTransform(resolved.sourceObjectWorldTransform, currentWorld)) {
				return reject(AnimationStatus::StaleHandle);
			}
			TerraEngine::Scene::WorldDelta worldDelta;
			worldDelta.offsetMetres = {
				static_cast<double>(resolved.delta.worldTranslation.x),
				static_cast<double>(resolved.delta.worldTranslation.y),
				static_cast<double>(resolved.delta.worldTranslation.z)};
			const TerraEngine::Scene::WorldCoordinateResult translated =
				TerraEngine::Scene::ApplyWorldDelta(currentPose->position, worldDelta);
			if(!translated.Succeeded()) {
				return reject(AnimationStatus::Rejected);
			}
			TerraEngine::Scene::WorldPose desiredPose = *currentPose;
			desiredPose.position = translated.position;
			desiredPose.rotation = glm::normalize(currentPose->rotation
				* ToGlm(YawRotation(resolved.delta.worldYawRadians)));
			receipt.sceneTransactionId = "animation-root-motion-"
				+ std::to_string(resolved.delta.animator.slotIndex) + "-"
				+ std::to_string(resolved.delta.animator.generation) + "-"
				+ std::to_string(resolved.producerId) + "-"
				+ std::to_string(resolved.idempotencyKey);
			TransactionalMutationBuilder builder(
				receipt.sceneTransactionId, TransactionOrigin::System, "AnimationRootMotion");
			builder.SetWorldTransform(objects, resolved.ownerObjectId, desiredPose);
			Transaction transaction = std::move(builder).Build();
			const TransactionResult preflight = sceneMutations.PreflightTransaction(transaction, objects);
			if(preflight.status != TransactionStatus::PreflightAccepted) return reject(AnimationStatus::Rejected);
			if(!AnimationRootMotionResultPublisher::Instance().RegisterSceneRequest(
				receipt.sceneTransactionId, result)) {
				return reject(AnimationStatus::HardLimitExceeded);
			}
			if(sceneVisibility == AnimationRootMotionSceneVisibility::ImmediateProtectedCommit) {
				// PublishAnimationAuthority is itself a declared protected transactional
				// boundary. A direct commit here supplies the same atomic rollback rules
				// while making authored root motion visible before Physics this frame.
				const TransactionResult committed = CommitTransaction(objects, transaction);
				AnimationRootMotionResultPublisher::Instance().ObserveSceneTransactionResult(committed);
				receipt.status = committed.status == TransactionStatus::Committed
					? AnimationStatus::Ok
					: (committed.status == TransactionStatus::Canceled
						? AnimationStatus::Cancelled : AnimationStatus::Rejected);
				AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(resolved, receipt);
				return receipt;
			}
			sceneMutations.EnqueueTransaction(std::move(transaction));
			receipt.status = AnimationStatus::Queued;
			AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(resolved, receipt);
			return receipt;
		}

		PhysicsCommandMetadata metadata;
		metadata.effectiveTick = static_cast<PhysicsTick>(resolved.delta.sourceEndTick);
		metadata.producerId = resolved.producerId;
		metadata.producerSequence = resolved.producerSequence;
		metadata.correlationId = "animation-root-motion-" + std::to_string(resolved.correlationId);
		metadata.idempotencyKey = "animation-root-motion-" + std::to_string(resolved.delta.animator.slotIndex)
			+ "-" + std::to_string(resolved.delta.animator.generation)
			+ "-" + std::to_string(resolved.producerId)
			+ "-" + std::to_string(resolved.idempotencyKey);
		metadata.requestFingerprint = metadata.idempotencyKey
			+ "-" + std::to_string(static_cast<std::uint32_t>(resolved.mode))
			+ "-" + std::to_string(resolved.delta.generation)
			+ "-" + std::to_string(resolved.publicationGeneration)
			+ "-" + std::to_string(resolved.delta.sourceBeginTick)
			+ "-" + std::to_string(resolved.delta.sourceEndTick)
			+ "-" + std::to_string(resolved.delta.worldTranslation.x)
			+ "-" + std::to_string(resolved.delta.worldTranslation.y)
			+ "-" + std::to_string(resolved.delta.worldTranslation.z)
			+ "-" + std::to_string(resolved.delta.worldYawRadians);
		metadata.cancellationToken = resolved.correlationId;
		PhysicsCommand command;
		AnimationFloat3 start{};
		AnimationFloat3 end{};
		AnimationQuaternion startRotation{};
		AnimationQuaternion endRotation{};
		bool character = resolved.mode == AnimationRootMotionApplicationMode::ApplyToCharacter;
		if(character) {
			const std::shared_ptr<const PhysicsCharacterStatePacket> states = physics.LatestCharacterStatePacket();
			if(!states || !states->states) return reject(AnimationStatus::NotReady);
			const auto state = std::find_if(states->states->begin(), states->states->end(), [&resolved](const PhysicsCharacterState& value) {
				return value.objectId == resolved.ownerObjectId
					&& value.objectGeneration == resolved.objectGeneration
					&& value.componentGeneration == resolved.componentGeneration;
			});
			if(state == states->states->end()) return reject(AnimationStatus::StaleHandle);
			start = {state->pose.position.x, state->pose.position.y, state->pose.position.z};
			end = {start.x + resolved.delta.worldTranslation.x,
				start.y + resolved.delta.worldTranslation.y,
				start.z + resolved.delta.worldTranslation.z};
			startRotation = FromPhysicsRotation(state->pose.rotation);
			endRotation = FromPhysicsRotation(ComposeYaw(
				state->pose.rotation, resolved.delta.worldYawRadians));
			// Velocity-driven movement lets Jolt coordinate collision, support motion,
			// and step handling. The published acknowledgement reports any adjusted
			// displacement or unapplied yaw rather than pretending a teleport occurred.
			const float inverseDuration = 1.0F / resolved.fixedDurationSeconds;
			command = PhysicsCommand::MakeCharacterMove(
				{states->world, state->handle, resolved.ownerObjectId,
					resolved.objectGeneration, resolved.componentGeneration},
				{resolved.delta.worldTranslation.x * inverseDuration,
					resolved.delta.worldTranslation.y * inverseDuration,
					resolved.delta.worldTranslation.z * inverseDuration}, metadata);
		} else if(resolved.mode == AnimationRootMotionApplicationMode::ApplyToPhysicsBody) {
			const std::shared_ptr<const IncrementalBodyStateSnapshot> states = physics.LatestIncrementalBodySnapshot();
			if(!states) return reject(AnimationStatus::NotReady);
			const std::vector<PhysicsBodyState> flat = states->Flatten();
			const auto state = std::find_if(flat.begin(), flat.end(), [&resolved](const PhysicsBodyState& value) {
				return value.objectId == resolved.ownerObjectId
					&& value.objectGeneration == resolved.objectGeneration
					&& value.componentGeneration == resolved.componentGeneration;
			});
			if(state == flat.end()) return reject(AnimationStatus::StaleHandle);
			start = {state->worldPose.position.x, state->worldPose.position.y, state->worldPose.position.z};
			end = {start.x + resolved.delta.worldTranslation.x,
				start.y + resolved.delta.worldTranslation.y,
				start.z + resolved.delta.worldTranslation.z};
			PhysicsPose pose = state->worldPose;
			pose.position = {end.x, end.y, end.z};
			startRotation = FromPhysicsRotation(pose.rotation);
			pose.rotation = ComposeYaw(pose.rotation, resolved.delta.worldYawRadians);
			endRotation = FromPhysicsRotation(pose.rotation);
			command = PhysicsCommand::MakeKinematicTarget(
				{states->world, resolved.ownerObjectId, resolved.objectGeneration, resolved.componentGeneration},
				pose, resolved.fixedDurationSeconds, metadata);
		} else {
			return reject(AnimationStatus::InvalidArgument);
		}

		const PhysicsCommandReceipt physicsReceipt = physics.SubmitCommand(std::move(command));
		if(!physicsReceipt.accepted || physicsReceipt.commandId == 0U) {
			if(physicsReceipt.duplicateIdempotency && physicsReceipt.commandId != 0U) {
				receipt.status = AnimationStatus::Queued;
				receipt.duplicate = true;
				receipt.physicsCommandId = physicsReceipt.commandId;
				AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(resolved, receipt);
				return receipt;
			}
			result.status = physicsReceipt.cancelled ? AnimationStatus::Cancelled : AnimationStatus::Rejected;
			result.disposition = AnimationRootMotionDisposition::Rejected;
			receipt.status = result.status;
			receipt.idempotencyConflict = physicsReceipt.idempotencyConflict;
			publishTerminal(result);
			return receipt;
		}
		receipt.physicsCommandId = physicsReceipt.commandId;
		result.physicsCommandId = physicsReceipt.commandId;
		if(!AnimationRootMotionResultPublisher::Instance().RegisterPhysicsRequest(
			physicsReceipt.commandId, result, resolved.ownerObjectId, resolved.objectGeneration,
			resolved.componentGeneration, start, end, startRotation, endRotation, character)) {
			(void)physics.CancelCommand(physicsReceipt.commandId);
			return reject(AnimationStatus::HardLimitExceeded);
		}
		receipt.status = AnimationStatus::Queued;
		AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(resolved, receipt);
		return receipt;
	}

	AnimationRootMotionApplicationReceipt RouteRootMotionApplication(
		const AnimationRootMotionApplicationRequest& request,
		ObjectManager& objects,
		TerraEngine::Scene::SceneMutationQueue& sceneMutations,
		TerraEngine::Physics::PhysicsSystem& physics,
		AnimationRootMotionSceneVisibility sceneVisibility,
		bool applicationReserved)
	{
		try {
			return RouteRootMotionApplicationImpl(
				request, objects, sceneMutations, physics, sceneVisibility, applicationReserved);
		} catch(...) {
			// This function runs at protected native/managed boundaries. No allocation
			// or provider exception may escape after pose authority has been published.
			const std::uint64_t abandonedPhysicsCommand =
				AnimationRootMotionResultPublisher::Instance().AbandonPendingApplication(request);
			if(abandonedPhysicsCommand != 0U) {
				try {
					(void)physics.CancelCommand(abandonedPhysicsCommand);
				} catch(...) {
					// Exception containment still terminalizes the animation receipt below.
				}
			}
			AnimationRootMotionApplicationReceipt failed;
			failed.status = AnimationStatus::InternalError;
			failed.animator = request.delta.animator;
			failed.mode = request.mode;
			failed.generation = request.delta.generation;
			failed.producerId = request.producerId;
			failed.producerSequence = request.producerSequence;
			failed.correlationId = request.correlationId;
			failed.idempotencyKey = request.idempotencyKey;
			failed.publicationGeneration = request.publicationGeneration;
			failed.observableTick = request.delta.sourceEndTick;
			AnimationRootMotionApplicationResult terminal = MakeApplicationResult(request);
			terminal.status = AnimationStatus::InternalError;
			terminal.disposition = AnimationRootMotionDisposition::Rejected;
			try {
				AnimationRootMotionResultPublisher::Instance().UpdateApplicationReceipt(
					request, failed);
			} catch(...) {
				// The function still returns a copied terminal receipt if mutex/provider
				// teardown prevents updating retained bookkeeping.
			}
			try {
				AnimationRootMotionResultPublisher::Instance().PublishImmediate(std::move(terminal));
			} catch(...) {
				// Optional diagnostic history never keeps the accepted record Queued.
			}
			return failed;
		}
	}
}
