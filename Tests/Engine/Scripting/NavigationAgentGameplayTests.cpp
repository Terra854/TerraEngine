// NavigationAgentGameplayTests.cpp - native gameplay-facing agent contracts.
//
// These tests exercise the protected system boundary with a real terrain
// publication and authored component. Navigation may publish advice, but it
// must never write the owner's transform or allow a stale attachment handle to
// control a replacement object.

#include "../TestHarness.h"

#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometryFormat.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Scripting/ScriptGlue.h"

#include <future>
#include <limits>
#include <string>

namespace TerraEngine::Tests {
	namespace {
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
			if(!terrain || !navigation.QueueInitialGroundBuild(Navigation::NavigationProfile::Defaults(), {terrain}))
				return false;
			navigation.Update(0.0F);
			const auto world = navigation.PublishedWorld();
			return world != nullptr && world->terrainState == Navigation::NavigationTerrainState::Available;
		}

		void Advance(Navigation::NavigationSystem& navigation,
			Navigation::NavigationAgentSystem& agents, std::uint32_t frames = 1U) {
			for(std::uint32_t i = 0U; i < frames; ++i) {
				navigation.Update(1.0F / 60.0F);
				agents.Update(1.0F / 60.0F);
			}
		}
	}

	void RegisterNavigationAgentGameplayTests(TestRegistry& registry) {
		registry.Add("Navigation.AgentGameplay.StopClearsGuidanceWithoutMovingTransform", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();

			const Object owner = objects.createObject("Native Agent Gameplay Owner");
			Transform* transform = objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents);
			const Navigation::NavigationAgentIdentity identity = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto world = navigation.PublishedWorld();
			Navigation::NavigationAgentReceipt receipt{};
			if(terrainReady && world) {
				Navigation::NavigationLocation destination{};
				destination.world = world->world;
				destination.position.offsetMetres = {6.0, 0.0, 0.0};
				receipt = agents.QueueLocationDestination(identity, destination, 0U, 0U, 1U, 7701U);
			}
			Advance(navigation, agents, 8U);
			const Navigation::NavigationAgentStatusSnapshot beforeStop = agents.ReadStatus(identity);
			const Navigation::NavigationAgentReceipt stop = agents.QueueStop(
				identity, beforeStop.configurationRevision, beforeStop.intentSequence, 2U, 7702U);
			Advance(navigation, agents, 2U);
			const auto guidance = agents.ReadGuidance(identity);
			const auto status = agents.ReadStatus(identity);

			context.Expect(terrainReady && receipt.state != Navigation::NavigationAgentReceiptState::Rejected
				&& stop.state != Navigation::NavigationAgentReceiptState::Rejected
				&& status.state == Navigation::NavigationAgentState::Stopped && !guidance.IsUsable(),
				"A native stop command must retire copied guidance at the protected boundary.");
			context.Expect(transform != nullptr && transform->worldPose.position.offsetMetres.x == 0.0
				&& transform->worldPose.position.offsetMetres.y == 0.0
				&& transform->worldPose.position.offsetMetres.z == 0.0,
				"Navigation advice must never directly move the gameplay transform.");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentGameplay.RemovedAndReaddedOwnerRejectsStaleIdentity", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Native Agent Rebind Owner");
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			// ResolveIdentity reads the protected published attachment snapshot, so
			// publish the newly created component before freezing the ABI identity.
			navigation.Update(1.0F / 60.0F);
			agents.Update(1.0F / 60.0F);
			NativeNavigationAgentResolveRequest resolve{};
			resolve.header = {1U, sizeof(resolve)};
			resolve.ownerObjectEntity = static_cast<std::uint32_t>(owner.EntityId());
			NativeNavigationAgentIdentitySnapshot nativeIdentity{};
			nativeIdentity.header = {1U, sizeof(nativeIdentity)};
			const NativeNavigationStatus resolveStatus = Native_NavigationAgent_ResolveIdentity(&resolve, &nativeIdentity);
			const auto stale = agents.FindIdentity(objects, objects.getUUID(owner));
			objects.RemoveComponent<NavigationAgentComponent>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			navigation.Update(1.0F / 60.0F);
			agents.Update(1.0F / 60.0F);
			const auto fresh = agents.FindIdentity(objects, objects.getUUID(owner));
			const auto staleReceipt = agents.QueueStop(stale, 0U, 0U, 3U, 7703U);
			agents.Update(1.0F / 60.0F);
			const auto staleResult = agents.ReadReceipt(stale, 7703U);
			NativeNavigationAgentStatusQuery staleQuery{};
			staleQuery.header = {1U, sizeof(staleQuery)};
			staleQuery.identity = nativeIdentity.identity;
			NativeNavigationAgentStatusSnapshot staleSnapshot{};
			staleSnapshot.header = {1U, sizeof(staleSnapshot)};
			const NativeNavigationStatus staleStatus = Native_NavigationAgent_ReadStatus(&staleQuery, &staleSnapshot);
			context.Expect(stale.IsValid() && fresh.IsValid() && stale != fresh
				&& resolveStatus == NativeNavigationStatus::Success
				&& staleReceipt.state != Navigation::NavigationAgentReceiptState::Rejected
				&& staleResult.has_value() && staleResult->state == Navigation::NavigationAgentReceiptState::Rejected,
				"Removing and re-adding an owner must reject the old generation-qualified native identity.");
			context.Expect(staleStatus == NativeNavigationStatus::StaleTarget
				&& staleSnapshot.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Stale)
				&& staleSnapshot.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::SceneReplaced),
				"The v1 status ABI must expose stale identity rejection after the component generation changes (status="
				+ std::to_string(static_cast<std::uint32_t>(staleStatus)) + ", outcome="
				+ std::to_string(staleSnapshot.outcome) + ", reason="
				+ std::to_string(staleSnapshot.reason) + ").");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});

		registry.Add("Navigation.AgentGameplay.NativeAbiCommandIsDeferredUntilCaptureAndPublishesGuidance", [](TestContext& context) {
			ComponentRegistry::RegisterAllComponents();
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Navigation::NavigationSystem& navigation = Navigation::NavigationSystem::Instance();
			Navigation::NavigationAgentSystem& agents = Navigation::NavigationAgentSystem::Instance();
			navigation.Shutdown();
			agents.Shutdown();
			navigation.Initialize();
			agents.Initialize();
			const Object owner = objects.createObject("Native Agent ABI Owner");
			(void)objects.AddComponent<Transform>(owner);
			(void)objects.AddComponent<NavigationAgentComponent>(owner);
			const bool terrainReady = PrepareTerrain(navigation);
			Advance(navigation, agents, 2U);

			NativeNavigationAgentResolveRequest resolve{};
			resolve.header = {1U, sizeof(resolve)};
			resolve.ownerObjectEntity = static_cast<std::uint32_t>(owner.EntityId());
			NativeNavigationAgentIdentitySnapshot identitySnapshot{};
			identitySnapshot.header = {1U, sizeof(identitySnapshot)};
			const NativeNavigationStatus resolveStatus = Native_NavigationAgent_ResolveIdentity(&resolve, &identitySnapshot);
			NativeNavigationAgentCommand command{};
			command.header = {1U, sizeof(command)};
			command.identity = identitySnapshot.identity;
			command.kind = NativeNavigationAgentCommandKind::SetLocationDestination;
			const auto publishedWorld = navigation.PublishedWorld();
			command.location.world.worldId = publishedWorld ? publishedWorld->world.worldId.value : 0U;
			command.location.world.worldGeneration = publishedWorld ? publishedWorld->world.worldGeneration : 0U;
			command.location.offsetX = 6.0;
			command.producerOrderingToken = 11U;
			command.correlationId = 8801U;
			NativeNavigationAgentCommand malformed = command;
			malformed.location.offsetX = std::numeric_limits<double>::quiet_NaN();
			malformed.correlationId = 8798U;
			NativeNavigationAgentCommandReceipt malformedReceipt{};
			malformedReceipt.header = {1U, sizeof(malformedReceipt)};
			const NativeNavigationStatus malformedStatus =
				Native_NavigationAgent_SubmitCommand(&malformed, &malformedReceipt);
			malformed = command;
			malformed.kind = static_cast<NativeNavigationAgentCommandKind>(99U);
			malformed.correlationId = 8799U;
			malformedReceipt = {};
			malformedReceipt.header = {1U, sizeof(malformedReceipt)};
			const NativeNavigationStatus unknownKindStatus =
				Native_NavigationAgent_SubmitCommand(&malformed, &malformedReceipt);
			malformed = command;
			malformed.targetObjectEntity = 1234U;
			malformed.correlationId = 8800U;
			malformedReceipt = {};
			malformedReceipt.header = {1U, sizeof(malformedReceipt)};
			const NativeNavigationStatus nonzeroTargetStatus =
				Native_NavigationAgent_SubmitCommand(&malformed, &malformedReceipt);
			NativeNavigationAgentCommandReceipt submitted{};
			submitted.header = {1U, sizeof(submitted)};
			const NativeNavigationStatus submitStatus = Native_NavigationAgent_SubmitCommand(&command, &submitted);

			NativeNavigationAgentStatusQuery statusQuery{};
			statusQuery.header = {1U, sizeof(statusQuery)};
			statusQuery.identity = identitySnapshot.identity;
			NativeNavigationAgentStatusSnapshot beforeCapture{};
			beforeCapture.header = {1U, sizeof(beforeCapture)};
			(void)Native_NavigationAgent_ReadStatus(&statusQuery, &beforeCapture);
			context.Expect(terrainReady && resolveStatus == NativeNavigationStatus::Success
				&& malformedStatus == NativeNavigationStatus::InvalidArgument
				&& unknownKindStatus == NativeNavigationStatus::InvalidArgument
				&& nonzeroTargetStatus == NativeNavigationStatus::InvalidArgument
				&& submitStatus == NativeNavigationStatus::Accepted
				&& submitted.receiptState == static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Queued),
				"Native ABI commands must resolve and queue copied intent before protected capture.");

			NativeNavigationAgentCommandReceipt applied{};
			applied.header = {1U, sizeof(applied)};
			NativeNavigationAgentReceiptQuery receiptQuery{};
			receiptQuery.header = {1U, sizeof(receiptQuery)};
			receiptQuery.identity = identitySnapshot.identity;
			receiptQuery.correlationId = command.correlationId;
			Advance(navigation, agents, 4U);
			(void)Native_NavigationAgent_ReadReceipt(&receiptQuery, &applied);
			NativeNavigationAgentReceiptQuery expiredQuery = receiptQuery;
			expiredQuery.correlationId = 8899U;
			NativeNavigationAgentCommandReceipt expiredReceipt{};
			expiredReceipt.header = {1U, sizeof(expiredReceipt)};
			const NativeNavigationStatus expiredStatus = Native_NavigationAgent_ReadReceipt(
				&expiredQuery, &expiredReceipt);

			NativeNavigationAgentGuidanceQuery guidanceQuery{};
			guidanceQuery.header = {1U, sizeof(guidanceQuery)};
			guidanceQuery.identity = identitySnapshot.identity;
			NativeNavigationAgentGuidanceSnapshot guidance{};
			guidance.header = {1U, sizeof(guidance)};
			for(std::uint32_t frame = 0U; frame < 64U && guidance.usable == 0U; ++frame) {
				(void)Native_NavigationAgent_ReadGuidance(&guidanceQuery, &guidance);
				if(guidance.usable == 0U) Advance(navigation, agents);
			}
			context.Expect(applied.receiptState == static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Applied)
				&& guidance.status == NativeNavigationStatus::Success && guidance.usable != 0U,
				"After capture and route publication, native status/receipt/guidance reads must expose copied gameplay data.");
			context.Expect(expiredStatus == NativeNavigationStatus::Expired
				&& expiredReceipt.receiptState == static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Expired)
				&& expiredReceipt.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Expired)
				&& expiredReceipt.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::Expired),
				"A live identity with an unknown correlation must return a native Expired receipt rather than an empty success packet (status="
				+ std::to_string(static_cast<std::uint32_t>(expiredStatus)) + ", state="
				+ std::to_string(expiredReceipt.receiptState) + ", outcome="
				+ std::to_string(expiredReceipt.outcome) + ", reason="
				+ std::to_string(expiredReceipt.reason) + ").");

			NativeNavigationAgentCommand stop = command;
			stop.kind = NativeNavigationAgentCommandKind::Stop;
			stop.location = {};
			stop.expectedConfigurationRevision = 0U;
			stop.expectedIntentSequence = 0U;
			stop.correlationId = 8802U;
			NativeNavigationAgentCommand workerDestination = command;
			workerDestination.expectedConfigurationRevision = 0U;
			workerDestination.expectedIntentSequence = 0U;
			workerDestination.correlationId = 8803U;
			auto workerSubmit = std::async(std::launch::async, [workerDestination]() {
				NativeNavigationAgentCommandReceipt receipt{};
				receipt.header = {1U, sizeof(receipt)};
				return std::pair{Native_NavigationAgent_SubmitCommand(&workerDestination, &receipt), receipt};
			});
			const auto workerResult = workerSubmit.get();
			// Queue Stop before the next protected capture. Zero preconditions leave
			// ordering to the queue: the preceding destination increments the intent,
			// so repeating its old nonzero precondition would correctly reject Stop.
			NativeNavigationAgentCommand sameFrameStop = stop;
			sameFrameStop.correlationId = 8804U;
			NativeNavigationAgentCommandReceipt sameFrameStopReceipt{};
			sameFrameStopReceipt.header = {1U, sizeof(sameFrameStopReceipt)};
			const NativeNavigationStatus sameFrameStopStatus = Native_NavigationAgent_SubmitCommand(
				&sameFrameStop, &sameFrameStopReceipt);
			NativeNavigationAgentStatusSnapshot beforeStopCapture{};
			beforeStopCapture.header = {1U, sizeof(beforeStopCapture)};
			(void)Native_NavigationAgent_ReadStatus(&statusQuery, &beforeStopCapture);
			Advance(navigation, agents, 2U);
			NativeNavigationAgentStatusSnapshot afterStopCapture{};
			afterStopCapture.header = {1U, sizeof(afterStopCapture)};
			(void)Native_NavigationAgent_ReadStatus(&statusQuery, &afterStopCapture);
			NativeNavigationAgentGuidanceSnapshot stoppedGuidance{};
			stoppedGuidance.header = {1U, sizeof(stoppedGuidance)};
			(void)Native_NavigationAgent_ReadGuidance(&guidanceQuery, &stoppedGuidance);
			context.Expect(workerResult.first == NativeNavigationStatus::Accepted
				&& sameFrameStopStatus == NativeNavigationStatus::Accepted
				&& beforeCapture.intentSequence == 0U
				&& beforeStopCapture.intentSequence == guidance.intentSequence
				&& afterStopCapture.state == static_cast<std::uint32_t>(Navigation::NavigationAgentState::Stopped)
				&& stoppedGuidance.usable == 0U,
				"Worker-submitted native commands remain invisible until capture, then stop clears published guidance.");

			NativeNavigationAgentCommand supersededFirst = command;
			supersededFirst.correlationId = 8810U;
			NativeNavigationAgentCommand supersededSecond = command;
			supersededSecond.correlationId = 8811U;
			NativeNavigationAgentCommandReceipt supersededFirstReceipt{};
			supersededFirstReceipt.header = {1U, sizeof(supersededFirstReceipt)};
			NativeNavigationAgentCommandReceipt supersededSecondReceipt{};
			supersededSecondReceipt.header = {1U, sizeof(supersededSecondReceipt)};
			const NativeNavigationStatus supersededFirstStatus = Native_NavigationAgent_SubmitCommand(
				&supersededFirst, &supersededFirstReceipt);
			const NativeNavigationStatus supersededSecondStatus = Native_NavigationAgent_SubmitCommand(
				&supersededSecond, &supersededSecondReceipt);
			NativeNavigationAgentReceiptQuery supersededQuery{};
			supersededQuery.header = {1U, sizeof(supersededQuery)};
			supersededQuery.identity = identitySnapshot.identity;
			supersededQuery.correlationId = supersededFirst.correlationId;
			NativeNavigationAgentCommandReceipt supersededRead{};
			supersededRead.header = {1U, sizeof(supersededRead)};
			Advance(navigation, agents, 2U);
			const NativeNavigationStatus supersededReadStatus = Native_NavigationAgent_ReadReceipt(
				&supersededQuery, &supersededRead);
			context.Expect(supersededFirstStatus == NativeNavigationStatus::Accepted
				&& supersededSecondStatus == NativeNavigationStatus::Accepted
				&& supersededReadStatus == NativeNavigationStatus::Success
				&& supersededRead.receiptState == static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Superseded)
				&& supersededRead.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::Cancelled)
				&& supersededRead.reason == static_cast<std::uint32_t>(Navigation::NavigationReason::SupersededRequest),
				"The native agent ABI must expose the first same-frame destination as Superseded after coalescing.");

			NativeNavigationAgentCommand capacityCommand = stop;
			NativeNavigationAgentCommandReceipt capacityReceipt{};
			capacityReceipt.header = {1U, sizeof(capacityReceipt)};
			for(std::uint64_t index = 0U; index < 8192U; ++index) {
				capacityCommand.correlationId = 900000U + index;
				(void)Native_NavigationAgent_SubmitCommand(&capacityCommand, &capacityReceipt);
			}
			capacityCommand.correlationId = 999999U;
			capacityReceipt = {};
			capacityReceipt.header = {1U, sizeof(capacityReceipt)};
			const NativeNavigationStatus capacityStatus = Native_NavigationAgent_SubmitCommand(
				&capacityCommand, &capacityReceipt);
			context.Expect(capacityStatus == NativeNavigationStatus::LimitExceeded
				&& capacityReceipt.receiptState == static_cast<std::uint32_t>(Navigation::NavigationAgentReceiptState::Rejected)
				&& capacityReceipt.outcome == static_cast<std::uint32_t>(Navigation::NavigationOutcome::RejectedCapacity),
				"The native agent ABI must return LimitExceeded/RejectedCapacity when its bounded command queue is full.");
			objects.clear();
			agents.Shutdown();
			navigation.Shutdown();
		});
	}
}
