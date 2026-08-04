#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Selection/GeometryRoutePolicy.h"

#include <cstdint>

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryRoutePolicyTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.RoutePolicy.CorrectnessFirstEligibilityAndFallback",
			[](TestContext &context)
			{
				Renderer::GeometryRoutePolicyInput input{};
				input.direct.valid = false;
				input.virtualGeometryHardware.valid = true;
				input.virtualGeometryHardware.estimatedCost = 1.0F;
				input.virtualGeometryCompute.valid = true;
				input.virtualGeometryCompute.estimatedCost = 2.0F;
				const auto decision = Renderer::SelectGeometryRoute(input);
				context.Expect(decision.selected == Renderer::GeometryRoute::VirtualGeometryHardware,
					"An ineligible direct candidate must never win on estimated cost.");

				input.virtualGeometryHardware.valid = false;
				input.virtualGeometryCompute.valid = false;
				const auto fallback = Renderer::SelectGeometryRoute(input);
				context.Expect(fallback.selected == Renderer::GeometryRoute::VirtualGeometryFallback,
					"When accelerated candidates are unavailable, selection must remain visible through the safe fallback.");
				context.Expect(fallback.fallbackReason == Renderer::GeometryRouteFallbackReason::CapabilityUnavailable,
					"Fallback decisions must preserve a typed capability reason.");
			});

		registry.Add("VirtualGeometry.RoutePolicy.OverrideAndDeterministicTieBreak",
			[](TestContext &context)
			{
				Renderer::GeometryRoutePolicyInput input{};
				input.direct.valid = true;
				input.direct.estimatedCost = 2.0F;
				input.virtualGeometryCompute.valid = true;
				input.virtualGeometryCompute.estimatedCost = 2.0F;
				input.virtualGeometryHardware.valid = true;
				input.virtualGeometryHardware.estimatedCost = 2.0F;
				const auto tied = Renderer::SelectGeometryRoute(input);
				context.Expect(tied.selected == Renderer::GeometryRoute::DirectMeshlet,
					"Equal-cost candidates must use the stable direct-first tie-break order.");

				input.authorOverride = Renderer::GeometryRoute::VirtualGeometryCompute;
				const auto overridden = Renderer::SelectGeometryRoute(input);
				context.Expect(overridden.selected == Renderer::GeometryRoute::VirtualGeometryCompute,
					"A supported author override must be honored and remain observable.");
				context.Expect(overridden.source == Renderer::GeometryRouteSelectionSource::AuthorOverride,
					"Override decisions must identify their selection source.");

				input.authorOverride = Renderer::GeometryRoute::VirtualGeometryHardware;
				input.virtualGeometryHardware.valid = false;
				const auto rejected = Renderer::SelectGeometryRoute(input);
				context.Expect(rejected.selected != Renderer::GeometryRoute::VirtualGeometryHardware,
					"An unsupported override must not bypass correctness eligibility.");
				context.Expect(rejected.fallbackReason == Renderer::GeometryRouteFallbackReason::AuthorOverrideUnavailable,
					"Rejected overrides must report author-override-unavailable.");
			});

		registry.Add("VirtualGeometry.RoutePolicy.HysteresisStabilizesBoundaryJitter",
			[](TestContext &context)
			{
				Renderer::GeometryRoutePolicyState state{};
				Renderer::GeometryRoutePolicyInput direct{};
				direct.direct.valid = true;
				direct.direct.estimatedCost = 1.0F;
				direct.virtualGeometryCompute.valid = true;
				direct.virtualGeometryCompute.estimatedCost = 1.1F;
				state.current = Renderer::GeometryRoute::DirectMeshlet;
				state.stableFrames = 8U;
				const auto first = Renderer::SelectGeometryRoute(direct, state);
				context.Expect(first.selected == Renderer::GeometryRoute::DirectMeshlet,
					"A stable current route must be retained inside the hysteresis margin.");

				direct.direct.estimatedCost = 2.0F;
				direct.virtualGeometryCompute.estimatedCost = 1.0F;
				const auto second = Renderer::SelectGeometryRoute(direct, first.nextState);
				context.Expect(second.selected == Renderer::GeometryRoute::DirectMeshlet,
					"One noisy boundary sample must not oscillate the selected route.");
				context.Expect(second.nextState.stableFrames == first.nextState.stableFrames + 1U,
					"Hysteresis state must advance deterministically with each accepted frame.");
			});

		registry.Add("VirtualGeometry.RoutePolicy.CompactRuntimeDecisionMatchesDetailedPolicy",
			[](TestContext &context)
			{
				Renderer::GeometryRoutePolicyInput input{};
				input.direct.valid = true;
				input.direct.estimatedCost = 3.0F;
				input.virtualGeometryHardware.valid = true;
				input.virtualGeometryHardware.estimatedCost = 1.0F;
				input.virtualGeometryCompute.valid = true;
				input.virtualGeometryCompute.estimatedCost = 2.0F;
				Renderer::GeometryRoutePolicyState previous{};
				previous.current = Renderer::GeometryRoute::DirectMeshlet;
				previous.challenger = Renderer::GeometryRoute::VirtualGeometryHardware;
				previous.stableFrames = 7U;
				previous.challengerFrames = 1U;

				const auto detailed = Renderer::SelectGeometryRoute(input, previous);
				const auto compact = Renderer::SelectGeometryRouteCompact(input, previous);
				context.Expect(compact.selected == detailed.selected
					&& compact.nextState.current == detailed.nextState.current
					&& compact.nextState.challenger == detailed.nextState.challenger
					&& compact.nextState.stableFrames == detailed.nextState.stableFrames
					&& compact.nextState.challengerFrames == detailed.nextState.challengerFrames,
					"The allocation-free runtime decision must preserve route selection and hysteresis exactly.");
				context.Expect(compact.candidates.empty(),
					"Compact runtime selection must omit candidate diagnostics that no render stage consumes.");
				context.Expect(detailed.candidates.size() == 3U,
					"The full inspection API must retain all candidate diagnostics.");
			});

		registry.Add("VirtualGeometry.RoutePolicy.RouteLockSelectsExactEligibleRoute",
			[](TestContext &context)
			{
				Renderer::GeometryRoutePolicyInput input{};
				input.direct.valid = true;
				input.direct.estimatedCost = 1.0F;
				input.virtualGeometryHardware.valid = true;
				input.virtualGeometryHardware.estimatedCost = 4.0F;
				input.virtualGeometryCompute.valid = true;
				input.virtualGeometryCompute.estimatedCost = 3.0F;
				input.authorOverride = Renderer::GeometryRoute::VirtualGeometryHardware;

				Renderer::GeometryRoutePolicyState previous{};
				previous.current = Renderer::GeometryRoute::DirectMeshlet;
				previous.stableFrames = 12u;
				const auto locked = Renderer::SelectGeometryRoute(input, previous);
				context.Expect(locked.selected == Renderer::GeometryRoute::VirtualGeometryHardware,
					"route.lock must select the requested eligible representation without hysteresis delay.");
				context.Expect(locked.source == Renderer::GeometryRouteSelectionSource::AuthorOverride,
					"Exact route locks must retain author-override provenance.");

				input.authorOverride = Renderer::GeometryRoute::VirtualGeometryCompute;
				input.virtualGeometryCompute.capabilitiesAvailable = false;
				const auto unavailable = Renderer::SelectGeometryRoute(input, locked.nextState);
				context.Expect(unavailable.selected == Renderer::GeometryRoute::VirtualGeometryFallback,
					"An unavailable route.lock must fail closed instead of choosing another representation.");
				context.Expect(unavailable.fallbackReason
					== Renderer::GeometryRouteFallbackReason::AuthorOverrideUnavailable,
					"Unavailable route.lock decisions must preserve their typed diagnostic.");

				Renderer::GeometryRouteFramePlan directOnlyPlan{};
				directOnlyPlan.selectedRoute = unavailable.selected;
				directOnlyPlan.instances = 1u;
				Renderer::BuildGeometryRouteFramePlan(directOnlyPlan);
				context.Expect(directOnlyPlan.vgHierarchyTraversalCount == 0u
					&& directOnlyPlan.vgPageRequestCount == 0u
					&& directOnlyPlan.vgDecodeCount == 0u
					&& directOnlyPlan.vgExecutorAllocationCount == 0u,
					"A failed route lock must not generate virtual-geometry work.");
			});
	}
}
