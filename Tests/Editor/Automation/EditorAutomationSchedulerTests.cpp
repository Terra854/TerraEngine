#include "../TestHarness.h"

#include "EditorMcpBridge.h"
#include "Engine/Core/Scheduling/SystemScheduler.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace TerraEngine::EditorTests
{
	namespace
	{
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
		registry.Add("Editor.Automation.Scheduler.DeclaresQueueAndSessionResources", [](TestContext& context) {
			const auto resources = TerraEngine::Scheduling::DefaultSchedulerResources();
			const auto hasResource = [&resources](std::string_view expected) {
				return std::any_of(resources.begin(), resources.end(), [expected](const auto& resource) {
					return resource.name == expected;
				});
			};

			context.Expect(hasResource("EditorAutomationQueue"), "Scheduler resources should describe immutable queued automation work.");
			context.Expect(hasResource("EditorAutomationSession"), "Scheduler resources should describe session-retained automation state.");

			const std::string source = ReadEditorSystemSource();
			context.Expect(!source.empty(), "Scheduler contract tests should read EditorSystem.cpp from the configured source root.");
			context.Expect(source.find("\"EditorAutomationQueue\"") != std::string::npos, "EditorSystem should read prepared automation requests.");
			context.Expect(source.find("\"EditorAutomationSession\"") != std::string::npos, "EditorSystem should declare session-retained automation state.");
			context.Expect(source.find("\"AnimationRuntimeState\"") != std::string::npos, "EditorSystem should declare its immutable animation-publication read.");
			context.Expect(source.find("\"SceneMutationQueue\"") != std::string::npos, "Scene-facing automation should publish through the deferred mutation queue.");
		});

		registry.Add("Editor.Automation.Scheduler.PreparesOnWorkersAndCommitsProtected", [](TestContext& context) {
			const std::string source = ReadEditorSystemSource();

			context.Expect(source.find("immutable request preparation") != std::string::npos, "Worker-safe automation work should prepare immutable requests.");
			context.Expect(source.find("may run on workers") != std::string::npos, "Scheduler diagnostics should explicitly identify worker-safe preparation.");
			// The commit phrase sits inside a longer sentence after the recording
			// boundary, so its grammatical capitalization is intentionally lower-case.
			context.Expect(source.find("prepared automation request commit") != std::string::npos, "Scheduler diagnostics should name the protected automation commit.");
			context.Expect(source.find("WorkerSystemPhaseKind::ProtectedCommit") != std::string::npos, "EditorSystem should remain classified as a protected commit.");
			context.Expect(source.find("HardBoundaryClassification::Protected") != std::string::npos, "Automation must not make the complete editor update worker-safe.");
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
			const std::string source = ReadEditorSystemSource();
			const auto flushPoints = TerraEngine::Scheduling::DefaultMutationFlushPoints();
			const bool hasFrameEndFlush = std::any_of(flushPoints.begin(), flushPoints.end(), [](const auto& flush) {
				return flush.position == TerraEngine::Scheduling::MutationFlushPosition::FrameEnd;
			});

			context.Expect(source.find("declaration.phase = \"PostFrame\"") != std::string::npos, "Protected editor automation should commit during the editor PostFrame phase.");
			context.Expect(source.find("\"SceneMutationQueue\"") != std::string::npos, "Automation should defer scene writes instead of mutating live storage.");
			context.Expect(hasFrameEndFlush, "A frame-end flush should publish protected automation mutations after EditorSystem runs.");
		});
	}
}
