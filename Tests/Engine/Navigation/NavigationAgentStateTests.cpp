// NavigationAgentStateTests.cpp - value-level agent state and guidance contracts.

#include "../TestHarness.h"

#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentTypes.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Scene/SceneMutationQueue.h"

#include <array>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace TerraEngine::Tests {
	namespace {
		Navigation::NavigationAgentIdentity ValidAgentIdentity() {
			UUID objectId{};
			objectId[0] = 0xA7U;
			return {{Navigation::NavigationWorldId{901U}, 4U}, {objectId, 7U}, 3U, 0U, 2U};
		}

		Navigation::NavigationLocation ValidLocation(const Navigation::NavigationWorldHandle& world) {
			Navigation::NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {2.0, 0.0, -3.0};
			return location;
		}

		Navigation::NavigationAgentGuidanceSnapshot ValidGuidance() {
			Navigation::NavigationAgentGuidanceSnapshot snapshot{};
			snapshot.identity = ValidAgentIdentity();
			snapshot.configurationRevision = 4U;
			snapshot.intentSequence = 5U;
			snapshot.publicationGeneration = 6U;
			snapshot.routeGeneration = 7U;
			snapshot.observation = {1U, 2U, 3U, 4U, 5U, 6U};
			snapshot.state = Navigation::NavigationAgentState::Following;
			snapshot.nextWaypoint = ValidLocation(snapshot.identity.world);
			snapshot.suggestedDirection = {1.0, 0.0, 0.0};
			snapshot.remainingDistanceMetres = 2.0;
			snapshot.usable = true;
			return snapshot;
		}

		std::shared_ptr<const Navigation::NavigationGeometrySnapshot> FlatTerrain() {
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{-16.0F, 0.0F, -16.0F}, {16.0F, 0.0F, 16.0F},
				{16.0F, 0.0F, -16.0F}, {-16.0F, 0.0F, 16.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			Navigation::NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = Navigation::NavigationSourceKind::Terrain;
			return Navigation::NavigationGeometrySnapshot::Create(descriptor, document).snapshot;
		}

		bool PrepareTerrain(Navigation::NavigationSystem& navigation) {
			const auto terrain = FlatTerrain();
			if(!terrain || !navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain})) {
				return false;
			}
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world != nullptr && world->terrainState == Navigation::NavigationTerrainState::Available;
		}

		void Advance(Navigation::NavigationSystem& navigation,
			Navigation::NavigationAgentSystem& agents, std::uint32_t frameCount = 1U) {
			for(std::uint32_t frame = 0U; frame < frameCount; ++frame) {
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}

		// Route building is asynchronous. Polling through bounded full frames keeps
		// this test on the public system boundary instead of inspecting its store.
		Navigation::NavigationAgentGuidanceSnapshot WaitForGuidance(
			Navigation::NavigationSystem& navigation, Navigation::NavigationAgentSystem& agents,
			Navigation::NavigationAgentIdentity identity, std::uint32_t maximumFrames = 64U) {
			for(std::uint32_t frame = 0U; frame < maximumFrames; ++frame) {
				Advance(navigation, agents);
				const Navigation::NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);
				if(guidance.IsUsable()) return guidance;
			}
			return {};
		}

		Navigation::NavigationLocation Location(const Navigation::NavigationWorldHandle& world,
			double x, double y = 0.0, double z = 0.0) {
			Navigation::NavigationLocation location{};
			location.world = world;
			location.position.offsetMetres = {x, y, z};
			return location;
		}

		// Builds copied automatic-capture inputs without retaining any scene object.
		// The mesh source moves only on the negative side of the terrain so tests can
		// distinguish an affected route from a route that should keep its evidence.
		Navigation::NavigationEligibleSourceCandidate AutomaticSource(
			Navigation::NavigationSourceKind kind, Navigation::NavigationGeneration revision,
			double originX, std::shared_ptr<const Navigation::NavigationGeometrySnapshot>& geometry,
			const Navigation::NavigationEligibleSourceIdentity* retainedIdentity = nullptr) {
			Navigation::NavigationEligibleSourceCandidate candidate{};
			if(retainedIdentity != nullptr) candidate.identity = *retainedIdentity;
			else {
				candidate.identity.sourceId = GenerateUUID();
				candidate.identity.objectId = GenerateUUID();
				candidate.identity.objectLifetimeGeneration = 1U;
				candidate.identity.assetId = GenerateUUID();
				candidate.identity.kind = kind;
			}
			candidate.revisions = {1U, revision, 1U, 1U, 1U};
			Navigation::NavigationGeometryDocument document{};
			document.kind = Navigation::NavigationGeometryPayloadKind::TriangleMesh;
			if(kind == Navigation::NavigationSourceKind::Terrain) {
				document.vertices = {{-32.0F, 0.0F, -32.0F}, {32.0F, 0.0F, 32.0F},
					{32.0F, 0.0F, -32.0F}, {-32.0F, 0.0F, 32.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};
			} else {
				document.vertices = {{-1.0F, 0.0F, -1.0F}, {1.0F, 2.0F, -1.0F},
					{1.0F, 0.0F, 1.0F}, {-1.0F, 2.0F, 1.0F}};
				document.triangles = {{0U, 1U, 2U}, {0U, 2U, 3U}};
			}
			Navigation::NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceUuid.value = candidate.identity.sourceId;
			descriptor.sourceRevision = Navigation::ComputeNavigationEligibleSourceRevision(
				candidate.identity, candidate.revisions);
			descriptor.canonicalPlacementRevision = revision;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = kind;
			descriptor.canonicalFrameOrigin.offsetMetres.x = originX;
			geometry = Navigation::NavigationGeometrySnapshot::Create(descriptor, std::move(document)).snapshot;
			candidate.geometry = geometry;
			candidate.activeInHierarchy = true;
			candidate.physicalBackingCurrent = true;
			return candidate;
		}

		Navigation::NavigationSystem::InitialGroundBuildInput AutomaticBuild(
			const Navigation::NavigationEligibleSourceCandidate& terrain,
			const Navigation::NavigationEligibleSourceCandidate& changed) {
			const std::array<Navigation::NavigationEligibleSourceCandidate, 2U> candidates{{terrain, changed}};
			const Navigation::NavigationEligibleSourceIndexBuildResult index =
				Navigation::BuildNavigationEligibleSourceIndex(candidates);
			Navigation::NavigationSystem::InitialGroundBuildInput build{};
			build.profile = Navigation::NavigationProfile::Defaults();
			build.sources = {terrain.geometry, changed.geometry};
			build.sourceIndex = {{terrain.identity.sourceId, terrain.geometry->Descriptor().sourceRevision,
				terrain.revisions.geometryRevision, true, terrain.identity, terrain.revisions},
				{changed.identity.sourceId, changed.geometry->Descriptor().sourceRevision,
				changed.revisions.geometryRevision, true, changed.identity, changed.revisions}};
			build.eligibleSourceIndex = index.accepted
				? std::make_shared<const Navigation::NavigationEligibleSourceIndex>(index.index) : nullptr;
			build.terrainPublicationGeneration = 1U;
			return build;
		}
	}

	void RegisterNavigationAgentStateTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentState.GuidanceIsUsableOnlyForFollowingEvidence", [](TestContext& context) {
			const Navigation::NavigationAgentGuidanceSnapshot valid = ValidGuidance();
			context.Expect(valid.IsUsable(),
				"Only a generation-qualified Following snapshot with valid waypoint and world evidence may guide gameplay movement.");

			for(const Navigation::NavigationAgentState state : {
				Navigation::NavigationAgentState::Idle, Navigation::NavigationAgentState::Requesting,
				Navigation::NavigationAgentState::Arrived, Navigation::NavigationAgentState::Stopped,
				Navigation::NavigationAgentState::Failed}) {
				auto unusable = valid;
				unusable.state = state;
				context.Expect(!unusable.IsUsable(),
					"Every non-Following agent state must publish zero-usable guidance so navigation never retains movement authority.");
			}
		});

		registry.Add("Navigation.AgentState.GuidanceFailsClosedForStaleOrNonFiniteValues", [](TestContext& context) {
			auto guidance = ValidGuidance();
			guidance.identity.runtimeSlotGeneration = 0U;
			context.Expect(!guidance.IsUsable(),
				"A recycled or removed agent attachment must invalidate previously retained guidance.");
			guidance = ValidGuidance();
			guidance.observation.worldPublicationGeneration = 0U;
			context.Expect(!guidance.IsUsable(),
				"Guidance without current terrain/world/source evidence must fail closed before a controller consumes it.");
			guidance = ValidGuidance();
			guidance.suggestedDirection.x = std::numeric_limits<double>::quiet_NaN();
			context.Expect(!guidance.IsUsable(),
				"A non-finite waypoint direction must never cross the agent-to-gameplay boundary.");
			guidance = ValidGuidance();
			guidance.remainingDistanceMetres = -0.01;
			context.Expect(!guidance.IsUsable(),
				"Negative remaining distance must reject instead of presenting a contradictory arrival signal.");
		});

		registry.Add("Navigation.AgentState.AuthoredSettingsAndCommandsRemainFiniteCopyValues", [](TestContext& context) {
			Navigation::NavigationAgentAuthoredSettings settings{};
			context.Expect(settings.Validate(),
				"The default copied traversal settings must be valid for an optional new agent attachment.");
			settings.minimumReplanIntervalSeconds = 0.09;
			context.Expect(!settings.Validate(),
				"Agent retry scheduling must reject intervals below the bounded 100-millisecond minimum.");
			settings = {};
			settings.arrivalToleranceMetres = std::numeric_limits<double>::infinity();
			context.Expect(!settings.Validate(),
				"Non-finite authored tolerances must reject before a component transaction publishes.");

			Navigation::NavigationAgentCommand command{};
			command.identity = ValidAgentIdentity();
			command.kind = Navigation::NavigationAgentCommandKind::SetLocationDestination;
			command.destination.kind = Navigation::NavigationDestinationKind::Location;
			command.destination.location = ValidLocation(command.identity.world);
			command.expectedConfigurationRevision = 4U;
			command.expectedIntentSequence = 5U;
			command.producerOrderingToken = 6U;
			command.correlationId = 7U;
			context.Expect(command.identity.IsValid()
				&& Navigation::ValidateNavigationRequestedDestination(command.destination).accepted,
				"A deferred command must contain a complete copied identity and destination before protected admission.");
		});

		registry.Add("Navigation.AgentState.BindingIndexSurvivesRemovalAndAttachmentReplacement", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown(); agents.Shutdown();
			navigation.Initialize(); agents.Initialize();

			const Object first = objects.createObject("Binding index first");
			const Object middle = objects.createObject("Binding index middle");
			const Object last = objects.createObject("Binding index last");
			(void)objects.AddComponent<NavigationAgentComponent>(first);
			(void)objects.AddComponent<NavigationAgentComponent>(middle);
			(void)objects.AddComponent<NavigationAgentComponent>(last);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity oldFirst = agents.FindIdentity(objects, objects.getUUID(first));
			const Navigation::NavigationAgentIdentity oldMiddle = agents.FindIdentity(objects, objects.getUUID(middle));
			const Navigation::NavigationAgentIdentity oldLast = agents.FindIdentity(objects, objects.getUUID(last));

			// Removing the first element shifts the later vector slots. A UUID sidecar
			// must rebuild those slots before any later identity lookup can use it.
			objects.removeObject(first);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity middleAfterRemoval =
				agents.FindIdentity(objects, objects.getUUID(middle));
			const Navigation::NavigationAgentIdentity lastAfterRemoval =
				agents.FindIdentity(objects, objects.getUUID(last));

			// Reattaching the component keeps the object UUID but changes the complete
			// attachment identity. The lookup fast path must not accept its stale slot.
			objects.RemoveComponent<NavigationAgentComponent>(middle);
			(void)objects.AddComponent<NavigationAgentComponent>(middle);
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity middleReplacement =
				agents.FindIdentity(objects, objects.getUUID(middle));
			const Navigation::NavigationAgentStatusSnapshot staleMiddle = agents.ReadStatus(oldMiddle);
			const Navigation::NavigationAgentIdentity lastAfterReplacement =
				agents.FindIdentity(objects, objects.getUUID(last));

			// A lifecycle reset must discard UUID-to-slot contents before a fresh
			// discovery pass. Change attachment generation while stopped so a stale
			// identity cannot be mistaken for the new runtime binding.
			agents.RevokeAllAgents(Navigation::NavigationReason::SceneReplaced);
			agents.Shutdown();
			objects.RemoveComponent<NavigationAgentComponent>(middle);
			(void)objects.AddComponent<NavigationAgentComponent>(middle);
			agents.Initialize();
			agents.Update(0.0F);
			const Navigation::NavigationAgentIdentity middleAfterLifecycleReset =
				agents.FindIdentity(objects, objects.getUUID(middle));
			const Navigation::NavigationAgentIdentity lastAfterLifecycleReset =
				agents.FindIdentity(objects, objects.getUUID(last));

			context.Expect(oldFirst.IsValid() && oldMiddle.IsValid() && oldLast.IsValid()
				&& middleAfterRemoval == oldMiddle && lastAfterRemoval == oldLast
				&& middleReplacement.IsValid() && middleReplacement != oldMiddle
				&& staleMiddle.outcome == Navigation::NavigationOutcome::Stale
				&& lastAfterReplacement == oldLast
				&& middleAfterLifecycleReset.IsValid()
				&& middleAfterLifecycleReset != middleReplacement
				&& lastAfterLifecycleReset.IsValid(),
				"The protected UUID-to-slot sidecar must rebuild after vector erasure and require a full lifetime/attachment identity match before returning a replacement binding.");
			agents.Shutdown(); navigation.Shutdown(); objects.clear();
		});

		registry.Add("Navigation.AgentState.LocationDestinationPublishesGuidanceWithoutMovingTheOwner", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Agent Guidance Owner");
			Transform* transform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			const Navigation::NavigationAgentReceipt receipt = terrainReady && world != nullptr
				? agents.QueueLocationDestination(identity, Location(world->world, 6.0), 0U, 0U, 1U, 1001U)
				: Navigation::NavigationAgentReceipt{};
			const Navigation::NavigationAgentGuidanceSnapshot initialGuidance = WaitForGuidance(navigation, agents, identity);

			context.Expect(identity.IsValid() && receipt.state != Navigation::NavigationAgentReceiptState::Rejected,
				"An enabled NavigationAgentComponent must bind to a generation-qualified runtime record and accept a valid copied destination.");
			context.Expect(transform != nullptr && transform->worldPose.position.offsetMetres.x == 0.0,
				"Agent pathfinding must never write its owner's Transform; gameplay remains responsible for consuming guidance.");
			context.Expect(initialGuidance.IsUsable() && initialGuidance.state == Navigation::NavigationAgentState::Following,
				"A terrain-backed destination must eventually expose real generation-qualified Following guidance.");

			// Gameplay, not the navigation agent, consumes guidance and publishes an
			// observed Transform pose. Walk every reported waypoint to verify progress
			// and final arrival without granting the agent movement authority.
			Navigation::NavigationAgentGuidanceSnapshot guidance = initialGuidance;
			for(std::uint32_t step = 0U; step < 8U && guidance.IsUsable(); ++step) {
				Scene::WorldPose observedPose{};
				observedPose.position = guidance.nextWaypoint.position;
				(void)objects.SetCanonicalWorldPose(owner, observedPose);
				objects.MarkObjectTransformDirty(owner);
				Advance(navigation, agents, 2U);
				guidance = agents.ReadGuidance(identity);
			}
			const Navigation::NavigationAgentStatusSnapshot arrived = agents.ReadStatus(identity);
			const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterArrival =
				navigation.PublishedStoreUsage();
			context.Expect(arrived.state == Navigation::NavigationAgentState::Arrived && !guidance.IsUsable(),
				"Only gameplay-observed waypoint progress may complete the destination and retire guidance.");
			context.Expect(usageAfterArrival.activeRequests == 0U && usageAfterArrival.activeRoutes == 0U,
				"Arrival must release the agent's retained completed provider pair once no movement guidance remains.");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.FreshDestinationDoesNotInheritRetryCooldown", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();

			const Object owner = objects.createObject("Fresh destination cooldown owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const std::shared_ptr<const Navigation::NavigationWorldRoot> world = navigation.PublishedWorld();
			const Navigation::NavigationAgentReceipt firstQueued = terrainReady && world != nullptr
				? agents.QueueLocationDestination(identity, Location(world->world, 4.0), 0U, 0U, 1U, 1201U)
				: Navigation::NavigationAgentReceipt{};

			// Do not advance elapsed time after the first admission. A successful route
			// request records the authored retry interval, so the second command proves
			// that an explicit replacement is not incorrectly treated as a retry.
			agents.Update(0.0F);
			const Navigation::NavigationAgentStatusSnapshot firstStatus = agents.ReadStatus(identity);
			const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterFirst = navigation.PublishedStoreUsage();
			Navigation::NavigationLocation invalidDestination = world
				? Location(world->world, 8.0) : Navigation::NavigationLocation{};
			// Keep this location structurally valid so the command crosses the queue
			// boundary, then give it a different valid world generation. Capture must
			// reject that stale-world input without disturbing the active request.
			if(invalidDestination.world.IsValid()) ++invalidDestination.world.worldGeneration;
			const Navigation::NavigationAgentReceipt rejectedQueued = identity.IsValid()
				? agents.QueueLocationDestination(identity, invalidDestination,
					firstStatus.configurationRevision, firstStatus.intentSequence, 2U, 1202U)
				: Navigation::NavigationAgentReceipt{};
			agents.Update(0.0F);
			const Navigation::NavigationAgentStatusSnapshot afterRejectedStatus = agents.ReadStatus(identity);
			const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterRejected = navigation.PublishedStoreUsage();
			const std::optional<Navigation::NavigationAgentReceipt> rejected = agents.ReadReceipt(identity, 1202U);
			const Navigation::NavigationAgentReceipt secondQueued = identity.IsValid() && world != nullptr
				? agents.QueueLocationDestination(identity, Location(world->world, 8.0),
					firstStatus.configurationRevision, firstStatus.intentSequence, 3U, 1203U)
				: Navigation::NavigationAgentReceipt{};
			agents.Update(0.0F);
			const Navigation::NavigationAgentStatusSnapshot secondStatus = agents.ReadStatus(identity);
			const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterReplacement = navigation.PublishedStoreUsage();
			const std::optional<Navigation::NavigationAgentReceipt> secondApplied = agents.ReadReceipt(identity, 1203U);

			context.Expect(identity.IsValid() && firstQueued.state != Navigation::NavigationAgentReceiptState::Rejected
				&& usageAfterFirst.activeRequests == 1U,
				"The cooldown fixture requires the first accepted destination to create one active provider request before its retry interval elapses.");
			context.Expect(rejectedQueued.state != Navigation::NavigationAgentReceiptState::Rejected
				&& rejected && rejected->state == Navigation::NavigationAgentReceiptState::Rejected
				&& afterRejectedStatus.intentSequence == firstStatus.intentSequence
				&& usageAfterRejected.activeRequests == usageAfterFirst.activeRequests,
				"A rejected destination must preserve the active intent and provider request instead of resetting or replacing the retry state.");
			context.Expect(secondQueued.state != Navigation::NavigationAgentReceiptState::Rejected
				&& secondApplied && secondApplied->state == Navigation::NavigationAgentReceiptState::Applied
				&& secondStatus.intentSequence == firstStatus.intentSequence + 1U
				&& secondStatus.state == Navigation::NavigationAgentState::Requesting
				&& usageAfterReplacement.activeRequests == 1U,
				"A fresh accepted destination must replace the old request and admit its new request immediately, without inheriting the retry cooldown.");

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.UnavailableTerrainInvalidatesGuidanceWithoutRevokingIntent", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Unavailable terrain agent owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto root = navigation.PublishedWorld();
			const Navigation::NavigationAgentReceipt queued = terrainReady && root != nullptr
				? agents.QueueLocationDestination(identity, Location(root->world, 6.0), 0U, 0U, 1U, 1010U)
				: Navigation::NavigationAgentReceipt{};
			const Navigation::NavigationAgentGuidanceSnapshot beforeFence = WaitForGuidance(navigation, agents, identity);
			const Navigation::NavigationAgentStatusSnapshot intentBeforeFence = agents.ReadStatus(identity);

			// Navigation support is no longer authoritative, but this is not a scene
			// replacement. Preserve the agent binding and copied destination intent so a
			// fresh terrain capture can retry it; only motion advice becomes unusable.
			const bool fenced = beforeFence.IsUsable() && navigation.PublishUnavailableTerrain(nullptr, true);
			agents.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd, 1U);
			agents.Update(1.0F / 60.0F);
			const Navigation::NavigationAgentStatusSnapshot afterFence = agents.ReadStatus(identity);
			const Navigation::NavigationAgentGuidanceSnapshot guidanceAfterFence = agents.ReadGuidance(identity);
			const auto unavailableRoot = navigation.PublishedWorld();
			// Keep this failure detail numeric and local to the assertion: it records the
			// published lifecycle facts without making enum presentation part of the ABI.
			const std::string failureDetail = " (identity=" + std::to_string(identity.IsValid())
				+ ", queued=" + std::to_string(static_cast<unsigned int>(queued.state))
				+ ", guidanceBefore=" + std::to_string(beforeFence.IsUsable())
				+ ", fenced=" + std::to_string(fenced)
				+ ", live=" + std::to_string(agents.HasLiveIdentity(identity))
				+ ", rootState=" + std::to_string(unavailableRoot ? static_cast<unsigned int>(unavailableRoot->terrainState) : 999U)
				+ ", guidanceAfter=" + std::to_string(guidanceAfterFence.IsUsable())
				+ ", intentBefore=" + std::to_string(intentBeforeFence.intentSequence)
				+ ", intentAfter=" + std::to_string(afterFence.intentSequence)
				+ ", configBefore=" + std::to_string(intentBeforeFence.configurationRevision)
				+ ", configAfter=" + std::to_string(afterFence.configurationRevision)
				+ ", stateAfter=" + std::to_string(static_cast<unsigned int>(afterFence.state))
				+ ", outcomeAfter=" + std::to_string(static_cast<unsigned int>(afterFence.outcome)) + ")";

			context.Expect(identity.IsValid() && queued.state != Navigation::NavigationAgentReceiptState::Rejected
				&& beforeFence.IsUsable() && fenced && agents.HasLiveIdentity(identity),
				"The unavailable-terrain fixture requires a bound agent with a previously usable copied route.");
			context.Expect(unavailableRoot != nullptr
				&& unavailableRoot->terrainState == Navigation::NavigationTerrainState::Unavailable
				&& !guidanceAfterFence.IsUsable()
				&& afterFence.intentSequence == intentBeforeFence.intentSequence
				&& afterFence.configurationRevision == intentBeforeFence.configurationRevision
				&& afterFence.state == Navigation::NavigationAgentState::Requesting
				// A following agent has already copied and released its provider pair. The
				// status therefore retains that last completed provider outcome, while the
				// Requesting state—not a fabricated new provider result—describes the
				// unavailable terrain retry lifecycle.
				&& intentBeforeFence.outcome == Navigation::NavigationOutcome::Complete
				&& afterFence.outcome == intentBeforeFence.outcome,
				"Publishing unavailable terrain must neutralize movement guidance while retaining the live agent identity, its last provider result, and retryable intent for a fresh capture."
				+ failureDetail);

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.MovingObjectTargetReplansAndStopWinsOverEarlierIntent", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Agent Moving Target Owner");
			const Object target = objects.createObject("Agent Moving Target");
			Transform* ownerTransform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			(void)objects.AddComponent<Transform>(target);
			Scene::WorldPose targetPose{};
			targetPose.position.offsetMetres = {4.0, 0.0, 0.0};
			(void)objects.SetCanonicalWorldPose(target, targetPose);
			objects.MarkObjectTransformDirty(target);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const Navigation::NavigationObjectIdentity targetIdentity{
				objects.getUUID(target), objects.getObjectLifetimeGeneration(target)};
			const Navigation::NavigationAgentReceipt pursuit = terrainReady
				? agents.QueueObjectDestination(identity, targetIdentity, 0U, 0U, 1U, 1002U)
				: Navigation::NavigationAgentReceipt{};
			const Navigation::NavigationAgentGuidanceSnapshot pursuitGuidance = WaitForGuidance(navigation, agents, identity);
			// A sub-threshold target adjustment must preserve the retained completed
			// route. The larger move below is the explicit superseding intent.
			targetPose.position.offsetMetres.x = 4.1;
			(void)objects.SetCanonicalWorldPose(target, targetPose);
			objects.MarkObjectTransformDirty(target);
			agents.Update(1.0F / 60.0F);
			const Navigation::NavigationAgentStatusSnapshot belowThreshold = agents.ReadStatus(identity);
			const Navigation::NavigationAgentGuidanceSnapshot belowThresholdGuidance = agents.ReadGuidance(identity);
			targetPose.position.offsetMetres.x = 8.0;
			(void)objects.SetCanonicalWorldPose(target, targetPose);
			objects.MarkObjectTransformDirty(target);
			// Run the protected agent edge without a navigation tick. This makes the
			// replacement intent observable before a fast provider can complete it.
			agents.Update(1.0F / 60.0F);
			const Navigation::NavigationAgentStatusSnapshot replanning = agents.ReadStatus(identity);
			const Navigation::NavigationAgentReceipt stopped = agents.QueueStop(
				identity, replanning.configurationRevision, replanning.intentSequence, 2U, 1003U);
			Advance(navigation, agents, 2U);
			const Navigation::NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
			const Navigation::NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);
			const Navigation::NavigationSystem::StoreUsageSnapshot usageAfterStop = navigation.PublishedStoreUsage();

			context.Expect(pursuit.state != Navigation::NavigationAgentReceiptState::Rejected
				&& pursuitGuidance.IsUsable() && belowThreshold.state == Navigation::NavigationAgentState::Following
				&& belowThresholdGuidance.IsUsable()
				&& replanning.state == Navigation::NavigationAgentState::Requesting,
				"A sub-threshold target move must retain guidance, while a larger move must release the retained route and request a replacement.");
			context.Expect(ownerTransform != nullptr && ownerTransform->worldPose.position.offsetMetres.x == 0.0,
				"Target pursuit must not move the owner before gameplay consumes the copied guidance.");
			context.Expect(stopped.state != Navigation::NavigationAgentReceiptState::Rejected
				&& status.state == Navigation::NavigationAgentState::Stopped && !guidance.IsUsable()
				&& usageAfterStop.activeRequests == 0U && usageAfterStop.activeRoutes == 0U,
				"A later Stop command must supersede target pursuit, release its retained handles, and make prior guidance unusable before gameplay can move again.");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.UnsupportedStackedFloorDestinationCannotReportArrival", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Agent Stacked Floor Owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			if(terrainReady && world != nullptr) {
				(void)agents.QueueLocationDestination(identity, Location(world->world, 0.0, 5.0), 0U, 0U, 1U, 1004U);
			}
			Advance(navigation, agents, 4U);
			const Navigation::NavigationAgentStatusSnapshot status = agents.ReadStatus(identity);
			const Navigation::NavigationAgentGuidanceSnapshot guidance = agents.ReadGuidance(identity);

			context.Expect(status.state != Navigation::NavigationAgentState::Arrived && !guidance.IsUsable(),
				"A vertically separated destination without compatible support must fail or remain pending; matching horizontal coordinates alone cannot produce arrival.");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.AutomaticSourceRefreshPreservesOnlyUnaffectedRetainedGuidance", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();

			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> firstChangedGeometry;
			const auto terrain = AutomaticSource(Navigation::NavigationSourceKind::Terrain,
				1U, 0.0, terrainGeometry);
			const auto firstChanged = AutomaticSource(Navigation::NavigationSourceKind::Mesh,
				1U, -10.0, firstChangedGeometry);
			const Navigation::NavigationSystem::InitialGroundBuildInput firstBuild =
				AutomaticBuild(terrain, firstChanged);
			const bool firstQueued = terrainGeometry != nullptr && firstChangedGeometry != nullptr
				&& firstBuild.eligibleSourceIndex != nullptr && navigation.QueueInitialGroundBuilds({firstBuild});
			navigation.Update(0.0F);

			const Object affectedOwner = objects.createObject("Affected automatic route agent");
			const Object unaffectedOwner = objects.createObject("Unaffected automatic route agent");
			(void)objects.AddComponent<Transform>(affectedOwner);
			(void)objects.AddComponent<Transform>(unaffectedOwner);
			(void)objects.AddComponent<NavigationAgentComponent>(affectedOwner);
			(void)objects.AddComponent<NavigationAgentComponent>(unaffectedOwner);
			Scene::WorldPose affectedPose{};
			affectedPose.position.offsetMetres.x = -12.0;
			Scene::WorldPose unaffectedPose{};
			unaffectedPose.position.offsetMetres.x = 8.0;
			(void)objects.SetCanonicalWorldPose(affectedOwner, affectedPose);
			(void)objects.SetCanonicalWorldPose(unaffectedOwner, unaffectedPose);
			objects.MarkObjectTransformDirty(affectedOwner);
			objects.MarkObjectTransformDirty(unaffectedOwner);
			Advance(navigation, agents, 2U);

			const std::shared_ptr<const Navigation::NavigationWorldRoot> world = navigation.PublishedWorld();
			const Navigation::NavigationAgentIdentity affected =
				agents.FindIdentity(objects, objects.getUUID(affectedOwner));
			const Navigation::NavigationAgentIdentity unaffected =
				agents.FindIdentity(objects, objects.getUUID(unaffectedOwner));
			const auto affectedReceipt = world ? agents.QueueLocationDestination(
				affected, Location(world->world, -6.0), 0U, 0U, 1U, 4001U)
				: Navigation::NavigationAgentReceipt{};
			const auto unaffectedReceipt = world ? agents.QueueLocationDestination(
				unaffected, Location(world->world, 14.0), 0U, 0U, 1U, 4002U)
				: Navigation::NavigationAgentReceipt{};
			const auto affectedGuidance = WaitForGuidance(navigation, agents, affected);
			const auto unaffectedGuidance = WaitForGuidance(navigation, agents, unaffected);

			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> replacementChangedGeometry;
			const auto replacementChanged = AutomaticSource(Navigation::NavigationSourceKind::Mesh,
				2U, -8.0, replacementChangedGeometry, &firstChanged.identity);
			const Navigation::NavigationSystem::InitialGroundBuildInput replacementBuild =
				AutomaticBuild(terrain, replacementChanged);
			const bool replacementQueued = replacementChangedGeometry != nullptr
				&& replacementBuild.eligibleSourceIndex != nullptr
				&& navigation.QueueInitialGroundBuilds({replacementBuild});

			// Staging immediately retires only the route crossing the copied old/new
			// mesh bounds. The untouched route remains consumable through publication.
			agents.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd, 1U);
			const auto affectedAfterStage = agents.ReadGuidance(affected);
			const auto unaffectedAfterStage = agents.ReadGuidance(unaffected);
			navigation.Update(0.0F);
			agents.Update(1.0F / 60.0F);
			const auto unaffectedAfterRefresh = agents.ReadGuidance(unaffected);
			const auto affectedStatus = agents.ReadStatus(affected);

			context.Expect(firstQueued && affected.IsValid() && unaffected.IsValid()
				&& affectedReceipt.state != Navigation::NavigationAgentReceiptState::Rejected
				&& unaffectedReceipt.state != Navigation::NavigationAgentReceiptState::Rejected
				&& affectedGuidance.IsUsable() && unaffectedGuidance.IsUsable() && replacementQueued,
				"The automatic source refresh regression requires two initial retained agent routes and a coherent replacement source index.");
			context.Expect(!affectedAfterStage.IsUsable() && unaffectedAfterStage.IsUsable()
				&& unaffectedAfterRefresh.IsUsable(),
				"A copied source transition must retire only overlapping guidance while an unaffected retained route refreshes its authoritative evidence.");
			context.Expect(affectedStatus.state == Navigation::NavigationAgentState::Requesting
				|| affectedStatus.state == Navigation::NavigationAgentState::Following,
				"A stale overlapping retained route must release its old handle and enter bounded replacement planning rather than reviving stale guidance.");

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentState.DynamicReceiptRefreshesOnlyUnaffectedAgentAndDirectRoutes", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();

			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> terrainGeometry;
			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> initialGeometry;
			const auto terrain = AutomaticSource(Navigation::NavigationSourceKind::Terrain,
				1U, 0.0, terrainGeometry);
			const auto initial = AutomaticSource(Navigation::NavigationSourceKind::Mesh,
				1U, -10.0, initialGeometry);
			const Navigation::NavigationSystem::InitialGroundBuildInput initialBuild = AutomaticBuild(terrain, initial);
			const bool initialQueued = terrainGeometry != nullptr && initialGeometry != nullptr
				&& initialBuild.eligibleSourceIndex != nullptr && navigation.QueueInitialGroundBuilds({initialBuild});
			navigation.Update(0.0F);

			const Object agentOwner = objects.createObject("Dynamic affected agent owner");
			const Object directOwner = objects.createObject("Dynamic unaffected direct owner");
			(void)objects.AddComponent<Transform>(agentOwner);
			(void)objects.AddComponent<Transform>(directOwner);
			(void)objects.AddComponent<NavigationAgentComponent>(agentOwner);
			Scene::WorldPose affectedPose{};
			affectedPose.position.offsetMetres.x = -12.0;
			Scene::WorldPose unaffectedPose{};
			unaffectedPose.position.offsetMetres.x = 8.0;
			(void)objects.SetCanonicalWorldPose(agentOwner, affectedPose);
			(void)objects.SetCanonicalWorldPose(directOwner, unaffectedPose);
			objects.MarkObjectTransformDirty(agentOwner);
			objects.MarkObjectTransformDirty(directOwner);
			Advance(navigation, agents, 2U);

			const auto world = navigation.PublishedWorld();
			const Navigation::NavigationAgentIdentity agent = agents.FindIdentity(objects, objects.getUUID(agentOwner));
			const auto agentReceipt = world ? agents.QueueLocationDestination(
				agent, Location(world->world, -6.0), 0U, 0U, 1U, 7101U) : Navigation::NavigationAgentReceipt{};
			const auto directReceipt = world ? navigation.RequestRoute(objects, objects.getUUID(directOwner),
				Location(world->world, 14.0), Navigation::NavigationProfile::Defaults())
				: Navigation::NavigationSystem::ObjectRouteReceipt{};
			const auto initialGuidance = WaitForGuidance(navigation, agents, agent);
			for(std::uint32_t frame = 0U; directReceipt.Succeeded() && frame < 32U; ++frame) {
				navigation.Update(0.0F);
				if(navigation.PublishedRouteForRequest(directReceipt.request).IsValid()) break;
			}
			const Navigation::NavigationRouteHandle directRoute = navigation.PublishedRouteForRequest(directReceipt.request);

			std::shared_ptr<const Navigation::NavigationGeometrySnapshot> replacementGeometry;
			const auto replacement = AutomaticSource(Navigation::NavigationSourceKind::Mesh,
				2U, -8.0, replacementGeometry, &initial.identity);
			const Navigation::NavigationSystem::InitialGroundBuildInput replacementBuild = AutomaticBuild(terrain, replacement);
			constexpr Navigation::NavigationGeneration kSequence = 431U;
			Navigation::NavigationSystem::DynamicGroundSourceReplacement staged{};
			if(replacementBuild.sourceIndex.size() > 1U) staged.source = replacementBuild.sourceIndex[1U];
			staged.snapshot = replacementGeometry;
			staged.committedChangeSequence = kSequence;
			const bool stageAccepted = replacementGeometry != nullptr
				&& navigation.StageDynamicGroundSourceReplacements({std::move(staged)});
			Scene::NavigationSourceMutationReceipt receipt{};
			receipt.accepted = true;
			receipt.transactionStatus = Scene::TransactionStatus::Committed;
			receipt.intent.sourceId = replacement.identity.sourceId;
			receipt.intent.operation = Scene::NavigationSourceMutationOperation::Move;
			receipt.intent.oldBounds = {{{0, 0, 0}, {-12.0, -1.0, -2.0}},
				{{0, 0, 0}, {-8.0, 3.0, 2.0}}, true};
			receipt.intent.newBounds = {{{0, 0, 0}, {-10.0, -1.0, -2.0}},
				{{0, 0, 0}, {-6.0, 3.0, 2.0}}, true};
			receipt.intent.committedSourceRevision = replacementBuild.sourceIndex.size() > 1U
				? replacementBuild.sourceIndex[1U].sourceRevision : 0U;
			receipt.intent.committedChangeSequence = kSequence;
			receipt.intent.physicalReadyRevision = 1U;
			receipt.intent.oldEnabled = true;
			receipt.intent.newEnabled = true;
			const bool receiptAccepted = stageAccepted && navigation.ObserveSceneMutationReceipts(
				std::span<const Scene::NavigationSourceMutationReceipt>{&receipt, 1U}) == 1U;
			agents.ObserveProtectedBoundary(Navigation::NavigationProtectedBoundary::FrameEnd, 1U);
			agents.Update(1.0F / 60.0F);
			const auto agentAfterReceipt = agents.ReadGuidance(agent);
			const auto directAfterReceipt = navigation.ValidateRoute(directRoute);
			Advance(navigation, agents, 3U);
			const auto directAfterRebuild = navigation.ValidateRoute(directRoute);
			const auto agentStatus = agents.ReadStatus(agent);

			context.Expect(initialQueued && agent.IsValid() && agentReceipt.state != Navigation::NavigationAgentReceiptState::Rejected
				&& directReceipt.Succeeded() && initialGuidance.IsUsable() && directRoute.IsValid()
				&& stageAccepted && receiptAccepted,
				"The dynamic receipt fixture requires one retained affected agent route and one unrelated direct route.");
			context.Expect(!agentAfterReceipt.IsUsable()
				&& directAfterReceipt.usable && directAfterRebuild.usable,
				"A committed dynamic receipt must stale only the intersecting agent path while refreshing the unaffected direct route through each root publication.");
			context.Expect(agentStatus.state == Navigation::NavigationAgentState::Requesting
				|| agentStatus.state == Navigation::NavigationAgentState::Following,
				"An affected retained agent route must release stale guidance and enter bounded replan publication rather than reviving it.");

			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});
	}
}
