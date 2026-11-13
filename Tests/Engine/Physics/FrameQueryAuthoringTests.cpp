// Real authoring packets, Jolt character ticks and frame-owned query lifecycle.
#include "TestHarness.h"
#include "Components/Physics/ColliderSetComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Physics/ConstraintSetComponent.h"
#include "Components/Physics/RigidBodyComponent.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Physics/Backend/JoltPhysicsBackend.h"
#include "Physics/PhysicsSystem.h"
#include "Physics/PhysicsReviewTestAccess.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"
#include "ThreadPool/ScopedTaskSettlement.h"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>
#include <cmath>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Physics;
		using namespace TerraEngine::Physics::Backend;
		struct PhysicsFixture
		{
			PhysicsSystem& system = PhysicsSystem::Instance();
			ObjectManager& objects = ObjectManager::instance();
			PhysicsSettings previousSettings = system.ConfiguredSettings();
			std::size_t previousWorkers = ThreadPool::instance().getThreadCount();
			void Initialize(TestContext& context)
			{
				system.Shutdown(); objects.clear(); TerraEngine::Scene::SceneMutationQueue::Instance().Clear();
				ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2U);
				ComponentRegistry::RegisterAllComponents(); system.SetEditorLifecycleControlled(true); system.Initialize();
				context.Expect(system.StartWorld(PhysicsSettings::Defaults()), "The actual physics authoring fixture must start its provider world with isolated default timing.");
			}
			void Tick()
			{
				// Match the production protected snapshot and typed publication flush.
				(void)objects.PublishSceneChangePacket(); system.Update(system.FixedTimeStep());
				const auto receipt = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "frame-query-review");
				system.OnAfterPhysicsFlush(receipt.transactionResults, receipt.publicationGroupResults);
			}
			void ScheduledTick(TestContext& context, float elapsed, bool expectAllPhases = true,
				std::function<void()> beforeFlush = {}) {
				(void)objects.PublishSceneChangePacket();
				auto work = system.DeclareFrameWorkloads(elapsed);
				context.Expect(work.size() == 7U, "The actual physics scheduler contract must retain its seven existing phases.");
				if(expectAllPhases) context.Expect(std::all_of(work.begin(), work.end(), [](const auto& item) { return item.active; }),
					"A secondary-only tick must activate snapshot, preparation, commit, simulate, extract, publish and barrier.");
				const auto plan = TerraEngine::Scheduling::ExecuteFrameWorkloads(std::move(work), "physics-secondary-review");
				context.Expect(plan.valid && plan.completed, "The real declared workload DAG must execute successfully.");
				if(beforeFlush) beforeFlush();
				const auto receipt = TerraEngine::Scene::SceneMutationQueue::Instance().Apply(objects, "frame-query-review");
				system.OnAfterPhysicsFlush(receipt.transactionResults, receipt.publicationGroupResults);
			}
			Object Body(TestContext& context, const char* name, double x)
			{
				Object object = objects.createObject(name);
				auto* rigid = objects.AddComponent<RigidBodyComponent>(object);
				auto* collider = objects.AddComponent<ColliderSetComponent>(object);
				context.Expect(rigid && collider, "The fixture needs real authored body and collider components.");
				rigid->useGravity = false; collider->Add();
				auto pose = objects.GetCanonicalWorldPose(object);
				context.Expect(pose.has_value(), "A provider object must have a canonical pose before authoring capture.");
				pose->position.offsetMetres = {x, 0.0, 0.0};
				context.Expect(objects.SetCanonicalWorldPose(object, *pose) == TerraEngine::Scene::WorldCoordinateError::None,
					"The fixture must publish a canonical body placement.");
				return object;
			}
			~PhysicsFixture()
			{
				// Settle any queued policy through the public protected lifecycle path,
				// then restore the incoming project policy before stopping this owner.
				// No Update/step is performed during cleanup, including exceptional exits.
				(void)system.StartWorld(); (void)system.StartWorld(previousSettings);
				(void)system.StopWorld(); system.Shutdown(); system.SetEditorLifecycleControlled(false);
				TerraEngine::Scene::SceneMutationQueue::Instance().Clear(); objects.clear();
				ThreadPool::instance().shutdown(); if(previousWorkers != 0U) ThreadPool::instance().initialize(previousWorkers);
			}
		};
	}

	void RegisterFrameQueryAuthoringTests(TestRegistry& registry)
	{
		registry.Add("Physics.FrameQuery.SecondaryAdmissionRetriesUnchangedGeneration", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto body = fixture.Body(context, "One-shot admission retry", 1000.0);
			PhysicsFrameCreateRequest create; create.frameId = {93U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(body), frame->frame)
				&& PhysicsReviewTestAccess::ArmSecondaryFailure(fixture.system, frame->frame, true), "Arm after assigning a real secondary owner.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			const auto rejected = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, frame->frame);
			context.Expect(rejected && rejected->runtimeBodies == 0U && rejected->providerBodies == 0U && rejected->typedBodies == 0U,
				"Failed staged admission must roll back every ownership layer.");
			// No scene, component, settings or asset edit is made before the retry.
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			const auto accepted = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, frame->frame);
			context.Expect(accepted && accepted->runtimeBodies == 1U && accepted->providerBodies == 1U && accepted->typedBodies == 1U,
				"The unchanged generation must retry and admit its rejected secondary packet.");
		});
		registry.Add("Physics.FrameQuery.SecondaryStepFailureStopsLaterRequests", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			PhysicsFrameCreateRequest create; create.frameId = {94U};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::ArmSecondaryFailure(fixture.system, frame->frame, false, 2U), "Arm the second request failure.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep() * 3.1F);
			const auto clock = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
			const auto attempts = PhysicsReviewTestAccess::ReadSecondaryStepAttempts(fixture.system, frame->frame);
			context.Expect(clock && clock->issued >= 3U && clock->completed == 1U && attempts && *attempts == 2U,
				"The first real provider step completes; the injected second failure prevents the third step.");
		});
		registry.Add("Physics.FrameQuery.ParentedBodyAndCharacterPublishWorldPoseOnce", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto parent = fixture.objects.createObject("Translated rotated parent");
			auto parentPose = *fixture.objects.GetCanonicalWorldPose(parent);
			parentPose.position.offsetMetres = {40.0, 5.0, -20.0};
			parentPose.rotation = glm::angleAxis(0.6F, glm::vec3(0.0F, 1.0F, 0.0F));
			context.Expect(fixture.objects.SetCanonicalWorldPose(parent, parentPose) == Scene::WorldCoordinateError::None, "Establish a real nonidentity parent pose.");
			const auto body = fixture.Body(context, "Parented dynamic body", 42.0);
			const auto character = fixture.objects.createObject("Parented virtual character");
			auto* controller = fixture.objects.AddComponent<CharacterControllerComponent>(character);
			context.Expect(controller != nullptr, "The virtual character must use the real authoring/provider path.");
			controller->useGravity = false;
			auto characterPose = *fixture.objects.GetCanonicalWorldPose(character);
			characterPose.position.offsetMetres = {50.0, 0.0, 0.0};
			context.Expect(fixture.objects.SetCanonicalWorldPose(character, characterPose) == Scene::WorldCoordinateError::None
				&& fixture.objects.setParent(body, parent) && fixture.objects.setParent(character, parent), "Keep-world parenting establishes distinct provider positions.");
			const auto beforeBody = *fixture.objects.GetCanonicalWorldPose(body);
			const auto beforeCharacter = *fixture.objects.GetCanonicalWorldPose(character);
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep(), true, [&] {
				context.Expect(fixture.objects.GetCanonicalWorldPose(body)->position == beforeBody.position
					&& fixture.objects.GetCanonicalWorldPose(character)->position == beforeCharacter.position, "Prepared provider output remains invisible until the protected scene flush.");
			});
			const auto expectPosition = [&](Object object, const Scene::WorldPose& expected) {
				const auto actual = *fixture.objects.GetCanonicalWorldPose(object);
				const auto localized = Scene::LocalizeWorldPosition(actual.position, expected.position, 1000.0);
				context.Expect(localized.Succeeded() && glm::length(localized.offsetMetres) < 0.001, "A resting parented provider must publish its original world position, with parent conversion applied exactly once.");
			};
			expectPosition(body, beforeBody); expectPosition(character, beforeCharacter);
		});
		registry.Add("Physics.FrameQuery.MembershipIngressInvalidatesAnOtherwiseReusableSnapshot", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.objects.createObject("Unadmitted membership fixture");
			PhysicsFrameCreateRequest create; create.frameId = {83U};
			const auto destination = fixture.system.CreateFrame(create);
			context.Expect(destination.has_value(), "Create a real destination frame before capturing the baseline.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep(), false); fixture.ScheduledTick(context, fixture.system.FixedTimeStep(), false);
			context.Expect(fixture.system.LatestDiagnostics() && fixture.system.LatestDiagnostics()->authoring.snapshotReused, "An empty exact scene packet must establish the actual reuse fast path.");
			PhysicsFrameMigrationRequest request; request.id = "cached-membership-ingress";
			request.destinationFrame = destination->frame; request.expectedDestinationResidencyGeneration = 1U;
			PhysicsFrameMigrationMember member; member.objectId = fixture.objects.getUUID(object);
			member.sourceMembership = {{kPrimaryPhysicsFrameId, fixture.system.WorldHandle().worldGeneration}, fixture.system.WorldHandle(), 1U};
			member.canonicalPose = *fixture.objects.GetCanonicalWorldPose(object); request.members.push_back(member);
			// This owner has no admitted native body. Callbacks exercise membership
			// ingress only; they do not stand in for a native body transfer test.
			const auto refused = fixture.system.CommitFrameMigration(request, {
				.stageDestination = [](const auto&) { return false; },
				.commitDestinationAndReleaseSource = [](const auto&) { return true; },
				.rollbackDestination = [](const auto&) {}});
			context.Expect(!refused.accepted, "Rejected ingress must leave the cached authority intact.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep(), false);
			context.Expect(fixture.system.LatestDiagnostics() && fixture.system.LatestDiagnostics()->authoring.snapshotReused, "Rejected ingress cannot invalidate an unchanged snapshot.");
			const auto accepted = fixture.system.CommitFrameMigration(request, {
				.stageDestination = [](const auto&) { return true; },
				.commitDestinationAndReleaseSource = [](const auto&) { return true; },
				.rollbackDestination = [](const auto&) {}});
			context.Expect(accepted.accepted, "Accept the real protected membership producer without editing the scene.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep(), false);
			context.Expect(fixture.system.LatestDiagnostics() && !fixture.system.LatestDiagnostics()->authoring.snapshotReused, "Accepted membership ingress must recapture even when the scene packet is empty.");
		});
		registry.Add("Physics.FrameQuery.LiveMigrationRefusesBeforeCallbacksAndRetainsAllOwnerLayers", [](TestContext& context) {
			// Exercise both actual source providers. A callback's success claim must
			// never replace the system-owned body or expose duplicate pose writers.
			for(const bool secondary : {false, true}) {
				PhysicsFixture fixture; fixture.Initialize(context);
				const auto body = fixture.Body(context, "Retained migration source", 10.0);
				PhysicsFrameCreateRequest create; create.frameId = {701U};
				const auto source = fixture.system.CreateFrame(create);
				create.frameId = {702U}; const auto destination = fixture.system.CreateFrame(create);
				context.Expect(source && destination, "Both isolated frame providers must exist.");
				if(secondary) context.Expect(PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system,
					fixture.objects.getUUID(body), source->frame), "Assign the secondary source before native admission.");
				fixture.Tick();
				const PhysicsFrameHandle sourceFrame = secondary ? source->frame
					: PhysicsFrameHandle{kPrimaryPhysicsFrameId, fixture.system.WorldHandle().worldGeneration};
				const auto before = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, sourceFrame);
				const auto primaryBefore = PhysicsReviewTestAccess::ReadTerrainShapeOwnership(fixture.system);
				PhysicsFrameMigrationRequest request; request.id = "refuse-live-source";
				request.destinationFrame = destination->frame; request.expectedDestinationResidencyGeneration = 2U;
				PhysicsFrameMigrationMember member; member.objectId = fixture.objects.getUUID(body);
				member.sourceMembership = {sourceFrame, secondary ? source->world : fixture.system.WorldHandle(), 1U};
				member.canonicalPose = *fixture.objects.GetCanonicalWorldPose(body); request.members.push_back(member);
				unsigned callbacks = 0;
				const auto receipt = fixture.system.CommitFrameMigration(request, {
					.stageDestination = [&](const auto&) { ++callbacks; return true; },
					.commitDestinationAndReleaseSource = [&](const auto&) { ++callbacks; return true; },
					.rollbackDestination = [&](const auto&) { ++callbacks; }});
				context.Expect(!receipt.accepted && receipt.state == PhysicsFrameMigrationState::PreflightFailed && callbacks == 0,
					"A live transfer must reject before any producer callback or partial mutation.");
				fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
				const auto after = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, sourceFrame);
				const auto target = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, destination->frame);
				context.Expect(target && target->runtimeBodies == 0 && target->typedBodies == 0 && target->providerBodies == 0,
					"Rejected migration cannot admit a destination ghost at the next protected reconciliation.");
				if(secondary) context.Expect(before && after && before->runtimeBodies == 1 && after->runtimeBodies == 1
					&& after->mappings == 1 && after->typedBodies == 1 && after->typedShapes == 1
					&& after->providerBodies == 1 && after->providerShapes == 1,
					"Secondary source runtime maps, typed handles and provider records must remain intact.");
				else {
					// ReadFrameBodyOwnership intentionally inspects secondary frames.
					// Use the primary provider/registry observation plus an actual query
					// to prove the primary body-to-object mapping remains authoritative.
					const auto primaryAfter = PhysicsReviewTestAccess::ReadTerrainShapeOwnership(fixture.system);
					PhysicsQueryRequest query; query.world = fixture.system.WorldHandle();
					query.pose.position = {7.0F, 0.0F, 0.0F}; query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 6.0F;
					const auto hit = fixture.system.QueryImmediate(query);
					context.Expect(primaryBefore && primaryAfter && primaryBefore->typedLiveBodies == 1
						&& primaryAfter->typedLiveBodies == 1 && primaryAfter->typedLiveShapes == 1
						&& primaryAfter->providerBodyRecords == 1 && primaryAfter->providerShapeRecords == 1
						&& hit.hits && hit.hits->size() == 1 && hit.hits->front().objectId == member.objectId,
						"Primary typed/native ownership and queried object identity must remain unchanged.");
				}
			}
		});
		registry.Add("Physics.FrameQuery.SecondaryRemovalRetiresEveryOwnerAfterProtectedVisibility", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto removed = fixture.Body(context, "Secondary removal", 1000.0);
			const auto retained = fixture.Body(context, "Other frame survivor", 2000.0);
			PhysicsFrameCreateRequest create; create.frameId = {97U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto first = fixture.system.CreateFrame(create);
			create.frameId = {98U}; create.canonicalOrigin.offsetMetres = {2000.0, 0.0, 0.0};
			const auto second = fixture.system.CreateFrame(create);
			context.Expect(first && second
				&& PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(removed), first->frame)
				&& PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(retained), second->frame), "Assign actual independent secondary owners.");
			fixture.Tick();
			const auto expectCount = [&](PhysicsFrameHandle frame, std::size_t count) {
				const auto owned = PhysicsReviewTestAccess::ReadFrameBodyOwnership(fixture.system, frame);
				context.Expect(owned && owned->runtimeBodies == count && owned->mappings == count && owned->typedBodies == count
					&& owned->typedShapes == count && owned->providerBodies == count && owned->providerShapes == count,
					"Runtime, reverse map, typed registries and native provider must agree on exact body/shape ownership.");
			};
			expectCount(first->frame, 1U); expectCount(second->frame, 1U);
			Scene::SceneMutationQueue queue;
			Scene::SceneMutationRequest request; request.operation = Scene::SceneMutationOperation::DeleteObject;
			request.targetObjectId = fixture.objects.getUUID(removed); request.expectedGraphRevision = UINT64_MAX;
			queue.Enqueue(request);
			context.Expect(!queue.Apply(fixture.objects, "Rejected deletion").applied, "A stale delete must reject atomically.");
			fixture.Tick(); expectCount(first->frame, 1U);
			queue.Clear(); request.expectedGraphRevision = 0U; queue.Enqueue(request);
			fixture.Tick(); expectCount(first->frame, 1U);
			context.Expect(queue.Apply(fixture.objects, "Accepted deletion").applied, "The declared scene flush publishes the accepted deletion.");
			// Native collision ownership changes at the next protected physics
			// reconciliation, never during worker ingress or the scene-only flush.
			expectCount(first->frame, 1U);
			fixture.Tick(); expectCount(first->frame, 0U); expectCount(second->frame, 1U);
			PhysicsQueryRequest query; query.world = first->world; query.frame = first->frame;
			query.pose.position = {-2.0F, 0.0F, 0.0F}; query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 4.0F;
			const auto result = fixture.system.QueryImmediate(query);
			context.Expect(result.hits && result.hits->empty(), "Deleted native bodies must no longer participate in real frame-local queries.");
		});
		registry.Add("Physics.FrameQuery.ZeroVelocityCapsRejectBeforeProviderAdmission", [](TestContext& context) {
			RigidBodyComponent body;
			context.Expect(body.Validate().accepted, "Default calibrated velocity caps must be accepted.");
			body.maxLinearVelocity = 0.0F;
			context.Expect(body.Validate().code == RigidBodyValidationCode::InvalidVelocityLimit && !body.Validate().accepted, "Zero linear cap rejects in authoring validation.");
			body = RigidBodyComponent{}; body.maxAngularVelocity = 0.0F;
			context.Expect(body.Validate().code == RigidBodyValidationCode::InvalidVelocityLimit && !body.Validate().accepted, "Zero angular cap rejects in authoring validation.");
		});
		registry.Add("Physics.FrameQuery.PendingSettingsRateChangeCannotDropPreparedTick", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Settings-rate actor", 0.0);
			fixture.objects.GetComponent<RigidBodyComponent>(object)->initialLinearVelocity = {1.0F, 0.0F, 0.0F};
			fixture.ScheduledTick(context, 0.0F, false);
			const auto oldWorld = fixture.system.WorldHandle();
			const auto oldPose = *fixture.objects.GetCanonicalWorldPose(object);
			auto settings = fixture.system.ConfiguredSettings(); settings.fixedTimeStep *= 0.5F;
			context.Expect(fixture.system.QueueSettingsUpdate(settings), "Queue a genuine protected settings change to a faster primary rate.");
			for(unsigned inspect = 0U; inspect != 3U; ++inspect) (void)fixture.system.DeclareFrameWorkloads(settings.fixedTimeStep);
			const auto oldClock = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, {kPrimaryPhysicsFrameId, oldWorld.worldGeneration});
			context.Expect(fixture.system.WorldHandle() == oldWorld && oldClock && oldClock->issued == 0U,
				"Declaration cannot apply pending settings or consume the future clock's first tick.");
			fixture.ScheduledTick(context, settings.fixedTimeStep, true, [&] {
				context.Expect(fixture.objects.GetCanonicalWorldPose(object)->position == oldPose.position,
					"The new-rate pose must remain deferred until the existing protected flush.");
			});
			const auto newWorld = fixture.system.WorldHandle();
			const auto advanced = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, {kPrimaryPhysicsFrameId, newWorld.worldGeneration});
			context.Expect(newWorld != oldWorld && advanced && advanced->issued == 1U && advanced->completed == 1U,
				"A settings-created first tick must not disappear because declaration saw the old slower rate.");
			context.Expect(fixture.system.SetPaused(true), "Pause before the next protected policy replacement.");
			++settings.positionIterations; context.Expect(fixture.system.QueueSettingsUpdate(settings), "Queue paused settings without requesting a tick.");
			fixture.ScheduledTick(context, 0.0F);
			const auto paused = PhysicsReviewTestAccess::ReadFrameClock(fixture.system,
				{kPrimaryPhysicsFrameId, fixture.system.WorldHandle().worldGeneration});
			context.Expect(fixture.system.IsPaused() && paused && paused->issued == 0U && paused->completed == 0U,
				"Conservative settings-transition activation must not manufacture a paused-world step.");
		});
		registry.Add("Physics.FrameQuery.SecondaryRejectedAuthoredPacketRetainsBothPriorBodies", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto first = fixture.Body(context, "Retained packet member", 1000.0);
			const auto second = fixture.Body(context, "Rejected packet member", 1005.0);
			PhysicsFrameCreateRequest create; create.frameId = {87U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(first), frame->frame)
				&& PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(second), frame->frame), "Assign both actual staged packet members.");
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			const auto read = [&](float x) {
				PhysicsQueryRequest query; query.world = frame->world; query.frame = frame->frame;
				query.pose.position = {x - 2.0F, 0.0F, 0.0F}; query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 4.0F;
				const auto result = fixture.system.QueryImmediate(query);
				context.Expect(result.hits && result.hits->size() == 1U, "Each prior packet member must have one real native body.");
				return result.hits->front().body;
			};
			const auto firstHandle = read(0.0F), secondHandle = read(5.0F);
			const auto firstRevision = fixture.objects.GetObjectTransformRevision(first), secondRevision = fixture.objects.GetObjectTransformRevision(second);
			// An unavailable material passes structural authoring validation and
			// reaches the actual secondary admission failure/retry boundary.
			// The other changed member is valid: neither candidate may replace its old
			// body when this complete provider packet cannot commit atomically.
			fixture.objects.GetComponent<RigidBodyComponent>(first)->maxLinearVelocity = 2.0F;
			auto& collider = fixture.objects.GetComponent<ColliderSetComponent>(second)->colliders.front();
			collider.useDefaultMaterial = false; collider.materialId = TerraEngine::GenerateUUID();
			fixture.objects.notifyObjectStructureChanged(first); fixture.objects.notifyObjectStructureChanged(second);
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			context.Expect(read(0.0F) == firstHandle && read(5.0F) == secondHandle
				&& fixture.objects.GetObjectTransformRevision(first) == firstRevision
				&& fixture.objects.GetObjectTransformRevision(second) == secondRevision,
				"Rejected full authored admission must retain both prior provider identities and unchanged scene publication.");
			// A permanently invalid authored batch must not keep preparing forever.
			for(unsigned attempt = 0; attempt < 3; ++attempt) fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			const auto rejectedCount = fixture.system.LatestDiagnostics()->counters.rejectedCount;
			for(unsigned frameIndex = 0; frameIndex < 8; ++frameIndex) fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			context.Expect(fixture.system.LatestDiagnostics()->counters.rejectedCount == rejectedCount
				&& read(0.0F) == firstHandle && read(5.0F) == secondHandle,
				"An unchanged invalid batch must suspend after bounded retries while retaining prior owners.");
			auto& repairedCollider = fixture.objects.GetComponent<ColliderSetComponent>(second)->colliders.front();
			repairedCollider.useDefaultMaterial = true; repairedCollider.materialId = TerraEngine::EMPTY_UUID;
			// Both members receive actual new authored input. A metadata-only notify
			// is not a new semantic body operation in the incremental preparation plan.
			fixture.objects.GetComponent<RigidBodyComponent>(first)->maxLinearVelocity = 3.0F;
			fixture.objects.notifyObjectStructureChanged(first); fixture.objects.notifyObjectStructureChanged(second);
			fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			// Recovery is a current-generation provider observation; handle replacement
			// is an implementation detail and a legal in-place update may retain it.
			for(const auto& object : {first, second}) {
				PhysicsQueryRequest query; query.world = frame->world; query.frame = frame->frame;
				query.pose.position = {object == first ? -2.0F : 3.0F, 0.0F, 0.0F};
				query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 4.0F;
				const auto result = fixture.system.QueryImmediate(query);
				const auto expected = fixture.objects.getObjectComponentGeneration(object);
				context.Expect(result.hits && result.hits->size() == 1U && result.hits->front().componentGeneration == expected,
					"A repaired authored batch must recover current provider generations. Expected=" + std::to_string(expected)
					+ " Actual=" + std::to_string(result.hits && !result.hits->empty() ? result.hits->front().componentGeneration : 0U));
			}
		});
		for(const bool settingsRestart : {false, true})
			registry.Add(settingsRestart ? "Physics.FrameQuery.PrimaryOverrideDoesNotSurviveSettingsRestart"
				: "Physics.FrameQuery.PrimaryOverrideDoesNotSurviveStopStart", [settingsRestart](TestContext& context) {
				PhysicsFixture fixture; fixture.Initialize(context);
				const auto primary = fixture.Body(context, "Returning primary membership", 0.0);
				const auto secondary = fixture.Body(context, "Retained secondary membership", 1000.0);
				const auto rejected = fixture.Body(context, "Rejected migration member", 1005.0);
				PhysicsFrameCreateRequest create; create.frameId = {84U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
				const auto frame = fixture.system.CreateFrame(create);
				context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(secondary), frame->frame)
					&& PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(rejected), frame->frame),
					"Independent members must enter their secondary authoring lifetime before admission.");
				PhysicsFrameMigrationRequest request; request.id = "pre-admission-membership";
				request.destinationFrame = {kPrimaryPhysicsFrameId, fixture.system.WorldHandle().worldGeneration};
				request.expectedDestinationResidencyGeneration = 1U;
				PhysicsFrameMigrationMember member; member.objectId = fixture.objects.getUUID(primary);
				member.sourceMembership = {frame->frame, frame->world, 1U}; member.canonicalPose = *fixture.objects.GetCanonicalWorldPose(primary);
				request.members.push_back(member);
				// Membership ingress precedes ALL provider admission: these callbacks
				// transfer no live provider state. The actual typed producer stamps the
				// primary override; real snapshot/Jolt admission and restart follow.
				const auto accepted = fixture.system.CommitFrameMigration(request, {
					.stageDestination = [](const auto&) { return true; },
					.commitDestinationAndReleaseSource = [](const auto&) { return true; },
					.rollbackDestination = [](const auto&) {}});
				context.Expect(accepted.accepted, "The actual typed producer must establish the exact primary membership override.");
				request.id = "rejected-pre-admission-membership"; request.members.front().objectId = fixture.objects.getUUID(rejected);
				request.members.front().canonicalPose = *fixture.objects.GetCanonicalWorldPose(rejected);
				bool rolledBack = false;
				const auto refusal = fixture.system.CommitFrameMigration(request, {
					.stageDestination = [](const auto&) { return true; },
					.commitDestinationAndReleaseSource = [](const auto&) { return false; },
					.rollbackDestination = [&](const auto&) { rolledBack = true; }});
				context.Expect(!refusal.accepted && rolledBack, "Rejected membership commit must roll back without replacing the source override.");
				fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
				const auto oldWorld = fixture.system.WorldHandle();
				if(settingsRestart) {
					auto settings = fixture.system.ConfiguredSettings(); ++settings.positionIterations;
					context.Expect(fixture.system.QueueSettingsUpdate(settings), "The real settings boundary must accept a restart-worthy update.");
				} else context.Expect(fixture.system.StopWorld() && fixture.system.StartWorld(), "The real primary provider must stop/start normally.");
				fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
				context.Expect(fixture.system.WorldHandle() != oldWorld, "Restart must actually change primary provider lifetime.");
				PhysicsQueryRequest query; query.world = fixture.system.WorldHandle(); query.pose.position = {-3.0F, 0.0F, 0.0F};
				query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 6.0F;
				const auto primaryHit = fixture.system.QueryImmediate(query);
				context.Expect(primaryHit.hits && primaryHit.hits->size() == 1U && primaryHit.hits->front().objectId == fixture.objects.getUUID(primary),
					"A stopped-primary override must not poison real body admission in the next world generation.");
				query.world = frame->world; query.frame = frame->frame;
				const auto secondaryHit = fixture.system.QueryImmediate(query);
				query.pose.position = {2.0F, 0.0F, 0.0F}; const auto rejectedHit = fixture.system.QueryImmediate(query);
				context.Expect(secondaryHit.hits && secondaryHit.hits->size() == 1U && secondaryHit.hits->front().objectId == fixture.objects.getUUID(secondary)
					&& rejectedHit.hits && rejectedHit.hits->size() == 1U && rejectedHit.hits->front().objectId == fixture.objects.getUUID(rejected),
					"Primary restart must retain live secondary membership and rejected migration must preserve its original authority.");
			});
		registry.Add("Physics.FrameQuery.SecondaryAdmissionRetainsAuthoredMotionAndSensorParameters", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto capped = fixture.Body(context, "Capped damped locked actor", 1000.0);
			const auto control = fixture.Body(context, "Undamped free actor", 1004.0);
			const auto angular = fixture.Body(context, "Angular cap and damping actor", 1008.0);
			const auto sensor = fixture.Body(context, "Secondary trigger", 1012.0);
			const auto primary = fixture.Body(context, "Primary authored velocity cap control", 0.0);
			auto* primaryBody = fixture.objects.GetComponent<RigidBodyComponent>(primary);
			primaryBody->initialLinearVelocity = {2.0F, 1.0F, 0.0F}; primaryBody->maxLinearVelocity = 0.25F; primaryBody->linearDamping = 0.9F;
			primaryBody->initialAngularVelocity = {0.0F, 2.0F, 0.0F}; primaryBody->maxAngularVelocity = 0.1F; primaryBody->angularDamping = 0.9F;
			auto* body = fixture.objects.GetComponent<RigidBodyComponent>(capped);
			body->initialLinearVelocity = {2.0F, 2.0F, 2.0F}; body->maxLinearVelocity = 0.25F;
			body->linearDamping = 0.9F; body->allowedDegreesOfFreedom = PhysicsTranslateX;
			body = fixture.objects.GetComponent<RigidBodyComponent>(control);
			body->initialLinearVelocity = {1.0F, 0.0F, 0.0F}; body->initialAngularVelocity = {0.0F, 1.0F, 0.0F}; body->angularDamping = 0.0F;
			body = fixture.objects.GetComponent<RigidBodyComponent>(angular);
			body->initialAngularVelocity = {0.0F, 2.0F, 0.0F}; body->maxAngularVelocity = 0.1F; body->angularDamping = 0.9F;
			fixture.objects.GetComponent<ColliderSetComponent>(sensor)->colliders.front().isTrigger = true;
			PhysicsFrameCreateRequest create; create.frameId = {85U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create); context.Expect(frame.has_value(), "Create the real parameter-comparison world.");
			for(const auto object : {capped, control, angular, sensor})
				context.Expect(PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame), "All authored parameter controls must share the actual provider frame.");
			for(unsigned tick = 0U; tick != 60U; ++tick) fixture.ScheduledTick(context, fixture.system.FixedTimeStep());
			const auto cappedPose = *fixture.objects.GetCanonicalWorldPose(capped);
			const auto controlPose = *fixture.objects.GetCanonicalWorldPose(control);
			const auto angularPose = *fixture.objects.GetCanonicalWorldPose(angular);
			const auto primaryPose = *fixture.objects.GetCanonicalWorldPose(primary);
			context.Expect(primaryPose.position.offsetMetres.x > 0.10 && primaryPose.position.offsetMetres.x < 0.20
				&& std::abs(primaryPose.position.offsetMetres.x - 2.0 * primaryPose.position.offsetMetres.y) < 0.001
				&& std::abs(primaryPose.rotation.y) > 0.001 && std::abs(primaryPose.rotation.y) < 0.04,
				"Primary authored over-cap initial vectors must clamp safely with direction preserved, using the same creation contract as secondary admission.");
			const double displacement = cappedPose.position.offsetMetres.x - 1000.0;
			context.Expect(displacement > 0.01 && displacement < 0.20 && std::abs(cappedPose.position.offsetMetres.y) < 1.0e-5
				&& std::abs(cappedPose.position.offsetMetres.z) < 1.0e-5 && controlPose.position.offsetMetres.x > 1004.9,
				"Actual secondary integration must honor authored linear cap/damping and locked translation axes, with an independently moving control.");
			context.Expect(std::abs(angularPose.rotation.y) > 0.001 && std::abs(angularPose.rotation.y) < 0.04
				&& std::abs(controlPose.rotation.y) > 0.4,
				"Actual secondary angular integration must honor its authored cap and damping rather than provider defaults.");
			PhysicsQueryRequest query; query.world = frame->world; query.frame = frame->frame;
			query.pose.position = {11.0F, 0.0F, 0.0F}; query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 2.0F;
			const auto excluded = fixture.system.QueryImmediate(query); query.filter.includeSensors = true;
			const auto included = fixture.system.QueryImmediate(query);
			context.Expect(excluded.hits && excluded.hits->empty() && included.hits && included.hits->size() == 1U
				&& included.hits->front().sensor && included.hits->front().objectId == fixture.objects.getUUID(sensor),
				"A trigger-only secondary collider must become an actual sensor and obey production query filtering.");
		});
		for(const bool frameCcd : {false, true})
			registry.Add(frameCcd ? "Physics.FrameQuery.SecondaryCcdUsesEnabledFramePolicy"
				: "Physics.FrameQuery.SecondaryCcdUsesDisabledFramePolicy", [frameCcd](TestContext& context) {
				PhysicsFixture fixture; fixture.Initialize(context);
				auto primarySettings = fixture.system.ConfiguredSettings(); primarySettings.continuousCollisionEnabled = !frameCcd;
				context.Expect(fixture.system.StopWorld() && fixture.system.StartWorld(primarySettings), "Give primary a deliberately opposite CCD policy.");
				const auto fast = fixture.Body(context, "Fast CCD actor", 1000.0);
				const auto wall = fixture.Body(context, "Thin CCD wall", 1001.5);
				auto* body = fixture.objects.GetComponent<RigidBodyComponent>(fast);
				body->initialLinearVelocity = {100.0F, 0.0F, 0.0F}; body->continuousCollisionMode = PhysicsMotionQuality::LinearCast;
				fixture.objects.GetComponent<RigidBodyComponent>(wall)->motionType = PhysicsMotionType::Static;
				fixture.objects.GetComponent<ColliderSetComponent>(wall)->colliders.front().shape = BoxColliderShape{{0.05F, 2.0F, 2.0F}};
				PhysicsFrameCreateRequest create; create.frameId = {86U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
				create.settings.fixedTimeStep = 0.05F; create.settings.maxAccumulatedTime = 0.1F;
				create.settings.continuousCollisionEnabled = frameCcd;
				const auto frame = fixture.system.CreateFrame(create);
				context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(fast), frame->frame)
					&& PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(wall), frame->frame), "The CCD comparison must use actual authored secondary bodies.");
				fixture.ScheduledTick(context, 0.05F);
				const auto pose = fixture.objects.GetCanonicalWorldPose(fast);
				context.Expect(pose && (frameCcd ? pose->position.offsetMetres.x < 1001.0 : pose->position.offsetMetres.x > 1003.0),
					"Actual thin-wall collision behavior must follow the owning frame CCD setting, independent of primary policy.");
			});
		registry.Add("Physics.FrameQuery.ReservedPrimaryIdCannotCreateSecondaryWorld", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto primary = fixture.Body(context, "Reserved-ID primary control", 0.0); fixture.Tick();
			const auto before = fixture.system.FrameDescriptors();
			PhysicsFrameCreateRequest reserved; reserved.frameId = kPrimaryPhysicsFrameId;
			context.Expect(!fixture.system.CreateFrame(reserved) && fixture.system.FrameDescriptors().size() == before.size(),
				"Reserved primary ID must reject before secondary provider creation or registry mutation.");
			PhysicsQueryRequest query; query.world = fixture.system.WorldHandle(); query.pose.position = {-3.0F, 0.0F, 0.0F};
			query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 6.0F;
			const auto hit = fixture.system.QueryImmediate(query);
			context.Expect(hit.hits && hit.hits->size() == 1U && hit.hits->front().objectId == fixture.objects.getUUID(primary),
				"Rejected secondary creation must leave primary query routing and provider identity intact.");
		});
		for(const bool pausedPrimary : {false, true})
			registry.Add(pausedPrimary ? "Physics.FrameQuery.SecondaryClockRunsWhilePrimaryPaused"
				: "Physics.FrameQuery.SecondaryFasterClockDoesNotDropTicks", [pausedPrimary](TestContext& context) {
				PhysicsFixture fixture; fixture.Initialize(context);
				const auto object = fixture.Body(context, "Independent-clock actor", 1000.0);
				fixture.objects.GetComponent<RigidBodyComponent>(object)->initialLinearVelocity = {1.0F, 0.0F, 0.0F};
				PhysicsFrameCreateRequest create; create.frameId = {81U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
				create.settings.fixedTimeStep = fixture.system.FixedTimeStep() * 0.5F;
				const auto frame = fixture.system.CreateFrame(create);
				context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame),
					"The faster clock must own a real isolated body before scheduler preparation.");
				if(pausedPrimary) context.Expect(fixture.system.SetPaused(true), "Pause only the primary control clock.");
				for(unsigned step = 1U; step <= 4U; ++step) {
					const auto oldPose = fixture.objects.GetCanonicalWorldPose(object);
					const auto before = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
					for(unsigned inspect = 0U; inspect != 3U; ++inspect) (void)fixture.system.DeclareFrameWorkloads(create.settings.fixedTimeStep);
					const auto after = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
					context.Expect(before && after && before->issued == after->issued && before->accumulator == after->accumulator,
						"Repeated declaration must not consume or advance a secondary clock.");
					fixture.ScheduledTick(context, create.settings.fixedTimeStep, true, [&] {
						context.Expect(fixture.objects.GetCanonicalWorldPose(object)->position == oldPose->position,
							"Scheduled secondary extraction must remain invisible until the existing typed flush.");
					});
					const auto clock = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
					context.Expect(clock && clock->issued == step && clock->completed == step,
						"Every scheduled secondary request must complete exactly once, without missing or duplicate clock ticks.");
				}
				const auto primary = PhysicsReviewTestAccess::ReadFrameClock(fixture.system,
					{kPrimaryPhysicsFrameId, fixture.system.WorldHandle().worldGeneration});
				context.Expect(primary && primary->completed == (pausedPrimary ? 0U : 2U), "Independent primary rate/paused policy must remain unchanged.");
			});
		registry.Add("Physics.FrameQuery.SecondaryPausedExplicitStepRetainsQueuedQuery", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Paused secondary actor", 1000.0);
			PhysicsFrameCreateRequest create; create.frameId = {82U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame), "Assign the actual paused-world actor.");
			context.Expect(fixture.system.SetPaused(true) && PhysicsReviewTestAccess::SetFramePaused(fixture.system, frame->frame, true), "Both clocks must pause independently.");
			fixture.ScheduledTick(context, 0.0F, false);
			PhysicsQueryRequest query; query.world = frame->world; query.frame = frame->frame;
			query.pose.position = {-3.0F, 0.0F, 0.0F}; query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 6.0F;
			const auto queued = fixture.system.SubmitQuery(query); context.Expect(queued.accepted, "The paused frame must retain its query admission.");
			fixture.ScheduledTick(context, 0.0F, false);
			context.Expect(PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame)->issued == 0U,
				"A queued query alone must not manufacture a paused-world tick.");
			context.Expect(PhysicsReviewTestAccess::RequestFrameExplicitStep(fixture.system, frame->frame), "The secondary clock must accept one real explicit paused step.");
			for(unsigned inspect = 0U; inspect != 3U; ++inspect) (void)fixture.system.DeclareFrameWorkloads(0.0F);
			context.Expect(PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame)->pendingExplicit == 1U,
				"Declaration must leave the one-shot paused request queued for preparation.");
			fixture.ScheduledTick(context, 0.0F);
			const auto read = fixture.system.ReadQuery(queued.handle);
			const auto clock = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
			context.Expect(clock && clock->issued == 1U && clock->completed == 1U && clock->pendingExplicit == 0U
				&& read.status == PhysicsResultStatus::Success && read.hits && read.hits->size() == 1U,
				"The owning provider must consume the explicit request once and complete the retained query at that exact tick.");
			fixture.ScheduledTick(context, 0.0F, false);
			context.Expect(PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame)->issued == 1U, "The next paused frame cannot replay the consumed explicit request.");
		});
		registry.Add("Physics.FrameQuery.SecondaryCatchupCapKeepsIssuedAndCompletedTicksAligned", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context); context.Expect(fixture.system.SetPaused(true), "Pause the primary clock for this secondary-only catchup.");
			PhysicsFrameCreateRequest create; create.frameId = {83U}; create.settings.fixedTimeStep = 1.0F / 120.0F;
			create.settings.maxSubsteps = 2U; create.settings.maxAccumulatedTime = 0.049F;
			const auto frame = fixture.system.CreateFrame(create); context.Expect(frame.has_value(), "The independently capped secondary clock must create.");
			fixture.ScheduledTick(context, 0.1F);
			const auto capped = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
			context.Expect(capped && capped->issued == 2U && capped->completed == 2U && capped->dropped > 0.0F
				&& capped->clamped > 0.0F && capped->accumulator < create.settings.fixedTimeStep,
				"Only the frame's configured two steps may execute; excess elapsed time is explicitly dropped/clamped.");
			fixture.ScheduledTick(context, create.settings.fixedTimeStep);
			const auto next = PhysicsReviewTestAccess::ReadFrameClock(fixture.system, frame->frame);
			context.Expect(next && next->issued == 3U && next->completed == 3U, "Catchup policy must not leave a skipped provider tick that poisons future completion.");
		});
		registry.Add("Physics.FramePublication.DestroyWaitsForExistingProtectedReceipt", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Pending island publication", 1000.0);
			fixture.objects.GetComponent<RigidBodyComponent>(object)->initialLinearVelocity = {1.0F, 0.0F, 0.0F}; // Ensure a real pose change, independent of no-op write suppression.
			PhysicsFrameCreateRequest create; create.frameId = {74U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame),
				"The pending-receipt control needs a real secondary provider body.");
			const auto revision = fixture.objects.GetObjectTransformRevision(object);
			(void)fixture.objects.PublishSceneChangePacket(); fixture.system.Update(fixture.system.FixedTimeStep());
			auto& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			const auto pending = queue.PendingPublicationGroups().size();
			context.Expect(pending != 0U && !fixture.system.DestroyFrame(frame->frame)
				&& queue.PendingPublicationGroups().size() == pending
				&& fixture.objects.GetObjectTransformRevision(object) == revision,
				"Retirement before receipt settlement must preserve queued groups and deferred scene visibility.");
			const auto frames = fixture.system.FrameDescriptors();
			context.Expect(std::any_of(frames.begin(), frames.end(), [&](const auto& value) { return value.frame == frame->frame; }),
				"Rejected retirement must preserve the provider frame lifetime.");
			const auto receipt = queue.Apply(fixture.objects, "frame-query-review-existing-boundary");
			fixture.system.OnAfterPhysicsFlush(receipt.transactionResults, receipt.publicationGroupResults);
			context.Expect(fixture.objects.GetObjectTransformRevision(object) > revision && fixture.system.DestroyFrame(frame->frame),
				"The original queued write must settle normally before the same retirement can succeed.");
		});
		registry.Add("Physics.FramePublication.SecondarySuccessivePosesRetainSolverProgress", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Falling secondary body", 1000.0);
			fixture.objects.GetComponent<RigidBodyComponent>(object)->useGravity = true;
			PhysicsFrameCreateRequest create; create.frameId = {71U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame),
				"The dynamic body must enter an actual isolated provider before capture.");
			std::uint64_t revision = fixture.objects.GetObjectTransformRevision(object);
			double previousY = 0.0, earlyDrop = 0.0, lastDrop = 0.0;
			PhysicsBodyHandle firstHandle;
			for(unsigned tick = 0U; tick != 8U; ++tick) {
				// A discontinuity forces a fresh capture on tick four, independently
				// checking SamePose feedback suppression as well as exact packet reuse.
				if(tick == 4U) (void)fixture.objects.PublishSceneChangePacket();
				fixture.Tick();
				const auto pose = fixture.objects.GetCanonicalWorldPose(object);
				const auto nextRevision = fixture.objects.GetObjectTransformRevision(object);
				context.Expect(pose && nextRevision > revision && pose->position.offsetMetres.y < previousY,
					"Every accepted secondary pose must advance the scene transform and its expected revision.");
				lastDrop = previousY - pose->position.offsetMetres.y;
				if(tick == 1U) earlyDrop = lastDrop;
				previousY = pose->position.offsetMetres.y; revision = nextRevision;
				PhysicsQueryRequest query; query.world = frame->world; query.frame = frame->frame;
				query.pose.position = {0.0F, static_cast<float>(previousY) + 2.0F, 0.0F};
				query.direction = {0.0F, -1.0F, 0.0F}; query.distance = 4.0F;
				const auto hit = fixture.system.QueryImmediate(query);
				context.Expect(hit.status == PhysicsResultStatus::Success && hit.hits && hit.hits->size() == 1U,
					"Each published pose must still identify one actual provider body.");
				if(tick == 0U) firstHandle = hit.hits->front().body;
				context.Expect(hit.hits->front().body == firstHandle, "Own transform feedback must not recreate the secondary solver body.");
			}
			const auto diagnostics = fixture.system.LatestDiagnostics();
			context.Expect(lastDrop > 2.0 * earlyDrop && diagnostics && diagnostics->publication.rejectedPackets == 0U,
				"Gravity must accumulate solver velocity across accepted publications without stale-revision rejection.");
		});
		registry.Add("Physics.FramePublication.SecondaryUnchangedPoseDoesNotMutateScene", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Stationary secondary body", 1000.0);
			PhysicsFrameCreateRequest create; create.frameId = {72U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame),
				"The stationary control needs a real secondary provider owner.");
			fixture.Tick(); fixture.Tick();
			const auto revision = fixture.objects.GetObjectTransformRevision(object);
			const auto accepted = fixture.system.LatestDiagnostics()->publication.acceptedPackets;
			for(unsigned tick = 0U; tick != 5U; ++tick) fixture.Tick();
			context.Expect(fixture.objects.GetObjectTransformRevision(object) == revision
				&& fixture.system.LatestDiagnostics()->publication.acceptedPackets == accepted,
				"Full secondary extraction must suppress already accepted identical poses rather than enqueue redundant groups.");
		});
		registry.Add("Physics.FramePublication.DestroyedMembershipRecapturesAndAllowsUnrelatedProgress", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto object = fixture.Body(context, "Retiring island body", 1000.0);
			PhysicsFrameCreateRequest create; create.frameId = {73U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame && PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(object), frame->frame),
				"The retiring member must first exist in the real secondary authoring snapshot.");
			fixture.Tick(); fixture.Tick();
			context.Expect(fixture.system.DestroyFrame(frame->frame), "The exact island lifetime must retire successfully.");
			// No scene mutation accompanies retirement. The provider must invalidate
			// cached membership itself, before unrelated authoring is introduced.
			fixture.Tick();
			PhysicsQueryRequest query; query.world = fixture.system.WorldHandle(); query.pose.position = {997.0F, 0.0F, 0.0F};
			query.direction = {1.0F, 0.0F, 0.0F}; query.distance = 6.0F;
			const auto returned = fixture.system.QueryImmediate(query);
			context.Expect(returned.hits && returned.hits->size() == 1U && returned.hits->front().objectId == fixture.objects.getUUID(object),
				"A destroyed-frame member must recapture into ordinary primary authoring without a triggering scene edit.");
			const auto unrelated = fixture.Body(context, "Unrelated new primary body", 5.0); fixture.Tick();
			query.pose.position = {2.0F, 0.0F, 0.0F};
			const auto progressed = fixture.system.QueryImmediate(query);
			context.Expect(progressed.status == PhysicsResultStatus::Success && progressed.hits && progressed.hits->size() == 1U
				&& progressed.hits->front().objectId == fixture.objects.getUUID(unrelated),
				"A stale destroyed-frame override must not reject the entire next authoring capture.");
		});
		registry.Add("Physics.FrameQuery.RecreatedSameFrameRejectsPriorQueryHandles", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			PhysicsFrameCreateRequest create{}; create.frameId = {19u};
			const auto oldFrame = fixture.system.CreateFrame(create);
			context.Expect(oldFrame.has_value(), "The first actual provider frame must be created.");
			PhysicsQueryRequest request{}; request.world = oldFrame->world; request.frame = oldFrame->frame;
			request.pose.position = {-3.0f, 0.0f, 0.0f}; request.direction = {1.0f, 0.0f, 0.0f}; request.distance = 6.0f;
			const auto oldQuery = fixture.system.SubmitQuery(request);
			context.Expect(oldQuery.accepted && fixture.system.DestroyFrame(oldFrame->frame), "A real queued query must precede actual frame retirement.");
			const auto replacement = fixture.system.CreateFrame(create);
			context.Expect(replacement.has_value() && replacement->frame != oldFrame->frame, "Recreation must reuse the same ID with a new frame lifetime.");
			request.world = replacement->world; request.frame = replacement->frame;
			const auto currentQuery = fixture.system.SubmitQuery(request);
			context.Expect(currentQuery.accepted, "The recreated provider world must accept its own fresh query.");
			// Check actual routes before identity assertions: old cancellation must not
			// reach a new slot whose local generation happens to match the retired one.
			const bool staleCanceled = fixture.system.CancelQuery(oldQuery.handle);
			const auto staleRead = fixture.system.ReadQuery(oldQuery.handle);
			fixture.Tick();
			const auto currentRead = fixture.system.ReadQuery(currentQuery.handle);
			context.Expect(!staleCanceled && staleRead.status == PhysicsResultStatus::StaleTarget
				&& staleRead.rejectionReason == PhysicsQueryRejectionReason::StaleWorld,
				"Old query Read/Cancel must reject the retired frame lifetime after same-ID recreation.");
			context.Expect(oldFrame->world != replacement->world && oldQuery.handle != currentQuery.handle
				&& currentRead.status == PhysicsResultStatus::Success,
				"The replacement world identity must protect its fresh query from stale cancellation.");
		});
		for(const bool immediate : {true, false})
			registry.Add(immediate ? "Physics.FrameQuery.SecondaryImmediateUsesOwningRegistries"
				: "Physics.FrameQuery.SecondaryScheduledCancelReadUsesOwningWorld", [immediate](TestContext& context) {
				PhysicsFixture fixture; fixture.Initialize(context);
				const auto primary = fixture.Body(context, "Primary local box", 0.0);
				const auto secondary = fixture.Body(context, "Secondary local box", 1000.0);
				PhysicsFrameCreateRequest create; create.frameId = {17U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
				const auto frame = fixture.system.CreateFrame(create);
				context.Expect(frame.has_value(), "The fixture must create an actual isolated Jolt frame.");
				// Internal fixture ingress deliberately bypasses the migration producer.
				// Every body/handle below is still built by actual snapshot/prepare/commit.
				context.Expect(PhysicsReviewTestAccess::AssignFixtureFrame(fixture.system, fixture.objects.getUUID(secondary), frame->frame),
					"Only the real fixture-owned object may be assigned before its first authoring capture.");
				fixture.Tick(); fixture.Tick();
				const auto diagnostics = fixture.system.LatestDiagnostics();
				context.Expect(diagnostics && diagnostics->frames
					&& std::any_of(diagnostics->frames->begin(), diagnostics->frames->end(), [&](const auto& value) {
						return value.descriptor.frame == frame->frame && value.bodyCount == 1U;
					}), "The actual secondary provider pipeline must own one constructed body before queries.");
				PhysicsQueryRequest primaryQuery; primaryQuery.world = fixture.system.WorldHandle();
				primaryQuery.pose.position = {-3.0F, 0.0F, 0.0F}; primaryQuery.direction = {1.0F, 0.0F, 0.0F}; primaryQuery.distance = 6.0F;
				auto secondaryQuery = primaryQuery; secondaryQuery.world = frame->world; secondaryQuery.frame = frame->frame;
				if(immediate)
				{
					const auto first = fixture.system.QueryImmediate(primaryQuery);
					const auto other = fixture.system.QueryImmediate(secondaryQuery);
					context.Expect(first.status == PhysicsResultStatus::Success && first.hits && first.hits->size() == 1U
						&& first.hits->front().objectId == fixture.objects.getUUID(primary),
						"The primary control query must hit its real local box.");
					context.Expect(other.status == PhysicsResultStatus::Success && other.hits && other.hits->size() == 1U
						&& other.hits->front().objectId == fixture.objects.getUUID(secondary) && other.hits->front().frame == frame->frame,
						"Equivalent secondary-frame geometry must survive validation against its own world and runtime registry.");
				}
				else
				{
					const auto primaryReceipt = fixture.system.SubmitQuery(primaryQuery);
					const auto canceled = fixture.system.SubmitQuery(secondaryQuery);
					const auto secondaryReceipt = fixture.system.SubmitQuery(secondaryQuery);
					context.Expect(primaryReceipt.accepted && canceled.accepted && secondaryReceipt.accepted,
						"All scheduled controls must receive actual owning-world query handles.");
					context.Expect(fixture.system.CancelQuery(canceled.handle), "A secondary handle must cancel in its owning world.");
					fixture.Tick();
					const auto first = fixture.system.ReadQuery(primaryReceipt.handle);
					const auto other = fixture.system.ReadQuery(secondaryReceipt.handle);
					const auto rejected = fixture.system.ReadQuery(canceled.handle);
					context.Expect(first.status == PhysicsResultStatus::Success && first.hits && first.hits->size() == 1U
						&& first.hits->front().objectId == fixture.objects.getUUID(primary), "Secondary cancellation must preserve the primary scheduled control.");
					context.Expect(rejected.status == PhysicsResultStatus::Cancelled
						&& other.status == PhysicsResultStatus::Success && other.hits && other.hits->size() == 1U
						&& other.hits->front().objectId == fixture.objects.getUUID(secondary),
						"Cancel, execution, and read must use the same secondary world without dropping its valid hits.");
					const auto retained = other.hits;
					context.Expect(fixture.system.DestroyFrame(frame->frame) && retained->size() == 1U
						&& fixture.system.ReadQuery(secondaryReceipt.handle).status != PhysicsResultStatus::Success,
						"Frame retirement must stale its handles while immutable prior result ownership remains valid.");
				}
			});

		for(const int kind : {0, 1, 2})
			registry.Add(kind == 0 ? "Physics.FrameQuery.NewRigidOwnerInvalidatesReusedAuthoring"
				: kind == 1 ? "Physics.FrameQuery.NewStaticColliderInvalidatesReusedAuthoring"
				: "Physics.FrameQuery.NewConstraintOwnerInvalidatesReusedAuthoring", [kind](TestContext& context) {
				PhysicsFixture fixture; fixture.Initialize(context);
				const Object first = fixture.Body(context, "Existing first", -4.0);
				const Object second = fixture.Body(context, "Existing second", 4.0);
				Object newcomer = fixture.objects.createObject("Previously nonphysics owner");
				if(kind == 1) fixture.objects.setObjectStatic(newcomer, true);
				fixture.Tick(); fixture.Tick(); fixture.Tick();
				const auto baseline = fixture.system.LatestDiagnostics();
				context.Expect(baseline && baseline->authoring.snapshotReused,
					"The real pipeline must first prove immutable authoring packet reuse.");
				if(kind == 0)
				{
					auto* rigid = fixture.objects.AddComponent<RigidBodyComponent>(newcomer);
					auto* colliders = fixture.objects.AddComponent<ColliderSetComponent>(newcomer);
					context.Expect(rigid && colliders, "The new dynamic owner must have a valid real collider.");
					rigid->useGravity = false; colliders->Add();
				}
				else if(kind == 1)
				{
					auto* colliders = fixture.objects.AddComponent<ColliderSetComponent>(newcomer);
					context.Expect(colliders != nullptr, "The new static owner must receive a real collider set."); colliders->Add();
				}
				else
				{
					auto* constraints = fixture.objects.AddComponent<ConstraintSetComponent>(newcomer);
					context.Expect(constraints != nullptr, "The new constraint owner must receive a real authored set.");
					ConstraintRecord record; record.firstObjectId = fixture.objects.getUUID(first);
					record.secondEndpointKind = PhysicsConstraintEndpointKind::ConnectedObject;
					record.secondObjectId = fixture.objects.getUUID(second); constraints->Add(record);
				}
				const auto detached = fixture.system.LatestDiagnostics();
				context.Expect(detached && detached->counters.bodyCount == baseline->counters.bodyCount
					&& detached->counters.constraintCount == baseline->counters.constraintCount,
					"Authoring changes must remain detached until the declared snapshot/commit boundary.");
				fixture.Tick();
				const auto current = fixture.system.LatestDiagnostics();
				context.Expect(current && !current->authoring.snapshotReused
					&& (kind == 2 ? current->counters.constraintCount == baseline->counters.constraintCount + 1U
						: current->counters.bodyCount == baseline->counters.bodyCount + 1U),
					"The next real protected authoring capture must discover the new rigid/collider/constraint owner.");
			});

		registry.Add("Physics.FrameQuery.BothWorldBatchesExecuteOnObservedWorkers", [](TestContext& context) {
			PhysicsFixture fixture; fixture.Initialize(context);
			const auto primary = fixture.Body(context, "Primary worker-query box", 0.0);
			const auto secondary = fixture.Body(context, "Secondary worker-query box", 1000.0);
			PhysicsFrameCreateRequest create; create.frameId = {23U}; create.canonicalOrigin.offsetMetres = {1000.0, 0.0, 0.0};
			const auto frame = fixture.system.CreateFrame(create);
			context.Expect(frame.has_value() && PhysicsReviewTestAccess::AssignFixtureFrame(
				fixture.system, fixture.objects.getUUID(secondary), frame->frame),
				"Both real provider worlds must own separately captured geometry before worker query qualification.");
			fixture.Tick(); fixture.Tick();
			std::shared_ptr<const std::vector<PhysicsQueryHit>> retainedSecondaryHits;
			PhysicsQueryHandle retainedSecondaryHandle;
			for(const bool selectedSecondary : {false, true}) {
				PhysicsQueryRequest request;
				request.world = selectedSecondary ? frame->world : fixture.system.WorldHandle();
				if(selectedSecondary) request.frame = frame->frame;
				request.pose.position = {-3.0F, 0.0F, 0.0F}; request.direction = {1.0F, 0.0F, 0.0F}; request.distance = 6.0F;
				const auto expectedObject = fixture.objects.getUUID(selectedSecondary ? secondary : primary);
				const auto immediate = fixture.system.QueryImmediate(request);
				context.Expect(immediate.status == PhysicsResultStatus::Success && immediate.hits && immediate.hits->size() == 1U
					&& immediate.hits->front().objectId == expectedObject,
					"Each observed worker episode needs an independent immediate owning-world control.");
				// Cancel one of65 admitted requests, leaving64 real queries: this
				// crosses the production child-range threshold without counting a
				// configured worker capacity as evidence that any worker ran.
				const std::vector<PhysicsQueryRequest> requests(65U, request);
				const auto submitted = fixture.system.SubmitQueryBatch(requests);
				context.Expect(submitted.accepted && submitted.receipts.size() == requests.size()
					&& std::all_of(submitted.receipts.begin(), submitted.receipts.end(), [](const auto& receipt) { return receipt.accepted; }),
					"The actual owning world must admit the complete bounded query batch.");
				context.Expect(fixture.system.CancelQuery(submitted.receipts.front().handle), "Cancellation must target this episode's owning query registry.");
				context.Expect(fixture.system.ReadQuery(submitted.receipts.back().handle).status != PhysicsResultStatus::Success,
					"Queued worker queries cannot publish successful hits before their protected tick boundary.");

				struct Observation {
					std::atomic_size_t created{0U}, submitted{0U}, children{0U}, settled{0U}, invalidWorkers{0U};
					std::atomic_uint64_t workerMask{0U};
					const std::thread::id coordinator = std::this_thread::get_id();
				} observed;
				// PhysicsSystem uses ScopedTaskSettlement only for query child ranges.
				// One world has pending queries in this scope, so its Unspecified/Child
				// checkpoint is attributable to this actual backend query batch.
				// Do not overwrite another fixture's hook or leave borrowed observation
				// state installed after the production settlement scope has returned.
				context.Expect(!Tasks::CaptureNestedTaskHookForTesting(), "Worker observation requires an unclaimed quiescent nested-task hook.");
				Tasks::SetNestedTaskHookForTesting([&observed](Tasks::NestedTaskCheckpoint checkpoint) {
					if(checkpoint.group != Tasks::NestedTaskGroup::Unspecified) return;
					if(checkpoint.stage == Tasks::NestedTaskStage::Creation) ++observed.created;
					else if(checkpoint.stage == Tasks::NestedTaskStage::Submission) ++observed.submitted;
					else if(checkpoint.stage == Tasks::NestedTaskStage::Result) ++observed.settled;
					else if(checkpoint.stage == Tasks::NestedTaskStage::Child) {
						++observed.children;
						const auto worker = ThreadPool::instance().CurrentExecutingWorkerIndex();
						if(!worker || *worker >= 64U || std::this_thread::get_id() == observed.coordinator) ++observed.invalidWorkers;
						else observed.workerMask.fetch_or(std::uint64_t{1U} << *worker);
					}
				});
				{
					struct ClearObservedHook {
						~ClearObservedHook() { Tasks::SetNestedTaskHookForTesting({}); }
					} clearHook;
					fixture.Tick();
				}
				context.Expect(observed.created != 0U && observed.created == observed.submitted
					&& observed.submitted == observed.children && observed.children == observed.settled
					&& observed.invalidWorkers == 0U && observed.workerMask != 0U,
					"Actual query children must execute on identified non-coordinator pool workers and settle before readback.");
				context.Expect(fixture.system.ReadQuery(submitted.receipts.front().handle).status == PhysicsResultStatus::Cancelled,
					"The canceled request must stay canceled while its64 siblings execute on workers.");
				for(std::size_t index = 1U; index < submitted.receipts.size(); ++index) {
					const auto result = fixture.system.ReadQuery(submitted.receipts[index].handle);
					context.Expect(result.status == PhysicsResultStatus::Success && result.world == request.world
						&& result.hits && result.hits->size() == 1U && result.hits->front().objectId == expectedObject
						&& (!selectedSecondary || result.hits->front().frame == frame->frame),
						"Every worker result must retain its owning world, actual object and selected frame.");
					if(selectedSecondary) { retainedSecondaryHits = result.hits; retainedSecondaryHandle = result.handle; }
				}
				std::cout << "[OBSERVE] Physics.FrameQuery.WorkerBatch world=" << request.world.worldId.value
					<< " secondary=" << selectedSecondary << " executedQueries=64 workerMask=" << observed.workerMask.load()
					<< " created=" << observed.created.load() << " children=" << observed.children.load()
					<< " settled=" << observed.settled.load() << '\n';
			}
			context.Expect(retainedSecondaryHits && fixture.system.DestroyFrame(frame->frame)
				&& retainedSecondaryHits->size() == 1U && retainedSecondaryHits->front().objectId == fixture.objects.getUUID(secondary)
				&& fixture.system.ReadQuery(retainedSecondaryHandle).status != PhysicsResultStatus::Success
				&& !fixture.system.CancelQuery(retainedSecondaryHandle),
				"Retiring the observed secondary worker world must stale its handles while preserving copied prior hits.");
		});

		registry.Add("Physics.FrameQuery.RefusedResizeDoesNotAbortOtherCharacters", [](TestContext& context) {
			const auto previousWorkers = ThreadPool::instance().getThreadCount();
			ThreadPool::instance().shutdown(); ThreadPool::instance().initialize(2U);
			JoltPhysicsBackend backend(ThreadPool::instance());
			const PhysicsWorldHandle world{{82U}, 1U};
			struct Cleanup {
				JoltPhysicsBackend& backend; PhysicsWorldHandle world; std::size_t workers;
				~Cleanup() { (void)backend.DestroyWorld(world); (void)backend.Shutdown(); ThreadPool::instance().shutdown(); if(workers) ThreadPool::instance().initialize(workers); }
			} cleanup{backend, world, previousWorkers};
			context.Expect(backend.Initialize().Succeeded(), "The resize fixture must initialize real native Jolt.");
			PhysicsBackendWorldConfiguration configuration; configuration.world = world;
			context.Expect(backend.CreateWorld(configuration).Succeeded(), "The resize fixture must create its isolated provider world.");
			// The original capsule fits, but growing it reaches a real overhead box.
			PhysicsBackendStagingToken staging;
			const PhysicsShapeHandle ceilingShape{world.worldId, 1U, 0U, 1U};
			PhysicsShapeRecipe ceiling; ceiling.kind = PhysicsShapeKind::Box; ceiling.dimensions = {2.0F, 0.5F, 2.0F};
			context.Expect(backend.CreateShape(world, ceilingShape, ceiling, staging).Succeeded(), "The resize obstruction must be a real Jolt shape.");
			PhysicsBodyCreateRequest blocker;
			blocker.handle = {world.worldId, 1U, 0U, 1U}; blocker.shape = ceilingShape; blocker.objectId = TerraEngine::GenerateUUID();
			blocker.objectGeneration = blocker.componentGeneration = 1U; blocker.motionType = PhysicsMotionType::Static;
			blocker.pose.position = {0.0F, 2.75F, 0.0F};
			context.Expect(backend.CreateBody(blocker, staging).Succeeded(), "The original capsule must fit below the obstruction.");
			PhysicsCharacterCreateRequest blocked;
			blocked.handle = {world.worldId, 1U, 0U, 1U}; blocked.objectId = TerraEngine::GenerateUUID();
			blocked.objectGeneration = blocked.componentGeneration = 1U; blocked.radius = 0.5F; blocked.height = 2.0F;
			blocked.pose.position = {0.0F, 1.1F, 0.0F}; blocked.useGravity = false;
			auto moving = blocked; moving.handle.slotIndex = 1U; moving.objectId = TerraEngine::GenerateUUID(); moving.pose.position.x = 5.0F;
			context.Expect(backend.CreateCharacter(blocked, staging).Succeeded() && backend.CreateCharacter(moving, staging).Succeeded()
				&& backend.CommitStaging(staging).Succeeded(), "Both actors and their obstruction must publish before the fixed tick.");
			PhysicsCharacterUpdateRequest move;
			move.handle = moving.handle; move.objectId = moving.objectId; move.objectGeneration = move.componentGeneration = 1U;
			move.desiredVelocity = {3.0F, 0.0F, 0.0F};
			PhysicsCharacterUpdateRequest resize;
			resize.handle = blocked.handle; resize.objectId = blocked.objectId; resize.objectGeneration = resize.componentGeneration = 1U;
			resize.resizeRequested = true; resize.resizeHeight = 4.0F; resize.recoverPenetration = false;
			context.Expect(backend.UpdateCharacter(move).Succeeded() && backend.UpdateCharacter(resize).Succeeded(),
				"A valid finite resize must pass admission and be refused only by the actual obstructed provider shape change.");
			(void)backend.AdvanceCharacters(world, 1U, 1.0F / 60.0F);
			const auto dimensions = backend.CharacterDimensionsForTesting(blocked.handle);
			context.Expect(dimensions && dimensions->height == blocked.height && dimensions->radius == blocked.radius,
				"Jolt must actually refuse the obstructed resize and retain the original capsule dimensions.");
			const auto refusal = backend.LatestCharacterResize(blocked.handle);
			context.Expect(refusal && !refusal->accepted && refusal->tick == 1U
				&& refusal->requestedHeight == resize.resizeHeight && refusal->retainedHeight == blocked.height,
				"The tick must expose an actor-local refusal with truthful requested and retained dimensions.");
			PhysicsBackendExtraction extraction; extraction.world = world;
			context.Expect(backend.Extract(extraction).Succeeded(), "The tick's actual actor states must remain extractable.");
			const auto moved = std::find_if(extraction.characters.begin(), extraction.characters.end(), [&](const auto& state) { return state.handle == moving.handle; });
			context.Expect(moved != extraction.characters.end() && std::fabs(moved->pose.position.x - 5.05F) < 0.005F,
				"A refused resize on the earlier actor must not skip the unaffected actor's one fixed tick.");
			context.Expect(backend.AdvanceCharacters(world, 2U, 1.0F / 60.0F).Succeeded(),
				"The rejected one-shot resize must not poison the next complete actor tick.");
		});
	}
}
