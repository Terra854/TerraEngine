#include "TestHarness.h"

#include "Resource/ResourceManager.h"
#include "Runtime/ContentMount/ContentMount.h"
#include "Runtime/ExportStartupCoordinator.h"
#include "Scene/SceneManager.h"
#include "TEPAK/PackWriter.h"
#include "Utilities/UUID.h"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		class ExportRuntimeWorkspace {
		public:
			ExportRuntimeWorkspace()
				: root_(std::filesystem::temp_directory_path() / "TerraEngineEngineTests" / "ExportRuntimeSmoke") {
				std::filesystem::remove_all(root_);
				std::filesystem::create_directories(ContentRoot());
				std::filesystem::create_directories(OutputRoot());
			}

			~ExportRuntimeWorkspace() {
				std::error_code ignoredError;
				std::filesystem::remove_all(root_, ignoredError);
				SceneManager::Instance().UnloadAll();
				ResourceManager::Instance().Shutdown();
				ResourceManager::Instance().ConfigureExportArchive({});
			}

			[[nodiscard]] std::filesystem::path ContentRoot() const {
				return root_ / "Content";
			}

			[[nodiscard]] std::filesystem::path OutputRoot() const {
				return root_ / "Output";
			}

			[[nodiscard]] std::filesystem::path PackPath() const {
				return OutputRoot() / "SmokeGame.tepak";
			}

			void WriteText(const std::filesystem::path& relativePath, const std::string& contents) const {
				const std::filesystem::path fullPath = ContentRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				if (!stream) {
					throw std::runtime_error("Failed to create export smoke fixture file: " + fullPath.string());
				}
				stream << contents;
			}

			void WriteBytes(const std::filesystem::path& relativePath, const std::vector<std::uint8_t>& contents) const {
				const std::filesystem::path fullPath = ContentRoot() / relativePath;
				std::filesystem::create_directories(fullPath.parent_path());
				std::ofstream stream(fullPath, std::ios::binary);
				if (!stream) {
					throw std::runtime_error("Failed to create export smoke fixture file: " + fullPath.string());
				}
				stream.write(reinterpret_cast<const char*>(contents.data()), static_cast<std::streamsize>(contents.size()));
			}

			void CreatePack() const {
				tepak::PackagingRequest request;
				request.inputRoot = ContentRoot();
				request.outputPackPath = PackPath();
				request.extentSizingBytes = 4096U;
				request.workerCount = 1U;
				request.enableCompression = false;
				request.enableDictionaryTraining = false;
				request.overwriteExisting = true;
				request.peakMemoryBudgetBytes = 64ULL * 1024ULL * 1024ULL;
				(void)tepak::PackWriter::Create(request);
			}

		private:
			std::filesystem::path root_;
		};

		std::string MakeGameManifest(std::string_view meshGuid, std::string_view materialGuid, std::string_view textureGuid) {
			return std::string("{\n")
				+ "\t\"name\": \"Smoke Export\",\n"
				+ "\t\"version\": \"2.5.0\",\n"
				+ "\t\"startupScene\": \"Scenes/Main.tescene\",\n"
				+ "\t\"assets\": [\n"
				+ "\t\t{ \"guid\": \"" + std::string(meshGuid) + "\", \"path\": \"Meshes/Smoke.temf\", \"type\": 0 },\n"
				+ "\t\t{ \"guid\": \"" + std::string(materialGuid) + "\", \"path\": \"Materials/Smoke.tmat\", \"type\": 2 },\n"
				+ "\t\t{ \"guid\": \"" + std::string(textureGuid) + "\", \"path\": \"Textures/Smoke.tetf\", \"type\": 1 }\n"
				+ "\t]\n"
				+ "}\n";
		}

		std::string MakeSceneDocument() {
			return "{\n\t\"name\": \"Mounted Smoke Scene\",\n\t\"objects\": []\n}\n";
		}

		void BootstrapExportRuntime(const ExportRuntimeWorkspace& workspace) {
			ResourceManager::Instance().ConfigureExportArchive(workspace.PackPath());
			ResourceManager::Instance().Initialize();
			SceneManager::Instance().SetSceneRoot(workspace.OutputRoot());
		}
	}

	void RegisterExportRuntimeSmokeTests(TestRegistry& registry) {
		registry.Add("Engine.ExportRuntime.MountsManifestSceneAndAssetsFromPack", [](TestContext& context) {
			ExportRuntimeWorkspace workspace;
			const TerraEngine::UUID meshGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID materialGuid = TerraEngine::GenerateUUID();
			const TerraEngine::UUID textureGuid = TerraEngine::GenerateUUID();
			const std::string meshGuidText = TerraEngine::ToHexString(meshGuid);
			const std::string materialGuidText = TerraEngine::ToHexString(materialGuid);
			const std::string textureGuidText = TerraEngine::ToHexString(textureGuid);
			const std::vector<std::uint8_t> payloadBytes{ 0x54U, 0x45U, 0x52U, 0x52U, 0x41U };
			const std::string materialPayload = "{\n\t\"name\": \"Smoke Material\"\n}\n";
			const std::vector<std::uint8_t> texturePayload{ 0x54U, 0x45U, 0x54U, 0x46U };
			const std::string uiPayload = "<ui />\n";
			const std::string shaderPayload = "fake sky shader bytes";
			const std::string fontPayload = "fake font bytes";

			workspace.WriteText("game.json", MakeGameManifest(meshGuidText, materialGuidText, textureGuidText));
			workspace.WriteText("Scenes/Main.tescene", MakeSceneDocument());
			workspace.WriteBytes("Data/Smoke.bytes", payloadBytes);
			workspace.WriteBytes("Meshes/Smoke.temf", {});
			workspace.WriteText("Materials/Smoke.tmat", materialPayload);
			workspace.WriteBytes("Textures/Smoke.tetf", texturePayload);
			workspace.WriteText("UI/Hud.tuiml", uiPayload);
			workspace.WriteText("Shaders/Sky/SkyBackground.mesh.tspf", shaderPayload);
			workspace.WriteText("Fonts/Roboto-Medium.ttf", fontPayload);
			workspace.CreatePack();

			BootstrapExportRuntime(workspace);
			const std::optional<std::filesystem::path> startupScene = ResourceManager::Instance().GetConfiguredStartupScene();
			const TerraEngine::UUID resolvedMesh = ResourceManager::Instance().ResolveAssetReference("Meshes/Smoke.temf", AssetType::Mesh);
			const TerraEngine::UUID resolvedMaterial = ResourceManager::Instance().ResolveAssetReference("Materials/Smoke.tmat", AssetType::Material);
			const TerraEngine::UUID resolvedTexture = ResourceManager::Instance().ResolveAssetReference("Textures/Smoke.tetf", AssetType::Texture);
			const std::vector<std::uint8_t> mountedPayload = ResourceManager::Instance().ReadFileBytes("Data/Smoke.bytes");
			const std::vector<std::uint8_t> mountedMaterialBytes = ResourceManager::Instance().ReadFileBytes("Materials/Smoke.tmat");
			const std::vector<std::uint8_t> mountedTextureBytes = ResourceManager::Instance().ReadFileBytes("Textures/Smoke.tetf");
			const std::vector<std::uint8_t> mountedUiBytes = ResourceManager::Instance().ReadFileBytes("UI/Hud.tuiml");
			const std::vector<std::uint8_t> mountedShaderBytes = ResourceManager::Instance().ReadFileBytes("Shaders/Sky/SkyBackground.mesh.tspf");
			const std::vector<std::uint8_t> mountedFontBytes = ResourceManager::Instance().ReadFileBytes("Fonts/Roboto-Medium.ttf");
			const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(*startupScene);
			const SceneDocument* scene = SceneManager::Instance().GetScene(sceneId);

			context.Expect(ResourceManager::Instance().IsExportRuntime(), "Configuring an archive should put ResourceManager into export runtime mode.");
			context.Expect(ContentMount::Instance().IsMounted(), "ResourceManager initialization should mount the export TEPAK archive.");
			context.ExpectEqual(std::string("Smoke Export"), ResourceManager::Instance().GetProductName(), "Product metadata should come from packed game.json.");
			context.ExpectEqual(std::string("2.5.0"), ResourceManager::Instance().GetVersion(), "Version metadata should come from packed game.json.");
			context.Expect(startupScene.has_value(), "The packed manifest should provide a startup scene path.");
			context.ExpectEqual(std::string("Scenes/Main.tescene"), startupScene->generic_string(), "Startup scene should preserve the manifest path for engine bootstrap.");
			context.Expect(resolvedMesh == meshGuid, "Manifest asset entries should register UUID-to-path lookups before scene loading.");
			context.Expect(resolvedMaterial == materialGuid, "Packed material assets should be addressable through the export manifest.");
			context.Expect(resolvedTexture == textureGuid, "Packed texture assets should be addressable through the export manifest.");
			context.Expect(mountedPayload == payloadBytes, "ResourceManager should read non-scene runtime bytes through the mounted archive.");
			context.ExpectEqual(materialPayload, std::string(mountedMaterialBytes.begin(), mountedMaterialBytes.end()), "Minimal export packages should keep material source bytes in the mounted archive.");
			context.Expect(mountedTextureBytes == texturePayload, "Minimal export packages should keep texture payload bytes in the mounted archive.");
			context.ExpectEqual(uiPayload, std::string(mountedUiBytes.begin(), mountedUiBytes.end()), "Minimal export packages should keep UI documents in the mounted archive.");
			context.ExpectEqual(shaderPayload, std::string(mountedShaderBytes.begin(), mountedShaderBytes.end()), "Engine-owned shader files should be read from the mounted archive.");
			context.ExpectEqual(fontPayload, std::string(mountedFontBytes.begin(), mountedFontBytes.end()), "Engine-owned font files should be read from the mounted archive.");
			context.Expect(sceneId != TerraEngine::EMPTY_UUID, "SceneManager should assign an ID when the startup scene loads from the mount.");
			context.Expect(scene != nullptr, "The mounted startup scene should be cached after loading.");
			context.ExpectEqual(std::string("Mounted Smoke Scene"), scene != nullptr ? scene->name : std::string{}, "SceneManager should parse scene JSON read from the mount.");
		});

		registry.Add("Engine.ExportRuntime.MissingManifestFailsStartupClearly", [](TestContext& context) {
			ExportRuntimeWorkspace workspace;
			workspace.WriteText("Scenes/Main.tescene", MakeSceneDocument());
			workspace.CreatePack();

			bool threwForMissingManifest = false;
			try {
				BootstrapExportRuntime(workspace);
			}
			catch (const std::exception&) {
				threwForMissingManifest = true;
			}

			context.Expect(threwForMissingManifest, "Export startup should fail when a mounted archive does not contain game.json.");
			context.Expect(!ContentMount::Instance().IsMounted(), "A failed export startup should unmount the partially opened archive.");
		});

		registry.Add("Engine.ExportRuntime.ValidationSummaryRejectsMissingStartupScene", [](TestContext& context) {
			TerraEngine::Runtime::ExportStartupCoordinatorInput input;
			input.exportRuntime = true;
			input.contentMounted = true;
			input.metadataLoaded = true;
			input.startupSceneConfigured = true;
			input.startupSceneLoaded = false;
			input.projectName = "Validation Smoke";
			input.startupScene = "Scenes/Missing.tescene";
			input.requiredSystems.push_back({ "ResourceManager", true, true, "Initialized" });

			const TerraEngine::Runtime::ExportStartupCoordinatorResult result =
				TerraEngine::Runtime::CoordinateExportStartup(input);

			context.Expect(!result.CanEnterRuntime(), "Launcher validation mode should reject missing startup scene content.");
			context.ExpectEqual(std::string("Scenes/Missing.tescene"), result.summary.startupScene, "Validation summary should preserve the failing startup scene path.");
		});
	}
}
