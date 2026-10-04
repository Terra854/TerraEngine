// Exercise the real automation save adapters, including their history adoption.
#include "../TestHarness.h"
#include "../../Engine/Runtime/CorrectnessTestSupport.h"
#include "EditorAutomationService.h"
#include "EditorContext.h"
#include "ProjectManager.h"
#include "UndoRedoSystem.h"
#include "Core/Object/ObjectManager.h"
#include "Scene/SceneManager.h"

#include <fstream>
#include <iterator>

namespace TerraEngine::EditorTests {
	namespace {
		std::string SavedBytes(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}
		// The command gives history a real unsaved state. Its target belongs to
		// the fixture and is released only after history has been cleared.
		class SaveHistoryCommand final : public ICommand {
		public:
			explicit SaveHistoryCommand(int& target) : target_(target) {}
			void Undo() override { target_ = 0; }
			void Redo() override { target_ = 1; }
			std::string GetName() const override { return "Save adapter authored edit"; }
		private:
			int& target_;
		};
		struct SaveAdapterFixture final {
			TerraEngine::Tests::Correctness::DisposableFixturePath paths;
			EditorContext editor;
			TerraEngine::Editor::Automation::EditorAutomationService service;
			std::filesystem::path previousRoot = SceneManager::Instance().GetSceneRoot();
			std::filesystem::path scene;
			Object object;
			int historyTarget = 1;
			explicit SaveAdapterFixture(TestContext& context) {
				editor.projectManager = std::make_shared<ProjectManager>();
				context.Expect(editor.projectManager->CreateProject(paths.Child("Project"), "SaveAdapter", ProjectTemplate::Blank),
					"A real disposable project must be available to the automation adapter.");
				auto& scenes = SceneManager::Instance();
				scenes.BeginLoadSession(); ObjectManager::instance().clear(); UndoRedoSystem::Instance().Clear();
				scenes.SetSceneRoot(editor.projectManager->GetProjectRoot() / "Assets");
				scene = editor.projectManager->GetProjectRoot() / "Assets/Scenes/Adapter.tescene";
				const nlohmann::json document{{"format", "TerraSceneUnboundedV1"}, {"name", "Adapter"}, {"objects", nlohmann::json::array()}};
				{ std::ofstream output(scene); output << document.dump(); }
				const auto id = scenes.LoadScene(scene);
				context.Expect(id != TerraEngine::EMPTY_UUID && scenes.SetActiveScene(id), "The production scene must become active.");
				object = ObjectManager::instance().createObject("Complete authored value");
				context.Expect(scenes.SaveScene(), "A complete source save must establish old bytes.");
				UndoRedoSystem::Instance().SetSavePoint();
				UndoRedoSystem::Instance().PushCommand(std::make_unique<SaveHistoryCommand>(historyTarget));
				service.SetEditorContext(&editor);
			}
			~SaveAdapterFixture() {
				UndoRedoSystem::Instance().Clear(); SceneManager::Instance().UnloadAll();
				ObjectManager::instance().clear(); SceneManager::Instance().SetSceneRoot(previousRoot);
			}
		};
		void CheckSaveAdapter(TestContext& context, bool saveAs) {
			using TerraEngine::Editor::Automation::WriteMode;
			SaveAdapterFixture fixture(context);
			const auto original = SavedBytes(fixture.scene);
			const auto cache = SceneManager::Instance().GetActiveScene()->jsonContents;
			const auto originalPath = SceneManager::Instance().GetActiveScene()->sourcePath;
			const auto destination = fixture.editor.projectManager->GetProjectRoot() / "Assets/Scenes/Copy.tescene";
			ObjectManager::instance().setObjectName(fixture.object, std::string(1, static_cast<char>(0xff)));
			SceneManager::Instance().ForceMarkActiveSceneDirty();
			const auto args = saveAs ? nlohmann::json{{"path", "Assets/Scenes/Copy.tescene"}} : nlohmann::json::object();
			const auto operation = saveAs ? "scene.save_as" : "scene.save";
			const auto rejected = fixture.service.HandleTool(operation, args, WriteMode::WritesEnabled, "failed-save-adapter");
			context.Expect(!rejected.value("ok", true), "The real save adapter must report a failed serialization.");
			context.Expect(SavedBytes(fixture.scene) == original && (!saveAs || !std::filesystem::exists(destination)),
				"Failed adapter saving must preserve source bytes and leave a new destination absent.");
			context.Expect(SceneManager::Instance().IsActiveSceneDirty() && !UndoRedoSystem::Instance().IsAtSavePoint()
				&& SceneManager::Instance().GetActiveScene()->jsonContents == cache
				&& SceneManager::Instance().GetActiveScene()->sourcePath == originalPath,
				"Failure must retain the current document, dirty history and last persisted cache.");
			ObjectManager::instance().setObjectName(fixture.object, "Complete successful retry");
			const auto retried = fixture.service.HandleTool(operation, args, WriteMode::WritesEnabled, "retried-save-adapter");
			context.Expect(retried.value("ok", false) && UndoRedoSystem::Instance().IsAtSavePoint(),
				"A complete retry must report success and adopt the exact saved history.");
			context.Expect(std::filesystem::is_regular_file(saveAs ? destination : fixture.scene), "Retry must publish complete bytes.");
		}
		void CheckFailedReloadSaveAs(TestContext& context, int failure) {
			using TerraEngine::Editor::Automation::WriteMode;
			SaveAdapterFixture fixture(context);
			auto& scenes = SceneManager::Instance();
			const auto activeId = scenes.GetActiveSceneId();
			const auto identity = scenes.GetActiveScene()->saveIdentity;
			const auto document = scenes.GetActiveScene()->parsedJson;
			const auto settings = Serializer::SerializeSceneRenderSettings(scenes.GetActiveSceneRenderSettings());
			const auto unrelatedPath = fixture.scene.parent_path() / "Unrelated.tescene";
			{ std::ofstream output(unrelatedPath); output << document.dump(); }
			const auto unrelatedId = scenes.LoadScene(unrelatedPath);
			context.Expect(unrelatedId != TerraEngine::EMPTY_UUID, "An unrelated resident document must survive retry without cache clearing.");
			ObjectManager::instance().setObjectName(fixture.object, "Dirty reload recovery edit");
			scenes.ForceMarkActiveSceneDirty();
			if(failure == 0) std::filesystem::remove(fixture.scene);
			else {
				std::ofstream output(fixture.scene, std::ios::trunc);
				if(failure == 1) output << "{ malformed scene";
				else { auto wrong = document; wrong["name"] = 42; output << wrong.dump(); }
			}
			// Use the actual open adapter: its failed reload must not adopt a new
			// scene or clear the dirty editor history before Save As can recover it.
			const auto rejected = fixture.service.HandleTool("scene.open", {{"path", "Assets/Scenes/Adapter.tescene"}}, WriteMode::WritesEnabled, "rejected-reload");
			context.Expect(!rejected.value("ok", true), "The production open adapter must report invalid replacement failure.");
			const auto* retained = scenes.GetScene(activeId);
			context.Expect(retained != nullptr, "Failed editor reload must retain the usable scene document.");
			context.Expect(retained->saveIdentity == identity && retained->parsedJson == document
				&& scenes.GetActiveSceneId() == activeId && scenes.IsActiveSceneDirty()
				&& !UndoRedoSystem::Instance().IsAtSavePoint() && scenes.GetScene(unrelatedId) != nullptr
				&& ObjectManager::instance().getObjectName(fixture.object) == "Dirty reload recovery edit"
				&& Serializer::SerializeSceneRenderSettings(scenes.GetActiveSceneRenderSettings()) == settings,
				"Rejected replacement must preserve dirty live values, settings, document identity, history and unrelated scenes.");
			const auto saved = fixture.service.HandleTool("scene.save_as", {{"path", "Assets/Scenes/Recovered.tescene"}}, WriteMode::WritesEnabled, "recovered-save-as");
			context.Expect(saved.value("ok", false) && UndoRedoSystem::Instance().IsAtSavePoint(), "Actual Save As must persist and adopt the retained live edit.");
			const auto reopened = fixture.service.HandleTool("scene.open", {{"path", "Assets/Scenes/Recovered.tescene"}}, WriteMode::WritesEnabled, "reopen-recovered-edit");
			context.Expect(reopened.value("ok", false), "The recovered Save As must reopen through production protected activation.");
			const auto roots = ObjectManager::instance().getRootObjects();
			context.Expect(roots.size() == 1 && ObjectManager::instance().getObjectName(roots.front()) == "Dirty reload recovery edit"
				&& scenes.GetScene(unrelatedId) != nullptr, "Reopened authored objects must contain the edit without clearing unrelated documents.");
		}
	}
	void RegisterEditorSceneSaveFailureSafetyTests(TestRegistry& registry) {
		registry.Add("Editor.SceneSaveFailureSafety.UnreadableReloadRetainsDirtySaveAs", [](TestContext& context) { CheckFailedReloadSaveAs(context, 0); });
		registry.Add("Editor.SceneSaveFailureSafety.MalformedReloadRetainsDirtySaveAs", [](TestContext& context) { CheckFailedReloadSaveAs(context, 1); });
		registry.Add("Editor.SceneSaveFailureSafety.WrongTypedReloadRetainsDirtySaveAs", [](TestContext& context) { CheckFailedReloadSaveAs(context, 2); });
		registry.Add("Editor.SceneSaveFailureSafety.FailedSavePreservesHistoryAndDocument", [](TestContext& context) { CheckSaveAdapter(context, false); });
		registry.Add("Editor.SceneSaveFailureSafety.FailedSaveAsDoesNotAdoptNewDestination", [](TestContext& context) { CheckSaveAdapter(context, true); });
	}
}
