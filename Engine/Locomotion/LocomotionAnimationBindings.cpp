// LocomotionAnimationBindings.cpp - protected binding capture and copied parameters.
#include "LocomotionAnimationBindings.h"
#include "Animation/AnimationSystem.h"
#include "Animation/Runtime/AnimationAuthorityPublication.h"
#include "Components/AnimatorComponent.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Resource/ResourceManager.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <glm/gtc/quaternion.hpp>

namespace TerraEngine::Locomotion {
	MotionParameterMapping ResolveMotionParameters(Animation::AnimatorInstanceHandle animator,
		std::uint64_t controllerGeneration, const MotionParameterNames& names,
		std::span<const Animation::AnimationControllerParameter> parameters) {
		MotionParameterMapping result;
		result.animator = animator;
		result.controllerGeneration = controllerGeneration;
		std::array<std::uint32_t, kMotionParameterCount> candidate{};
		for(std::size_t index = 0U; index < names.size(); ++index) {
			if(names[index].empty()) continue;
			const auto field = static_cast<MotionParameterField>(index);
			auto reject = [&](MotionParameterError error) {
				result.error = error;
				result.failedField = field;
				return result; // IDs stay empty until the complete mapping succeeds.
			};
			for(std::size_t prior = 0U; prior < index; ++prior)
				if(names[prior] == names[index]) return reject(MotionParameterError::Duplicate);
			const auto found = std::find_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
				return parameter.name == names[index];
			});
			if(found == parameters.end()) return reject(MotionParameterError::Missing);
			if(std::count_if(parameters.begin(), parameters.end(), [&](const auto& parameter) {
				return parameter.name == names[index];
			}) != 1) return reject(MotionParameterError::Duplicate);
			const auto expected = index < 3U ? Animation::AnimationControllerParameterType::Float
				: Animation::AnimationControllerParameterType::Boolean;
			if(found->type != expected) return reject(MotionParameterError::WrongType);
			if(found->stableId == 0U || found->stableId > std::numeric_limits<std::uint32_t>::max())
				return reject(MotionParameterError::UnsupportedId);
			candidate[index] = static_cast<std::uint32_t>(found->stableId);
			for(std::size_t prior = 0U; prior < index; ++prior)
				if(candidate[prior] == candidate[index]) return reject(MotionParameterError::Duplicate);
		}
		result.ids = candidate;
		return result;
	}
	PreparedMotionParameters PrepareMotionParameters(const MotionParameterMapping& mapping,
		const LocomotionObservation& observation, std::uint32_t producerId, std::uint64_t sequence) noexcept {
		PreparedMotionParameters result;
		result.physicalTick = observation.physicalTick;
		result.error = mapping.error;
		result.failedField = mapping.failedField;
		if(mapping.error != MotionParameterError::None) return result;
		const auto finiteFloat = [](double value) {
			return std::isfinite(value) && std::abs(value) <= std::numeric_limits<float>::max();
		};
		if(!mapping.animator.IsValid() || sequence == 0U
			|| sequence > std::numeric_limits<std::uint64_t>::max() - kMotionParameterCount
			|| !finiteFloat(observation.observedPlanarSpeed) || observation.observedPlanarSpeed < 0.0
			|| !finiteFloat(observation.observedLocalDirection.x)
			|| !finiteFloat(observation.observedLocalDirection.y)
			|| !finiteFloat(observation.observedLocalDirection.z)) {
			result.error = MotionParameterError::InvalidObservation;
			return result;
		}
		// Discontinuities have no derivative history. A grounded sample remains
		// meaningful, but speed/direction/moving must not extrapolate the last tick.
		const bool history = observation.motionHistoryAvailable
			&& std::isfinite(observation.observationDurationSeconds)
			&& observation.observationDurationSeconds > 0.0;
		const std::array<float, 3U> scalars{
			history ? static_cast<float>(observation.observedPlanarSpeed) : 0.0F,
			history ? static_cast<float>(observation.observedLocalDirection.x) : 0.0F,
			history ? static_cast<float>(observation.observedLocalDirection.z) : 0.0F};
		for(std::size_t index = 0U; index < mapping.ids.size(); ++index) {
			if(mapping.ids[index] == 0U) continue;
			auto& command = result.commands[result.count++];
			command.target = mapping.animator;
			command.parameterId = mapping.ids[index];
			command.producerId = producerId;
			command.producerSequence = sequence + index;
			command.correlationId = sequence + index;
			// targetTick stays zero: Physics and Animation have distinct timelines.
			if(index < scalars.size()) {
				command.parameterType = Animation::AnimationParameterType::Float;
				command.value = scalars[index];
			} else {
				command.parameterType = Animation::AnimationParameterType::Boolean;
				command.value = index == 3U ? observation.grounded : history && scalars[0] > 0.001F;
			}
		}
		return result;
	}

	namespace {
		[[nodiscard]] bool CurrentAsset(Animation::AnimationAssetHandle handle, AssetType type) {
			if(!handle.IsValid()) return false;
			auto& resources = ResourceManager::Instance();
			const auto generation = resources.TryGetAnimationAssetRuntimeGeneration(handle.assetId.value, type);
			return generation && *generation == handle.generation && resources.IsAnimationAssetPublished(handle.assetId.value, type);
		}
		[[nodiscard]] bool IsDescendant(ObjectManager& objects, const Object& source, const Object& owner) {
			auto parent = objects.getParent(source);
			// ObjectManager prevents cycles. The explicit bound also makes corrupted
			// hierarchy input fail closed instead of hanging a protected boundary.
			for(std::size_t depth = 0U; parent && depth < 65536U; ++depth) {
				if(parent->EntityId() == owner.EntityId()) return true;
				parent = objects.getParent(*parent);
			}
			return false;
		}
		[[nodiscard]] bool Upright(const glm::dquat& q) {
			const auto up = q * glm::dvec3(0.0, 1.0, 0.0);
			return std::abs(up.x) <= 0.001 && std::abs(up.y - 1.0) <= 0.001 && std::abs(up.z) <= 0.001;
		}
		[[nodiscard]] bool RelativeBasis(ObjectManager& objects, const Object& owner, const Object& source,
			Animation::AnimationQuaternion& rotation, Animation::AnimationFloat3& scale,
			Animation::AnimationFloat3& translation) {
			const auto ownerPose = objects.GetCanonicalWorldPose(owner);
			const auto sourcePose = objects.GetCanonicalWorldPose(source);
			if(!ownerPose || !sourcePose) return false;
			const auto make = [](auto value) { return glm::dquat(value.w, value.x, value.y, value.z); };
			auto controller = make(ownerPose->rotation);
			auto animator = make(sourcePose->rotation);
			const double controllerNorm = glm::dot(controller, controller);
			const double animatorNorm = glm::dot(animator, animator);
			if(!std::isfinite(controllerNorm) || !std::isfinite(animatorNorm)
				|| controllerNorm < 1.0e-12 || animatorNorm < 1.0e-12) return false;
			controller = glm::normalize(controller);
			animator = glm::normalize(animator);
			if(!Upright(controller) || !Upright(animator)) return false;
			const auto a = sourcePose->scale;
			const auto b = ownerPose->scale;
			if(!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(a.z)
				|| !std::isfinite(b.x) || !std::isfinite(b.y) || !std::isfinite(b.z)
				|| a.x <= 0.0 || a.y <= 0.0 || a.z <= 0.0
				|| std::abs(b.x - 1.0) > 0.001 || std::abs(b.y - 1.0) > 0.001 || std::abs(b.z - 1.0) > 0.001)
				return false;
			const auto relative = glm::inverse(controller) * animator;
			// Recovery aligns an animator origin, which may be offset from its
			// owning controller. Localize exact world cells before rotating the
			// bounded displacement; never flatten a distant address into a float.
			const auto offset = Scene::LocalizeWorldPosition(sourcePose->position, ownerPose->position,
				Scene::kTransformSpaceMaximumRelativeRadiusMetres);
			if(!offset.Succeeded()) return false;
			const auto localOffset = glm::inverse(controller) * offset.offsetMetres;
			translation = {static_cast<float>(localOffset.x), static_cast<float>(localOffset.y),
				static_cast<float>(localOffset.z)};
			rotation = {static_cast<float>(relative.x), static_cast<float>(relative.y),
				static_cast<float>(relative.z), static_cast<float>(relative.w)};
			scale = {static_cast<float>(a.x / b.x), static_cast<float>(a.y / b.y), static_cast<float>(a.z / b.z)};
			return Animation::IsFinite(scale);
		}
		// Called only by protected producers/Physics ingress. In particular this
		// never takes Animation's lifecycle mutex while a root route holds it.
		class AnimatorValidity final : public Physics::CharacterMotionValidity {
		public:
			LocomotionHandle owner;
			CapturedLocomotionAnimator captured;
			std::uint32_t bindingKind = 0U;
			double speed = 0.0;
			double turnRate = 0.0;
			bool rejectRagdoll = true;
			[[nodiscard]] Physics::CharacterMotionValidityReason Validate() const noexcept override {
				using Reason = Physics::CharacterMotionValidityReason;
				try {
					const auto exclusions = Animation::AnimationSystem::Instance().ReadRagdollExclusions();
					if(rejectRagdoll && exclusions && std::any_of(exclusions->entries.begin(), exclusions->entries.end(),
						[this](const auto& entry) { return entry.animator == captured.animator; })) return Reason::StaleEvidence;
					auto& objects = ObjectManager::instance();
					const auto controller = objects.getObject(owner.objectId);
					const auto source = objects.getObject(captured.objectId);
					if(!controller || !source || objects.GetSceneGeneration() != owner.sceneLifetime
						|| objects.getObjectLifetimeGeneration(*controller) != owner.objectGeneration
						|| objects.getObjectComponentGeneration(*controller) != owner.attachmentGeneration
						|| objects.getObjectLifetimeGeneration(*source) != captured.objectGeneration
						|| objects.getObjectComponentGeneration(*source) != captured.componentGeneration
						|| !objects.isObjectActiveInHierarchy(*controller) || !objects.isObjectActiveInHierarchy(*source))
						return Reason::StaleLifetime;
					const auto* authored = objects.GetComponent<CharacterLocomotionComponent>(*controller);
					const auto* animator = objects.GetComponent<AnimatorComponent>(*source);
					if(!authored || !authored->active || !animator || !animator->active
						|| animator->controllerAssetId != captured.authoredControllerId || animator->defaultClipAssetId != captured.authoredClipId
						|| authored->animatorBinding != bindingKind || authored->maximumSpeed != speed || authored->turnRate != turnRate
						|| (bindingKind == CharacterLocomotionComponent::kDescendantBinding
							&& (authored->animatorObjectId != captured.objectId || !IsDescendant(objects, *source, *controller))))
						return Reason::StaleConfiguration;
					const auto frame = Animation::AnimationSystem::Instance().LoadPublishedAuthorityFrame();
					const auto* row = frame ? frame->Find(captured.animator) : nullptr;
					if(!row || !row->active || row->safeFallback || row->clip != captured.clip || row->controller != captured.controller)
						return Reason::StaleEvidence;
					if(!CurrentAsset(captured.clip, AssetType::AnimationClip)
						|| (captured.controller.IsValid() && !CurrentAsset(captured.controller, AssetType::AnimationController)))
						return Reason::StaleEvidence;
					Animation::AnimationQuaternion rotation;
					Animation::AnimationFloat3 scale;
					Animation::AnimationFloat3 translation;
					if(!RelativeBasis(objects, *controller, *source, rotation, scale, translation)) return Reason::StaleConfiguration;
					const auto& expected = captured.relativeRotation;
					const double dot = std::abs(double(rotation.x) * expected.x + double(rotation.y) * expected.y
						+ double(rotation.z) * expected.z + double(rotation.w) * expected.w);
					if(dot < 1.0 - 1.0e-5 || std::abs(scale.x - captured.relativeScale.x) > 0.001F
						|| std::abs(scale.y - captured.relativeScale.y) > 0.001F || std::abs(scale.z - captured.relativeScale.z) > 0.001F
						|| std::abs(translation.x - captured.relativeTranslation.x) > 0.001F
						|| std::abs(translation.y - captured.relativeTranslation.y) > 0.001F
						|| std::abs(translation.z - captured.relativeTranslation.z) > 0.001F)
						return Reason::StaleConfiguration;
					return Reason::Valid;
				} catch(...) { return Reason::Unavailable; }
			}
		};
	}

	CapturedLocomotionAnimator CaptureLocomotionAnimator(LocomotionHandle owner,
		const CharacterLocomotionComponent& authored, const CapturedLocomotionAnimator* previous) {
		CapturedLocomotionAnimator result;
		if(authored.animatorBinding == CharacterLocomotionComponent::kNoneBinding) {
			result.reason = LocomotionReason::None;
			return result;
		}
		result.reason = LocomotionReason::MissingBinding;
		auto& objects = ObjectManager::instance();
		const auto controllerObject = objects.getObject(owner.objectId);
		const auto sourceObject = objects.getObject(authored.animatorBinding == CharacterLocomotionComponent::kSelfBinding
			? owner.objectId : authored.animatorObjectId);
		if(!controllerObject || !sourceObject || objects.GetSceneGeneration() != owner.sceneLifetime
			|| objects.getObjectLifetimeGeneration(*controllerObject) != owner.objectGeneration
			|| objects.getObjectComponentGeneration(*controllerObject) != owner.attachmentGeneration
			|| !objects.isObjectActiveInHierarchy(*sourceObject)
			|| (authored.animatorBinding == CharacterLocomotionComponent::kDescendantBinding
				&& !IsDescendant(objects, *sourceObject, *controllerObject))) return result;
		const auto* component = objects.GetComponent<AnimatorComponent>(*sourceObject);
		if(!component || !component->active) return result;
		result.reason = LocomotionReason::AnimationUnavailable;
		auto& animation = Animation::AnimationSystem::Instance();
		const auto animator = animation.ResolveAnimatorForObject(objects.getUUID(*sourceObject));
		const auto frame = animation.LoadPublishedAuthorityFrame();
		const auto* row = frame ? frame->Find(animator) : nullptr;
		if(!row || !row->active || row->safeFallback) return result;
		// A resource can reload before Animation publishes a replacement row. Do
		// not reuse old mapping IDs or root capability during that one-frame gap.
		if((row->clip.IsValid() && !CurrentAsset(row->clip, AssetType::AnimationClip))
			|| (row->controller.IsValid() && !CurrentAsset(row->controller, AssetType::AnimationController))) return result;
		result.animator = animator;
		result.authoredControllerId = component->controllerAssetId;
		result.authoredClipId = component->defaultClipAssetId;
		result.objectId = objects.getUUID(*sourceObject);
		result.objectGeneration = objects.getObjectLifetimeGeneration(*sourceObject);
		result.componentGeneration = objects.getObjectComponentGeneration(*sourceObject);
		result.controller = row->controller;
		result.clip = row->clip;
		result.names = {authored.speedParameter, authored.localDirectionXParameter, authored.localDirectionZParameter,
			authored.groundedParameter, authored.movingParameter};
		const bool sameMapping = previous && previous->animator == result.animator
			&& previous->controller == result.controller && previous->names == result.names;
		if(sameMapping) result.mapping = previous->mapping;
		else {
			Animation::AnimationControllerAsset asset;
			Animation::AnimationGeneration generation = 0U;
			const bool anyNames = std::any_of(result.names.begin(), result.names.end(), [](const auto& name) { return !name.empty(); });
			if(!anyNames || (row->controller.IsValid() && ResourceManager::Instance().CopyAnimationControllerPublication(
				row->controller.assetId.value, asset, generation) && generation == row->controller.generation))
				result.mapping = ResolveMotionParameters(animator, row->controller.generation, result.names, asset.parameters);
			else {
				result.mapping.animator = animator;
				result.mapping.error = MotionParameterError::Missing;
			}
		}
		if(previous && previous->clip == result.clip) result.clipHasRootMotion = previous->clipHasRootMotion;
		else if(row->clip.IsValid()) {
			Animation::AnimationClipAsset clip;
			Animation::AnimationGeneration generation = 0U;
			result.clipHasRootMotion = ResourceManager::Instance().CopyAnimationClipPublication(row->clip.assetId.value, clip, generation)
				&& generation == row->clip.generation && clip.rootMotion.enabled;
		}
		const bool basisValid = RelativeBasis(objects, *controllerObject, *sourceObject,
			result.relativeRotation, result.relativeScale, result.relativeTranslation);
		result.rootReady = result.clipHasRootMotion && basisValid
			&& row->requestedExecutionPolicy != Animation::AnimationExecutionPolicy::ForceGpuVisual;
		result.reason = result.mapping.error == MotionParameterError::None ? LocomotionReason::None : LocomotionReason::AnimationUnavailable;
		if(basisValid) {
			auto guard = std::make_shared<AnimatorValidity>();
			guard->owner = owner;
			guard->captured = result;
			guard->bindingKind = authored.animatorBinding;
			guard->speed = authored.maximumSpeed;
			guard->turnRate = authored.turnRate;
			guard->rejectRagdoll = false;
			if(result.rootReady) {
				auto motionGuard = std::make_shared<AnimatorValidity>(*guard);
				motionGuard->rejectRagdoll = true;
				result.validity = std::move(motionGuard);
			}
			result.bindingValidity = std::move(guard);
		}
		return result;
	}
}
