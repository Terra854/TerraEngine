// TerrainCreationImportTests.cpp -- Terrain source validation and atomic creation checks.

#include "TestHarness.h"

#include "Editor/AssetCompilers/TerrainCompiler.h"
#include "Editor/TerraEngine-Editor/Assets/AssetDatabase.h"
#include "Editor/TerraEngine-Editor/Terrain/TerrainCreationService.h"

#include <chrono>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace TerraEngine::EditorTests
{
	void RegisterTerrainCreationImportTests(TestRegistry& registry)
	{
		registry.Add("Editor.TerrainCreation.ProjectBrowserExposesAtomicFlatCreation", [](TestContext& context) {
			const auto browserPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/ProjectBrowserWindow.cpp";
			std::ifstream browserSource(browserPath);
			const std::string source(
				(std::istreambuf_iterator<char>(browserSource)),
				std::istreambuf_iterator<char>());
			context.Expect(source.find("Terrain (.teterrain)") != std::string::npos
				&& source.find("CreateProjectTerrainCreationService()") != std::string::npos
				&& source.find("service.CreateOrImport(request, targetPath)") != std::string::npos,
				"Project Browser must expose terrain creation through the checked atomic service instead of writing an empty descriptor.");
			context.Expect(source.find("IsTerrainPrivateDataDirectory") != std::string::npos
				&& source.find("[TER]") != std::string::npos,
				"Project Browser must present one terrain parent tile while hiding its implementation-owned private data directory.");
			context.Expect(source.find("importer == \"TerrainImporter\") payloadType = \"ASSET_TERRAIN\"") != std::string::npos,
				"Project Browser must publish registered terrain parents with the dedicated terrain drag payload.");

			const auto inspectorPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp";
			std::ifstream inspectorSourceFile(inspectorPath);
			const std::string inspectorSource(
				(std::istreambuf_iterator<char>(inspectorSourceFile)),
				std::istreambuf_iterator<char>());
			const std::size_t queuedAssignment = inspectorSource.find(
				"SceneMutationQueue::Instance().EnqueueTransaction(builder.Build())");
			const std::size_t requestedProxyRestore = inspectorSource.find(
				"m_Owner->RequestTerrainProxyRestore()", queuedAssignment);
			context.Expect(inspectorSource.find("AcceptDragDropPayload(\"ASSET_TERRAIN\")") != std::string::npos
				&& inspectorSource.find("droppedMetadata->importerName != \"TerrainImporter\"") != std::string::npos
				&& inspectorSource.find("UpdateComponentProperty(objectId, \"TerrainComponent\", \"terrainAssetId\"") != std::string::npos
				&& queuedAssignment != std::string::npos
				&& requestedProxyRestore != std::string::npos
				&& requestedProxyRestore > queuedAssignment,
				"Terrain Inspector drops must queue an undoable terrainAssetId transaction and arm proxy restoration for the protected post-flush boundary.");

			const auto projectRoot = std::filesystem::temp_directory_path()
				/ ("TerrainProjectBrowser-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const auto destination = projectRoot / "Assets" / "Landscapes" / "New Terrain.teterrain";
			std::error_code ignored;
			std::filesystem::remove_all(projectRoot, ignored);
			std::filesystem::create_directories(destination.parent_path(), ignored);
			AssetDatabase::Instance().Shutdown();
			AssetDatabase::Instance().Initialize(projectRoot);

			TerraEngine::AssetPipeline::TerrainImportRequest request;
			request.assetId = TerraEngine::GenerateUUID();
			const auto created = TerraEngine::Editor::Terrain::CreateProjectTerrainCreationService()
				.CreateOrImport(request, destination);
			const auto registered = AssetDatabase::Instance().GetAssets().find(
				std::filesystem::absolute(destination));
			context.Expect(created.succeeded && registered != AssetDatabase::Instance().GetAssets().end()
				&& registered->second.guid == request.assetId
				&& registered->second.importerName == "TerrainImporter",
				"The browser creation path must register the durable terrain UUID through the live project Asset Database.");

			AssetDatabase::Instance().Shutdown();
			std::filesystem::remove_all(projectRoot, ignored);
		});

		registry.Add("Editor.TerrainCreation.SceneInstanceUsesTerrainComponentOnly", [](TestContext& context) {
			const auto terrainWindowPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/TerrainWindow.cpp";
			std::ifstream terrainWindowFile(terrainWindowPath);
			const std::string terrainWindowSource(
				(std::istreambuf_iterator<char>(terrainWindowFile)),
				std::istreambuf_iterator<char>());
			const std::size_t proxyPublication = terrainWindowSource.find(
				"TerrainRenderProxyRegistry::Instance().Publish");
			const std::size_t existingInstanceGate = terrainWindowSource.find(
				"if(terrainRenderProxyInstanceQueued) return", proxyPublication);
			context.Expect(proxyPublication != std::string::npos
				&& existingInstanceGate != std::string::npos
				&& existingInstanceGate > proxyPublication
				&& terrainWindowSource.find(".AddComponent(local, std::string(TerrainComponent::kComponentName))") != std::string::npos
				&& terrainWindowSource.find(".AddComponent(local, std::string(MeshRenderComponent::kComponentName))") == std::string::npos,
				"Terrain creation and recovery must republish derived render resources before gating scene creation, while adding only TerrainComponent to the scene object.");
			context.Expect(terrainWindowSource.find("DrawMaterialControls();") != std::string::npos
				&& terrainWindowSource.find("ImGuiColorEditFlags_Uint8") != std::string::npos
				&& terrainWindowSource.find("ImGuiColorEditFlags_DisplayRGB") != std::string::npos,
				"Terrain material settings must expose the normalized base color through an explicit 0-255 RGB editor instead of an unbounded generic vector control.");
			context.Expect(terrainWindowSource.find(
				"layerMaterial->header.baseColorFactor[0] * settings.baseColor.r") != std::string::npos,
				"Healthy painted layers must include the TerrainComponent tint exactly once in their runtime color factor.");
			const std::size_t runtimeBindingPublication = terrainWindowSource.find(
				"TerrainMaterialRuntimeRegistry::Instance().Publish");
			const std::size_t generatedMaterialInvalidation = terrainWindowSource.find(
				"InvalidateGeneratedMaterial(terrainRenderProxyMaterialId)", runtimeBindingPublication);
			context.Expect(runtimeBindingPublication != std::string::npos
				&& generatedMaterialInvalidation != std::string::npos
				&& generatedMaterialInvalidation > runtimeBindingPublication,
				"Terrain layer changes must retire the stable generated GPU material only after publishing the complete replacement runtime binding.");
			context.Expect(terrainWindowSource.find("void TerrainWindow::RefreshMaterialAsset") != std::string::npos
				&& terrainWindowSource.find("SynchronizeTerrainMaterial(true)") != std::string::npos
				&& terrainWindowSource.find("if(controlTexturesChanged)") != std::string::npos,
				"Material edits must immediately republish terrain layer bindings while preserving unchanged paint control textures.");
			context.Expect(terrainWindowSource.find("Clear Material") != std::string::npos
				&& terrainWindowSource.find("the only enabled layer already covers 100% of the terrain") != std::string::npos,
				"Terrain painting must explain the fully weighted base-layer case and let artists restore its flat Base Color material.");

			const auto inspectorPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp";
			std::ifstream inspectorFile(inspectorPath);
			const std::string inspectorSource(
				(std::istreambuf_iterator<char>(inspectorFile)),
				std::istreambuf_iterator<char>());
			context.Expect(inspectorSource.find("propertyName != \"baseColor\"") != std::string::npos
				&& inspectorSource.find("propertyName != \"metallic\"") != std::string::npos
				&& inspectorSource.find("propertyName != \"roughness\"") != std::string::npos,
				"Terrain material properties must not also render through generic unbounded reflected controls.");
			context.Expect(inspectorSource.find("ReloadEditedMaterial(it->second.guid, path)") != std::string::npos
				&& inspectorSource.find("RefreshMaterialAsset(it->second.guid)") != std::string::npos,
				"Material Inspector edits must update the live material cache and any referencing terrain without waiting for the asset watcher.");

			const auto editorSubsystemPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp";
			std::ifstream editorSubsystemFile(editorSubsystemPath);
			const std::string editorSubsystemSource(
				(std::istreambuf_iterator<char>(editorSubsystemFile)),
				std::istreambuf_iterator<char>());
			context.Expect(editorSubsystemSource.find(
				"TerrainRenderProxyRegistry::Instance()") != std::string::npos
				&& editorSubsystemSource.find("terrainProxyRestorePending = true") != std::string::npos
				&& editorSubsystemSource.find("QueueAutomaticTerrainCollision") != std::string::npos
				&& editorSubsystemSource.find("RemoveTerrainCollisions(removedAsset)") != std::string::npos
				&& editorSubsystemSource.find(".Remove(removedAsset)") != std::string::npos,
				"The editor must restore missing terrain derivatives and retire render/collision state when the last component disappears.");
			const std::size_t projectClose = editorSubsystemSource.find(
				"void MainEditorSubsystem::CloseCurrentProject()");
			const std::size_t releaseProjectTerrain = editorSubsystemSource.find(
				"terrainTools->ReleaseProjectTerrainState()", projectClose);
			const std::size_t clearResourceCaches = editorSubsystemSource.find(
				"ResourceManager::Instance().ClearCaches()", projectClose);
			context.Expect(projectClose != std::string::npos
				&& releaseProjectTerrain != std::string::npos
				&& clearResourceCaches != std::string::npos
				&& releaseProjectTerrain < clearResourceCaches,
				"Project close must discard cached terrain resource identities before cache clearing so reopening the same terrain rebuilds a valid proxy.");

			const auto rendererPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Renderer/RenderSystem.cpp";
			std::ifstream rendererFile(rendererPath);
			const std::string rendererSource(
				(std::istreambuf_iterator<char>(rendererFile)),
				std::istreambuf_iterator<char>());
			context.Expect(rendererSource.find("forEachObjectWithComponents<Transform, TerrainComponent>") != std::string::npos
				&& rendererSource.find("Resolve(terrain->terrainAssetId)") != std::string::npos
				&& rendererSource.find("kResourceTerrainRenderProxies") != std::string::npos,
				"RenderSystem must collect TerrainComponent instances through the declared terrain proxy resource contract.");

			const auto runtimeSystemPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Terrain/TerrainDerivedSystem.cpp";
			std::ifstream runtimeSystemFile(runtimeSystemPath);
			const std::string runtimeSystemSource(
				(std::istreambuf_iterator<char>(runtimeSystemFile)),
				std::istreambuf_iterator<char>());
			const auto terrainMaterialPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Terrain/TerrainMaterial.cpp";
			std::ifstream terrainMaterialFile(terrainMaterialPath);
			const std::string terrainMaterialSource(
				(std::istreambuf_iterator<char>(terrainMaterialFile)),
				std::istreambuf_iterator<char>());
			const auto exportPipelinePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Editor/Export/ExportPipeline.cpp";
			std::ifstream exportPipelineFile(exportPipelinePath);
			const std::string exportPipelineSource(
				(std::istreambuf_iterator<char>(exportPipelineFile)),
				std::istreambuf_iterator<char>());
			context.Expect(runtimeSystemSource.find("BuildMountedTerrainRuntime") != std::string::npos
				&& runtimeSystemSource.find("TerrainRenderProxyRegistry::Instance().Publish") != std::string::npos
				&& runtimeSystemSource.find("CommitTerrainCollisionReplacement") != std::string::npos
				&& runtimeSystemSource.find("CreateTerrainMaterial") != std::string::npos
				&& terrainWindowSource.find("CreateTerrainMaterial") != std::string::npos
				&& terrainMaterialSource.find("ShadingModel::MetallicRoughness") != std::string::npos
				&& exportPipelineSource.find("importer == \"TerrainImporter\") type = AssetType::Terrain") != std::string::npos,
				"Editor and standalone terrain proxies must publish lit render materials and collision products without serialized helper components.");

			// The editor executable publishes terrain proxies while RenderSystem consumes
			// them from the engine DLL. Keeping Instance() out of the header guarantees
			// both modules reach the same registry on Windows instead of creating one
			// function-local singleton per binary module.
			const auto proxyHeaderPath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Renderer/Terrain/TerrainRenderProxyRegistry.h";
			std::ifstream proxyHeaderFile(proxyHeaderPath);
			const std::string proxyHeaderSource(
				(std::istreambuf_iterator<char>(proxyHeaderFile)),
				std::istreambuf_iterator<char>());
			const auto proxySourcePath = std::filesystem::path(TERRAENGINE_SOURCE_DIR)
				/ "Engine/Renderer/Terrain/TerrainRenderProxyRegistry.cpp";
			std::ifstream proxySourceFile(proxySourcePath);
			const std::string proxySource(
				(std::istreambuf_iterator<char>(proxySourceFile)),
				std::istreambuf_iterator<char>());
			context.Expect(proxyHeaderSource.find("class TERRAENGINE_API TerrainRenderProxyRegistry final") != std::string::npos
				&& proxyHeaderSource.find("static TerrainRenderProxyRegistry& Instance();") != std::string::npos
				&& proxyHeaderSource.find("static TerrainRenderProxyRegistry registry;") == std::string::npos
				&& proxySource.find("TerrainRenderProxyRegistry::Instance()") != std::string::npos
				&& proxySource.find("static TerrainRenderProxyRegistry registry;") != std::string::npos,
				"Terrain proxy singleton storage must live in the exported engine DLL so editor publication and renderer consumption share one registry.");
		});

		registry.Add("Editor.TerrainCreation.FlatCreatesPrivateGeneration", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainFlat-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const auto destination = root / "Flat.teterrain";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.assetId = TerraEngine::GenerateUUID();
			TerraEngine::Editor::Terrain::TerrainCreationService service;
			const auto result = service.CreateOrImport(request, destination);
			context.Expect(result.succeeded && result.heightfield.heightsMm.size() == 256U * 256U
				&& std::filesystem::exists(destination) && std::filesystem::exists(destination.string() + ".data/CURRENT")
				&& std::filesystem::exists(destination.string() + ".data/roots/1.json"),
				"Flat creation must publish one parent asset plus a complete private CURRENT/root/blob generation.");
			const auto reopened = service.OpenExisting(destination);
			context.Expect(reopened.succeeded && reopened.assetId == request.assetId
				&& reopened.heightfield.heightsMm == result.heightfield.heightsMm,
				"A scene reload must recover the exact persisted origin area used to rebuild its render proxy.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.Raw16ValidatesEndianByteCountAndCancellation", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainRaw-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root); const auto raw = root / "source.raw";
			std::ofstream output(raw, std::ios::binary); output.put('\x34'); output.put('\x12'); output.put('\x78'); output.put('\x56'); output.close();
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Raw16LittleEndian; request.sourcePath = raw; request.rawWidth = 2U; request.rawHeight = 1U; request.assetId = TerraEngine::GenerateUUID();
			const auto decoded = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(decoded.succeeded && decoded.heightfield.heightsMm[0] < decoded.heightfield.heightsMm[1], "RAW16 must decode declared little-endian samples without resampling.");
			TerraEngine::Editor::Terrain::TerrainCreationService service;
			const auto created = service.CreateOrImport(request, root / "TwoByOne.teterrain");
			context.Expect(created.succeeded && std::filesystem::exists(root / "TwoByOne.teterrain.data/CURRENT"),
				"A non-256 RAW import must tile and publish a complete private terrain generation.");
			request.rawWidth = 3U; const auto rejected = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(!rejected.succeeded, "A RAW16 byte-count mismatch must reject before asset publication.");
			request.rawWidth = 2U; const auto cancelled = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request, [](float, std::string_view, std::string_view) { return false; });
			context.Expect(cancelled.cancelled, "Cancellation must stop import before a candidate can be registered.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.Png16PreservesPrecisionAndRejectsInvalidDepth", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainPng-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			std::filesystem::create_directories(root);
			const auto png16 = root / "height16.png"; const auto png8 = root / "height8.png"; const auto corrupt = root / "corrupt.png";
			const std::array<std::uint8_t, 70> precise = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x10,0x00,0x00,0x00,0x00,0x6A,0xEE,0x47,0x16,0x00,0x00,0x00,0x0B,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x10,0x32,0x01,0x00,0x00,0x5B,0x00,0x47,0x96,0xFB,0x1B,0x65,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
			const std::array<std::uint8_t, 67> lowPrecision = {0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x00,0x00,0x00,0x00,0x3A,0x7E,0x9B,0x55,0x00,0x00,0x00,0x0A,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x68,0x00,0x00,0x00,0x82,0x00,0x81,0x77,0xCD,0x72,0xB6,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82};
			std::ofstream(png16, std::ios::binary).write(reinterpret_cast<const char*>(precise.data()), precise.size());
			std::ofstream(png8, std::ios::binary).write(reinterpret_cast<const char*>(lowPrecision.data()), lowPrecision.size());
			std::ofstream(corrupt, std::ios::binary) << "not a png";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.sourceFormat = TerraEngine::AssetPipeline::TerrainSourceFormat::Png16; request.sourcePath = png16; request.assetId = TerraEngine::GenerateUUID(); request.heightScaleMetres = 10.0F; request.heightOffsetMetres = -1.0F;
			const auto decoded = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(decoded.succeeded && decoded.heightfield.heightsMm.size() == 1U && decoded.heightfield.heightsMm[0] == -289,
				"A true PNG16 sample must retain its 0x1234 precision while declared scale and origin are applied.");
			request.sourcePath = png8; const auto rejected8 = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			request.sourcePath = corrupt; const auto rejectedCorrupt = TerraEngine::AssetPipeline::CompileTerrainHeightfield(request);
			context.Expect(!rejected8.succeeded && !rejectedCorrupt.succeeded, "8-bit and corrupt PNG sources must be rejected before terrain publication.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});

		registry.Add("Editor.TerrainCreation.RegistrationFailureRollsBackParent", [](TestContext& context) {
			const auto root = std::filesystem::temp_directory_path() / ("TerrainRollback-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
			const auto destination = root / "Rejected.teterrain";
			TerraEngine::AssetPipeline::TerrainImportRequest request; request.assetId = TerraEngine::GenerateUUID();
			TerraEngine::Editor::Terrain::TerrainCreationService service([](const std::filesystem::path&, std::string& error) { error = "Test registration rejection."; return false; });
			const auto result = service.CreateOrImport(request, destination);
			context.Expect(!result.succeeded && !std::filesystem::exists(destination) && !std::filesystem::exists(destination.string() + ".data"),
				"Failed registration must leave neither a public parent nor private data behind.");
			std::error_code ignored; std::filesystem::remove_all(root, ignored);
		});
	}
}
