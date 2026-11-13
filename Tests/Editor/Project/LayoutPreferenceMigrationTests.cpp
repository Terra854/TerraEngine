#include "../TestHarness.h"
#include "Editor/TerraEngine-Editor/Project/EditorPreferences.h"
#include "Editor/TerraEngine-Editor/Core/EditorSystem.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "Utilities/UUID.h"
#include "Core/Logger/Logger.h"

#include <fstream>
#include <filesystem>
#include <chrono>
#include <atomic>
#include <cstdlib>

namespace TerraEngine::EditorTests
{
	// The single friend supplies CPU-only lifecycle/layout inputs to the real
	// editor methods; it never fabricates successful native GPU initialization.
	class LayoutPreferenceMigrationFixture
	{
	public:
		static void ExerciseDeferredStartup(TestContext& context)
		{
			EditorSystem editor;
			// Setup may be complete before Vulkan/ImGui startup. Reproduce exactly
			// that state without constructing a surface or a fake native backend.
			editor.initialized = true;
			editor.awaitingRendererInitialization = true;
			RendererEfficiencyDeviceRecreationRequest request;
			request.operationId = "deferred-startup"; request.fixtureStepId = "pending";
			request.fixtureId = "private-lifecycle"; request.fixtureHash = "fixture-only";
			request.projectRoot = std::filesystem::temp_directory_path();
			request.projectGeneration = request.expectedRendererSessionGeneration = request.expectedBackendResourceGeneration = 1;
#if defined(TERRAENGINE_RENDERER_EFFICIENCY_VALIDATION)
			std::string reason;
			context.Expect(editor.QueueRendererEfficiencyDeviceRecreation(request, reason), "The actual validation ingress must accept complete pending intent: " + reason);
#else
			// Non-validation builds cannot admit public recreation requests. Seed
			// only the same copied intent to exercise lifecycle retention there.
			editor.pendingRendererEfficiencyDeviceRecreation = request;
			editor.rendererEfficiencyDeviceRecreationReceipt.request = request;
			editor.rendererEfficiencyDeviceRecreationReceipt.status = RendererEfficiencyDeviceRecreationStatus::Pending;
#endif
			editor.Initialize(); // No native context: this attempt cannot create ImGui.
			editor.Update(1.0F / 60.0F); // Must return before any UI/project work.
			const auto receipt = editor.GetRendererEfficiencyDeviceRecreationReceipt();
			context.Expect(editor.awaitingRendererInitialization && !editor.ownsImGuiContext
				&& editor.pendingRendererEfficiencyDeviceRecreation.has_value()
				&& receipt.status == RendererEfficiencyDeviceRecreationStatus::Pending && receipt.request.operationId == request.operationId,
				"A deferred retry retains accepted intent and cannot enter UI work before the renderer is ready.");
		}
		static void Exercise(TestContext& context, const std::filesystem::path& root, const nlohmann::json& malformed)
		{
			EditorSystem editor;
			editor.context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.Expect(editor.context.projectManager->CreateProject(root, "MalformedLayout", ProjectTemplate::Blank, &error),
				"The real layout loader needs a disposable project: " + error);
			editor.mainEditor = std::make_shared<MainEditorSubsystem>();
			observedValidWindow = false;
			editor.managedSetWindowVisibility = [](const char* title, int visible) {
				if (std::string_view(title) == "valid-window" && visible != 0) observedValidWindow = true;
			};
			// The test concerns JSON visibility entries. Removing this generated
			// fixture ini avoids requiring an unrelated ImGui docking context.
			std::filesystem::remove(root / "UserSettings" / "EditorLayout.ini");
			const nlohmann::json layout = {{"windows", nlohmann::json::array({malformed,
				{{"title", "valid-window"}, {"visible", true}, {"managed", true}}})}};
			std::ofstream(root / "UserSettings" / "EditorLayout.terra") << layout;
			bool diagnosed = false;
			const auto sink = Logger::Instance().AddSink([&diagnosed](const LogEntry& entry) {
				if (entry.level >= LogLevel::Warning && entry.file.find("EditorSystem") != std::string::npos) diagnosed = true;
			});
			// Both rejecting the document and skipping a bad entry are permitted.
			// Either must diagnose the invalid input and leave the next load usable.
			editor.LoadWorkspaceLayout();
			Logger::Instance().RemoveSink(sink);
			context.Expect(diagnosed, "Malformed layout input must emit a normal loader diagnostic.");
			std::ofstream(root / "UserSettings" / "EditorLayout.terra") << nlohmann::json{{"windows", nlohmann::json::array({
				{{"title", "valid-window"}, {"visible", true}, {"managed", true}}})}};
			observedValidWindow = false;
			editor.LoadWorkspaceLayout();
			context.Expect(observedValidWindow, "Malformed layout input must leave a subsequent valid load executable.");
		}
	private:
		inline static bool observedValidWindow = false;
	};

	void RegisterLayoutPreferenceMigrationTests(TestRegistry& registry)
	{
		registry.Add("Editor.LayoutPreferenceMigration.DeferredRendererStartupPreservesPendingRecreation", LayoutPreferenceMigrationFixture::ExerciseDeferredStartup);
		registry.Add("Editor.LayoutPreferenceMigration.MalformedEntriesDoNotAbortOrPreventValidRestore", [](TestContext& context) {
			if (const char* root = std::getenv("TERRAENGINE_LAYOUT_MIGRATION_CHILD_ROOT"))
			{
				const char* entry = std::getenv("TERRAENGINE_LAYOUT_MIGRATION_ENTRY");
				context.Expect(entry != nullptr, "The watched child must receive its malformed input.");
				LayoutPreferenceMigrationFixture::Exercise(context, root, nlohmann::json::parse(entry));
				return;
			}
			// Missing const keys can assert in JSON before exceptions reach the
			// loader's catch block. Each watched child contains that native failure.
			std::string failures;
			for (const auto& entry : {nlohmann::json{{"visible", true}}, nlohmann::json{{"title", "missing-visible"}},
				nlohmann::json{{"title", 42}, {"visible", true}}, nlohmann::json{{"title", "wrong-visible"}, {"visible", "yes"}}})
			{
				const auto root = std::filesystem::temp_directory_path()
					/ ("TerraEngine-LayoutMigration-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
				const auto executable = TerraEngine::PlatformUtils::GetExecutablePath();
				TerraEngine::Utilities::OwnedProcessRequest request;
				request.workingDirectory = executable.parent_path();
				request.commandLine = "\"" + executable.string() + "\"";
				request.timeout = std::chrono::seconds(10);
				request.environment = {{"TERRAENGINE_LAYOUT_MIGRATION_CHILD_ROOT", root.string()},
					{"TERRAENGINE_LAYOUT_MIGRATION_ENTRY", entry.dump()},
					{"TERRAENGINE_EDITOR_PREFS_SESSION_PATH", (root / "preferences.json").string()},
					{"TERRAENGINE_TEST_FILTER", "Editor.LayoutPreferenceMigration.MalformedEntriesDoNotAbortOrPreventValidRestore"},
					{"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
				const std::atomic_bool canceled{false};
				const auto child = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
				std::error_code ignored;
				const auto absolute = std::filesystem::weakly_canonical(root, ignored);
				const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored);
				if (!ignored && absolute.parent_path() == temporary
					&& absolute.filename().string().starts_with("TerraEngine-LayoutMigration-"))
					std::filesystem::remove_all(absolute, ignored);
				if (!child.started || child.timedOut || child.exitCode != 0)
					failures += "Input " + entry.dump() + " (exit " + std::to_string(child.exitCode)
						+ ", timeout " + std::to_string(child.timedOut) + "): " + child.output + "\n";
			}
			context.Expect(failures.empty(), "Every malformed layout child must diagnose safely and permit its valid control: " + failures);
		});

		registry.Add("Editor.LayoutPreferenceMigration.UnrelatedSavePreservesExplicitUninitializedDiagnostics", [](TestContext& context) {
			// Exercise the production serializers across an actual save/reload. A
			// recent-project update must not opt into editor diagnostic overrides.
			EditorPreferences preferences;
			preferences.rendererDiagnosticsPreferencesInitialized = false;
			preferences.recentProjects.push_back({"Recent project", "disposable-project", 123});
			const auto path = std::filesystem::temp_directory_path()
				/ ("TerraEngine-PreferenceMigration-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
			nlohmann::json saved = preferences;
			{ std::ofstream output(path); output << saved; }
			nlohmann::json loaded;
			{ std::ifstream input(path); input >> loaded; }
			std::error_code ignored;
			std::filesystem::remove(path, ignored);
			const auto restored = loaded.get<EditorPreferences>();
			context.Expect(!restored.rendererDiagnosticsPreferencesInitialized,
				"An explicit false initialization marker must survive unrelated preference saves.");
			context.ExpectEqual(std::size_t{1}, restored.recentProjects.size(), "The unrelated recent-project save must actually persist.");
		});

		registry.Add("Editor.LayoutPreferenceMigration.LegacyExplicitDiagnosticFieldsRemainInitialized", [](TestContext& context) {
			// Legacy documents lack the marker, so authored diagnostic fields still
			// establish intent. Explicit false and absent-marker cases differ.
			const nlohmann::json legacy = {{"vulkanValidationLayers", true}, {"renderDebugDiagnostics", false}};
			const auto preferences = legacy.get<EditorPreferences>();
			context.Expect(preferences.rendererDiagnosticsPreferencesInitialized && preferences.vulkanValidationLayers,
				"Legacy authored diagnostic preferences must retain their migration behavior.");
		});
	}
}
