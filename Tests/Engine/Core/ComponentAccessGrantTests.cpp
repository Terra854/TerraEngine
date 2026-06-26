#include "../TestHarness.h"

#include "Core/Scheduling/ComponentAccess.h"

namespace TerraEngine::Tests {
	void RegisterComponentAccessGrantTests(TestRegistry& registry)
	{
		registry.Add("Engine.ComponentAccessGrant.ValidatedReadGrantIsAccepted", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest request;
			request.grantId = "read-transform";
			request.workloadName = "Movement";
			request.readComponents = { "Transform" };
			request.schedulerValidated = true;

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(request);

			context.Expect(result.granted, "Scheduler-validated read grant should be accepted.");
			context.ExpectEqual(std::string("Transform"), result.normalized.readComponents.front(), "Read component should be preserved.");
		});

		registry.Add("Engine.ComponentAccessGrant.UndeclaredMutationIsRejected", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest request;
			request.grantId = "empty";
			request.workloadName = "Unsafe";
			request.schedulerValidated = true;

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(request);

			context.Expect(!result.granted, "Empty validated grant should be denied.");
			context.Expect(!result.diagnostics.empty(), "Grant denial should include diagnostics.");
		});

		registry.Add("Engine.ComponentAccessGrant.UnvalidatedGrantFallsBack", [](TestContext& context) {
			TerraEngine::Scheduling::ComponentAccessGrantRequest request;
			request.grantId = "unvalidated";
			request.workloadName = "EditorReflection";
			request.readComponents = { "Transform" };

			const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(request);

			context.Expect(!result.granted, "Unvalidated grants should not bypass protected locks.");
			context.Expect(result.fallbackReason.find("Scheduler validation") != std::string::npos, "Fallback reason should explain scheduler validation requirement.");
		});

		registry.Add("Engine.ComponentAccessGrant.ValidatesLargeHotLoopBatch", [](TestContext& context) {
			constexpr std::size_t grantCount = 10000;
			std::size_t grantedCount = 0;
			for (std::size_t index = 0; index < grantCount; ++index)
			{
				TerraEngine::Scheduling::ComponentAccessGrantRequest request;
				request.grantId = "batch-grant";
				request.workloadName = "TransformHotLoop";
				request.readComponents = { "Transform", "Bounds" };
				request.writeComponents = { "WorldTransform" };
				request.schedulerValidated = true;

				const auto result = TerraEngine::Scheduling::ValidateComponentAccessGrant(request);
				if (result.granted)
				{
					++grantedCount;
				}
			}

			// The validator normalizes small vectors and returns value objects, so
			// a large frame batch should stay allocation-light and deterministic.
			context.ExpectEqual(grantCount, grantedCount, "Every scheduler-validated hot-loop grant should be accepted.");
		});
	}
}
