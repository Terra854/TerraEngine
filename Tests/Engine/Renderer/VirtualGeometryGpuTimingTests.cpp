#include "TestHarness.h"

#include "Renderer/Graph/RenderDiagnostics.h"

namespace TerraEngine::Tests
{
	void RegisterVirtualGeometryGpuTimingTests(TestRegistry &registry)
	{
		registry.Add("VirtualGeometry.GpuTiming.PublishesPendingUnavailableInvalidAndMeasuredWithoutInventedZero",
			[](TestContext &context)
			{
				const TerraEngine::Renderer::VirtualGeometryFrameViewIdentity publication = { 90u, 2u, 7u };

				TerraEngine::Renderer::VirtualGeometryMeasurementRecord pending{};
				pending.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Pending;
				pending.identity = publication;
				const TerraEngine::Renderer::VirtualGeometryGpuTimingSnapshot pendingSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryGpuTimingSnapshot(pending, publication);
				context.Expect(pendingSnapshot.availability == TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Pending,
					"An unresolved query should remain pending instead of becoming a zero-duration sample.");
				context.Expect(!pendingSnapshot.durationNanoseconds.has_value(), "Pending timing must not expose an authoritative number.");

				TerraEngine::Renderer::VirtualGeometryMeasurementRecord unavailable = pending;
				unavailable.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Unavailable;
				unavailable.rejectionReason = "timestamps unsupported";
				const TerraEngine::Renderer::VirtualGeometryGpuTimingSnapshot unavailableSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryGpuTimingSnapshot(unavailable, publication);
				context.Expect(!unavailableSnapshot.durationNanoseconds.has_value(), "Unavailable timing must remain non-numeric.");

				TerraEngine::Renderer::VirtualGeometryMeasurementRecord stale = pending;
				stale.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
				stale.identity.rendererGeneration = 6u;
				stale.durationNanoseconds = 42u;
				stale.queryIdentity = "query-90";
				stale.executionEvidenceId = "submission-90";
				const TerraEngine::Renderer::VirtualGeometryGpuTimingSnapshot staleSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryGpuTimingSnapshot(stale, publication);
				context.Expect(staleSnapshot.availability == TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Invalid,
					"Measurements from another renderer generation must be published as invalid.");
				context.Expect(!staleSnapshot.durationNanoseconds.has_value(), "Invalid stale timing must discard its numeric duration.");

				TerraEngine::Renderer::VirtualGeometryMeasurementRecord measured = stale;
				measured.identity = publication;
				measured.durationNanoseconds = 0u;
				const TerraEngine::Renderer::VirtualGeometryGpuTimingSnapshot measuredSnapshot =
					TerraEngine::Renderer::BuildVirtualGeometryGpuTimingSnapshot(measured, publication);
				context.Expect(
					measuredSnapshot.HasAuthoritativeDuration(), "Completed measured zero is valid when its query and execution evidence are present.");
				context.ExpectEqual(
					std::uint64_t{ 0u }, *measuredSnapshot.durationNanoseconds, "Measured zero should remain distinguishable from unavailable timing.");
			});

		registry.Add("VirtualGeometry.GpuTiming.GenerationSafePublicationRejectsStaleRecords",
			[](TestContext &context)
			{
				TerraEngine::Renderer::RenderDiagnosticsSummary summary{};
				const TerraEngine::Renderer::VirtualGeometryFrameViewIdentity publication = { 101u, 3u, 12u };
				TerraEngine::Renderer::VirtualGeometryMeasurementRecord sample{};
				sample.availability = TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Measured;
				sample.identity = publication;
				sample.durationNanoseconds = 123u;
				sample.queryIdentity = "query-101";
				sample.executionEvidenceId = "submission-101";

				TerraEngine::Renderer::ApplyVirtualGeometryGpuTimingDiagnostics(summary, { sample }, publication);
				context.ExpectEqual(static_cast<std::size_t>(1u),
					summary.virtualGeometryCapabilityDiagnostics.timingSnapshots.size(),
					"A generation-safe timing sample should publish once.");
				context.Expect(summary.virtualGeometryCapabilityDiagnostics.timingSnapshots.front().HasAuthoritativeDuration(),
					"The published matching record should remain measured.");

				sample.identity.frameId = 100u;
				TerraEngine::Renderer::ApplyVirtualGeometryGpuTimingDiagnostics(summary, { sample }, publication);
				context.Expect(summary.virtualGeometryCapabilityDiagnostics.timingSnapshots.front().availability ==
								   TerraEngine::Renderer::VirtualGeometryMeasurementAvailability::Invalid,
					"Replacing the publication with stale work must invalidate, not merge, the timing record.");
			});
	}
} // namespace TerraEngine::Tests
