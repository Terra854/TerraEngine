#include "TestHarness.h"

#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryCapabilityTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.Capability.TransitionsRequireSupportThenLiveExecution",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryCapabilityKey key{};
				key.stage = TerraEngine::Renderer::VirtualGeometryCapabilityStage::Compaction;
				key.backend = TerraEngine::Renderer::VirtualGeometryCapabilityBackend::Vulkan;
				key.route = TerraEngine::Renderer::VirtualGeometryCapabilityRoute::MeshShader;
				key.deviceIdentity = "test-adapter";

				TerraEngine::Renderer::VirtualGeometryCapabilityEvidence evidence = TerraEngine::Renderer::MakePlannedVirtualGeometryCapability(std::move(key));
				context.Expect(evidence.planned && !evidence.supported && !evidence.executed && !evidence.validated,
					"A newly planned route must not imply device support, execution, or validation.");

				TerraEngine::Renderer::VirtualGeometryCapabilityExecution execution{};
				execution.completedLiveCommand = true;
				execution.identity = { 51u, 4u, 9u };
				execution.evidenceId = "submission-51-4";
				context.Expect(!TerraEngine::Renderer::AttachVirtualGeometryExecutionEvidence(evidence, execution),
					"Execution evidence must be rejected until support was evaluated for the active device.");

				TerraEngine::Renderer::EvaluateVirtualGeometryCapabilitySupport(evidence, true, "mesh shaders available", "maxMeshOutputVertices=256");
				context.Expect(evidence.supported, "Supported devices should promote only the support fact.");
				context.Expect(TerraEngine::Renderer::AttachVirtualGeometryExecutionEvidence(evidence, execution),
					"A completed generation-safe live command should promote execution.");
				context.Expect(evidence.executed && evidence.CanReportExecuted(), "Executed capability claims require the stored live-command evidence.");
			});

		registry.Add("VirtualGeometry.Capability.ValidationRequiresAcceptedArtifactsAndInvalidatesAtomically",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryCapabilityEvidence evidence{};
				evidence.planned = true;
				TerraEngine::Renderer::EvaluateVirtualGeometryCapabilitySupport(evidence, true, "supported", "limits");
				TerraEngine::Renderer::VirtualGeometryCapabilityExecution execution{};
				execution.completedLiveCommand = true;
				execution.identity = { 52u, 1u, 10u };
				execution.evidenceId = "submission-52-1";
				context.Expect(TerraEngine::Renderer::AttachVirtualGeometryExecutionEvidence(evidence, execution),
					"The fixture needs a proven execution before validation.");

				context.Expect(!TerraEngine::Renderer::AttachVirtualGeometryValidationEvidence(evidence, {}),
					"Validation must reject an empty artifact inventory without changing the execution fact.");
				context.Expect(evidence.executed && !evidence.validated, "Rejected validation must leave the prior execution state intact.");

				context.Expect(TerraEngine::Renderer::AttachVirtualGeometryValidationEvidence(evidence, { "readback-52", "image-52", "readback-52" }),
					"Accepted artifacts should promote a proven execution to validated.");
				context.Expect(evidence.validated && evidence.CanReportValidated(), "Validated claims require both live execution and accepted evidence.");
				context.ExpectEqual(static_cast<std::size_t>(2u),
					evidence.validationArtifactIds.size(),
					"Evidence IDs should be deduplicated deterministically before publication.");
			});
	}
} // namespace TerraEngine::Tests
