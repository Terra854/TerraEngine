// Navigation remains the route owner. This adapter only copies and revalidates
// its current advisory alongside the physical geometry that can safely use it.
#include "LocomotionNavigationBindings.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/NavigationAgentComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include <cmath>

namespace TerraEngine::Locomotion {
	namespace {
		struct CapturedNavigation {
			Navigation::NavigationAgentGuidanceSnapshot guidance;
			Navigation::NavigationAgentAuthoredSettings settings;
			NavigationCompatibilityInput geometry;
			std::uint64_t transformRevision = 0U;
			std::uint64_t destinationTransformRevision = 0U;
		};

		// This function is never invoked by a preparation worker or backend. All
		// object access is limited to scheduler-owned capture/commit boundaries.
		[[nodiscard]] std::optional<CapturedNavigation> ReadCurrent(LocomotionHandle handle) {
			auto& objects = ObjectManager::instance();
			const auto object = objects.getObject(handle.objectId);
			if(!object || objects.GetSceneGeneration() != handle.sceneLifetime
				|| objects.getObjectLifetimeGeneration(*object) != handle.objectGeneration
				|| objects.getObjectComponentGeneration(*object) != handle.attachmentGeneration
				|| !objects.isObjectActiveInHierarchy(*object)) return std::nullopt;
			const auto* locomotion = objects.GetComponent<CharacterLocomotionComponent>(*object);
			const auto* agent = objects.GetComponent<NavigationAgentComponent>(*object);
			const auto* controller = objects.GetComponent<CharacterControllerComponent>(*object);
			const auto pose = objects.GetCanonicalWorldPose(*object);
			if(!locomotion || !locomotion->active || locomotion->navigationBinding != CharacterLocomotionComponent::kSelfBinding
				|| !agent || !agent->active || !controller || !pose) return std::nullopt;
			auto& navigation = Navigation::NavigationAgentSystem::Instance();
			const auto identity = navigation.ResolvePublishedAgent(handle.objectId);
			if(!identity) return std::nullopt;
			CapturedNavigation result;
			result.guidance = navigation.ReadGuidance(*identity);
			result.settings = agent->ToAuthoredSettings();
			result.transformRevision = objects.GetObjectTransformRevision(*object);
			if(result.guidance.destination.kind == Navigation::NavigationDestinationKind::Object) {
				const auto& destination = result.guidance.destination.object;
				const auto target = objects.getObject(destination.objectId);
				if(!target || objects.getObjectLifetimeGeneration(*target) != destination.lifetimeGeneration
					|| !objects.isObjectActiveInHierarchy(*target)) return std::nullopt;
				result.destinationTransformRevision = objects.GetObjectTransformRevision(*target);
			}
			auto& geometry = result.geometry;
			geometry.controllerEnabled = controller->active;
			geometry.physicsEnabled = controller->enabled;
			geometry.shape = controller->shapeKind == Physics::PhysicsCharacterShapeKind::Capsule
				? Physics::PhysicsShapeKind::Capsule : Physics::PhysicsShapeKind::Cylinder;
			geometry.worldScale = {pose->scale.x, pose->scale.y, pose->scale.z};
			geometry.upDirection = {controller->upDirection.x, controller->upDirection.y, controller->upDirection.z};
			// A tilted owner does not become a ground-navigation capsule merely
			// because its authored controller up vector still says +Y.
			const auto q = pose->rotation;
			const double norm = double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w;
			if(!std::isfinite(norm) || norm < 1.0e-12
				|| std::abs(2.0 * (double(q.x) * q.y - double(q.w) * q.z) / norm) > 0.001
				|| std::abs(2.0 * (double(q.y) * q.z + double(q.w) * q.x) / norm) > 0.001
				|| 2.0 * (double(q.x) * q.x + double(q.z) * q.z) / norm > 0.001)
				geometry.upDirection = {};
			geometry.localCenter = {controller->localCenter.x, controller->localCenter.y, controller->localCenter.z};
			geometry.radiusMetres = controller->radius;
			geometry.totalHeightMetres = controller->height;
			geometry.contactPaddingMetres = controller->contactPadding;
			geometry.stepHeightMetres = controller->stepHeight;
			geometry.maxSlopeDegrees = controller->maximumSlopeAngleDegrees;
			geometry.profile = result.settings.traversal;
			const auto* colliders = objects.GetComponent<ColliderSetComponent>(*object);
			// Match the existing follower's self-contact policy: the redundant owner
			// collider set must be disabled, even when its records are currently empty.
			geometry.ownerColliderSetEnabled = colliders && colliders->active;
			return result;
		}

		[[nodiscard]] bool SameGeometry(const NavigationCompatibilityInput& a, const NavigationCompatibilityInput& b) {
			auto sameVector = [](auto x, auto y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
			return a.controllerEnabled == b.controllerEnabled && a.physicsEnabled == b.physicsEnabled
				&& a.ownerColliderSetEnabled == b.ownerColliderSetEnabled && a.shape == b.shape
				&& sameVector(a.worldScale, b.worldScale) && sameVector(a.upDirection, b.upDirection)
				&& sameVector(a.localCenter, b.localCenter) && a.radiusMetres == b.radiusMetres
				&& a.totalHeightMetres == b.totalHeightMetres && a.contactPaddingMetres == b.contactPaddingMetres
				&& a.stepHeightMetres == b.stepHeightMetres && a.maxSlopeDegrees == b.maxSlopeDegrees
				&& a.profile == b.profile;
		}

		class NavigationValidity final : public Physics::CharacterMotionValidity {
		public:
			NavigationValidity(LocomotionHandle handle, CapturedNavigation capture)
				: handle_(handle), capture_(std::move(capture)) {}

			[[nodiscard]] Physics::CharacterMotionValidityReason Validate() const noexcept override {
				using Reason = Physics::CharacterMotionValidityReason;
				try {
					const auto current = ReadCurrent(handle_);
					if(!current || current->guidance.identity != capture_.guidance.identity) return Reason::StaleLifetime;
					if(!SameGeometry(current->geometry, capture_.geometry)
						|| current->settings.arrivalToleranceMetres != capture_.settings.arrivalToleranceMetres
						|| current->settings.targetDisplacementThresholdMetres != capture_.settings.targetDisplacementThresholdMetres
						|| current->settings.minimumReplanIntervalSeconds != capture_.settings.minimumReplanIntervalSeconds
						|| current->guidance.configurationRevision != capture_.guidance.configurationRevision)
						return Reason::StaleConfiguration;
					const auto& now = current->guidance;
					const auto& before = capture_.guidance;
					if(now.IsUsable() != before.IsUsable() || now.state != before.state
						|| current->transformRevision != capture_.transformRevision
						|| current->destinationTransformRevision != capture_.destinationTransformRevision
						|| now.intentSequence != before.intentSequence || now.publicationGeneration != before.publicationGeneration
						|| now.routeGeneration != before.routeGeneration || now.observation != before.observation
						|| now.destination != before.destination || now.nextWaypoint != before.nextWaypoint
						|| now.suggestedDirection != before.suggestedDirection || now.remainingDistanceMetres != before.remainingDistanceMetres
						|| now.outcome != before.outcome || now.reason != before.reason) return Reason::StaleEvidence;
					return Reason::Valid;
				} catch(...) {
					// A failed diagnostic read must never make stale motion authoritative.
					return Reason::Unavailable;
				}
			}
		private:
			LocomotionHandle handle_;
			CapturedNavigation capture_;
		};
	}

	std::shared_ptr<const Physics::CharacterMotionValidity> CaptureNavigationBinding(LocomotionPreparationInput& input) {
		const auto current = ReadCurrent(input.runtime.handle);
		if(!current) return {};
		input.navigationGuidance = current->guidance;
		input.navigationCompatibility = CheckNavigationCompatibility(current->geometry);
		input.arrivalToleranceMetres = current->settings.arrivalToleranceMetres;
		if(!input.navigationCompatibility->compatible) return {};
		return std::make_shared<const NavigationValidity>(input.runtime.handle, *current);
	}
}
