// Test-first scheduler contracts for Character Locomotion.
//
// These checks intentionally use stable resource and phase names instead of
// production constants, so missing scheduler metadata fails visibly while the
// feature is still being implemented.

#include "../TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/Scheduling/JobGraph.h"
#include "Core/Scheduling/WorkerSafety.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Components/CharacterLocomotionComponent.h"
#include "Components/Physics/CharacterControllerComponent.h"
#include "Components/Transform.h"
#include "Locomotion/LocomotionSystem.h"
#include "Physics/PhysicsSystem.h"
#include "Scene/SceneMutationQueue.h"
#include "ThreadPool/ThreadPool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <type_traits>
#include <vector>

namespace TerraEngine::Tests
{
	namespace
	{
		using namespace TerraEngine::Scheduling;

		const SchedulerPhaseDefinition* FindPhase(
			const std::vector<SchedulerPhaseDefinition>& phases, std::string_view name)
		{
			const auto found = std::find_if(phases.begin(), phases.end(), [name](const auto& phase) {
				return phase.name == name;
			});
			return found == phases.end() ? nullptr : &*found;
		}

		bool HasString(const std::vector<std::string>& values, std::string_view expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		const FrameWorkloadDeclaration* FindWorkload(
			const std::vector<FrameWorkloadDeclaration>& workloads, std::string_view descriptor)
		{
			const auto found = std::find_if(workloads.begin(), workloads.end(), [descriptor](const auto& workload) {
				return workload.stableDescriptorId == descriptor;
			});
			return found == workloads.end() ? nullptr : &*found;
		}

		void RunCapturedFrame(const std::vector<FrameWorkloadDeclaration>& workloads)
		{
			JobGraphContext jobContext{};
			if(const auto* capture = FindWorkload(workloads, "LocomotionSystem/Locomotion/Capture");
				capture != nullptr && capture->work) capture->work(jobContext);
			{
				WorkerSafetyScope workerScope("LocomotionCutoffPrepare");
				for(std::size_t shard = 0U; shard < 8U; ++shard) {
					const std::string descriptor = "LocomotionSystem/Locomotion/Prepare/" + std::to_string(shard);
					if(const auto* prepare = FindWorkload(workloads, descriptor);
						prepare != nullptr && prepare->work) prepare->work(jobContext);
				}
			}
			if(const auto* publish = FindWorkload(workloads, "LocomotionSystem/Locomotion/Publish");
				publish != nullptr && publish->work) publish->work(jobContext);
		}

		class BoundLocomotionRuntime final {
		public:
			BoundLocomotionRuntime()
			{
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				auto& physics = Physics::PhysicsSystem::Instance();
				physics.Shutdown();
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
				locomotion_.Shutdown();
				locomotion_.Initialize();
				object_ = ObjectManager::instance().createObject("Locomotion scheduler contract");
				(void)ObjectManager::instance().AddComponent<Transform>(object_);
				(void)ObjectManager::instance().AddComponent<CharacterControllerComponent>(object_);
				(void)ObjectManager::instance().AddComponent<CharacterLocomotionComponent>(object_);
			}

			~BoundLocomotionRuntime()
			{
				locomotion_.Shutdown();
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				ObjectManager::instance().clear();
				ThreadPool::instance().shutdown();
			}

			bool Bind(TestContext& context)
			{
				auto& objects = ObjectManager::instance();
				auto& physics = Physics::PhysicsSystem::Instance();
				context.Expect(physics.StartWorld(), "The scheduler fixture requires a live Physics world.");
				for(std::uint32_t attempt = 0U; attempt < 4U; ++attempt) {
					(void)objects.PublishSceneChangePacket();
					physics.Update(physics.FixedTimeStep());
					const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
						objects, "locomotion-scheduler-physics-flush");
					physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
				}
				locomotion_.ObserveProtectedBoundary();
				const auto handle = locomotion_.ResolvePublished(objects.getUUID(object_));
				context.Expect(handle.has_value(), "A valid live controller must be bound before workload declaration.");
				context.Expect(locomotion_.ReadDiagnostics().liveSlots == 1U,
					"The scheduler fixture must contain exactly one live locomotion slot before capture.");
				return handle.has_value();
			}

			void AdvancePhysics()
			{
				auto& objects = ObjectManager::instance();
				auto& physics = Physics::PhysicsSystem::Instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(physics.FixedTimeStep());
				const auto flushed = Scene::SceneMutationQueue::Instance().Apply(
					objects, "locomotion-scheduler-physics-flush");
				physics.OnAfterPhysicsFlush(flushed.transactionResults, flushed.publicationGroupResults);
			}

			UUID ObjectId() const { return ObjectManager::instance().getUUID(object_); }

			double CharacterX(UUID objectId) const
			{
				const auto packet = Physics::PhysicsSystem::Instance().LatestCharacterStatePacket();
				if(!packet || !packet->states) return std::numeric_limits<double>::quiet_NaN();
				const auto state = std::find_if(packet->states->begin(), packet->states->end(), [objectId](const auto& value) {
					return value.objectId == objectId;
				});
				return state == packet->states->end()
					? std::numeric_limits<double>::quiet_NaN() : state->pose.position.x;
			}

		private:
			Locomotion::LocomotionSystem& locomotion_ = Locomotion::LocomotionSystem::Instance();
			Object object_{};
		};
	}

	void RegisterLocomotionSchedulerTests(TestRegistry& registry)
	{
		registry.Add("Engine.Locomotion.Scheduler.PhaseIsOrder42BetweenScriptingAndAnimation", [](TestContext& context) {
			const std::vector<SchedulerPhaseDefinition> phases = DefaultSchedulerPhases();
			const SchedulerPhaseDefinition* scripting = FindPhase(phases, "Scripting");
			const SchedulerPhaseDefinition* locomotion = FindPhase(phases, "Locomotion");
			const SchedulerPhaseDefinition* animation = FindPhase(phases, "Animation");
			context.Expect(scripting != nullptr && locomotion != nullptr && animation != nullptr,
				"The default scheduler must register Scripting, Locomotion, and Animation phases.");
			if(scripting == nullptr || locomotion == nullptr || animation == nullptr) return;

			context.ExpectEqual(42, locomotion->order,
				"Locomotion must use its committed order 42 between gameplay scripts and animation evaluation.");
			context.Expect(scripting->order < locomotion->order && locomotion->order < animation->order,
				"The Locomotion phase must consume script intent before Animation and Physics use its prepared targets.");
			context.Expect(!locomotion->flushBefore && !locomotion->flushAfter,
				"Locomotion must not introduce a protected global mutation flush; its typed outputs use their declared handoff boundary.");
		});

		registry.Add("Engine.Locomotion.Scheduler.RegistersTypedHandoffsAndEightPreparedShards", [](TestContext& context) {
			const std::vector<SchedulerResourceDefinition> resources = DefaultSchedulerResources();
			const std::array<std::string_view, 14> requiredResources = {
				"LocomotionCaptureInputs",
				"LocomotionPrepared/0", "LocomotionPrepared/1", "LocomotionPrepared/2", "LocomotionPrepared/3",
				"LocomotionPrepared/4", "LocomotionPrepared/5", "LocomotionPrepared/6", "LocomotionPrepared/7",
				"LocomotionAuthority", "LocomotionObservations", "LocomotionReceipts",
				"CharacterMotionTargets", "RootMotionIntervals"
			};
			for(const std::string_view name : requiredResources) {
				const auto count = std::count_if(resources.begin(), resources.end(), [name](const auto& resource) {
					return resource.name == name;
				});
				context.ExpectEqual(1, static_cast<int>(count),
					"Each locomotion capture, prepared shard, authority, observation, receipt, and typed physics handoff must have one distinct scheduler registration.");
			}
		});

		registry.Add("Engine.Locomotion.Scheduler.DoesNotAddGlobalFlushPoints", [](TestContext& context) {
			const std::vector<MutationFlushPoint> flushes = DefaultMutationFlushPoints();
			const std::array<std::string_view, 5> existingFlushNames = {
				"After Scene", "After Scripting", "Before Physics", "After Physics", "Frame End"
			};
			context.ExpectEqual(existingFlushNames.size(), flushes.size(),
				"Locomotion must not add an unconditional global scene-mutation flush point.");
			for(const std::string_view name : existingFlushNames) {
				context.Expect(std::count_if(flushes.begin(), flushes.end(), [name](const auto& flush) {
					return flush.name == name;
				}) == 1,
					"Existing protected mutation flush points must retain their stable identities.");
			}
		});

		registry.Add("Engine.Locomotion.Scheduler.DeclaresProtectedLifecycleOwnership", [](TestContext& context) {
			using TerraEngine::Locomotion::LocomotionSystem;
			static_assert(std::is_base_of_v<SystemsBase, LocomotionSystem>);

			const auto declaration = LocomotionSystem::Instance().SchedulerDeclaration();
			context.ExpectEqual(std::string("LocomotionSystem"), declaration.name,
				"LocomotionSystem must expose a stable scheduler identity.");
			context.ExpectEqual(std::string("Locomotion"), declaration.phase,
				"Locomotion lifecycle reconciliation must be declared in the Locomotion phase.");
			context.Expect(declaration.mainThread
				&& declaration.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit
				&& declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected
				&& declaration.hardBoundary,
				"Live binding reconciliation and lease revocation must remain coordinator-owned protected work.");
			context.Expect(declaration.requiresFlushedSceneMutations,
				"Locomotion membership reconciliation must observe scene mutations at its declared protected boundary.");
			context.Expect(!declaration.usesDefaultMetadata,
				"Locomotion lifecycle effects must be explicit in diagnostics rather than inferred from default metadata.");
			context.Expect(!declaration.protectedCommit.empty() && !declaration.protectedReason.empty(),
				"The declaration must explain its lifecycle commit and why it is protected.");
			for(const std::string_view resource : { "LocomotionAuthority", "LocomotionObservations",
				"LocomotionReceipts", "CharacterMotionTargets", "PhysicsWorld",
				"PhysicsCharacterMotionLeases", "RootMotionIntervals", "RootMotionResults",
				"LocomotionAnimatorBindings", "AnimationConsumerMasks", "AnimationCommandQueues" }) {
				context.Expect(HasString(declaration.writes, resource),
					"Protected lifecycle writes must list every authority, observation, receipt, and Physics handoff resource.");
			}
		});

		registry.Add("Engine.Locomotion.Scheduler.OmitsWorkAndShutsDownWithoutPhysicsWorld", [](TestContext& context) {
			auto& physics = TerraEngine::Physics::PhysicsSystem::Instance();
			if(physics.WorldHandle().IsValid()) {
				context.Expect(false,
					"This lifecycle contract requires no active Physics world; the test will not stop or mutate an existing world.");
				return;
			}

			auto& system = TerraEngine::Locomotion::LocomotionSystem::Instance();
			// Shutdown is safe before or after initialization when no Physics world exists.
			system.Shutdown();
			system.Initialize();
			const auto workloads = system.DeclareFrameWorkloads(1.0F / 60.0F);
			system.Shutdown();
			context.Expect(workloads.empty(),
				"An initialized LocomotionSystem must submit no work when there is no active Physics world.");
			context.Expect(!physics.WorldHandle().IsValid(),
				"The no-world lifecycle check must not create or change Physics world state.");
		});

		registry.Add("Engine.Locomotion.Scheduler.BoundControllerDeclaresStableCaptureEightShardsAndPublish", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			BoundLocomotionRuntime runtime;
			if(!runtime.Bind(context)) return;
			auto& system = TerraEngine::Locomotion::LocomotionSystem::Instance();
			const std::vector<FrameWorkloadDeclaration> first = system.DeclareFrameWorkloads(1.0F / 60.0F);
			const std::vector<FrameWorkloadDeclaration> repeated = system.DeclareFrameWorkloads(1.0F / 60.0F);

			const std::string captureId = "LocomotionSystem/Locomotion/Capture";
			const std::string publishId = "LocomotionSystem/Locomotion/Publish";
			const FrameWorkloadDeclaration* capture = FindWorkload(first, captureId);
			const FrameWorkloadDeclaration* publish = FindWorkload(first, publishId);
			context.ExpectEqual(std::size_t{10U}, first.size(),
				"One active controller must declare capture, eight independent preparation shards, and protected publication.");
			context.Expect(capture != nullptr && publish != nullptr,
				"The bound controller workloads must expose stable Capture and Publish descriptors.");
			if(capture == nullptr || publish == nullptr) return;
			context.Expect(static_cast<bool>(capture->work) && static_cast<bool>(publish->work),
				"Capture and protected Publish descriptors must contain executable callbacks.");

			context.Expect(capture->phase == "Locomotion" && capture->mainThreadRequired
				&& !capture->workerEligible && capture->phaseKind == WorkerSystemPhaseKind::ProtectedCommit,
				"Capture must resolve live ECS/Physics evidence only in its protected boundary.");
			context.Expect(HasString(capture->reads, "LocomotionComponents")
				&& HasString(capture->reads, "LocomotionCommands")
				&& HasString(capture->reads, "PhysicsCharacterStatePackets")
				&& HasString(capture->writes, "LocomotionCaptureInputs"),
				"Capture must declare its component, command, and character-state inputs and copied output.");

			std::array<std::string, 8U> shardIds{};
			std::array<std::string, 8U> shardResources{};
			std::array<const FrameWorkloadDeclaration*, 8U> shards{};
			const std::array<std::string_view, 5U> forbiddenWorkerReads = {
				"ObjectManager", "PhysicsWorld", "PhysicsCharacterStatePackets",
				"LocomotionComponents", "LocomotionCommands"
			};
			for(std::size_t shard = 0U; shard < shards.size(); ++shard) {
				shardIds[shard] = "LocomotionSystem/Locomotion/Prepare/" + std::to_string(shard);
				shardResources[shard] = "LocomotionPrepared/" + std::to_string(shard);
				shards[shard] = FindWorkload(first, shardIds[shard]);
				context.Expect(shards[shard] != nullptr,
					"Every deterministic preparation shard must own a distinct stable descriptor.");
				if(shards[shard] == nullptr) continue;
				const FrameWorkloadDeclaration& prepare = *shards[shard];
				context.Expect(prepare.phase == "Locomotion" && prepare.workerEligible
					&& !prepare.mainThreadRequired && prepare.phaseKind == WorkerSystemPhaseKind::WorkerPrepare
					&& static_cast<bool>(prepare.work)
					&& HasString(prepare.reads, "LocomotionCaptureInputs")
					&& prepare.writes.size() == 1U && HasString(prepare.writes, shardResources[shard])
					&& HasString(prepare.explicitDependencies, captureId),
					"Each Prepare shard must read captured values, depend on Capture, and write only its own immutable shard.");
				for(const std::string_view forbidden : forbiddenWorkerReads)
					context.Expect(!HasString(prepare.reads, forbidden),
						"Worker preparation must not declare live ECS, Physics, or producer-queue resources.");
			}

			context.Expect(publish->phase == "Locomotion" && publish->mainThreadRequired
				&& !publish->workerEligible && publish->phaseKind == WorkerSystemPhaseKind::ProtectedCommit
				&& publish->hardBoundary && HasString(publish->reads, "LocomotionCaptureInputs")
				&& HasString(publish->writes, "LocomotionAuthority")
				&& HasString(publish->writes, "LocomotionObservations")
				&& HasString(publish->writes, "LocomotionReceipts")
				&& HasString(publish->writes, "CharacterMotionTargets"),
				"Publish must validate current generations and publish protected authority, observation, receipt, and target outputs.");
			// Revocation cancels real provider intervals, not just logical tokens.
			for(const auto* boundary : {capture, publish}) {
				for(const std::string_view resource : {"PhysicsWorld", "PhysicsCharacterMotionLeases",
					"RootMotionIntervals", "RootMotionResults"})
					context.Expect(HasString(boundary->writes, resource),
						"Protected lifecycle boundaries must expose provider reset and terminal root-result writes.");
			}
			for(std::size_t shard = 0U; shard < shards.size(); ++shard) {
				context.Expect(HasString(publish->reads, shardResources[shard])
					&& HasString(publish->explicitDependencies, shardIds[shard]),
					"Protected Publish must read and depend on every prepared shard.");
			}

			const auto plan = ValidateFrameWorkloads(first, "locomotion-eight-shard-contract");
			context.Expect(plan.valid,
				"Capture, independent shard writes, and protected publication must form a valid scheduler resource graph.");
			context.Expect(repeated.size() == first.size(),
				"Repeated declaration for unchanged membership must preserve the same workload count.");
			if(repeated.size() == first.size()) {
				for(std::size_t index = 0U; index < first.size(); ++index)
					context.Expect(repeated[index].stableDescriptorId == first[index].stableDescriptorId
						&& repeated[index].reads == first[index].reads
						&& repeated[index].writes == first[index].writes
						&& repeated[index].explicitDependencies == first[index].explicitDependencies,
						"Unchanged controller membership must reproduce descriptor identities, resources, and dependencies.");
			}

			// Execute only the copied-value phase under the runtime worker guard. Any
			// accidental live ObjectManager/provider access from a worker callback
			// would record a WorkerSafety violation.
			JobGraphContext jobContext{};
			if(capture->work) capture->work(jobContext);
			(void)DrainWorkerSafetyViolations();
			{
				WorkerSafetyScope workerScope("LocomotionPrepareContract");
				for(const FrameWorkloadDeclaration* shard : shards)
					if(shard != nullptr && shard->work) shard->work(jobContext);
			}
			const auto violations = DrainWorkerSafetyViolations();
			context.Expect(violations.empty(),
				"Worker Prepare callbacks must not access live ECS or provider APIs.");
		});

		registry.Add("Engine.Locomotion.Scheduler.IntentSubmittedAfterCaptureWaitsForNextFrame", [](TestContext& context) {
			BoundLocomotionRuntime runtime;
			if(!runtime.Bind(context)) return;
			auto& system = TerraEngine::Locomotion::LocomotionSystem::Instance();
			const UUID objectId = runtime.ObjectId();
			const auto handle = system.ResolvePublished(objectId);
			context.Expect(handle.has_value(), "The command-cutoff fixture must resolve its live attachment.");
			if(!handle) return;

			TerraEngine::Locomotion::LocomotionCommand acquire;
			acquire.kind = TerraEngine::Locomotion::CommandKind::Acquire;
			acquire.handle = *handle;
			acquire.expectedConfigurationRevision = 1U;
			acquire.producerId = 712U;
			acquire.sequence = 1U;
			acquire.correlationId = 7121U;
			const auto acquisition = system.SubmitBatch(std::span(&acquire, 1U));
			context.Expect(acquisition.queued,
				"An ownership request queued before Capture must be eligible in that same Locomotion frame.");
			RunCapturedFrame(system.DeclareFrameWorkloads(1.0F / 60.0F));
			const auto acquired = system.ReadReceipt(*handle, acquire.producerId, acquire.sequence, acquire.correlationId);
			context.Expect(acquired.state == TerraEngine::Locomotion::ReceiptState::Applied
				&& acquired.ownerToken != 0U,
				"Protected Publish must apply pre-capture Acquire and return its opaque owner token.");
			if(acquired.ownerToken == 0U) return;

			const auto secondFrame = system.DeclareFrameWorkloads(1.0F / 60.0F);
			JobGraphContext secondFrameContext{};
			const auto* secondCapture = FindWorkload(secondFrame, "LocomotionSystem/Locomotion/Capture");
			context.Expect(secondCapture != nullptr && static_cast<bool>(secondCapture->work),
				"The second frame must expose its Capture callback before intent admission.");
			if(secondCapture == nullptr || !secondCapture->work) return;
			secondCapture->work(secondFrameContext);

			TerraEngine::Locomotion::LocomotionCommand lateIntent;
			lateIntent.kind = TerraEngine::Locomotion::CommandKind::SetIntent;
			lateIntent.handle = *handle;
			lateIntent.expectedAuthorityEpoch = acquired.authorityEpoch;
			lateIntent.expectedConfigurationRevision = 1U;
			lateIntent.ownerToken = acquired.ownerToken;
			lateIntent.producerId = acquire.producerId;
			lateIntent.sequence = 2U;
			lateIntent.correlationId = 7122U;
			lateIntent.directionSpace = TerraEngine::Locomotion::DirectionSpace::World;
			lateIntent.intentKind = TerraEngine::Locomotion::IntentKind::Script;
			lateIntent.direction = {1.0, 0.0, 0.0};
			lateIntent.requestedSpeed = 3.0;
			lateIntent.lifetime = TerraEngine::Locomotion::IntentLifetime::UntilStopped;
			const auto lateAdmission = system.SubmitBatch(std::span(&lateIntent, 1U));
			context.Expect(lateAdmission.queued,
				"A well-formed owner intent submitted after Capture must be accepted for the next frame.");
			const auto* secondPublish = FindWorkload(secondFrame, "LocomotionSystem/Locomotion/Publish");
			for(std::size_t shard = 0U; shard < 8U; ++shard) {
				const std::string descriptor = "LocomotionSystem/Locomotion/Prepare/" + std::to_string(shard);
				if(const auto* prepare = FindWorkload(secondFrame, descriptor);
					prepare != nullptr && prepare->work) prepare->work(secondFrameContext);
			}
			context.Expect(secondPublish != nullptr && static_cast<bool>(secondPublish->work),
				"Protected Publish must follow all second-frame preparation callbacks.");
			if(secondPublish == nullptr || !secondPublish->work) return;
			secondPublish->work(secondFrameContext);
			const auto stillQueued = system.ReadReceipt(*handle, lateIntent.producerId,
				lateIntent.sequence, lateIntent.correlationId);
			context.Expect(stillQueued.state == TerraEngine::Locomotion::ReceiptState::Queued,
				"A command arriving after Capture must remain queued through that frame's Publish.");
			const auto waitingObservation = system.CopyObservations();
			context.Expect(waitingObservation && waitingObservation->entries.size() == 1U
				&& waitingObservation->entries.front().desiredVelocity == glm::dvec3(0.0),
				"A post-capture intent must contribute no target velocity until the following frame.");
			runtime.AdvancePhysics();
			const double beforeMovement = runtime.CharacterX(objectId);
			context.Expect(std::isfinite(beforeMovement),
				"A completed Physics publication must retain the controller pose for movement comparison.");

			RunCapturedFrame(system.DeclareFrameWorkloads(1.0F / 60.0F));
			const auto applied = system.ReadReceipt(*handle, lateIntent.producerId,
				lateIntent.sequence, lateIntent.correlationId);
			const auto movingObservation = system.CopyObservations();
			context.Expect(applied.state == TerraEngine::Locomotion::ReceiptState::Applied
				&& movingObservation && movingObservation->entries.size() == 1U
				&& movingObservation->entries.front().desiredVelocity.x > 0.0,
				"The next Capture/Prepare/Publish frame must apply the queued intent and publish its nonzero target.");
			runtime.AdvancePhysics();
			const double afterMovement = runtime.CharacterX(objectId);
			context.Expect(std::isfinite(beforeMovement) && std::isfinite(afterMovement)
				&& afterMovement > beforeMovement + 1.0e-4,
				"The locomotion target accepted on the next frame must move the real Physics character.");
		});
	}
}
