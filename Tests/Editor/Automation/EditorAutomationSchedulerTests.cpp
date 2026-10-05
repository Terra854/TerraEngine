#include "../TestHarness.h"

#include "EditorMcpBridge.h"
#include "Engine/Core/Scheduling/SystemScheduler.h"
#include "Engine/Core/SystemBase.h"
#include "Editor/TerraEngine-Editor/Core/EditorSchedulerDeclaration.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	namespace
	{
		// Only declaration validation runs in this fixture. Native editor lifecycle
		// work is intentionally absent; the metadata comes from the actual helper
		// shared by EditorSystem rather than a copied list of expected resources.
		class EditorDeclarationFixture final : public SystemsBase
		{
		public:
			void Initialize() override {}
			void Update(float) override {}
			void Shutdown() override {}
		};

		std::string ReadEditorSystemSource()
		{
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Core/EditorSystem.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}

		std::string ReadEditorMcpBridgeSource()
		{
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Automation/EditorMcpBridge.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}

		std::string ReadEditorAutomationServiceSource()
		{
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			return contents.str();
		}
	}

	void RegisterEditorAutomationSchedulerTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Scheduler.ActualEditorDeclarationValidatesAllRegisteredResources", [](TestContext& context) {
			using namespace TerraEngine::Scheduling;
			EditorDeclarationFixture system;
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			const auto resources = DefaultSchedulerResources();
			SystemScheduler scheduler;
			scheduler.SetResourceDefinitions(resources);
			scheduler.RegisterSystem(system, declaration);
			const auto& summary = scheduler.BuildSchedule();
			context.Expect(summary.valid, "The actual editor declaration must validate against the real default scheduler registry: " + summary.formatted);
			context.Expect(!declaration.reads.empty() && !declaration.writes.empty(), "The actual editor contract must declare both read and write ownership.");

			// Remove each declared identity independently. This proves the real
			// scheduler checks every read/write name, including future additions,
			// without teaching the fixture a second resource-name contract.
			std::set<std::string> declaredResources(declaration.reads.begin(), declaration.reads.end());
			declaredResources.insert(declaration.writes.begin(), declaration.writes.end());
			for (const auto& resourceName : declaredResources) {
				auto missingResources = resources;
				std::erase_if(missingResources, [&](const auto& resource) { return resource.name == resourceName; });
				SystemScheduler missingScheduler;
				missingScheduler.SetResourceDefinitions(std::move(missingResources));
				missingScheduler.RegisterSystem(system, declaration);
				const auto& rejected = missingScheduler.BuildSchedule();
				const bool namesMissingResource = std::any_of(rejected.diagnostics.begin(), rejected.diagnostics.end(), [&](const auto& diagnostic) {
					return diagnostic.severity == DiagnosticSeverity::Error && diagnostic.category == DiagnosticCategory::Resource
						&& diagnostic.resourceName == resourceName;
				});
				context.Expect(!rejected.valid && namesMissingResource, "Removing declared editor resource must reject startup with its exact identity: " + resourceName);
			}
		});

		registry.Add("Editor.Automation.Scheduler.DeclaresQueueAndSessionResources", [](TestContext& context) {
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const auto hasResource = [&resources](std::string_view expected) {
				return std::any_of(resources.begin(), resources.end(), [expected](const auto& resource) {
					return resource.name == expected;
				});
			};

			context.Expect(hasResource("EditorAutomationQueue"), "Scheduler resources should describe immutable queued automation work.");
			context.Expect(hasResource("EditorAutomationSession"), "Scheduler resources should describe session-retained automation state.");
			context.Expect(hasResource("EditorProfilerWorkerCompletions"), "Scheduler resources should register profiler worker completions before EditorSystem validation.");
			context.Expect(hasResource("EditorProfilerHealthPublication"), "Scheduler resources should register bounded profiler health publication before EditorSystem validation.");

			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			const auto declares = [&](std::string_view name) {
				return std::find(declaration.reads.begin(), declaration.reads.end(), name) != declaration.reads.end()
					|| std::find(declaration.writes.begin(), declaration.writes.end(), name) != declaration.writes.end();
			};
			context.Expect(declares("EditorAutomationQueue"), "EditorSystem should read prepared automation requests.");
			context.Expect(declares("EditorAutomationSession"), "EditorSystem should declare session-retained automation state.");
			context.Expect(declares("AnimationRuntimeState"), "EditorSystem should declare its immutable animation-publication read.");
			context.Expect(declares("SceneMutationQueue"), "Scene-facing automation should publish through the deferred mutation queue.");
		});

		registry.Add("Editor.Automation.Scheduler.PreparesOnWorkersAndCommitsProtected", [](TestContext& context) {
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			context.Expect(declaration.safeSubwork.find("immutable request preparation") != std::string::npos, "Worker-safe automation work should prepare immutable requests.");
			context.Expect(declaration.safeSubwork.find("may run on workers") != std::string::npos, "Scheduler diagnostics should explicitly identify worker-safe preparation.");
			context.Expect(declaration.protectedCommit.find("prepared automation request commit") != std::string::npos, "Scheduler diagnostics should name the protected automation commit.");
			context.Expect(declaration.phaseKind == TerraEngine::Scheduling::WorkerSystemPhaseKind::ProtectedCommit, "EditorSystem should remain classified as a protected commit.");
			context.Expect(declaration.boundaryClassification == TerraEngine::Scheduling::HardBoundaryClassification::Protected, "Automation must not make the complete editor update worker-safe.");
		});

		registry.Add("Editor.Automation.Scheduler.RecordingPumpPrecedesEditorEarlyReturns", [](TestContext& context) {
			const std::string editor = ReadEditorSystemSource();
			const std::string bridge = ReadEditorMcpBridgeSource();
			const std::string service = ReadEditorAutomationServiceSource();
			const std::size_t pump = editor.find("PumpAnimationRecordingsAtProtectedFrame()");
			const std::size_t physicsValidation = editor.find("ProcessPhysicsInvariantValidation(deltaTime)");

			context.Expect(pump != std::string::npos && physicsValidation != std::string::npos
					&& pump < physicsValidation,
				"Scheduler-owned recording capture must run before the installed-validation early return.");
			context.Expect(bridge.find("service.PumpAnimationRecordingsAtProtectedFrame()")
					!= std::string::npos,
				"The bridge must forward protected capture to its one shared automation service, not a request-local store.");
			context.Expect(service.find("EditorAutomationService::~EditorAutomationService()")
					!= std::string::npos
					&& service.find("AnimationAutomationJobs(jobService).CancelAllRecordings()")
						!= std::string::npos,
				"Destroying the shared automation service must retire every process-global recording session keyed by its job store.");
			const std::size_t lateTransportDrain = editor.find("PumpPendingTransportToolCallsAtProtectedFrame()");
			const std::size_t imguiRender = editor.find("ImGui::Render()");
			context.Expect(lateTransportDrain != std::string::npos && imguiRender != std::string::npos
					&& lateTransportDrain < imguiRender,
				"Transport-originated durable jobs should receive a second protected drain before ImGui rendering can stall their acknowledgement.");
			const std::size_t bridgeLateDrain = bridge.find("void EditorMcpBridge::PumpPendingTransportToolCallsAtProtectedFrame()");
			const std::size_t bridgeAnimationPump = bridge.find("void EditorMcpBridge::PumpAnimationRecordingsAtProtectedFrame()");
			context.Expect(bridgeLateDrain != std::string::npos
					&& bridgeAnimationPump != std::string::npos
					&& bridgeLateDrain < bridgeAnimationPump
					&& bridge.find("ProcessPendingToolCalls();", bridgeLateDrain) != std::string::npos,
				"The late scheduler drain must use the bridge's protected transport queue rather than letting a worker mutate editor state directly.");
		});

		registry.Add("Editor.Automation.Scheduler.IdleBridgeDoesNotPublishWork", [](TestContext& context) {
			TerraEngine::Editor::Automation::EditorMcpBridge bridge;
			bridge.Initialize(nullptr);
			const std::size_t initialActivityCount = bridge.GetRecentActivity().size();

			// Repeated idle updates model normal editor frames. With no queued call,
			// Update must not manufacture requests, approvals, or activity records.
			for (int frame = 0; frame < 8; ++frame)
			{
				bridge.Update(1.0f / 60.0f);
			}

			const nlohmann::json status = bridge.BuildStatusJson();
			context.ExpectEqual(static_cast<std::size_t>(0), bridge.GetQueuedRequests().size(), "Idle frames should leave the automation queue empty.");
			context.ExpectEqual(static_cast<std::size_t>(0), status.value("pendingRequestCount", static_cast<std::size_t>(1)), "Idle frames should publish no pending request work.");
			context.ExpectEqual(static_cast<std::size_t>(0), status.value("pendingApprovalCount", static_cast<std::size_t>(1)), "Idle frames should publish no approval work.");
			context.ExpectEqual(initialActivityCount, bridge.GetRecentActivity().size(), "Idle updates should not emit synthetic automation activity.");
			bridge.Shutdown();
		});

		registry.Add("Editor.Automation.Scheduler.SceneWritesBecomeVisibleAtFrameEnd", [](TestContext& context) {
			const auto declaration = TerraEngine::Editor::MakeEditorSchedulerDeclaration();
			const auto flushPoints = TerraEngine::Scheduling::DefaultMutationFlushPoints();
			const bool hasFrameEndFlush = std::any_of(flushPoints.begin(), flushPoints.end(), [](const auto& flush) {
				return flush.position == TerraEngine::Scheduling::MutationFlushPosition::FrameEnd;
			});

			context.Expect(declaration.phase == "PostFrame", "Protected editor automation should commit during the editor PostFrame phase.");
			context.Expect(std::find(declaration.writes.begin(), declaration.writes.end(), "SceneMutationQueue") != declaration.writes.end(), "Automation should defer scene writes instead of mutating live storage.");
			context.Expect(hasFrameEndFlush, "A frame-end flush should publish protected automation mutations after EditorSystem runs.");
		});
	}
}
