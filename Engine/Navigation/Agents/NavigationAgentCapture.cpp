#include "NavigationAgentCapture.h"
#include "Components/NavigationAgentComponent.h"
#include "Core/Object/ObjectManager.h"
namespace TerraEngine::Navigation {
	std::optional<NavigationAgentCapture> CaptureNavigationAgent(ObjectManager& objects, const NavigationAgentRuntimeRecord& record) {
		const auto owner = objects.getObject(record.identity.object.objectId);
		if(!owner || objects.getObjectLifetimeGeneration(*owner) != record.identity.object.lifetimeGeneration) return {};
		const auto* component = objects.getRegistry().try_get<NavigationAgentComponent>(owner->EntityId());
		const auto pose = objects.GetCanonicalWorldPose(*owner);
		if(!component || !component->active || !pose || !record.settings.Validate()) return {};
		NavigationAgentCapture capture{}; capture.runtime = record;
		capture.owner = {record.identity.object, {record.identity.world, pose->position}, objects.GetObjectTransformRevision(*owner), objects.isObjectActiveInHierarchy(*owner)};
		if(!capture.owner.activeInHierarchy) return {};
		if(record.destination.kind == NavigationDestinationKind::Object) {
			const auto target = objects.getObject(record.destination.object.objectId);
			// Keep a valid owner packet when the target vanished. Preparation then
			// publishes Failed and clears stale guidance instead of silently skipping
			// the record and leaving an earlier route visible.
			if(!target || objects.getObjectLifetimeGeneration(*target) != record.destination.object.lifetimeGeneration) return capture;
			const auto targetPose = objects.GetCanonicalWorldPose(*target);
			if(!targetPose || !objects.isObjectActiveInHierarchy(*target)) return capture;
			capture.target = NavigationObjectEndpoint{record.destination.object, {record.identity.world, targetPose->position}, objects.GetObjectTransformRevision(*target), true};
		}
		return capture;
	}
}
