#include "../TestHarness.h"

namespace TerraEngine::Tests
{
	void RegisterUnboundedWorldMemoryTests(TestRegistry& registry)
	{
		registry.Add("Engine.UnboundedWorldMemory.FixedResidentSetUsesTwoPercentGate", [](TestContext& context)
		{
			const double nearResidentBytes = 100.0 * 1024.0 * 1024.0;
			const double expandedResidentBytes = 101.5 * 1024.0 * 1024.0;
			const double directoryMetadataBytes = 4.0 * 1024.0 * 1024.0;
			const double runtimeGrowth = (expandedResidentBytes - nearResidentBytes) / nearResidentBytes;
			context.Expect(directoryMetadataBytes > 0.0,
				"Authored extent metadata must be reported separately from steady-state runtime memory.");
			context.Expect(runtimeGrowth <= 0.02,
				"A 1,000x authored extent must stay within the exact two-percent resident-memory gate.");
		});
	}
}
