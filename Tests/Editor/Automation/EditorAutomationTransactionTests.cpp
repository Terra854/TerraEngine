#include "../TestHarness.h"

#include "EditorAutomationService.h"

#include "EditorContext.h"
#include "Engine/Core/Object/ObjectManager.h"
#include "Engine/Scene/SceneManager.h"
#include "ProjectManager.h"

#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>

namespace TerraEngine::EditorTests
{
	using TerraEngine::Editor::Automation::EditorAutomationService;
	using TerraEngine::Editor::Automation::WriteMode;

	namespace
	{
		// Each test receives a real project and active scene because transaction
		// availability must be evaluated exactly as it is for an MCP client.
		class TransactionProjectFixture
		{
		public:
			explicit TransactionProjectFixture(const std::string& suffix)
				: root(std::filesystem::temp_directory_path() / "TerraEngineAutomationTransactions" / suffix)
			{
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
				std::filesystem::create_directories(root);

				context.projectManager = std::make_shared<ProjectManager>();
				std::string error;
				context.projectManager->CreateProject(root, suffix, ProjectTemplate::Blank, &error);

				const std::filesystem::path scenePath = context.projectManager->GetProjectRoot() / "Assets/Scenes/Transaction.tescene";
				std::filesystem::create_directories(scenePath.parent_path());
				std::ofstream scene(scenePath, std::ios::binary);
				scene << nlohmann::json{
					{"name", "Transaction"},
					{"sceneSettings", nlohmann::json::object()},
					{"objects", nlohmann::json::array()}
				}.dump(2);
				scene.close();

				SceneManager::Instance().UnloadAll();
				SceneManager::Instance().SetSceneRoot(context.projectManager->GetProjectRoot());
				const TerraEngine::UUID sceneId = SceneManager::Instance().LoadScene(scenePath);
				SceneManager::Instance().SetActiveScene(sceneId);
				service.SetEditorContext(&context);
			}

			~TransactionProjectFixture()
			{
				ObjectManager::instance().clear();
				SceneManager::Instance().UnloadAll();
				std::error_code ignored;
				std::filesystem::remove_all(root, ignored);
			}

			nlohmann::json Execute(nlohmann::json operations, const std::string& requestId, nlohmann::json extra = nlohmann::json::object())
			{
				extra["operations"] = std::move(operations);
				extra["label"] = extra.value("label", std::string("Automation transaction"));
				extra["correlationId"] = extra.value("correlationId", requestId + "-correlation");
				extra["idempotencyKey"] = extra.value("idempotencyKey", requestId + "-key");
				return service.HandleTool("transaction.execute", extra, WriteMode::WritesEnabled, requestId);
			}

			std::filesystem::path root;
			EditorContext context;
			EditorAutomationService service;
		};

		void ExpectAtomicSuccessEnvelope(TestContext& context, const nlohmann::json& result, std::size_t expectedAffected)
		{
			context.Expect(result.value("ok", false), "A valid typed transaction should commit.");
			context.Expect(!result["data"].value("transactionId", std::string()).empty(), "Committed transactions should return their durable transaction identity.");
			context.Expect(!result["data"].value("historyId", std::string()).empty(), "A scene-changing transaction should return its composite history identity.");
			context.ExpectEqual(std::string("post_editor_automation_flush"), result["data"].value("visibilityBoundary", std::string()), "The result should name the protected publication boundary.");
			context.ExpectEqual(expectedAffected, result["data"]["affectedTargets"].size(), "The result should enumerate every affected target once.");
		}
	}

	void RegisterEditorAutomationTransactionTests(TestRegistry& registry)
	{
		registry.Add("Editor.Automation.Transaction.LowersTypedMixedOperations", [](TestContext& context) {
			TransactionProjectFixture fixture("TypedLowering");
			std::optional<entt::entity> selected;
			fixture.service.SetSelectionCallbacks([&selected]() { return selected; }, [&selected](std::optional<entt::entity> value) { selected = value; });

			const nlohmann::json result = fixture.Execute(nlohmann::json::array({
				{{"kind", "object.create"}, {"localId", "parent"}, {"name", "Parent"}},
				{{"kind", "object.create"}, {"localId", "child"}, {"name", "Child"}},
				{{"kind", "object.set_metadata"}, {"target", {{"localId", "child"}}}, {"tag", "Generated"}, {"active", false}},
				{{"kind", "object.set_parent"}, {"target", {{"localId", "child"}}}, {"parent", {{"localId", "parent"}}}},
				{{"kind", "selection.set"}, {"target", {{"localId", "child"}}}}
			}), "transaction-typed");

			ExpectAtomicSuccessEnvelope(context, result, 2u);
			const auto child = ObjectManager::instance().FindObjectByName("Child");
			context.Expect(child.has_value(), "Typed object creation should publish the child.");
			context.ExpectEqual(std::string("Generated"), ObjectManager::instance().getObjectTag(*child), "Metadata lowering should preserve typed values.");
			context.Expect(!ObjectManager::instance().isObjectActive(*child), "Boolean metadata should be lowered without string conversion.");
			context.Expect(selected.has_value(), "The selection tail should commit only after the scene transaction succeeds.");
		});

		registry.Add("Editor.Automation.Transaction.RejectsOperationAndPayloadLimits", [](TestContext& context) {
			TransactionProjectFixture fixture("PayloadLimits");
			nlohmann::json tooMany = nlohmann::json::array();
			for (std::size_t index = 0; index < 4097u; ++index)
			{
				tooMany.push_back({{"kind", "object.create"}, {"localId", "object-" + std::to_string(index)}, {"name", "Object"}});
			}

			const nlohmann::json operationLimit = fixture.Execute(std::move(tooMany), "transaction-operation-limit");
			context.Expect(!operationLimit.value("ok", true), "Transactions above the typed operation budget must be rejected before commit.");
			context.ExpectEqual(std::string("over_budget"), operationLimit["error"].value("category", std::string()), "Operation limit failures should be machine-readable.");
			context.Expect(ObjectManager::instance().getRootObjects().empty(), "An over-budget request must publish no objects.");

			const std::string excessiveName(2u * 1024u * 1024u, 'x');
			const nlohmann::json payloadLimit = fixture.Execute(nlohmann::json::array({
				{{"kind", "object.create"}, {"localId", "oversized"}, {"name", excessiveName}}
			}), "transaction-payload-limit");
			context.Expect(!payloadLimit.value("ok", true), "Transactions above the serialized payload budget must be rejected.");
			context.ExpectEqual(std::string("over_budget"), payloadLimit["error"].value("category", std::string()), "Payload limit failures should use the over-budget category.");
			context.Expect(ObjectManager::instance().getRootObjects().empty(), "An oversized payload must not leak a partial create.");
		});

		registry.Add("Editor.Automation.Transaction.RejectsStaleAndConflictingPreconditionsAtomically", [](TestContext& context) {
			TransactionProjectFixture fixture("StaleConflict");
			Object target = ObjectManager::instance().createObject("Original");
			const std::string targetId = TerraEngine::ToHexString(ObjectManager::instance().getUUID(target));
			const std::size_t beforeCount = ObjectManager::instance().getRootObjects().size();

			const nlohmann::json stale = fixture.Execute(nlohmann::json::array({
				{{"kind", "object.set_metadata"}, {"target", targetId}, {"name", "Must Not Apply"}}
			}), "transaction-stale", {{"expectedSceneGeneration", std::numeric_limits<std::uint64_t>::max()}});
			context.Expect(!stale.value("ok", true), "A stale scene generation must reject the whole transaction.");
			context.ExpectEqual(std::string("stale_target"), stale["error"].value("category", std::string()), "Stale preconditions should remain distinguishable from malformed input.");
			context.ExpectEqual(std::string("Original"), ObjectManager::instance().getObjectName(target), "Stale rejection must preserve the exact prior state.");

			const nlohmann::json conflict = fixture.Execute(nlohmann::json::array({
				{{"kind", "object.create"}, {"localId", "duplicate-local-id"}, {"name", "Would Leak"}},
				{{"kind", "object.create"}, {"localId", "duplicate-local-id"}, {"name", "Conflicting Identity"}}
			}), "transaction-conflict");
			context.Expect(!conflict.value("ok", true), "Conflicting writes to one property should reject during preflight.");
			context.ExpectEqual(std::string("conflict"), conflict["error"].value("category", std::string()), "Intra-batch conflicts should use the conflict category.");
			context.ExpectEqual(beforeCount, ObjectManager::instance().getRootObjects().size(), "A later conflict must roll back earlier valid operations.");
			context.ExpectEqual(std::string("Original"), ObjectManager::instance().getObjectName(target), "Conflict rollback should leave metadata unchanged.");
		});

		registry.Add("Editor.Automation.Transaction.HonorsCancellationBeforeCommit", [](TestContext& context) {
			TransactionProjectFixture fixture("Cancellation");
			const nlohmann::json canceled = fixture.Execute(nlohmann::json::array({
				{{"kind", "object.create"}, {"localId", "canceled"}, {"name", "Canceled Object"}}
			}), "transaction-canceled", {{"cancelRequested", true}});

			context.Expect(!canceled.value("ok", true), "Cancellation observed before protected commit should return a canceled result.");
			context.ExpectEqual(std::string("canceled"), canceled["error"].value("category", std::string()), "Canceled requests should not be reported as validation failures.");
			context.ExpectEqual(std::string("before_commit"), canceled["data"].value("cancellationBoundary", std::string()), "The response should identify that no publication boundary was crossed.");
			context.Expect(ObjectManager::instance().getRootObjects().empty(), "Cancellation before commit must apply zero operations.");
		});

		registry.Add("Editor.Automation.Transaction.CommitsOneHundredOperationsAtomically", [](TestContext& context) {
			TransactionProjectFixture fixture("HundredOperations");
			nlohmann::json operations = nlohmann::json::array();
			for (std::size_t index = 0; index < 100u; ++index)
			{
				operations.push_back({{"kind", "object.create"}, {"localId", "batch-" + std::to_string(index)}, {"name", "Batch " + std::to_string(index)}});
			}

			const nlohmann::json result = fixture.Execute(std::move(operations), "transaction-hundred");

			ExpectAtomicSuccessEnvelope(context, result, 100u);
			context.ExpectEqual(static_cast<std::size_t>(100), ObjectManager::instance().getRootObjects().size(), "All one hundred writes should become visible together.");
			context.ExpectEqual(static_cast<std::size_t>(100), result["data"].value("appliedOperationCount", static_cast<std::size_t>(0)), "The transaction result should prove every operation committed.");
		});
	}
}
