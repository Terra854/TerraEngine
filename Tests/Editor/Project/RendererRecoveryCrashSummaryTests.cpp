#include "TestHarness.h"
#include "Core/FatalErrorReport.h"

#include <filesystem>

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryCrashSummaryTests(TestRegistry& registry)
	{
		registry.Add("Editor.Project.RendererRecoveryCrashSummaryLoadsPreviousReport", [](TestContext& context) {
			TerraEngine::FatalErrorDialogReport report = TerraEngine::BuildFatalErrorDialogReport(
				TerraEngine::FatalErrorCategory::RendererRecoveryFailed,
				"Previous renderer recovery failure.",
				"Renderer rebuild budget exhausted.");
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineRendererRecoveryCrashSummaryTests" / "previous.txt";
			std::string error;

			context.Expect(TerraEngine::SaveFatalErrorSummarySidecar(report, path, &error), "Previous crash summary should write before loading.");
			TerraEngine::FatalErrorDialogReport loaded;
			context.Expect(TerraEngine::LoadFatalErrorSummarySidecar(path, loaded, &error), "Previous crash summary should load on next launch.");
			context.Expect(loaded.shownOnNextLaunch, "Loaded previous crash summaries should be marked for next-launch presentation.");
			context.ExpectEqual(report.shortReason, loaded.shortReason, "Previous crash summary should preserve the reason.");
		});
	}
}
