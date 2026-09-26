// Protected-boundary coverage for NavigationAgent guidance consumed by locomotion.
// The fixture uses real navigation publications and a live Physics controller;
// no test writes a Transform to simulate movement or supplies provider pointers.
#include "../TestHarness.h"

#include "Components/CharacterLocomotionComponent.h"
#include "Components/NavigationAgentComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/JobGraph.h"
#include "Locomotion/LocomotionSystem.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;
	using namespace TerraEngine::Locomotion;

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

		class NavigationLocomotionFixture final
		{
		public:
			NavigationLocomotionFixture()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				physics_.Shutdown();
				physics_.SetEditorLifecycleControlled(true);
				physics_.Initialize();
				navigation_.Shutdown();
				agents_.Shutdown();
				navigation_.Initialize();
				agents_.Initialize();
				locomotion_.Shutdown();
				locomotion_.Initialize();

				owner_ = ObjectManager::instance().createObject("Navigation locomotion owner");
				(void)ObjectManager::instance().AddComponent<Transform>(owner_);
				target_ = ObjectManager::instance().createObject("Moving navigation target");
				(void)ObjectManager::instance().AddComponent<Transform>(target_);
				Scene::WorldPose targetPose{};
				targetPose.position.offsetMetres = {8.0, 0.0, 0.0};
				(void)ObjectManager::instance().SetCanonicalWorldPose(target_, targetPose);
				ObjectManager::instance().MarkObjectTransformDirty(target_);
				auto* controller = ObjectManager::instance().AddComponent<CharacterControllerComponent>(owner_);
				if(controller != nullptr) {
					controller->radius = 0.35F;
					controller->height = 1.8F;
					controller->localCenter = {0.0F, 0.9F, 0.0F};
					controller->useGravity = false;
				}
				auto* authoredNavigation = ObjectManager::instance().AddComponent<NavigationAgentComponent>(owner_);
				if(authoredNavigation != nullptr) {
					authoredNavigation->radiusMetres = 0.4;
					authoredNavigation->heightMetres = 1.8;
					authoredNavigation->stepHeightMetres = 0.3;
					authoredNavigation->clearanceMarginMetres = 0.03;
					authoredNavigation->arrivalToleranceMetres = 0.25;
				}
				auto* movement = ObjectManager::instance().AddComponent<CharacterLocomotionComponent>(owner_);
				if(movement != nullptr) movement->navigationBinding = CharacterLocomotionComponent::kSelfBinding;
			}

			~NavigationLocomotionFixture()
			{
				locomotion_.Shutdown();
				agents_.Shutdown();
				navigation_.Shutdown();
				(void)physics_.StopWorld();
				physics_.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Start(TestContext& context)
			{
				auto& objects = ObjectManager::instance();
				context.Expect(physics_.StartWorld(), "The fixture requires a real active Physics world.");
				for(std::uint32_t frame = 0U; frame < 4U; ++frame) AdvancePhysics();
				const auto terrain = FlatTerrain();
				const auto* authoredNavigation = ObjectManager::instance().GetComponent<NavigationAgentComponent>(owner_);
				const NavigationProfile profile = authoredNavigation != nullptr
					? authoredNavigation->ToNavigationProfile() : NavigationProfile::Defaults();
				const bool terrainPublished = terrain != nullptr
					&& authoredNavigation != nullptr && navigation_.QueueInitialGroundBuild(profile, {terrain});
				if(terrainPublished) navigation_.Update(0.0F);
				for(std::uint32_t frame = 0U; frame < 3U; ++frame) AdvanceNavigation();
				locomotion_.ObserveProtectedBoundary();
				handle_ = locomotion_.ResolvePublished(objects.getUUID(owner_));
				identity_ = agents_.FindIdentity(objects, objects.getUUID(owner_));
				const auto world = navigation_.PublishedWorld();
				context.Expect(terrainPublished && world != nullptr
					&& world->terrainState == NavigationTerrainState::Available,
					"The fixture must publish its in-memory flat navigation terrain.");
				context.Expect(handle_.has_value() && identity_.IsValid(),
					"Both systems must publish their live owner attachment before guidance tests run.");
				return terrainPublished && world != nullptr && handle_.has_value() && identity_.IsValid();
			}

			void AdvancePhysics(float deltaTime = 1.0F / 60.0F)
			{
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics_.Update(deltaTime);
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(objects,
					"locomotion-navigation-boundary-physics-flush");
				physics_.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			void AdvanceNavigation(std::uint32_t count = 1U)
			{
				for(std::uint32_t frame = 0U; frame < count; ++frame) {
					navigation_.Update(1.0F / 60.0F);
					agents_.Update(1.0F / 60.0F);
				}
			}

			void AdvanceNavigation(float deltaTime)
			{
				navigation_.Update(deltaTime);
				agents_.Update(deltaTime);
			}

			double CharacterX() const
			{
				const auto packet = physics_.LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::numeric_limits<double>::quiet_NaN();
				const UUID id = ObjectManager::instance().getUUID(owner_);
				const auto state = std::find_if(packet->states->begin(), packet->states->end(), [id](const auto& item) {
					return item.objectId == id;
				});
				return state == packet->states->end()
					? std::numeric_limits<double>::quiet_NaN() : state->pose.position.x;
			}

			bool RequestTarget(double x, std::uint64_t correlation)
			{
				Scene::WorldPose pose{};
				pose.position.offsetMetres = {x, 0.0, 0.0};
				(void)ObjectManager::instance().SetCanonicalWorldPose(target_, pose);
				ObjectManager::instance().MarkObjectTransformDirty(target_);
				const NavigationObjectIdentity targetIdentity{
					ObjectManager::instance().getUUID(target_), ObjectManager::instance().getObjectLifetimeGeneration(target_)};
				const auto receipt = agents_.QueueObjectDestination(identity_, targetIdentity,
					0U, 0U, correlation, correlation + 100000U);
				if(receipt.state == NavigationAgentReceiptState::Rejected) return false;
				for(std::uint32_t frame = 0U; frame < 64U; ++frame) {
					AdvanceNavigation();
					if(agents_.ReadGuidance(identity_).IsUsable()) return true;
				}
				return false;
			}

			bool RequestLocation(double x, std::uint64_t correlation)
			{
				const auto world = navigation_.PublishedWorld();
				if(world == nullptr) return false;
				NavigationLocation destination{};
				destination.world = world->world;
				destination.position.offsetMetres = {x, 0.0, 0.0};
				const auto receipt = agents_.QueueLocationDestination(identity_, destination,
					0U, 0U, correlation, correlation + 100000U);
				if(receipt.state == NavigationAgentReceiptState::Rejected) return false;
				for(std::uint32_t frame = 0U; frame < 64U; ++frame) {
					AdvanceNavigation();
					if(agents_.ReadGuidance(identity_).IsUsable()) return true;
				}
				return false;
			}

			void MoveTarget(double x)
			{
				Scene::WorldPose pose{};
				pose.position.offsetMetres = {x, 0.0, 0.0};
				(void)ObjectManager::instance().SetCanonicalWorldPose(target_, pose);
				ObjectManager::instance().MarkObjectTransformDirty(target_);
			}

			std::string GuidanceFailure() const
			{
				const auto status = agents_.ReadStatus(identity_);
				const auto guidance = agents_.ReadGuidance(identity_);
				return "Navigation route did not publish usable guidance (state="
					+ std::string(ToString(status.state)) + ", outcome="
					+ std::string(ToString(status.outcome)) + ", reason="
					+ std::string(ToString(status.reason)) + ", guidance="
					+ std::string(ToString(guidance.reason)) + ").";
			}

			CommandReceipt Acquire(std::uint64_t producer)
			{
				LocomotionCommand command{};
				command.kind = CommandKind::Acquire;
				command.handle = *handle_;
				command.expectedConfigurationRevision = 1U;
				command.producerId = producer;
				command.sequence = 1U;
				command.correlationId = producer * 100U + 1U;
				(void)locomotion_.SubmitBatch(std::span(&command, 1U));
				locomotion_.Update(1.0F / 60.0F);
				return locomotion_.ReadReceipt(*handle_, producer, command.sequence, command.correlationId);
			}

			LocomotionCommand Follow(const CommandReceipt& acquired, std::uint64_t producer,
				std::uint64_t sequence) const
			{
				LocomotionCommand command{};
				command.kind = CommandKind::FollowNavigation;
				command.handle = *handle_;
				command.expectedAuthorityEpoch = acquired.authorityEpoch;
				command.expectedConfigurationRevision = 1U;
				command.ownerToken = acquired.ownerToken;
				command.producerId = producer;
				command.sequence = sequence;
				command.correlationId = producer * 100U + sequence;
				command.intentKind = IntentKind::Navigation;
				command.requestedSpeed = 3.0;
				command.lifetime = IntentLifetime::UntilStopped;
				return command;
			}

			Object Owner() const { return owner_; }
			Object Target() const { return target_; }
			LocomotionSystem& Locomotion() { return locomotion_; }
			NavigationAgentSystem& Agents() { return agents_; }
			NavigationAgentIdentity Identity() const { return identity_; }
			LocomotionHandle Handle() const { return *handle_; }

		private:
			Physics::PhysicsSystem& physics_ = Physics::PhysicsSystem::Instance();
			NavigationSystem& navigation_ = NavigationSystem::Instance();
			NavigationAgentSystem& agents_ = NavigationAgentSystem::Instance();
			LocomotionSystem& locomotion_ = LocomotionSystem::Instance();
			Object owner_{};
			Object target_{};
			NavigationAgentIdentity identity_{};
			std::optional<LocomotionHandle> handle_;
		};

		const Scheduling::FrameWorkloadDeclaration* FindWorkload(
			const std::vector<Scheduling::FrameWorkloadDeclaration>& workloads, std::string_view id)
		{
			const auto found = std::find_if(workloads.begin(), workloads.end(), [id](const auto& item) {
				return item.stableDescriptorId == id;
			});
			return found == workloads.end() ? nullptr : &*found;
		}

		void RunPrepareAndPublish(LocomotionSystem& system,
			const std::vector<Scheduling::FrameWorkloadDeclaration>& workloads)
		{
			Scheduling::JobGraphContext context{};
			for(std::size_t shard = 0U; shard < 8U; ++shard) {
				const std::string id = "LocomotionSystem/Locomotion/Prepare/" + std::to_string(shard);
				if(const auto* prepare = FindWorkload(workloads, id); prepare != nullptr && prepare->work)
					prepare->work(context);
			}
			if(const auto* publish = FindWorkload(workloads, "LocomotionSystem/Locomotion/Publish");
				publish != nullptr && publish->work) publish->work(context);
			(void)system;
		}
	}

	void RegisterLocomotionNavigationBoundaryTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.NavigationBoundary.MissingGuidanceIsNeutral", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context)) return;
			const auto acquired = fixture.Acquire(870U);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The owner must acquire its physical controller before following a route.");
			if(acquired.ownerToken == 0U) return;
			LocomotionCommand follow = fixture.Follow(acquired, 870U, 2U);
			const auto admitted = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			fixture.Locomotion().Update(1.0F / 60.0F);
			const auto observations = fixture.Locomotion().CopyObservations();
			context.Expect(admitted.queued && !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable()
				&& observations != nullptr && observations->entries.size() == 1U
				&& observations->entries.front().desiredVelocity == glm::dvec3(0.0)
				&& observations->entries.front().navigationReason != LocomotionReason::None,
				"A Navigation intent without current usable guidance must publish a neutral target and an explicit navigation reason.");
		});

		registry.Add("Engine.Locomotion.NavigationBoundary.StaleGuidanceAfterCaptureCannotPublish", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context)) return;
			if(!fixture.RequestTarget(8.0, 871U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(871U);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The route-following owner must hold the controller lease.");
			if(acquired.ownerToken == 0U) return;
			LocomotionCommand follow = fixture.Follow(acquired, 871U, 2U);
			const auto admitted = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			const auto workloads = fixture.Locomotion().DeclareFrameWorkloads(1.0F / 60.0F);
			Scheduling::JobGraphContext captureContext{};
			const auto* capture = FindWorkload(workloads, "LocomotionSystem/Locomotion/Capture");
			context.Expect(capture != nullptr && static_cast<bool>(capture->work),
				"The stale-guidance contract needs the declared Capture boundary.");
			if(capture == nullptr || !capture->work) return;
			const double beforeStalePublish = fixture.CharacterX();
			capture->work(captureContext);

			// The target is moved after locomotion Capture but before its protected
			// Publish. Navigation reports the changed endpoint at its own boundary,
			// invalidating the copied guidance evidence.
			auto* transform = ObjectManager::instance().GetComponent<Transform>(fixture.Target());
			if(transform != nullptr) {
				Scene::WorldPose moved = transform->worldPose;
				moved.position.offsetMetres.x = 10.0;
				(void)ObjectManager::instance().SetCanonicalWorldPose(fixture.Target(), moved);
				ObjectManager::instance().MarkObjectTransformDirty(fixture.Target());
			}
			const auto world = NavigationSystem::Instance().PublishedWorld();
			fixture.Agents().ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world != nullptr ? world->rootGeneration + 1U : 2U);
			RunPrepareAndPublish(fixture.Locomotion(), workloads);
			const auto observations = fixture.Locomotion().CopyObservations();
			fixture.AdvancePhysics();
			const double afterStalePublish = fixture.CharacterX();
			context.Expect(admitted.queued && transform != nullptr
				&& !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable()
				&& observations != nullptr && observations->entries.size() == 1U
				&& observations->entries.front().desiredVelocity == glm::dvec3(0.0)
				&& observations->entries.front().navigationReason != LocomotionReason::None
				&& std::isfinite(beforeStalePublish) && std::isfinite(afterStalePublish)
				&& std::abs(afterStalePublish - beforeStalePublish) <= 1.0e-6,
				"Guidance invalidated after Capture must be revalidated before Publish, clear its stale target, and cause no subsequent physical movement.");
		});

		registry.Add("Engine.Locomotion.NavigationBoundary.ProfileChangeAfterCaptureCannotPublish", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context)) return;
			if(!fixture.RequestTarget(8.0, 874U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(874U);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"The route-following owner must acquire the controller before the configuration boundary test.");
			if(acquired.ownerToken == 0U) return;
			LocomotionCommand follow = fixture.Follow(acquired, 874U, 2U);
			const auto admitted = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			const auto workloads = fixture.Locomotion().DeclareFrameWorkloads(1.0F / 60.0F);
			Scheduling::JobGraphContext captureContext{};
			const auto* capture = FindWorkload(workloads, "LocomotionSystem/Locomotion/Capture");
			context.Expect(capture != nullptr && static_cast<bool>(capture->work),
				"The authored-profile guard test needs the declared Capture callback.");
			if(capture == nullptr || !capture->work) return;
			const auto handleBefore = fixture.Locomotion().ResolvePublished(fixture.Handle().objectId);
			capture->work(captureContext);

			// This remains the same NavigationAgent attachment, but its copied
			// traversal profile revision changes after locomotion captured guidance.
			auto* navigation = ObjectManager::instance().GetComponent<NavigationAgentComponent>(fixture.Owner());
			if(navigation != nullptr) {
				navigation->radiusMetres += 0.05;
				ObjectManager::instance().notifyObjectComponentValueChanged(fixture.Owner(),
					NavigationAgentComponent::kComponentName);
			}
			const auto world = NavigationSystem::Instance().PublishedWorld();
			fixture.Agents().ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world != nullptr ? world->rootGeneration + 1U : 2U);
			RunPrepareAndPublish(fixture.Locomotion(), workloads);
			const auto observations = fixture.Locomotion().CopyObservations();
			const auto handleAfter = fixture.Locomotion().ResolvePublished(fixture.Handle().objectId);
			context.Expect(admitted.queued && navigation != nullptr && handleBefore && handleAfter
				&& *handleBefore == fixture.Handle() && *handleAfter == fixture.Handle()
				&& !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable()
				&& observations != nullptr && observations->entries.size() == 1U
				&& observations->entries.front().desiredVelocity == glm::dvec3(0.0)
				&& observations->entries.front().navigationReason != LocomotionReason::None,
				"A profile revision changed after Capture must suppress the route target at Publish without replacing the locomotion identity.");
		});

		registry.Add("Engine.Locomotion.NavigationBoundary.NavigationStopQueuesButLocomotionStopIsImmediate", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context)) return;
			if(!fixture.RequestTarget(8.0, 872U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(872U);
			if(acquired.ownerToken == 0U) {
				context.Expect(false, "The route-following owner must first acquire its controller.");
				return;
			}
			LocomotionCommand follow = fixture.Follow(acquired, 872U, 2U);
			(void)fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			fixture.Locomotion().Update(1.0F / 60.0F);
			const auto moving = fixture.Locomotion().CopyObservations();
			const auto beforeStop = fixture.Agents().ReadStatus(fixture.Identity());
			const auto navStop = fixture.Agents().QueueStop(fixture.Identity(), beforeStop.configurationRevision,
				beforeStop.intentSequence, 873U, 87301U);
			const bool guidanceRemainsUntilAgentAdvance = fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable();

			LocomotionCommand stop{};
			stop.kind = CommandKind::Stop;
			stop.handle = fixture.Handle();
			stop.expectedAuthorityEpoch = acquired.authorityEpoch;
			stop.expectedConfigurationRevision = 1U;
			stop.ownerToken = acquired.ownerToken;
			stop.producerId = 872U;
			stop.sequence = 3U;
			stop.correlationId = 87203U;
			const auto stopAdmission = fixture.Locomotion().SubmitBatch(std::span(&stop, 1U));
			fixture.Locomotion().Update(1.0F / 60.0F);
			const auto stopped = fixture.Locomotion().CopyObservations();
			const auto stopReceipt = fixture.Locomotion().ReadReceipt(fixture.Handle(), stop.producerId,
				stop.sequence, stop.correlationId);
			context.Expect(navStop.state == NavigationAgentReceiptState::Queued
				&& guidanceRemainsUntilAgentAdvance && moving != nullptr && moving->entries.size() == 1U
				&& stopAdmission.queued && stopReceipt.state == ReceiptState::Applied
				&& stopped != nullptr && stopped->entries.size() == 1U
				&& stopped->entries.front().desiredVelocity == glm::dvec3(0.0)
				&& stopped->entries.front().authority.state == AuthorityState::Stopped,
				"A queued NavigationAgent Stop must wait for its agent boundary, while the owner's Locomotion Stop clears the target in the same protected update.");
			fixture.AdvanceNavigation();
			const auto navStopReceipt = fixture.Agents().ReadReceipt(fixture.Identity(), 87301U);
			context.Expect(navStopReceipt && navStopReceipt->state == NavigationAgentReceiptState::Applied
				&& !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable(),
				"After the NavigationAgent update applies its queued Stop, its prior route guidance must be unusable.");
		});

		const auto runFixedDestinationArrival = [](TestContext& context, float deltaTime, std::uint64_t seed) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context)) return;
			constexpr double destinationX = 3.0;
			if(!fixture.RequestLocation(destinationX, seed)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(seed);
			context.Expect(acquired.state == ReceiptState::Applied && acquired.ownerToken != 0U,
				"A fixed-destination route needs a successfully leased controller.");
			if(acquired.ownerToken == 0U) return;
			LocomotionCommand follow = fixture.Follow(acquired, seed, 2U);
			const auto admission = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			bool arrived = false;
			bool moved = false;
			bool overshot = false;
			double previousX = fixture.CharacterX();
			double maximumX = previousX;
			for(std::uint32_t frame = 0U; frame < 360U && !arrived; ++frame) {
				fixture.Locomotion().Update(deltaTime);
				fixture.AdvancePhysics(deltaTime);
				fixture.AdvanceNavigation(deltaTime);
				const double currentX = fixture.CharacterX();
				if(std::isfinite(currentX)) {
					moved = moved || currentX > previousX + 1.0e-5;
					maximumX = std::max(maximumX, currentX);
					previousX = currentX;
					overshot = overshot || currentX > destinationX + 1.0e-5;
				}
				arrived = fixture.Agents().ReadStatus(fixture.Identity()).state == NavigationAgentState::Arrived;
			}
			// Keep stepping after observed arrival to prove the controller does not
			// continue creeping past the final support point once guidance is retired.
			for(std::uint32_t frame = 0U; frame < 8U && arrived; ++frame) {
				fixture.Locomotion().Update(deltaTime);
				fixture.AdvancePhysics(deltaTime);
				fixture.AdvanceNavigation(deltaTime);
				const double currentX = fixture.CharacterX();
				if(std::isfinite(currentX)) {
					maximumX = std::max(maximumX, currentX);
					overshot = overshot || currentX > destinationX + 1.0e-5;
				}
			}
			const double finalX = fixture.CharacterX();
			const auto finalStatus = fixture.Agents().ReadStatus(fixture.Identity());
			const std::string failureDetail = " (dt=" + std::to_string(deltaTime)
				+ ", moved=" + std::to_string(moved)
				+ ", arrived=" + std::to_string(arrived)
				+ ", finalX=" + std::to_string(finalX)
				+ ", maxX=" + std::to_string(maximumX)
				+ ", overshot=" + std::to_string(overshot)
				+ ", status=" + std::string(ToString(finalStatus.state)) + ")";
			context.Expect(admission.queued && moved && arrived
				&& finalStatus.state == NavigationAgentState::Arrived
				&& std::isfinite(finalX) && std::abs(destinationX - finalX) <= 0.25001
				&& maximumX <= destinationX + 1.0e-5 && !overshot,
				"Repeated Navigation-to-Locomotion-to-Physics updates must reach the fixed destination within authored arrival tolerance, report observed Arrived, and never overshoot."
					+ failureDetail);
		};
		registry.Add("Engine.Locomotion.NavigationBoundary.FixedDestinationArrivalAt30Hz",
			[runFixedDestinationArrival](TestContext& context) { runFixedDestinationArrival(context, 1.0F / 30.0F, 875U); });
		registry.Add("Engine.Locomotion.NavigationBoundary.FixedDestinationArrivalAt60Hz",
			[runFixedDestinationArrival](TestContext& context) { runFixedDestinationArrival(context, 1.0F / 60.0F, 876U); });
		registry.Add("Engine.Locomotion.NavigationBoundary.FixedDestinationArrivalAt120Hz",
			[runFixedDestinationArrival](TestContext& context) { runFixedDestinationArrival(context, 1.0F / 120.0F, 877U); });

		registry.Add("Engine.Locomotion.NavigationBoundary.MovingObjectTargetKeepsRouteAndMovement", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context) || !fixture.RequestTarget(6.0, 878U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(878U);
			if(acquired.ownerToken == 0U) {
				context.Expect(false, "The moving-target route must acquire its controller before follow starts.");
				return;
			}
			LocomotionCommand follow = fixture.Follow(acquired, 878U, 2U);
			const auto admission = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			for(std::uint32_t frame = 0U; frame < 24U; ++frame) {
				fixture.Locomotion().Update(1.0F / 60.0F);
				fixture.AdvancePhysics();
				fixture.AdvanceNavigation();
			}
			const double beforeRetarget = fixture.CharacterX();
			const auto beforeGuidance = fixture.Agents().ReadGuidance(fixture.Identity());
			const auto beforeStatus = fixture.Agents().ReadStatus(fixture.Identity());
			fixture.MoveTarget(9.0);
			fixture.Agents().ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				beforeGuidance.publicationGeneration + 1U);
			const bool guidanceClearedOnMove = !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable();
			bool resumedForNewEndpoint = false;
			bool arrived = false;
			for(std::uint32_t frame = 0U; frame < 600U; ++frame) {
				fixture.Locomotion().Update(1.0F / 60.0F);
				fixture.AdvancePhysics();
				fixture.AdvanceNavigation();
				const auto guidance = fixture.Agents().ReadGuidance(fixture.Identity());
				if(guidance.IsUsable() && guidance.destination.kind == NavigationDestinationKind::Object
					&& guidance.destination.object == beforeGuidance.destination.object) {
					resumedForNewEndpoint = true;
				}
				if(fixture.Agents().ReadStatus(fixture.Identity()).state == NavigationAgentState::Arrived) {
					arrived = true;
					break;
				}
			}
			const double finalX = fixture.CharacterX();
			const auto finalStatus = fixture.Agents().ReadStatus(fixture.Identity());
			const auto finalGuidance = fixture.Agents().ReadGuidance(fixture.Identity());
			const std::string failureDetail = " (beforeX=" + std::to_string(beforeRetarget)
				+ ", finalX=" + std::to_string(finalX)
				+ ", targetX=9, guidanceCleared=" + std::to_string(guidanceClearedOnMove)
				+ ", resumed=" + std::to_string(resumedForNewEndpoint)
				+ ", arrived=" + std::to_string(arrived)
				+ ", state=" + std::string(ToString(finalStatus.state))
				+ ", outcome=" + std::string(ToString(finalStatus.outcome))
				+ ", reason=" + std::string(ToString(finalStatus.reason))
				+ ", guidanceUsable=" + std::to_string(finalGuidance.IsUsable())
				+ ", remaining=" + std::to_string(finalGuidance.remainingDistanceMetres)
				+ ", intentSequence=" + std::to_string(beforeStatus.intentSequence)
				+ "->" + std::to_string(finalStatus.intentSequence) + ")";
			context.Expect(admission.queued && beforeGuidance.IsUsable()
				&& beforeGuidance.destination.kind == NavigationDestinationKind::Object
				&& std::isfinite(beforeRetarget) && guidanceClearedOnMove
				&& fixture.Agents().HasLiveIdentity(fixture.Identity())
				&& beforeStatus.intentSequence == finalStatus.intentSequence && resumedForNewEndpoint
				&& arrived && finalStatus.state == NavigationAgentState::Arrived
				&& std::isfinite(finalX) && finalX > beforeRetarget + 0.1
				&& std::abs(finalX - 9.0) <= 0.25001 && finalX <= 9.0 + 1.0e-5,
				"Moving an active object destination must invalidate old advice, resume the same destination identity, continue physical movement, and arrive without overshoot at its new pose."
					+ failureDetail);
		});

		registry.Add("Engine.Locomotion.NavigationBoundary.UnavailableTerrainAfterCaptureClearsPhysicalTarget", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context) || !fixture.RequestTarget(8.0, 879U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(879U);
			if(acquired.ownerToken == 0U) {
				context.Expect(false, "Terrain invalidation needs an active leased route follower.");
				return;
			}
			LocomotionCommand follow = fixture.Follow(acquired, 879U, 2U);
			const auto admission = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			fixture.Locomotion().Update(1.0F / 60.0F);
			const auto moving = fixture.Locomotion().CopyObservations();
			fixture.AdvancePhysics();
			const double beforeFence = fixture.CharacterX();
			const auto workloads = fixture.Locomotion().DeclareFrameWorkloads(1.0F / 60.0F);
			Scheduling::JobGraphContext captureContext{};
			const auto* capture = FindWorkload(workloads, "LocomotionSystem/Locomotion/Capture");
			context.Expect(capture != nullptr && static_cast<bool>(capture->work),
				"Terrain invalidation must cross the declared Locomotion capture/publication boundary.");
			if(capture == nullptr || !capture->work) return;
			capture->work(captureContext);
			const bool unavailablePublished = NavigationSystem::Instance().PublishUnavailableTerrain(nullptr, true);
			const auto world = NavigationSystem::Instance().PublishedWorld();
			fixture.Agents().ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world != nullptr ? world->rootGeneration + 1U : 2U);
			RunPrepareAndPublish(fixture.Locomotion(), workloads);
			const auto observations = fixture.Locomotion().CopyObservations();
			fixture.AdvancePhysics();
			const double afterFence = fixture.CharacterX();
			context.Expect(admission.queued && moving != nullptr && moving->entries.size() == 1U
				&& moving->entries.front().desiredVelocity.x > 0.0 && unavailablePublished
				&& !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable()
				&& observations != nullptr && observations->entries.size() == 1U
				&& observations->entries.front().desiredVelocity == glm::dvec3(0.0)
				&& observations->entries.front().navigationReason != LocomotionReason::None
				&& std::isfinite(beforeFence) && std::isfinite(afterFence)
				&& std::abs(afterFence - beforeFence) <= 1.0e-6,
				"Publishing unavailable terrain after Locomotion Capture must revoke route advice at Publish and prevent the prior physical contribution from moving the character again.");
		});

		registry.Add("Engine.Locomotion.NavigationBoundary.AgentAttachmentRemovedAfterCaptureCannotPublish", [](TestContext& context) {
			NavigationLocomotionFixture fixture;
			if(!fixture.Start(context) || !fixture.RequestTarget(8.0, 880U)) {
				context.Expect(false, fixture.GuidanceFailure());
				return;
			}
			const auto acquired = fixture.Acquire(880U);
			if(acquired.ownerToken == 0U) {
				context.Expect(false, "Attachment removal needs an active leased route follower.");
				return;
			}
			LocomotionCommand follow = fixture.Follow(acquired, 880U, 2U);
			const auto admission = fixture.Locomotion().SubmitBatch(std::span(&follow, 1U));
			fixture.Locomotion().Update(1.0F / 60.0F);
			const auto moving = fixture.Locomotion().CopyObservations();
			fixture.AdvancePhysics();
			const double beforeRemoval = fixture.CharacterX();
			const auto workloads = fixture.Locomotion().DeclareFrameWorkloads(1.0F / 60.0F);
			Scheduling::JobGraphContext captureContext{};
			const auto* capture = FindWorkload(workloads, "LocomotionSystem/Locomotion/Capture");
			context.Expect(capture != nullptr && static_cast<bool>(capture->work),
				"Navigation attachment removal must occur after the declared locomotion Capture boundary.");
			if(capture == nullptr || !capture->work) return;
			const auto retainedHandle = fixture.Locomotion().ResolvePublished(fixture.Handle().objectId);
			capture->work(captureContext);
			const std::uint64_t componentGenerationBeforeRemoval =
				ObjectManager::instance().getObjectComponentGeneration(fixture.Owner());
			ObjectManager::instance().RemoveComponent<NavigationAgentComponent>(fixture.Owner());
			const std::uint64_t componentGenerationAfterRemoval =
				ObjectManager::instance().getObjectComponentGeneration(fixture.Owner());
			const auto world = NavigationSystem::Instance().PublishedWorld();
			fixture.Agents().ObserveProtectedBoundary(NavigationProtectedBoundary::FrameEnd,
				world != nullptr ? world->rootGeneration + 1U : 2U);
			RunPrepareAndPublish(fixture.Locomotion(), workloads);
			const auto observations = fixture.Locomotion().CopyObservations();
			fixture.AdvancePhysics();
			const double afterRemoval = fixture.CharacterX();
			const auto resolvedAfterRemoval = fixture.Locomotion().ResolvePublished(fixture.Handle().objectId);
			const bool oldObservationAbsent = observations == nullptr
				|| std::none_of(observations->entries.begin(), observations->entries.end(), [handle = fixture.Handle()](const auto& item) {
					return item.handle == handle;
				});
			const std::string failureDetail = " (componentGeneration="
				+ std::to_string(componentGenerationBeforeRemoval) + "->"
				+ std::to_string(componentGenerationAfterRemoval)
				+ ", resolvedAfter=" + std::to_string(resolvedAfterRemoval.has_value())
				+ ", oldObservationAbsent=" + std::to_string(oldObservationAbsent)
				+ ", observations=" + std::to_string(observations ? observations->entries.size() : 0U)
				+ ", pose=" + std::to_string(beforeRemoval) + "->" + std::to_string(afterRemoval) + ")";
			context.Expect(admission.queued && moving != nullptr && moving->entries.size() == 1U
				&& moving->entries.front().desiredVelocity.x > 0.0
				&& componentGenerationAfterRemoval > componentGenerationBeforeRemoval && retainedHandle
				&& *retainedHandle == fixture.Handle() && !resolvedAfterRemoval && oldObservationAbsent
				&& !fixture.Agents().ReadGuidance(fixture.Identity()).IsUsable()
				&& std::isfinite(beforeRemoval) && std::isfinite(afterRemoval)
				&& std::abs(afterRemoval - beforeRemoval) <= 1.0e-6,
				"Removing the captured NavigationAgent attachment must revoke the old locomotion identity and its observation, clear guidance, and prevent the previously published physical target from moving the controller."
					+ failureDetail);
		});
	}
}
