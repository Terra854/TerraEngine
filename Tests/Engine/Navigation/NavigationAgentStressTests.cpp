// Bounded native agent qualification contracts.
//
// These tests use the value-only store, command queue, capture packet, and
// preparation seams. They retain no live ObjectManager pointer in worker data
// and count every admitted record so overload cannot be hidden by a summary.
#include "../TestHarness.h"

#include "Navigation/Agents/NavigationAgentCapture.h"
#include "Navigation/Agents/NavigationAgentCommandQueue.h"
#include "Navigation/Agents/NavigationAgentPreparation.h"
#include "Navigation/Agents/NavigationAgentStore.h"
#include "Navigation/Agents/NavigationAgentSystem.h"
#include "Navigation/Geometry/NavigationGeometrySnapshot.h"
#include "Navigation/NavigationSystem.h"
#include "Core/Object/ComponentRegistry.h"
#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/EcsQueryTaskGraph.h"
#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Components/NavigationAgentComponent.h"
#include "Components/Transform.h"
#include "ThreadPool/ThreadPool.h"

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	using namespace TerraEngine::Navigation;

	namespace
	{
		NavigationAgentIdentity Identity(std::uint32_t index)
		{
			NavigationAgentIdentity identity{};
			identity.world = { NavigationWorldId{ 901U }, 4U };
			identity.object.lifetimeGeneration = 1U;
			identity.object.objectId[0] = static_cast<std::uint8_t>(index & 0xffU);
			identity.object.objectId[1] = static_cast<std::uint8_t>((index >> 8U) & 0xffU);
			identity.object.objectId[2] = static_cast<std::uint8_t>((index >> 16U) & 0xffU);
			identity.object.objectId[3] = static_cast<std::uint8_t>((index >> 24U) & 0xffU);
			identity.attachmentGeneration = 1U;
			// Queue/preparation packets must carry a complete runtime handle. The
			// store replaces this provisional slot when it admits a live record.
			identity.runtimeSlot = 0U;
			identity.runtimeSlotGeneration = 1U;
			return identity;
		}

		NavigationAgentCommand Destination(const NavigationAgentIdentity& identity,
			std::uint64_t correlation, double x)
		{
			NavigationAgentCommand command{};
			command.identity = identity;
			command.kind = NavigationAgentCommandKind::SetLocationDestination;
			command.destination.kind = NavigationDestinationKind::Location;
			command.destination.location.world = identity.world;
			command.destination.location.position.offsetMetres = { x, 0.0, 0.0 };
			command.correlationId = correlation;
			command.producerOrderingToken = correlation;
			return command;
		}

		NavigationAgentRuntimeRecord Runtime(const NavigationAgentIdentity& identity,
			std::uint32_t index)
		{
			NavigationAgentRuntimeRecord runtime{};
			runtime.identity = identity;
			runtime.identity.object.lifetimeGeneration = 1U;
			runtime.intentSequence = index + 1U;
			runtime.destination.kind = NavigationDestinationKind::Location;
			runtime.destination.location.world = identity.world;
			runtime.destination.location.position.offsetMetres = { 10.0, 0.0, 0.0 };
			return runtime;
		}

		NavigationAgentCandidate PrepareFixture(std::uint32_t index)
		{
			const NavigationAgentIdentity identity = Identity(index + 1U);
			NavigationAgentCapture capture{};
			capture.runtime = Runtime(identity, index);
			capture.owner.object = identity.object;
			capture.owner.location.world = identity.world;
			capture.owner.location.position.offsetMetres = { 0.0, 0.0, 0.0 };
			capture.owner.transformRevision = index + 1U;
			capture.owner.activeInHierarchy = true;
			capture.routePoints = { capture.runtime.destination.location };
			capture.routeUsable = true;
			capture.observation = { 1U, index + 1U, index + 1U, 2U, 3U, 4U };
			capture.publicationGeneration = 2U;
			capture.routeGeneration = 3U;
			return PrepareNavigationAgent(capture);
		}

		std::shared_ptr<const NavigationGeometrySnapshot> MakeRouteStressTerrain()
		{
			// This small flat mesh keeps every route in one complete coverage group.
			// The test is about scheduler packet fan-out, not obstacle path quality.
			NavigationGeometryDocument document{};
			document.kind = NavigationGeometryPayloadKind::TriangleMesh;
			document.vertices = {{1.0F, 0.0F, 1.0F}, {31.0F, 0.0F, 31.0F},
				{31.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 31.0F}};
			document.triangles = {{0U, 1U, 2U}, {0U, 3U, 1U}};

			NavigationGeometrySnapshotDescriptor descriptor{};
			descriptor.sourceRevision = 1U;
			descriptor.canonicalPlacementRevision = 1U;
			descriptor.physicalReadyRevision = 1U;
			descriptor.sourceKind = NavigationSourceKind::Terrain;
			const auto created = NavigationGeometrySnapshot::Create(descriptor, document);
			return created.accepted ? created.snapshot : nullptr;
		}

		bool PublishRouteStressTerrain(NavigationSystem& navigation,
			const NavigationProfile& profile,
			const std::shared_ptr<const NavigationGeometrySnapshot>& terrain)
		{
			if(!terrain || !navigation.QueueInitialGroundBuild(profile, {terrain})) {
				return false;
			}
			for(std::uint32_t frame = 0U; frame < 16U; ++frame) {
				navigation.Update(1.0F / 60.0F);
				const auto world = navigation.PublishedWorld();
				if(world && !world->coverage.empty()) {
					return true;
				}
			}
			return false;
		}

		// The real navigation declarations deliberately name their neighbouring
		// systems. These no-op protected anchors make this focused scheduler fixture
		// satisfy that production dependency contract without emulating either system.
		class SchedulerDependencyAnchor final : public SystemsBase {
		public:
			SchedulerDependencyAnchor(std::string name, std::string phase)
				: name_(std::move(name))
				, phase_(std::move(phase)) {}

			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
			std::string SystemName() override { return name_; }

			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override {
				TerraEngine::Scheduling::SystemDeclaration declaration{};
				declaration.name = name_;
				declaration.phase = phase_;
				declaration.mainThread = true;
				declaration.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
				declaration.profilingLabel = name_ + " test dependency anchor";
				declaration.usesDefaultMetadata = false;
				return declaration;
			}

		private:
			std::string name_;
			std::string phase_;
		};
	}

	void RegisterNavigationAgentStressTests(TestRegistry& registry)
	{
		registry.Add("Navigation.AgentStress.StoreRetainsOneThousandGenerationQualifiedRecords", [](TestContext& context) {
			constexpr std::size_t count = 1000U;
			NavigationAgentStore store(count);
			std::vector<NavigationAgentIdentity> identities;
			identities.reserve(count);
			std::size_t admitted = 0U;
			for(std::uint32_t index = 0U; index < count; ++index) {
				const NavigationAgentIdentity requested = Identity(index + 1U);
				const NavigationAgentStoreCreateResult created = store.Create(requested, Runtime(requested, index));
				if(created.Succeeded()) {
					++admitted;
					identities.push_back(created.identity);
				}
			}
			const NavigationAgentStoreCreateResult overCapacity = store.Create(Identity(2001U), Runtime(Identity(2001U), 2000U));
			context.Expect(admitted == count && store.AliveCount() == count,
				"The bounded runtime store must retain every admitted record in the 1,000-agent qualification cohort.");
			context.Expect(!overCapacity.Succeeded() && overCapacity.status == NavigationHandleStatus::CapacityExceeded,
				"The 1,001st runtime record must reject explicitly without growing storage.");

			const NavigationAgentIdentity oldIdentity = identities.front();
			context.Expect(store.Release(oldIdentity), "A live generation-qualified record must release cleanly.");
			const NavigationAgentStoreCreateResult replacement = store.Create(Identity(3001U), Runtime(Identity(3001U), 3000U));
			context.Expect(replacement.Succeeded() && replacement.identity.runtimeSlot == oldIdentity.runtimeSlot
				&& replacement.identity.runtimeSlotGeneration != oldIdentity.runtimeSlotGeneration
				&& store.Read(oldIdentity).status != NavigationHandleStatus::Alive,
				"Slot reuse must advance generation and prevent an old agent identity from aliasing its replacement.");
		});

		registry.Add("Navigation.AgentStress.QueueAdmitsAndDrainsOneThousandCommandsWithoutLoss", [](TestContext& context) {
			constexpr std::size_t count = 1000U;
			NavigationAgentCommandQueue queue(count + 8U);
			std::size_t admitted = 0U;
			for(std::uint32_t index = 0U; index < count; ++index) {
				const NavigationAgentIdentity identity = Identity(index + 1U);
				const NavigationAgentReceipt receipt = queue.Enqueue(Destination(identity, index + 1U, static_cast<double>(index)));
				admitted += receipt.state == NavigationAgentReceiptState::Queued ? 1U : 0U;
			}
			const std::vector<NavigationAgentCommand> commands = queue.Drain();
			std::size_t receiptsFound = 0U;
			for(std::uint32_t index = 0U; index < count; ++index) {
				const auto receipt = queue.ReadReceipt(Identity(index + 1U), index + 1U);
				receiptsFound += receipt.has_value() && receipt->state == NavigationAgentReceiptState::Queued ? 1U : 0U;
			}
			context.Expect(admitted == count && commands.size() == count && receiptsFound == count,
				"Every admitted destination must have one copied command and one readable receipt in the bounded 1,000-command cohort.");
		});

		registry.Add("Navigation.AgentStress.PreparationRetainsIdentityAndProducesDeterministicGuidance", [](TestContext& context) {
			constexpr std::size_t count = 1000U;
			std::size_t prepared = 0U;
			std::size_t deterministic = 0U;
			for(std::uint32_t index = 0U; index < count; ++index) {
				const NavigationAgentIdentity identity = Identity(index + 1U);
				NavigationAgentCapture capture{};
				capture.runtime = Runtime(identity, index);
				capture.owner.object = identity.object;
				capture.owner.location.world = identity.world;
				capture.owner.location.position.offsetMetres = { 0.0, 0.0, 0.0 };
				capture.owner.transformRevision = index + 1U;
				capture.owner.activeInHierarchy = true;
				capture.routePoints = { capture.runtime.destination.location };
				capture.routeUsable = true;
				capture.observation = { 1U, index + 1U, index + 1U, 2U, 3U, 4U };
				capture.publicationGeneration = 2U;
				capture.routeGeneration = 3U;
				const NavigationAgentCandidate first = PrepareNavigationAgent(capture);
				const NavigationAgentCandidate second = PrepareNavigationAgent(capture);
				const bool valid = first.runtime.identity == identity
					&& first.runtime.state == NavigationAgentState::Following
					&& first.runtime.guidance.IsUsable();
				prepared += valid ? 1U : 0U;
				deterministic += valid && first.runtime.guidance.nextWaypoint == second.runtime.guidance.nextWaypoint
					&& first.runtime.guidance.suggestedDirection == second.runtime.guidance.suggestedDirection ? 1U : 0U;
			}
			context.Expect(prepared == count && deterministic == count,
				"All 1,000 captured immutable records must prepare valid guidance with deterministic identity and direction results.");
		});

		registry.Add("Navigation.AgentStress.StoppedAndInvalidTargetPreparationFailsClosed", [](TestContext& context) {
			NavigationAgentIdentity identity = Identity(4001U);
			NavigationAgentCapture stopped{};
			stopped.runtime = Runtime(identity, 0U);
			stopped.runtime.state = NavigationAgentState::Stopped;
			stopped.owner.object = identity.object;
			stopped.owner.location.world = identity.world;
			stopped.owner.activeInHierarchy = true;
			const NavigationAgentCandidate stoppedCandidate = PrepareNavigationAgent(stopped);
			context.Expect(stoppedCandidate.runtime.state == NavigationAgentState::Stopped
				&& !stoppedCandidate.runtime.guidance.IsUsable(),
				"A stopped intent must never produce usable guidance during preparation.");

			NavigationAgentCapture missingTarget{};
			missingTarget.runtime = Runtime(identity, 1U);
			missingTarget.runtime.destination.kind = NavigationDestinationKind::Object;
			missingTarget.owner = stopped.owner;
			const NavigationAgentCandidate missingCandidate = PrepareNavigationAgent(missingTarget);
			context.Expect(missingCandidate.runtime.state == NavigationAgentState::Failed
				&& missingCandidate.runtime.reason == NavigationReason::InvalidDestination
				&& !missingCandidate.runtime.guidance.IsUsable(),
				"An unavailable captured target must fail closed rather than retrying stale guidance indefinitely.");
		});

		registry.Add("Navigation.AgentStress.OneThousandPreparationPacketsMatchAtOneFourAndEightWorkers", [](TestContext& context) {
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t previousWorkerCount = pool.getThreadCount();
			pool.shutdown();
			struct ThreadPoolRestore final {
				ThreadPool& pool;
				std::size_t workerCount;
				~ThreadPoolRestore() {
					pool.shutdown();
					if(workerCount != 0U) pool.initialize(workerCount);
				}
			} restore{ pool, previousWorkerCount };

			const auto runCorpus = [](std::size_t workerCount) {
				ThreadPool& workerPool = ThreadPool::instance();
				workerPool.initialize(workerCount);
				if(workerPool.getThreadCount() != workerCount) {
					workerPool.shutdown();
					return std::uint64_t{ 0U };
				}
				std::vector<std::future<std::uint64_t>> futures;
				futures.reserve(1000U);
				for(std::uint32_t index = 0U; index < 1000U; ++index) {
					auto task = workerPool.createTask([index]() -> std::uint64_t {
						const NavigationAgentCandidate candidate = PrepareFixture(index);
						if(candidate.runtime.state != NavigationAgentState::Following
							|| !candidate.runtime.guidance.IsUsable()) return 0U;
						return (static_cast<std::uint64_t>(index + 1U) * 0x9e3779b97f4a7c15ULL)
							^ candidate.runtime.guidance.intentSequence;
					});
					futures.push_back(task->getFuture());
					workerPool.enqueue(task, 1U);
				}
				std::uint64_t aggregate = 14695981039346656037ULL;
				bool everyPacketValid = true;
				for(std::future<std::uint64_t>& future : futures) {
					const std::uint64_t result = future.get();
					everyPacketValid = everyPacketValid && result != 0U;
					aggregate ^= result;
					aggregate *= 1099511628211ULL;
				}
				workerPool.shutdown();
				return everyPacketValid ? aggregate : 0U;
			};

			const std::uint64_t oneWorker = runCorpus(1U);
			const std::uint64_t fourWorkers = runCorpus(4U);
			const std::uint64_t eightWorkers = runCorpus(8U);
			context.Expect(oneWorker != 0U && oneWorker == fourWorkers && oneWorker == eightWorkers,
				"All 1,000 captured agent preparation packets must be valid and deterministic across 1, 4, and 8 real workers.");
		});

		registry.Add("Navigation.AgentStress.RealSchedulerTicksOneThousandAgentsAtOneFourAndEightWorkers", [](TestContext& context) {
			// This is intentionally a scheduler test rather than another direct
			// PrepareNavigationAgent loop. Each count exercises the registered
			// capture/worker/publish declarations against real component bindings.
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t originalWorkers = pool.getThreadCount();
			pool.shutdown();
			struct RestorePool final {
				ThreadPool& pool;
				std::size_t workers;
				~RestorePool() {
					pool.shutdown();
					if(workers != 0U) pool.initialize(workers);
				}
			} restore{pool, originalWorkers};

			struct SchedulerEvidence {
				bool configuredWorkerCount = false;
				bool retainedAgents = false;
				bool scheduleValid = false;
				bool agentPrepareExecuted = false;
				std::size_t activeAgentCount = 0U;
				std::size_t authoredComponentCount = 0U;
				std::size_t activeHierarchyCount = 0U;
				std::size_t validSettingsCount = 0U;
				bool publishedWorldValid = false;
				std::string scheduleDiagnostic;
			};
			const auto execute = [](std::size_t workerCount) {
				SchedulerEvidence evidence{};
				ComponentRegistry::RegisterAllComponents();
				ObjectManager& objects = ObjectManager::instance();
				NavigationSystem& navigation = NavigationSystem::Instance();
				NavigationAgentSystem& agents = NavigationAgentSystem::Instance();
				objects.clear();
				navigation.Shutdown();
				agents.Shutdown();
				ThreadPool::instance().initialize(workerCount);
				navigation.Initialize();
				agents.Initialize();
				for(std::uint32_t index = 0U; index < 1000U; ++index) {
					const Object owner = objects.createObject("Scheduler Stress Agent " + std::to_string(index));
					(void)objects.AddComponent<Transform>(owner);
					(void)objects.AddComponent<NavigationAgentComponent>(owner);
				}
				navigation.SetContractTestWorkAvailable(true);
				agents.SetContractTestWorkAvailable(true);
				Scheduling::SetEcsGraphFrameDiagnosticsRequested(true);
				Scheduling::SystemScheduler scheduler;
				SchedulerDependencyAnchor worldSpatial("WorldSpatialSystem", "Scene");
				SchedulerDependencyAnchor scripting("ScriptingSystem", "Scripting");
				// Unit schedulers validate workload metadata by default. This stress
				// case must execute the production declarations through the normal frame
				// entry point after registering their explicit dependency anchors.
				scheduler.SetFrameWorkloadCompatibilityFallbackEnabled(true);
				scheduler.RegisterSystem(worldSpatial);
				scheduler.RegisterSystem(navigation);
				scheduler.RegisterSystem(agents);
				scheduler.RegisterSystem(scripting);
				const Scheduling::ScheduleSummary& builtSchedule = scheduler.BuildSchedule();
				evidence.scheduleDiagnostic = builtSchedule.formatted;
				for(std::uint32_t frame = 0U; frame < 3U; ++frame) {
					(void)scheduler.ExecuteFrame(1.0F / 60.0F,
						[](const Scheduling::MutationFlushPoint&) {
							return Scheduling::MutationFlushRuntimeState{};
						});
				}
				const Scheduling::FrameExecutionPlan& executedPlan = scheduler.LatestFrameExecutionPlan();
				const auto diagnostics = agents.PublishedDiagnostics();
				if(const auto world = navigation.PublishedWorld()) {
					evidence.publishedWorldValid = world->world.IsValid();
				}
				evidence.configuredWorkerCount = ThreadPool::instance().getThreadCount() == workerCount;
				evidence.activeAgentCount = diagnostics ? diagnostics->activeAgents : 0U;
				// A single-component EnTT view does not expose size_hint on every
				// supported version, so count the actual authored attachments directly.
				for(const entt::entity entity : objects.getRegistry().view<NavigationAgentComponent>()) {
					++evidence.authoredComponentCount;
					const std::optional<Object> owner = objects.getObject(entity);
					const NavigationAgentComponent* component = objects.getRegistry().try_get<NavigationAgentComponent>(entity);
					evidence.activeHierarchyCount += owner && objects.isObjectActiveInHierarchy(*owner) ? 1U : 0U;
					evidence.validSettingsCount += component && component->ValidateAuthoredValues() ? 1U : 0U;
				}
				evidence.retainedAgents = evidence.activeAgentCount == 1000U;
				evidence.scheduleValid = builtSchedule.valid && scheduler.LatestSummary().valid
					&& executedPlan.valid && executedPlan.completed;
				evidence.agentPrepareExecuted = std::any_of(executedPlan.results.begin(),
					executedPlan.results.end(), [](const Scheduling::FrameWorkloadResult& result) {
						return result.declaration.stableDescriptorId.starts_with("NavigationAgentSystem/Navigation/Prepare")
							&& result.completed && !result.fallbackUsed;
					});
				Scheduling::SetEcsGraphFrameDiagnosticsRequested(false);
				navigation.SetContractTestWorkAvailable(false);
				agents.SetContractTestWorkAvailable(false);
				agents.Shutdown();
				navigation.Shutdown();
				objects.clear();
				ThreadPool::instance().shutdown();
				return evidence;
			};

			const SchedulerEvidence oneWorker = execute(1U);
			const SchedulerEvidence fourWorkers = execute(4U);
			const SchedulerEvidence eightWorkers = execute(8U);
			context.Expect(oneWorker.configuredWorkerCount && fourWorkers.configuredWorkerCount && eightWorkers.configuredWorkerCount,
				"The real scheduler stress fixture must configure exactly 1, 4, and 8 workers before each tick corpus.");
			context.Expect(oneWorker.scheduleValid && fourWorkers.scheduleValid && eightWorkers.scheduleValid,
				"The registered NavigationSystem and NavigationAgentSystem declarations must produce a valid scheduler frame plan. "
				"One-worker scheduler diagnostics: " + oneWorker.scheduleDiagnostic);
			context.Expect(oneWorker.retainedAgents && fourWorkers.retainedAgents && eightWorkers.retainedAgents,
				"The registered capture boundary must retain all 1,000 active NavigationAgentComponent attachments at every worker count "
				"(1 worker authored/active=" + std::to_string(oneWorker.authoredComponentCount) + "/"
				+ std::to_string(oneWorker.activeAgentCount) + ", 4 workers="
				+ std::to_string(fourWorkers.authoredComponentCount) + "/" + std::to_string(fourWorkers.activeAgentCount)
				+ ", 8 workers=" + std::to_string(eightWorkers.authoredComponentCount) + "/"
				+ std::to_string(eightWorkers.activeAgentCount) + "; 1 worker active/valid/world="
				+ std::to_string(oneWorker.activeHierarchyCount) + "/" + std::to_string(oneWorker.validSettingsCount)
				+ "/" + (oneWorker.publishedWorldValid ? "valid" : "invalid") + ").");
			context.Expect(oneWorker.agentPrepareExecuted && fourWorkers.agentPrepareExecuted && eightWorkers.agentPrepareExecuted,
				"The real scheduler must execute declared agent preparation workloads; the separate preparation corpus proves their worker-pool determinism.");
		});

		registry.Add("Navigation.AgentStress.RoutePreparationPublishesFourSearchesPerConfiguredLaneAtOneFourAndEightWorkers", [](TestContext& context) {
			// Submit more work than the configured search-lane budget and execute one
			// real frame. This proves the scheduler emits eight deterministic ranges
			// with at most four independent searches each, instead of an unbounded
			// serial Detour loop inside a worker.
			constexpr std::size_t routeCount = 96U;
			ThreadPool& pool = ThreadPool::instance();
			const std::size_t originalWorkers = pool.getThreadCount();
			pool.shutdown();
			struct RestorePool final {
				ThreadPool& pool;
				std::size_t workers;
				~RestorePool() {
					pool.shutdown();
					if(workers != 0U) pool.initialize(workers);
				}
			} restore{pool, originalWorkers};

			struct RouteBatchEvidence {
				bool settingsConfigured = false;
				bool scheduleValid = false;
				bool expectedBatchTerminal = false;
				std::size_t workerPrepareRangeCount = 0U;
				std::size_t workerPrepareRangeWidth = 0U;
				std::size_t completed = 0U;
				std::size_t recoveredOriginalRequests = 0U;
				bool capturedRequestCancelled = false;
				bool cancelledRequestTerminal = false;
				std::size_t lastBatch = 0U;
				std::size_t highWaterBatch = 0U;
			};
			const auto execute = [](std::size_t workerCount, std::size_t maximumSearches,
				bool abandonCapturedBatch = false) {
				RouteBatchEvidence evidence{};
				ThreadPool::instance().initialize(workerCount);
				NavigationSystem& navigation = NavigationSystem::Instance();
				navigation.Shutdown();
				const NavigationSettings originalSettings = navigation.SettingsSnapshot();
				NavigationSettings configuredSettings = originalSettings;
				configuredSettings.maxConcurrentSearches = maximumSearches;
				evidence.settingsConfigured = navigation.ConfigureSettings(configuredSettings);
				navigation.Initialize();
				const NavigationProfile profile = NavigationProfile::Defaults();
				const auto terrain = MakeRouteStressTerrain();
				if(!PublishRouteStressTerrain(navigation, profile, terrain)) {
					navigation.Shutdown();
					(void)navigation.ConfigureSettings(originalSettings);
					ThreadPool::instance().shutdown();
					return evidence;
				}

				const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
				if(!world) {
					navigation.Shutdown();
					(void)navigation.ConfigureSettings(originalSettings);
					ThreadPool::instance().shutdown();
					return evidence;
				}
				std::vector<NavigationRequestHandle> requests;
				requests.reserve(routeCount);
				for(std::size_t index = 0U; index < routeCount; ++index) {
					Ground::GroundRouteQuery::Request request{};
					request.owner = {world->world,
						NavigationOwnerId{static_cast<std::uint32_t>(index + 1U)}, 1U};
					request.start.world = world->world;
					request.destination.world = world->world;
					request.start.position.offsetMetres = {4.0, 0.0, 4.0 + static_cast<double>(index % 8U)};
					request.destination.position.offsetMetres = {28.0, 0.0, 4.0 + static_cast<double>(index % 8U)};
					request.profile = profile;
					requests.push_back(navigation.SubmitOneShotRoute(std::move(request)));
				}
				if(abandonCapturedBatch) {
					navigation.SetContractTestWorkAvailable(true);
					const std::vector<Scheduling::FrameWorkloadDeclaration> workloads =
						navigation.DeclareFrameWorkloads(1.0F / 60.0F);
					const auto capture = std::find_if(workloads.begin(), workloads.end(),
						[](const Scheduling::FrameWorkloadDeclaration& workload) {
							return workload.stableDescriptorId == "NavigationSystem/Navigation/Capture";
						});
					Scheduling::JobGraphContext context{};
					if(capture != workloads.end() && capture->work) {
						capture->work(context);
						evidence.capturedRequestCancelled = !requests.empty()
							&& navigation.CancelRequest(requests.front());
					}
				}

				navigation.SetContractTestWorkAvailable(true);
				Scheduling::SystemScheduler scheduler;
				SchedulerDependencyAnchor worldSpatial("WorldSpatialSystem", "Scene");
				SchedulerDependencyAnchor scripting("ScriptingSystem", "Scripting");
				scheduler.RegisterSystem(worldSpatial);
				scheduler.RegisterSystem(navigation);
				scheduler.RegisterSystem(scripting);
				const Scheduling::ScheduleSummary& schedule = scheduler.BuildSchedule();
				(void)scheduler.ExecuteFrame(1.0F / 60.0F,
					[](const Scheduling::MutationFlushPoint&) {
						return Scheduling::MutationFlushRuntimeState{};
					});

				const Scheduling::FrameExecutionPlan& plan = scheduler.LatestFrameExecutionPlan();
				evidence.scheduleValid = schedule.valid && scheduler.LatestSummary().valid
					&& plan.valid && plan.completed;
				const auto prepared = std::find_if(plan.results.begin(), plan.results.end(),
					[](const Scheduling::FrameWorkloadResult& result) {
						return result.declaration.stableDescriptorId == "NavigationSystem/Navigation/Prepare";
					});
				if(prepared != plan.results.end() && prepared->completed && !prepared->fallbackUsed) {
					evidence.workerPrepareRangeCount = prepared->partitionCount;
					evidence.workerPrepareRangeWidth = prepared->declaration.partitionWorkItemCount;
				}
				for(std::size_t index = 0U; index < requests.size(); ++index) {
					const NavigationRequestHandle handle = requests[index];
					const auto read = navigation.ReadRequest(handle);
					if(read.record && IsTerminalNavigationOutcome(read.record->outcome)) {
						++evidence.completed;
					}
					if(index == 0U && read.record
						&& read.record->outcome == NavigationOutcome::Cancelled) {
						evidence.cancelledRequestTerminal = true;
					}
					if(abandonCapturedBatch && index > 0U && index < 4U && read.record
						&& read.record->outcome == NavigationOutcome::Complete) {
						++evidence.recoveredOriginalRequests;
					}
				}
				// Defaults expose eight route lanes with four searches in each. The same
				// immutable packet remains deterministic when fewer physical workers run
				// those logical ranges serially.
				const std::size_t expectedRouteBatch = std::min<std::size_t>(8U, maximumSearches) * 4U;
				evidence.expectedBatchTerminal = evidence.completed == expectedRouteBatch;
				const NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();
				evidence.lastBatch = usage.lastRoutePreparationBatch;
				evidence.highWaterBatch = usage.routePreparationBatchHighWater;

				navigation.SetContractTestWorkAvailable(false);
				navigation.Shutdown();
				(void)navigation.ConfigureSettings(originalSettings);
				ThreadPool::instance().shutdown();
				return evidence;
			};

			const RouteBatchEvidence oneWorker = execute(1U, 8U);
			const RouteBatchEvidence fourWorkers = execute(4U, 8U);
			const RouteBatchEvidence eightWorkers = execute(8U, 8U);
			const RouteBatchEvidence singleSearch = execute(1U, 1U);
			const RouteBatchEvidence cancelledBatch = execute(1U, 1U, true);
			context.Expect(oneWorker.settingsConfigured && fourWorkers.settingsConfigured
				&& eightWorkers.settingsConfigured && singleSearch.settingsConfigured
				&& cancelledBatch.settingsConfigured
				&& oneWorker.scheduleValid && fourWorkers.scheduleValid && eightWorkers.scheduleValid
				&& singleSearch.scheduleValid && cancelledBatch.scheduleValid,
				"The bounded route fan-out fixture must retain a valid protected-capture, worker-prepare, and protected-publish schedule.");
			context.Expect(oneWorker.workerPrepareRangeCount == 8U && fourWorkers.workerPrepareRangeCount == 8U
				&& eightWorkers.workerPrepareRangeCount == 8U && oneWorker.workerPrepareRangeWidth == 4U
				&& fourWorkers.workerPrepareRangeWidth == 4U && eightWorkers.workerPrepareRangeWidth == 4U,
				"Thirty-two route packets must fan out into exactly eight deterministic worker ranges at 1, 4, and 8 workers.");
			context.Expect(oneWorker.expectedBatchTerminal && fourWorkers.expectedBatchTerminal && eightWorkers.expectedBatchTerminal,
				"One scheduler frame must publish exactly one 32-route bounded search batch rather than one request or a long serial queue. "
				"Completed routes at 1/4/8 workers: " + std::to_string(oneWorker.completed) + "/"
				+ std::to_string(fourWorkers.completed) + "/" + std::to_string(eightWorkers.completed) + ".");
			context.Expect(oneWorker.lastBatch == 32U && fourWorkers.lastBatch == 32U
				&& eightWorkers.lastBatch == 32U && oneWorker.highWaterBatch == 32U
				&& fourWorkers.highWaterBatch == 32U && eightWorkers.highWaterBatch == 32U,
				"The protected capture diagnostics must cap each scheduler batch to four requests per configured search lane.");
			context.Expect(singleSearch.expectedBatchTerminal && singleSearch.workerPrepareRangeCount == 1U
				&& singleSearch.workerPrepareRangeWidth == 4U && singleSearch.lastBatch == 4U
				&& singleSearch.highWaterBatch == 4U,
				"A maxConcurrentSearches=1 budget must execute four sequential requests in one range, rather than reducing the per-frame batch to one request.");
			context.Expect(cancelledBatch.capturedRequestCancelled && cancelledBatch.cancelledRequestTerminal
				&& cancelledBatch.recoveredOriginalRequests == 3U && cancelledBatch.completed == 5U
				&& cancelledBatch.lastBatch == 4U
				&& cancelledBatch.highWaterBatch == 4U,
				"An abandoned captured range must requeue its three uncancelled FIFO requests, refill its four-route lane from pending work, and preserve the cancelled request's terminal result.");
		});

		registry.Add("Navigation.AgentStress.CompatibilityUpdateDrainsBoundedRouteBatch", [](TestContext& context) {
			// Hosts that do not install a scheduler still use NavigationSystem::Update.
			// Its serial fallback must retain the same bounded multi-route semantics.
			NavigationSystem& navigation = NavigationSystem::Instance();
			navigation.Shutdown();
			navigation.Initialize();
			const NavigationProfile profile = NavigationProfile::Defaults();
			const auto terrain = MakeRouteStressTerrain();
			context.Expect(PublishRouteStressTerrain(navigation, profile, terrain),
				"The compatibility route fixture must publish flat complete coverage first.");
			const std::shared_ptr<const NavigationWorldRoot> world = navigation.PublishedWorld();
			if(!world) {
				navigation.Shutdown();
				return;
			}

			constexpr std::size_t routeCount = 4U;
			std::vector<NavigationRequestHandle> requests;
			requests.reserve(routeCount);
			for(std::size_t index = 0U; index < routeCount; ++index) {
				Ground::GroundRouteQuery::Request request{};
				request.owner = {world->world,
					NavigationOwnerId{static_cast<std::uint32_t>(index + 1U)}, 1U};
				request.start.world = world->world;
				request.destination.world = world->world;
				request.start.position.offsetMetres = {4.0, 0.0, 4.0 + static_cast<double>(index)};
				request.destination.position.offsetMetres = {28.0, 0.0, 4.0 + static_cast<double>(index)};
				request.profile = profile;
				requests.push_back(navigation.SubmitOneShotRoute(std::move(request)));
			}

			navigation.Update(1.0F / 60.0F);
			std::size_t completed = 0U;
			for(const NavigationRequestHandle handle : requests) {
				const auto read = navigation.ReadRequest(handle);
				if(read.record && IsTerminalNavigationOutcome(read.record->outcome)) {
					++completed;
				}
			}
			const NavigationSystem::StoreUsageSnapshot usage = navigation.PublishedStoreUsage();
			context.Expect(completed == routeCount && usage.lastRoutePreparationBatch == routeCount,
				"Compatibility Update must prepare and publish every request in its bounded route batch, not only the queue head.");
			navigation.Shutdown();
		});
	}
}
