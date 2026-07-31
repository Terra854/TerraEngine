#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Geometry/GeometryClassPolicy.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryClassPolicyTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.ClassPolicy.SupportStateAndViewContract",
			[](TestContext &context)
			{
				const auto policy = Renderer::MakeProductionGeometryClassPolicy(Renderer::GeometryClass::Terrain);
				context.Expect(policy.state == Renderer::GeometrySupportState::ValidatedCandidate,
					"Offline terrain policy must remain a candidate until installed live evidence passes.");
				context.Expect(policy.views.camera && policy.views.shadow && policy.views.capture && policy.views.picking && policy.views.editor,
					"Candidate policy must declare every required view before it can be promoted.");
				context.Expect(policy.fallback.representation != Renderer::GeometryRepresentation::None,
					"Every policy must retain a safe fallback representation.");
			});

		registry.Add("VirtualGeometry.ClassPolicy.BoundsTransitionAndReadiness",
			[](TestContext &context)
			{
				Renderer::GeometryClassPolicyInput input{};
				input.geometryClass = Renderer::GeometryClass::Displacement;
				input.bounds.errorPixels = 0.25F;
				input.transition.hysteresisPixels = 1.0F;
				input.readiness.requiredCapabilities = Renderer::GeometryCapability::Vulkan;
				const auto pending = Renderer::EvaluateGeometryClassPolicy(input);
				context.Expect(pending.boundsConservative, "Accepted class policy must prove conservative current bounds.");
				context.Expect(pending.transitionStable, "Accepted class policy must prove temporally stable transitions.");
				context.Expect(pending.readiness == Renderer::GeometryReadiness::Unavailable,
					"Default readiness must fail closed when installed live evidence is missing.");
				input.readiness.liveEvidencePassed = true;
				const auto accepted = Renderer::EvaluateGeometryClassPolicy(input);
				context.Expect(accepted.readiness == Renderer::GeometryReadiness::Ready,
					"A policy cannot advertise production support before readiness evidence passes.");
			});
	}
}
