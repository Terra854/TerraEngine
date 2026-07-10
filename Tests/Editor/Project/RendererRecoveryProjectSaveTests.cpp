#include "TestHarness.h"
#include "ProjectManager.h"
#include "Renderer/RendererRecoveryEditorTestFixtures.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryProjectSaveTests(TestRegistry& registry)
	{
		registry.Add("Editor.RendererRecovery.TerminalDialogDoesNotOfferSave", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoverySnapshot snapshot = MakeEditorRendererRecoverySnapshot();

			context.Expect(!snapshot.canSaveProjectData, "Terminal device loss should not advertise project saving after the fatal dialog takes control.");
			context.Expect(snapshot.affectedResourceName.find("Scene View") != std::string::npos, "The safe-save diagnostic should preserve affected renderer context without requiring GPU handles.");
		});

		registry.Add("Editor.RendererRecovery.SafeSaveDoesNotCookAssets", [](TestContext& context) {
			const std::filesystem::path projectRoot = std::filesystem::temp_directory_path() / "TerraEngineRendererRecoverySafeSave";
			std::filesystem::remove_all(projectRoot);
			ProjectManager projectManager;
			std::string error;

			context.Expect(projectManager.CreateProject(projectRoot, "Renderer Recovery Safe Save", ProjectTemplate::Blank, &error), error.empty() ? "Temporary project should be created." : error);
			error.clear();
			context.Expect(projectManager.SaveProjectDataForRendererRecovery(&error), error.empty() ? "Recovery safe-save should write CPU-owned project data." : error);
			const RendererRecoveryProjectActionState& state = projectManager.GetRendererRecoveryActionState();

			context.Expect(state.canSaveProjectData, "Recovery action state should remain save-capable for the active project.");
			context.Expect(state.lastSafeSaveMessage.find("renderer state") != std::string::npos, "Safe-save message should state that renderer state was not used.");
			context.Expect(!std::filesystem::exists(projectRoot / "Build" / "Assets"), "Recovery safe-save must not create cooked asset output.");
			std::filesystem::remove_all(projectRoot);
		});
	}
}
