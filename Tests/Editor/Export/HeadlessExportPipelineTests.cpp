#include "TestHarness.h"

#include "ExportPipeline.h"
#include "ExportRuntimeLayout.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		class TempHeadlessPipelineDirectory {
		public:
			TempHeadlessPipelineDirectory()
				: root_(std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests" / "HeadlessPipeline") {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				std::filesystem::create_directories(root_);
			}

			~TempHeadlessPipelineDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

		private:
			std::filesystem::path root_;
		};

		class ScopedCurrentDirectory {
		public:
			explicit ScopedCurrentDirectory(const std::filesystem::path& replacement)
				: original_(std::filesystem::current_path()) {
				std::filesystem::current_path(replacement);
			}

			~ScopedCurrentDirectory() {
				std::error_code ignoredError;
				std::filesystem::current_path(original_, ignoredError);
			}

		private:
			std::filesystem::path original_;
		};

		class ScopedEmptyDisplayVariable {
		public:
			ScopedEmptyDisplayVariable() {
				const char* existing = std::getenv("DISPLAY");
				if(existing != nullptr) {
					hadValue_ = true;
					original_ = existing;
				}
#ifdef _WIN32
				_putenv_s("DISPLAY", "");
#else
				unsetenv("DISPLAY");
#endif
			}

			~ScopedEmptyDisplayVariable() {
#ifdef _WIN32
				_putenv_s("DISPLAY", hadValue_ ? original_.c_str() : "");
#else
				if(hadValue_) setenv("DISPLAY", original_.c_str(), 1);
				else unsetenv("DISPLAY");
#endif
			}

		private:
			bool hadValue_ = false;
			std::string original_;
		};

		void Touch(const std::filesystem::path& path, const std::string& contents = "runtime-payload") {
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output << contents;
		}

		ExportRuntimeLayout MakeRuntimeLayout(
			const std::filesystem::path& root,
			ExportPlatform platform)
		{
			ExportRuntimeLayout layout;
			layout.installRoot = root / "Installed";
			layout.editorDirectory = layout.installRoot / "Editor";
			layout.engineDirectory = layout.installRoot / "Engine";
			layout.launcherDirectory = layout.installRoot / "Launcher";
			layout.shaderDirectory = layout.editorDirectory / "Shaders";
			layout.fontDirectory = layout.editorDirectory / "Fonts";
			layout.managedDirectory = layout.editorDirectory / "Managed";
			layout.executablePath = layout.editorDirectory
				/ ("TerraEngine-Export" + GetExportExecutableSuffix(platform));
			layout.modelPackageCompiler = layout.editorDirectory
				/ ("TerraEngineModelPackageCompiler" + GetExportExecutableSuffix(platform));

			Touch(layout.executablePath);
			Touch(layout.modelPackageCompiler);
			Touch(layout.launcherDirectory / GetExportLauncherFileName("TerraEngine-Launcher", platform));
			Touch(layout.launcherDirectory / GetExportEngineLibraryName(platform));
			Touch(layout.launcherDirectory / GetExportGlfwLibraryName(platform));
			std::filesystem::create_directories(layout.engineDirectory);
			std::filesystem::create_directories(layout.managedDirectory);
			Touch(layout.fontDirectory / "Roboto-Medium.ttf", "font-payload");

			const std::vector<std::filesystem::path> requiredShaders = {
				"Default.task.tspf",
				"Default.mesh.tspf",
				"Default.frag.tspf",
				"Editor/SceneViewGrid.vert.spv",
				"Editor/SceneViewGrid.frag.spv",
				"Sky/SkyBackground.task.tspf",
				"Sky/SkyBackground.mesh.tspf",
				"Sky/SkyBackground.frag.tspf",
				"UI/ui_instanced.vert.spv",
				"UI/ui_instanced.frag.spv",
				"UI/ui_static.vert.spv",
				"UI/ui_static.frag.spv",
				"UI/ui_sdf_text.vert.spv",
				"UI/ui_sdf_text.frag.spv",
				"Lighting/ClusterDepthMask.comp.spv",
				"Lighting/ClusterGrid.comp.spv",
				"Lighting/ClusterLightCount.comp.spv",
				"Lighting/ClusterLightFill.comp.spv",
				"Lighting/ClusterLightPrefix.comp.spv",
				"Lighting/LightCull.comp.spv",
				"PostProcess/HiZGenerate.comp.spv",
				"PostProcess/VrsGenerate.comp.spv",
				"PostProcess/Gtao.comp.spv",
				"PostProcess/GtaoBlur.comp.spv"
			};
			for(const std::filesystem::path& shader : requiredShaders) {
				Touch(layout.shaderDirectory / shader, "shader-bytecode");
			}
			return layout;
		}

		ExportRequest MakeContentOnlyRequest(const std::filesystem::path& root) {
			ExportRequest request;
			request.projectRoot = root / "Content Only Project";
			Touch(request.projectRoot / "project.json", R"({"name":"Content Only Project"})");
			Touch(request.projectRoot / "Assets" / "Scenes" / "Startup.tescene", R"({"objects":[]})");
			Touch(request.projectRoot / "Assets" / "UI" / "Baseline.teui", R"({"root":{}})");

			request.profile.name = "Content Only";
			request.profile.id = "content-only";
			request.profile.platform = GetHostExportPlatform();
			request.profile.startupScene = "Scenes/Startup.tescene";
			request.profile.includedScenes = {"Scenes/Startup.tescene"};
			request.profile.compressionPreset = CompressionPreset::None;
			request.profile.enableManagedCompilation = false;
			request.finalOutputDirectory = root / "Published Output";
			request.evidenceDirectory = root / "Evidence" / "content-only-run";
			request.runtimeLayout = MakeRuntimeLayout(root, request.profile.platform);
			request.invocationKind = ExportInvocationKind::HeadlessCli;
			request.runId = "content-only-run";
			return request;
		}
	}

	void RegisterHeadlessExportPipelineTests(TestRegistry& registry) {
		registry.Add("Editor.HeadlessExportPipeline.RuntimeDiscoveryIgnoresCurrentWorkingDirectory", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			const ExportPlatform platform = GetHostExportPlatform();
			const ExportRuntimeLayout expected = MakeRuntimeLayout(tempDirectory.Root(), platform);
			const std::filesystem::path unrelatedDirectory = tempDirectory.Root() / "Unrelated" / "Working Directory";
			std::filesystem::create_directories(unrelatedDirectory);
			ScopedCurrentDirectory currentDirectory(unrelatedDirectory);

			ExportRuntimeLayout discovered;
			std::string error;
			context.Expect(DiscoverExportRuntimeLayout(expected.executablePath, discovered, error), "Installed runtime discovery should derive all paths from the executable, not the process directory.");
			context.Expect(ValidateExportRuntimeLayout(discovered, platform, error), "A complete synthetic installed tree should validate from an unrelated working directory.");
			context.ExpectEqual(std::filesystem::canonical(expected.installRoot).string(), discovered.installRoot.string(), "Runtime discovery should retain the executable-relative install root.");
		});

		registry.Add("Editor.HeadlessExportPipeline.InvalidRuntimeLayoutFailsBeforeContentWork", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			std::filesystem::remove(request.runtimeLayout.modelPackageCompiler);
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request);

			context.Expect(!result.success, "A missing installed runtime dependency should prevent export.");
			context.Expect(result.failureCategory == ExportFailureCategory::InvalidRuntimeLayout, "Runtime-layout rejection should keep its stable failure category.");
			context.ExpectEqual(2, result.exitCode, "Invalid runtime layouts should map to automation exit code 2.");
			context.Expect(!std::filesystem::exists(request.finalOutputDirectory), "Invalid runtime layout must not publish a partial output directory.");
			context.Expect(std::filesystem::is_regular_file(request.evidenceDirectory / "export-result.json"), "A parsed request should retain failure evidence even when runtime validation fails.");
		});

		registry.Add("Editor.HeadlessExportPipeline.ContentOnlyExportNeedsNoDisplayOrGraphicsDevice", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			const std::filesystem::path sourceScene = request.projectRoot / "Assets" / "Scenes" / "Startup.tescene";
			const auto sourceTimestamp = std::filesystem::last_write_time(sourceScene);
			const std::filesystem::path unrelatedDirectory = tempDirectory.Root() / "Launch Elsewhere";
			std::filesystem::create_directories(unrelatedDirectory);
			ScopedCurrentDirectory currentDirectory(unrelatedDirectory);
			ScopedEmptyDisplayVariable emptyDisplay;
			std::vector<ExportStage> observedStages;
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request, [&observedStages](const ExportProgress& progress) {
				observedStages.push_back(progress.stage);
			});

			const std::string firstDiagnostic = result.validationResult.diagnostics.empty()
				? std::string{}
				: " First diagnostic: " + result.validationResult.diagnostics.front().message
					+ " (" + result.validationResult.diagnostics.front().filePath + ")";
			context.Expect(result.success, std::string("A prepared content-only project should export without a window, display, or graphics device: ") + result.message + firstDiagnostic);
			context.Expect(result.failureCategory == ExportFailureCategory::None, "A successful content-only export should report no failure category.");
			context.ExpectEqual(0, result.exitCode, "A successful headless export should return process code 0.");
			context.Expect(result.stage == ExportStage::Complete, "Successful export should finish at the complete stage.");
			context.Expect(std::filesystem::is_regular_file(result.packageResult.outputPath), "The validated TEPAK archive should be published into the final output.");
			context.Expect(std::filesystem::is_regular_file(request.finalOutputDirectory
				/ GetExportLauncherFileName(request.projectRoot.filename().string(), request.profile.platform)), "The installed launcher should be staged beside the archive.");
			context.Expect(std::filesystem::is_regular_file(request.evidenceDirectory / "resolved-manifest.json"), "Successful pipeline execution should write resolved manifest evidence.");
			context.Expect(std::filesystem::is_regular_file(request.evidenceDirectory / "content-inventory.json"), "Successful pipeline execution should write packaged inventory evidence.");
			context.Expect(std::find(observedStages.begin(), observedStages.end(), ExportStage::ManagedCompilation) == observedStages.end(), "Content-only export should skip managed compilation entirely.");
			context.Expect(std::find(observedStages.begin(), observedStages.end(), ExportStage::AssetStaging) != observedStages.end(), "Progress should identify asset staging separately from archive packaging.");
			context.Expect(std::find(observedStages.begin(), observedStages.end(), ExportStage::Complete) != observedStages.end(), "Progress should expose the final protected publication outcome.");
			context.Expect(sourceTimestamp == std::filesystem::last_write_time(sourceScene), "Headless export should read project content without rewriting the live source scene.");
		});
	}
}
