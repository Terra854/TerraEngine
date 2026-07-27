// RootMotion.cpp - Loop-aware root transforms without live scene mutation.

#include "Animation/Runtime/RootMotion.h"

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
				&& std::isfinite(delta.localYawRadians) && std::isfinite(delta.worldYawRadians);
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
			AnimationTransform world{};
			bool captured = false;
			std::optional<Object> current = object;
			std::size_t depth = 0U;
			while(current && depth++ < 4096U) {
				const Transform* transform = objects.GetComponent<Transform>(*current);
				if(transform == nullptr) return false;
				AnimationTransform local;
				local.translation = FromGlm(transform->position);
				local.rotation = FromGlm(transform->rotation);
				local.scale = FromGlm(transform->scale);
				if(!IsFinite(local) || !IsNormalized(local.rotation)) return false;
				world = captured ? CombineTransform(local, world) : local;
				captured = true;
				current = objects.getParent(*current);
			}
			if(current.has_value() || !captured) return false;
			output = world;
			return true;
		}

		bool SameSourceTransform(
			const AnimationTransform& expected,
			const AnimationTransform& actual) noexcept
		{
			return NearlyEqual(expected.translation, actual.translation)
				&& NearlyEquivalent(expected.rotation, actual.rotation)
				&& NearlyEqual(expected.scale, actual.scale);
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
			|| !std::isfinite(request.accumulatedCycleYawRadians)) {
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
			|| current.layerIndex != delta.layerIndex) {
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
	AnimationRootMotionResultPublisher::AnimationRootMotionResultPublisher() = default;
	AnimationRootMotionResultPublisher::~AnimationRootMotionResultPublisher() = default;

	namespace {
		constexpr std::size_t kRetainedRootMotionResults = 4096U;
		constexpr std::size_t kRetainedRootMotionApplications = 4096U;

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
				// Allocate before RetainResultLocked can evict bounded history. This keeps
				// an allocation failure from removing an unrelated terminal record.
				results_.reserve(results_.size() + 1U);
				RetainResultLocked(std::move(value));
				return true;
			} catch(...) {
				receipt.status = AnimationStatus::InternalError;
				return false;
			}
		};
		const auto exact = std::find_if(applications_.begin(), applications_.end(), [&request](const ApplicationRecord& value) {
			return value.request.delta.animator == request.delta.animator
				&& value.request.producerId == request.producerId
				&& value.request.idempotencyKey == request.idempotencyKey;
		});
		if(exact != applications_.end()) {
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
		const auto evictableApplication = !requiresApplicationEviction
			? applications_.end()
			: std::find_if(applications_.begin(), applications_.end(), [](const ApplicationRecord& value) {
				// Queued records own a pending protected-boundary acknowledgement and
				// can never be evicted. Any other status is a copied terminal receipt.
				return value.receipt.status != AnimationStatus::Queued;
			});
		if(requiresApplicationEviction
			&& evictableApplication == applications_.end()) {
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
			if(applications_.size() < kRetainedRootMotionApplications) {
				applications_.reserve(applications_.size() + 1U);
			}
			if(results_.size() < kRetainedRootMotionResults) {
				results_.reserve(results_.size() + 1U);
			}
			const bool addAppliedMutationSlot = IsMutatingMode(request.mode)
				&& appliedMutation == appliedMutations_.end();
			if(addAppliedMutationSlot) {
				if(appliedMutations_.size() >= kRetainedRootMotionApplications) {
					receipt.status = AnimationStatus::HardLimitExceeded;
					return receipt;
				}
				appliedMutations_.reserve(appliedMutations_.size() + 1U);
			}
			// Only a full vector captures an iterator for eviction. When capacity is
			// available, reserve() may invalidate the old end iterator, so the explicit
			// pre-reserve decision prevents comparing or erasing through that stale value.
			if(requiresApplicationEviction) {
				applications_.erase(evictableApplication);
			}
			receipt.status = AnimationStatus::Queued;
			applications_.push_back({request, receipt});
			deferred.status = AnimationStatus::Queued;
			deferred.disposition = AnimationRootMotionDisposition::Deferred;
			try {
				RetainResultLocked(std::move(deferred));
			} catch(...) {
				applications_.pop_back();
				throw;
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
		const auto found = std::find_if(applications_.begin(), applications_.end(), [&request](const ApplicationRecord& value) {
			return value.request.delta.animator == request.delta.animator
				&& value.request.producerId == request.producerId
				&& value.request.idempotencyKey == request.idempotencyKey;
		});
		return found != applications_.end() && found->receipt.duplicate;
	}

	void AnimationRootMotionResultPublisher::UpdateApplicationReceipt(
		const AnimationRootMotionApplicationRequest& request,
		const AnimationRootMotionApplicationReceipt& receipt)
	{
		std::scoped_lock lock(mutex_);
		const auto found = std::find_if(applications_.begin(), applications_.end(), [&request](const ApplicationRecord& value) {
			return value.request.delta.animator == request.delta.animator
				&& value.request.producerId == request.producerId
				&& value.request.idempotencyKey == request.idempotencyKey;
		});
		if(found == applications_.end()) return;
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
		return commandId;
	}

	void AnimationRootMotionResultPublisher::RetainResultLocked(
		AnimationRootMotionApplicationResult result)
	{
		const auto duplicate = std::find_if(results_.begin(), results_.end(), [&result](const auto& value) {
			return value.animator == result.animator
				&& value.producerId == result.producerId
				&& value.idempotencyKey == result.idempotencyKey
				&& value.correlationId == result.correlationId
				&& value.idempotencyConflict == result.idempotencyConflict;
		});
		if(duplicate != results_.end()) {
			*duplicate = std::move(result);
			return;
		}
		if(results_.size() >= kRetainedRootMotionResults) results_.erase(results_.begin());
		results_.push_back(std::move(result));
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
		const auto application = std::find_if(applications_.begin(), applications_.end(), [&result](const ApplicationRecord& value) {
			return value.request.delta.animator == result.animator
				&& value.request.producerId == result.producerId
				&& value.request.idempotencyKey == result.idempotencyKey;
		});
		if(application != applications_.end()) {
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
		const auto application = std::find_if(applications_.begin(), applications_.end(), [&result](const ApplicationRecord& value) {
			return value.request.delta.animator == result.animator
				&& value.request.producerId == result.producerId
				&& value.request.idempotencyKey == result.idempotencyKey;
		});
		if(application != applications_.end()) {
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
			if(results_.size() < kRetainedRootMotionResults) {
				results_.reserve(results_.size() + 1U);
			}
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
			const auto application = std::find_if(applications_.begin(), applications_.end(), [&result](const ApplicationRecord& value) {
				return value.request.delta.animator == result.animator
					&& value.request.producerId == result.producerId
					&& value.request.idempotencyKey == result.idempotencyKey;
			});
			if(application != applications_.end()) {
				application->receipt.status = result.status;
				result.duplicate = result.duplicate || application->receipt.duplicate;
				if(result.disposition == AnimationRootMotionDisposition::Applied
					|| result.disposition == AnimationRootMotionDisposition::Adjusted) {
					RememberAppliedMutationLocked(*application);
				}
			}
			try {
				if(results_.size() < kRetainedRootMotionResults) {
					results_.reserve(results_.size() + 1U);
				}
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
		std::erase_if(applications_, [animator](const ApplicationRecord& value) {
			return value.request.delta.animator == animator;
		});
		std::erase_if(appliedMutations_, [animator](const AppliedMutationRecord& value) {
			return value.animator == animator;
		});
		std::erase_if(results_, [animator](const AnimationRootMotionApplicationResult& value) {
			return value.animator == animator;
		});
	}

	void AnimationRootMotionResultPublisher::CopyForAnimator(
		AnimatorInstanceHandle animator,
		std::vector<AnimationRootMotionApplicationResult>& output) const
	{
		std::vector<AnimationRootMotionApplicationResult> staged;
		std::scoped_lock lock(mutex_);
		try {
			for(const AnimationRootMotionApplicationResult& result : results_) {
				if(result.animator == animator) staged.push_back(result);
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
		applications_.clear();
		appliedMutations_.clear();
		results_.clear();
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

		if(resolved.mode == AnimationRootMotionApplicationMode::ApplyToTransform) {
			const std::optional<Object> object = objects.getObject(resolved.ownerObjectId);
			if(!object
				|| TerraEngine::CompressObjectGeneration(objects.getObjectLifetimeGeneration(*object)) != resolved.objectGeneration
				|| TerraEngine::CompressObjectGeneration(objects.getObjectComponentGeneration(*object)) != resolved.componentGeneration) {
				return reject(AnimationStatus::StaleHandle);
			}
			const Transform* transform = objects.GetComponent<Transform>(*object);
			if(transform == nullptr) return reject(AnimationStatus::InvalidType);
			AnimationTransform currentWorld;
			if(!CaptureWorldTransform(objects, *object, currentWorld)) {
				return reject(AnimationStatus::InvalidType);
			}
			if(resolved.sourceObjectWorldTransformCaptured
				&& !SameSourceTransform(resolved.sourceObjectWorldTransform, currentWorld)) {
				return reject(AnimationStatus::StaleHandle);
			}
			AnimationTransform desiredWorld = currentWorld;
			desiredWorld.translation = FromGlm(
				ToGlm(currentWorld.translation) + ToGlm(resolved.delta.worldTranslation));
			desiredWorld.rotation = FromGlm(ToGlm(currentWorld.rotation)
				* ToGlm(YawRotation(resolved.delta.worldYawRadians)));
			glm::vec3 localPosition;
			glm::quat localRotation;
			if(!ConvertWorldTargetToLocal(
				objects, *object, desiredWorld, localPosition, localRotation)) {
				return reject(AnimationStatus::Rejected);
			}
			TransactionTransformUpdate update;
			update.objectId = resolved.ownerObjectId;
			update.position = localPosition;
			update.rotation = localRotation;
			receipt.sceneTransactionId = "animation-root-motion-"
				+ std::to_string(resolved.delta.animator.slotIndex) + "-"
				+ std::to_string(resolved.delta.animator.generation) + "-"
				+ std::to_string(resolved.producerId) + "-"
				+ std::to_string(resolved.idempotencyKey);
			TransactionalMutationBuilder builder(
				receipt.sceneTransactionId, TransactionOrigin::System, "AnimationRootMotion");
			builder.UpdateTransformBatch(objects, {update});
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
