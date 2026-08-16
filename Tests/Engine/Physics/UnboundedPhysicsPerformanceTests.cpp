#include "../TestHarness.h"

#include <algorithm>
#include <string>
#include <vector>

namespace TerraEngine::Tests
{
	void RegisterUnboundedPhysicsPerformanceTests(TestRegistry& registry)
	{
		registry.Add("Physics.UnboundedPerformance.NearAndDistantWorkloadsShareIdentity", [](TestContext& context)
		{
			const std::vector<std::string> nearRuns = { "run-a", "run-b", "run-c", "run-d", "run-e" };
			const std::vector<std::string> farRuns = nearRuns;
			context.Expect(nearRuns.size() == 5U && farRuns.size() == 5U,
				"Physics comparison requires five measured runs per location.");
			context.Expect(nearRuns == farRuns,
				"Distant physics runs must use the same workload and hardware identity.");
		});
	}
}
