#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Geometry/VirtualGeometryRepresentations.h"

namespace TerraEngine::Tests
{
	namespace
	{
		TerraEngine::Renderer::VirtualGeometryAggregateRepresentation MakeAggregate(
			std::uint64_t stableId, float error)
		{
			using namespace TerraEngine::Renderer;
			VirtualGeometryAggregateRepresentation aggregate{};
			aggregate.kind = VirtualGeometryAggregateKind::Assembly;
			aggregate.stableId = stableId;
			aggregate.canonicalError = error;
			aggregate.completeFallbackCoverage = true;
			aggregate.sourceElementCount = 64u;
			aggregate.coverageRatio = 1.0f;
			aggregate.sharedStorageIdentity = 7u;
			aggregate.generationIdentity = 11u;
			aggregate.productionValidated = true;
			aggregate.atomicTransitionReady = true;
			return aggregate;
		}
	}

	void RegisterVirtualGeometryAggregateTransitionTests(TestRegistry& registry)
	{
		registry.Add("Renderer.VirtualGeometryAggregateTransition.AcceptsCompleteGeometricReplacement",
			[](TestContext& context) {
				using namespace TerraEngine::Renderer;
				const auto detailed = MakeAggregate(100u, 0.25f);
				const auto coarse = MakeAggregate(200u, 2.0f);
				const VirtualGeometryAggregateTransition transition =
					BuildVirtualGeometryAggregateTransition(detailed, coarse);
				context.Expect(!transition.failOpen && transition.fromId == 100u
					&& transition.toId == 200u && transition.error == 2.0f,
					"A complete coarser representation must replace geometry atomically without a traditional whole-mesh LOD fallback.");
			});

		registry.Add("Renderer.VirtualGeometryAggregateTransition.IncompleteTargetFailsOpen",
			[](TestContext& context) {
				using namespace TerraEngine::Renderer;
				const auto detailed = MakeAggregate(300u, 0.5f);
				auto incomplete = MakeAggregate(400u, 4.0f);
				incomplete.completeFallbackCoverage = false;
				const VirtualGeometryAggregateTransition transition =
					BuildVirtualGeometryAggregateTransition(detailed, incomplete);
				context.Expect(transition.failOpen && transition.fromId == detailed.stableId,
					"An incomplete transition target must keep the last complete geometric coverage instead of producing holes or black output.");
			});
	}
}
