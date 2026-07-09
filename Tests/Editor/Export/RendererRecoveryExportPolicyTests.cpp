#include "TestHarness.h"
#include "BuildProfile.h"
#include "Renderer/RendererRecoveryPolicy.h"

#include <filesystem>
#include <fstream>
#include <sstream>

namespace TerraEngine::EditorTests
{
	void RegisterRendererRecoveryExportPolicyTests(TestRegistry& registry)
	{
		registry.Add("Editor.Export.RendererRecoveryPolicyDefaultsAreBounded", [](TestContext& context) {
			const TerraEngine::Renderer::RendererRecoveryPolicy policy =
				TerraEngine::Renderer::MakeExportRendererRecoveryPolicy(false, true, 2u, {});

			context.Expect(policy.diagnosticsEnabled, "Export recovery policy should keep diagnostics enabled when requested.");
			context.Expect(policy.fatalOnExhaustedAttempts, "Export recovery policy should be fatal after bounded attempts are exhausted.");
			context.ExpectEqual(2u, policy.maxRendererRebuildAttemptsPerIncident, "Export recovery policy should preserve the configured attempt budget.");
		});

		registry.Add("Editor.Export.RendererRecoveryPolicyRoundTripsThroughBuildProfile", [](TestContext& context) {
			BuildProfile profile;
			profile.name = "Recovery Policy";
			profile.id = "recoverypolicy";
			profile.rendererRecoveryDiagnosticsEnabled = false;
			profile.rendererRecoveryMaxRebuildAttempts = 3u;
			profile.rendererRecoveryFatalMessage = "Renderer recovery failed in export.";
			const std::filesystem::path path = std::filesystem::temp_directory_path() / "TerraEngineEditorTests" / "RendererRecoveryBuildProfile" / "profile.json";

			context.Expect(SaveBuildProfile(profile, path), "Build profile should save renderer recovery policy fields.");
			const BuildProfile loaded = LoadBuildProfile(path);

			context.Expect(!loaded.rendererRecoveryDiagnosticsEnabled, "Renderer recovery diagnostics toggle should round-trip.");
			context.ExpectEqual(3u, loaded.rendererRecoveryMaxRebuildAttempts, "Renderer recovery attempt budget should round-trip.");
			context.ExpectEqual(profile.rendererRecoveryFatalMessage, loaded.rendererRecoveryFatalMessage, "Renderer recovery fatal message should round-trip.");
		});

		registry.Add("Editor.Export.RendererRecoveryManifestIsPackagedFromProfile", [](TestContext& context) {
			const std::filesystem::path pipelinePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR) / "Editor" / "Export" / "ExportPipeline.cpp";
			std::ifstream stream(pipelinePath, std::ios::binary);
			std::ostringstream buffer;
			buffer << stream.rdbuf();
			const std::string source = buffer.str();

			context.Expect(source.find("BuildRendererRecoveryManifest") != std::string::npos, "Export pipeline should build renderer recovery manifest metadata.");
			context.Expect(source.find("\"maxRendererRebuildAttemptsPerIncident\"") != std::string::npos, "Export manifest should publish the bounded rebuild attempt budget.");
			context.Expect(source.find("\"fullApplicationRestart\"") != std::string::npos, "Export manifest should explicitly disable full application restart recovery.");
		});
	}
}
