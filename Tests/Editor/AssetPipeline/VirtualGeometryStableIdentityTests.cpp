#include "TestHarness.h"

#include "VirtualGeometryBuildRecipe.h"
#include "VirtualGeometryIdentity.h"
#include "VirtualGeometryQuantization.h"

#include <array>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryStableIdentityTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometry.Identity.IsContentDerivedAndDeterministic",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				const auto first = AssignVirtualGeometryPrimitive(7u, 2u, 8u);
				const auto repeated = AssignVirtualGeometryPrimitive(7u, 2u, 8u);
				const auto changed = AssignVirtualGeometryPrimitive(8u, 2u, 8u);
				context.ExpectEqual(first.primitiveId, repeated.primitiveId, "Identical content and ordinals must reproduce primitive IDs.");
				context.ExpectEqual(first.clusterId, repeated.clusterId, "Identical content must reproduce cluster assignment.");
				context.Expect(first.primitiveId != changed.primitiveId, "Changing source content must invalidate stable identities.");
				context.Expect(first.pageId < 8u, "Content-derived page assignment must remain in range.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.Identity.NormalizesRecipeAndStageKeys",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				VirtualGeometryBuildRecipe first{ "AA", "BB", "CC", "DD", "Importer-1", "Compiler-1", "MSVC-1", "Terra-64" };
				VirtualGeometryBuildRecipe normalized = first;
				normalized.sourceHash = " aa ";
				normalized.codecProfile = "terra-64";
				context.ExpectEqual(first.Key(), normalized.Key(), "Canonical recipe tokens must ignore cosmetic case and space.");
				context.Expect(first.StageKey("encode") != first.StageKey("publish"), "Distinct stages must never share cache identities.");
				VirtualGeometryStageCacheRecord record{ "encode", first.StageKey("encode"), "output-hash", "content-addressed hit", true, true };
				context.Expect(record.Matches("encode", first.StageKey("encode")), "A verified output with the exact stage identity may resume.");
				context.Expect(!record.Matches("publish", first.StageKey("encode")), "A cache record must not cross stage boundaries.");
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.Quantization.KeepsSharedBoundariesWatertight",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				const VirtualGeometryQuantizationBounds bounds{ { -10.0f, -10.0f, -10.0f }, { 10.0f, 10.0f, 10.0f } };
				const auto first = QuantizeVirtualGeometryPosition({ 0.5f, 0.5f, 0.5f }, bounds);
				const auto second = QuantizeVirtualGeometryPosition({ 0.5f, 0.5f, 0.5f }, bounds);
				const std::array<VirtualGeometryQuantizedPosition, 1u> firstBoundary{ first };
				const std::array<VirtualGeometryQuantizedPosition, 1u> secondBoundary{ second };
				context.Expect(
					ValidateVirtualGeometryWatertightBoundary(firstBoundary, secondBoundary), "Shared global bounds must encode a seam identically.");
				const auto decoded = DequantizeVirtualGeometryPosition(first, bounds);
				context.Expect(decoded[0] >= -10.0f && decoded[0] <= 10.0f, "Decoded positions must remain inside the global bounds.");
			});
	}
} // namespace TerraEngine::EditorTests
