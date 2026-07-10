#include "TestHarness.h"
#include "Core/FatalErrorReport.h"
#include "Utilities/PlatformUtils.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::Tests
{
	void RegisterFatalErrorDialogTests(TestRegistry& registry)
	{
		registry.Add("Platform.FatalErrorReport.SidecarRemainsInspectable", [](TestContext& context) {
			TerraEngine::FatalErrorDialogReport report = TerraEngine::BuildFatalErrorDialogReport(
				TerraEngine::FatalErrorCategory::RendererRecoveryFailed,
				"Renderer recovery failed in test.",
				"Recovery attempts exhausted.");
			report.crashDumpPath = "C:/Temp/TerraEngineCrash-test.dmp";
			report.crashDumpStatus = TerraEngine::CrashDumpStatus::Failed;
			report.crashDumpFailureReason = "unit test simulated dump failure";
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineFatalErrorDialogTests" / "report.txt";
			std::string error;

			context.Expect(TerraEngine::SaveFatalErrorSummarySidecar(report, path, &error), "Fatal sidecar should write to a temporary test path.");
			std::ifstream input(path, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			context.Expect(contents.str().find("reason=Renderer recovery failed in test.") != std::string::npos, "Fatal sidecar should preserve the human-readable reason.");
			context.Expect(contents.str().find("dump=C:/Temp/TerraEngineCrash-test.dmp") != std::string::npos, "Fatal sidecar should preserve the dump path for manual inspection.");
		});

		registry.Add("Platform.FatalErrorReport.EditorCrashDumpsUseEditorRoot", [](TestContext& context) {
			const std::filesystem::path crashDumpDirectory = TerraEngine::PlatformUtils::GetCrashDumpDirectory(TerraEngine::kEditorFatalErrorProductName);
			const std::string path = crashDumpDirectory.generic_string();

			context.Expect(path.find("TerraEngine Editor/Diagnostics/CrashDumps") != std::string::npos, "Editor crash dumps should use the stable TerraEngine Editor diagnostics root.");
			context.Expect(path.find("SaveData") == std::string::npos, "Editor crash dumps must remain outside persistent project or game save data.");
		});

		registry.Add("Platform.FatalErrorReport.CrashLogUsesCriticalSeverity", [](TestContext& context) {
			const std::filesystem::path sourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Engine" / "Window" / "WindowSystem.cpp";
			std::ifstream input(sourcePath, std::ios::binary);
			std::ostringstream source;
			source << input.rdbuf();

			context.Expect(source.str().find("LogLevel::Critical, \"Fatal error dialog:") != std::string::npos, "The terminal crash report should be emitted as CRITICAL instead of ERROR.");
		});

		registry.Add("Platform.FatalErrorReport.CrashDumpsStayOutOfSaveData", [](TestContext& context) {
			const std::filesystem::path crashDumpDirectory = TerraEngine::PlatformUtils::GetCrashDumpDirectory("TerraEngine Renderer Recovery Test");
			const std::string path = crashDumpDirectory.generic_string();

			context.Expect(!crashDumpDirectory.empty(), "Crash dump directory should resolve to a writable diagnostics path.");
			context.Expect(path.find("CrashDumps") != std::string::npos, "Crash dump directory should include CrashDumps.");
			context.Expect(path.find("SaveData") == std::string::npos, "Crash dump directory must not be inside SaveData.");
		});
	}
}
