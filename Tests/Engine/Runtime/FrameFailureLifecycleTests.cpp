#include "../TestHarness.h"
#include "CorrectnessTestSupport.h"

#include "Core/MainEngine.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Resource/ResourceManager.h"
#include "Scene/SceneManager.h"
#include "Scene/TransactionalMutation.h"

#include <atomic>
#include <stdexcept>
#include <type_traits>

namespace TerraEngine::Tests {
	// Live callbacks and the process host retain Engine's address. Reject value
	// transfer at compile time so the ownership ledger cannot be duplicated.
	static_assert(!std::is_copy_constructible_v<Engine> && !std::is_move_constructible_v<Engine>);
	namespace {
		// A real editor system is the existing injection point into Engine's
		// scheduler. It declares isolated resources, so this fault cannot be
		// mistaken for an expected asset or scene-mutation rejection.
		class FailingFrameSystem final : public SystemsBase {
		public:
			explicit FailingFrameSystem(bool worker) : worker_(worker) {}
			void Initialize() override {
				++initializations; ++acquiredOwners;
				if(failInitialization) throw std::runtime_error("Injected fixture initialization failure");
			}
			void Update(float) override { throw std::runtime_error("Unexpected fixture frame failure"); }
			void PrepareShutdown() override { ++drains; poolAliveAtDrain = ThreadPool::instance().getThreadCount() != 0U; independentGate.Cancel(); }
			void Shutdown() override {
				++shutdowns; if(acquiredOwners != 0U) --acquiredOwners;
				if(failShutdown) throw std::runtime_error("Injected fixture shutdown failure");
			}
			std::string SystemName() override { return "FrameFailureFixture"; }
			TerraEngine::Scheduling::SystemDeclaration SchedulerDeclaration() override
			{
				TerraEngine::Scheduling::SystemDeclaration declaration;
				declaration.name = SystemName();
				declaration.phase = "PostFrame";
				declaration.reads = { "Resources" };
				declaration.writes = { "EditorState" };
				if(rollbackFault) declaration.writes.push_back("SceneMutationQueue");
				declaration.mainThread = !worker_;
				declaration.usesDefaultMetadata = false;
				declaration.phaseKind = worker_ ? TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare
					: TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
				declaration.boundaryClassification = worker_ ? TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe
					: TerraEngine::Scheduling::HardBoundaryClassification::Protected;
				declaration.safeSubwork = "Fixture-owned immutable input and counter only.";
				declaration.protectedCommit = rollbackFault ? "Protected typed transaction with rollback before fault observation." : "Fixture fault observation; no live scene writes.";
				declaration.protectedReason = declaration.protectedCommit;
				return declaration;
			}
			std::vector<TerraEngine::Scheduling::FrameWorkloadDeclaration> DeclareFrameWorkloads(float) override
			{
				++declarations;
				TerraEngine::Scheduling::FrameWorkloadDeclaration work;
				work.workloadName = "FrameFailureFixture.Throw";
				work.stableDescriptorId = "frame-failure-fixture/throw";
				work.ownerSystem = SystemName();
				work.phase = "PostFrame";
				work.reads = { "Resources" };
				work.writes = { "EditorState" };
				if(rollbackFault) work.writes.push_back("SceneMutationQueue");
				work.workerEligible = worker_;
				work.estimatedWorkItemCount = 256U;
				work.mainThreadRequired = !worker_;
				work.phaseKind = worker_ ? TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare
					: TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit;
				work.boundaryClassification = worker_ ? TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe
					: TerraEngine::Scheduling::HardBoundaryClassification::Protected;
				work.safeSubwork = "Fixture counter; no live storage access.";
				work.protectedCommit = rollbackFault ? "Protected typed transaction rollback and fixture cleanup before required exception." : "Fixture protected fault, with no scene mutation.";
				work.protectedReason = work.protectedCommit;
				work.work = [this](TerraEngine::Scheduling::JobGraphContext&) {
					++executions;
					if(handledErrors) {
						// A normal protected rejection is a returned result, never an
						// escaped required-job exception. The optional asset is absent.
						TerraEngine::Scene::SceneMutationQueue queue;
						queue.Enqueue({"handled-missing-object", TerraEngine::Scene::SceneMutationOperation::DeleteObject, TerraEngine::GenerateUUID()});
						const auto result = queue.Apply(ObjectManager::instance(), "fixture handled rejection");
						handledMutationRejected = !result.applied && result.rejectedMutationCount != 0U;
						optionalAssetAbsent = !ResourceManager::Instance().GetAudioClip(TerraEngine::GenerateUUID());
						return;
					}
					if(withIndependent) {
						if(!independentGate.WaitForArrivals(1U)) throw std::runtime_error("Independent fixture did not reach its controlled gate");
						independentGate.Release();
					}
					if(rollbackFault) {
						// Exercise the real protected journal before an unexpected
						// required callback failure escapes into Engine's terminal gate.
						auto& objects = ObjectManager::instance();
						Object root = objects.createObject("Frame Rollback Owner");
						const auto rootId = objects.getUUID(root);
						(void)objects.PublishSceneChangePacket();
						const auto generation = objects.GetSceneGeneration();
						const auto revision = objects.GetSpatialRevision();
						struct ClearHook {
							ObjectManager& objects;
							~ClearHook() { objects.SetCreationFailureHookForTesting({}); }
						} clear{objects};
						objects.SetCreationFailureHookForTesting([](ObjectCreationStage stage) {
							if(stage == ObjectCreationStage::Allocated) throw std::runtime_error("Controlled protected factory failure");
						});
						const auto transaction = TerraEngine::Scene::TransactionalMutationBuilder("frame-rollback", TerraEngine::Scene::TransactionOrigin::Test)
							.SetName(rootId, "Aborted Frame Rename").CreateObject(1, "Aborted Frame Creation").Build();
						const auto result = TerraEngine::Scene::CommitTransaction(objects, transaction);
						mutationRolledBack = result.status == TerraEngine::Scene::TransactionStatus::Rejected
							&& objects.getObjectName(root) == "Frame Rollback Owner"
							&& objects.GetSceneGeneration() == generation && objects.GetSpatialRevision() == revision
							&& result.inverseDeltas.empty() && result.createdObjects.empty();
						objects.SetCreationFailureHookForTesting({});
						objects.removeObject(root);
						if(!mutationRolledBack) throw std::runtime_error("Protected mutation failed to restore before frame escape");
					}
					throw std::runtime_error("Unexpected fixture frame failure");
				};
				if(invalidPlan) work.explicitDependencies = {"frame-failure-fixture/nonexistent-prerequisite"};
				if(!withIndependent) return {std::move(work)};
				auto independent = work;
				independent.workloadName = "FrameFailureFixture.Independent";
				independent.stableDescriptorId = "frame-failure-fixture/independent";
				independent.workerEligible = true; independent.mainThreadRequired = false;
				independent.phaseKind = TerraEngine::Scheduling::WorkerSystemPhaseKind::WorkerPrepare;
				independent.boundaryClassification = TerraEngine::Scheduling::HardBoundaryClassification::WorkerSafe;
				independent.writes.clear(); independent.explicitDependencies.clear();
				independent.work = [wait = independentGate.MakeWaiter(), settled = independentSettled](TerraEngine::Scheduling::JobGraphContext&) {
					if(wait() != Correctness::GateResult::Released) throw std::runtime_error("Independent fixture gate was not released");
					settled.Increment();
				};
				// The scheduler attaches the prior phase to the first descriptor
				// of each system. Give both sibling jobs an explicit shared prelude
				// so a protected fault cannot run before the worker is eligible.
				auto prelude = work;
				prelude.workloadName = "FrameFailureFixture.Prelude";
				prelude.stableDescriptorId = "frame-failure-fixture/prelude";
				prelude.workerEligible = false; prelude.mainThreadRequired = true;
				prelude.writes.clear(); prelude.explicitDependencies.clear();
				prelude.work = [](TerraEngine::Scheduling::JobGraphContext&) {};
				independent.explicitDependencies = {prelude.stableDescriptorId};
				work.explicitDependencies = {prelude.stableDescriptorId};
				auto dependent = work;
				dependent.workloadName = "FrameFailureFixture.Dependent";
				dependent.stableDescriptorId = "frame-failure-fixture/dependent";
				dependent.explicitDependencies = {work.stableDescriptorId};
				dependent.work = [this](TerraEngine::Scheduling::JobGraphContext&) { ++dependentExecutions; };
				return {std::move(prelude), std::move(independent), std::move(work), std::move(dependent)};
			}
			std::atomic<unsigned> executions = 0;
			unsigned declarations = 0;
			unsigned initializations = 0;
			unsigned shutdowns = 0;
			unsigned acquiredOwners = 0, drains = 0;
			std::atomic<unsigned> dependentExecutions = 0;
			bool failInitialization = false, invalidPlan = false, withIndependent = false, handledErrors = false;
			bool rollbackFault = false, mutationRolledBack = false, failShutdown = false;
			bool handledMutationRejected = false, optionalAssetAbsent = false, poolAliveAtDrain = false;
			Correctness::ControlledGate independentGate;
			Correctness::CompletionCounter independentSettled;
		private:
			bool worker_;
		};

		void CheckEngineFailure(TestContext& context, bool worker, bool independent = false, bool invalidPlan = false, bool rollbackFault = false)
		{
			Correctness::DisposableFixturePath paths;
			auto& resources = ResourceManager::Instance();
			const auto previousAssetRoot = resources.GetAssetRoot();
			const auto previousEngineRoot = resources.GetEngineRoot();
			resources.SetAssetRoot(paths.Root());
			resources.SetEngineRoot(paths.Root());
			FailingFrameSystem fixture(worker);
			fixture.withIndependent = independent; fixture.invalidPlan = invalidPlan; fixture.rollbackFault = rollbackFault;
			// At least two workers allow a fault and an already-running independent
			// callback to meet at controlled gates without depending on CPU count.
			if(ThreadPool::instance().getThreadCount() < 2U) ThreadPool::instance().shutdown();
			ThreadPool::instance().initialize(2U);
			Engine engine;
			engine.ConfigureHeadlessCpuFallback();
			engine.SetEditorMode(&fixture);
			// Cleanup precedes assertions: even the pre-fix failure must leave no
			// pool or singleton owner alive when the harness reports its result.
			bool firstContinued = true;
			bool secondContinued = true;
			unsigned firstDeclarations = 0;
			bool summaryFailed = false;
			std::optional<EngineFrameFailureReport> firstFailure;
			bool reportStable = false;
			try {
				engine.Initialize();
				firstContinued = engine.Update(1.0F / 60.0F);
				firstDeclarations = fixture.declarations;
				firstFailure = engine.GetFrameFailure();
				summaryFailed = !engine.GetSchedulerSummary().valid;
				secondContinued = engine.Update(1.0F / 60.0F);
				engine.Shutdown();
				engine.Shutdown();
				const auto& finalFailure = engine.GetFrameFailure();
				reportStable = firstFailure && finalFailure && firstFailure->frame == finalFailure->frame
					&& firstFailure->workload == finalFailure->workload && firstFailure->phase == finalFailure->phase
					&& firstFailure->reason == finalFailure->reason;
			} catch (...) {
				engine.Shutdown();
				resources.SetAssetRoot(previousAssetRoot);
				resources.SetEngineRoot(previousEngineRoot);
				resources.BeginRequestSession();
				throw;
			}
			resources.SetAssetRoot(previousAssetRoot);
			resources.SetEngineRoot(previousEngineRoot);
			// Shutdown observations above belong to the failed engine owner. Only
			// after restoring roots may the next shared-process fixture admit work.
			resources.BeginRequestSession();
			context.Expect(summaryFailed, "The actual Engine scheduler must observe the required fault.");
			context.Expect(!firstContinued && !secondContinued, "A failed required frame must terminate the actual Engine loop.");
			context.ExpectEqual(firstDeclarations, fixture.declarations, "A second Update must not capture another frame.");
			context.ExpectEqual(invalidPlan ? 0U : 1U, fixture.executions.load(), "Invalid plans execute no work; a required callback fault runs once.");
			context.ExpectEqual(1U, fixture.shutdowns, "Repeated shutdown must release an initialized owner once.");
			context.Expect(firstFailure && reportStable && !firstFailure->reason.empty(), "The first actionable report must survive subsequent ticks and shutdown unchanged.");
			if(!invalidPlan) context.Expect(firstFailure->workload.find("FrameFailureFixture.Throw") != std::string::npos
				&& firstFailure->phase == "PostFrame" && firstFailure->reason.find("Unexpected fixture frame failure") != std::string::npos,
				"Actual required failure diagnostics must identify the throwing workload, phase and original exception: workload="
				+ firstFailure->workload + ", phase=" + firstFailure->phase + ", reason=" + firstFailure->reason);
			else context.Expect(firstFailure->reason.find("prerequisite") != std::string::npos || firstFailure->reason.find("dependency") != std::string::npos,
				"Invalid required-plan diagnostics must identify the missing dependency.");
			if(independent) {
				context.ExpectEqual(0U, fixture.dependentExecutions.load(), "A dependent callback must not run after its required prerequisite fails.");
				context.ExpectEqual(std::size_t{1}, fixture.independentSettled.Value(), "Already-running independent work must settle before failed Update returns.");
			}
			context.Expect(fixture.poolAliveAtDrain && fixture.acquiredOwners == 0U, "Owner drain runs while the worker dependency is alive and releases counted ownership.");
			if(rollbackFault) context.Expect(fixture.mutationRolledBack, "The protected journal must restore earlier writes before the required fault terminates frame execution.");
		}

		void CheckInitializationOrHandledErrors(TestContext& context, bool initializeFault, bool shutdownFault = false) {
			Correctness::DisposableFixturePath paths;
			auto& resources = ResourceManager::Instance();
			const auto previousAssetRoot = resources.GetAssetRoot();
			const auto previousEngineRoot = resources.GetEngineRoot();
			resources.SetAssetRoot(paths.Root()); resources.SetEngineRoot(paths.Root());
			FailingFrameSystem fixture(false);
			fixture.failInitialization = initializeFault; fixture.handledErrors = !initializeFault; fixture.failShutdown = shutdownFault;
			Engine engine; engine.ConfigureHeadlessCpuFallback(); engine.SetEditorMode(&fixture);
			bool initializationThrew = false, firstContinued = false, secondContinued = false, noFailure = false;
			bool shutdownSettled = false;
			try {
				try { engine.Initialize(); }
				catch(const std::runtime_error& error) {
					if(!initializeFault || std::string(error.what()).find("Injected fixture initialization failure") == std::string::npos) throw;
					initializationThrew = true;
				}
				firstContinued = engine.Update(1.0F / 60.0F);
				secondContinued = engine.Update(1.0F / 60.0F);
				noFailure = !engine.GetFrameFailure();
				engine.Shutdown(); engine.Shutdown();
				const auto& finalFailure = engine.GetFrameFailure();
				shutdownSettled = ThreadPool::instance().getThreadCount() == 0U && !engine.Update(1.0F / 60.0F)
					&& (!shutdownFault || (finalFailure && finalFailure->workload == "Editor" && finalFailure->phase == "Shutdown"
						&& finalFailure->reason.find("Injected fixture shutdown failure") != std::string::npos));
			} catch(...) {
				engine.Shutdown(); resources.SetAssetRoot(previousAssetRoot); resources.SetEngineRoot(previousEngineRoot);
				resources.BeginRequestSession(); throw;
			}
			resources.SetAssetRoot(previousAssetRoot); resources.SetEngineRoot(previousEngineRoot);
			// Keep the asserted engine shutdown closed, then create a separate
			// resource session for consumers registered later in this test process.
			resources.BeginRequestSession();
			context.ExpectEqual(1U, fixture.initializations, "Each real engine lifetime attempts editor initialization once.");
			context.ExpectEqual(1U, fixture.shutdowns, "Even partially acquired editor ownership must clean up exactly once.");
			context.Expect(fixture.drains == 1U && fixture.poolAliveAtDrain && fixture.acquiredOwners == 0U,
				"The acquired editor owner must drain while dependencies live before release.");
			context.Expect(shutdownSettled, "Even an owner shutdown exception must preserve its diagnostic, release later dependencies and start no next frame.");
			if(initializeFault) {
				context.Expect(initializationThrew && !firstContinued && !secondContinued && fixture.declarations == 0U && fixture.executions == 0U,
					"A throwing initialization cleans partial ownership and starts zero subsequent normal frames.");
			} else {
				context.Expect(firstContinued && secondContinued && noFailure && fixture.executions == 2U
					&& fixture.handledMutationRejected && fixture.optionalAssetAbsent,
					"Expected invalid mutation rejection and an absent optional asset must permit the following normal frame.");
			}
		}
	}

	void RegisterFrameFailureLifecycleTests(TestRegistry& registry)
	{
		registry.Add("Engine.FrameFailureLifecycle.WorkerFaultStopsActualEngine", [](TestContext& context) {
			CheckEngineFailure(context, true, true);
		});
		registry.Add("Engine.FrameFailureLifecycle.ProtectedFaultStopsActualEngine", [](TestContext& context) {
			CheckEngineFailure(context, false, true);
		});
		registry.Add("Engine.FrameFailureLifecycle.InvalidRequiredPlanStartsNoJobs", [](TestContext& context) {
			CheckEngineFailure(context, false, false, true);
		});
		registry.Add("Engine.FrameFailureLifecycle.ProtectedMutationRollbackPrecedesTerminalEscape", [](TestContext& context) {
			CheckEngineFailure(context, false, false, false, true);
		});
		registry.Add("Engine.FrameFailureLifecycle.PartialEditorInitializationCleansAcquiredOwner", [](TestContext& context) {
			CheckInitializationOrHandledErrors(context, true);
		});
		registry.Add("Engine.FrameFailureLifecycle.HandledMutationAndOptionalAssetErrorsContinue", [](TestContext& context) {
			CheckInitializationOrHandledErrors(context, false);
		});
		registry.Add("Engine.FrameFailureLifecycle.OwnerShutdownFaultStillSettlesDependencies", [](TestContext& context) {
			CheckInitializationOrHandledErrors(context, false, true);
		});
	}
}
