#include "TestHarness.h"
#include "Core/FatalErrorReport.h"
#include "Utilities/PlatformUtils.h"

#include <filesystem>

namespace TerraEngine::Tests
{
	void RegisterFatalErrorDialogTests(TestRegistry& registry)
	{
		registry.Add("Platform.FatalErrorReport.SidecarRoundTrips", [](TestContext& context) {
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
			TerraEngine::FatalErrorDialogReport loaded;
			context.Expect(TerraEngine::LoadFatalErrorSummarySidecar(path, loaded, &error), "Fatal sidecar should load from a temporary test path.");
			context.ExpectEqual(report.shortReason, loaded.shortReason, "Fatal sidecar should preserve the short reason.");
			context.ExpectEqual(report.crashDumpPath.generic_string(), loaded.crashDumpPath.generic_string(), "Fatal sidecar should preserve the dump path.");
			context.Expect(loaded.crashDumpStatus == TerraEngine::CrashDumpStatus::Failed, "Fatal sidecar should preserve the dump status.");
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
