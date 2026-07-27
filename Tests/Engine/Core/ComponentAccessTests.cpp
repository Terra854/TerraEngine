#include "../TestHarness.h"

#include "Core/Object/ObjectManager.h"
#include "Core/Scheduling/ComponentAccess.h"

#include <algorithm>
#include <string>
#include <vector>

namespace TerraEngine::Tests {
	namespace {
		TerraEngine::Scheduling::ComponentAccessDeclaration MakeAccess()
		{
			TerraEngine::Scheduling::ComponentAccessDeclaration declaration;
			declaration.ownerSystem = "PhysicsSystem";
			declaration.queryName = "IntegrateBodies";
			declaration.workerEligible = true;
			return declaration;
		}

		bool HasCode(const std::vector<TerraEngine::Scheduling::ComponentAccessDiagnostic>& diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode code)
		{
			return std::any_of(diagnostics.begin(), diagnostics.end(), [&](const TerraEngine::Scheduling::ComponentAccessDiagnostic& diagnostic) {
				return diagnostic.code == code;
			});
		}
	}

	void RegisterComponentAccessTests(TestRegistry& registry)
	{
		registry.Add("Engine.ComponentAccess.NormalizesDuplicatesAndWritePromotion", [](TestContext& context) {
			auto declaration = MakeAccess();
			declaration.readComponents = { "Transform", "Transform", "RigidBody" };
			declaration.writeComponents = { "Transform", "Transform" };

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessDeclaration(declaration);

			context.Expect(result.valid, "Duplicate and read-promoted access should remain valid.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.normalized.readComponents.size(), "Write declarations should remove the same component from read declarations.");
			context.ExpectEqual(std::string("RigidBody"), result.normalized.readComponents.front(), "Non-written reads should be preserved.");
			context.ExpectEqual(static_cast<std::size_t>(1), result.normalized.writeComponents.size(), "Duplicate writes should be collapsed.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::DuplicateRead), "Duplicate reads should produce an info diagnostic.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::DuplicateWrite), "Duplicate writes should produce an info diagnostic.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::ReadPromotedToWrite), "Read/write overlap should explain write promotion.");
		});

		registry.Add("Engine.ComponentAccess.ClassifiesReadReadAndWriteConflicts", [](TestContext& context) {
			auto readerA = MakeAccess();
			readerA.ownerSystem = "AnimationA";
			readerA.readComponents = { "Transform" };
			auto readerB = MakeAccess();
			readerB.ownerSystem = "AnimationB";
			readerB.readComponents = { "Transform" };
			auto writer = MakeAccess();
			writer.ownerSystem = "Physics";
			writer.writeComponents = { "Transform" };

			const auto readRead = TerraEngine::Scheduling::ClassifyComponentAccessConflict(readerA, readerB);
			const auto readWrite = TerraEngine::Scheduling::ClassifyComponentAccessConflict(readerA, writer);
			const auto writeWrite = TerraEngine::Scheduling::ClassifyComponentAccessConflict(writer, writer);

			context.ExpectEqual(std::string("read-read"), TerraEngine::Scheduling::ToString(readRead.kind), "Read/read access should be recognized as compatible overlap.");
			context.Expect(!readRead.requiresOrdering, "Read/read access should not require ordering.");
			context.ExpectEqual(std::string("read-write"), TerraEngine::Scheduling::ToString(readWrite.kind), "Read/write access should require deterministic ordering.");
			context.Expect(readWrite.requiresOrdering, "Read/write access should require ordering.");
			context.ExpectEqual(std::string("write-write"), TerraEngine::Scheduling::ToString(writeWrite.kind), "Write/write access should require deterministic ordering.");
			context.Expect(writeWrite.requiresOrdering, "Write/write access should require ordering.");
		});

		registry.Add("Engine.ComponentAccess.MissingWorkerDeclarationFallsBackProtected", [](TestContext& context) {
			auto declaration = MakeAccess();
			const auto result = TerraEngine::Scheduling::ValidateComponentAccessDeclaration(declaration);

			context.Expect(result.valid, "Missing access on worker-eligible work should be diagnosed without crashing validation.");
			context.Expect(result.protectedFallback, "Worker-eligible work without access metadata should fall back to protected execution.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::MissingWorkerDeclaration), "Missing worker declaration should produce a diagnostic.");
		});

		registry.Add("Engine.ComponentAccess.ReflectedEditorScriptBoundaryStaysProtected", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest reflectedRequest;
			reflectedRequest.grantId = "editor-script-reflection";
			reflectedRequest.workloadName = "ManagedScriptReflection";
			reflectedRequest.readComponents = { "Transform" };
			reflectedRequest.scope = "reflected-editor-script";

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(reflectedRequest);

			context.Expect(!result.granted, "Reflected editor/script access should not become a worker grant without scheduler validation.");
			context.Expect(result.fallbackReason.find("Scheduler validation") != std::string::npos, "Protected fallback should explain that scheduler validation is required.");
		});

		registry.Add("Engine.ComponentAccess.GrantModesSeparateWorkerAndProtectedCommit", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest readGrant;
			readGrant.grantId = "read";
			readGrant.workloadName = "ReadWork";
			readGrant.readComponents = { "Transform" };
			readGrant.grantMode = TerraEngine::Scheduling::ComponentAccessGrantMode::WorkerRead;
			readGrant.schedulerValidated = true;
			const auto readResult = TerraEngine::Scheduling::ValidateComponentAccessGrant(readGrant);

			TerraEngine::Scheduling::ComponentAccessGrantRequest protectedGrant;
			protectedGrant.grantId = "commit";
			protectedGrant.workloadName = "CommitWork";
			protectedGrant.writeComponents = { "Transform" };
			protectedGrant.grantMode = TerraEngine::Scheduling::ComponentAccessGrantMode::ProtectedCommit;
			const auto protectedResult = TerraEngine::Scheduling::ValidateComponentAccessGrant(protectedGrant);

			readGrant.writeComponents = { "Transform" };
			const auto invalidRead = TerraEngine::Scheduling::ValidateComponentAccessGrant(readGrant);

			context.Expect(readResult.granted, "Scheduler-validated worker-read grants should be allowed.");
			context.Expect(protectedResult.granted, "Protected commit grants should be allowed on the serialized commit path.");
			context.Expect(!invalidRead.granted, "Worker-read grants should reject writes.");
			context.Expect(protectedResult.normalized.scope.find("protected-commit") != std::string::npos, "Grant scope should name the protected commit mode.");
		});

		registry.Add("Engine.ComponentAccess.ScopeValidationRejectsSnapshotWrites", [](TestContext& context) {
			auto declaration = MakeAccess();
			declaration.scopes = {
				TerraEngine::Scheduling::ComponentAccessScope::Snapshot,
				TerraEngine::Scheduling::ComponentAccessScope::Input,
				TerraEngine::Scheduling::ComponentAccessScope::Window
			};
			declaration.writeComponents = { "Transform" };

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessDeclaration(declaration);

			context.Expect(!result.valid, "Read-only snapshot/input/window declarations should reject writes.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::AccessScopeConflict), "Scope conflict should be diagnosed with a stable code.");
			context.Expect(result.formatted.find("snapshot") != std::string::npos, "Formatted diagnostics should name the rejected scope.");
		});

		registry.Add("Engine.ComponentAccess.ProtectedEditorScopeFallsBack", [](TestContext& context) {
			auto declaration = MakeAccess();
			declaration.scopes = {
				TerraEngine::Scheduling::ComponentAccessScope::Editor,
				TerraEngine::Scheduling::ComponentAccessScope::ProtectedWrite
			};
			declaration.writeComponents = { "Transform" };

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessDeclaration(declaration);

			context.Expect(result.valid, "Protected/editor scopes should be valid, but not worker scheduled.");
			context.Expect(result.protectedFallback, "Protected/editor scopes should force protected fallback.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::ProtectedFallback), "Fallback should be visible to diagnostics.");
		});

		registry.Add("Engine.ComponentAccess.StaleGenerationBlocksWorkerSnapshot", [](TestContext& context) {
			auto declaration = MakeAccess();
			declaration.scopes = { TerraEngine::Scheduling::ComponentAccessScope::Snapshot };
			declaration.readComponents = { "Transform" };
			declaration.snapshotGeneration = 7;
			declaration.currentGeneration = 9;

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessDeclaration(declaration);

			context.Expect(!result.valid, "Stale snapshot generations should fail closed before worker execution.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::StaleGeneration), "Stale generation should be diagnosed explicitly.");
		});

		registry.Add("Engine.ComponentAccess.GrantRejectsStaleInputSnapshot", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest request;
			request.grantId = "input-snapshot";
			request.workloadName = "InputReaders";
			request.readComponents = { "InputState" };
			request.scope = "frame-input";
			request.scopeKind = TerraEngine::Scheduling::ComponentAccessScope::Input;
			request.grantMode = TerraEngine::Scheduling::ComponentAccessGrantMode::WorkerRead;
			request.schedulerValidated = true;
			request.snapshotGeneration = 12;
			request.currentGeneration = 13;

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(request);

			context.Expect(!result.granted, "Worker grants should reject stale input/window snapshots.");
			context.Expect(result.fallbackReason.find("stale") != std::string::npos, "Fallback reason should explain stale generation.");
			context.Expect(HasCode(result.diagnostics, TerraEngine::Scheduling::ComponentAccessDiagnosticCode::StaleGeneration), "Grant diagnostics should carry stale-generation code.");
		});

		registry.Add("Engine.ComponentAccess.ValidatesCompactReferenceBeforeWorkerUse", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("WorkerReference");
			Object reference = objects.makeObjectHandle(object.EntityId());
			context.Expect(TerraEngine::IsResolvedObjectIdentity(objects.validateObjectHandle(reference)), "Fresh compact references should be valid before worker-readable use.");

			objects.removeObject(object);

			context.Expect(!TerraEngine::IsResolvedObjectIdentity(objects.validateObjectHandle(reference)), "Worker-readable compact references should fail closed after deletion.");
			objects.clear();
		});

		registry.Add("Engine.ComponentAccess.ObjectManagerObjectComponentOperationsUpdateGenerations", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			Object object = objects.createObject("ObjectComponentAccess");
			const std::uint64_t before = objects.getObjectComponentGeneration(object);

			context.Expect(objects.HasComponent<Transform>(object), "New objects should start with a Transform component.");
			objects.RemoveComponent<Transform>(object);
			context.Expect(!objects.HasComponent<Transform>(object), "Object component removal should remove the Transform through the canonical ObjectManager path.");
			context.Expect(objects.getObjectComponentGeneration(object) > before, "Removing a component through ObjectManager should bump component generation.");

			Transform* restored = objects.AddComponent<Transform>(object);

			context.Expect(restored != nullptr, "Object component add should restore Transform.");
			context.Expect(objects.GetComponent<Transform>(object) == restored, "Object component get should return the restored component.");
			objects.clear();
		});

		registry.Add("Engine.ComponentAccess.ReflectedTransformWritesPublishSpatialRevisions", [](TestContext& context) {
			ObjectManager& objects = ObjectManager::instance();
			objects.clear();
			const Object object = objects.createObject("Reflected Transform Target");
			const std::uint64_t transformRevisionBefore =
				objects.GetObjectTransformRevision(object);
			const std::uint64_t spatialRevisionBefore = objects.GetSpatialRevision();

			bool propertyWritten = false;
			{
				std::lock_guard lock(ComponentAccessRegistryMutex());
				auto transformAccess = ComponentAccessRegistry().find(
					std::string(Transform::kComponentName));
				if(transformAccess != ComponentAccessRegistry().end()) {
					propertyWritten = transformAccess->second.entitySetter(
						objects.getRegistry(),
						object.EntityId(),
						"Position",
						glm::vec3{7.0F, 2.0F, -3.0F});
				}
			}

			const Transform* transform = objects.GetComponent<Transform>(object);
			context.Expect(propertyWritten && transform != nullptr
				&& transform->position == glm::vec3(7.0F, 2.0F, -3.0F),
				"The shared reflected setter must apply the Transform value used by inspector, automation, and managed runtime callers.");
			context.Expect(objects.GetObjectTransformRevision(object) > transformRevisionBefore
				&& objects.GetSpatialRevision() > spatialRevisionBefore,
				"A reflected Transform write must invalidate render, hierarchy, and bounds snapshots without requiring a caller-specific dirty notification.");
			objects.clear();
		});
	}
}
