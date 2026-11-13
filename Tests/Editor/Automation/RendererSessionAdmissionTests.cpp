#include "../TestHarness.h"
#include "../TestSha256.h"
#include "Tests/Support/RendererEfficiency/Editor/EditorRendererEfficiencyValidation.h"
#include "Utilities/OwnedProcess.h"
#include "Utilities/PlatformUtils.h"
#include "Utilities/UUID.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace TerraEngine::EditorTests
{
	namespace
	{
		void ExerciseRendererAdmission(TestContext& context, const std::filesystem::path& root, bool missingCameras)
		{
			using TerraEngine::Editor::Automation::EditorRendererEfficiencyValidation;
			std::filesystem::create_directories(root);
			nlohmann::json marker = {{"schemaVersion", 1}, {"kind", "RendererEfficiencyValidationProject"},
				{"projectRoot", std::filesystem::canonical(root).generic_string()},
				{"fixtureId", "admission-regression"}, {"fixtureHash", "owned-admission-hash"}};
			if (missingCameras)
			{
				// Seal both real inputs with an explicit empty camera list. This reaches
				// camera validation rather than failing on a missing input or hash provider.
				const nlohmann::json steps = {{"schemaVersion", 1}, {"fixtureId", "admission-regression"},
					{"fixtureHash", "owned-admission-hash"}, {"seed", 0}, {"stage", "P3"},
					{"cameras", nlohmann::json::array()}, {"nativeExtent", {{"width", 64}, {"height", 64}}},
					{"steps", nlohmann::json::array({{{"id", "depth-step"}, {"ordinal", 0}, {"warmupFrames", 60}, {"measuredFrames", 300}}})}};
				const auto bytes = steps.dump();
				std::ofstream(root / "renderer-efficiency-steps.json") << bytes;
				marker["stepsPath"] = "renderer-efficiency-steps.json";
				marker["stepsHash"] = ComputeTestSha256(bytes);
				const nlohmann::json cameras = {{"schemaVersion", 1}, {"cameras", steps.at("cameras")}, {"nativeExtent", steps.at("nativeExtent")}};
				const auto cameraBytes = cameras.dump();
				std::ofstream(root / "renderer-efficiency-camera.json") << cameraBytes;
				marker["cameraHash"] = ComputeTestSha256(cameraBytes);
			}
			std::ofstream(root / "renderer-efficiency-project.json") << marker;
			EditorRendererEfficiencyValidation host;
			TerraEngine::Renderer::RendererEfficiencyOptionsSnapshot options;
			options.optionsSessionGeneration = 3;
			std::string error;
			const bool admitted = host.Start(root, 17,
				{{"fixtureId", "admission-regression"}, {"fixtureHash", "owned-admission-hash"},
					{"stage", "P3"}, {"runId", "admission-probe"}}, options, error);
			const bool inactiveAfterRejection = !host.Active();
			const bool diagnosticPresent = !error.empty();
			if(missingCameras) {
#if defined(_WIN32)
				context.Expect(error.find("camera") != std::string::npos || error.find("Camera") != std::string::npos, "Missing-camera admission must reach camera validation: " + error);
#else
				// This platform cannot seal installed steps; do not claim camera-path coverage.
				context.Expect(error.find("SHA256 provider unavailable") != std::string::npos, "Non-Windows admission must identify its unavailable hash provider: " + error);
#endif
			}
			// The known-good P1 control requires no installed P3 assets. Retain the
			// same host so leaked active state cannot hide behind a fresh instance.
			marker.erase("stepsPath");
			marker.erase("stepsHash");
			std::ofstream(root / "renderer-efficiency-project.json") << marker;
			const bool restarted = host.Start(root, 17,
				{{"fixtureId", "admission-regression"}, {"fixtureHash", "owned-admission-hash"}, {"stage", "P1"}}, options, error);
			context.Expect(!admitted && inactiveAfterRejection && diagnosticPresent,
				"Missing P3 steps/cameras must reject before any active session state is published.");
			context.Expect(restarted, "The same host must admit a valid start after rejected P3 input: " + error);
		}

		void CheckRendererAdmission(TestContext& context, const std::string& testName, bool missingCameras)
		{
			if (const char* root = std::getenv("TERRAENGINE_RENDERER_ADMISSION_CHILD_ROOT"))
			{
				ExerciseRendererAdmission(context, root, missingCameras);
				return;
			}
			// Missing P3 cameras previously reached vector::front after publishing
			// active=true. An owned subprocess contains that possible native abort.
			const auto root = std::filesystem::temp_directory_path()
				/ ("TerraEngine-RendererAdmission-" + TerraEngine::ToHexString(TerraEngine::GenerateUUID()));
			const auto executable = TerraEngine::PlatformUtils::GetExecutablePath();
			TerraEngine::Utilities::OwnedProcessRequest request;
			request.workingDirectory = executable.parent_path();
			request.commandLine = "\"" + executable.string() + "\"";
			request.timeout = std::chrono::seconds(10);
			request.environment = {{"TERRAENGINE_RENDERER_ADMISSION_CHILD_ROOT", root.string()},
				{"TERRAENGINE_TEST_FILTER", testName}, {"TERRAENGINE_TEST_EXCLUDE_FILTER", ""}};
			const std::atomic_bool canceled{false};
			const auto child = TerraEngine::Utilities::OwnedProcess::Run(request, canceled);
			std::error_code ignored;
			// Only this generated direct child of temp may be removed, including
			// after a crashed pre-fix child left its small fixture files behind.
			const auto absolute = std::filesystem::weakly_canonical(root, ignored);
			const auto temporary = std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(), ignored);
			if (!ignored && absolute.parent_path() == temporary
				&& absolute.filename().string().starts_with("TerraEngine-RendererAdmission-"))
				std::filesystem::remove_all(absolute, ignored);
			context.Expect(child.started && !child.timedOut && child.exitCode == 0,
				"P3 admission child must reject safely and admit its valid control (exit " + std::to_string(child.exitCode)
					+ ", timeout " + std::to_string(child.timedOut) + "): " + child.output);
		}
	}

	void RegisterRendererSessionAdmissionTests(TestRegistry& registry)
	{
		registry.Add("Editor.RendererSessionAdmission.MissingInstalledP3StepsDoesNotActivate", [](TestContext& context) {
			CheckRendererAdmission(context, "Editor.RendererSessionAdmission.MissingInstalledP3StepsDoesNotActivate", false);
		});
		registry.Add("Editor.RendererSessionAdmission.MissingInstalledP3CamerasDoesNotActivate", [](TestContext& context) {
			CheckRendererAdmission(context, "Editor.RendererSessionAdmission.MissingInstalledP3CamerasDoesNotActivate", true);
		});
	}
}
