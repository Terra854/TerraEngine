#include "TestHarness.h"

#include "ExportEvidence.h"
#include "ExportPipeline.h"
#include "ExportRuntimeLayout.h"
#include "ExportValidator.h"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace TerraEngine::EditorTests {
	namespace {
		BuildProfile MakeParityProfile();

		class TempTwoRunParityDirectory {
		public:
			TempTwoRunParityDirectory()
				: root_(std::filesystem::temp_directory_path()
					/ "TerraEngineEditorTests" / "TwoRunExportParity") {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				std::filesystem::create_directories(root_);
			}

			~TempTwoRunParityDirectory() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
			}

			[[nodiscard]] const std::filesystem::path& Root() const {
				return root_;
			}

		private:
			std::filesystem::path root_;
		};

		void TouchParityFile(
			const std::filesystem::path& path,
			const std::string& contents = "runtime-payload")
		{
			std::filesystem::create_directories(path.parent_path());
			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			output << contents;
		}

		std::string ReadParityText(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			std::ostringstream buffer;
			buffer << input.rdbuf();
			return buffer.str();
		}

		ExportRuntimeLayout MakeParityRuntimeLayout(
			const std::filesystem::path& installRoot,
			const std::filesystem::path& executablePath,
			ExportPlatform platform)
		{
			ExportRuntimeLayout layout;
			layout.installRoot = installRoot;
			layout.editorDirectory = installRoot / "Editor";
			layout.engineDirectory = installRoot / "Engine";
			layout.launcherDirectory = installRoot / "Launcher";
			layout.shaderDirectory = layout.editorDirectory / "Shaders";
			layout.fontDirectory = layout.editorDirectory / "Fonts";
			layout.managedDirectory = layout.editorDirectory / "Managed";
			layout.dotnetSdkDirectory = layout.editorDirectory / "DotNet";
#ifdef _WIN32
			layout.dotnetExecutable = layout.dotnetSdkDirectory / "dotnet.exe";
#else
			layout.dotnetExecutable = layout.dotnetSdkDirectory / "dotnet";
#endif
			layout.nativeAotToolchainDirectory = layout.editorDirectory / "NativeAotNuGet";
			layout.nativeAotNugetConfig = layout.nativeAotToolchainDirectory / "NuGet.Config";
			layout.nativeAotNugetFeed = layout.nativeAotToolchainDirectory / "feed";
			layout.nativeAotGlobalJson = layout.nativeAotToolchainDirectory / "global.json";
			layout.executablePath = executablePath;
			layout.modelPackageCompiler = layout.editorDirectory
				/ ("TerraEngineModelPackageCompiler" + GetExportExecutableSuffix(platform));

			TouchParityFile(layout.executablePath);
			TouchParityFile(layout.modelPackageCompiler);
			TouchParityFile(layout.launcherDirectory
				/ GetExportLauncherFileName("TerraEngine-Launcher", platform));
			TouchParityFile(layout.launcherDirectory / GetExportEngineLibraryName(platform));
			TouchParityFile(layout.launcherDirectory / GetExportGlfwLibraryName(platform));
			std::filesystem::create_directories(layout.engineDirectory);
			std::filesystem::create_directories(layout.managedDirectory);
			std::filesystem::create_directories(layout.dotnetSdkDirectory);
			TouchParityFile(layout.dotnetExecutable);
			std::filesystem::create_directories(layout.nativeAotNugetFeed);
			TouchParityFile(layout.nativeAotNugetConfig, "offline-config");
			TouchParityFile(layout.nativeAotGlobalJson, "sdk-pin");
			TouchParityFile(layout.fontDirectory / "Roboto-Medium.ttf", "font-payload");

			// Package inspection deliberately requires the complete runtime shader
			// inventory. Tiny non-placeholder payloads exercise paths and parity
			// without requiring a graphics device to interpret shader bytecode.
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
				TouchParityFile(layout.shaderDirectory / shader, "shader-bytecode");
			}
			return layout;
		}

		ExportRequest MakeTwoRunParityRequest(
			const std::filesystem::path& root,
			const ExportRuntimeLayout& runtimeLayout,
			ExportInvocationKind invocationKind,
			const std::string& runId,
			const std::filesystem::path& outputDirectory,
			const std::filesystem::path& evidenceDirectory)
		{
			ExportRequest request;
			request.projectRoot = root / "Parity Project";
			request.profile = MakeParityProfile();
			request.profile.compressionPreset = CompressionPreset::None;
			request.finalOutputDirectory = outputDirectory;
			request.evidenceDirectory = evidenceDirectory;
			request.runtimeLayout = runtimeLayout;
			request.invocationKind = invocationKind;
			request.runId = runId;
			return request;
		}

		std::vector<ExportContentInventoryEntry> ReadParityInventory(
			const std::filesystem::path& evidenceDirectory)
		{
			const nlohmann::json document = nlohmann::json::parse(
				ReadParityText(evidenceDirectory / "content-inventory.json"));
			std::vector<ExportContentInventoryEntry> entries;
			for(const nlohmann::json& item : document.at("entries")) {
				ExportContentInventoryEntry entry;
				entry.logicalPath = item.at("logicalPath").get<std::string>();
				entry.assetType = item.at("assetType").get<std::string>();
				entry.sourceKind = item.at("sourceKind").get<std::string>();
				entry.byteSize = item.at("byteSize").get<std::uint64_t>();
				entry.contentHash = item.at("contentHash").get<std::string>();
				entry.required = item.at("required").get<bool>();
				entry.placeholderDetected = item.at("placeholderDetected").get<bool>();
				entries.push_back(std::move(entry));
			}
			return entries;
		}

		BuildProfile MakeParityProfile() {
			BuildProfile profile;
			profile.name = "Parity Profile";
			profile.id = "parity-profile";
			profile.platform = GetHostExportPlatform();
			profile.outputDirectory = "Builds/StoredOutput";
			profile.startupScene = "Scenes/Startup.tescene";
			profile.includedScenes = {
				"Scenes/Startup.tescene",
				"Scenes/Secondary.tescene"
			};
			profile.enableManagedCompilation = false;
			profile.enableVulkanValidationLayers = true;
			profile.enableRenderDebugDiagnostics = true;
			profile.rendererRecoveryDiagnosticsEnabled = true;
			profile.virtualGeometryQualityLevel = 3;
			profile.virtualGeometryBudgetBytes = 128u * 1024u * 1024u;
			return profile;
		}

		std::string MakeResolvedManifest(
			const std::string& runId,
			const std::string& invocationKind,
			const std::string& outputDirectory)
		{
			return std::string("{")
				+ "\"runId\":\"" + runId + "\","
				+ "\"invocationKind\":\"" + invocationKind + "\","
				+ "\"outputDirectory\":\"" + outputDirectory + "\","
				+ "\"startupScene\":\"Scenes/Startup.tescene\","
				+ "\"scenes\":[\"Scenes/Startup.tescene\",\"Scenes/Secondary.tescene\"],"
				+ "\"renderProfile\":{\"shadows\":\"high\",\"resolutionScale\":1.0},"
				+ "\"residencyPolicy\":{\"enabled\":true,\"cleanupIntervalSeconds\":1.0},"
				+ "\"virtualGeometry\":{\"quality\":\"high\",\"geometryBudgetBytes\":134217728},"
				+ "\"rendererRecovery\":{\"diagnosticsEnabled\":true,\"mode\":\"failClosed\"}"
				+ "}";
		}
	}

	void RegisterExportParityTests(TestRegistry& registry) {
		registry.Add("Editor.ExportParity.ManifestExcludesOnlyInvocationMetadata", [](TestContext& context) {
			const std::string editorManifest = MakeResolvedManifest(
				"editor-run", "editor", "C:/Builds/Editor");
			const std::string headlessManifest = MakeResolvedManifest(
				"headless-run", "headless_cli", "D:/Automation/Headless");

			const std::string normalizedEditor = NormalizeResolvedManifestForParity(editorManifest);
			const std::string normalizedHeadless = NormalizeResolvedManifestForParity(headlessManifest);

			context.Expect(!normalizedEditor.empty(), "Valid resolved manifests should normalize into comparable JSON.");
			context.ExpectEqual(normalizedEditor, normalizedHeadless, "Run id, invocation kind, and absolute output path are the only manifest invocation metadata excluded from parity.");
		});

		registry.Add("Editor.ExportParity.IncludedSceneAndRuntimeSettingDriftFails", [](TestContext& context) {
			const std::string authoritative = MakeResolvedManifest(
				"shared-run", "editor", "C:/Builds/One");
			std::string changedScene = MakeResolvedManifest(
				"shared-run", "headless_cli", "C:/Builds/Two");
			const std::size_t scenePosition = changedScene.find("Scenes/Secondary.tescene");
			changedScene.replace(scenePosition, std::string("Scenes/Secondary.tescene").size(), "Scenes/Different.tescene");

			std::string changedRuntime = MakeResolvedManifest(
				"shared-run", "headless_cli", "C:/Builds/Two");
			const std::size_t qualityPosition = changedRuntime.find("\"quality\":\"high\"");
			changedRuntime.replace(qualityPosition, std::string("\"quality\":\"high\"").size(), "\"quality\":\"low\"");

			context.Expect(NormalizeResolvedManifestForParity(authoritative)
				!= NormalizeResolvedManifestForParity(changedScene), "A different included scene should fail editor/headless parity.");
			context.Expect(NormalizeResolvedManifestForParity(authoritative)
				!= NormalizeResolvedManifestForParity(changedRuntime), "A different runtime setting should fail editor/headless parity.");
			context.Expect(NormalizeResolvedManifestForParity("{ malformed").empty(), "Malformed evidence should fail normalization instead of comparing as valid content.");
		});

		registry.Add("Editor.ExportParity.InventoryComparisonIgnoresDiscoveryOrderOnly", [](TestContext& context) {
			const ExportContentInventoryEntry scene = {
				"Scenes/Startup.tescene", "scene", "raw", 128u, "scene-hash", true, false
			};
			const ExportContentInventoryEntry mesh = {
				"Meshes/Baseline.temf", "mesh", "compiled", 256u, "mesh-hash", true, false
			};
			std::vector<ExportContentInventoryEntry> editorInventory = {scene, mesh};
			std::vector<ExportContentInventoryEntry> headlessInventory = {mesh, scene};

			context.Expect(EquivalentExportContentInventories(editorInventory, headlessInventory), "Equivalent inventory should compare equal even when filesystem discovery order differs.");
			headlessInventory.front().required = false;
			context.Expect(!EquivalentExportContentInventories(editorInventory, headlessInventory), "Required-entry drift should fail inventory parity.");
			headlessInventory = {mesh, scene};
			headlessInventory.front().placeholderDetected = true;
			context.Expect(!EquivalentExportContentInventories(editorInventory, headlessInventory), "Placeholder status drift should fail inventory parity.");
		});

		registry.Add("Editor.ExportParity.SharedValidatorReturnsIdenticalDecisions", [](TestContext& context) {
			BuildProfile sharedProfile = MakeParityProfile();
			sharedProfile.startupScene.clear();
			const std::filesystem::path projectRoot = std::filesystem::temp_directory_path()
				/ "TerraEngineEditorTests" / "ParityValidationProject";
			std::error_code ignoredError;
			std::filesystem::remove_all(projectRoot, ignoredError);
			std::filesystem::create_directories(projectRoot);
			ExportValidator validator;

			// Both entry points submit the same immutable profile and project values
			// to this validator; invocation metadata is deliberately absent here.
			const ExportValidationResult editorResult = validator.ValidateAll(sharedProfile, projectRoot);
			const ExportValidationResult headlessResult = validator.ValidateAll(sharedProfile, projectRoot);

			context.ExpectEqual(editorResult.passed, headlessResult.passed, "Editor and headless requests should receive the same validation decision.");
			context.ExpectEqual(editorResult.errorCount, headlessResult.errorCount, "Editor and headless requests should receive the same error count.");
			context.ExpectEqual(editorResult.warningCount, headlessResult.warningCount, "Editor and headless requests should receive the same warning count.");
			context.ExpectEqual(editorResult.diagnostics.size(), headlessResult.diagnostics.size(), "Editor and headless requests should receive the same structured findings.");
			for(std::size_t index = 0; index < editorResult.diagnostics.size(); ++index) {
				const ExportDiagnostic& editorDiagnostic = editorResult.diagnostics[index];
				const ExportDiagnostic& headlessDiagnostic = headlessResult.diagnostics[index];
				context.Expect(editorDiagnostic.severity == headlessDiagnostic.severity, "Parity should retain diagnostic severity.");
				context.ExpectEqual(editorDiagnostic.category, headlessDiagnostic.category, "Parity should retain diagnostic category.");
				context.ExpectEqual(editorDiagnostic.filePath, headlessDiagnostic.filePath, "Parity should retain the affected path.");
				context.ExpectEqual(editorDiagnostic.message, headlessDiagnostic.message, "Parity should retain the shared validation explanation.");
			}
			std::filesystem::remove_all(projectRoot, ignoredError);
		});

		registry.Add("Editor.ExportParity.EditorAndHeadlessPipelinesProduceEquivalentEvidence", [](TestContext& context) {
			TempTwoRunParityDirectory tempDirectory;
			const ExportPlatform platform = GetHostExportPlatform();
			const std::filesystem::path installRoot = tempDirectory.Root() / "Installed";
			const ExportRuntimeLayout runtimeLayout = MakeParityRuntimeLayout(
				installRoot,
				installRoot / "Editor" / ("TerraEngine-Export" + GetExportExecutableSuffix(platform)),
				platform);

			const std::filesystem::path projectRoot = tempDirectory.Root() / "Parity Project";
			TouchParityFile(projectRoot / "project.json", R"({"name":"Parity Project"})");
			TouchParityFile(projectRoot / "Assets" / "Scenes" / "Startup.tescene", R"({"objects":[]})");
			TouchParityFile(projectRoot / "Assets" / "Scenes" / "Secondary.tescene", R"({"objects":[]})");
			TouchParityFile(projectRoot / "Assets" / "UI" / "Parity.teui", R"({"root":{}})");
			TouchParityFile(projectRoot / "ProjectSettings" / "RenderSettings.json",
				R"({"settings":{"maxShadowLights":4,"shadowResolution":1536,"ambientOcclusionStrength":0.75,"ambientOcclusionQuality":2,"textureBudgetMb":384,"resolutionScale":0.9,"variableRateShading":1}})");

			const ExportRequest editorRequest = MakeTwoRunParityRequest(
				tempDirectory.Root(), runtimeLayout, ExportInvocationKind::Editor,
				"editor-parity-run", tempDirectory.Root() / "Editor Output",
				tempDirectory.Root() / "Evidence" / "Editor");
			const ExportRequest headlessRequest = MakeTwoRunParityRequest(
				tempDirectory.Root(), runtimeLayout, ExportInvocationKind::HeadlessCli,
				"headless-parity-run", tempDirectory.Root() / "Headless Output",
				tempDirectory.Root() / "Evidence" / "Headless");
			ExportPipeline editorPipeline;
			ExportPipeline headlessPipeline;

			const ExportResult editorResult = editorPipeline.Execute(editorRequest);
			const ExportResult headlessResult = headlessPipeline.Execute(headlessRequest);

			context.Expect(editorResult.success, std::string("The editor invocation should complete the shared content-only pipeline: ") + editorResult.message);
			context.Expect(headlessResult.success, std::string("The headless invocation should complete the shared content-only pipeline: ") + headlessResult.message);
			context.Expect(editorResult.validationResult.passed == headlessResult.validationResult.passed, "Both entry points should receive the same validation decision.");
			context.ExpectEqual(editorResult.validationResult.errorCount, headlessResult.validationResult.errorCount, "Both entry points should receive the same validation error count.");
			context.ExpectEqual(editorResult.validationResult.warningCount, headlessResult.validationResult.warningCount, "Both entry points should receive the same validation warning count.");
			context.ExpectEqual(editorResult.validationResult.diagnostics.size(), headlessResult.validationResult.diagnostics.size(), "Both entry points should receive the same number of validation diagnostics.");
			for(std::size_t index = 0; index < editorResult.validationResult.diagnostics.size(); ++index) {
				const ExportDiagnostic& editorDiagnostic = editorResult.validationResult.diagnostics[index];
				const ExportDiagnostic& headlessDiagnostic = headlessResult.validationResult.diagnostics[index];
				context.Expect(editorDiagnostic.severity == headlessDiagnostic.severity, "Two-run parity should retain diagnostic severity.");
				context.ExpectEqual(editorDiagnostic.category, headlessDiagnostic.category, "Two-run parity should retain diagnostic category.");
				context.ExpectEqual(editorDiagnostic.stage, headlessDiagnostic.stage, "Two-run parity should retain stable diagnostic stage.");
				context.ExpectEqual(editorDiagnostic.code, headlessDiagnostic.code, "Two-run parity should retain stable diagnostic code.");
				context.ExpectEqual(editorDiagnostic.filePath, headlessDiagnostic.filePath, "Two-run parity should retain the affected path.");
			}

			const std::string editorManifestText = ReadParityText(
				editorRequest.evidenceDirectory / "resolved-manifest.json");
			const std::string headlessManifestText = ReadParityText(
				headlessRequest.evidenceDirectory / "resolved-manifest.json");
			context.ExpectEqual(editorManifestText, headlessManifestText, "Shared pipeline inputs should produce byte-identical resolved manifests.");
			context.ExpectEqual(
				NormalizeResolvedManifestForParity(editorManifestText),
				NormalizeResolvedManifestForParity(headlessManifestText),
				"Normalized manifest parity should also exclude any future invocation-only metadata.");

			const nlohmann::json editorManifest = nlohmann::json::parse(editorManifestText);
			const nlohmann::json headlessManifest = nlohmann::json::parse(headlessManifestText);
			context.Expect(editorManifest.at("startupScene") == headlessManifest.at("startupScene"), "Both entry points should preserve the same startup scene.");
			context.Expect(editorManifest.at("scenes") == headlessManifest.at("scenes"), "Both entry points should preserve the same included scenes and order.");
			context.ExpectEqual(2u, static_cast<unsigned int>(editorManifest.at("scenes").size()), "The parity fixture should exercise more than the startup scene.");
			for(const char* settingsBlock : {"renderProfile", "residencyPolicy", "virtualGeometry", "rendererRecovery"}) {
				context.Expect(editorManifest.contains(settingsBlock), "The resolved manifest should retain every required runtime settings block.");
				context.Expect(editorManifest.at(settingsBlock) == headlessManifest.at(settingsBlock), "Editor and headless runtime settings should remain identical.");
			}
			context.ExpectEqual(std::string("medium"), editorManifest.at("renderProfile").at("shadows").get<std::string>(), "Project render settings should drive the shared resolved render profile.");
			context.ExpectEqual(3u, editorManifest.at("virtualGeometry").at("qualityLevel").get<unsigned int>(), "Profile virtual-geometry settings should survive both entry points.");

			const std::vector<ExportContentInventoryEntry> editorInventory = ReadParityInventory(editorRequest.evidenceDirectory);
			const std::vector<ExportContentInventoryEntry> headlessInventory = ReadParityInventory(headlessRequest.evidenceDirectory);
			context.Expect(!editorInventory.empty(), "A successful two-run parity export should retain inspectable content inventory.");
			context.Expect(EquivalentExportContentInventories(editorInventory, headlessInventory), "Editor and headless package inputs should match by path, type, size, hash, required status, and placeholder status.");
		});

		registry.Add("Editor.ExportParity.BuildTreeExecutableDiscoversInstalledPayload", [](TestContext& context) {
			TempTwoRunParityDirectory tempDirectory;
			const ExportPlatform platform = GetHostExportPlatform();
			const std::filesystem::path developerBuildDirectory = tempDirectory.Root() / "DeveloperBuild";
			const std::filesystem::path executablePath = developerBuildDirectory
				/ ("TerraEngine-Export" + GetExportExecutableSuffix(platform));
			const ExportRuntimeLayout expected = MakeParityRuntimeLayout(
				developerBuildDirectory / "Build", executablePath, platform);
			ExportRuntimeLayout discovered;
			std::string error;

			context.Expect(DiscoverExportRuntimeLayout(executablePath, discovered, error), "A build-tree executable should resolve the installed payload below its sibling Build directory.");
			context.Expect(ValidateExportRuntimeLayout(discovered, platform, error), "The discovered build-tree payload should satisfy the same runtime contract as an installed executable.");
			context.Expect(std::filesystem::canonical(expected.installRoot) == discovered.installRoot, "Build-tree discovery should choose <build-directory>/Build as its install root.");
			context.Expect(std::filesystem::canonical(expected.editorDirectory) == discovered.editorDirectory, "Build-tree discovery should choose the installed Editor tool directory, not the executable directory.");
		});
	}
}
