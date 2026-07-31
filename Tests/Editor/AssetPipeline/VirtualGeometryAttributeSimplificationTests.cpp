#include "TestHarness.h"

#include "VirtualGeometryErrorModel.h"

#include <array>

namespace TerraEngine::EditorTests
{
	void RegisterVirtualGeometryAttributeSimplificationTests(TestRegistry &registry)
	{
		registry.Add("Editor.AssetPipeline.VirtualGeometry.Attributes.ProtectsEveryAuthoredBoundary",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				const VirtualGeometryAttributeSignature reference{ 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
				context.Expect(CanSimplifyVirtualGeometryAttributesTogether(reference, reference), "Identical authored signatures may share simplification.");

				const std::array<VirtualGeometryAttributeSignature, 8u> changed = { VirtualGeometryAttributeSignature{ 9u, 2u, 3u, 4u, 5u, 6u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 9u, 3u, 4u, 5u, 6u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 9u, 4u, 5u, 6u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 3u, 9u, 5u, 6u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 3u, 4u, 9u, 6u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 3u, 4u, 5u, 9u, 7u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 3u, 4u, 5u, 6u, 9u, 8u },
					VirtualGeometryAttributeSignature{ 1u, 2u, 3u, 4u, 5u, 6u, 7u, 9u } };
				for (const VirtualGeometryAttributeSignature &signature : changed)
				{
					context.Expect(!CanSimplifyVirtualGeometryAttributesTogether(reference, signature),
						"Material, UV, normal, tangent, hard-edge, skin, morph, and authored-mask "
						"changes must remain protected.");
				}
			});

		registry.Add("Editor.AssetPipeline.VirtualGeometry.Error.AccumulatesDescendantAndScreenCoverage",
			[](TestContext &context)
			{
				using namespace TerraEngine::Editor;
				const VirtualGeometryCoverageProof proof = BuildVirtualGeometryCoverageProof(0.5f, { 1.0f, 2.0f }, 100.0f, 10.0f);
				context.Expect(proof.Valid(), "A complete finite descendant set must create a valid proof.");
				context.ExpectEqual(2.0f, proof.descendantError, "The parent must retain the worst descendant error.");
				context.ExpectEqual(20.0f, proof.screenError, "Screen error must use the accumulated descendant error.");

				context.Expect(!BuildVirtualGeometryCoverageProof(0.5f, {}, 100.0f, 10.0f).Valid(), "Missing descendant coverage must fail closed.");
			});
	}
} // namespace TerraEngine::EditorTests
