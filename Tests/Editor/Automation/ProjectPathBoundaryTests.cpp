// Exercise production automation entry points. Every outside-project target is
// still inside this fixture's private root, so a failing boundary stays safe.
#include "../TestHarness.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "AssetDatabase.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <algorithm>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#undef min
#undef max
#endif

namespace TerraEngine::EditorTests {
	namespace {
		using Editor::Automation::EditorAutomationService;
		using Editor::Automation::WriteMode;
		struct BoundaryWorkspace {
			std::filesystem::path root;
			std::filesystem::path previousSceneRoot = SceneManager::Instance().GetSceneRoot();
			EditorContext editor;
			EditorAutomationService service;
			BoundaryWorkspace() {
				// Exclusive creation prevents concurrent test processes sharing cleanup.
				const auto token = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
				for(unsigned i = 0;; ++i) {
					root = std::filesystem::temp_directory_path() / ("TerraPathBoundary-" + token + "-" + std::to_string(i));
					if(std::filesystem::create_directory(root)) break;
				}
				AssetDatabase::Instance().Shutdown();
				editor.projectManager = std::make_shared<ProjectManager>(); std::string error;
				if(!editor.projectManager->CreateProject(root / "Project", "Boundary", ProjectTemplate::Blank, &error)) throw TestFailure(error);
				editor.assetDatabase = std::shared_ptr<AssetDatabase>(&AssetDatabase::Instance(), [](AssetDatabase*) {});
				editor.assetDatabase->Initialize(Project()); editor.assetDatabase->PauseWatcherForTests();
				service.SetEditorContext(&editor);
			}
			~BoundaryWorkspace() {
				UndoRedoSystem::Instance().Clear(); ObjectManager::instance().clear(); SceneManager::Instance().UnloadAll();
				SceneManager::Instance().SetSceneRoot(previousSceneRoot); AssetDatabase::Instance().Shutdown();
				std::error_code ignored; std::filesystem::remove_all(root, ignored);
			}
			std::filesystem::path Project() const { return editor.projectManager->GetProjectRoot(); }
			static void Write(const std::filesystem::path& path, std::string_view bytes) {
				std::filesystem::create_directories(path.parent_path()); std::ofstream(path, std::ios::binary | std::ios::trunc) << bytes;
			}
			static std::string Read(const std::filesystem::path& path) {
				std::ifstream input(path, std::ios::binary); return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
			}
		};
		// A redirect is mandatory evidence, not an optional platform capability.
		// Both ends are computed from this fixture, and cleanup unlinks only the
		// entry we exclusively created before the workspace removes its own tree.
		class FixtureDirectoryRedirect final {
		public:
			explicit FixtureDirectoryRedirect(const BoundaryWorkspace& workspace)
				: FixtureDirectoryRedirect(workspace.Project() / "Assets/redirect", workspace.root / "Outside") {}
			FixtureDirectoryRedirect(const std::filesystem::path& link, const std::filesystem::path& target)
				: link_(link) {
				if(std::filesystem::symlink_status(link_).type() != std::filesystem::file_type::not_found)
					throw TestFailure("Redirect entry must be absent before exclusive creation.");
#ifdef _WIN32
				if(CreateSymbolicLinkW(link_.c_str(), target.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
					owned_ = true;
				} else {
					// NTFS directory junctions need no symbolic-link privilege. Match the
					// native reparse buffer used by ContentArchiveSafetyTests.
					if(!CreateDirectoryW(link_.c_str(), nullptr)) throw TestFailure("Cannot exclusively create fixture junction directory.");
					owned_ = true;
					const HANDLE handle = CreateFileW(link_.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
						FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
					bool success = false;
					if(handle != INVALID_HANDLE_VALUE) {
						struct JunctionData { DWORD tag; WORD length, reserved, subOffset, subLength, printOffset, printLength; wchar_t paths[2048]; } data{};
						const auto substitute = L"\\??\\" + std::filesystem::absolute(target).native();
						const auto print = target.native();
						if(substitute.size() + print.size() + 2U < 2048U) {
							data.tag = IO_REPARSE_TAG_MOUNT_POINT;
							data.subLength = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
							data.printOffset = data.subLength + sizeof(wchar_t);
							data.printLength = static_cast<WORD>(print.size() * sizeof(wchar_t));
							std::copy(substitute.begin(), substitute.end(), data.paths);
							std::copy(print.begin(), print.end(), data.paths + substitute.size() + 1U);
							data.length = static_cast<WORD>(8U + (substitute.size() + print.size() + 2U) * sizeof(wchar_t));
							DWORD returned = 0;
							success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, 8U + data.length, nullptr, 0, &returned, nullptr) != FALSE;
						}
						CloseHandle(handle);
					}
					if(!success) { Unlink(); throw TestFailure("Mandatory fixture junction creation failed."); }
				}
#else
				std::error_code creationError; std::filesystem::create_directory_symlink(target, link_, creationError);
				if(creationError) throw TestFailure("Mandatory fixture directory redirect creation failed: " + creationError.message());
				owned_ = true;
#endif
				std::error_code error;
				const auto actual = std::filesystem::canonical(link_, error);
				std::error_code targetError; const auto expected = std::filesystem::canonical(target, targetError);
				if(error || targetError || actual != expected) { Unlink(); throw TestFailure("Redirect must canonically resolve to the owned fixture target."); }
			}
			FixtureDirectoryRedirect(const FixtureDirectoryRedirect&) = delete;
			FixtureDirectoryRedirect& operator=(const FixtureDirectoryRedirect&) = delete;
			~FixtureDirectoryRedirect() { Unlink(); }
		private:
			void Unlink() noexcept {
				if(!owned_) return;
#ifdef _WIN32
				// RemoveDirectory removes the reparse entry without walking its target.
				RemoveDirectoryW(link_.c_str());
#else
				std::error_code ignored; std::filesystem::remove(link_, ignored);
#endif
				owned_ = false;
			}
			std::filesystem::path link_;
			bool owned_{};
		};
		void ExpectValidation(TestContext& context, const nlohmann::json& response) {
			context.Expect(!response.value("ok", true) && response.contains("error") && response["error"].value("category", "") == "validation",
				"Outside-project input must fail validation before reading or importing it. Response: " + response.dump());
		}
	}
	void RegisterProjectPathBoundaryTests(TestRegistry& registry) {
		registry.Add("Editor.ProjectPathBoundary.AnimationImportRetryReplaysAcrossServerGeneration", [](TestContext& context) {
			BoundaryWorkspace w;
			BoundaryWorkspace::Write(w.Project() / "Assets/Retry.gltf", R"({"asset":{"version":"2.0"},"scenes":[{}],"scene":0})");
			const nlohmann::json arguments = {{"sourcePath", "Assets/Retry.gltf"}, {"idempotencyKey", "import-generation-retry"}};
			const auto first = w.service.HandleTool("animation.asset.import", arguments, WriteMode::WritesEnabled, "import-first");
			context.Expect(first.value("ok", false), "The public import must admit a real disposable source: " + first.dump());
			const auto retry = w.service.HandleTool("animation.asset.import", arguments, WriteMode::WritesEnabled, "import-retry");
			context.Expect(retry.value("ok", false) && retry["data"].value("idempotencyDisposition", "") == "replay"
				&& retry["data"].value("jobId", "") == first["data"].value("jobId", "")
				&& retry["data"].at("generation") == first["data"].at("generation"),
				"An identical retry must reuse the original job and receipt after server generation advances: " + retry.dump());
		});
		registry.Add("Editor.ProjectPathBoundary.AssetRootRedirectKeepsCatalogIdentity", [](TestContext& context) {
			BoundaryWorkspace w; auto& database = *w.editor.assetDatabase;
			database.Shutdown(); std::filesystem::create_directories(w.Project() / "Assets/Target");
			FixtureDirectoryRedirect rootAlias(w.root / "ProjectAlias", w.Project());
			struct DrainAliasedDatabase { AssetDatabase& database; ~DrainAliasedDatabase() { database.Shutdown(); } } drain{database};
			const auto aliasAssets = w.root / "ProjectAlias/Assets";
			database.Initialize(w.root / "ProjectAlias"); database.PauseWatcherForTests();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			for(;;) {
				database.Update(); const auto state = database.GetDiscoveryDiagnostics();
				if(!state.pending && !state.scanning && state.pendingImports == 0U) break;
				context.Expect(std::chrono::steady_clock::now() < deadline, "Aliased root discovery must settle before the observed import.");
				std::this_thread::yield();
			}
			context.Expect(database.GetAssetRoot() == aliasAssets, "The fixture must retain a genuinely noncanonical catalog root spelling.");
			FixtureDirectoryRedirect inRoot(aliasAssets / "in-root", w.Project() / "Assets/Target");
			const auto source = aliasAssets / "Target/fresh.txt";
			BoundaryWorkspace::Write(source, "actual source through an in-root alias");
			const auto imported = w.service.HandleTool("asset.import", {{"path", "Assets/in-root/fresh.txt"}, {"force", true}}, WriteMode::WritesEnabled, "alias-import");
			context.Expect(imported.value("ok", false), "Contained redirect import must succeed: " + imported.dump());
			const auto found = database.GetAssets().find(source);
			context.Expect(found != database.GetAssets().end(), "Imported metadata must use the configured database root and canonical in-root target.");
			const auto guid = found->second.guid;
			context.Expect(TerraEngine::ToHexString(guid) == nlohmann::json::parse(BoundaryWorkspace::Read(source.string() + ".meta"))["guid"],
				"The actual catalog lookup and persisted sidecar must agree on identity.");
			for(const auto target : {std::string("Assets/in-root/fresh.txt"), TerraEngine::ToHexString(guid)}) {
				const auto rebuilt = w.service.HandleTool("asset.virtualGeometry.rebuild", {{"target", target}, {"force", true}}, WriteMode::WritesEnabled, "alias-rebuild");
				context.Expect(rebuilt.value("ok", false) && rebuilt["data"]["failedAssets"].size() == 1U, "Generic rebuild must reach the real non-mesh handler: " + rebuilt.dump());
				const auto& failure = rebuilt["data"]["failedAssets"][0];
				context.Expect(failure.value("importer", "") == "GenericImporter" && failure.value("guid", "") == TerraEngine::ToHexString(guid)
					&& failure.value("reason", "") == "virtual geometry metadata is not readable", "Rebuild path and UUID must resolve actual metadata, not fail catalog lookup.");
			}
			std::size_t aliases = 0;
			for(const auto& [path, metadata] : database.GetAssets()) if(std::filesystem::weakly_canonical(path) == std::filesystem::canonical(source)) {
				++aliases; context.Expect(path == source && metadata.guid == guid, "No physical-root or redirect alias may acquire a second catalog identity.");
			}
			context.ExpectEqual(std::size_t{1}, aliases, "Repeated import/rebuild must leave exactly one metadata entry for the source.");
		});
		registry.Add("Editor.ProjectPathBoundary.AssetRedirectEscapesRejectBeforeImport", [](TestContext& context) {
			BoundaryWorkspace w; const auto outside = w.root / "Outside/sentinel.txt";
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			for(;;) {
				w.editor.assetDatabase->Update(); const auto state = w.editor.assetDatabase->GetDiscoveryDiagnostics();
				if(!state.pending && !state.scanning && state.pendingImports == 0U) break;
				context.Expect(std::chrono::steady_clock::now() < deadline, "Initial discovery must settle before creating the unimported lexical decoy.");
				std::this_thread::yield();
			}
			BoundaryWorkspace::Write(outside, "outside unchanged");
			std::filesystem::create_directories(w.root / "Outside/nested");
			FixtureDirectoryRedirect redirect(w.Project() / "Assets/escape", w.root / "Outside/nested");
			BoundaryWorkspace::Write(w.Project() / "Assets/sentinel.txt", "contained lexical decoy");
			for(const auto* operation : {"asset.import", "asset.virtualGeometry.rebuild", "asset.virtualGeometry.repair.execute"}) {
				const auto field = std::string(operation) == "asset.import" ? "path" : "target";
				for(const auto* spelling : {"Assets/escape/../sentinel.txt", "Assets/escape/not-yet-existing.txt"}) {
					const auto response = w.service.HandleTool(operation, {{field, spelling}, {"force", true}}, WriteMode::WritesEnabled, "alias-escape");
					ExpectValidation(context, response);
				}
			}
			context.Expect(!w.editor.assetDatabase->PublishAssetOrganization("Assets/sentinel.txt", "Assets/escape/not-yet-existing.txt", "move", {}).published,
				"A new destination must resolve its existing redirected parent before any outside write.");
			std::filesystem::path rejected;
			context.Expect(!AssetDatabase::ResolveContainedAssetIoPath(w.Project() / "Assets", w.Project() / "Assets/sentinel.txt/child", rejected),
				"An ordinary file cannot supply an existing directory for a missing tail.");
			context.Expect(BoundaryWorkspace::Read(outside) == "outside unchanged" && !std::filesystem::exists(outside.string() + ".meta"),
				"Canonical redirect/parent escape must not write outside metadata or source bytes.");
			context.Expect(!std::filesystem::exists(w.Project() / "Assets/sentinel.txt.meta"), "Rejected raw spelling must not import its lexically collapsed decoy.");
			context.Expect(std::filesystem::is_empty(w.root / "Outside/nested"), "Missing-leaf escapes must create no outside source, sidecar or generated output.");
		});
		for(const bool relative : {false, true}) registry.Add(std::string("Editor.ProjectPathBoundary.SceneSave.") + (relative ? "Relative" : "Absolute"), [relative](TestContext& context) {
			BoundaryWorkspace w;
			BoundaryWorkspace::Write(w.Project() / "Assets/Scenes/Boundary.tescene", nlohmann::json{{"format", "TerraSceneUnboundedV1"}, {"name", "Boundary"}, {"sceneSettings", nlohmann::json::object()}, {"objects", nlohmann::json::array()}}.dump());
			SceneManager::Instance().BeginLoadSession(); SceneManager::Instance().SetSceneRoot(w.Project() / "Assets");
			context.Expect(w.service.HandleTool("scene.open", {{"path", "Assets/Scenes/Boundary.tescene"}}, WriteMode::WritesEnabled, "boundary-open").value("ok", false), "Contained scene must open.");
			context.Expect(w.service.HandleTool("scene.save", nlohmann::json::object(), WriteMode::WritesEnabled, "boundary-save-control").value("ok", false), "Default contained save must remain available.");
			const auto outside = w.root / "Outside/sentinel.tescene"; BoundaryWorkspace::Write(outside, "outside sentinel");
			const auto spelling = relative ? std::filesystem::relative(outside, w.Project() / "Assets").generic_string() : outside.string();
			const auto response = w.service.HandleTool("scene.save", {{"path", spelling}}, WriteMode::WritesEnabled, "boundary-save");
			// Inspect bytes before asserting the envelope: even pre-fix writes stay private.
			context.ExpectEqual(std::string("outside sentinel"), BoundaryWorkspace::Read(outside), "Rejected save must preserve outside bytes.");
			ExpectValidation(context, response);
		});
		registry.Add("Editor.ProjectPathBoundary.SceneSaveDirectoryRedirect", [](TestContext& context) {
			BoundaryWorkspace w; const auto outside = w.root / "Outside/sentinel.tescene";
			BoundaryWorkspace::Write(outside, "outside sentinel");
			FixtureDirectoryRedirect redirect(w);
			BoundaryWorkspace::Write(w.Project() / "Assets/Scenes/Boundary.tescene", nlohmann::json{{"format", "TerraSceneUnboundedV1"}, {"name", "Boundary"}, {"sceneSettings", nlohmann::json::object()}, {"objects", nlohmann::json::array()}}.dump());
			SceneManager::Instance().BeginLoadSession(); SceneManager::Instance().SetSceneRoot(w.Project() / "Assets");
			context.Expect(w.service.HandleTool("scene.open", {{"path", "Assets/Scenes/Boundary.tescene"}}, WriteMode::WritesEnabled, "boundary-redirect-open").value("ok", false), "Contained scene must open.");
			const auto response = w.service.HandleTool("scene.save", {{"path", "redirect/sentinel.tescene"}}, WriteMode::WritesEnabled, "boundary-redirect-save");
			context.ExpectEqual(std::string("outside sentinel"), BoundaryWorkspace::Read(outside), "Canonical save escape must preserve outside bytes.");
			ExpectValidation(context, response);
		});
		for(const auto* operation : {"asset.virtualGeometry.rebuild", "asset.virtualGeometry.repair.execute"}) registry.Add(std::string("Editor.ProjectPathBoundary.") + operation, [operation](TestContext& context) {
			BoundaryWorkspace w; const auto outside = w.root / "Outside/model.obj";
			context.Expect(w.service.FindCapability(operation) != nullptr, "The real endpoint must be discoverable through the provider registry.");
			// A generic contained source takes the real importer/non-mesh failure route without
			// launching a mesh compiler. This distinguishes a guarded executable alias
			// from an unavailable capability or a synthetic validation-only response.
			BoundaryWorkspace::Write(w.Project() / "Assets/contained.txt", "contained source");
			const auto control = w.service.HandleTool(operation, {{"target", "Assets/contained.txt"}, {"force", false}}, WriteMode::WritesEnabled, "boundary-contained-rebuild");
			context.Expect(control.value("ok", false), "Contained endpoint control must dispatch successfully. Response: " + control.dump());
			context.Expect(control["data"].value("assetRegistrationBoundary", "") == "asset-database-finish-import"
				&& control["data"].value("status", "") == "failed" && control["data"].value("target", "") == "Assets/contained.txt"
				&& control["data"]["rebuiltAssets"].empty() && control["data"]["skippedAssets"].empty()
				&& control["data"]["failedAssets"].is_array() && control["data"]["failedAssets"].size() == 1U,
				"Contained generic source must reach the real rebuild handler and report its non-mesh failure. Response: " + control.dump());
			const auto& failure = control["data"]["failedAssets"][0];
			context.Expect(failure.value("importer", "") == "GenericImporter" && failure.value("path", "") == "contained.txt"
				&& failure.value("reason", "") == "virtual geometry metadata is not readable" && !failure.value("guid", "").empty(),
				"Contained dispatch must return the actual imported asset identity and expected non-mesh failure. Response: " + control.dump());
			context.Expect(std::filesystem::exists(w.Project() / "Assets/contained.txt.meta"), "Contained control must create real metadata through the protected importer.");
			BoundaryWorkspace::Write(outside, "# private outside sentinel\n");
				const auto response = w.service.HandleTool(operation, {{"target", outside.string()}, {"force", true}}, WriteMode::WritesEnabled, "boundary-rebuild");
				context.ExpectEqual(std::string("# private outside sentinel\n"), BoundaryWorkspace::Read(outside), "Rejected import must preserve source bytes.");
				context.Expect(!std::filesystem::exists(outside.string() + ".meta"), "Rejected import must not create outside metadata.");
				std::size_t outsideEntries = 0; for(const auto& entry : std::filesystem::recursive_directory_iterator(outside.parent_path())) { (void)entry; ++outsideEntries; }
				context.ExpectEqual(std::size_t{1}, outsideEntries, "Rejected import must create no outside generated outputs.");
				ExpectValidation(context, response);
		});
		registry.Add("Editor.ProjectPathBoundary.ContentVerifySpellings", [](TestContext& context) {
			BoundaryWorkspace w; BoundaryWorkspace::Write(w.Project() / "Assets/inside.txt", "inside");
			context.Expect(w.service.HandleTool("asset.content.verify", {{"path", "Assets/inside.txt"}, {"requestCount", 1}}, WriteMode::ReadOnly, "boundary-read-control").value("ok", false), "Contained verification must remain available.");
			const auto outside = w.root / "Outside/sentinel.txt"; BoundaryWorkspace::Write(outside, "outside sentinel");
			for(const auto& spelling : {outside.string(), std::string("../Outside/sentinel.txt"), std::string("Assets/../../Outside/sentinel.txt")}) {
				const auto response = w.service.HandleTool("asset.content.verify", {{"path", spelling}, {"requestCount", 1}}, WriteMode::ReadOnly, "boundary-read");
				ExpectValidation(context, response);
				context.Expect(!response.value("data", nlohmann::json::object()).contains("contentFingerprint"), "Rejected reads must disclose no fingerprint.");
			}
			if(!outside.root_name().empty()) {
				// Windows drive-relative and root-relative names are not ordinary
				// project-relative paths even though is_absolute() may return false.
				for(const auto& spelling : {outside.root_name().string() + outside.relative_path().string(), (outside.root_directory() / outside.relative_path()).string()}) {
					ExpectValidation(context, w.service.HandleTool("asset.content.verify", {{"path", spelling}, {"requestCount", 1}}, WriteMode::ReadOnly, "boundary-rooted-read"));
				}
			}
		});
		registry.Add("Editor.ProjectPathBoundary.ContentVerifyDirectoryRedirect", [](TestContext& context) {
			BoundaryWorkspace w; BoundaryWorkspace::Write(w.root / "Outside/sentinel.txt", "outside sentinel");
			FixtureDirectoryRedirect redirect(w);
			const auto response = w.service.HandleTool("asset.content.verify", {{"path", "Assets/redirect/sentinel.txt"}, {"requestCount", 1}}, WriteMode::ReadOnly, "boundary-redirect");
			ExpectValidation(context, response);
			context.Expect(!response.value("data", nlohmann::json::object()).contains("contentFingerprint"), "Canonical escape must disclose no fingerprint.");
		});
	}
}
