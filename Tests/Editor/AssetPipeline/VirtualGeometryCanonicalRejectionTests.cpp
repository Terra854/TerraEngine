#include "TestHarness.h"

#include "Resource/VirtualGeometryCodec.h"

#include <cstdint>
#include <vector>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryCanonicalRejectionTests(TestRegistry& registry)
	{
		// A noncanonical input is a source-recook request, never an invitation
		// to interpret or transform retired cooked bytes.
		registry.Add("VirtualGeometry.Editor.AssetPipeline.CanonicalProduction.RejectsNonTemfprd1",
			[](TestContext& context)
			{
				const std::vector<std::uint8_t> historicalBytes{ 'T', 'E', 'M', 'F' };
				const auto validation = TerraEngine::Resource::ValidateTemfProductionFile(historicalBytes, {});
				context.Expect(!validation.IsComplete(),
					"Non-TEMFPRD1 bytes must return recook-required without interpreting retired data.");
			});
	}
} // namespace TerraEngine::EditorTests
