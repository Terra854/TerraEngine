#include "TestHarness.h"

#include "AnimationExportDependencies.h"
#include "AssetImportResult.h"
#include "ExportPipeline.h"
#include "ExportRuntimeLayout.h"
#include "MaterialAssetWriter.h"
#include "TEPAK/PackReader.h"
#include "Utilities/UUID.h"
#include "validation/AnimationPackValidator.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
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

		std::string ReadArchiveText(
			const tepak::PackReader& reader,
			const std::string& path)
		{
			const tepak::ByteBuffer bytes = reader.ReadAll(path);
			return std::string(
				reinterpret_cast<const char*>(bytes.data()),
				bytes.size());
		}

		std::string AddGeneratedPrefabPackage(
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& runtimePath =
				"Models/Generated/Headless.teprefab")
		{
			using namespace TerraEngine::AssetPipeline;
			ModelPackageImportResult result;
			result.status = ModelImportStatus::Succeeded;
			result.packageGuid = TerraEngine::GenerateUUID();
			result.animationManifest.packageGuid = result.packageGuid;
			result.animationManifest.sourceHashHex = "headless-prefab-source";
			result.animationManifest.importerName = "GltfPackageImporter";
			result.animationManifest.settingsFingerprintHex = "headless-prefab-settings";
			result.animationManifest.candidateGeneration = 1U;
			result.animationManifest.publishedGeneration = 1U;
			result.animationManifest.lastKnownGoodGeneration = 1U;
			result.animationManifest.publishedAtomically = true;

			GeneratedAnimationSubAsset prefab;
			prefab.guid = TerraEngine::GenerateUUID();
			prefab.kind = AnimationImportSubAssetKind::Prefab;
			prefab.semanticKey = "prefab:headless";
			prefab.displayName = "Headless Generated Prefab";
			prefab.sourcePath = runtimePath;
			const std::string prefabGuid = TerraEngine::ToHexString(prefab.guid);
			result.animationManifest.subAssets.push_back(std::move(prefab));
			CanonicalizeAnimationImportManifest(result.animationManifest);
			result.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(result.animationManifest);

			const std::filesystem::path sourcePath =
				projectRoot / "Assets" / "Models" / "Headless.gltf";
			Touch(
				projectRoot / "Assets" / runtimePath,
				R"({"objects":[]})");
			if(!SaveModelPackageImportResult(sourcePath, result))
			{
				return {};
			}
			return prefabGuid;
		}

		std::string AddGeneratedMaterialPackage(
			const std::filesystem::path& projectRoot,
			const std::filesystem::path& materialRuntimePath =
				"Models/Generated/HeadlessMaterial.tmat",
			const std::filesystem::path& animationRuntimePath = {})
		{
			using namespace TerraEngine::AssetPipeline;
			const std::filesystem::path manifestRuntimePath =
				animationRuntimePath.empty()
					? materialRuntimePath
					: animationRuntimePath;
			const TerraEngine::UUID materialGuid = TerraEngine::GenerateUUID();

			ModelPackageImportResult result;
			result.status = ModelImportStatus::Succeeded;
			result.packageGuid = TerraEngine::GenerateUUID();

			// The package result is one of the ordinary export discovery sources.
			// Its authored-case path intentionally matches the standalone material
			// metadata below, just like a real model importer publication.
			GeneratedModelMaterialAsset generatedMaterial;
			generatedMaterial.guid = materialGuid;
			generatedMaterial.sourcePath = materialRuntimePath;
			generatedMaterial.compiledPath = materialRuntimePath;
			generatedMaterial.displayName = "Headless Generated Material";
			result.generatedMaterials.push_back(std::move(generatedMaterial));

			result.animationManifest.packageGuid = result.packageGuid;
			result.animationManifest.sourceHashHex = "headless-material-source";
			result.animationManifest.importerName = "GltfPackageImporter";
			result.animationManifest.settingsFingerprintHex =
				"headless-material-settings";
			result.animationManifest.candidateGeneration = 1U;
			result.animationManifest.publishedGeneration = 1U;
			result.animationManifest.lastKnownGoodGeneration = 1U;
			result.animationManifest.publishedAtomically = true;

			// Animation closure normalization lowercases this runtime path. The
			// exporter must coalesce it with the authored-case metadata record only
			// after the staged bytes have also been proven identical.
			GeneratedAnimationSubAsset manifestMaterial;
			manifestMaterial.guid = materialGuid;
			manifestMaterial.kind = AnimationImportSubAssetKind::Material;
			manifestMaterial.semanticKey = "material:headless";
			manifestMaterial.displayName = "Headless Generated Material";
			manifestMaterial.sourcePath = manifestRuntimePath;
			manifestMaterial.compiledPath = manifestRuntimePath;
			result.animationManifest.subAssets.push_back(
				std::move(manifestMaterial));
			CanonicalizeAnimationImportManifest(result.animationManifest);
			result.animationManifest.dependencyFingerprintHex =
				BuildAnimationImportDependencyFingerprint(
					result.animationManifest);

			const std::filesystem::path sourcePath =
				projectRoot / "Assets" / "Models" / "HeadlessMaterial.gltf";
			// Pre-export metadata validation requires both the authored package and
			// its legacy parent output to exist. Staging still excludes the source
			// model and registers only the generated child material.
			Touch(sourcePath, R"({"asset":{"version":"2.0"}})");
			Touch(
				projectRoot / "Build" / "Assets"
					/ "Models" / "HeadlessMaterial.temf");
			MaterialAssetWriteDesc materialDesc;
			materialDesc.header =
				CreateDefaultMaterialHeader(TerraEngine::EMPTY_UUID);
			materialDesc.header.baseColorFactor[0] = 0.8f;
			if(!WriteMaterialAsset(
					projectRoot / "Assets" / materialRuntimePath,
					materialDesc)
				|| !WriteMaterialAsset(
					projectRoot / "Build" / "Assets"
						/ materialRuntimePath,
					materialDesc))
			{
				return {};
			}
			if(manifestRuntimePath != materialRuntimePath)
			{
				MaterialAssetWriteDesc conflictingMaterialDesc = materialDesc;
				conflictingMaterialDesc.header.baseColorFactor[0] = 0.1f;
				if(!WriteMaterialAsset(
					projectRoot / "Assets" / manifestRuntimePath,
					conflictingMaterialDesc)
					|| !WriteMaterialAsset(
					projectRoot / "Build" / "Assets"
						/ manifestRuntimePath,
					conflictingMaterialDesc))
				{
					return {};
				}
			}

			const std::string materialGuidText =
				TerraEngine::ToHexString(materialGuid);
			Touch(
				projectRoot / "Assets"
					/ (materialRuntimePath.generic_string() + ".meta"),
				nlohmann::json{
					{"compiledPath", materialRuntimePath.generic_string()},
					{"guid", materialGuidText},
					{"importerName", "MaterialImporter"},
					{"sourcePath", materialRuntimePath.generic_string()}
				}.dump(2));
			Touch(
				sourcePath.string() + ".meta",
				nlohmann::json{
					{"compiledPath", "Models/HeadlessMaterial.temf"},
					{"guid", TerraEngine::ToHexString(result.packageGuid)},
					{"importerName", "GltfPackageImporter"},
					{"sourcePath", "Models/HeadlessMaterial.gltf"}
				}.dump(2));
			if(!SaveModelPackageImportResult(sourcePath, result))
			{
				return {};
			}
			return materialGuidText;
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
			layout.nativeAotToolchainDirectory = layout.editorDirectory / "NativeAotNuGet";
			layout.nativeAotNugetConfig = layout.nativeAotToolchainDirectory / "NuGet.Config";
			layout.nativeAotNugetFeed = layout.nativeAotToolchainDirectory / "feed";
			layout.nativeAotGlobalJson = layout.nativeAotToolchainDirectory / "global.json";
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
			std::filesystem::create_directories(layout.nativeAotNugetFeed);
			Touch(layout.nativeAotNugetConfig, "offline-config");
			Touch(layout.nativeAotGlobalJson, "sdk-pin");
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

		registry.Add("Editor.HeadlessExportPipeline.GeneratedPrefabStaysOutsideResourceAssetMap", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			const std::string prefabGuid =
				AddGeneratedPrefabPackage(request.projectRoot);
			context.Expect(
				!prefabGuid.empty(),
				"The generated-prefab fixture must publish a valid import sidecar.");
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request);
			context.Expect(
				result.success,
				"A generated package prefab should remain an archive document instead "
				"of being rejected as an unknown ResourceManager type: " + result.message);
			if(!result.success)
			{
				return;
			}

			const tepak::PackReader reader =
				tepak::PackReader::Open(result.packageResult.outputPath);
			const nlohmann::json gameManifest = nlohmann::json::parse(
				ReadArchiveText(reader, "game.json"));
			const nlohmann::json animationManifest = nlohmann::json::parse(
				ReadArchiveText(reader, std::string(
					Export::kAnimationRuntimeManifestPath)));
			const bool prefabInResourceMap = std::any_of(
				gameManifest.at("assets").begin(),
				gameManifest.at("assets").end(),
				[&prefabGuid](const nlohmann::json& asset) {
					return asset.value("guid", std::string{}) == prefabGuid;
				});
			const bool prefabInDependencyManifest = std::any_of(
				animationManifest.at("assets").begin(),
				animationManifest.at("assets").end(),
				[&prefabGuid](const nlohmann::json& asset) {
					return asset.value("guid", std::string{}) == prefabGuid
						&& asset.value("assetType", 0U)
							== Export::kUnknownAnimationExportAssetType
						&& asset.value("generatedPackageAsset", false);
				});

			context.Expect(
				!prefabInResourceMap,
				"game.json assets must remain a typed ResourceManager GUID table.");
			context.Expect(
				prefabInDependencyManifest
					&& !reader.ReadAll(
						"models/generated/headless.teprefab").empty(),
				"The prefab must remain in the source-free package dependency "
				"manifest and the same TEPAK.");
			context.Expect(
				Export::AnimationPackValidator::ValidateArchive(
					result.packageResult.outputPath).accepted,
				"Deep animation package validation must accept the intentionally "
				"untyped prefab document record.");
		});

		registry.Add("Editor.HeadlessExportPipeline.GeneratedMaterialDiscoveryIsIdempotent", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			const std::string materialGuid =
				AddGeneratedMaterialPackage(request.projectRoot);
			context.Expect(
				!materialGuid.empty(),
				"The generated-material fixture must publish a valid import sidecar.");
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request);
			context.Expect(
				result.success,
				"Package metadata and animation closure should coalesce the same "
				"canonical material and bytes: " + result.message);
			if(!result.success)
			{
				return;
			}

			const tepak::PackReader reader =
				tepak::PackReader::Open(result.packageResult.outputPath);
			const nlohmann::json gameManifest = nlohmann::json::parse(
				ReadArchiveText(reader, "game.json"));
			const std::vector<nlohmann::json> matchingAssets =
				[&gameManifest, &materialGuid]() {
					std::vector<nlohmann::json> matches;
					for(const nlohmann::json& asset : gameManifest.at("assets"))
					{
						if(asset.value("guid", std::string{}) == materialGuid)
						{
							matches.push_back(asset);
						}
					}
					return matches;
				}();

			context.ExpectEqual(
				1U,
				matchingAssets.size(),
				"One generated material GUID must produce exactly one ResourceManager record.");
			if(matchingAssets.size() == 1U)
			{
				context.Expect(
					matchingAssets.front().value("type", 0U)
						== static_cast<std::uint32_t>(AssetType::Material),
					"The coalesced GUID must retain its stable Material type.");
				context.Expect(
					!reader.ReadAll(
						"models/generated/headlessmaterial.tmat").empty(),
					"The coalesced record must resolve the same canonical TEPAK entry.");
			}
		});

		registry.Add("Editor.HeadlessExportPipeline.GeneratedMaterialGuidConflictFailsClosed", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			context.Expect(
				!AddGeneratedMaterialPackage(
					request.projectRoot,
					"Models/Generated/HeadlessMaterial.tmat",
					"Models/Generated/DifferentMaterial.tmat").empty(),
				"The conflicting generated-material fixture must publish a valid sidecar.");
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request);
			context.Expect(
				!result.success
					&& result.failureCategory
						== ExportFailureCategory::AssetStagingFailed
					&& result.errorMessage.find(
						"different runtime type or canonical path")
						!= std::string::npos,
				"A reused material GUID must not coalesce distinct canonical assets.");
		});

		registry.Add("Editor.HeadlessExportPipeline.UnknownGeneratedDocumentFailsClosed", [](TestContext& context) {
			TempHeadlessPipelineDirectory tempDirectory;
			ExportRequest request = MakeContentOnlyRequest(tempDirectory.Root());
			context.Expect(
				!AddGeneratedPrefabPackage(
					request.projectRoot,
					"Models/Generated/Headless.unsupported").empty(),
				"The unknown generated-document fixture must publish a valid sidecar.");
			ExportPipeline pipeline;

			const ExportResult result = pipeline.Execute(request);
			context.Expect(
				!result.success
					&& result.failureCategory
						== ExportFailureCategory::AssetStagingFailed
					&& result.errorMessage.find("no stable runtime AssetType")
						!= std::string::npos,
				"Only a validated .teprefab document may stay outside the typed "
				"ResourceManager asset map.");
		});
	}
}
