// LocomotionAutomationTests.cpp - public JSON boundaries over real native state.
#include "../TestHarness.h"

#include "Editor/TerraEngine-Editor/Automation/EditorAutomationService.h"
#include "Editor/TerraEngine-Editor/Core/EditorContext.h"
#include "Editor/TerraEngine-Editor/Project/ProjectManager.h"
#include "Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.h"
#include "Engine/Components/CharacterLocomotionComponent.h"
#include "Engine/Components/Physics/CharacterControllerComponent.h"
#include "Engine/Components/Transform.h"
#include "Engine/Core/Object/ComponentRegistry.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Locomotion/LocomotionSystem.h"
#include "Engine/Physics/PhysicsSystem.h"
#include "Engine/Scene/SceneManager.h"
#include "Engine/Scene/SceneMutationQueue.h"
#include "Engine/Scripting/ScriptingSystem.h"
#include "Engine/ThreadPool/ThreadPool.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <memory>

namespace TerraEngine::EditorTests {
	namespace {
		using namespace TerraEngine::Editor::Automation;
		using Json = nlohmann::json;
		constexpr float kStep = 1.0F / 60.0F;

		// The fixture uses the public service, a real project/scene, and the real
		// controller. No handler stub or direct store injection can make it pass.
		struct LocomotionAutomationFixture {
			std::filesystem::path root = std::filesystem::temp_directory_path()
				/ "TerraEngineLocomotionAutomation" / ToHexString(GenerateUUID());
			EditorContext editor;
			EditorAutomationService service;
			Object owner;
			bool ready = false;
			Locomotion::LocomotionSystem& locomotion = Locomotion::LocomotionSystem::Instance();
			Physics::PhysicsSystem& physics = Physics::PhysicsSystem::Instance();

			LocomotionAutomationFixture() {
				ThreadPool::instance().shutdown();
				ThreadPool::instance().initialize();
				ComponentRegistry::RegisterAllComponents();
				locomotion.Shutdown();
				physics.Shutdown();
				ObjectManager::instance().clear();
				Scene::SceneMutationQueue::Instance().Clear();
				UndoRedoSystem::Instance().Clear();
				std::filesystem::create_directories(root);
				editor.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				if(!editor.projectManager->CreateProject(root, "Locomotion Commands", ProjectTemplate::Blank, &error)) return;
				const auto path = editor.projectManager->GetProjectRoot() / "Assets/Scenes/Locomotion.tescene";
				std::filesystem::create_directories(path.parent_path());
				std::ofstream document(path, std::ios::binary);
				document << Json{{"format", "TerraSceneUnboundedV1"}, {"name", "Locomotion"},
					{"sceneSettings", Json::object()}, {"objects", Json::array()}}.dump(2);
				document.close();
				SceneManager::Instance().UnloadAll();
				SceneManager::Instance().SetSceneRoot(editor.projectManager->GetProjectRoot());
				const UUID scene = SceneManager::Instance().LoadScene(path);
				SceneManager::Instance().SetActiveScene(scene);
				service.SetEditorContext(&editor);
				auto& objects = ObjectManager::instance();
				owner = objects.createObject("Automation Character");
				(void)objects.AddComponent<Transform>(owner);
				auto* controller = objects.AddComponent<CharacterControllerComponent>(owner);
				(void)objects.AddComponent<CharacterLocomotionComponent>(owner);
				if(!controller) return;
				controller->useGravity = false;
				physics.SetEditorLifecycleControlled(true);
				physics.Initialize();
				if(!physics.StartWorld()) return;
				ScriptingSystem::Instance().SetRunning(true);
				locomotion.Initialize();
				Advance();
				Advance();
				ready = locomotion.ResolvePublished(objects.getUUID(owner)).has_value();
			}
			~LocomotionAutomationFixture() {
				ScriptingSystem::Instance().SetRunning(false);
				locomotion.Shutdown();
				(void)physics.StopWorld();
				physics.Shutdown();
				Scene::SceneMutationQueue::Instance().Clear();
				UndoRedoSystem::Instance().Clear();
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				ThreadPool::instance().shutdown();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}
			void Advance() {
				auto& objects = ObjectManager::instance();
				(void)objects.PublishSceneChangePacket();
				physics.Update(kStep);
				const auto flush = Scene::SceneMutationQueue::Instance().Apply(objects, "locomotion-automation-physics");
				physics.OnAfterPhysicsFlush(flush.transactionResults, flush.publicationGroupResults);
				locomotion.ObserveProtectedBoundary();
				locomotion.Update(kStep);
			}
			Json Resolve() {
				return service.HandleTool("locomotion.resolve", {{"objectId", ToHexString(ObjectManager::instance().getUUID(owner))}},
					WriteMode::ReadOnly, "locomotion-resolve");
			}
		};

		Json AcquireCommand(const Json& binding, std::uint64_t sequence = 1U) {
			return {{"kind", "Acquire"}, {"handle", binding.at("handle")},
				{"expectedAuthorityEpoch", binding.at("authorityEpoch")},
				{"expectedConfigurationRevision", binding.at("configurationRevision")},
				{"ownerToken", 0U}, {"producerId", 7101U}, {"sequence", sequence},
				{"correlationId", 7000U + sequence}, {"intentKind", "Script"}, {"mode", "Velocity"}};
		}
	}

	void RegisterLocomotionAutomationTests(TestRegistry& registry) {
		registry.Add("Editor.Locomotion.Automation.DeclaresBoundedProtectedCapabilities", [](TestContext& context) {
			EditorAutomationService service;
			for(const std::string_view name : {"locomotion.resolve", "locomotion.inspect", "locomotion.command", "locomotion.receipt"}) {
				const auto capability = service.GetRegistry().Find(name, 1U);
				context.Expect(capability.has_value(), "Every locomotion operation must be discoverable through the real registry.");
				if(!capability) continue;
				const auto& descriptor = capability->descriptor;
				const bool write = name == "locomotion.command";
				context.Expect(descriptor.permission == (write ? AutomationPermission::ProjectWrite : AutomationPermission::Read),
					"Only command admission requires write permission.");
				context.Expect(!descriptor.execution.visibilityBoundary.empty()
					&& descriptor.execution.protectedCommit == write,
					"Declarations distinguish immutable reads from protected queued command admission.");
				if(write) context.Expect(std::find(descriptor.execution.commitWrites.begin(), descriptor.execution.commitWrites.end(),
					"LocomotionCommands") != descriptor.execution.commitWrites.end(), "Command admission declares its typed queue resource.");
			}
		});

		registry.Add("Editor.Locomotion.Automation.PermissionReceiptsAndAtomicAdmission", [](TestContext& context) {
			LocomotionAutomationFixture fixture;
			context.Expect(fixture.ready, "The real automation character must publish before tool dispatch.");
			if(!fixture.ready) return;
			const Json resolved = fixture.Resolve();
			context.Expect(resolved.value("ok", false), "Resolve must return a full current handle and acquisition preconditions.");
			if(!resolved.value("ok", false)) return;
			const Json binding = resolved.at("data");
			const Json command = AcquireCommand(binding);
			const Json payload = {{"commands", Json::array({command})}};
			const auto pendingBefore = fixture.locomotion.ReadDiagnostics().pendingCommands;
			const Json denied = fixture.service.HandleTool("locomotion.command", payload, WriteMode::ReadOnly, "locomotion-denied");
			context.Expect(!denied.value("ok", true) && fixture.locomotion.ReadDiagnostics().pendingCommands == pendingBefore,
				"Read-only transport must not admit any movement command.");
			Json malformed = command;
			malformed["sequence"] = "not-a-number";
			const Json rejected = fixture.service.HandleTool("locomotion.command", {{"commands", Json::array({command, malformed})}},
				WriteMode::WritesEnabled, "locomotion-atomic-invalid");
			context.Expect(!rejected.value("ok", true), "One malformed batch member rejects the whole JSON batch before admission.");
			const Json query = {{"handle", binding.at("handle")}, {"producerId", 7101U}, {"sequence", 1U}, {"correlationId", 7001U}};
			const Json absent = fixture.service.HandleTool("locomotion.receipt", query, WriteMode::ReadOnly, "locomotion-rejected-absent");
			context.Expect(absent.value("ok", false) && absent.at("data").at("receipt").value("state", "") == "Expired",
				"Rejected transport and malformed batches must not admit even their first valid member.");
			const Json queued = fixture.service.HandleTool("locomotion.command", payload, WriteMode::WritesEnabled, "locomotion-acquire");
			context.Expect(queued.value("ok", false) && queued.at("data").at("receipts").at(0).value("state", "") == "Queued",
				"Admission is Queued, never falsely Applied before the movement boundary.");
			if(!queued.value("ok", false)) return;
			const Json waiting = fixture.service.HandleTool("locomotion.receipt", query, WriteMode::ReadOnly, "locomotion-waiting");
			context.Expect(waiting.value("ok", false) && waiting.at("data").at("receipt").value("state", "") == "Queued",
				"The exact producer/sequence/correlation receipt remains queued until publication.");
			fixture.Advance();
			const Json applied = fixture.service.HandleTool("locomotion.receipt", query, WriteMode::ReadOnly, "locomotion-applied");
			context.Expect(applied.value("ok", false) && applied.at("data").at("receipt").value("state", "") == "Applied"
				&& applied.at("data").at("receipt").value("ownerToken", std::uint64_t{}) != 0U,
				"Only protected native publication grants a nonzero movement owner token.");
			const Json retry = fixture.service.HandleTool("locomotion.command", payload, WriteMode::WritesEnabled, "locomotion-retry");
			context.Expect(retry.value("ok", false) && retry.at("data").at("receipts").at(0) == applied.at("data").at("receipt"),
				"An exact semantic retry returns the retained applied receipt without another acquisition.");
			context.Expect(!UndoRedoSystem::Instance().CanUndo(), "Runtime movement commands must not create authored undo history.");
		});

		registry.Add("Editor.Locomotion.Automation.RejectedAuthoredEditPreservesConfiguration", [](TestContext& context) {
			LocomotionAutomationFixture fixture;
			context.Expect(fixture.ready, "The authoring automation fixture must initialize.");
			if(!fixture.ready) return;
			auto& objects = ObjectManager::instance();
			const Json before = CharacterLocomotionComponent::SerializeAuthored(*objects.GetComponent<CharacterLocomotionComponent>(fixture.owner));
			const Json edit = fixture.service.HandleTool("component.set_property", {
				{"object", ToHexString(objects.getUUID(fixture.owner))}, {"component", "CharacterLocomotionComponent"},
				{"property", "maximumSpeed"}, {"value", -1.0}}, WriteMode::WritesEnabled, "locomotion-invalid-edit");
			(void)Scene::SceneMutationQueue::Instance().Apply(objects, "locomotion-invalid-edit-flush");
			context.Expect(!edit.value("ok", true)
				&& CharacterLocomotionComponent::SerializeAuthored(*objects.GetComponent<CharacterLocomotionComponent>(fixture.owner)) == before
				&& !UndoRedoSystem::Instance().CanUndo(),
				"Generic authored edits share full locomotion validation and rejected changes create no history.");
		});

		registry.Add("Editor.Locomotion.Automation.BoundedObservationAndStaleIdentity", [](TestContext& context) {
			LocomotionAutomationFixture fixture;
			context.Expect(fixture.ready, "Observation fixture must publish a real controller.");
			if(!fixture.ready) return;
			const Json resolved = fixture.Resolve();
			context.Expect(resolved.value("ok", false), "Resolve must succeed before stale identity coverage.");
			if(!resolved.value("ok", false)) return;
			const Json snapshot = fixture.service.HandleTool("locomotion.inspect", {{"maxItems", 256U}}, WriteMode::ReadOnly, "locomotion-inspect");
			context.Expect(snapshot.value("ok", false) && snapshot.at("data").at("observations").size() == 1U
				&& snapshot.at("data").value("publicationEpoch", std::uint64_t{}) != 0U,
				"Inspection returns one bounded immutable publication with an explicit epoch.");
			const Json excessive = fixture.service.HandleTool("locomotion.inspect", {{"maxItems", 257U}}, WriteMode::ReadOnly, "locomotion-excessive");
			context.Expect(!excessive.value("ok", true), "Observation bounds are rejected, not silently truncated.");
			Json oldCommand = AcquireCommand(resolved.at("data"));
			oldCommand["handle"]["attachmentGeneration"] = std::uint64_t{1} << 48U;
			const Json stale = fixture.service.HandleTool("locomotion.command", {{"commands", Json::array({oldCommand})}},
				WriteMode::WritesEnabled, "locomotion-stale");
			context.Expect(!stale.value("ok", true), "A high-word stale attachment generation is preserved and rejected.");
			ScriptingSystem::Instance().SetRunning(false);
			const Json notPlaying = fixture.service.HandleTool("locomotion.command", {{"commands", Json::array({AcquireCommand(resolved.at("data"))})}},
				WriteMode::WritesEnabled, "locomotion-not-playing");
			context.Expect(!notPlaying.value("ok", true), "Write permission alone must not admit commands outside Play.");
		});
	}
}
