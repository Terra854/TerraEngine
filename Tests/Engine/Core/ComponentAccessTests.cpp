#include "../TestHarness.h"

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
	}
}
