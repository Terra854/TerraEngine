#include "../TestHarness.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include "ScriptCompilationSubsystem.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "Utilities/UUID.h"
#include "Editor/Export/ExportRuntimeLayout.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <memory>
#include <optional>
#include <thread>
#include <vector>
#include <iostream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		class ScriptWorkspaceOverlapFixture
		{
		public:
			explicit ScriptWorkspaceOverlapFixture(const std::filesystem::path& root)
				: owner(std::this_thread::get_id()), compilation(ScriptCompilationSubsystem::Instance())
			{
				context.projectManager = std::make_shared<ProjectManager>();
				ready = context.projectManager->CreateProject(root, "WorkspaceOverlap", ProjectTemplate::Blank, &error);
				compilation.Initialize(context);
				compilation.SetBuildInputObserverForTests([this](const ScriptCompilationSubsystem::ScriptBuildInputs& inputs) {
					std::lock_guard lock(mutex);
					observedInputs = inputs;
					inputThread = std::this_thread::get_id();
				});
				compilation.SetWorkspaceAccessObserverForTests([this](std::string_view operation) {
					std::lock_guard lock(mutex);
					if (std::this_thread::get_id() != owner)
					{
						foreignAccesses.emplace_back(operation);
						// Record ownership before any mutable service access, then stop
						// this pre-fix worker without invoking a real compiler or a race.
						throw TestFailure("Build worker accessed owner-only workspace: " + std::string(operation));
					}
				});
				compilation.SetWorkerEntryHookForTests([this] {
					std::unique_lock lock(mutex);
					// This hook is inside RunBuildJob, after observing its detached packet.
					workerThread = std::this_thread::get_id();
					entered = true;
					signal.notify_all();
					signal.wait(lock, [this] { return released; });
				});
				if (ready) compilation.OnProjectOpened();
			}
			~ScriptWorkspaceOverlapFixture()
			{
				ReleaseAndDrain();
				// No worker can still read callback captures when they are removed.
				compilation.SetWorkerEntryHookForTests({});
				compilation.SetBuildInputObserverForTests({});
				compilation.SetWorkspaceAccessObserverForTests({});
				compilation.Shutdown();
			}
			void ReleaseAndDrain()
			{
				compilation.RequestCancellation();
				{ std::lock_guard lock(mutex); released = true; }
				signal.notify_all();
				compilation.StopAdmissionsAndDrain();
			}
			bool WaitForWorker()
			{
				std::unique_lock lock(mutex);
				return signal.wait_for(lock, std::chrono::seconds(3), [this] { return entered; });
			}
			EditorContext context;
			const std::thread::id owner;
			std::thread::id workerThread, inputThread;
			ScriptCompilationSubsystem& compilation;
			std::mutex mutex;
			std::condition_variable signal;
			bool ready = false, entered = false, released = false;
			std::string error;
			std::vector<std::string> foreignAccesses;
			std::optional<ScriptCompilationSubsystem::ScriptBuildInputs> observedInputs;
		};

		void ExerciseWorkspaceOverlap(TestContext& context, const std::filesystem::path& root)
		{
			ScriptWorkspaceOverlapFixture fixture(root);
			context.Expect(fixture.ready, "A disposable native project must be available: " + fixture.error);
			const auto expectedRoot = fixture.compilation.GetWorkspaceService().GetProjectRoot();
			const auto expectedProject = fixture.compilation.GetWorkspaceService().GetProjectPath();
			const auto expectedAssembly = fixture.compilation.GetWorkspaceService().GetAssemblyPath();
			const auto expectedProfile = fixture.compilation.GetStatus().activeProfile;
			ExportRuntimeLayout runtimeLayout;
			std::string runtimeError;
			context.Expect(DiscoverExportRuntimeLayout(TerraEngine::PlatformUtils::GetExecutablePath(), runtimeLayout, runtimeError),
				"The build-input check needs the installed runtime layout: " + runtimeError);
			// Inject a filesystem exception at the actual owner preparation seam.
			// It must settle the coordinator before any build worker is admitted.
			auto failOnce = std::make_shared<std::atomic_bool>(true);
			fixture.compilation.SetWorkspaceAccessObserverForTests([&fixture, failOnce](std::string_view operation) {
				if(operation == "EnsureWorkspace" && failOnce->exchange(false))
					throw std::filesystem::filesystem_error("Injected workspace directory failure", std::make_error_code(std::errc::permission_denied));
				std::lock_guard lock(fixture.mutex);
				if(std::this_thread::get_id() != fixture.owner) {
					fixture.foreignAccesses.emplace_back(operation);
					throw TestFailure("Build worker accessed owner-only workspace: " + std::string(operation));
				}
			});
			fixture.compilation.RequestBuild(ScriptBuildReason::FileChange);
			const auto failedPreparation = fixture.compilation.GetStatus();
			context.Expect(failedPreparation.state == ScriptCoordinatorState::Failed && !failedPreparation.queuedBuild
				&& failedPreparation.progress == 1.0 && !fixture.compilation.IsBusy()
				&& failedPreparation.lastSummary.find("Injected workspace directory failure") != std::string::npos,
				"Preparation exceptions must publish terminal failure and permit the next request to recover.");
			fixture.compilation.RequestBuild(ScriptBuildReason::FileChange);
			context.Expect(fixture.WaitForWorker(), "The actual build worker must enter its deterministic checkpoint.");
			context.Expect(fixture.compilation.IsBusy(), "The workspace commands must actually overlap an admitted build.");
			ScriptWorkspaceStatus status;
			std::string syncError, regenerationError;
			const bool synchronized = fixture.compilation.SynchronizeWorkspace(status, &syncError);
			const bool regenerated = fixture.compilation.RegenerateWorkspace(&regenerationError);
			const auto capturedRoot = fixture.compilation.GetWorkspaceService().GetProjectRoot();
			const auto capturedStatus = fixture.compilation.GetWorkspaceService().GetStatus();
			static_cast<void>(fixture.compilation.GetStatus());
			fixture.ReleaseAndDrain();
			std::cout << "Script build overlap identities: owner=" << fixture.owner
				<< " worker=" << fixture.workerThread << " packet-reader=" << fixture.inputThread << '\n';
			context.Expect(fixture.workerThread != std::thread::id{} && fixture.workerThread != fixture.owner
				&& fixture.inputThread == fixture.workerThread && std::this_thread::get_id() == fixture.owner,
				"Actual build preparation must consume captured inputs on its observed foreign worker while workspace commands remain owner-only.");
			context.Expect(synchronized && regenerated, "Owner workspace commands need valid installed prerequisites: " + syncError + regenerationError);
			context.Expect(capturedRoot == root && !capturedStatus.projectPath.empty(), "Overlapping status reads must identify the active canonical workspace.");
			context.Expect(fixture.foreignAccesses.empty(),
				"The build worker must use captured values and never read/write the mutable workspace service.");
			context.Expect(fixture.observedInputs.has_value(), "The actual worker must observe one detached typed input packet.");
			const auto& inputs = *fixture.observedInputs;
			context.Expect(inputs.projectRoot == expectedRoot && inputs.projectPath == expectedProject
				&& inputs.assemblyPath == expectedAssembly && inputs.profile == expectedProfile
				&& inputs.dotnetExecutable == runtimeLayout.dotnetExecutable,
				"Regeneration and status access must preserve the worker's captured canonical paths, profile, and bundled SDK.");
		}
	}

	void RegisterScriptWorkspaceConcurrencyTests(TestRegistry& registry)
	{
		registry.Add("Editor.ScriptWorkspaceConcurrency.OwnerCommandsOverlapWorkerWithoutSharedServiceAccess", [](TestContext& context) {
			if (const char* root = std::getenv("TERRAENGINE_WORKSPACE_OVERLAP_CHILD_ROOT"))
			{
				ExerciseWorkspaceOverlap(context, root);
				return;
			}
			// Child-only preferences keep project creation away from the user's store;
			// the owned process also bounds unexpected runtime/drain prerequisite bugs.
			const auto root = std::filesystem::temp_directory_path()
				/ ("TerraEngine-WorkspaceOverlap-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			const auto executable = TerraEngine::PlatformUtils::GetExecutablePath();
			TerraEngine::Utilities::OwnedProcessRequest request;
			request.workingDirectory = executable.parent_path();
			request.commandLine = "\"" + executable.string() + "\"";
			request.timeout = std::chrono::seconds(20);
			request.environment = {{"TERRAENGINE_WORKSPACE_OVERLAP_CHILD_ROOT", root.string()},
				{"TERRAENGINE_EDITOR_PREFS_SESSION_PATH", (root / "preferences.json").string()},
				{"TERRAENGINE_TEST_FILTER", "Editor.ScriptWorkspaceConcurrency.OwnerCommandsOverlapWorkerWithoutSharedServiceAccess"},
				{"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
			const std::atomic_bool canceled{false};
			const auto child = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
			std::error_code ignored;
			const auto absolute = std::filesystem::weakly_canonical(root, ignored);
			const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored);
			if (!ignored && absolute.parent_path() == temporary
				&& absolute.filename().string().starts_with("TerraEngine-WorkspaceOverlap-"))
				std::filesystem::remove_all(absolute, ignored);
			// Successful owned children are otherwise quiet. Retain the actual
			// observed identities in the parent receipt as well as asserting them.
			const auto identityStart = child.output.find("Script build overlap identities:");
			if(identityStart != std::string::npos)
				std::cout << child.output.substr(identityStart, child.output.find('\n', identityStart) - identityStart) << '\n';
			context.Expect(child.started && !child.timedOut && child.exitCode == 0,
				"Workspace overlap must complete with protected service ownership (exit " + std::to_string(child.exitCode)
					+ ", timeout " + std::to_string(child.timedOut) + "): " + child.output);
			context.Expect(identityStart != std::string::npos, "Successful workspace overlap must retain its actual owner/worker identity observation.");
		});
	}
}
