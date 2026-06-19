#include "TestHarness.h"

#include "Core/Scheduling/SystemScheduler.h"
#include "Core/SystemBase.h"
#include "Scene/SceneMutationQueue.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class FakeSystem final : public SystemsBase {
		public:
			FakeSystem(std::string systemName, std::vector<std::string>* executionLog = nullptr)
				: name(std::move(systemName))
				, log(executionLog)
			{
			}

			void Initialize() override {}

			void Update(float) override
			{
				if (log != nullptr)
				{
					log->push_back(name);
				}
			}

			void Shutdown() override {}

			std::string SystemName() override
			{
				return name;
			}

		private:
			std::string name;
			std::vector<std::string>* log = nullptr;
		};

		TerraEngine::Scheduling::SystemDeclaration MakeDeclaration(std::string name, std::string phase)
		{
			TerraEngine::Scheduling::SystemDeclaration declaration;
			declaration.name = std::move(name);
			declaration.phase = std::move(phase);
			declaration.mainThread = true;
			declaration.profilingLabel = declaration.name;
			declaration.usesDefaultMetadata = false;
			return declaration;
		}

		bool Contains(const std::vector<std::string>& values, const std::string& expected)
		{
			return std::find(values.begin(), values.end(), expected) != values.end();
		}

		bool HasDiagnostic(const TerraEngine::Scheduling::ScheduleSummary& summary, TerraEngine::Scheduling::DiagnosticCategory category)
		{
			return std::any_of(summary.diagnostics.begin(), summary.diagnostics.end(), [category](const TerraEngine::Scheduling::ScheduleDiagnostic& diagnostic) {
				return diagnostic.category == category;
			});
		}

		const TerraEngine::Scheduling::ScheduleDiagnostic* FindDiagnostic(const TerraEngine::Scheduling::ScheduleSummary& summary, TerraEngine::Scheduling::DiagnosticCategory category)
		{
			const auto found = std::find_if(summary.diagnostics.begin(), summary.diagnostics.end(), [category](const TerraEngine::Scheduling::ScheduleDiagnostic& diagnostic) {
				return diagnostic.category == category;
			});
			return found == summary.diagnostics.end() ? nullptr : &(*found);
		}
	}

	void RegisterSystemSchedulerTests(TestRegistry& registry)
	{
		registry.Add("Engine.SystemScheduler.ValidPhaseOrderIsDeterministic", [](TestContext& context) {
			FakeSystem render("Render");
			FakeSystem input("Input");
			FakeSystem scene("Scene");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(render, MakeDeclaration("Render", "Render"));
			scheduler.RegisterSystem(input, MakeDeclaration("Input", "Input"));
			scheduler.RegisterSystem(scene, MakeDeclaration("Scene", "Scene"));

			const TerraEngine::Scheduling::ScheduleSummary first = scheduler.BuildSchedule();
			const TerraEngine::Scheduling::ScheduleSummary second = scheduler.BuildSchedule();

			context.Expect(first.valid, "A valid phase-only schedule should build.");
			context.ExpectEqual(std::string("Input"), first.systems[0], "Input phase should run before Scene and Render.");
			context.ExpectEqual(std::string("Scene"), first.systems[1], "Scene phase should run before Render.");
			context.ExpectEqual(std::string("Render"), first.systems[2], "Render phase should run last in this fixture.");
			context.ExpectEqual(first.formatted, second.formatted, "Repeated validation should produce the same summary text.");
		});

		registry.Add("Engine.SystemScheduler.DefaultMetadataFallsBackToPostFrame", [](TestContext& context) {
			FakeSystem legacy("LegacySystem");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(legacy);

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();

			context.Expect(summary.valid, "Default scheduler metadata should be valid.");
			context.Expect(Contains(summary.systems, "LegacySystem"), "Defaulted systems should still be scheduled.");
			context.Expect(Contains(summary.defaultedSystems, "LegacySystem"), "Summary should identify systems using default metadata.");
			context.Expect(HasDiagnostic(summary, TerraEngine::Scheduling::DiagnosticCategory::DefaultMetadata), "Default metadata should produce an informational diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.UnknownDependencyAndCycleAreRejected", [](TestContext& context) {
			FakeSystem a("A");
			FakeSystem b("B");
			TerraEngine::Scheduling::SystemScheduler unknownDependencyScheduler;
			auto unknown = MakeDeclaration("A", "Scene");
			unknown.after.push_back("Missing");
			unknownDependencyScheduler.RegisterSystem(a, unknown);

			const TerraEngine::Scheduling::ScheduleSummary& unknownSummary = unknownDependencyScheduler.BuildSchedule();
			context.Expect(!unknownSummary.valid, "Unknown dependencies should reject the schedule.");
			context.Expect(HasDiagnostic(unknownSummary, TerraEngine::Scheduling::DiagnosticCategory::Dependency), "Unknown dependency should be reported as a dependency diagnostic.");

			TerraEngine::Scheduling::SystemScheduler cycleScheduler;
			auto declarationA = MakeDeclaration("A", "Scene");
			auto declarationB = MakeDeclaration("B", "Scene");
			declarationA.after.push_back("B");
			declarationB.after.push_back("A");
			cycleScheduler.RegisterSystem(a, declarationA);
			cycleScheduler.RegisterSystem(b, declarationB);

			const TerraEngine::Scheduling::ScheduleSummary& cycleSummary = cycleScheduler.BuildSchedule();
			context.Expect(!cycleSummary.valid, "Dependency cycles should reject the schedule.");
			context.Expect(HasDiagnostic(cycleSummary, TerraEngine::Scheduling::DiagnosticCategory::Dependency), "Cycle should be reported as a dependency diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.ResourceReadReadIsCompatibleAndReadWriteConflicts", [](TestContext& context) {
			FakeSystem readerA("ReaderA");
			FakeSystem readerB("ReaderB");
			TerraEngine::Scheduling::SystemScheduler readScheduler;
			auto readA = MakeDeclaration("ReaderA", "Scene");
			auto readB = MakeDeclaration("ReaderB", "Scene");
			readA.reads.push_back("SceneGraph");
			readB.reads.push_back("SceneGraph");
			readScheduler.RegisterSystem(readerA, readA);
			readScheduler.RegisterSystem(readerB, readB);
			context.Expect(readScheduler.BuildSchedule().valid, "Unordered read/read access should be compatible.");

			FakeSystem writer("Writer");
			TerraEngine::Scheduling::SystemScheduler conflictScheduler;
			auto writerDeclaration = MakeDeclaration("Writer", "Scene");
			writerDeclaration.writes.push_back("SceneGraph");
			conflictScheduler.RegisterSystem(readerA, readA);
			conflictScheduler.RegisterSystem(writer, writerDeclaration);
			const TerraEngine::Scheduling::ScheduleSummary& conflictSummary = conflictScheduler.BuildSchedule();
			context.Expect(!conflictSummary.valid, "Unordered read/write access should be rejected.");
			context.Expect(HasDiagnostic(conflictSummary, TerraEngine::Scheduling::DiagnosticCategory::Resource), "Read/write conflict should produce a resource diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.ResourceWriteWriteConflictsButOrderedAccessIsAccepted", [](TestContext& context) {
			FakeSystem writerA("WriterA");
			FakeSystem writerB("WriterB");
			TerraEngine::Scheduling::SystemScheduler conflictScheduler;
			auto writeA = MakeDeclaration("WriterA", "Scene");
			auto writeB = MakeDeclaration("WriterB", "Scene");
			writeA.writes.push_back("ObjectManager");
			writeB.writes.push_back("ObjectManager");
			conflictScheduler.RegisterSystem(writerA, writeA);
			conflictScheduler.RegisterSystem(writerB, writeB);
			context.Expect(!conflictScheduler.BuildSchedule().valid, "Unordered write/write access should be rejected.");

			TerraEngine::Scheduling::SystemScheduler orderedScheduler;
			writeB.after.push_back("WriterA");
			orderedScheduler.RegisterSystem(writerA, writeA);
			orderedScheduler.RegisterSystem(writerB, writeB);
			context.Expect(orderedScheduler.BuildSchedule().valid, "Explicit ordering should make write/write access valid.");
		});

		registry.Add("Engine.SystemScheduler.UnknownResourceIsRejected", [](TestContext& context) {
			FakeSystem system("UsesMissingResource");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			auto declaration = MakeDeclaration("UsesMissingResource", "Scene");
			declaration.reads.push_back("MissingResource");
			scheduler.RegisterSystem(system, declaration);

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();
			context.Expect(!summary.valid, "Unknown resources should reject the schedule.");
			const TerraEngine::Scheduling::ScheduleDiagnostic* diagnostic = FindDiagnostic(summary, TerraEngine::Scheduling::DiagnosticCategory::Resource);
			context.Expect(diagnostic != nullptr, "Unknown resource should produce a structured resource diagnostic.");
			context.ExpectEqual(std::string("MissingResource"), diagnostic->resourceName, "Resource diagnostic should include the missing resource name.");
		});

		registry.Add("Engine.SystemScheduler.MainThreadPlacementIsValidated", [](TestContext& context) {
			FakeSystem allowed("Allowed");
			FakeSystem blocked("Blocked");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			auto phases = TerraEngine::Scheduling::DefaultSchedulerPhases();
			phases.push_back({ "WorkerOnly", 80, false, true, false, false, "Future worker-only test phase." });
			scheduler.SetPhaseDefinitions(phases);
			scheduler.RegisterSystem(allowed, MakeDeclaration("Allowed", "Scene"));
			scheduler.RegisterSystem(blocked, MakeDeclaration("Blocked", "WorkerOnly"));

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();
			context.Expect(!summary.valid, "A main-thread system in a worker-only phase should be rejected.");
			context.Expect(HasDiagnostic(summary, TerraEngine::Scheduling::DiagnosticCategory::MainThread), "Main-thread placement should produce a main-thread diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.MutationFlushRequirementIsValidated", [](TestContext& context) {
			FakeSystem render("RenderNeedsFlush");
			auto declaration = MakeDeclaration("RenderNeedsFlush", "Render");
			declaration.requiresFlushedSceneMutations = true;

			TerraEngine::Scheduling::SystemScheduler validScheduler;
			validScheduler.SetMutationFlushPoints({ { "After Scene", TerraEngine::Scheduling::MutationFlushPosition::AfterPhase, "Scene", {}, true, false } });
			validScheduler.RegisterSystem(render, declaration);
			context.Expect(validScheduler.BuildSchedule().valid, "A render system after a Scene flush should satisfy the flush requirement.");

			TerraEngine::Scheduling::SystemScheduler invalidScheduler;
			invalidScheduler.SetMutationFlushPoints({});
			invalidScheduler.RegisterSystem(render, declaration);
			const TerraEngine::Scheduling::ScheduleSummary& invalidSummary = invalidScheduler.BuildSchedule();
			context.Expect(!invalidSummary.valid, "Missing mutation flush should reject systems that require flushed scene state.");
			context.Expect(HasDiagnostic(invalidSummary, TerraEngine::Scheduling::DiagnosticCategory::Mutation), "Missing flush should produce a mutation diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.FrameEndPendingMutationIsDiagnosed", [](TestContext& context) {
			std::vector<std::string> log;
			FakeSystem system("Scene", &log);
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetMutationFlushPoints({ { "Frame End", TerraEngine::Scheduling::MutationFlushPosition::FrameEnd, "", {}, true, true } });
			scheduler.RegisterSystem(system, MakeDeclaration("Scene", "Scene"));
			context.Expect(scheduler.BuildSchedule().valid, "Fixture schedule should start valid.");

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.ExecuteFrame(0.016f, [](const TerraEngine::Scheduling::MutationFlushPoint&) {
				TerraEngine::Scheduling::MutationFlushRuntimeState state;
				state.pendingAfterFlush = 1;
				return state;
			});

			context.Expect(!summary.valid, "Pending frame-end mutations should make the frame summary invalid.");
			context.Expect(HasDiagnostic(summary, TerraEngine::Scheduling::DiagnosticCategory::Mutation), "Pending frame-end mutations should produce a mutation diagnostic.");
		});

		registry.Add("Engine.SystemScheduler.SceneMutationFlushesRunAtDeclaredBoundaries", [](TestContext& context) {
			std::vector<std::string> log;
			FakeSystem scene("Scene", &log);
			FakeSystem render("Render", &log);
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.SetMutationFlushPoints({
				{ "After Scene", TerraEngine::Scheduling::MutationFlushPosition::AfterPhase, "Scene", {}, true, false },
				{ "Frame End", TerraEngine::Scheduling::MutationFlushPosition::FrameEnd, "", {}, true, true }
			});
			auto renderDeclaration = MakeDeclaration("Render", "Render");
			renderDeclaration.requiresFlushedSceneMutations = true;
			scheduler.RegisterSystem(scene, MakeDeclaration("Scene", "Scene"));
			scheduler.RegisterSystem(render, renderDeclaration);

			scheduler.ExecuteFrame(0.016f, [&](const TerraEngine::Scheduling::MutationFlushPoint& point) {
				log.push_back("flush:" + point.name);
				return TerraEngine::Scheduling::MutationFlushRuntimeState{};
			});

			context.ExpectEqual(std::string("Scene"), log[0], "Scene system should run first.");
			context.ExpectEqual(std::string("flush:After Scene"), log[1], "After Scene flush should run before Render.");
			context.ExpectEqual(std::string("Render"), log[2], "Render should run after scene mutations flush.");
			context.ExpectEqual(std::string("flush:Frame End"), log[3], "Frame End flush should run last.");
		});

		registry.Add("Engine.SystemScheduler.SceneMutationQueuePendingSnapshotIsReadOnly", [](TestContext& context) {
			TerraEngine::Scene::SceneMutationQueue& queue = TerraEngine::Scene::SceneMutationQueue::Instance();
			queue.Clear();
			TerraEngine::Scene::SceneMutationRequest request;
			request.requestId = "scheduler-test";
			request.displayName = "Object";
			queue.Enqueue(request);

			std::vector<TerraEngine::Scene::SceneMutationRequest> pending = queue.PendingRequests();
			pending.clear();

			context.ExpectEqual(static_cast<std::size_t>(1), queue.Size(), "Editing the pending snapshot should not edit the real mutation queue.");
			queue.Clear();
		});

		registry.Add("Engine.SystemScheduler.ValidSummaryContainsScheduleFields", [](TestContext& context) {
			FakeSystem input("Input");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(input, MakeDeclaration("Input", "Input"));

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();

			context.Expect(summary.valid, "Summary fixture should be valid.");
			context.Expect(Contains(summary.phases, "Input"), "Summary should include ordered phase names.");
			context.Expect(Contains(summary.systems, "Input"), "Summary should include ordered systems.");
			context.Expect(!summary.flushPoints.empty(), "Summary should include configured flush points.");
			context.Expect(Contains(summary.mainThreadSystems, "Input"), "Summary should include main-thread systems.");
			context.Expect(summary.formatted.find("System scheduler summary: valid") != std::string::npos, "Formatted summary should include validity.");
		});

		registry.Add("Engine.SystemScheduler.InvalidDiagnosticFieldsAreStructured", [](TestContext& context) {
			FakeSystem system("BadPhase");
			TerraEngine::Scheduling::SystemScheduler scheduler;
			scheduler.RegisterSystem(system, MakeDeclaration("BadPhase", "MissingPhase"));

			const TerraEngine::Scheduling::ScheduleSummary& summary = scheduler.BuildSchedule();
			const TerraEngine::Scheduling::ScheduleDiagnostic* diagnostic = FindDiagnostic(summary, TerraEngine::Scheduling::DiagnosticCategory::Phase);

			context.Expect(!summary.valid, "Unknown phase fixture should be invalid.");
			context.Expect(diagnostic != nullptr, "Unknown phase should create a phase diagnostic.");
			context.ExpectEqual(std::string("BadPhase"), diagnostic->systemName, "Diagnostic should include the system name.");
			context.ExpectEqual(std::string("MissingPhase"), diagnostic->phaseName, "Diagnostic should include the phase name.");
			context.Expect(!diagnostic->message.empty(), "Diagnostic should include a message.");
			context.Expect(!diagnostic->recommendedAction.empty(), "Diagnostic should include a recommended action.");
		});

		registry.Add("Engine.SystemScheduler.InvalidSummaryOrderingIsDeterministic", [](TestContext& context) {
			FakeSystem a("A");
			FakeSystem b("B");
			auto declarationA = MakeDeclaration("A", "Scene");
			auto declarationB = MakeDeclaration("B", "Scene");
			declarationA.reads.push_back("MissingA");
			declarationB.writes.push_back("MissingB");

			TerraEngine::Scheduling::SystemScheduler first;
			first.RegisterSystem(a, declarationA);
			first.RegisterSystem(b, declarationB);
			TerraEngine::Scheduling::SystemScheduler second;
			second.RegisterSystem(a, declarationA);
			second.RegisterSystem(b, declarationB);

			context.ExpectEqual(first.BuildSchedule().formatted, second.BuildSchedule().formatted, "Identical invalid inputs should produce identical diagnostics and summaries.");
		});
	}
}
