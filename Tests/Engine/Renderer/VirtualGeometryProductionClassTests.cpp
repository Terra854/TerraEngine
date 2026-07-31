#include "TestHarness.h"

#include "Editor/AssetCompilers/VirtualGeometryDisplacementBuilder.h"
#include "Editor/AssetCompilers/VirtualGeometrySplineCurveBuilder.h"
#include "Renderer/VirtualGeometry/Geometry/GeometryClassPolicy.h"

#include <array>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryProductionClassTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.ProductionClasses.RepresentativeAndAdversarialPolicies",
			[](TestContext &context)
			{
				constexpr Renderer::GeometryClass classes[] = {
					Renderer::GeometryClass::Spline, Renderer::GeometryClass::Curve,
					Renderer::GeometryClass::Displacement, Renderer::GeometryClass::Tessellation,
					Renderer::GeometryClass::Foliage, Renderer::GeometryClass::Terrain,
					Renderer::GeometryClass::Assembly, Renderer::GeometryClass::Distant };
				for (const auto geometryClass : classes)
				{
					const auto representative = Renderer::BuildGeometryClassFixture(geometryClass, false);
					const auto adversarial = Renderer::BuildGeometryClassFixture(geometryClass, true);
					context.Expect(representative.deterministic && adversarial.deterministic,
						"Representative and adversarial class fixtures must generate deterministic candidates.");
					context.Expect(representative.coverageValid || representative.fallbackActive,
						"A representative class must prove coverage or retain a safe fallback.");
					context.Expect(adversarial.rejectedAtomically || adversarial.fallbackActive,
						"Adversarial class input must reject atomically or remain visible through a diagnosed fallback.");
				}
			});

		registry.Add("VirtualGeometry.ProductionClasses.CrackFreeTransitionsAndSharedIdentity",
			[](TestContext &context)
			{
				const auto result = Renderer::ValidateGeometryClassTransition(Renderer::GeometryClass::Displacement,
					Renderer::GeometryClassTransitionCase::NeighborEdgeMismatch);
				context.Expect(result.edgeAgreement, "Displacement/tessellation transitions must agree on shared edge factors.");
				context.Expect(result.noCracks && result.coveragePreserved, "Class transitions must remain crack-free and hole-free.");
				context.Expect(Renderer::ValidateGeometryClassTransition(Renderer::GeometryClass::Assembly,
					Renderer::GeometryClassTransitionCase::RepeatedParts).identityStable,
					"Assembly repeated parts must preserve stable object/material/picking identity.");
			});

		registry.Add("VirtualGeometry.ProductionClasses.BoundedCurveAndDisplacementBuilders",
			[](TestContext &context)
			{
				const std::array<Editor::VirtualGeometryCurveControlPoint, 3u> controls = {{
					{{0.0F, 0.0F, 0.0F}, 0.25F, 1u, 11u},
					{{1.0F, 1.0F, 0.0F}, 0.25F, 1u, 12u},
					{{2.0F, 0.0F, 0.0F}, 0.5F, 2u, 13u}}};
				Editor::VirtualGeometrySplineCurveBuildRequest curveRequest{};
				curveRequest.controlPoints = controls;
				curveRequest.sourceGeneration = 17u;
				const auto firstCurve =
					Editor::BuildVirtualGeometrySplineCurve(curveRequest);
				const auto secondCurve =
					Editor::BuildVirtualGeometrySplineCurve(curveRequest);
				context.Expect(firstCurve.IsComplete()
					&& firstCurve.generationIdentity == secondCurve.generationIdentity
					&& firstCurve.lineIndices == secondCurve.lineIndices,
					"Curve candidates must stay deterministic and bounded for equal immutable controls.");

				const std::array<float, 4u> heights = {
					0.0F, 0.25F, 0.5F, 0.0F};
				Editor::VirtualGeometryDisplacementRegion region{};
				region.regionIdentity = 29u;
				region.heightSamples = heights;
				region.sampleWidth = 2u;
				region.sampleHeight = 2u;
				region.requestedEdgeFactors = {2u, 4u, 2u, 4u};
				region.neighborEdgeFactors = {4u, 2u, 4u, 2u};
				Editor::VirtualGeometryDisplacementBuildRequest displacementRequest{};
				displacementRequest.regions =
					std::span<const Editor::VirtualGeometryDisplacementRegion>(&region, 1u);
				displacementRequest.maximumDisplacement = 1.0F;
				displacementRequest.sourceGeneration = 31u;
				const auto displacement =
					Editor::BuildVirtualGeometryDisplacement(displacementRequest);
				context.Expect(displacement.IsComplete()
					&& displacement.regions.front().agreedEdgeFactors
						== std::array<std::uint32_t, 4u>{4u, 4u, 4u, 4u},
					"Neighbor regions must publish matching bounded edge factors before tessellation.");
			});
	}
} // namespace TerraEngine::Tests
