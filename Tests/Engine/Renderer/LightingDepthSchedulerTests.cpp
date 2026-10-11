// Depth preparation must remain immutable worker work while native query,
// submission and evidence ownership stay at the existing protected boundary.
#include "../TestHarness.h"
#include "Renderer/RenderSystem.h"
#include <algorithm>

namespace TerraEngine::Tests
{
	namespace
	{
		bool Has(const std::vector<std::string>& values, std::string_view name)
		{
			return std::find(values.begin(), values.end(), name) != values.end();
		}
	}
	void RegisterLightingDepthSchedulerTests(TestRegistry& registry)
	{
		registry.Add("Renderer.Efficiency.LightingDepthScheduler.SystemResourceOwnership", [](TestContext& context) {
			const auto declaration = RenderSystem::Instance().SchedulerDeclaration();
			context.Expect(declaration.phase == "Render" && declaration.hardBoundary,
				"Depth queries reuse the existing renderer phase and protected boundary.");
			for (const auto* name : {"LightingDepthPreparedInputs", "LightingDepthQueryOwners"})
				context.Expect(Has(declaration.reads,name) && Has(declaration.writes,name),
					"Diagnostics must expose prepared input and physical query ownership reads/writes.");
			context.Expect(Has(declaration.writes,"LightingDepthEvidence") && !declaration.safeSubwork.empty()
				&& !declaration.protectedCommit.empty(), "Native evidence publication and worker preparation require explicit declarations.");
		});
		registry.Add("Renderer.Efficiency.LightingDepthScheduler.WorkerAndProtectedBoundary", [](TestContext& context) {
			using namespace Scheduling;
			const auto workloads = RenderSystem::Instance().DeclareFrameWorkloads(0);
			context.ExpectEqual(workloads.size(),std::size_t{5},"Depth reduction adds no renderer phase or mutation flush.");
			bool prepared = false, committed = false;
			for (const auto& workload : workloads)
			{
				if (Has(workload.writes,"LightingDepthPreparedInputs"))
				{
					prepared = true;
					context.Expect(workload.workerEligible && !workload.sameFrameVisibilityRequired,
						"Exact-bit CPU inputs belong to worker preparation without scene visibility changes.");
				}
				if (Has(workload.writes,"LightingDepthQueryOwners") || Has(workload.writes,"LightingDepthEvidence"))
				{
					committed = true;
					context.Expect(!workload.workerEligible && workload.mainThreadRequired
						&& workload.boundaryClassification == HardBoundaryClassification::Protected
						&& Has(workload.reads,"LightingDepthPreparedInputs"),
						"Native recording consumes worker bytes only within the protected backend boundary.");
				}
				if (workload.workerEligible)
					context.Expect(!Has(workload.writes,"ObjectManager") && !Has(workload.writes,"SceneGraph"),
						"Depth workers cannot mutate live objects or hierarchy storage.");
			}
			context.Expect(prepared && committed,"Both CPU preparation and native protected ownership must be declared.");
		});
	}
}
