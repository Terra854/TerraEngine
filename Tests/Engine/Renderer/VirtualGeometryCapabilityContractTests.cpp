#include "TestHarness.h"

#include "Renderer/RHI/RHIDefinitions.h"
#include "Renderer/VirtualGeometry/Capability/VirtualGeometryCapability.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryCapabilityContractTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.CapabilityContract.RequiresLiveGenerationSafeExecution",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryCapabilityEvidence evidence{};
				evidence.key.stage = TerraEngine::Renderer::VirtualGeometryCapabilityStage::Traversal;
				evidence.key.backend = TerraEngine::Renderer::VirtualGeometryCapabilityBackend::Vulkan;
				evidence.key.route = TerraEngine::Renderer::VirtualGeometryCapabilityRoute::MeshShader;
				evidence.key.deviceIdentity = "test-adapter";
				evidence.planned = true;
				evidence.supported = true;

				// Prepared packets are useful work, but they are not evidence that a live
				// backend command completed for one exact frame/view/generation identity.
				evidence.execution.preparedOnly = true;
				context.Expect(!evidence.CanReportExecuted(), "Prepared-only capability work must never be reported as executed.");

				evidence.execution.preparedOnly = false;
				evidence.execution.completedLiveCommand = true;
				evidence.execution.identity.frameId = 17u;
				evidence.execution.identity.viewId = 3u;
				evidence.execution.identity.rendererGeneration = 9u;
				evidence.execution.evidenceId = "submission-17-3";
				evidence.executed = true;
				context.Expect(
					evidence.CanReportExecuted(), "A completed live command with a complete identity and evidence ID should be executable evidence.");
			});

		registry.Add("VirtualGeometry.CapabilityContract.ValidatedRequiresExecutedEvidence",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryCapabilityEvidence evidence{};
				evidence.planned = true;
				evidence.supported = true;
				evidence.validated = true;
				evidence.validationArtifactIds = { "readback-1" };

				context.Expect(!evidence.CanReportValidated(), "Validation artifacts alone must not validate a route that has not executed.");

				evidence.execution.completedLiveCommand = true;
				evidence.execution.identity = { 21u, 2u, 6u };
				evidence.execution.evidenceId = "completed-21-2";
				evidence.executed = true;
				context.Expect(evidence.CanReportValidated(), "Validation requires both completed execution evidence and an accepted artifact.");
			});

		registry.Add("VirtualGeometry.CapabilityContract.MeasurementsNeverInventNumericValues",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryMeasurementRecord pending{};
				pending.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Pending;
				pending.durationNanoseconds = 0u;
				context.Expect(!pending.HasAuthoritativeDuration(), "Pending measurements must not expose a numeric duration, including zero.");

				TerraEngine::Renderer::VirtualGeometryMeasurementRecord measured{};
				measured.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
				measured.durationNanoseconds = 0u;
				measured.queryIdentity = "query-slot-4";
				measured.executionEvidenceId = "completed-22-1";
				context.Expect(
					measured.HasAuthoritativeDuration(), "Measured zero is authoritative only with a completed query identity and execution evidence.");
			});

		registry.Add("VirtualGeometry.CapabilityContract.BackendReceiptsRequireObservedCompletion",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBackendExecutionReceipt receipt{};
				receipt.frameNumber = 41u;
				receipt.rendererGeneration = 3u;
				receipt.geometryGeneration = 7u;
				receipt.commandGeneration = 11u;
				receipt.completionFence = 19u;
				receipt.commandRecorded = true;
				receipt.commandSubmitted = true;
				receipt.generationMatched = true;
				receipt.evidenceId = "backend-receipt-41";
				context.Expect(!receipt.IsExecuted(), "Recording and submission alone must not be reported as executed work.");

				receipt.completionObserved = true;
				context.Expect(receipt.IsExecuted(), "A receipt becomes execution evidence only after matching completion is observed.");
			});

		registry.Add("VirtualGeometry.CapabilityContract.BackendMeasurementsRequireQueryIdentity",
			[](TestContext &context)
			{
				TerraEngine::Renderer::VirtualGeometryBackendMeasurement measurement{};
				measurement.frameNumber = 42u;
				measurement.rendererGeneration = 3u;
				measurement.queryPoolGeneration = 5u;
				measurement.availability = TerraEngine::Renderer::BackendMeasurementAvailability::Measured;
				context.Expect(!measurement.HasAuthoritativeValue(), "A measured label without a query range must not invent a duration.");

				measurement.queryCount = 2u;
				context.Expect(measurement.HasAuthoritativeValue(), "A completed non-empty query range may publish an authoritative value, including zero.");
			});
	}
} // namespace TerraEngine::Tests
