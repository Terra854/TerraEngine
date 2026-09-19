// Protected-boundary contracts for the native NavigationAgentSystem.
//
// These tests drive the real object/component binding and observe only copied
// status and guidance. They verify that lifetime and intent boundaries clear
// advice before gameplay can consume it.
#include "../TestHarness.h"

#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"

#include <cstdint>
#include <memory>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		std::shared_ptr<const NavigationGeometrySnapshot> FlatTerrain()
		{
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			return NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		bool PrepareTerrain(NavigationSystem& navigation)
		{
			const auto terrain = FlatTerrain();
			if(!terrain || !navigation.QueueInitialGroundBuild(NavigationProfile::Defaults(), {terrain})) return false;
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world != nullptr && world->terrainState == NavigationTerrainState::Available;
		}

		void Advance(NavigationSystem& navigation, NavigationAgentSystem& agents, std::uint32_t frames)
		{
			for(std::uint32_t frame = 0U; frame < frames; ++frame) {
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}

		// Wait only through public full-frame updates. This proves a route result is
		// published as guidance rather than merely accepted by the command queue.
		NavigationAgentGuidanceSnapshot WaitForGuidance(NavigationSystem& navigation,
			NavigationAgentSystem& agents, NavigationAgentIdentity identity,
			std::uint32_t maximumFrames = 64U)
		{
			for(std::uint32_t frame = 0U; frame < maximumFrames; ++frame) {
				Advance(navigation, agents, 1U);
				const NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);
				if(guidance.IsUsable()) return guidance;
			}
			return {};
		}

		NavigationLocation Destination(const NavigationWorldHandle& world, double x, double y = 0.0)
		{
			NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, y, 0.0};
			return location;
		}
	}

	void RegisterNavigationAgentBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Navigation.AgentBoundary.OwnerRemovalClearsGuidanceAtProtectedBoundary", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Owner Removal");
			Transform* transform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			const auto receipt = terrainReady && world ? agents.QueueLocationDestination(identity,
				Destination(world->world, 6.0), 0U, 0U, 1U, 2101U) : NavigationAgentReceipt{};
			const NavigationAgentGuidanceSnapshot published = WaitForGuidance(navigation, agents, identity);
			const bool transformUnmoved = transform != nullptr
				&& transform->worldPose.position.offsetMetres.x == 0.0;
			objects.removeObject(owner);
			agents.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world ? world->rootGeneration + 1U : 1U);
			const NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);
			context.Expect(identity.IsValid() && receipt.state != NavigationAgentReceiptState::Rejected
				&& published.IsUsable()
				&& transformUnmoved
				&& !guidance.IsUsable(),
				"Removing an agent owner must revoke previously usable copied guidance without the agent moving its Transform.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentBoundary.OwnerPoseRemovalReleasesRetainedRoute", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Owner Pose Removal");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			const auto receipt = terrainReady && world ? agents.QueueLocationDestination(identity,
				Destination(world->world, 6.0), 0U, 0U, 1U, 2110U) : NavigationAgentReceipt{};
			const NavigationAgentGuidanceSnapshot guidanceBeforeRemoval = WaitForGuidance(navigation, agents, identity);

			// The authored agent remains attached, but it no longer has a canonical
			// owner pose. This must be a terminal protected-boundary failure, not a
			// skipped capture that leaves its retained provider pair alive forever.
			objects.RemoveComponent<Transform>(owner);
			agents.ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world ? world->rootGeneration + 1U : 1U);
			// Removing any component advances the object's structural generation.
			// The old wrapper must become stale; the newly reconciled attachment
			// independently reports that its remaining agent has no owner pose.
			const NavigationAgentStatusSnapshot oldStatus = agents.ReadStatus(identity);
			const NavigationAgentIdentity currentIdentity = agents.FindIdentity(objects, objects.getUUID(owner));
			const NavigationAgentStatusSnapshot status = agents.ReadStatus(currentIdentity);
			const bool immediatelyInvalidated = status.state == NavigationAgentState::Failed
				&& status.outcome == NavigationOutcome::InvalidEndpoint
				&& status.reason == NavigationReason::InvalidStart
				&& oldStatus.outcome == NavigationOutcome::Stale
				&& oldStatus.reason == NavigationReason::SceneReplaced
				&& !agents.ReadGuidance(identity).IsUsable()
				&& !agents.ReadGuidance(currentIdentity).IsUsable();
			// Cancellation is visible immediately at the agent's protected boundary.
			// Let the provider's normal protected publication finish any queued cleanup
			// before asserting aggregate store ownership, so this test does not assume
			// that a worker-safe provider recycles a slot synchronously.
			navigation.Update(0.0F);
			const NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();
			context.Expect(receipt.state != NavigationAgentReceiptState::Rejected
				&& guidanceBeforeRemoval.IsUsable()
				&& immediatelyInvalidated
				&& usage.activeRequests == 0U && usage.activeRoutes == 0U,
				"Removing an owner's canonical pose must release retained request/route handles and publish InvalidStart before another consumer can use guidance.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentBoundary.StopClearsPriorRouteBeforeNewIntent", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance(); objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Stop Owner");
			Transform* transform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation); Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			const auto destination = terrainReady && world ? agents.QueueLocationDestination(identity,
				Destination(world->world, 8.0), 0U, 0U, 1U, 2102U) : NavigationAgentReceipt{};
			const NavigationAgentGuidanceSnapshot published = WaitForGuidance(navigation, agents, identity);
			const NavigationAgentStatusSnapshot beforeStop = agents.ReadStatus(identity);
			const auto stopped = agents.QueueStop(identity, beforeStop.configurationRevision,
				beforeStop.intentSequence, 2U, 2103U);
			Advance(navigation, agents, 2U);
			const NavigationAgentStatusSnapshot afterStop = agents.ReadStatus(identity);
			context.Expect(destination.state != NavigationAgentReceiptState::Rejected
				&& stopped.state != NavigationAgentReceiptState::Rejected
				&& published.IsUsable()
				&& afterStop.state == NavigationAgentState::Stopped
				&& !agents.ReadGuidance(identity).IsUsable()
				&& transform != nullptr && transform->worldPose.position.offsetMetres.x == 0.0,
				"Stop must clear prior copied route guidance while leaving Transform movement exclusively to gameplay.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentBoundary.RemovedObjectTargetBecomesTerminalFailure", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance(); objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Target Owner");
			const Object target = objects.createObject("Boundary Target");
			(void)objects.AddComponent<Transform>(owner); (void)objects.AddComponent<NavigationAgentComponent>(owner);
			(void)objects.AddComponent<Transform>(target);
			const bool terrainReady = PrepareTerrain(navigation); Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			const NavigationObjectIdentity targetIdentity{objects.getUUID(target), objects.getObjectLifetimeGeneration(target)};
			const auto receipt = terrainReady && world ? agents.QueueObjectDestination(identity, targetIdentity,
				0U, 0U, 1U, 2104U) : NavigationAgentReceipt{};
			// Admit the copied target identity while it is valid. Removing it before
			// this boundary is a rejected command that correctly preserves Idle, not a
			// terminal failure of an intent that never existed.
			Advance(navigation, agents, 1U);
			const auto admitted = agents.ReadReceipt(identity, 2104U);
			objects.removeObject(target);
			Advance(navigation, agents, 8U);
			const NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
			context.Expect(receipt.state != NavigationAgentReceiptState::Rejected
				&& admitted && admitted->state == NavigationAgentReceiptState::Applied
				&& status.state == NavigationAgentState::Failed
				&& status.outcome == NavigationOutcome::InvalidEndpoint
				&& status.reason == NavigationReason::InvalidDestination
				&& !agents.ReadGuidance(identity).IsUsable(),
				"A removed object target must become a terminal invalid-endpoint failure rather than retrying stale work forever.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentBoundary.ConfigChangePreservesIntentAndReplans", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Config Owner");
			const Object target = objects.createObject("Boundary Config Target");
			Transform* ownerTransform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			(void)objects.AddComponent<Transform>(target);
			Scene::WorldPose targetPose{};
			targetPose.position.offsetMetres = {5.0, 0.0, 0.0};
			(void)objects.SetCanonicalWorldPose(target, targetPose);
			objects.MarkObjectTransformDirty(target);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const NavigationObjectIdentity targetIdentity{
				objects.getUUID(target), objects.getObjectLifetimeGeneration(target)};
			const NavigationAgentReceipt receipt = terrainReady
				? agents.QueueObjectDestination(identity, targetIdentity, 0U, 0U, 1U, 2105U)
				: NavigationAgentReceipt{};
			const NavigationAgentGuidanceSnapshot beforeChange = WaitForGuidance(navigation, agents, identity);
			const NavigationAgentStatusSnapshot beforeStatus = agents.ReadStatus(identity);
			NavigationAgentComponent* component = objects.GetComponent<NavigationAgentComponent>(owner);
			if(component != nullptr) {
				component->arrivalToleranceMetres = 0.15;
				objects.notifyObjectComponentValueChanged(owner, NavigationAgentComponent::kComponentName);
			}
			Advance(navigation, agents, 1U);
			const NavigationAgentStatusSnapshot afterChange = agents.ReadStatus(identity);
			const NavigationAgentGuidanceSnapshot duringReplan = agents.ReadGuidance(identity);
			const NavigationAgentGuidanceSnapshot afterReplan = WaitForGuidance(navigation, agents, identity);

			context.Expect(receipt.state != NavigationAgentReceiptState::Rejected && beforeChange.IsUsable()
				&& component != nullptr && afterChange.configurationRevision > beforeStatus.configurationRevision,
				"A protected authored-settings edit must advance configuration revision after a real agent intent exists.");
			context.Expect(!duringReplan.IsUsable() && afterReplan.IsUsable()
				&& afterReplan.destination.kind == NavigationDestinationKind::Object
				&& afterReplan.destination.object == targetIdentity,
				"A settings edit must invalidate old advice, replan, and retain the copied object destination identity.");
			context.Expect(ownerTransform != nullptr && ownerTransform->worldPose.position.offsetMetres.x == 0.0,
				"Replanning after configuration changes must not move the owner Transform.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentBoundary.InactiveTargetParentFailsAndClearsGuidance", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			NavigationSystem& navigation = NavigationSystem::Instance();
			NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown(); navigation.Initialize(); agents.Initialize();
			const Object owner = objects.createObject("Boundary Parent Owner");
			const Object parent = objects.createObject("Boundary Target Parent");
			const Object target = objects.createObject("Boundary Parent Target");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			(void)objects.AddComponent<Transform>(target);
			(void)objects.setParent(target, parent);
			Scene::WorldPose targetPose{};
			targetPose.position.offsetMetres = {4.0, 0.0, 0.0};
			(void)objects.SetCanonicalWorldPose(target, targetPose);
			objects.MarkObjectTransformDirty(target);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 1U);
			const NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const NavigationObjectIdentity targetIdentity{
				objects.getUUID(target), objects.getObjectLifetimeGeneration(target)};
			const NavigationAgentReceipt receipt = terrainReady
				? agents.QueueObjectDestination(identity, targetIdentity, 0U, 0U, 1U, 2106U)
				: NavigationAgentReceipt{};
			const NavigationAgentGuidanceSnapshot beforeDisable = WaitForGuidance(navigation, agents, identity);
			objects.setObjectActive(parent, false);
			Advance(navigation, agents, 2U);
			const NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);

			context.Expect(receipt.state != NavigationAgentReceiptState::Rejected && beforeDisable.IsUsable()
				&& status.state == NavigationAgentState::Failed
				&& status.outcome == NavigationOutcome::InvalidEndpoint
				&& !agents.ReadGuidance(identity).IsUsable(),
				"A target disabled through an ancestor must fail like a removed target and clear its advice.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});
	}
}
