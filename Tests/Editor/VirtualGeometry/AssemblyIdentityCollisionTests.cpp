// Exercise the production identity allocator with a deterministic collision,
// rather than replacing its hash with a test-only implementation.
#include "../TestHarness.h"
#include "VirtualGeometryAssemblyBuilder.h"
#include <algorithm>
#include <vector>

namespace TerraEngine::EditorTests {
	void RegisterAssemblyIdentityCollisionTests(TestRegistry& registry) {
		registry.Add("Editor.VirtualGeometry.AssemblyIdentity.CollisionAndPermutation", [](TestContext& context) {
			// GOLDEN ends in 0x15: 1 ^ GOLDEN equals 2 ^ (GOLDEN + 1).
			// Thus these ordinary inputs collide in the original XOR allocator.
			const auto forward = Editor::BuildVirtualGeometryAssemblyCandidates({1U, 2U});
			const auto reverse = Editor::BuildVirtualGeometryAssemblyCandidates({2U, 1U});
			context.ExpectEqual(std::size_t{2}, forward.size(), "Both source pieces must survive identity allocation.");
			context.ExpectEqual(forward.size(), reverse.size(), "Permutation must preserve candidate count.");
			context.Expect(forward[0].stableId != forward[1].stableId, "Distinct assembly pieces must not share one stable ID.");
			for(std::size_t i = 0; i < forward.size(); ++i) {
				context.Expect(forward[i].Valid(), "Collision resolution must retain complete valid provenance.");
				context.ExpectEqual(forward[i].stableId, reverse[i].stableId, "Sorted source identity must remain reproducible.");
				context.ExpectEqual(std::uint64_t{i + 1U}, forward[i].sharedStorageIdentity, "Resolution must preserve source identity.");
				context.ExpectEqual(forward[i].stableId, forward[i].fallbackStableId, "Fallback must use the resolved ID.");
				context.ExpectEqual(forward[i].stableId, forward[i].generationIdentity, "Generation must use the resolved ID.");
			}
		});
		registry.Add("Editor.VirtualGeometry.AssemblyIdentity.ZeroAndOrdinaryControls", [](TestContext& context) {
			const auto ordinary = Editor::BuildVirtualGeometryAssemblyCandidates({3U, 9U});
			const auto filtered = Editor::BuildVirtualGeometryAssemblyCandidates({9U, 0U, 3U});
			context.ExpectEqual(std::size_t{2}, filtered.size(), "Zero is an absent source, not a new assembly piece.");
			context.ExpectEqual(ordinary.size(), filtered.size(), "Zero filtering must retain ordinary pieces.");
			for(std::size_t i = 0; i < filtered.size(); ++i) {
				context.Expect(filtered[i].Valid(), "Ordinary candidates must remain valid.");
				context.ExpectEqual(ordinary[i].stableId, filtered[i].stableId, "Filtering zero must not perturb ordinary IDs.");
			}
		});
	}
}
