#include "TestHarness.h"
#include "RendererThreadingTestFixtures.h"
#include "Core/Scheduling/FrameWorkload.h"
#include "Renderer/RenderSystem.h"

namespace TerraEngine::Tests {

	void RegisterRenderGraphThreadingContractTests(TestRegistry& registry)
	{
		registry.Add("Renderer.ThreadingContract.BudgetsDetectOverflow", [](TestContext& context) {
			TerraEngine::Renderer::RenderWorkBudget budget{ TerraEngine::Renderer::RenderWorkBudgetCategory::DrawPreparation, 4u, 6u, TerraEngine::Renderer::RenderBudgetOverflowPolicy::Serialize, "draw" };
			context.Expect(TerraEngine::Renderer::IsRenderWorkOverBudget(budget), "Renderer work budgets should detect overflow before queues grow without limit.");
		});

		registry.Add("Renderer.ThreadingContract.PresentationMustBeCoordinatorOwned", [](TestContext& context) {
			auto pass = MakeRendererPass("present", 7u, TerraEngine::Renderer::RenderAccessMode::Present);
			pass.ownership = TerraEngine::Renderer::RenderWorkerOwnership::WorkerSafe;
			const auto result = TerraEngine::Renderer::ValidateRenderGraph({ pass });
			context.Expect(!result.valid, "Presentation resources must be rejected when worker-owned.");
		});

		registry.Add("Renderer.ThreadingContract.SchedulerDeclarationIsWorkerSafeForImmutablePreparation", [](TestContext& context) {
			TerraEngine::Scheduling::FrameWorkloadDeclaration workload;
			const TerraEngine::Scheduling::SystemDeclaration declaration = RenderSystem::Instance().SchedulerDeclaration();
			workload.workloadName = declaration.name;
			workload.ownerSystem = declaration.name;
			workload.phase = declaration.phase;
			workload.reads = declaration.reads;
			workload.writes = declaration.writes;
			workload.workerEligible = !declaration.mainThread;
			workload.mainThreadRequired = declaration.mainThread;
			workload.protectedReason = "Renderer immutable preparation is snapshot-backed.";

			const TerraEngine::Scheduling::FrameExecutionPlan plan = TerraEngine::Scheduling::ValidateFrameWorkloads({ workload }, "renderer-worker-compat");

			context.Expect(plan.valid, "Renderer scheduler declaration should validate as a frame workload.");
			context.ExpectEqual(static_cast<std::size_t>(1), plan.migrationSummary.workerSafeCount, "Renderer immutable CPU preparation should be worker-safe in the migration summary.");
		});
	}

}
