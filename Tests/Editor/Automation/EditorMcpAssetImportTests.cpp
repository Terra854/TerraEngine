#include "../TestHarness.h"

#include "AssetDatabase.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace TerraEngine::EditorTests {
	namespace {
		struct BlockedMcpImportGate {
			std::mutex mutex;
			std::condition_variable condition;
			bool workerEntered = false;
			bool allowCompletion = false;
		};

		class ScopedBlockedMcpImport {
		public:
			ScopedBlockedMcpImport()
				: gate(std::make_shared<BlockedMcpImportGate>())
			{
				const std::shared_ptr<BlockedMcpImportGate> sharedGate = gate;
				AssetDatabase::Instance().SetAsyncImportCompileHookForTests(
					[sharedGate](const AssetMetadata&, const std::atomic_bool& cancellationRequested) {
						std::unique_lock<std::mutex> lock(sharedGate->mutex);
						sharedGate->workerEntered = true;
						sharedGate->condition.notify_all();
						while (!sharedGate->allowCompletion && !cancellationRequested.load())
						{
							sharedGate->condition.wait_for(lock, std::chrono::milliseconds(5));
						}
						return !cancellationRequested.load();
					});
			}

			~ScopedBlockedMcpImport()
			{
				Release();
				// Assertions may leave the worker active. Drain it before removing the
				// captured gate so the process-wide AssetDatabase remains reusable.
				AssetDatabase::Instance().CancelAllImportsAndWait(false);
				AssetDatabase::Instance().SetAsyncImportCompileHookForTests({});
			}

			bool WaitForWorker() const
			{
				std::unique_lock<std::mutex> lock(gate->mutex);
				return gate->condition.wait_for(lock, std::chrono::seconds(3), [this]() {
					return gate->workerEntered;
				});
			}

			void Release() const
			{
				{
					std::lock_guard<std::mutex> lock(gate->mutex);
					gate->allowCompletion = true;
				}
				gate->condition.notify_all();
			}

		private:
			std::shared_ptr<BlockedMcpImportGate> gate;
		};

		struct TempMcpImportProject {
			explicit TempMcpImportProject(std::string name)
				: root(std::filesystem::temp_directory_path() / "TerraEngineMcpAssetImport" / std::move(name)) {
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);
			}

			~TempMcpImportProject() {
				std::error_code ignored;
				AssetDatabase::Instance().Shutdown();
				std::filesystem::remove_all(root, ignored);
			}

			std::filesystem::path root;
		};

		EditorContext MakeContext(TempMcpImportProject& project) {
			EditorContext context;
			context.projectManager = std::make_shared<ProjectManager>();
			std::string error;
			context.projectManager->CreateProject(project.root, project.root.filename().string(), ProjectTemplate::Blank, &error);
			context.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
			context.assetDatabase->Initialize(context.projectManager->GetProjectRoot());
			return context;
		}
	}

	void RegisterEditorMcpAssetImportTests(TestRegistry& registry) {
		registry.Add("Editor.MCP.AssetImport.DiscoveryAdvertisesDryRun", [](TestContext& context) {
			TerraEngine::Editor::Automation::EditorAutomationService service;
			const nlohmann::json discovery = service.BuildCapabilityList();
			const nlohmann::json& capabilities = discovery["capabilities"];
			const auto import = std::find_if(capabilities.begin(), capabilities.end(), [](const nlohmann::json& capability) {
				return capability.value("name", std::string{}) == "asset.import";
			});
			context.Expect(import != capabilities.end()
				&& (*import)["inputSchema"]["properties"].contains("dryRun")
				&& (*import)["inputSchema"]["properties"]["dryRun"].value("type", std::string{}) == "boolean",
				"MCP discovery must advertise the handler's non-mutating asset-import preflight argument.");
		});

		registry.Add("Editor.MCP.AssetImport.DryRunValidatesWithoutSideEffects", [](TestContext& context) {
			TempMcpImportProject project("DryRunNoSideEffects");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot
				/ "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path projectAsset = editorContext.assetDatabase->GetAssetRoot()
				/ "Models" / "dry-run.gltf";
			const std::filesystem::path expectedCookedOutput = project.root
				/ "Build" / "Assets" / "Models" / "dry-run.temf";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset,
				std::filesystem::copy_options::overwrite_existing);

			// A valid package source makes this a meaningful preflight. Capture all
			// mutable owners before the call so a future handler reorder cannot hide
			// metadata, worker, database, cooked-output, or durable-job side effects.
			const std::filesystem::path absoluteProjectAsset = std::filesystem::absolute(projectAsset);
			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const std::size_t jobsBefore = service.GetJobService().List(256U).size();
			const nlohmann::json result = service.HandleTool(
				"asset.import",
				{{"path", "Models/dry-run.gltf"}, {"force", true}, {"dryRun", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"mcp-import-dry-run");

			context.Expect(result.value("ok", false)
				&& result["data"].value("status", std::string{}) == "validated"
				&& result["data"].value("dryRun", false)
				&& result["data"].value("path", std::string{}) == "Models/dry-run.gltf",
				"A dry-run import must return the validated preflight contract for a valid model source.");
			context.Expect(!std::filesystem::exists(projectAsset.string() + ".meta")
				&& !std::filesystem::exists(expectedCookedOutput)
				&& !editorContext.assetDatabase->GetAssets().contains(absoluteProjectAsset)
				&& !editorContext.assetDatabase->IsAssetCompiling(absoluteProjectAsset)
				&& editorContext.assetDatabase->GetImportProgressSnapshot().empty()
				&& service.GetJobService().List(256U).size() == jobsBefore,
				"A dry-run import must not write metadata or cooked output, register an asset, queue compiler work, or create a durable job.");
		});

		registry.Add("Editor.MCP.AssetImport.ReturnsGltfPackageResult", [](TestContext& context) {
			TempMcpImportProject project("Response");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path projectAsset = editorContext.assetDatabase->GetAssetRoot() / "Models" / "one-material.gltf";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset, std::filesystem::copy_options::overwrite_existing);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json result = service.HandleTool("asset.import", {{"path", "Models/one-material.gltf"}, {"force", true}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-1");
			context.Expect(result.value("ok", false), "MCP asset import should succeed for a project glTF.");
			context.Expect(result["data"].contains("importResult"), "MCP response should include package import details.");
			context.Expect(!result["data"]["importResult"]["generatedMeshes"].empty(), "MCP response should include generated mesh ids.");
			context.Expect(!result["data"]["importResult"]["generatedMaterials"].empty(), "MCP response should include generated material ids.");
		});

		registry.Add("Editor.MCP.AssetImport.ReturnsFbxPackageResult", [](TestContext& context) {
			TempMcpImportProject project("FbxResponse");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path sourceFixture = sourceRoot / "External/assimp/test/models/FBX/box.fbx";
			const std::filesystem::path projectAsset = editorContext.assetDatabase->GetAssetRoot() / "Models" / "box.fbx";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(sourceFixture, projectAsset, std::filesystem::copy_options::overwrite_existing);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			const nlohmann::json result = service.HandleTool("asset.import", {{"path", "Models/box.fbx"}, {"force", true}}, TerraEngine::Editor::Automation::WriteMode::WritesEnabled, "mcp-import-fbx-1");
			context.Expect(result.value("ok", false), "MCP asset import should succeed for a project FBX.");
			context.Expect(result["data"].contains("importResult"), "MCP FBX response should include package import details.");
			context.ExpectEqual(std::string("Fbx"), result["data"]["importResult"]["package"].value("packageKind", std::string()), "MCP FBX response should report package kind.");
			context.Expect(!result["data"]["importResult"]["generatedMeshes"].empty(), "MCP FBX response should include generated mesh ids.");
		});

		registry.Add("Editor.MCP.AssetImport.WaitsForActiveSamePathImport", [](TestContext& context) {
			TempMcpImportProject project("SamePathReadAfterWrite");
			EditorContext editorContext = MakeContext(project);
			const std::filesystem::path sourceRoot = std::filesystem::path(TERRAENGINE_SOURCE_DIR);
			const std::filesystem::path oneMaterial =
				sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/one-material.gltf";
			const std::filesystem::path multiMaterial =
				sourceRoot / "Tests/Editor/AssetPipeline/Fixtures/Gltf/multi-material.gltf";
			const std::filesystem::path projectAsset =
				editorContext.assetDatabase->GetAssetRoot() / "Models" / "current-source.gltf";
			std::filesystem::create_directories(projectAsset.parent_path());
			std::filesystem::copy_file(
				oneMaterial,
				projectAsset,
				std::filesystem::copy_options::overwrite_existing);

			// Seed a durable prior sidecar, then replace the source while a watcher-
			// style worker is held active. The stale sidecar contains one material;
			// the current source and required MCP response contain two.
			editorContext.assetDatabase->ImportAsset(projectAsset, true);
			std::filesystem::copy_file(
				multiMaterial,
				projectAsset,
				std::filesystem::copy_options::overwrite_existing);

			ScopedBlockedMcpImport blockedImport;
			editorContext.assetDatabase->ImportAssetWithProgress(projectAsset, true);
			context.Expect(blockedImport.WaitForWorker(),
				"Read-after-write regression requires an active async import for the requested glTF path.");
			// Coalesce a second same-path watcher request. Explicit import must remove
			// this duplicate without touching any unrelated jobs.
			editorContext.assetDatabase->ImportAssetWithProgress(projectAsset, true);

			TerraEngine::Editor::Automation::EditorAutomationService service;
			service.SetEditorContext(&editorContext);
			std::atomic_bool responseReturned = false;
			std::atomic_bool responseReturnedBeforeWorkerRelease = false;
			std::future<void> releaseWorker = std::async(std::launch::async, [&]() {
				std::this_thread::sleep_for(std::chrono::milliseconds(75));
				responseReturnedBeforeWorkerRelease.store(responseReturned.load());
				blockedImport.Release();
			});

			const nlohmann::json result = service.HandleTool(
				"asset.import",
				{{"path", "Models/current-source.gltf"}, {"force", true}},
				TerraEngine::Editor::Automation::WriteMode::WritesEnabled,
				"mcp-import-same-path-1");
			responseReturned.store(true);
			releaseWorker.get();

			context.Expect(!responseReturnedBeforeWorkerRelease.load(),
				"Explicit MCP import must wait for the active same-path worker instead of returning the stale sidecar.");
			context.Expect(result.value("ok", false),
				"Explicit MCP import should complete after finalizing the same-path worker.");
			context.Expect(result["data"].contains("importResult")
				&& result["data"]["importResult"].is_object(),
				"Completed explicit import should return its current package result.");
			context.ExpectEqual(
				static_cast<std::size_t>(2U),
				result["data"]["importResult"]["generatedMaterials"].size(),
				"MCP response must describe the current two-material source, not the prior one-material sidecar.");
			context.Expect(!editorContext.assetDatabase->IsAssetCompiling(projectAsset),
				"The requested path should have no active worker after its explicit synchronous reimport.");
		});
	}
}
